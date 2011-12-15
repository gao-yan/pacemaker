/*
 * Copyright (c) 2004 Andrew Beekhof <andrew@beekhof.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
#include <crm_internal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include <dirent.h>
#include <libgen.h>             /* Add it for compiling on OSX */

#include <crm/crm.h>
#include <crm/stonith-ng.h>
#include <crm/stonith-ng-internal.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <stonith/stonith.h>

CRM_TRACE_INIT_DATA(stonith);

typedef struct stonith_private_s {
    char *token;
    IPC_Channel *command_channel;
    IPC_Channel *callback_channel;
    GCHSource *callback_source;
    GHashTable *stonith_op_callback_table;
    GList *notify_list;

    void (*op_callback) (stonith_t * st, const xmlNode * msg, int call, int rc, xmlNode * output,
                         void *userdata);

} stonith_private_t;

typedef struct stonith_notify_client_s {
    const char *event;
    const char *obj_id;         /* implement one day */
    const char *obj_type;       /* implement one day */
    void (*notify) (stonith_t * st, const char *event, xmlNode * msg);

} stonith_notify_client_t;

typedef struct stonith_callback_client_s {
    void (*callback) (stonith_t * st, const xmlNode * msg, int call, int rc, xmlNode * output,
                      void *userdata);
    const char *id;
    void *user_data;
    gboolean only_success;
    struct timer_rec_s *timer;

} stonith_callback_client_t;

struct notify_blob_s {
    stonith_t *stonith;
    xmlNode *xml;
};

struct timer_rec_s {
    int call_id;
    int timeout;
    guint ref;
    stonith_t *stonith;
};

typedef enum stonith_errors (*stonith_op_t) (const char *, int, const char *, xmlNode *,
                                             xmlNode *, xmlNode *, xmlNode **, xmlNode **);

