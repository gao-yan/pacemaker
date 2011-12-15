/* 
 * Copyright (C) 2009 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/common/ipc.h>
#include <crm/common/cluster.h>

#include <crm/stonith-ng.h>
#include <crm/stonith-ng-internal.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>
#include <internal.h>

#include <clplumbing/proctrack.h>

GHashTable *device_list = NULL;
static int active_children = 0;

static gboolean stonith_device_dispatch(gpointer user_data);
static void exec_child_done(ProcTrack* proc, int status, int signo, int rc, int waslogged);
static void exec_child_new(ProcTrack* p) { active_children++; }
static const char *exec_child_name(ProcTrack* p) {
    async_command_t *cmd = proctrack_data(p);
    return cmd->client?cmd->client:cmd->remote;
}

static ProcTrack_ops StonithdProcessTrackOps = {
	exec_child_done,
	exec_child_new,
	exec_child_name,
};


static void free_async_command(async_command_t *cmd) 
{
    crm_free(cmd->device);
    crm_free(cmd->action);
    crm_free(cmd->victim);
    crm_free(cmd->remote);
    crm_free(cmd->client);
    crm_free(cmd->origin);
    crm_free(cmd->op);
    crm_free(cmd);    
}

static async_command_t *create_async_command(xmlNode *msg) 
{
    async_command_t *cmd = NULL;
    xmlNode *op = get_xpath_object("//@"F_STONITH_ACTION, msg, LOG_ERR);
    const char *action = crm_element_value(op, F_STONITH_ACTION);

    CRM_CHECK(action != NULL, crm_log_xml_warn(msg, "NoAction"); return NULL);

    crm_malloc0(cmd, sizeof(async_command_t));
    crm_element_value_int(msg, F_STONITH_CALLID,   &(cmd->id));
    crm_element_value_int(msg, F_STONITH_CALLOPTS, &(cmd->options));
    crm_element_value_int(msg, F_STONITH_TIMEOUT,  &(cmd->timeout));

    cmd->timeout *= 1000;
    cmd->origin = crm_element_value_copy(msg, F_ORIG);
    cmd->remote = crm_element_value_copy(msg, F_STONITH_REMOTE);
    cmd->client = crm_element_value_copy(msg, F_STONITH_CLIENTID);
    cmd->op     = crm_element_value_copy(msg, F_STONITH_OPERATION);
    cmd->action = crm_strdup(action);
    cmd->victim = crm_element_value_copy(op, F_STONITH_TARGET);
    cmd->pt_ops = &StonithdProcessTrackOps;

    CRM_CHECK(cmd->op != NULL, crm_log_xml_warn(msg, "NoOp"); free_async_command(cmd); return NULL);
    CRM_CHECK(cmd->client != NULL || cmd->remote != NULL, crm_log_xml_warn(msg, "NoClient"));
    return cmd;
}

static int stonith_manual_ack(xmlNode *msg, remote_fencing_op_t *op) 
{
    struct _ProcTrack p;
    async_command_t *cmd = create_async_command(msg);
    xmlNode *dev = get_xpath_object("//@"F_STONITH_TARGET, msg, LOG_ERR);
    
    if(cmd == NULL) {
	return st_err_missing;
    }

    cmd->device = crm_strdup("manual_ack");
    cmd->remote = crm_strdup(op->id);
    
    crm_notice("Injecting manual confirmation that %s is safely off/down",
               crm_element_value(dev, F_STONITH_TARGET));

    memset(&p, 0, sizeof(struct _ProcTrack));
    p.privatedata = cmd;

    exec_child_done(&p, 0, 0, 0, FALSE);
    return stonith_ok;
}

static gboolean stonith_device_execute(stonith_device_t *device)
{
    int rc = 0;
    int exec_rc = 0;
    async_command_t *cmd = NULL;
    CRM_CHECK(device != NULL, return FALSE);

    if(device->active_pid) {	
	crm_trace("%s is still active with pid %u", device->id, device->active_pid);
	return TRUE;
    }
    
    if(device->pending_ops) {
	GList *first = device->pending_ops;
	device->pending_ops = g_list_remove_link(device->pending_ops, first);
	cmd = first->data;
	g_list_free_1(first);
    }

    if(cmd == NULL) {
	crm_info("Nothing to do for %s", device->id);
	return TRUE;
    }
    
    cmd->device = crm_strdup(device->id);
    exec_rc = run_stonith_agent(device->agent, cmd->action, cmd->victim,
				device->params, device->aliases, &rc, NULL, cmd);

    if(exec_rc > 0) {
	crm_debug("Operation %s%s%s on %s is active with pid: %d",
		  cmd->action, cmd->victim?" for node ":"", cmd->victim?cmd->victim:"",
		  device->id, exec_rc);
	device->active_pid = exec_rc;
	
    } else {
	struct _ProcTrack p;
	memset(&p, 0, sizeof(struct _ProcTrack));
	p.privatedata = cmd;
	
	crm_warn("Operation %s%s%s on %s failed (%d/%d)",
		 cmd->action, cmd->victim?" for node ":"", cmd->victim?cmd->victim:"",
		 device->id, exec_rc, rc);
	exec_child_done(&p, 0, 0, rc<0?rc:exec_rc, 0);
    }
    return TRUE;
}

static gboolean stonith_device_dispatch(gpointer user_data)
{
    return stonith_device_execute(user_data);
}

static void schedule_stonith_command(async_command_t *cmd, stonith_device_t *device)
{
    CRM_CHECK(cmd != NULL, return);
    CRM_CHECK(device != NULL, return);

    crm_trace("Scheduling %s on %s", cmd->action, device->id);
    device->pending_ops = g_list_append(device->pending_ops, cmd);
    mainloop_set_trigger(device->work);
}

static void free_device(gpointer data)
{
    GListPtr gIter = NULL;
    stonith_device_t *device = data;

    g_hash_table_destroy(device->params);
    g_hash_table_destroy(device->aliases);
    
    for(gIter = device->pending_ops; gIter != NULL; gIter = gIter->next) {
	struct _ProcTrack p;
	async_command_t *cmd = gIter->data;

	memset(&p, 0, sizeof(struct _ProcTrack));
	p.privatedata = cmd;
	
	crm_warn("Removal of device '%s' purged operation %s", device->id, cmd->action);
	exec_child_done(&p, 0, 0, st_err_unknown_device, 0);
	free_async_command(cmd);
    }
    g_list_free(device->pending_ops);

    slist_basic_destroy(device->targets);
    crm_free(device->namespace);
    crm_free(device->agent);
    crm_free(device->id);
    crm_free(device);
}

static GHashTable *build_port_aliases(const char *hostmap, GListPtr *targets) 
{
    char *name = NULL;
    int last = 0, lpc = 0, max = 0, added = 0;
    GHashTable *aliases = g_hash_table_new_full(crm_str_hash, g_str_equal, g_hash_destroy_str, g_hash_destroy_str);
    
    if(hostmap == NULL) {
	return aliases;
    }
    
    max = strlen(hostmap);
    for(; lpc <= max; lpc++) {
	switch(hostmap[lpc]) {
	    /* Assignment chars */
	    case '=':
	    case ':':
		if(lpc > last) {
		    crm_free(name);
		    crm_malloc0(name, 1 + lpc - last);
		    strncpy(name, hostmap + last, lpc - last);
		}
		last = lpc + 1;
		break;
		
	    /* Delimeter chars */
	    /* case ',': Potentially used to specify multiple ports */
	    case 0:
	    case ';':
	    case ' ':
	    case '\t':
		if(name) {
		    char *value = NULL;
		    crm_malloc0(value, 1 + lpc - last);
		    strncpy(value, hostmap + last, lpc - last);
		    
		    crm_debug("Adding alias '%s'='%s'", name, value);
		    g_hash_table_replace(aliases, name, value);
		    if(targets) {
			*targets = g_list_append(*targets, crm_strdup(value));
		    }
		    value=NULL;
		    name=NULL;
		    added++;

		} else if(lpc > last) {
		    crm_debug("Parse error at offset %d near '%s'", lpc-last, hostmap+last);
		}
		
		last = lpc + 1;
		break;
	}

    	if(hostmap[lpc] == 0) {
	    break;   
	}
    }

    if(added == 0) {
	crm_info("No host mappings detected in '%s'", hostmap);
    }
    
    crm_free(name);
    return aliases;
}

