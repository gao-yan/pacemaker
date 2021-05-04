/*
 * Copyright 2012-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>         // uint32_t, uint64_t
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include <dirent.h>

#include <crm/crm.h>
#include <crm/lrmd.h>
#include <crm/lrmd_internal.h>
#include <crm/services.h>
#include <crm/common/mainloop.h>
#include <crm/common/ipc_internal.h>
#include <crm/common/remote_internal.h>
#include <crm/msg_xml.h>

#include <crm/stonith-ng.h>

#ifdef HAVE_GNUTLS_GNUTLS_H
#  undef KEYFILE
#  include <gnutls/gnutls.h>
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX_TLS_RECV_WAIT 10000

CRM_TRACE_INIT_DATA(lrmd);

static int lrmd_api_disconnect(lrmd_t * lrmd);
static int lrmd_api_is_connected(lrmd_t * lrmd);

/* IPC proxy functions */
int lrmd_internal_proxy_send(lrmd_t * lrmd, xmlNode *msg);
static void lrmd_internal_proxy_dispatch(lrmd_t *lrmd, xmlNode *msg);
void lrmd_internal_set_proxy_callback(lrmd_t * lrmd, void *userdata, void (*callback)(lrmd_t *lrmd, void *userdata, xmlNode *msg));

#ifdef HAVE_GNUTLS_GNUTLS_H
#  define LRMD_CLIENT_HANDSHAKE_TIMEOUT 5000    /* 5 seconds */
gnutls_psk_client_credentials_t psk_cred_s;
int lrmd_tls_set_key(gnutls_datum_t * key);
static void lrmd_tls_disconnect(lrmd_t * lrmd);
static int global_remote_msg_id = 0;
static void lrmd_tls_connection_destroy(gpointer userdata);
#endif

typedef struct lrmd_private_s {
    uint64_t type;
    char *token;
    mainloop_io_t *source;

    /* IPC parameters */
    crm_ipc_t *ipc;

    pcmk__remote_t *remote;

    /* Extra TLS parameters */
    char *remote_nodename;
#ifdef HAVE_GNUTLS_GNUTLS_H
    char *server;
    int port;
    gnutls_psk_client_credentials_t psk_cred_c;

    /* while the async connection is occurring, this is the id
     * of the connection timeout timer. */
    int async_timer;
    int sock;
    /* since tls requires a round trip across the network for a
     * request/reply, there are times where we just want to be able
     * to send a request from the client and not wait around (or even care
     * about) what the reply is. */
    int expected_late_replies;
    GList *pending_notify;
    crm_trigger_t *process_notify;
#endif

    lrmd_event_callback callback;

    /* Internal IPC proxy msg passing for remote guests */
    void (*proxy_callback)(lrmd_t *lrmd, void *userdata, xmlNode *msg);
    void *proxy_callback_userdata;
    char *peer_version;
} lrmd_private_t;

static lrmd_list_t *
lrmd_list_add(lrmd_list_t * head, const char *value)
{
    lrmd_list_t *p, *end;

    p = calloc(1, sizeof(lrmd_list_t));
    p->val = strdup(value);

    end = head;
    while (end && end->next) {
        end = end->next;
    }

    if (end) {
        end->next = p;
    } else {
        head = p;
    }

    return head;
}

void
lrmd_list_freeall(lrmd_list_t * head)
{
    lrmd_list_t *p;

    while (head) {
        char *val = (char *)head->val;

        p = head->next;
        free(val);
        free(head);
        head = p;
    }
}

lrmd_key_value_t *
lrmd_key_value_add(lrmd_key_value_t * head, const char *key, const char *value)
{
    lrmd_key_value_t *p, *end;

    p = calloc(1, sizeof(lrmd_key_value_t));
    p->key = strdup(key);
    p->value = strdup(value);

    end = head;
    while (end && end->next) {
        end = end->next;
    }

    if (end) {
        end->next = p;
    } else {
        head = p;
    }

    return head;
}

void
lrmd_key_value_freeall(lrmd_key_value_t * head)
{
    lrmd_key_value_t *p;

    while (head) {
        p = head->next;
        free(head->key);
        free(head->value);
        free(head);
        head = p;
    }
}

/*!
 * Create a new lrmd_event_data_t object
 *
 * \param[in] rsc_id       ID of resource involved in event
 * \param[in] task         Action name
 * \param[in] interval_ms  Action interval
 *
 * \return Newly allocated and initialized lrmd_event_data_t
 * \note This functions asserts on memory errors, so the return value is
 *       guaranteed to be non-NULL. The caller is responsible for freeing the
 *       result with lrmd_free_event().
 */
lrmd_event_data_t *
lrmd_new_event(const char *rsc_id, const char *task, guint interval_ms)
{
    lrmd_event_data_t *event = calloc(1, sizeof(lrmd_event_data_t));

    CRM_ASSERT(event != NULL);
    if (rsc_id != NULL) {
        event->rsc_id = strdup(rsc_id);
        CRM_ASSERT(event->rsc_id != NULL);
    }
    if (task != NULL) {
        event->op_type = strdup(task);
        CRM_ASSERT(event->op_type != NULL);
    }
    event->interval_ms = interval_ms;
    return event;
}

lrmd_event_data_t *
lrmd_copy_event(lrmd_event_data_t * event)
{
    lrmd_event_data_t *copy = NULL;

    copy = calloc(1, sizeof(lrmd_event_data_t));

    /* This will get all the int values.
     * we just have to be careful not to leave any
     * dangling pointers to strings. */
    memcpy(copy, event, sizeof(lrmd_event_data_t));

    copy->rsc_id = event->rsc_id ? strdup(event->rsc_id) : NULL;
    copy->op_type = event->op_type ? strdup(event->op_type) : NULL;
    copy->user_data = event->user_data ? strdup(event->user_data) : NULL;
    copy->output = event->output ? strdup(event->output) : NULL;
    copy->exit_reason = event->exit_reason ? strdup(event->exit_reason) : NULL;
    copy->remote_nodename = event->remote_nodename ? strdup(event->remote_nodename) : NULL;
    copy->params = pcmk__str_table_dup(event->params);

    return copy;
}

/*!
 * \brief Free an executor event
 *
 * \param[in]  Executor event object to free
 */
void
lrmd_free_event(lrmd_event_data_t *event)
{
    if (event == NULL) {
        return;
    }
    // @TODO Why are these const char *?
    free((void *) event->rsc_id);
    free((void *) event->op_type);
    free((void *) event->user_data);
    free((void *) event->output);
    free((void *) event->exit_reason);
    free((void *) event->remote_nodename);
    if (event->params != NULL) {
        g_hash_table_destroy(event->params);
    }
    free(event);
}

static void
lrmd_dispatch_internal(lrmd_t * lrmd, xmlNode * msg)
{
    const char *type;
    const char *proxy_session = crm_element_value(msg, F_LRMD_IPC_SESSION);
    lrmd_private_t *native = lrmd->lrmd_private;
    lrmd_event_data_t event = { 0, };

    if (proxy_session != NULL) {
        /* this is proxy business */
        lrmd_internal_proxy_dispatch(lrmd, msg);
        return;
    } else if (!native->callback) {
        /* no callback set */
        crm_trace("notify event received but client has not set callback");
        return;
    }

    event.remote_nodename = native->remote_nodename;
    type = crm_element_value(msg, F_LRMD_OPERATION);
    crm_element_value_int(msg, F_LRMD_CALLID, &event.call_id);
    event.rsc_id = crm_element_value(msg, F_LRMD_RSC_ID);

    if (pcmk__str_eq(type, LRMD_OP_RSC_REG, pcmk__str_none)) {
        event.type = lrmd_event_register;
    } else if (pcmk__str_eq(type, LRMD_OP_RSC_UNREG, pcmk__str_none)) {
        event.type = lrmd_event_unregister;
    } else if (pcmk__str_eq(type, LRMD_OP_RSC_EXEC, pcmk__str_none)) {
        time_t epoch = 0;

        crm_element_value_int(msg, F_LRMD_TIMEOUT, &event.timeout);
        crm_element_value_ms(msg, F_LRMD_RSC_INTERVAL, &event.interval_ms);
        crm_element_value_int(msg, F_LRMD_RSC_START_DELAY, &event.start_delay);
        crm_element_value_int(msg, F_LRMD_EXEC_RC, (int *)&event.rc);
        crm_element_value_int(msg, F_LRMD_OP_STATUS, &event.op_status);
        crm_element_value_int(msg, F_LRMD_RSC_DELETED, &event.rsc_deleted);

        crm_element_value_epoch(msg, F_LRMD_RSC_RUN_TIME, &epoch);
        event.t_run = (unsigned int) epoch;

        crm_element_value_epoch(msg, F_LRMD_RSC_RCCHANGE_TIME, &epoch);
        event.t_rcchange = (unsigned int) epoch;

        crm_element_value_int(msg, F_LRMD_RSC_EXEC_TIME, (int *)&event.exec_time);
        crm_element_value_int(msg, F_LRMD_RSC_QUEUE_TIME, (int *)&event.queue_time);

        event.op_type = crm_element_value(msg, F_LRMD_RSC_ACTION);
        event.user_data = crm_element_value(msg, F_LRMD_RSC_USERDATA_STR);
        event.output = crm_element_value(msg, F_LRMD_RSC_OUTPUT);
        event.exit_reason = crm_element_value(msg, F_LRMD_RSC_EXIT_REASON);
        event.type = lrmd_event_exec_complete;

        event.params = xml2list(msg);
    } else if (pcmk__str_eq(type, LRMD_OP_NEW_CLIENT, pcmk__str_none)) {
        event.type = lrmd_event_new_client;
    } else if (pcmk__str_eq(type, LRMD_OP_POKE, pcmk__str_none)) {
        event.type = lrmd_event_poke;
    } else {
        return;
    }

    crm_trace("op %s notify event received", type);
    native->callback(&event);

    if (event.params) {
        g_hash_table_destroy(event.params);
    }
}