static const char META_TEMPLATE[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE resource-agent SYSTEM \"ra-api-1.dtd\">\n"
    "<resource-agent name=\"%s\">\n"
    "  <version>1.0</version>\n"
    "  <longdesc lang=\"en\">\n"
    "%s\n"
    "  </longdesc>\n"
    "  <shortdesc lang=\"en\">%s</shortdesc>\n"
    "%s\n"
    "  <actions>\n"
    "    <action name=\"start\"   timeout=\"60\" />\n"
    "    <action name=\"stop\"    timeout=\"15\" />\n"
    "    <action name=\"status\"  timeout=\"60\" />\n"
    "    <action name=\"monitor\" timeout=\"60\" interval=\"3600\" start-delay=\"15\" />\n"
    "    <action name=\"meta-data\"  timeout=\"15\" />\n"
    "  </actions>\n"
    "  <special tag=\"heartbeat\">\n"
    "    <version>2.0</version>\n" "  </special>\n" "</resource-agent>\n";

bool stonith_dispatch(stonith_t * st);
gboolean stonith_dispatch_internal(IPC_Channel * channel, gpointer user_data);
void stonith_perform_callback(stonith_t * stonith, xmlNode * msg, int call_id, int rc);
xmlNode *stonith_create_op(int call_id, const char *token, const char *op, xmlNode * data,
                           int call_options);
int stonith_send_command(stonith_t * stonith, const char *op, xmlNode * data,
                         xmlNode ** output_data, int call_options, int timeout);

static void stonith_connection_destroy(gpointer user_data);
static void stonith_send_notification(gpointer data, gpointer user_data);

static void
stonith_connection_destroy(gpointer user_data)
{
    stonith_t *stonith = user_data;
    stonith_private_t *native = NULL;
    struct notify_blob_s blob;

    blob.stonith = stonith;
    blob.xml = create_xml_node(NULL, "notify");

    native = stonith->private;
    native->callback_source = NULL;

    stonith->state = stonith_disconnected;
    crm_xml_add(blob.xml, F_TYPE, T_STONITH_NOTIFY);
    crm_xml_add(blob.xml, F_SUBTYPE, T_STONITH_NOTIFY_DISCONNECT);

    g_list_foreach(native->notify_list, stonith_send_notification, &blob);
    free_xml(blob.xml);
}

static int
stonith_api_register_device(stonith_t * stonith, int call_options,
                            const char *id, const char *namespace, const char *agent,
                            stonith_key_value_t * params)
{
    int rc = 0;
    xmlNode *data = create_xml_node(NULL, F_STONITH_DEVICE);
    xmlNode *args = create_xml_node(data, XML_TAG_ATTRS);

    crm_xml_add(data, XML_ATTR_ID, id);
    crm_xml_add(data, "origin", __FUNCTION__);
    crm_xml_add(data, "agent", agent);
    crm_xml_add(data, "namespace", namespace);

    for (; params; params = params->next) {
        hash2field((gpointer) params->key, (gpointer) params->value, args);
    }

    rc = stonith_send_command(stonith, STONITH_OP_DEVICE_ADD, data, NULL, call_options, 0);
    free_xml(data);

    return rc;
}

static int
stonith_api_remove_device(stonith_t * stonith, int call_options, const char *name)
{
    int rc = 0;
    xmlNode *data = NULL;

    data = create_xml_node(NULL, F_STONITH_DEVICE);
    crm_xml_add(data, "origin", __FUNCTION__);
    crm_xml_add(data, XML_ATTR_ID, name);
    rc = stonith_send_command(stonith, STONITH_OP_DEVICE_DEL, data, NULL, call_options, 0);
    free_xml(data);

    return rc;
}

static void
append_arg(gpointer key, gpointer value, gpointer user_data)
{
    int len = 3;                /* =, \n, \0 */
    int last = 0;
    char **args = user_data;

    CRM_CHECK(key != NULL, return);
    CRM_CHECK(value != NULL, return);

    if (strstr(key, "pcmk_")) {
        return;
    } else if (strstr(key, CRM_META)) {
        return;
    } else if (safe_str_eq(key, "crm_feature_set")) {
        return;
    }

    len += strlen(key);
    len += strlen(value);
    if (*args != NULL) {
        last = strlen(*args);
    }

    crm_realloc(*args, last + len);
    crm_debug_2("Appending: %s=%s", (char *)key, (char *)value);
    sprintf((*args) + last, "%s=%s\n", (char *)key, (char *)value);
}

static void
append_const_arg(const char *key, const char *value, char **arg_list)
{
    char *glib_sucks_key = crm_strdup(key);
    char *glib_sucks_value = crm_strdup(value);

    append_arg(glib_sucks_key, glib_sucks_value, arg_list);

    crm_free(glib_sucks_value);
    crm_free(glib_sucks_key);
}

static void
append_host_specific_args(const char *victim, const char *map, GHashTable * params, char **arg_list)
{
    char *name = NULL;
    int last = 0, lpc = 0, max = 0;

    if (map == NULL) {
        /* The best default there is for now... */
        crm_debug("Using default arg map: port=uname");
        append_const_arg("port", victim, arg_list);
        return;
    }

    max = strlen(map);
    crm_debug("Processing arg map: %s", map);
    for (; lpc < max + 1; lpc++) {
        if (isalpha(map[lpc])) {
            /* keep going */

        } else if (map[lpc] == '=' || map[lpc] == ':') {
            crm_free(name);
            crm_malloc0(name, 1 + lpc - last);
            strncpy(name, map + last, lpc - last);
            crm_debug("Got name: %s", name);
            last = lpc + 1;

        } else if (map[lpc] == 0 || map[lpc] == ',' || isspace(map[lpc])) {
            char *param = NULL;
            const char *value = NULL;

            crm_malloc0(param, 1 + lpc - last);
            strncpy(param, map + last, lpc - last);
            last = lpc + 1;

            crm_debug("Got key: %s", param);
            if (name == NULL) {
                crm_err("Misparsed '%s', found '%s' without a name", map, param);
                crm_free(param);
                continue;
            }

            if (safe_str_eq(param, "uname")) {
                value = victim;
            } else {
                char *key = crm_meta_name(param);

                value = g_hash_table_lookup(params, key);
                crm_free(key);
            }

            if (value) {
                crm_debug("Setting '%s'='%s' (%s) for %s", name, value, param, victim);
                append_const_arg(name, value, arg_list);

            } else {
                crm_err("No node attribute '%s' for '%s'", name, victim);
            }

            crm_free(name);
            name = NULL;
            crm_free(param);
            if (map[lpc] == 0) {
                break;
            }

        } else if (isspace(map[lpc])) {
            last = lpc;
        }
    }
    crm_free(name);
}

static char *
make_args(const char *action, const char *victim, GHashTable * device_args, GHashTable * port_map)
{
    char buffer[512];
    char *arg_list = NULL;
    const char *value = NULL;

    CRM_CHECK(action != NULL, return NULL);

    if (device_args) {
        g_hash_table_foreach(device_args, append_arg, &arg_list);
    }

    buffer[511] = 0;
    snprintf(buffer, 511, "pcmk_%s_action", action);
    if (device_args) {
        value = g_hash_table_lookup(device_args, buffer);
    }

    if (value == NULL && device_args) {
        /* Legacy support for early 1.1 releases - Remove for 1.2 */
        snprintf(buffer, 511, "pcmk_%s_cmd", action);
        value = g_hash_table_lookup(device_args, buffer);
    }

    if (value) {
        crm_info("Substituting action '%s' for requested operation '%s'", value, action);
        action = value;
    }

    append_const_arg(STONITH_ATTR_ACTION_OP, action, &arg_list);
    if (victim && device_args) {
        const char *alias = victim;
        const char *param = g_hash_table_lookup(device_args, STONITH_ATTR_HOSTARG);

        if (port_map && g_hash_table_lookup(port_map, victim)) {
            alias = g_hash_table_lookup(port_map, victim);
        }

        /* Always supply the node's name too:
         *    https://fedorahosted.org/cluster/wiki/FenceAgentAPI
         */
        append_const_arg("nodename", victim, &arg_list);

        /* Check if we need to supply the victim in any other form */
        if (param == NULL) {
            const char *map = g_hash_table_lookup(device_args, STONITH_ATTR_ARGMAP);

            if (map == NULL) {
                param = "port";
                value = g_hash_table_lookup(device_args, param);

            } else {
                /* Legacy handling */
                append_host_specific_args(alias, map, device_args, &arg_list);
                value = map;    /* Nothing more to do */
            }

        } else if (safe_str_eq(param, "none")) {
            value = param;      /* Nothing more to do */

        } else {
            value = g_hash_table_lookup(device_args, param);
        }

        /* Don't overwrite explictly set values for $param */
        if (value == NULL || safe_str_eq(value, "dynamic")) {
            crm_info("%s-ing node '%s' as '%s=%s'", action, victim, param, alias);
            append_const_arg(param, alias, &arg_list);
        }
    }

    crm_debug_3("Calculated: %s", arg_list);
    return arg_list;
}

/* Borrowed from libfence and extended */
int
run_stonith_agent(const char *agent, const char *action, const char *victim,
                  GHashTable * device_args, GHashTable * port_map, int *agent_result, char **output,
                  async_command_t * track)
{
    char *args = make_args(action, victim, device_args, port_map);
    int pid, status, len, rc = st_err_internal;
    int p_read_fd, p_write_fd;  /* parent read/write file descriptors */
    int c_read_fd, c_write_fd;  /* child read/write file descriptors */
    int fd1[2];
    int fd2[2];

    c_read_fd = c_write_fd = p_read_fd = p_write_fd = -1;

    if (args == NULL || agent == NULL)
        goto fail;
    len = strlen(args);

    if (pipe(fd1))
        goto fail;
    p_read_fd = fd1[0];
    c_write_fd = fd1[1];

    if (pipe(fd2))
        goto fail;
    c_read_fd = fd2[0];
    p_write_fd = fd2[1];

    crm_debug("forking");
    pid = fork();
    if (pid < 0) {
        rc = st_err_agent_fork;
        goto fail;
    }

    if (pid) {
        /* parent */
        int ret;
        int total = 0;

        fcntl(p_read_fd, F_SETFL, fcntl(p_read_fd, F_GETFL, 0) | O_NONBLOCK);

        do {
            crm_debug("sending args");
            ret = write(p_write_fd, args + total, len - total);
            if (ret > 0) {
                total += ret;
            }

        } while (errno == EINTR && total < len);

        if (total != len) {
            crm_perror(LOG_ERR, "Sent %d not %d bytes", total, len);
            if (ret >= 0) {
                rc = st_err_agent_args;
            }
            goto fail;
        }

        close(p_write_fd);

        if (track) {
            track->stdout = p_read_fd;
            NewTrackedProc(pid, 0, PT_LOGNORMAL, track, track->pt_ops);
            crm_trace("Op: %s on %s, timeout: %d", action, agent, track->timeout);

            if (track->timeout) {
                track->killseq[0].mstimeout = track->timeout;   /* after timeout send TERM */
                track->killseq[0].signalno = SIGTERM;
                track->killseq[1].mstimeout = 5000;     /* after another 5s remove it */
                track->killseq[1].signalno = SIGKILL;
                track->killseq[2].mstimeout = 5000;     /* if it's still there after another 5s, complain */
                track->killseq[2].signalno = 0;

                SetTrackedProcTimeouts(pid, track->killseq);

            } else {
                crm_err("No timeout set for stonith operation %s with device %s", action, agent);
            }

            close(c_write_fd);
            close(c_read_fd);
            crm_free(args);
            return pid;

        } else {
            waitpid(pid, &status, 0);

            if (output != NULL) {
                len = 0;
                do {
                    char buf[500];

                    ret = read(p_read_fd, buf, 500);
                    if (ret > 0) {
                        buf[ret] = 0;
                        crm_realloc(*output, len + ret + 1);
                        sprintf((*output) + len, "%s", buf);
                        crm_debug("%d: %s", ret, (*output) + len);
                        len += ret;
                    }

                } while (ret == 500 || (ret < 0 && errno == EINTR));
            }

            rc = st_err_agent;
            *agent_result = st_err_agent;
            if (WIFEXITED(status)) {
                crm_debug("result = %d", WEXITSTATUS(status));
                *agent_result = -WEXITSTATUS(status);
                rc = 0;
            }
        }

    } else {
        /* child */
        const char *st_dev_id_key = CRM_META "_" F_STONITH_DEVICE;
        const char *st_dev_id_value = NULL;

        close(1);
        if (dup(c_write_fd) < 0)
            goto fail;
        close(2);
        if (dup(c_write_fd) < 0)
            goto fail;
        close(0);
        if (dup(c_read_fd) < 0)
            goto fail;

        /* keep c_write_fd open so parent can report all errors. */
        close(c_read_fd);
        close(p_read_fd);
        close(p_write_fd);

        st_dev_id_value = g_hash_table_lookup(device_args, st_dev_id_key);
        if (st_dev_id_value) {
            setenv(st_dev_id_key, st_dev_id_value, 1);
        }

        execlp(agent, agent, NULL);
        exit(EXIT_FAILURE);
    }

  fail:
    crm_free(args);

    if (p_read_fd >= 0) {
        close(p_read_fd);
    }
    if (p_write_fd >= 0) {
        close(p_write_fd);
    }

    if (c_read_fd >= 0) {
        close(c_read_fd);
    }
    if (c_write_fd >= 0) {
        close(c_write_fd);
    }

    return rc;
}

static int
stonith_api_device_list(stonith_t * stonith, int call_options, const char *namespace,
                        stonith_key_value_t ** devices, int timeout)
{
    int count = 0;

    if (devices == NULL) {
        crm_err("Parameter error: stonith_api_device_list");
        return -2;
    }

    /* Include Heartbeat agents */
    if (namespace == NULL || safe_str_eq("heartbeat", namespace)) {
        char **entry = NULL;
        char **type_list = stonith_types();

        for (entry = type_list; entry != NULL && *entry; ++entry) {
            crm_trace("Added: %s", *entry);
            *devices = stonith_key_value_add(*devices, NULL, *entry);
            count++;
        }
        if (type_list) {
            stonith_free_hostlist(type_list);
        }
    }

    /* Include Red Hat agents, basically: ls -1 @sbin_dir@/fence_* */
    if (namespace == NULL || safe_str_eq("redhat", namespace)) {
        struct dirent **namelist;
        int file_num = scandir(RH_STONITH_DIR, &namelist, 0, alphasort);

        if (file_num > 0) {
            struct stat prop;
            char buffer[FILENAME_MAX + 1];

            while (file_num--) {
                if ('.' == namelist[file_num]->d_name[0]) {
                    free(namelist[file_num]);
                    continue;

                } else if (0 != strncmp(RH_STONITH_PREFIX,
                                        namelist[file_num]->d_name, strlen(RH_STONITH_PREFIX))) {
                    free(namelist[file_num]);
                    continue;
                }

                snprintf(buffer, FILENAME_MAX, "%s/%s", RH_STONITH_DIR, namelist[file_num]->d_name);
                if (stat(buffer, &prop) == 0 && S_ISREG(prop.st_mode)) {
                    *devices = stonith_key_value_add(*devices, NULL, namelist[file_num]->d_name);
                    count++;
                }

                free(namelist[file_num]);
            }
            free(namelist);
        }
    }

    return count;
}

static int
stonith_api_device_metadata(stonith_t * stonith, int call_options, const char *agent,
                            const char *namespace, char **output, int timeout)
{
    int rc = 0;
    int bufferlen = 0;

    char *buffer = NULL;
    char *xml_meta_longdesc = NULL;
    char *xml_meta_shortdesc = NULL;

    char *meta_param = NULL;
    char *meta_longdesc = NULL;
    char *meta_shortdesc = NULL;
    const char *provider = get_stonith_provider(agent, namespace);

    Stonith *stonith_obj = NULL;
    static const char *no_parameter_info = "<!-- no value -->";

    crm_info("looking up %s/%s metadata", agent, provider);

    /* By having this in a library, we can access it from stonith_admin
     *  when neither lrmd or stonith-ng are running
     * Important for the crm shell's validations...
     */

    if (safe_str_eq(provider, "redhat")) {

        int exec_rc = run_stonith_agent(agent, "metadata", NULL, NULL, NULL, &rc, &buffer, NULL);

        if (exec_rc < 0 || rc != 0 || buffer == NULL) {
            /* failed */
            crm_debug("Query failed: %d %d: %s", exec_rc, rc, crm_str(buffer));

            /* provide a fake metadata entry */
            meta_longdesc = crm_strdup(no_parameter_info);
            meta_shortdesc = crm_strdup(no_parameter_info);
            meta_param = crm_strdup("  <parameters>\n"
                                    "    <parameter name=\"action\">\n"
                                    "      <getopt mixed=\"-o\" />\n"
                                    "      <content type=\"string\" default=\"reboot\" />\n"
                                    "      <shortdesc lang=\"en\">Fencing action (null, off, on, [reboot], status, hostlist, devstatus)</shortdesc>\n"
                                    "    </parameter>\n" "  </parameters>");

            goto build;
        }

    } else {
        stonith_obj = stonith_new(agent);

        meta_longdesc = crm_strdup(stonith_get_info(stonith_obj, ST_DEVICEDESCR));
        if (meta_longdesc == NULL) {
            crm_warn("no long description in %s's metadata.", agent);
            meta_longdesc = crm_strdup(no_parameter_info);
        }

        meta_shortdesc = crm_strdup(stonith_get_info(stonith_obj, ST_DEVICEID));
        if (meta_shortdesc == NULL) {
            crm_warn("no short description in %s's metadata.", agent);
            meta_shortdesc = crm_strdup(no_parameter_info);
        }

        meta_param = crm_strdup(stonith_get_info(stonith_obj, ST_CONF_XML));
        if (meta_param == NULL) {
            crm_warn("no list of parameters in %s's metadata.", agent);
            meta_param = crm_strdup(no_parameter_info);
        }

  build:
        xml_meta_longdesc =
            (char *)xmlEncodeEntitiesReentrant(NULL, (const unsigned char *)meta_longdesc);
        xml_meta_shortdesc =
            (char *)xmlEncodeEntitiesReentrant(NULL, (const unsigned char *)meta_shortdesc);

        bufferlen = strlen(META_TEMPLATE) + strlen(agent)
            + strlen(xml_meta_longdesc) + strlen(xml_meta_shortdesc)
            + strlen(meta_param) + 1;

        crm_malloc0(buffer, bufferlen);
        snprintf(buffer, bufferlen - 1, META_TEMPLATE,
                 agent, xml_meta_longdesc, xml_meta_shortdesc, meta_param);

        xmlFree(xml_meta_longdesc);
        xmlFree(xml_meta_shortdesc);

        if (stonith_obj) {
            stonith_delete(stonith_obj);
        }

        crm_free(meta_shortdesc);
        crm_free(meta_longdesc);
        crm_free(meta_param);
    }

    if (output) {
        *output = buffer;

    } else {
        crm_free(buffer);
    }

    return rc;
}

static int
stonith_api_query(stonith_t * stonith, int call_options, const char *target,
                  stonith_key_value_t ** devices, int timeout)
{
    int rc = 0, lpc = 0, max = 0;

    xmlNode *data = NULL;
    xmlNode *output = NULL;
    xmlXPathObjectPtr xpathObj = NULL;

    CRM_CHECK(devices != NULL, return st_err_missing);

    data = create_xml_node(NULL, F_STONITH_DEVICE);
    crm_xml_add(data, "origin", __FUNCTION__);
    crm_xml_add(data, F_STONITH_TARGET, target);
    rc = stonith_send_command(stonith, STONITH_OP_QUERY, data, &output, call_options, timeout);

    if (rc < 0) {
        return rc;
    }

    xpathObj = xpath_search(output, "//@agent");
    if (xpathObj) {
        max = xpathObj->nodesetval->nodeNr;

        for (lpc = 0; lpc < max; lpc++) {
            xmlNode *match = getXpathResult(xpathObj, lpc);

            CRM_CHECK(match != NULL, continue);

            crm_info("%s[%d] = %s", "//@agent", lpc, xmlGetNodePath(match));
            *devices = stonith_key_value_add(*devices, NULL, crm_element_value(match, XML_ATTR_ID));
        }
    }

    free_xml(output);
    free_xml(data);
    return max;
}

static int
stonith_api_call(stonith_t * stonith, int call_options, const char *id, const char *action,
                 const char *victim, int timeout)
{
    int rc = 0;
    xmlNode *data = NULL;

    data = create_xml_node(NULL, F_STONITH_DEVICE);
    crm_xml_add(data, "origin", __FUNCTION__);
    crm_xml_add(data, F_STONITH_DEVICE, id);
    crm_xml_add(data, F_STONITH_ACTION, action);
    crm_xml_add(data, F_STONITH_TARGET, victim);

    rc = stonith_send_command(stonith, STONITH_OP_EXEC, data, NULL, call_options, timeout);
    free_xml(data);

    return rc;
}

static int
stonith_api_fence(stonith_t * stonith, int call_options, const char *node, const char *action,
                  int timeout)
{
    int rc = 0;
    xmlNode *data = NULL;

    data = create_xml_node(NULL, __FUNCTION__);
    crm_xml_add(data, F_STONITH_TARGET, node);
    crm_xml_add(data, F_STONITH_ACTION, action);
    crm_xml_add_int(data, F_STONITH_TIMEOUT, timeout);

    rc = stonith_send_command(stonith, STONITH_OP_FENCE, data, NULL, call_options, timeout);
    free_xml(data);

    return rc;
}

static int
stonith_api_confirm(stonith_t * stonith, int call_options, const char *target)
{
    return stonith_api_fence(stonith, call_options|st_opt_manual_ack, target, "off", 0);
}

static int
stonith_api_history(stonith_t * stonith, int call_options, const char *node,
                    stonith_history_t ** history, int timeout)
{
    int rc = 0;
    xmlNode *data = NULL;
    xmlNode *output = NULL;
    stonith_history_t *last = NULL;

    *history = NULL;

    if (node) {
        data = create_xml_node(NULL, __FUNCTION__);
        crm_xml_add(data, F_STONITH_TARGET, node);
    }

    rc = stonith_send_command(stonith, STONITH_OP_FENCE_HISTORY, data, &output,
                              call_options | st_opt_sync_call, timeout);
    free_xml(data);

    if (rc == 0) {
        xmlNode *op = NULL;
        xmlNode *reply = get_xpath_object("//" F_STONITH_HISTORY_LIST, output, LOG_ERR);

        for (op = __xml_first_child(reply); op != NULL; op = __xml_next(op)) {
            stonith_history_t *kvp;

            crm_malloc0(kvp, sizeof(stonith_history_t));
            kvp->target = crm_element_value_copy(op, F_STONITH_TARGET);
            kvp->action = crm_element_value_copy(op, F_STONITH_ACTION);
            kvp->origin = crm_element_value_copy(op, F_STONITH_ORIGIN);
            kvp->delegate = crm_element_value_copy(op, F_STONITH_DELEGATE);
            crm_element_value_int(op, F_STONITH_DATE, &kvp->completed);
            crm_element_value_int(op, F_STONITH_STATE, &kvp->state);

            if (last) {
                last->next = kvp;
            } else {
                *history = kvp;
            }
            last = kvp;
        }
    }
    return rc;
}

const char *
stonith_error2string(enum stonith_errors return_code)
{
    const char *error_msg = NULL;

    switch (return_code) {
        case stonith_ok:
            error_msg = "OK";
            break;
        case st_err_not_supported:
            error_msg = "Not supported";
            break;
        case st_err_authentication:
            error_msg = "Not authenticated";
            break;
        case st_err_generic:
            error_msg = "Generic error";
            break;
        case st_err_internal:
            error_msg = "Internal error";
            break;
        case st_err_unknown_device:
            error_msg = "Unknown device";
            break;
        case st_err_unknown_operation:
            error_msg = "Unknown operation";
            break;
        case st_err_unknown_port:
            error_msg = "Unknown victim";
            break;
        case st_err_none_available:
            error_msg = "No available fencing devices";
            break;
        case st_err_connection:
            error_msg = "Not connected";
            break;
        case st_err_missing:
            error_msg = "Missing input";
            break;
        case st_err_exists:
            error_msg = "Device exists";
            break;
        case st_err_timeout:
            error_msg = "Operation timed out";
            break;
        case st_err_signal:
            error_msg = "Killed by signal";
            break;
        case st_err_ipc:
            error_msg = "IPC connection failed";
            break;
        case st_err_peer:
            error_msg = "Error from peer";
            break;
        case stonith_pending:
            error_msg = "Stonith operation is in progress";
            break;
        case st_err_agent_fork:
            error_msg = "Call to fork() failed";
            break;
        case st_err_agent_args:
            error_msg = "Could not send arguments to the stonith device";
            break;
        case st_err_agent:
            error_msg = "Execution of the stonith agent failed";
            break;
    }

    if (error_msg == NULL) {
        crm_err("Unknown Stonith error code: %d", return_code);
        error_msg = "<unknown error>";
    }

    return error_msg;
}

gboolean
is_redhat_agent(const char *agent)
{
    int rc = 0;
    struct stat prop;
    char buffer[FILENAME_MAX + 1];

    snprintf(buffer, FILENAME_MAX, "%s/%s", RH_STONITH_DIR, agent);
    rc = stat(buffer, &prop);
    if (rc >= 0 && S_ISREG(prop.st_mode)) {
        return TRUE;
    }
    return FALSE;
}

const char *
get_stonith_provider(const char *agent, const char *provider)
{
    /* This function sucks */
    if (is_redhat_agent(agent)) {
        return "redhat";

    } else {
        Stonith *stonith_obj = stonith_new(agent);

        if (stonith_obj) {
            stonith_delete(stonith_obj);
            return "heartbeat";
        }
    }

    crm_err("No such device: %s", agent);
    return NULL;
}

static gint
stonithlib_GCompareFunc(gconstpointer a, gconstpointer b)
{
    int rc = 0;
    const stonith_notify_client_t *a_client = a;
    const stonith_notify_client_t *b_client = b;

    CRM_CHECK(a_client->event != NULL && b_client->event != NULL, return 0);
    rc = strcmp(a_client->event, b_client->event);
    if (rc == 0) {
        if (a_client->notify == NULL || b_client->notify == NULL) {
            return 0;

        } else if (a_client->notify == b_client->notify) {
            return 0;

        } else if (((long)a_client->notify) < ((long)b_client->notify)) {
            crm_err("callbacks for %s are not equal: %p vs. %p",
                    a_client->event, a_client->notify, b_client->notify);
            return -1;
        }
        crm_err("callbacks for %s are not equal: %p vs. %p",
                a_client->event, a_client->notify, b_client->notify);
        return 1;
    }
    return rc;
}

static int
get_stonith_token(IPC_Channel * ch, char **token)
{
    int rc = stonith_ok;
    xmlNode *reg_msg = NULL;
    const char *msg_type = NULL;
    const char *tmp_ticket = NULL;

    CRM_CHECK(ch != NULL, return st_err_missing);
    CRM_CHECK(token != NULL, return st_err_missing);

    crm_debug_4("Waiting for msg on command channel");

    reg_msg = xmlfromIPC(ch, MAX_IPC_DELAY);

    if (ch->ops->get_chan_status(ch) != IPC_CONNECT) {
        crm_err("No reply message - disconnected");
        free_xml(reg_msg);
        return st_err_connection;

    } else if (reg_msg == NULL) {
        crm_err("No reply message - empty");
        return st_err_ipc;
    }

    msg_type = crm_element_value(reg_msg, F_STONITH_OPERATION);
    tmp_ticket = crm_element_value(reg_msg, F_STONITH_CLIENTID);

    if (safe_str_neq(msg_type, CRM_OP_REGISTER)) {
        crm_err("Invalid registration message: %s", msg_type);
        rc = st_err_internal;

    } else if (tmp_ticket == NULL) {
        crm_err("No registration token provided");
        crm_log_xml_warn(reg_msg, "Bad reply");
        rc = st_err_internal;

    } else {
        crm_debug("Obtained registration token: %s", tmp_ticket);
        *token = crm_strdup(tmp_ticket);
    }

    free_xml(reg_msg);
    return rc;
}

xmlNode *
stonith_create_op(int call_id, const char *token, const char *op, xmlNode * data, int call_options)
{
    int rc = HA_OK;
    xmlNode *op_msg = create_xml_node(NULL, "stonith_command");

    CRM_CHECK(op_msg != NULL, return NULL);
    CRM_CHECK(token != NULL, return NULL);

    crm_xml_add(op_msg, F_XML_TAGNAME, "stonith_command");

    crm_xml_add(op_msg, F_TYPE, T_STONITH_NG);
    crm_xml_add(op_msg, F_STONITH_CALLBACK_TOKEN, token);
    crm_xml_add(op_msg, F_STONITH_OPERATION, op);
    crm_xml_add_int(op_msg, F_STONITH_CALLID, call_id);
    crm_debug_4("Sending call options: %.8lx, %d", (long)call_options, call_options);
    crm_xml_add_int(op_msg, F_STONITH_CALLOPTS, call_options);

    if (data != NULL) {
        add_message_xml(op_msg, F_STONITH_CALLDATA, data);
    }

    if (rc != HA_OK) {
        crm_err("Failed to create STONITH operation message");
        crm_log_xml(LOG_ERR, "BadOp", op_msg);
        free_xml(op_msg);
        return NULL;
    }

    return op_msg;
}

static void
stonith_destroy_op_callback(gpointer data)
{
    stonith_callback_client_t *blob = data;

    if (blob->timer && blob->timer->ref > 0) {
        g_source_remove(blob->timer->ref);
    }
    crm_free(blob->timer);
    crm_free(blob);
}

static int
stonith_api_signoff(stonith_t * stonith)
{
    stonith_private_t *native = stonith->private;

    crm_debug("Signing out of the STONITH Service");

    /* close channels */
    if (native->command_channel != NULL) {
        native->command_channel->ops->destroy(native->command_channel);
        native->command_channel = NULL;
    }

    if (native->callback_source != NULL) {
        G_main_del_IPC_Channel(native->callback_source);
        native->callback_source = NULL;
    }

    if (native->callback_channel != NULL) {
#ifdef BUG
        native->callback_channel->ops->destroy(native->callback_channel);
#endif
        native->callback_channel = NULL;
    }

    stonith->state = stonith_disconnected;
    return stonith_ok;
}

static int
stonith_api_signon(stonith_t * stonith, const char *name, int *stonith_fd)
{
    int rc = stonith_ok;
    xmlNode *hello = NULL;
    char *uuid_ticket = NULL;
    stonith_private_t *native = stonith->private;

    crm_debug_4("Connecting command channel");

    stonith->state = stonith_connected_command;
    native->command_channel = init_client_ipc_comms_nodispatch(stonith_channel);

    if (native->command_channel == NULL) {
        crm_debug("Connection to command channel failed");
        rc = st_err_connection;

    } else if (native->command_channel->ch_status != IPC_CONNECT) {
        crm_err("Connection may have succeeded," " but authentication to command channel failed");
        rc = st_err_authentication;
    }

    if (rc == stonith_ok) {
        rc = get_stonith_token(native->command_channel, &uuid_ticket);
        if (rc == stonith_ok) {
            native->token = uuid_ticket;
            uuid_ticket = NULL;

        } else {
            stonith->state = stonith_disconnected;
            native->command_channel->ops->disconnect(native->command_channel);
            return rc;
        }
    }

    native->callback_channel = init_client_ipc_comms_nodispatch(stonith_channel_callback);

    if (native->callback_channel == NULL) {
        crm_debug("Connection to callback channel failed");
        rc = st_err_connection;

    } else if (native->callback_channel->ch_status != IPC_CONNECT) {
        crm_err("Connection may have succeeded," " but authentication to command channel failed");
        rc = st_err_authentication;
    }

    if (rc == stonith_ok) {
        native->callback_channel->send_queue->max_qlen = 500;
        rc = get_stonith_token(native->callback_channel, &uuid_ticket);
        if (rc == stonith_ok) {
            crm_free(native->token);
            native->token = uuid_ticket;
        }
    }

    if (rc == stonith_ok) {
        CRM_CHECK(native->token != NULL,;
            );
        hello = stonith_create_op(0, native->token, CRM_OP_REGISTER, NULL, 0);
        crm_xml_add(hello, F_STONITH_CLIENTNAME, name);

        if (send_ipc_message(native->command_channel, hello) == FALSE) {
            rc = st_err_internal;
        }

        free_xml(hello);
    }

    if (rc == stonith_ok) {
        if (stonith_fd != NULL) {
            *stonith_fd =
                native->callback_channel->ops->get_recv_select_fd(native->callback_channel);

        } else {                /* do mainloop */

            crm_debug_4("Connecting callback channel");
            native->callback_source =
                G_main_add_IPC_Channel(G_PRIORITY_HIGH, native->callback_channel, FALSE,
                                       stonith_dispatch_internal, stonith,
                                       default_ipc_connection_destroy);

            if (native->callback_source == NULL) {
                crm_err("Callback source not recorded");
                rc = st_err_connection;

            } else {
                set_IPC_Channel_dnotify(native->callback_source, stonith_connection_destroy);
            }
        }
    }

    if (rc == stonith_ok) {
#if HAVE_MSGFROMIPC_TIMEOUT
        stonith->call_timeout = MAX_IPC_DELAY;
#endif
        crm_debug("Connection to STONITH successful");
        return stonith_ok;
    }

    crm_debug("Connection to STONITH failed: %s", stonith_error2string(rc));
    stonith->cmds->disconnect(stonith);
    return rc;
}

static int
stonith_set_notification(stonith_t * stonith, const char *callback, int enabled)
{
    xmlNode *notify_msg = create_xml_node(NULL, __FUNCTION__);
    stonith_private_t *native = stonith->private;

    if (stonith->state != stonith_disconnected) {
        crm_xml_add(notify_msg, F_STONITH_OPERATION, T_STONITH_NOTIFY);
        if (enabled) {
            crm_xml_add(notify_msg, F_STONITH_NOTIFY_ACTIVATE, callback);
        } else {
            crm_xml_add(notify_msg, F_STONITH_NOTIFY_DEACTIVATE, callback);
        }
        send_ipc_message(native->callback_channel, notify_msg);
    }

    free_xml(notify_msg);
    return stonith_ok;
}

static int
stonith_api_add_notification(stonith_t * stonith, const char *event,
                             void (*callback) (stonith_t * stonith, const char *event,
                                               xmlNode * msg))
{
    GList *list_item = NULL;
    stonith_notify_client_t *new_client = NULL;
    stonith_private_t *private = NULL;

    private = stonith->private;
    crm_debug_2("Adding callback for %s events (%d)", event, g_list_length(private->notify_list));

    crm_malloc0(new_client, sizeof(stonith_notify_client_t));
    new_client->event = event;
    new_client->notify = callback;

    list_item = g_list_find_custom(private->notify_list, new_client, stonithlib_GCompareFunc);

    if (list_item != NULL) {
        crm_warn("Callback already present");
        crm_free(new_client);
        return st_err_exists;

    } else {
        private->notify_list = g_list_append(private->notify_list, new_client);

        stonith_set_notification(stonith, event, 1);

        crm_debug_3("Callback added (%d)", g_list_length(private->notify_list));
    }
    return stonith_ok;
}

static int
stonith_api_del_notification(stonith_t * stonith, const char *event)
{
    GList *list_item = NULL;
    stonith_notify_client_t *new_client = NULL;
    stonith_private_t *private = NULL;

    crm_debug("Removing callback for %s events", event);

    private = stonith->private;
    crm_malloc0(new_client, sizeof(stonith_notify_client_t));
    new_client->event = event;
    new_client->notify = NULL;

    list_item = g_list_find_custom(private->notify_list, new_client, stonithlib_GCompareFunc);

    stonith_set_notification(stonith, event, 0);

    if (list_item != NULL) {
        stonith_notify_client_t *list_client = list_item->data;

        private->notify_list = g_list_remove(private->notify_list, list_client);
        crm_free(list_client);

        crm_debug_3("Removed callback");

    } else {
        crm_debug_3("Callback not present");
    }
    crm_free(new_client);
    return stonith_ok;
}

static gboolean
stonith_async_timeout_handler(gpointer data)
{
    struct timer_rec_s *timer = data;

    crm_debug("Async call %d timed out after %dms", timer->call_id, timer->timeout);
    stonith_perform_callback(timer->stonith, NULL, timer->call_id, st_err_timeout);

    /* Always return TRUE, never remove the handler
     * We do that in stonith_del_callback()
     */
    return TRUE;
}

static int
stonith_api_add_callback(stonith_t * stonith, int call_id, int timeout, bool only_success,
                         void *user_data, const char *callback_name,
                         void (*callback) (stonith_t * st, const xmlNode * msg, int call, int rc,
                                           xmlNode * output, void *userdata))
{
    stonith_callback_client_t *blob = NULL;
    stonith_private_t *private = NULL;

    CRM_CHECK(stonith != NULL, return st_err_missing);
    CRM_CHECK(stonith->private != NULL, return st_err_missing);
    private = stonith->private;

    if (call_id == 0) {
        private->op_callback = callback;

    } else if (call_id < 0) {
        if (only_success == FALSE) {
            callback(stonith, NULL, call_id, call_id, NULL, user_data);
        } else {
            crm_warn("STONITH call failed: %s", stonith_error2string(call_id));
        }
        return FALSE;
    }

    crm_malloc0(blob, sizeof(stonith_callback_client_t));
    blob->id = callback_name;
    blob->only_success = only_success;
    blob->user_data = user_data;
    blob->callback = callback;

    if (timeout > 0) {
        struct timer_rec_s *async_timer = NULL;

        crm_malloc0(async_timer, sizeof(struct timer_rec_s));
        blob->timer = async_timer;

        async_timer->stonith = stonith;
        async_timer->call_id = call_id;
        async_timer->timeout = timeout * 1100;
        async_timer->ref =
            g_timeout_add(async_timer->timeout, stonith_async_timeout_handler, async_timer);
    }

    g_hash_table_insert(private->stonith_op_callback_table, GINT_TO_POINTER(call_id), blob);

    return TRUE;
}

static int
stonith_api_del_callback(stonith_t * stonith, int call_id, bool all_callbacks)
{
    stonith_private_t *private = stonith->private;

    if (all_callbacks) {
        private->op_callback = NULL;
        g_hash_table_destroy(private->stonith_op_callback_table);
        private->stonith_op_callback_table = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                                   NULL,
                                                                   stonith_destroy_op_callback);

    } else if (call_id == 0) {
        private->op_callback = NULL;

    } else {
        g_hash_table_remove(private->stonith_op_callback_table, GINT_TO_POINTER(call_id));
    }
    return stonith_ok;
}