static void parse_host_line(const char *line, GListPtr *output) 
{
    int lpc = 0;
    int max = 0;
    int last = 0;

    if(line) {
	max = strlen(line);
    } else {
	return;
    }

    /* Check for any complaints about additional parameters that the device doesn't understand */
    if(strstr(line, "invalid") || strstr(line, "variable")) {
	crm_debug("Skipping: %s", line);
	return;
    }
    
    crm_debug_2("Processing: %s", line);
    /* Skip initial whitespace */
    for(lpc = 0; lpc <= max && isspace(line[lpc]); lpc++) {
	last = lpc+1;
    }

    /* Now the actual content */
    for(lpc = 0; lpc <= max; lpc++) {
	gboolean a_space = isspace(line[lpc]);
	if(a_space && lpc < max && isspace(line[lpc+1])) {
	    /* fast-forward to the end of the spaces */
	
	} else if(a_space || line[lpc] == ',' || line[lpc] == 0) {
	    int rc = 0;
	    char *entry = NULL;
	    
	    crm_malloc0(entry, 1 + lpc - last);
	    rc = sscanf(line+last, "%[a-zA-Z0-9_-.]", entry);
	    if(rc != 1) {
		crm_warn("Could not parse (%d %d): %s", last, lpc, line+last);
		
	    } else if(safe_str_neq(entry, "on") && safe_str_neq(entry, "off")) {
		crm_debug_2("Adding '%s'", entry);
		*output = g_list_append(*output, entry);
		entry = NULL;
	    }
	    
	    crm_free(entry);
	    last = lpc + 1;
	}
    }
}