// \return Always 0, to indicate that IPC mainloop source should be kept
static int
lrmd_ipc_dispatch(const char *buffer, ssize_t length, gpointer userdata)
{
    lrmd_t *lrmd = userdata;
    lrmd_private_t *native = lrmd->lrmd_private;

    if (native->callback != NULL) {
        xmlNode *msg = string2xml(buffer);

        lrmd_dispatch_internal(lrmd, msg);
        free_xml(msg);
    }
    return 0;
}

#ifdef HAVE_GNUTLS_GNUTLS_H
static void
lrmd_free_xml(gpointer userdata)
{
    free_xml((xmlNode *) userdata);
}

static bool
remote_executor_connected(lrmd_t * lrmd)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    return (native->remote->tls_session != NULL);
}

/*!
 * \internal
 * \brief TLS dispatch function (for both trigger and file descriptor sources)
 *
 * \param[in] userdata  API connection
 *
 * \return Always return a nonnegative value, which as a file descriptor
 *         dispatch function means keep the mainloop source, and as a
 *         trigger dispatch function, 0 means remove the trigger from the
 *         mainloop while 1 means keep it (and job completed)
 */
static int
lrmd_tls_dispatch(gpointer userdata)
{
    lrmd_t *lrmd = userdata;
    lrmd_private_t *native = lrmd->lrmd_private;
    xmlNode *xml = NULL;
    int rc = pcmk_rc_ok;

    if (!remote_executor_connected(lrmd)) {
        crm_trace("TLS dispatch triggered after disconnect");
        return 0;
    }

    crm_trace("TLS dispatch triggered");

    /* First check if there are any pending notifies to process that came
     * while we were waiting for replies earlier. */
    if (native->pending_notify) {
        GList *iter = NULL;

        crm_trace("Processing pending notifies");
        for (iter = native->pending_notify; iter; iter = iter->next) {
            lrmd_dispatch_internal(lrmd, iter->data);
        }
        g_list_free_full(native->pending_notify, lrmd_free_xml);
        native->pending_notify = NULL;
    }

    /* Next read the current buffer and see if there are any messages to handle. */
    switch (pcmk__remote_ready(native->remote, 0)) {
        case pcmk_rc_ok:
            rc = pcmk__read_remote_message(native->remote, -1);
            xml = pcmk__remote_message_xml(native->remote);
            break;
        case ETIME:
            // Nothing to read, check if a full message is already in buffer
            xml = pcmk__remote_message_xml(native->remote);
            break;
        default:
            rc = ENOTCONN;
            break;
    }
    while (xml) {
        const char *msg_type = crm_element_value(xml, F_LRMD_REMOTE_MSG_TYPE);
        if (pcmk__str_eq(msg_type, "notify", pcmk__str_casei)) {
            lrmd_dispatch_internal(lrmd, xml);
        } else if (pcmk__str_eq(msg_type, "reply", pcmk__str_casei)) {
            if (native->expected_late_replies > 0) {
                native->expected_late_replies--;
            } else {
                int reply_id = 0;
                crm_element_value_int(xml, F_LRMD_CALLID, &reply_id);
                /* if this happens, we want to know about it */
                crm_err("Got outdated Pacemaker Remote reply %d", reply_id);
            }
        }
        free_xml(xml);
        xml = pcmk__remote_message_xml(native->remote);
    }

    if (rc == ENOTCONN) {
        crm_info("Lost %s executor connection while reading data",
                 (native->remote_nodename? native->remote_nodename : "local"));
        lrmd_tls_disconnect(lrmd);
        return 0;
    }
    return 1;
}
#endif

/* Not used with mainloop */
int
lrmd_poll(lrmd_t * lrmd, int timeout)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    switch (native->type) {
        case pcmk__client_ipc:
            return crm_ipc_ready(native->ipc);

#ifdef HAVE_GNUTLS_GNUTLS_H
        case pcmk__client_tls:
            if (native->pending_notify) {
                return 1;
            } else {
                int rc = pcmk__remote_ready(native->remote, 0);

                switch (rc) {
                    case pcmk_rc_ok:
                        return 1;
                    case ETIME:
                        return 0;
                    default:
                        return pcmk_rc2legacy(rc);
                }
            }
#endif
        default:
            crm_err("Unsupported connection type: %d", native->type);
    }

    return 0;
}

/* Not used with mainloop */
bool
lrmd_dispatch(lrmd_t * lrmd)
{
    lrmd_private_t *private = NULL;

    CRM_ASSERT(lrmd != NULL);

    private = lrmd->lrmd_private;
    switch (private->type) {
        case pcmk__client_ipc:
            while (crm_ipc_ready(private->ipc)) {
                if (crm_ipc_read(private->ipc) > 0) {
                    const char *msg = crm_ipc_buffer(private->ipc);

                    lrmd_ipc_dispatch(msg, strlen(msg), lrmd);
                }
            }
            break;
#ifdef HAVE_GNUTLS_GNUTLS_H
        case pcmk__client_tls:
            lrmd_tls_dispatch(lrmd);
            break;
#endif
        default:
            crm_err("Unsupported connection type: %d", private->type);
    }

    if (lrmd_api_is_connected(lrmd) == FALSE) {
        crm_err("Connection closed");
        return FALSE;
    }

    return TRUE;
}

static xmlNode *
lrmd_create_op(const char *token, const char *op, xmlNode *data, int timeout,
               enum lrmd_call_options options)
{
    xmlNode *op_msg = create_xml_node(NULL, "lrmd_command");

    CRM_CHECK(op_msg != NULL, return NULL);
    CRM_CHECK(token != NULL, return NULL);

    crm_xml_add(op_msg, F_XML_TAGNAME, "lrmd_command");
    crm_xml_add(op_msg, F_TYPE, T_LRMD);
    crm_xml_add(op_msg, F_LRMD_CALLBACK_TOKEN, token);
    crm_xml_add(op_msg, F_LRMD_OPERATION, op);
    crm_xml_add_int(op_msg, F_LRMD_TIMEOUT, timeout);
    crm_xml_add_int(op_msg, F_LRMD_CALLOPTS, options);

    if (data != NULL) {
        add_message_xml(op_msg, F_LRMD_CALLDATA, data);
    }

    crm_trace("Created executor %s command with call options %.8lx (%d)",
              op, (long)options, options);
    return op_msg;
}

static void
lrmd_ipc_connection_destroy(gpointer userdata)
{
    lrmd_t *lrmd = userdata;
    lrmd_private_t *native = lrmd->lrmd_private;

    crm_info("IPC connection destroyed");

    /* Prevent these from being cleaned up in lrmd_api_disconnect() */
    native->ipc = NULL;
    native->source = NULL;

    if (native->callback) {
        lrmd_event_data_t event = { 0, };
        event.type = lrmd_event_disconnect;
        event.remote_nodename = native->remote_nodename;
        native->callback(&event);
    }
}