static void
stonith_dump_pending_op(gpointer key, gpointer value, gpointer user_data)
{
    int call = GPOINTER_TO_INT(key);
    stonith_callback_client_t *blob = value;

    crm_debug("Call %d (%s): pending", call, crm_str(blob->id));
}

void
stonith_dump_pending_callbacks(stonith_t * stonith)
{
    stonith_private_t *private = stonith->private;

    if (private->stonith_op_callback_table == NULL) {
        return;
    }
    return g_hash_table_foreach(private->stonith_op_callback_table, stonith_dump_pending_op, NULL);
}

void
stonith_perform_callback(stonith_t * stonith, xmlNode * msg, int call_id, int rc)
{
    xmlNode *output = NULL;
    stonith_private_t *private = NULL;
    stonith_callback_client_t *blob = NULL;
    stonith_callback_client_t local_blob;

    CRM_CHECK(stonith != NULL, return);
    CRM_CHECK(stonith->private != NULL, return);

    private = stonith->private;

    local_blob.id = NULL;
    local_blob.callback = NULL;
    local_blob.user_data = NULL;
    local_blob.only_success = FALSE;

    if (msg != NULL) {
        crm_element_value_int(msg, F_STONITH_RC, &rc);
        crm_element_value_int(msg, F_STONITH_CALLID, &call_id);
        output = get_message_xml(msg, F_STONITH_CALLDATA);
    }

    CRM_CHECK(call_id > 0, crm_warn("Strange or missing call-id"));

    blob = g_hash_table_lookup(private->stonith_op_callback_table, GINT_TO_POINTER(call_id));

    if (blob != NULL) {
        local_blob = *blob;
        blob = NULL;

        stonith_api_del_callback(stonith, call_id, FALSE);

    } else {
        crm_debug_2("No callback found for call %d", call_id);
        local_blob.callback = NULL;
    }

    if (stonith == NULL) {
        crm_debug("No stonith object supplied");
    }

    if (local_blob.callback != NULL && (rc == stonith_ok || local_blob.only_success == FALSE)) {
        crm_debug_2("Invoking callback %s for call %d", crm_str(local_blob.id), call_id);
        local_blob.callback(stonith, msg, call_id, rc, output, local_blob.user_data);

    } else if (private->op_callback == NULL && rc != stonith_ok) {
        crm_warn("STONITH command failed: %s", stonith_error2string(rc));
        crm_log_xml(LOG_DEBUG, "Failed STONITH Update", msg);
    }

    if (private->op_callback != NULL) {
        crm_debug_2("Invoking global callback for call %d", call_id);
        private->op_callback(stonith, msg, call_id, rc, output, NULL);
    }
    crm_debug_4("OP callback activated.");
}