static GListPtr parse_host_list(const char *hosts) 
{
    int lpc = 0;
    int max = 0;
    int last = 0;
    GListPtr output = NULL;

    if(hosts == NULL) {
	return output;
    }
    
    max = strlen(hosts);
    for(lpc = 0; lpc <= max; lpc++) {
	if(hosts[lpc] == '\n' || hosts[lpc] == 0) {
	    char *line = NULL;

	    crm_malloc0(line, 2 + lpc - last);
	    snprintf(line, 1 + lpc - last, "%s", hosts+last);
	    parse_host_line(line, &output);
	    crm_free(line);

	    last = lpc + 1;	    
	}
    }
    
    return output;
}

static stonith_device_t *build_device_from_xml(xmlNode *msg) 
{
    xmlNode *dev = get_xpath_object("//"F_STONITH_DEVICE, msg, LOG_ERR);
    stonith_device_t *device = NULL;

    crm_malloc0(device, sizeof(stonith_device_t));
    device->id = crm_element_value_copy(dev, XML_ATTR_ID);
    device->agent = crm_element_value_copy(dev, "agent");
    device->namespace = crm_element_value_copy(dev, "namespace");
    device->params = xml2list(dev);
    
    g_hash_table_insert(device->params, crm_strdup(CRM_META "_" F_STONITH_DEVICE), crm_strdup(device->id));
    device->work = mainloop_add_trigger(G_PRIORITY_HIGH, stonith_device_dispatch, device);
    /* TODO: Hook up priority */
    
    return device;
}

static int stonith_device_register(xmlNode *msg) 
{
    const char *value = NULL;
    stonith_device_t *device = build_device_from_xml(msg);

    value = g_hash_table_lookup(device->params, STONITH_ATTR_HOSTLIST);
    if(value) {
	device->targets = parse_host_list(value);
    }
	
    value = g_hash_table_lookup(device->params, STONITH_ATTR_HOSTMAP);
    device->aliases = build_port_aliases(value, &(device->targets));

    g_hash_table_replace(device_list, device->id, device);

    crm_info("Added '%s' to the device list (%d active devices)", device->id, g_hash_table_size(device_list));
    return stonith_ok;
}

static int stonith_device_remove(xmlNode *msg) 
{
    xmlNode *dev = get_xpath_object("//"F_STONITH_DEVICE, msg, LOG_ERR);
    const char *id = crm_element_value(dev, XML_ATTR_ID);
    if(g_hash_table_remove(device_list, id)) {
	crm_info("Removed '%s' from the device list (%d active devices)",
		 id, g_hash_table_size(device_list));
    } else {
	crm_info("Device '%s' not found (%d active devices)",
		 id, g_hash_table_size(device_list));
    }
    
    return stonith_ok;
}

static gboolean string_in_list(GListPtr list, const char *item)
{
    int lpc = 0;
    int max = g_list_length(list);
    for(lpc = 0; lpc < max; lpc ++) {
	const char *value = g_list_nth_data(list, lpc);
	if(safe_str_eq(item, value)) {
	    return TRUE;
	}
    }
    return FALSE;
}