#ifdef HAVE_GNUTLS_GNUTLS_H
static void
lrmd_tls_connection_destroy(gpointer userdata)
{
    lrmd_t *lrmd = userdata;
    lrmd_private_t *native = lrmd->lrmd_private;

    crm_info("TLS connection destroyed");

    if (native->remote->tls_session) {
        gnutls_bye(*native->remote->tls_session, GNUTLS_SHUT_RDWR);
        gnutls_deinit(*native->remote->tls_session);
        gnutls_free(native->remote->tls_session);
    }
    if (native->psk_cred_c) {
        gnutls_psk_free_client_credentials(native->psk_cred_c);
    }
    if (native->sock) {
        close(native->sock);
    }
    if (native->process_notify) {
        mainloop_destroy_trigger(native->process_notify);
        native->process_notify = NULL;
    }
    if (native->pending_notify) {
        g_list_free_full(native->pending_notify, lrmd_free_xml);
        native->pending_notify = NULL;
    }

    free(native->remote->buffer);
    native->remote->buffer = NULL;
    native->source = 0;
    native->sock = 0;
    native->psk_cred_c = NULL;
    native->remote->tls_session = NULL;
    native->sock = 0;

    if (native->callback) {
        lrmd_event_data_t event = { 0, };
        event.remote_nodename = native->remote_nodename;
        event.type = lrmd_event_disconnect;
        native->callback(&event);
    }
    return;
}

// \return Standard Pacemaker return code
int
lrmd__remote_send_xml(pcmk__remote_t *session, xmlNode *msg, uint32_t id,
                      const char *msg_type)
{
    crm_xml_add_int(msg, F_LRMD_REMOTE_MSG_ID, id);
    crm_xml_add(msg, F_LRMD_REMOTE_MSG_TYPE, msg_type);
    return pcmk__remote_send_xml(session, msg);
}

static xmlNode *
lrmd_tls_recv_reply(lrmd_t * lrmd, int total_timeout, int expected_reply_id, int *disconnected)
{
    lrmd_private_t *native = lrmd->lrmd_private;
    xmlNode *xml = NULL;
    time_t start = time(NULL);
    const char *msg_type = NULL;
    int reply_id = 0;
    int remaining_timeout = 0;

    /* A timeout of 0 here makes no sense.  We have to wait a period of time
     * for the response to come back.  If -1 or 0, default to 10 seconds. */
    if (total_timeout <= 0 || total_timeout > MAX_TLS_RECV_WAIT) {
        total_timeout = MAX_TLS_RECV_WAIT;
    }

    while (!xml) {

        xml = pcmk__remote_message_xml(native->remote);
        if (!xml) {
            /* read some more off the tls buffer if we still have time left. */
            if (remaining_timeout) {
                remaining_timeout = total_timeout - ((time(NULL) - start) * 1000);
            } else {
                remaining_timeout = total_timeout;
            }
            if (remaining_timeout <= 0) {
                crm_err("Never received the expected reply during the timeout period, disconnecting.");
                *disconnected = TRUE;
                return NULL;
            }

            if (pcmk__read_remote_message(native->remote,
                                          remaining_timeout) == ENOTCONN) {
                *disconnected = TRUE;
            } else {
                *disconnected = FALSE;
            }
            xml = pcmk__remote_message_xml(native->remote);
            if (!xml) {
                crm_err("Unable to receive expected reply, disconnecting.");
                *disconnected = TRUE;
                return NULL;
            } else if (*disconnected) {
                return NULL;
            }
        }

        CRM_ASSERT(xml != NULL);

        crm_element_value_int(xml, F_LRMD_REMOTE_MSG_ID, &reply_id);
        msg_type = crm_element_value(xml, F_LRMD_REMOTE_MSG_TYPE);

        if (!msg_type) {
            crm_err("Empty msg type received while waiting for reply");
            free_xml(xml);
            xml = NULL;
        } else if (pcmk__str_eq(msg_type, "notify", pcmk__str_casei)) {
            /* got a notify while waiting for reply, trigger the notify to be processed later */
            crm_info("queueing notify");
            native->pending_notify = g_list_append(native->pending_notify, xml);
            if (native->process_notify) {
                crm_info("notify trigger set.");
                mainloop_set_trigger(native->process_notify);
            }
            xml = NULL;
        } else if (!pcmk__str_eq(msg_type, "reply", pcmk__str_casei)) {
            /* msg isn't a reply, make some noise */
            crm_err("Expected a reply, got %s", msg_type);
            free_xml(xml);
            xml = NULL;
        } else if (reply_id != expected_reply_id) {
            if (native->expected_late_replies > 0) {
                native->expected_late_replies--;
            } else {
                crm_err("Got outdated reply, expected id %d got id %d", expected_reply_id, reply_id);
            }
            free_xml(xml);
            xml = NULL;
        }
    }

    if (native->remote->buffer && native->process_notify) {
        mainloop_set_trigger(native->process_notify);
    }

    return xml;
}

static int
lrmd_tls_send(lrmd_t * lrmd, xmlNode * msg)
{
    int rc = 0;
    lrmd_private_t *native = lrmd->lrmd_private;

    global_remote_msg_id++;
    if (global_remote_msg_id <= 0) {
        global_remote_msg_id = 1;
    }

    rc = lrmd__remote_send_xml(native->remote, msg, global_remote_msg_id,
                               "request");
    if (rc != pcmk_rc_ok) {
        crm_err("Disconnecting because TLS message could not be sent to "
                "Pacemaker Remote: %s", pcmk_rc_str(rc));
        lrmd_tls_disconnect(lrmd);
        return -ENOTCONN;
    }
    return pcmk_ok;
}

static int
lrmd_tls_send_recv(lrmd_t * lrmd, xmlNode * msg, int timeout, xmlNode ** reply)
{
    int rc = 0;
    int disconnected = 0;
    xmlNode *xml = NULL;

    if (!remote_executor_connected(lrmd)) {
        return -1;
    }

    rc = lrmd_tls_send(lrmd, msg);
    if (rc < 0) {
        return rc;
    }

    xml = lrmd_tls_recv_reply(lrmd, timeout, global_remote_msg_id, &disconnected);

    if (disconnected) {
        crm_err("Pacemaker Remote disconnected while waiting for reply to request id %d",
                global_remote_msg_id);
        lrmd_tls_disconnect(lrmd);
        rc = -ENOTCONN;
    } else if (!xml) {
        crm_err("Did not receive reply from Pacemaker Remote for request id %d (timeout %dms)",
                global_remote_msg_id, timeout);
        rc = -ECOMM;
    }

    if (reply) {
        *reply = xml;
    } else {
        free_xml(xml);
    }

    return rc;
}
#endif

static int
lrmd_send_xml(lrmd_t * lrmd, xmlNode * msg, int timeout, xmlNode ** reply)
{
    int rc = -1;
    lrmd_private_t *native = lrmd->lrmd_private;

    switch (native->type) {
        case pcmk__client_ipc:
            rc = crm_ipc_send(native->ipc, msg, crm_ipc_client_response, timeout, reply);
            break;
#ifdef HAVE_GNUTLS_GNUTLS_H
        case pcmk__client_tls:
            rc = lrmd_tls_send_recv(lrmd, msg, timeout, reply);
            break;
#endif
        default:
            crm_err("Unsupported connection type: %d", native->type);
    }

    return rc;
}

static int
lrmd_send_xml_no_reply(lrmd_t * lrmd, xmlNode * msg)
{
    int rc = -1;
    lrmd_private_t *native = lrmd->lrmd_private;

    switch (native->type) {
        case pcmk__client_ipc:
            rc = crm_ipc_send(native->ipc, msg, crm_ipc_flags_none, 0, NULL);
            break;
#ifdef HAVE_GNUTLS_GNUTLS_H
        case pcmk__client_tls:
            rc = lrmd_tls_send(lrmd, msg);
            if (rc == pcmk_ok) {
                /* we don't want to wait around for the reply, but
                 * since the request/reply protocol needs to behave the same
                 * as libqb, a reply will eventually come later anyway. */
                native->expected_late_replies++;
            }
            break;
#endif
        default:
            crm_err("Unsupported connection type: %d", native->type);
    }

    return rc;
}

static int
lrmd_api_is_connected(lrmd_t * lrmd)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    switch (native->type) {
        case pcmk__client_ipc:
            return crm_ipc_connected(native->ipc);
#ifdef HAVE_GNUTLS_GNUTLS_H
        case pcmk__client_tls:
            return remote_executor_connected(lrmd);
#endif
        default:
            crm_err("Unsupported connection type: %d", native->type);
    }

    return 0;
}

/*!
 * \internal
 * \brief Send a prepared API command to the executor
 *
 * \param[in]  lrmd          Existing connection to the executor
 * \param[in]  op            Name of API command to send
 * \param[in]  data          Command data XML to add to the sent command
 * \param[out] output_data   If expecting a reply, it will be stored here
 * \param[in]  timeout       Timeout in milliseconds (if 0, defaults to
 *                           a sensible value per the type of connection,
 *                           standard vs. pacemaker remote);
 *                           also propagated to the command XML
 * \param[in]  call_options  Call options to pass to server when sending
 * \param[in]  expect_reply  If TRUE, wait for a reply from the server;
 *                           must be TRUE for IPC (as opposed to TLS) clients
 *
 * \return pcmk_ok on success, -errno on error
 */