static void
stonith_send_notification(gpointer data, gpointer user_data)
{
    struct notify_blob_s *blob = user_data;
    stonith_notify_client_t *entry = data;
    const char *event = NULL;

    if (blob->xml == NULL) {
        crm_warn("Skipping callback - NULL message");
        return;
    }

    event = crm_element_value(blob->xml, F_SUBTYPE);

    if (entry == NULL) {
        crm_warn("Skipping callback - NULL callback client");
        return;

    } else if (entry->notify == NULL) {
        crm_warn("Skipping callback - NULL callback");
        return;

    } else if (safe_str_neq(entry->event, event)) {
        crm_debug_4("Skipping callback - event mismatch %p/%s vs. %s", entry, entry->event, event);
        return;
    }

    crm_debug_4("Invoking callback for %p/%s event...", entry, event);
    entry->notify(blob->stonith, event, blob->xml);
    crm_debug_4("Callback invoked...");
}

int
stonith_send_command(stonith_t * stonith, const char *op, xmlNode * data, xmlNode ** output_data,
                     int call_options, int timeout)
{
    int rc = HA_OK;

    xmlNode *op_msg = NULL;
    xmlNode *op_reply = NULL;

    stonith_private_t *native = stonith->private;

    if (stonith->state == stonith_disconnected) {
        return st_err_connection;
    }

    if (output_data != NULL) {
        *output_data = NULL;
    }

    if (op == NULL) {
        crm_err("No operation specified");
        return st_err_missing;
    }

    stonith->call_id++;
    /* prevent call_id from being negative (or zero) and conflicting
     *    with the stonith_errors enum
     * use 2 because we use it as (stonith->call_id - 1) below
     */
    if (stonith->call_id < 1) {
        stonith->call_id = 1;
    }

    CRM_CHECK(native->token != NULL,;
        );
    op_msg = stonith_create_op(stonith->call_id, native->token, op, data, call_options);
    if (op_msg == NULL) {
        return st_err_missing;
    }

    crm_xml_add_int(op_msg, F_STONITH_TIMEOUT, timeout);
    crm_debug_3("Sending %s message to STONITH service, Timeout: %d", op, timeout);
    if (send_ipc_message(native->command_channel, op_msg) == FALSE) {
        crm_err("Sending message to STONITH service FAILED");
        free_xml(op_msg);
        return st_err_ipc;

    } else {
        crm_debug_3("Message sent");
    }

    free_xml(op_msg);

    if ((call_options & st_opt_discard_reply)) {
        crm_debug_3("Discarding reply");
        return stonith_ok;

    } else if (!(call_options & st_opt_sync_call)) {
        crm_debug_3("Async call, returning");
        CRM_CHECK(stonith->call_id != 0, return st_err_ipc);

        return stonith->call_id;
    }

    rc = IPC_OK;
    crm_debug_3("Waiting for a syncronous reply");

    rc = stonith_ok;
    while (IPC_ISRCONN(native->command_channel)) {
        int reply_id = -1;
        int msg_id = stonith->call_id;

        op_reply = xmlfromIPC(native->command_channel, timeout);
        if (op_reply == NULL) {
            rc = st_err_peer;
            break;
        }

        crm_element_value_int(op_reply, F_STONITH_CALLID, &reply_id);
        if (reply_id <= 0) {
            rc = st_err_peer;
            break;

        } else if (reply_id == msg_id) {
            crm_debug_3("Syncronous reply received");
            crm_log_xml(LOG_MSG, "Reply", op_reply);
            if (crm_element_value_int(op_reply, F_STONITH_RC, &rc) != 0) {
                rc = st_err_peer;
            }

            if (output_data != NULL && is_not_set(call_options, st_opt_discard_reply)) {
                *output_data = op_reply;
                op_reply = NULL;
            }

            goto done;

        } else if (reply_id < msg_id) {
            crm_debug("Recieved old reply: %d (wanted %d)", reply_id, msg_id);
            crm_log_xml(LOG_MSG, "Old reply", op_reply);

        } else if ((reply_id - 10000) > msg_id) {
            /* wrap-around case */
            crm_debug("Recieved old reply: %d (wanted %d)", reply_id, msg_id);
            crm_log_xml(LOG_MSG, "Old reply", op_reply);

        } else {
            crm_err("Received a __future__ reply:" " %d (wanted %d)", reply_id, msg_id);
        }
        free_xml(op_reply);
        op_reply = NULL;
    }

    if (op_reply == NULL && stonith->state == stonith_disconnected) {
        rc = st_err_connection;

    } else if (rc == stonith_ok && op_reply == NULL) {
        rc = st_err_peer;
    }

  done:
    if (IPC_ISRCONN(native->command_channel) == FALSE) {
        crm_err("STONITH disconnected: %d", native->command_channel->ch_status);
        stonith->state = stonith_disconnected;
    }

    free_xml(op_reply);
    return rc;
}