static int stonith_device_action(xmlNode *msg, char **output) 
{
    int rc = stonith_ok;
    xmlNode *dev = get_xpath_object("//"F_STONITH_DEVICE, msg, LOG_ERR);
    const char *id = crm_element_value(dev, F_STONITH_DEVICE);

    async_command_t *cmd = NULL;
    stonith_device_t *device = NULL;

    if(id) {
	crm_debug_2("Looking for '%s'", id);
	device = g_hash_table_lookup(device_list, id);
    }

    if(device) {
	cmd = create_async_command(msg);
	if(cmd == NULL) {
	    free_device(device);
	    return st_err_internal;
	}

	schedule_stonith_command(cmd, device);
	rc = stonith_pending;
	
    } else {
	crm_notice("Device %s not found", id?id:"<none>");
	rc = st_err_unknown_device;
    }
    return rc;
}

static gboolean can_fence_host_with_device(stonith_device_t *dev, const char *host)
{
    gboolean can = FALSE;
    const char *alias = host;
    const char *check_type = NULL;

    if(dev == NULL) {
	return FALSE;

    } else if(host == NULL) {
	return TRUE;
    }

    if(g_hash_table_lookup(dev->aliases, host)) {
	alias = g_hash_table_lookup(dev->aliases, host);
    }

    check_type = g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTCHECK);
    
    if(check_type == NULL) {

	if(g_hash_table_lookup(dev->params, STONITH_ATTR_HOSTLIST)) {
	    check_type = "static-list";
	} else {
	    check_type = "dynamic-list";
	}
    }
    
    if(safe_str_eq(check_type, "none")) {
	can = TRUE;

    } else if(safe_str_eq(check_type, "static-list")) {

	/* Presence in the hostmap is sufficient
	 * Only use if all hosts on which the device can be active can always fence all listed hosts
	 */

	if(string_in_list(dev->targets, host)) {
	    can = TRUE;
	}

    } else if(safe_str_eq(check_type, "dynamic-list")) {
	time_t now = time(NULL);

	/* Host/alias must be in the list output to be eligable to be fenced
	 *
	 * Will cause problems if down'd nodes aren't listed or (for virtual nodes)
	 *  if the guest is still listed despite being moved to another machine
	 */
	
	if(dev->targets_age < 0) {
	    crm_trace("Port list queries disabled for %s", dev->id);
	    
	} else if(dev->targets == NULL || dev->targets_age + 60 < now) {
	    char *output = NULL;
	    int rc = stonith_ok;
	    int exec_rc = stonith_ok;
	    
	    /* Check for the target's presence in the output of the 'list' command */
	    slist_basic_destroy(dev->targets);
	    dev->targets = NULL;
	    
	    exec_rc = run_stonith_agent(dev->agent, "list", NULL, dev->params, NULL, &rc, &output, NULL);
	    if(exec_rc < 0 || rc != 0) {
		crm_notice("Disabling port list queries for %s (%d/%d): %s",
				dev->id, exec_rc, rc, output);
		dev->targets_age = -1;
		
	    } else {
		crm_info("Refreshing port list for %s", dev->id);
		dev->targets = parse_host_list(output);
		dev->targets_age = now;
	    }
	
	    crm_free(output);
	}
	
	if(string_in_list(dev->targets, alias)) {
	    can = TRUE;
	}

    } else if(safe_str_eq(check_type, "status")) {
	int rc = 0;
	int exec_rc = 0;

	/* Run the status operation for the device/target combination
	 * Will cause problems if the device doesn't return 2 for down'd nodes or
	 *  (for virtual nodes) if the device doesn't return 1 for guests that
	 *  have been moved to another host
	 */

	exec_rc = run_stonith_agent(
	    dev->agent, "status", host, dev->params, dev->aliases, &rc, NULL, NULL);

	if(exec_rc != 0) {
	    crm_err("Could not invoke %s: rc=%d", dev->id, exec_rc);

	} else if(rc == 1 /* unkown */) {
	    crm_debug_2("Host %s is not known by %s", host, dev->id);
	    
	} else if(rc == 0 /* active */ || rc == 2 /* inactive */) {
	    can = TRUE;

	} else {
	    crm_err("Unkown result calling %s for %s with %s: rc=%d", "status", host, dev->id, rc);
	}

    } else {
	crm_err("Unknown check type: %s", check_type);
    }

    if(safe_str_eq(host, alias)) {
	crm_info("%s can%s fence %s: %s", dev->id, can?"":" not", host, check_type);
    } else {
	crm_info("%s can%s fence %s (aka. '%s'): %s", dev->id, can?"":" not", host, alias, check_type);
    }
    return can;
}