static int
lrmd_send_command(lrmd_t *lrmd, const char *op, xmlNode *data,
                  xmlNode **output_data, int timeout,
                  enum lrmd_call_options options, gboolean expect_reply)
{
    int rc = pcmk_ok;
    lrmd_private_t *native = lrmd->lrmd_private;
    xmlNode *op_msg = NULL;
    xmlNode *op_reply = NULL;

    if (!lrmd_api_is_connected(lrmd)) {
        return -ENOTCONN;
    }

    if (op == NULL) {
        crm_err("No operation specified");
        return -EINVAL;
    }

    CRM_CHECK(native->token != NULL,;
        );
    crm_trace("Sending %s op to executor", op);

    op_msg = lrmd_create_op(native->token, op, data, timeout, options);

    if (op_msg == NULL) {
        return -EINVAL;
    }

    if (expect_reply) {
        rc = lrmd_send_xml(lrmd, op_msg, timeout, &op_reply);
    } else {
        rc = lrmd_send_xml_no_reply(lrmd, op_msg);
        goto done;
    }

    if (rc < 0) {
        crm_perror(LOG_ERR, "Couldn't perform %s operation (timeout=%d): %d", op, timeout, rc);
        rc = -ECOMM;
        goto done;

    } else if(op_reply == NULL) {
        rc = -ENOMSG;
        goto done;
    }

    rc = pcmk_ok;
    crm_trace("%s op reply received", op);
    if (crm_element_value_int(op_reply, F_LRMD_RC, &rc) != 0) {
        rc = -ENOMSG;
        goto done;
    }

    crm_log_xml_trace(op_reply, "Reply");

    if (output_data) {
        *output_data = op_reply;
        op_reply = NULL;        /* Prevent subsequent free */
    }

  done:
    if (lrmd_api_is_connected(lrmd) == FALSE) {
        crm_err("Executor disconnected");
    }

    free_xml(op_msg);
    free_xml(op_reply);
    return rc;
}

static int
lrmd_api_poke_connection(lrmd_t * lrmd)
{
    int rc;
    lrmd_private_t *native = lrmd->lrmd_private;
    xmlNode *data = create_xml_node(NULL, F_LRMD_RSC);

    crm_xml_add(data, F_LRMD_ORIGIN, __func__);
    rc = lrmd_send_command(lrmd, LRMD_OP_POKE, data, NULL, 0, 0,
                           (native->type == pcmk__client_ipc));
    free_xml(data);

    return rc < 0 ? rc : pcmk_ok;
}

int
remote_proxy_check(lrmd_t * lrmd, GHashTable *hash)
{
    int rc;
    const char *value;
    lrmd_private_t *native = lrmd->lrmd_private;
    xmlNode *data = create_xml_node(NULL, F_LRMD_OPERATION);

    crm_xml_add(data, F_LRMD_ORIGIN, __func__);

    value = g_hash_table_lookup(hash, "stonith-watchdog-timeout");
    crm_xml_add(data, F_LRMD_WATCHDOG, value);

    rc = lrmd_send_command(lrmd, LRMD_OP_CHECK, data, NULL, 0, 0,
                           (native->type == pcmk__client_ipc));
    free_xml(data);

    return rc < 0 ? rc : pcmk_ok;
}

static int
lrmd_handshake(lrmd_t * lrmd, const char *name)
{
    int rc = pcmk_ok;
    lrmd_private_t *native = lrmd->lrmd_private;
    xmlNode *reply = NULL;
    xmlNode *hello = create_xml_node(NULL, "lrmd_command");

    crm_xml_add(hello, F_TYPE, T_LRMD);
    crm_xml_add(hello, F_LRMD_OPERATION, CRM_OP_REGISTER);
    crm_xml_add(hello, F_LRMD_CLIENTNAME, name);
    crm_xml_add(hello, F_LRMD_PROTOCOL_VERSION, LRMD_PROTOCOL_VERSION);

    /* advertise that we are a proxy provider */
    if (native->proxy_callback) {
        crm_xml_add(hello, F_LRMD_IS_IPC_PROVIDER, "true");
    }

    rc = lrmd_send_xml(lrmd, hello, -1, &reply);

    if (rc < 0) {
        crm_perror(LOG_DEBUG, "Couldn't complete registration with the executor API: %d", rc);
        rc = -ECOMM;
    } else if (reply == NULL) {
        crm_err("Did not receive registration reply");
        rc = -EPROTO;
    } else {
        const char *version = crm_element_value(reply, F_LRMD_PROTOCOL_VERSION);
        const char *msg_type = crm_element_value(reply, F_LRMD_OPERATION);
        const char *tmp_ticket = crm_element_value(reply, F_LRMD_CLIENTID);

        crm_element_value_int(reply, F_LRMD_RC, &rc);

        if (rc == -EPROTO) {
            crm_err("Executor protocol version mismatch between client (%s) and server (%s)",
                LRMD_PROTOCOL_VERSION, version);
            crm_log_xml_err(reply, "Protocol Error");

        } else if (!pcmk__str_eq(msg_type, CRM_OP_REGISTER, pcmk__str_casei)) {
            crm_err("Invalid registration message: %s", msg_type);
            crm_log_xml_err(reply, "Bad reply");
            rc = -EPROTO;
        } else if (tmp_ticket == NULL) {
            crm_err("No registration token provided");
            crm_log_xml_err(reply, "Bad reply");
            rc = -EPROTO;
        } else {
            crm_trace("Obtained registration token: %s", tmp_ticket);
            native->token = strdup(tmp_ticket);
            native->peer_version = strdup(version?version:"1.0"); /* Included since 1.1 */
            rc = pcmk_ok;
        }
    }

    free_xml(reply);
    free_xml(hello);

    if (rc != pcmk_ok) {
        lrmd_api_disconnect(lrmd);
    }
    return rc;
}

static int
lrmd_ipc_connect(lrmd_t * lrmd, int *fd)
{
    int rc = pcmk_ok;
    lrmd_private_t *native = lrmd->lrmd_private;

    struct ipc_client_callbacks lrmd_callbacks = {
        .dispatch = lrmd_ipc_dispatch,
        .destroy = lrmd_ipc_connection_destroy
    };

    crm_info("Connecting to executor");

    if (fd) {
        /* No mainloop */
        native->ipc = crm_ipc_new(CRM_SYSTEM_LRMD, 0);
        if (native->ipc && crm_ipc_connect(native->ipc)) {
            *fd = crm_ipc_get_fd(native->ipc);
        } else if (native->ipc) {
            crm_perror(LOG_ERR, "Connection to executor failed");
            rc = -ENOTCONN;
        }
    } else {
        native->source = mainloop_add_ipc_client(CRM_SYSTEM_LRMD, G_PRIORITY_HIGH, 0, lrmd, &lrmd_callbacks);
        native->ipc = mainloop_get_ipc_client(native->source);
    }

    if (native->ipc == NULL) {
        crm_debug("Could not connect to the executor API");
        rc = -ENOTCONN;
    }

    return rc;
}

#ifdef HAVE_GNUTLS_GNUTLS_H
static void
copy_gnutls_datum(gnutls_datum_t *dest, gnutls_datum_t *source)
{
    dest->data = gnutls_malloc(source->size);
    CRM_ASSERT(dest->data);
    memcpy(dest->data, source->data, source->size);
    dest->size = source->size;
}

static void
clear_gnutls_datum(gnutls_datum_t *datum)
{
    gnutls_free(datum->data);
    datum->data = NULL;
    datum->size = 0;
}

#define KEY_READ_LEN 256