static gboolean
stonith_msgready(stonith_t * stonith)
{
    stonith_private_t *private = NULL;

    if (stonith == NULL) {
        crm_err("No STONITH!");
        return FALSE;
    }

    private = stonith->private;

    if (private->command_channel != NULL) {
        /* drain the channel */
        IPC_Channel *cmd_ch = private->command_channel;
        xmlNode *cmd_msg = NULL;

        while (cmd_ch->ch_status != IPC_DISCONNECT && cmd_ch->ops->is_message_pending(cmd_ch)) {
            /* this will happen when the STONITH exited from beneath us */
            cmd_msg = xmlfromIPC(cmd_ch, MAX_IPC_DELAY);
            free_xml(cmd_msg);
        }

    } else {
        crm_err("No command channel");
    }

    if (private->callback_channel == NULL) {
        crm_err("No callback channel");
        return FALSE;

    } else if (private->callback_channel->ch_status == IPC_DISCONNECT) {
        crm_info("Lost connection to the STONITH service [%d].",
                 private->callback_channel->farside_pid);
        return FALSE;

    } else if (private->callback_channel->ops->is_message_pending(private->callback_channel)) {
        crm_debug_4("Message pending on command channel [%d]",
                    private->callback_channel->farside_pid);
        return TRUE;
    }

    crm_debug_3("No message pending");
    return FALSE;
}