struct device_search_s 
{
	const char *host;
	GListPtr capable;
};

static void search_devices(
    gpointer key, gpointer value, gpointer user_data) 
{
    stonith_device_t *dev = value;
    struct device_search_s *search = user_data;
    if(can_fence_host_with_device(dev, search->host)) {
	search->capable = g_list_append(search->capable, value);
    }
}

static int stonith_query(xmlNode *msg, xmlNode **list) 
{
    struct device_search_s search;
    int available_devices = 0;
    xmlNode *dev = get_xpath_object("//@"F_STONITH_TARGET, msg, LOG_DEBUG_3);
	
    search.host = NULL;
    search.capable = NULL;

    if(dev) {
        const char *device = crm_element_value(dev, F_STONITH_DEVICE);
	search.host = crm_element_value(dev, F_STONITH_TARGET);
        if(device && safe_str_eq(device, "manual_ack")) {
            /* No query necessary */
            if(list) {
                *list = NULL;
            }
            return stonith_ok;
        }
    }
    
    crm_log_xml_debug(msg, "Query");
	
    g_hash_table_foreach(device_list, search_devices, &search);
    available_devices = g_list_length(search.capable);
    if(search.host) {
	crm_debug("Found %d matching devices for '%s'",
		 available_devices, search.host);
    } else {
	crm_debug("%d devices installed", available_devices);
    }
    
    /* Pack the results into data */
    if(list) {
	GListPtr lpc = NULL;
	*list = create_xml_node(NULL, __FUNCTION__);
	crm_xml_add(*list, F_STONITH_TARGET, search.host);
	crm_xml_add_int(*list, "st-available-devices", available_devices);
	for(lpc = search.capable; lpc != NULL; lpc = lpc->next) {
	    stonith_device_t *device = (stonith_device_t*)lpc->data;
	    dev = create_xml_node(*list, F_STONITH_DEVICE);
	    crm_xml_add(dev, XML_ATTR_ID, device->id);
	    crm_xml_add(dev, "namespace", device->namespace);
	    crm_xml_add(dev, "agent", device->agent);
	    if(search.host == NULL) {
		xmlNode *attrs = create_xml_node(dev, XML_TAG_ATTRS);
		g_hash_table_foreach(device->params, hash2field, attrs);
	    }
	}
    }
    
    g_list_free(search.capable);

    return available_devices;
}

static void log_operation(async_command_t *cmd, int rc, int pid, const char *next, const char *output) 
{
    if(rc == 0) {
	next = NULL;
    }
    
    if(cmd->victim != NULL) {
	do_crm_log(rc==0?LOG_INFO:LOG_ERR,
		   "Operation '%s' [%d] (call %d from %s) for host '%s' with device '%s' returned: %d%s%s",
		   cmd->action, pid, cmd->id, cmd->client, cmd->victim, cmd->device, rc,
		   next?". Trying: ":"", next?next:"");
    } else {
	do_crm_log(rc==0?LOG_DEBUG:LOG_NOTICE,
		   "Operation '%s' [%d] for device '%s' returned: %d%s%s",
		   cmd->action, pid, cmd->device, rc, next?". Trying: ":"", next?next:"");
    }

    if(output) {
	/* Logging the whole string confuses syslog when the string is xml */ 
	char *local_copy = crm_strdup(output);
	int lpc = 0, last = 0, more = strlen(local_copy);
	for(lpc = 0; lpc < more; lpc++) {
	    if(local_copy[lpc] == '\n' || local_copy[lpc] == 0) {
		local_copy[lpc] = 0;
		do_crm_log(rc==0?LOG_INFO:LOG_ERR, "%s: %s",
			   cmd->device, local_copy+last);
		last = lpc+1;
	    }
	}
	crm_debug("%s output: %s (total %d bytes)", cmd->device, local_copy+last, more);
	crm_free(local_copy);
    }
}