static int
set_key(gnutls_datum_t * key, const char *location)
{
    FILE *stream;
    size_t buf_len = KEY_READ_LEN;
    static gnutls_datum_t key_cache = { 0, };
    static time_t key_cache_updated = 0;

    if (location == NULL) {
        return -1;
    }

    if (key_cache.data != NULL) {
        if ((time(NULL) - key_cache_updated) < 60) {
            copy_gnutls_datum(key, &key_cache);
            crm_debug("Using cached Pacemaker Remote key");
            return 0;
        } else {
            clear_gnutls_datum(&key_cache);
            key_cache_updated = 0;
            crm_debug("Cleared Pacemaker Remote key cache");
        }
    }

    stream = fopen(location, "r");
    if (!stream) {
        return -1;
    }

    key->data = gnutls_malloc(buf_len);
    key->size = 0;
    while (!feof(stream)) {
        int next = fgetc(stream);

        if (next == EOF) {
            if (!feof(stream)) {
                crm_err("Error reading Pacemaker Remote key; copy in memory may be corrupted");
            }
            break;
        }
        if (key->size == buf_len) {
            buf_len = key->size + KEY_READ_LEN;
            key->data = gnutls_realloc(key->data, buf_len);
            CRM_ASSERT(key->data);
        }
        key->data[key->size++] = (unsigned char) next;
    }
    fclose(stream);

    if (key->size == 0) {
        clear_gnutls_datum(key);
        return -1;
    }

    if (key_cache.data == NULL) {
        copy_gnutls_datum(&key_cache, key);
        key_cache_updated = time(NULL);
        crm_debug("Cached Pacemaker Remote key");
    }

    return 0;
}

int
lrmd_tls_set_key(gnutls_datum_t * key)
{
    const char *specific_location = getenv("PCMK_authkey_location");

    if (set_key(key, specific_location) == 0) {
        crm_debug("Using custom authkey location %s", specific_location);
        return pcmk_ok;

    } else if (specific_location) {
        crm_err("No valid Pacemaker Remote key found at %s, trying default location", specific_location);
    }

    if ((set_key(key, DEFAULT_REMOTE_KEY_LOCATION) != 0)
        && (set_key(key, ALT_REMOTE_KEY_LOCATION) != 0)) {
        crm_err("No valid Pacemaker Remote key found at %s", DEFAULT_REMOTE_KEY_LOCATION);
        return -ENOKEY;
    }

    return pcmk_ok;
}

static void
lrmd_gnutls_global_init(void)
{
    static int gnutls_init = 0;

    if (!gnutls_init) {
        crm_gnutls_global_init();
    }
    gnutls_init = 1;
}
#endif

static void
report_async_connection_result(lrmd_t * lrmd, int rc)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    if (native->callback) {
        lrmd_event_data_t event = { 0, };
        event.type = lrmd_event_connect;
        event.remote_nodename = native->remote_nodename;
        event.connection_rc = rc;
        native->callback(&event);
    }
}

#ifdef HAVE_GNUTLS_GNUTLS_H
static inline int
lrmd__tls_client_handshake(pcmk__remote_t *remote)
{
    return pcmk__tls_client_handshake(remote, LRMD_CLIENT_HANDSHAKE_TIMEOUT);
}

/*!
 * \internal
 * \brief Add trigger and file descriptor mainloop sources for TLS
 *
 * \param[in] lrmd          API connection with established TLS session
 * \param[in] do_handshake  Whether to perform executor handshake
 *
 * \return Standard Pacemaker return code
 */
static int
add_tls_to_mainloop(lrmd_t *lrmd, bool do_handshake)
{
    lrmd_private_t *native = lrmd->lrmd_private;
    int rc = pcmk_rc_ok;

    char *name = crm_strdup_printf("pacemaker-remote-%s:%d",
                                   native->server, native->port);

    struct mainloop_fd_callbacks tls_fd_callbacks = {
        .dispatch = lrmd_tls_dispatch,
        .destroy = lrmd_tls_connection_destroy,
    };

    native->process_notify = mainloop_add_trigger(G_PRIORITY_HIGH,
                                                  lrmd_tls_dispatch, lrmd);
    native->source = mainloop_add_fd(name, G_PRIORITY_HIGH, native->sock, lrmd,
                                     &tls_fd_callbacks);

    /* Async connections lose the client name provided by the API caller, so we
     * have to use our generated name here to perform the executor handshake.
     *
     * @TODO Keep track of the caller-provided name. Perhaps we should be using
     * that name in this function instead of generating one anyway.
     */
    if (do_handshake) {
        rc = lrmd_handshake(lrmd, name);
        rc = pcmk_legacy2rc(rc);
    }
    free(name);
    return rc;
}

static void
lrmd_tcp_connect_cb(void *userdata, int rc, int sock)
{
    lrmd_t *lrmd = userdata;
    lrmd_private_t *native = lrmd->lrmd_private;
    gnutls_datum_t psk_key = { NULL, 0 };

    native->async_timer = 0;

    if (rc != pcmk_rc_ok) {
        lrmd_tls_connection_destroy(lrmd);
        crm_info("Could not connect to Pacemaker Remote at %s:%d: %s "
                 CRM_XS " rc=%d",
                 native->server, native->port, pcmk_rc_str(rc), rc);
        report_async_connection_result(lrmd, pcmk_rc2legacy(rc));
        return;
    }

    /* The TCP connection was successful, so establish the TLS connection.
     * @TODO make this async to avoid blocking code in client
     */

    native->sock = sock;

    rc = lrmd_tls_set_key(&psk_key);
    if (rc != 0) {
        crm_warn("Could not set key for Pacemaker Remote at %s:%d " CRM_XS " rc=%d",
                 native->server, native->port, rc);
        lrmd_tls_connection_destroy(lrmd);
        report_async_connection_result(lrmd, rc);
        return;
    }

    gnutls_psk_allocate_client_credentials(&native->psk_cred_c);
    gnutls_psk_set_client_credentials(native->psk_cred_c, DEFAULT_REMOTE_USERNAME, &psk_key, GNUTLS_PSK_KEY_RAW);
    gnutls_free(psk_key.data);

    native->remote->tls_session = pcmk__new_tls_session(sock, GNUTLS_CLIENT,
                                                        GNUTLS_CRD_PSK,
                                                        native->psk_cred_c);
    if (native->remote->tls_session == NULL) {
        lrmd_tls_connection_destroy(lrmd);
        report_async_connection_result(lrmd, -EPROTO);
        return;
    }

    if (lrmd__tls_client_handshake(native->remote) != pcmk_rc_ok) {
        crm_warn("Disconnecting after TLS handshake with Pacemaker Remote server %s:%d failed",
                 native->server, native->port);
        gnutls_deinit(*native->remote->tls_session);
        gnutls_free(native->remote->tls_session);
        native->remote->tls_session = NULL;
        lrmd_tls_connection_destroy(lrmd);
        report_async_connection_result(lrmd, -EKEYREJECTED);
        return;
    }

    crm_info("TLS connection to Pacemaker Remote server %s:%d succeeded",
             native->server, native->port);
    rc = add_tls_to_mainloop(lrmd, true);
    report_async_connection_result(lrmd, pcmk_rc2legacy(rc));
}

static int
lrmd_tls_connect_async(lrmd_t * lrmd, int timeout /*ms */ )
{
    int rc;
    int timer_id = 0;
    lrmd_private_t *native = lrmd->lrmd_private;

    lrmd_gnutls_global_init();
    native->sock = -1;
    rc = pcmk__connect_remote(native->server, native->port, timeout, &timer_id,
                              &(native->sock), lrmd, lrmd_tcp_connect_cb);
    if (rc != pcmk_rc_ok) {
        crm_warn("Pacemaker Remote connection to %s:%s failed: %s "
                 CRM_XS " rc=%d",
                 native->server, native->port, pcmk_rc_str(rc), rc);
        return -1;
    }
    native->async_timer = timer_id;
    return pcmk_ok;
}

static int
lrmd_tls_connect(lrmd_t * lrmd, int *fd)
{
    int rc;

    lrmd_private_t *native = lrmd->lrmd_private;
    gnutls_datum_t psk_key = { NULL, 0 };

    lrmd_gnutls_global_init();

    native->sock = -1;
    rc = pcmk__connect_remote(native->server, native->port, 0, NULL,
                              &(native->sock), NULL, NULL);
    if (rc != pcmk_rc_ok) {
        crm_warn("Pacemaker Remote connection to %s:%s failed: %s "
                 CRM_XS " rc=%d",
                 native->server, native->port, pcmk_rc_str(rc), rc);
        lrmd_tls_connection_destroy(lrmd);
        return -ENOTCONN;
    }

    rc = lrmd_tls_set_key(&psk_key);
    if (rc < 0) {
        lrmd_tls_connection_destroy(lrmd);
        return rc;
    }

    gnutls_psk_allocate_client_credentials(&native->psk_cred_c);
    gnutls_psk_set_client_credentials(native->psk_cred_c, DEFAULT_REMOTE_USERNAME, &psk_key, GNUTLS_PSK_KEY_RAW);
    gnutls_free(psk_key.data);

    native->remote->tls_session = pcmk__new_tls_session(native->sock, GNUTLS_CLIENT,
                                                        GNUTLS_CRD_PSK,
                                                        native->psk_cred_c);
    if (native->remote->tls_session == NULL) {
        lrmd_tls_connection_destroy(lrmd);
        return -EPROTO;
    }

    if (lrmd__tls_client_handshake(native->remote) != pcmk_rc_ok) {
        crm_err("Session creation for %s:%d failed", native->server, native->port);
        gnutls_deinit(*native->remote->tls_session);
        gnutls_free(native->remote->tls_session);
        native->remote->tls_session = NULL;
        lrmd_tls_connection_destroy(lrmd);
        return -EKEYREJECTED;
    }

    crm_info("Client TLS connection established with Pacemaker Remote server %s:%d", native->server,
             native->port);

    if (fd) {
        *fd = native->sock;
    } else {
        add_tls_to_mainloop(lrmd, false);
    }
    return pcmk_ok;
}
#endif