static int
stonith_rcvmsg(stonith_t * stonith)
{
    const char *type = NULL;
    stonith_private_t *private = NULL;
    struct notify_blob_s blob;

    if (stonith == NULL) {
        crm_err("No STONITH!");
        return FALSE;
    }

    blob.stonith = stonith;
    private = stonith->private;

    /* if it is not blocking mode and no message in the channel, return */
    if (stonith_msgready(stonith) == FALSE) {
        crm_debug_3("No message ready and non-blocking...");
        return 0;
    }

    /* IPC_INTR is not a factor here */
    blob.xml = xmlfromIPC(private->callback_channel, MAX_IPC_DELAY);
    if (blob.xml == NULL) {
        crm_warn("Received a NULL msg from STONITH service.");
        return 0;
    }

    /* do callbacks */
    type = crm_element_value(blob.xml, F_TYPE);
    crm_debug_4("Activating %s callbacks...", type);

    if (safe_str_eq(type, T_STONITH_NG)) {
        stonith_perform_callback(stonith, blob.xml, 0, 0);

    } else if (safe_str_eq(type, T_STONITH_NOTIFY)) {
        g_list_foreach(private->notify_list, stonith_send_notification, &blob);

    } else {
        crm_err("Unknown message type: %s", type);
        crm_log_xml_warn(blob.xml, "BadReply");
    }

    free_xml(blob.xml);

    return 1;
}