#define READ_MAX 500
static void
exec_child_done(ProcTrack* proc, int status, int signum, int rc, int waslogged)
{
    int len = 0;
    int more = 0;
    gboolean bcast = FALSE;
    
    char *output = NULL;  
    xmlNode *data = NULL;
    xmlNode *reply = NULL;

    int pid = proctrack_pid(proc);
    stonith_device_t *device = NULL;
    async_command_t *cmd = proctrack_data(proc);
    
    CRM_CHECK(cmd != NULL, return);
    active_children--;

    /* The device is ready to do something else now */
    device = g_hash_table_lookup(device_list, cmd->device);
    if(device) {
	device->active_pid = 0;
	mainloop_set_trigger(device->work);
    }

    if( signum ) {
	rc = st_err_signal;
	if( proctrack_timedout(proc) ) {
	    crm_warn("Child '%d' performing action '%s' with '%s' timed out",
		     pid, cmd->action, cmd->device);
	    rc = st_err_timeout;
	}
    }

    do {
	char buffer[READ_MAX];

	errno = 0;
	if(cmd->stdout > 0) {
	    memset(&buffer, 0, READ_MAX);
	    more = read(cmd->stdout, buffer, READ_MAX-1);
	    crm_trace("Got %d more bytes: %s", more, buffer);
	}
	
	if(more > 0) {
	    crm_realloc(output, len + more + 1);
	    sprintf(output+len, "%s", buffer);
	    len += more;
	}
	
    } while (more == (READ_MAX-1) || (more < 0 && errno == EINTR));

    if(cmd->stdout) {
	close(cmd->stdout);
	cmd->stdout = 0;
    }

    if(rc != 0 && cmd->device_next) {
	stonith_device_t *dev = cmd->device_next->data;

	log_operation(cmd, rc, pid, dev->id, output);
	
	cmd->device_next = cmd->device_next->next;
	schedule_stonith_command(cmd, dev);
	goto done;
    }

    if(rc > 0) {
	rc = st_err_generic;
    }
    
    reply = stonith_construct_async_reply(cmd, output, data, rc);
    if(safe_str_eq(cmd->action, "metadata")) {
	/* Too verbose to log */
	crm_free(output); output = NULL;

    } else if(crm_str_eq(cmd->action, "reboot", TRUE)
	   || crm_str_eq(cmd->action, "poweroff", TRUE)
	   || crm_str_eq(cmd->action, "poweron", TRUE)
	   || crm_str_eq(cmd->action, "off", TRUE)
	   || crm_str_eq(cmd->action, "on", TRUE)) {
	bcast = TRUE;
    }

    log_operation(cmd, rc, pid, NULL, output);
    crm_log_xml_debug_3(reply, "Reply");
    
    if(bcast && !stand_alone) {
	/* Send reply as T_STONITH_NOTIFY so everyone does notifications
	 * Potentially limit to unsucessful operations to the originator?
	 */
	crm_xml_add(reply, F_STONITH_OPERATION, T_STONITH_NOTIFY);
	send_cluster_message(NULL, crm_msg_stonith_ng, reply, FALSE);

    } else if(cmd->origin) {
	send_cluster_message(cmd->origin, crm_msg_stonith_ng, reply, FALSE);

    } else {
	do_local_reply(reply, cmd->client, cmd->options & st_opt_sync_call, FALSE);
    }
    
    free_async_command(cmd);
  done:

    reset_proctrack_data(proc);
    crm_free(output);
    free_xml(reply);
    free_xml(data);
}

static gint sort_device_priority(gconstpointer a, gconstpointer b)
{
    const stonith_device_t *dev_a = a;
    const stonith_device_t *dev_b = a;
    if(dev_a->priority > dev_b->priority) {
	return -1;
    } else if(dev_a->priority < dev_b->priority) {
	return 1;
    }
    return 0;
}

static int stonith_fence(xmlNode *msg) 
{
    int options = 0;
    struct device_search_s search;
    stonith_device_t *device = NULL;
    async_command_t *cmd = create_async_command(msg);
    xmlNode *dev = get_xpath_object("//@"F_STONITH_TARGET, msg, LOG_ERR);
    
    if(cmd == NULL) {
	return st_err_internal;
    }
    
    search.capable = NULL;
    search.host = crm_element_value(dev, F_STONITH_TARGET);

    crm_element_value_int(msg, F_STONITH_CALLOPTS, &options);
    if(options & st_opt_cs_nodeid) {
        int nodeid = crm_atoi(search.host, NULL);
        crm_node_t *node = crm_get_peer(nodeid, NULL);
        if(node) {
            search.host = node->uname;
        }
    }

    crm_log_xml_info(msg, "Exec");
    
    g_hash_table_foreach(device_list, search_devices, &search);
    crm_info("Found %d matching devices for '%s'", g_list_length(search.capable), search.host);

    if(g_list_length(search.capable) == 0) {
	free_async_command(cmd);	
	return st_err_none_available;
    }

    /* Order based on priority */
    search.capable = g_list_sort(search.capable, sort_device_priority);
    
    device = search.capable->data;
    cmd->device = device->id;

    if(g_list_length(search.capable) > 1) {
	cmd->device_list = search.capable;
    }

    schedule_stonith_command(cmd, device);
    return stonith_pending;
}