static int
lrmd_api_connect(lrmd_t * lrmd, const char *name, int *fd)
{
    int rc = -ENOTCONN;
    lrmd_private_t *native = lrmd->lrmd_private;

    switch (native->type) {
        case pcmk__client_ipc:
            rc = lrmd_ipc_connect(lrmd, fd);
            break;
#ifdef HAVE_GNUTLS_GNUTLS_H
        case pcmk__client_tls:
            rc = lrmd_tls_connect(lrmd, fd);
            break;
#endif
        default:
            crm_err("Unsupported connection type: %d", native->type);
    }

    if (rc == pcmk_ok) {
        rc = lrmd_handshake(lrmd, name);
    }

    return rc;
}

static int
lrmd_api_connect_async(lrmd_t * lrmd, const char *name, int timeout)
{
    int rc = 0;
    lrmd_private_t *native = lrmd->lrmd_private;

    CRM_CHECK(native && native->callback, return -1);

    switch (native->type) {
        case pcmk__client_ipc:
            /* fake async connection with ipc.  it should be fast
             * enough that we gain very little from async */
            rc = lrmd_api_connect(lrmd, name, NULL);
            if (!rc) {
                report_async_connection_result(lrmd, rc);
            }
            break;
#ifdef HAVE_GNUTLS_GNUTLS_H
        case pcmk__client_tls:
            rc = lrmd_tls_connect_async(lrmd, timeout);
            if (rc) {
                /* connection failed, report rc now */
                report_async_connection_result(lrmd, rc);
            }
            break;
#endif
        default:
            crm_err("Unsupported connection type: %d", native->type);
    }

    return rc;
}

static void
lrmd_ipc_disconnect(lrmd_t * lrmd)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    if (native->source != NULL) {
        /* Attached to mainloop */
        mainloop_del_ipc_client(native->source);
        native->source = NULL;
        native->ipc = NULL;

    } else if (native->ipc) {
        /* Not attached to mainloop */
        crm_ipc_t *ipc = native->ipc;

        native->ipc = NULL;
        crm_ipc_close(ipc);
        crm_ipc_destroy(ipc);
    }
}

#ifdef HAVE_GNUTLS_GNUTLS_H
static void
lrmd_tls_disconnect(lrmd_t * lrmd)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    if (native->remote->tls_session) {
        gnutls_bye(*native->remote->tls_session, GNUTLS_SHUT_RDWR);
        gnutls_deinit(*native->remote->tls_session);
        gnutls_free(native->remote->tls_session);
        native->remote->tls_session = 0;
    }

    if (native->async_timer) {
        g_source_remove(native->async_timer);
        native->async_timer = 0;
    }

    if (native->source != NULL) {
        /* Attached to mainloop */
        mainloop_del_ipc_client(native->source);
        native->source = NULL;

    } else if (native->sock) {
        close(native->sock);
        native->sock = 0;
    }

    if (native->pending_notify) {
        g_list_free_full(native->pending_notify, lrmd_free_xml);
        native->pending_notify = NULL;
    }
}
#endif

static int
lrmd_api_disconnect(lrmd_t * lrmd)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    crm_info("Disconnecting %s %s executor connection",
             pcmk__client_type_str(native->type),
             (native->remote_nodename? native->remote_nodename : "local"));
    switch (native->type) {
        case pcmk__client_ipc:
            lrmd_ipc_disconnect(lrmd);
            break;
#ifdef HAVE_GNUTLS_GNUTLS_H
        case pcmk__client_tls:
            lrmd_tls_disconnect(lrmd);
            break;
#endif
        default:
            crm_err("Unsupported connection type: %d", native->type);
    }

    free(native->token);
    native->token = NULL;

    free(native->peer_version);
    native->peer_version = NULL;
    return 0;
}

static int
lrmd_api_register_rsc(lrmd_t * lrmd,
                      const char *rsc_id,
                      const char *class,
                      const char *provider, const char *type, enum lrmd_call_options options)
{
    int rc = pcmk_ok;
    xmlNode *data = NULL;

    if (!class || !type || !rsc_id) {
        return -EINVAL;
    }
    if (pcmk_is_set(pcmk_get_ra_caps(class), pcmk_ra_cap_provider)
        && (provider == NULL)) {
        return -EINVAL;
    }

    data = create_xml_node(NULL, F_LRMD_RSC);

    crm_xml_add(data, F_LRMD_ORIGIN, __func__);
    crm_xml_add(data, F_LRMD_RSC_ID, rsc_id);
    crm_xml_add(data, F_LRMD_CLASS, class);
    crm_xml_add(data, F_LRMD_PROVIDER, provider);
    crm_xml_add(data, F_LRMD_TYPE, type);
    rc = lrmd_send_command(lrmd, LRMD_OP_RSC_REG, data, NULL, 0, options, TRUE);
    free_xml(data);

    return rc;
}

static int
lrmd_api_unregister_rsc(lrmd_t * lrmd, const char *rsc_id, enum lrmd_call_options options)
{
    int rc = pcmk_ok;
    xmlNode *data = create_xml_node(NULL, F_LRMD_RSC);

    crm_xml_add(data, F_LRMD_ORIGIN, __func__);
    crm_xml_add(data, F_LRMD_RSC_ID, rsc_id);
    rc = lrmd_send_command(lrmd, LRMD_OP_RSC_UNREG, data, NULL, 0, options, TRUE);
    free_xml(data);

    return rc;
}

lrmd_rsc_info_t *
lrmd_new_rsc_info(const char *rsc_id, const char *standard,
                  const char *provider, const char *type)
{
    lrmd_rsc_info_t *rsc_info = calloc(1, sizeof(lrmd_rsc_info_t));

    CRM_ASSERT(rsc_info);
    if (rsc_id) {
        rsc_info->id = strdup(rsc_id);
        CRM_ASSERT(rsc_info->id);
    }
    if (standard) {
        rsc_info->standard = strdup(standard);
        CRM_ASSERT(rsc_info->standard);
    }
    if (provider) {
        rsc_info->provider = strdup(provider);
        CRM_ASSERT(rsc_info->provider);
    }
    if (type) {
        rsc_info->type = strdup(type);
        CRM_ASSERT(rsc_info->type);
    }
    return rsc_info;
}

lrmd_rsc_info_t *
lrmd_copy_rsc_info(lrmd_rsc_info_t * rsc_info)
{
    return lrmd_new_rsc_info(rsc_info->id, rsc_info->standard,
                             rsc_info->provider, rsc_info->type);
}

void
lrmd_free_rsc_info(lrmd_rsc_info_t * rsc_info)
{
    if (!rsc_info) {
        return;
    }
    free(rsc_info->id);
    free(rsc_info->type);
    free(rsc_info->standard);
    free(rsc_info->provider);
    free(rsc_info);
}

static lrmd_rsc_info_t *
lrmd_api_get_rsc_info(lrmd_t * lrmd, const char *rsc_id, enum lrmd_call_options options)
{
    lrmd_rsc_info_t *rsc_info = NULL;
    xmlNode *data = create_xml_node(NULL, F_LRMD_RSC);
    xmlNode *output = NULL;
    const char *class = NULL;
    const char *provider = NULL;
    const char *type = NULL;

    crm_xml_add(data, F_LRMD_ORIGIN, __func__);
    crm_xml_add(data, F_LRMD_RSC_ID, rsc_id);
    lrmd_send_command(lrmd, LRMD_OP_RSC_INFO, data, &output, 0, options, TRUE);
    free_xml(data);

    if (!output) {
        return NULL;
    }

    class = crm_element_value(output, F_LRMD_CLASS);
    provider = crm_element_value(output, F_LRMD_PROVIDER);
    type = crm_element_value(output, F_LRMD_TYPE);

    if (!class || !type) {
        free_xml(output);
        return NULL;
    } else if (pcmk_is_set(pcmk_get_ra_caps(class), pcmk_ra_cap_provider)
               && !provider) {
        free_xml(output);
        return NULL;
    }

    rsc_info = lrmd_new_rsc_info(rsc_id, class, provider, type);
    free_xml(output);
    return rsc_info;
}