bool
stonith_dispatch(stonith_t * st)
{
    stonith_private_t *private = NULL;
    bool stay_connected = TRUE;

    CRM_CHECK(st != NULL, return FALSE);

    private = st->private;

    stay_connected = stonith_dispatch_internal(private->callback_channel, (gpointer *) st);
    return stay_connected;
}

gboolean
stonith_dispatch_internal(IPC_Channel * channel, gpointer user_data)
{
    stonith_t *stonith = user_data;
    stonith_private_t *private = NULL;
    gboolean stay_connected = TRUE;

    CRM_CHECK(stonith != NULL, return FALSE);

    private = stonith->private;
    CRM_CHECK(private->callback_channel == channel, return FALSE);

    while (stonith_msgready(stonith)) {
        /* invoke the callbacks but dont block */
        int rc = stonith_rcvmsg(stonith);

        if (rc < 0) {
            crm_err("Message acquisition failed: %d", rc);
            break;

        } else if (rc == 0) {
            break;
        }
    }

    if (private->callback_channel && private->callback_channel->ch_status != IPC_CONNECT) {
        crm_crit("Lost connection to the STONITH service [%d/callback].", channel->farside_pid);
        private->callback_source = NULL;
        stay_connected = FALSE;
    }

    if (private->command_channel && private->command_channel->ch_status != IPC_CONNECT) {
        crm_crit("Lost connection to the STONITH service [%d/command].", channel->farside_pid);
        private->callback_source = NULL;
        stay_connected = FALSE;
    }

    return stay_connected;
}