xmlNode *stonith_construct_reply(xmlNode *request, char *output, xmlNode *data, int rc) 
{
    int lpc = 0;
    xmlNode *reply = NULL;
	
    const char *name = NULL;
    const char *value = NULL;
    const char *names[] = {
	F_STONITH_OPERATION,
	F_STONITH_CALLID,
	F_STONITH_CLIENTID,
	F_STONITH_REMOTE,
	F_STONITH_CALLOPTS
    };

    crm_debug_4("Creating a basic reply");
    reply = create_xml_node(NULL, T_STONITH_REPLY);

    crm_xml_add(reply, "st_origin", __FUNCTION__);
    crm_xml_add(reply, F_TYPE, T_STONITH_NG);
    crm_xml_add(reply, "st_output", output);
    crm_xml_add_int(reply, F_STONITH_RC, rc);

    CRM_CHECK(request != NULL, crm_warn("Can't create a sane reply"); return reply);
    for(lpc = 0; lpc < DIMOF(names); lpc++) {
	name = names[lpc];
	value = crm_element_value(request, name);
	crm_xml_add(reply, name, value);
    }

    if(data != NULL) {
	crm_debug_4("Attaching reply output");
	add_message_xml(reply, F_STONITH_CALLDATA, data);
    }
    return reply;
}

xmlNode *stonith_construct_async_reply(async_command_t *cmd, char *output, xmlNode *data, int rc) 
{
    xmlNode *reply = NULL;

    crm_debug_4("Creating a basic reply");
    reply = create_xml_node(NULL, T_STONITH_REPLY);

    crm_xml_add(reply, "st_origin", __FUNCTION__);
    crm_xml_add(reply, F_TYPE, T_STONITH_NG);

    crm_xml_add(reply, F_STONITH_OPERATION, cmd->op);
    crm_xml_add(reply, F_STONITH_REMOTE, cmd->remote);
    crm_xml_add(reply, F_STONITH_CLIENTID, cmd->client);
    crm_xml_add_int(reply, F_STONITH_CALLID, cmd->id);
    crm_xml_add_int(reply, F_STONITH_CALLOPTS, cmd->options);
    
    crm_xml_add_int(reply, F_STONITH_RC, rc);
    crm_xml_add(reply, "st_output", output);

    if(data != NULL) {
	crm_info("Attaching reply output");
	add_message_xml(reply, F_STONITH_CALLDATA, data);
    }
    return reply;
}