void
lrmd_free_op_info(lrmd_op_info_t *op_info)
{
    if (op_info) {
        free(op_info->rsc_id);
        free(op_info->action);
        free(op_info->interval_ms_s);
        free(op_info->timeout_ms_s);
        free(op_info);
    }
}

static int
lrmd_api_get_recurring_ops(lrmd_t *lrmd, const char *rsc_id, int timeout_ms,
                           enum lrmd_call_options options, GList **output)
{
    xmlNode *data = NULL;
    xmlNode *output_xml = NULL;
    int rc = pcmk_ok;

    if (output == NULL) {
        return -EINVAL;
    }
    *output = NULL;

    // Send request
    if (rsc_id) {
        data = create_xml_node(NULL, F_LRMD_RSC);
        crm_xml_add(data, F_LRMD_ORIGIN, __func__);
        crm_xml_add(data, F_LRMD_RSC_ID, rsc_id);
    }
    rc = lrmd_send_command(lrmd, LRMD_OP_GET_RECURRING, data, &output_xml,
                           timeout_ms, options, TRUE);
    if (data) {
        free_xml(data);
    }

    // Process reply
    if ((rc != pcmk_ok) || (output_xml == NULL)) {
        return rc;
    }
    for (xmlNode *rsc_xml = first_named_child(output_xml, F_LRMD_RSC);
         (rsc_xml != NULL) && (rc == pcmk_ok);
         rsc_xml = crm_next_same_xml(rsc_xml)) {

        rsc_id = crm_element_value(rsc_xml, F_LRMD_RSC_ID);
        if (rsc_id == NULL) {
            crm_err("Could not parse recurring operation information from executor");
            continue;
        }
        for (xmlNode *op_xml = first_named_child(rsc_xml, T_LRMD_RSC_OP);
             op_xml != NULL; op_xml = crm_next_same_xml(op_xml)) {

            lrmd_op_info_t *op_info = calloc(1, sizeof(lrmd_op_info_t));

            if (op_info == NULL) {
                rc = -ENOMEM;
                break;
            }
            op_info->rsc_id = strdup(rsc_id);
            op_info->action = crm_element_value_copy(op_xml, F_LRMD_RSC_ACTION);
            op_info->interval_ms_s = crm_element_value_copy(op_xml,
                                                            F_LRMD_RSC_INTERVAL);
            op_info->timeout_ms_s = crm_element_value_copy(op_xml,
                                                           F_LRMD_TIMEOUT);
            *output = g_list_prepend(*output, op_info);
        }
    }
    free_xml(output_xml);
    return rc;
}


static void
lrmd_api_set_callback(lrmd_t * lrmd, lrmd_event_callback callback)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    native->callback = callback;
}

void
lrmd_internal_set_proxy_callback(lrmd_t * lrmd, void *userdata, void (*callback)(lrmd_t *lrmd, void *userdata, xmlNode *msg))
{
    lrmd_private_t *native = lrmd->lrmd_private;

    native->proxy_callback = callback;
    native->proxy_callback_userdata = userdata;
}

void
lrmd_internal_proxy_dispatch(lrmd_t *lrmd, xmlNode *msg)
{
    lrmd_private_t *native = lrmd->lrmd_private;

    if (native->proxy_callback) {
        crm_log_xml_trace(msg, "PROXY_INBOUND");
        native->proxy_callback(lrmd, native->proxy_callback_userdata, msg);
    }
}

int
lrmd_internal_proxy_send(lrmd_t * lrmd, xmlNode *msg)
{
    if (lrmd == NULL) {
        return -ENOTCONN;
    }
    crm_xml_add(msg, F_LRMD_OPERATION, CRM_OP_IPC_FWD);

    crm_log_xml_trace(msg, "PROXY_OUTBOUND");
    return lrmd_send_xml_no_reply(lrmd, msg);
}

static int
stonith_get_metadata(const char *provider, const char *type, char **output)
{
    int rc = pcmk_ok;
    stonith_t *stonith_api = stonith_api_new();

    if (stonith_api == NULL) {
        crm_err("Could not get fence agent meta-data: API memory allocation failed");
        return -ENOMEM;
    }

    rc = stonith_api->cmds->metadata(stonith_api, st_opt_sync_call, type,
                                     provider, output, 0);
    if ((rc == pcmk_ok) && (*output == NULL)) {
        rc = -EIO;
    }
    stonith_api->cmds->free(stonith_api);
    return rc;
}

static int
lrmd_api_get_metadata(lrmd_t *lrmd, const char *standard, const char *provider,
                      const char *type, char **output,
                      enum lrmd_call_options options)
{
    return lrmd->cmds->get_metadata_params(lrmd, standard, provider, type,
                                           output, options, NULL);
}

static int
lrmd_api_get_metadata_params(lrmd_t *lrmd, const char *standard,
                             const char *provider, const char *type,
                             char **output, enum lrmd_call_options options,
                             lrmd_key_value_t *params)
{
    svc_action_t *action = NULL;
    GHashTable *params_table = NULL;

    if (!standard || !type) {
        lrmd_key_value_freeall(params);
        return -EINVAL;
    }

    if (pcmk__str_eq(standard, PCMK_RESOURCE_CLASS_STONITH, pcmk__str_casei)) {
        lrmd_key_value_freeall(params);
        return stonith_get_metadata(provider, type, output);
    }

    params_table = pcmk__strkey_table(free, free);
    for (const lrmd_key_value_t *param = params; param; param = param->next) {
        g_hash_table_insert(params_table, strdup(param->key), strdup(param->value));
    }
    action = resources_action_create(type, standard, provider, type,
                                     CRMD_ACTION_METADATA, 0,
                                     CRMD_METADATA_CALL_TIMEOUT, params_table,
                                     0);
    lrmd_key_value_freeall(params);

    if (action == NULL) {
        crm_err("Unable to retrieve meta-data for %s:%s:%s",
                standard, provider, type);
        return -EINVAL;
    }

    if (!services_action_sync(action)) {
        crm_err("Failed to retrieve meta-data for %s:%s:%s",
                standard, provider, type);
        services_action_free(action);
        return -EIO;
    }

    if (!action->stdout_data) {
        crm_err("Failed to receive meta-data for %s:%s:%s",
                standard, provider, type);
        services_action_free(action);
        return -EIO;
    }

    *output = strdup(action->stdout_data);
    services_action_free(action);

    return pcmk_ok;
}

static int
lrmd_api_exec(lrmd_t *lrmd, const char *rsc_id, const char *action,
              const char *userdata, guint interval_ms,
              int timeout,      /* ms */
              int start_delay,  /* ms */
              enum lrmd_call_options options, lrmd_key_value_t * params)
{
    int rc = pcmk_ok;
    xmlNode *data = create_xml_node(NULL, F_LRMD_RSC);
    xmlNode *args = create_xml_node(data, XML_TAG_ATTRS);
    lrmd_key_value_t *tmp = NULL;

    crm_xml_add(data, F_LRMD_ORIGIN, __func__);
    crm_xml_add(data, F_LRMD_RSC_ID, rsc_id);
    crm_xml_add(data, F_LRMD_RSC_ACTION, action);
    crm_xml_add(data, F_LRMD_RSC_USERDATA_STR, userdata);
    crm_xml_add_ms(data, F_LRMD_RSC_INTERVAL, interval_ms);
    crm_xml_add_int(data, F_LRMD_TIMEOUT, timeout);
    crm_xml_add_int(data, F_LRMD_RSC_START_DELAY, start_delay);

    for (tmp = params; tmp; tmp = tmp->next) {
        hash2smartfield((gpointer) tmp->key, (gpointer) tmp->value, args);
    }

    rc = lrmd_send_command(lrmd, LRMD_OP_RSC_EXEC, data, NULL, timeout, options, TRUE);
    free_xml(data);

    lrmd_key_value_freeall(params);
    return rc;
}