static int
stonith_api_free(stonith_t * stonith)
{
    int rc = stonith_ok;

    if (stonith->state != stonith_disconnected) {
        rc = stonith->cmds->disconnect(stonith);
    }

    if (stonith->state == stonith_disconnected) {
        stonith_private_t *private = stonith->private;

        g_hash_table_destroy(private->stonith_op_callback_table);
        crm_free(private->token);
        crm_free(stonith->private);
        crm_free(stonith->cmds);
        crm_free(stonith);
    }

    return rc;
}

void
stonith_api_delete(stonith_t * stonith)
{
    stonith_private_t *private = stonith->private;
    GList *list = private->notify_list;

    while (list != NULL) {
        stonith_notify_client_t *client = g_list_nth_data(list, 0);

        list = g_list_remove(list, client);
        crm_free(client);
    }

    stonith->cmds->free(stonith);
    stonith = NULL;
}

stonith_t *
stonith_api_new(void)
{
    stonith_t *new_stonith = NULL;
    stonith_private_t *private = NULL;

    crm_malloc0(new_stonith, sizeof(stonith_t));
    crm_malloc0(private, sizeof(stonith_private_t));
    new_stonith->private = private;

    private->stonith_op_callback_table = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                               NULL, stonith_destroy_op_callback);
    private->notify_list = NULL;

    new_stonith->call_id = 1;
    new_stonith->state = stonith_disconnected;

    crm_malloc0(new_stonith->cmds, sizeof(stonith_api_operations_t));

/* *INDENT-OFF* */
    new_stonith->cmds->free       = stonith_api_free;
    new_stonith->cmds->connect    = stonith_api_signon;
    new_stonith->cmds->disconnect = stonith_api_signoff;
    
    new_stonith->cmds->call       = stonith_api_call;
    new_stonith->cmds->fence      = stonith_api_fence;
    new_stonith->cmds->confirm    = stonith_api_confirm;
    new_stonith->cmds->history    = stonith_api_history;

    new_stonith->cmds->list       = stonith_api_device_list;
    new_stonith->cmds->metadata   = stonith_api_device_metadata;

    new_stonith->cmds->query           = stonith_api_query;
    new_stonith->cmds->remove_device   = stonith_api_remove_device;
    new_stonith->cmds->register_device = stonith_api_register_device;
    
    new_stonith->cmds->remove_callback       = stonith_api_del_callback;	
    new_stonith->cmds->register_callback     = stonith_api_add_callback;	
    new_stonith->cmds->remove_notification   = stonith_api_del_notification;
    new_stonith->cmds->register_notification = stonith_api_add_notification;
/* *INDENT-ON* */

    return new_stonith;
}

stonith_key_value_t *
stonith_key_value_add(stonith_key_value_t * kvp, const char *key, const char *value)
{
    stonith_key_value_t *p;

    crm_malloc(p, sizeof(stonith_key_value_t));
    p->next = kvp;
    p->key = crm_strdup(key);
    p->value = crm_strdup(value);
    return (p);
}

void
stonith_key_value_freeall(stonith_key_value_t * kvp, int keys, int values)
{
    stonith_key_value_t *p;

    while (kvp) {
        p = kvp->next;
        if (keys) {
            free(kvp->key);
        }
        if (values) {
            free(kvp->value);
        }
        free(kvp);
        kvp = p;
    }
}


int
stonith_api_cs_kick(int nodeid, int timeout, bool off)
{
    int rc = stonith_ok;
    stonith_t *st = NULL;
    enum stonith_call_options opts = st_opt_sync_call|st_opt_allow_suicide|st_opt_cs_nodeid;

    crm_log_init("st-client", LOG_INFO, FALSE, FALSE, 0, NULL);

    st = stonith_api_new();
    if(st) {
	rc = st->cmds->connect(st, crm_system_name, NULL);
    }

    if(st && rc == stonith_ok) {
        char *name = crm_itoa(nodeid);
        crm_info("Requesting that node %d be terminated", nodeid);
        rc = st->cmds->fence(st, opts, name, "off", 120);
        crm_free(name);
    }

    if(st) {
        st->cmds->disconnect(st);
        stonith_api_delete(st);
    }

    if(rc < stonith_ok) {
        crm_err("Could not terminate node %d: %s", nodeid, stonith_error2string(rc));
        rc = 1;

    } else {
        rc = 0;
    }
    return rc;
}

time_t
stonith_api_cs_time(int nodeid, bool in_progress)
{
    int rc = 0;
    time_t when = 0;
    time_t progress = 0;
    stonith_t *st = NULL;
    stonith_history_t *history, *hp = NULL;

    crm_log_init("st-client", LOG_INFO, FALSE, FALSE, 0, NULL);

    st = stonith_api_new();
    if(st) {
	rc = st->cmds->connect(st, crm_system_name, NULL);
    }

    if(st && rc == stonith_ok) {
        char *name = NULL;
        if(nodeid > 0) {
            name = crm_itoa(nodeid);
        }
        st->cmds->history(st, st_opt_sync_call|st_opt_cs_nodeid, name, &history, 120);
        crm_free(name);

        for(hp = history; hp; hp = hp->next) {
            if(in_progress) {
                if(hp->state != st_done && hp->state != st_failed) {
                    progress = time(NULL);
                }
                
            } else if(hp->state == st_done) {
                when = hp->completed;
            }
        }
    }

    if(progress) {
        crm_debug("Node %d is in the process of being shot", nodeid);
        when = progress;

    } else if(when != 0) {
        crm_debug("Node %d was last shot at: %s", nodeid, ctime(&when));

    } else {
        crm_debug("It does not appear node %d has been shot", nodeid);
    }

    if(st) {
        st->cmds->disconnect(st);
        stonith_api_delete(st);
    }
    return when;
}