void
stonith_command(stonith_client_t *client, xmlNode *request, const char *remote)
{
    int call_options = 0;
    int rc = st_err_generic;

    gboolean is_reply = FALSE;
    gboolean always_reply = FALSE;

    xmlNode *reply = NULL;
    xmlNode *data = NULL;

    char *output = NULL;
    const char *op = crm_element_value(request, F_STONITH_OPERATION);
    const char *client_id = crm_element_value(request, F_STONITH_CLIENTID);
    
    crm_element_value_int(request, F_STONITH_CALLOPTS, &call_options);
    if(get_xpath_object("//"T_STONITH_REPLY, request, LOG_DEBUG_3)) {
	is_reply = TRUE;
    }
    
    if(device_list == NULL) {
	device_list = g_hash_table_new_full(
	    crm_str_hash, g_str_equal, NULL, free_device);
    }
    
    crm_debug("Processing %s%s from %s (%16x)", op, is_reply?" reply":"",
	      client?client->name:remote, call_options);

    if(crm_str_eq(op, CRM_OP_REGISTER, TRUE)) {
	return;
	    
    } else if(crm_str_eq(op, STONITH_OP_DEVICE_ADD, TRUE)) {
	rc = stonith_device_register(request);
	do_stonith_notify(call_options, op, rc, request, NULL);

    } else if(crm_str_eq(op, STONITH_OP_DEVICE_DEL, TRUE)) {
	rc = stonith_device_remove(request);
	do_stonith_notify(call_options, op, rc, request, NULL);
	

    } else if(crm_str_eq(op, STONITH_OP_CONFIRM, TRUE)) {
	async_command_t *cmd = create_async_command(request);
	xmlNode *reply = stonith_construct_async_reply(cmd, NULL, NULL, 0);

	crm_xml_add(reply, F_STONITH_OPERATION, T_STONITH_NOTIFY);
	crm_notice("Broadcasting manual fencing confirmation for node %s", cmd->victim);
	send_cluster_message(NULL, crm_msg_stonith_ng, reply, FALSE);

	free_async_command(cmd);
	free_xml(reply);

    } else if(crm_str_eq(op, STONITH_OP_EXEC, TRUE)) {
	rc = stonith_device_action(request, &output);

    } else if(is_reply && crm_str_eq(op, STONITH_OP_QUERY, TRUE)) {
	process_remote_stonith_query(request);
	return;
	
    } else if(crm_str_eq(op, STONITH_OP_QUERY, TRUE)) {
	create_remote_stonith_op(client_id, request, TRUE); /* Record it for the future notification */
	rc = stonith_query(request, &data);
        always_reply = TRUE;
        if(!data) {
            return;
        }
        
    } else if(is_reply && crm_str_eq(op, T_STONITH_NOTIFY, TRUE)) {
	process_remote_stonith_exec(request);
	return;
	
    } else if(crm_str_eq(op, T_STONITH_NOTIFY, TRUE)) {
	const char *flag_name = NULL;

	flag_name = crm_element_value(request, F_STONITH_NOTIFY_ACTIVATE);
	if(flag_name) {
	    crm_debug("Setting %s callbacks for %s (%s): ON",
		      flag_name, client->name, client->id);
	    client->flags |= get_stonith_flag(flag_name);
	}
	
	flag_name = crm_element_value(request, F_STONITH_NOTIFY_DEACTIVATE);
	if(flag_name) {
	    crm_debug("Setting %s callbacks for %s (%s): off",
		      flag_name, client->name, client->id);
	    client->flags |= get_stonith_flag(flag_name);
	}
	return;

    /* } else if(is_reply && crm_str_eq(op, STONITH_OP_FENCE, TRUE)) { */
    /* 	process_remote_stonith_exec(request); */
    /* 	return; */

    } else if(is_reply == FALSE && crm_str_eq(op, STONITH_OP_FENCE, TRUE)) {

        if(remote || stand_alone) {
            rc = stonith_fence(request);
            
        } else if(call_options & st_opt_manual_ack) {
	    remote_fencing_op_t *rop = initiate_remote_stonith_op(client, request, TRUE);
            rc = stonith_manual_ack(request, rop);
            
        } else if(call_options & st_opt_local_first) {
	    rc = stonith_fence(request);
	    if(rc < 0) {
		initiate_remote_stonith_op(client, request, FALSE);
		return;
	    }

	} else {
	    initiate_remote_stonith_op(client, request, FALSE);
	    return;
	}

    } else if (crm_str_eq(op, STONITH_OP_FENCE_HISTORY, TRUE)) {
	rc = stonith_fence_history(request, &data);
	always_reply = TRUE;

    } else {
	crm_err("Unknown %s%s from %s", op, is_reply?" reply":"",
		 client?client->name:remote);
	crm_log_xml_warn(request, "UnknownOp");
    }

    do_crm_log(rc>0?LOG_DEBUG:LOG_INFO,"Processed %s%s from %s: rc=%d", op, is_reply?" reply":"",
	       client?client->name:remote, rc);
    
    if(is_reply || rc == stonith_pending) {
	/* Nothing (yet) */	
	
    } else if(remote) {
	reply = stonith_construct_reply(request, output, data, rc);
	send_cluster_message(remote, crm_msg_stonith_ng, reply, FALSE);
	free_xml(reply);

    } else if(rc <= 0 || always_reply) {
	reply = stonith_construct_reply(request, output, data, rc);
	do_local_reply(reply, client_id, call_options & st_opt_sync_call, remote!=NULL);
	free_xml(reply);
    }

    crm_free(output);
    free_xml(data);
}