/* timeout is in ms */
static int
lrmd_api_exec_alert(lrmd_t *lrmd, const char *alert_id, const char *alert_path,
                    int timeout, lrmd_key_value_t *params)
{
    int rc = pcmk_ok;
    xmlNode *data = create_xml_node(NULL, F_LRMD_ALERT);
    xmlNode *args = create_xml_node(data, XML_TAG_ATTRS);
    lrmd_key_value_t *tmp = NULL;

    crm_xml_add(data, F_LRMD_ORIGIN, __func__);
    crm_xml_add(data, F_LRMD_ALERT_ID, alert_id);
    crm_xml_add(data, F_LRMD_ALERT_PATH, alert_path);
    crm_xml_add_int(data, F_LRMD_TIMEOUT, timeout);

    for (tmp = params; tmp; tmp = tmp->next) {
        hash2smartfield((gpointer) tmp->key, (gpointer) tmp->value, args);
    }

    rc = lrmd_send_command(lrmd, LRMD_OP_ALERT_EXEC, data, NULL, timeout,
                           lrmd_opt_notify_orig_only, TRUE);
    free_xml(data);

    lrmd_key_value_freeall(params);
    return rc;
}

static int
lrmd_api_cancel(lrmd_t *lrmd, const char *rsc_id, const char *action,
                guint interval_ms)
{
    int rc = pcmk_ok;
    xmlNode *data = create_xml_node(NULL, F_LRMD_RSC);

    crm_xml_add(data, F_LRMD_ORIGIN, __func__);
    crm_xml_add(data, F_LRMD_RSC_ACTION, action);
    crm_xml_add(data, F_LRMD_RSC_ID, rsc_id);
    crm_xml_add_ms(data, F_LRMD_RSC_INTERVAL, interval_ms);
    rc = lrmd_send_command(lrmd, LRMD_OP_RSC_CANCEL, data, NULL, 0, 0, TRUE);
    free_xml(data);
    return rc;
}

static int
list_stonith_agents(lrmd_list_t ** resources)
{
    int rc = 0;
    stonith_t *stonith_api = stonith_api_new();
    stonith_key_value_t *stonith_resources = NULL;
    stonith_key_value_t *dIter = NULL;

    if (stonith_api == NULL) {
        crm_err("Could not list fence agents: API memory allocation failed");
        return -ENOMEM;
    }
    stonith_api->cmds->list_agents(stonith_api, st_opt_sync_call, NULL,
                                   &stonith_resources, 0);
    stonith_api->cmds->free(stonith_api);

    for (dIter = stonith_resources; dIter; dIter = dIter->next) {
        rc++;
        if (resources) {
            *resources = lrmd_list_add(*resources, dIter->value);
        }
    }

    stonith_key_value_freeall(stonith_resources, 1, 0);
    return rc;
}

static int
lrmd_api_list_agents(lrmd_t * lrmd, lrmd_list_t ** resources, const char *class,
                     const char *provider)
{
    int rc = 0;
    int stonith_count = 0; // Initially, whether to include stonith devices

    if (pcmk__str_eq(class, PCMK_RESOURCE_CLASS_STONITH, pcmk__str_casei)) {
        stonith_count = 1;

    } else {
        GList *gIter = NULL;
        GList *agents = resources_list_agents(class, provider);

        for (gIter = agents; gIter != NULL; gIter = gIter->next) {
            *resources = lrmd_list_add(*resources, (const char *)gIter->data);
            rc++;
        }
        g_list_free_full(agents, free);

        if (!class) {
            stonith_count = 1;
        }
    }

    if (stonith_count) {
        // Now, if stonith devices are included, how many there are
        stonith_count = list_stonith_agents(resources);
        if (stonith_count > 0) {
            rc += stonith_count;
        }
    }
    if (rc == 0) {
        crm_notice("No agents found for class %s", class);
        rc = -EPROTONOSUPPORT;
    }
    return rc;
}

static int
does_provider_have_agent(const char *agent, const char *provider, const char *class)
{
    int found = 0;
    GList *agents = NULL;
    GList *gIter2 = NULL;

    agents = resources_list_agents(class, provider);
    for (gIter2 = agents; gIter2 != NULL; gIter2 = gIter2->next) {
        if (pcmk__str_eq(agent, gIter2->data, pcmk__str_casei)) {
            found = 1;
        }
    }
    g_list_free_full(agents, free);

    return found;
}

static int
lrmd_api_list_ocf_providers(lrmd_t * lrmd, const char *agent, lrmd_list_t ** providers)
{
    int rc = pcmk_ok;
    char *provider = NULL;
    GList *ocf_providers = NULL;
    GList *gIter = NULL;

    ocf_providers = resources_list_providers(PCMK_RESOURCE_CLASS_OCF);

    for (gIter = ocf_providers; gIter != NULL; gIter = gIter->next) {
        provider = gIter->data;
        if (!agent || does_provider_have_agent(agent, provider,
                                               PCMK_RESOURCE_CLASS_OCF)) {
            *providers = lrmd_list_add(*providers, (const char *)gIter->data);
            rc++;
        }
    }

    g_list_free_full(ocf_providers, free);
    return rc;
}

static int
lrmd_api_list_standards(lrmd_t * lrmd, lrmd_list_t ** supported)
{
    int rc = 0;
    GList *standards = NULL;
    GList *gIter = NULL;

    standards = resources_list_standards();

    for (gIter = standards; gIter != NULL; gIter = gIter->next) {
        *supported = lrmd_list_add(*supported, (const char *)gIter->data);
        rc++;
    }

    if (list_stonith_agents(NULL) > 0) {
        *supported = lrmd_list_add(*supported, PCMK_RESOURCE_CLASS_STONITH);
        rc++;
    }

    g_list_free_full(standards, free);
    return rc;
}

lrmd_t *
lrmd_api_new(void)
{
    lrmd_t *new_lrmd = NULL;
    lrmd_private_t *pvt = NULL;

    new_lrmd = calloc(1, sizeof(lrmd_t));
    pvt = calloc(1, sizeof(lrmd_private_t));
    pvt->remote = calloc(1, sizeof(pcmk__remote_t));
    new_lrmd->cmds = calloc(1, sizeof(lrmd_api_operations_t));

    pvt->type = pcmk__client_ipc;
    new_lrmd->lrmd_private = pvt;

    new_lrmd->cmds->connect = lrmd_api_connect;
    new_lrmd->cmds->connect_async = lrmd_api_connect_async;
    new_lrmd->cmds->is_connected = lrmd_api_is_connected;
    new_lrmd->cmds->poke_connection = lrmd_api_poke_connection;
    new_lrmd->cmds->disconnect = lrmd_api_disconnect;
    new_lrmd->cmds->register_rsc = lrmd_api_register_rsc;
    new_lrmd->cmds->unregister_rsc = lrmd_api_unregister_rsc;
    new_lrmd->cmds->get_rsc_info = lrmd_api_get_rsc_info;
    new_lrmd->cmds->get_recurring_ops = lrmd_api_get_recurring_ops;
    new_lrmd->cmds->set_callback = lrmd_api_set_callback;
    new_lrmd->cmds->get_metadata = lrmd_api_get_metadata;
    new_lrmd->cmds->exec = lrmd_api_exec;
    new_lrmd->cmds->cancel = lrmd_api_cancel;
    new_lrmd->cmds->list_agents = lrmd_api_list_agents;
    new_lrmd->cmds->list_ocf_providers = lrmd_api_list_ocf_providers;
    new_lrmd->cmds->list_standards = lrmd_api_list_standards;
    new_lrmd->cmds->exec_alert = lrmd_api_exec_alert;
    new_lrmd->cmds->get_metadata_params = lrmd_api_get_metadata_params;

    return new_lrmd;
}

lrmd_t *
lrmd_remote_api_new(const char *nodename, const char *server, int port)
{
#ifdef HAVE_GNUTLS_GNUTLS_H
    lrmd_t *new_lrmd = lrmd_api_new();
    lrmd_private_t *native = new_lrmd->lrmd_private;

    if (!nodename && !server) {
        lrmd_api_delete(new_lrmd);
        return NULL;
    }

    native->type = pcmk__client_tls;
    native->remote_nodename = nodename ? strdup(nodename) : strdup(server);
    native->server = server ? strdup(server) : strdup(nodename);
    native->port = port;
    if (native->port == 0) {
        native->port = crm_default_remote_port();
    }

    return new_lrmd;
#else
    crm_err("Cannot communicate with Pacemaker Remote because GnuTLS is not enabled for this build");
    return NULL;
#endif
}

void
lrmd_api_delete(lrmd_t * lrmd)
{
    if (!lrmd) {
        return;
    }
    lrmd->cmds->disconnect(lrmd);       /* no-op if already disconnected */
    free(lrmd->cmds);
    if (lrmd->lrmd_private) {
        lrmd_private_t *native = lrmd->lrmd_private;

#ifdef HAVE_GNUTLS_GNUTLS_H
        free(native->server);
#endif
        free(native->remote_nodename);
        free(native->remote);
        free(native->token);
        free(native->peer_version);
    }

    free(lrmd->lrmd_private);
    free(lrmd);
}
