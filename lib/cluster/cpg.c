/*
 * Copyright 2004-2018 Andrew Beekhof <andrew@beekhof.net>
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>
#include <bzlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <crm/common/ipc.h>
#include <crm/cluster/internal.h>
#include <crm/common/mainloop.h>
#include <sys/utsname.h>

#include <qb/qbipcc.h>
#include <qb/qbutil.h>

#include <corosync/corodefs.h>
#include <corosync/corotypes.h>
#include <corosync/hdb.h>
#include <corosync/cpg.h>

#include <crm/msg_xml.h>

cpg_handle_t pcmk_cpg_handle = 0; /* TODO: Remove, use cluster.cpg_handle */

static bool cpg_evicted = FALSE;
gboolean(*pcmk_cpg_dispatch_fn) (int kind, const char *from, const char *data) = NULL;

#define cs_repeat(counter, max, code) do {		\
	code;						\
	if(rc == CS_ERR_TRY_AGAIN || rc == CS_ERR_QUEUE_FULL) {  \
	    counter++;					\
	    crm_debug("Retrying operation after %ds", counter);	\
	    sleep(counter);				\
	} else {                                        \
            break;                                      \
        }                                               \
    } while(counter < max)

void
cluster_disconnect_cpg(crm_cluster_t *cluster)
{
    pcmk_cpg_handle = 0;
    if (cluster->cpg_handle) {
        crm_trace("Disconnecting CPG");
        cpg_leave(cluster->cpg_handle, &cluster->group);
        cpg_finalize(cluster->cpg_handle);
        mainloop_del_fd(cluster->cpg_gsource);
        cluster->cpg_gsource = NULL;
        cluster->cpg_handle = 0;

    } else {
        crm_info("No CPG connection");
    }
}

uint32_t get_local_nodeid(cpg_handle_t handle)
{
    int rc = CS_OK;
    int retries = 0;
    static uint32_t local_nodeid = 0;
    cpg_handle_t local_handle = handle;
    cpg_callbacks_t cb = { };

    if(local_nodeid != 0) {
        return local_nodeid;
    }

    if(handle == 0) {
        crm_trace("Creating connection");
        cs_repeat(retries, 5, rc = cpg_initialize(&local_handle, &cb));
    }

    if (rc == CS_OK) {
        retries = 0;
        crm_trace("Performing lookup");
        cs_repeat(retries, 5, rc = cpg_local_get(local_handle, &local_nodeid));
    }

    if (rc != CS_OK) {
        crm_err("Could not get local node id from the CPG API: %s (%d)", ais_error2text(rc), rc);
    }
    if(handle == 0) {
        crm_trace("Closing connection");
        cpg_finalize(local_handle);
    }
    crm_debug("Local nodeid is %u", local_nodeid);
    return local_nodeid;
}


GListPtr cs_message_queue = NULL;
int cs_message_timer = 0;

static ssize_t crm_cs_flush(gpointer data);

static gboolean
crm_cs_flush_cb(gpointer data)
{
    cs_message_timer = 0;
    crm_cs_flush(data);
    return FALSE;
}

#define CS_SEND_MAX 200
static ssize_t
crm_cs_flush(gpointer data)
{
    int sent = 0;
    ssize_t rc = 0;
    int queue_len = 0;
    static unsigned int last_sent = 0;
    cpg_handle_t *handle = (cpg_handle_t *)data;

    if (*handle == 0) {
        crm_trace("Connection is dead");
        return pcmk_ok;
    }

    queue_len = g_list_length(cs_message_queue);
    if ((queue_len % 1000) == 0 && queue_len > 1) {
        crm_err("CPG queue has grown to %d", queue_len);

    } else if (queue_len == CS_SEND_MAX) {
        crm_warn("CPG queue has grown to %d", queue_len);
    }

    if (cs_message_timer) {
        /* There is already a timer, wait until it goes off */
        crm_trace("Timer active %d", cs_message_timer);
        return pcmk_ok;
    }

    while (cs_message_queue && sent < CS_SEND_MAX) {
        struct iovec *iov = cs_message_queue->data;

        errno = 0;
        rc = cpg_mcast_joined(*handle, CPG_TYPE_AGREED, iov, 1);

        if (rc != CS_OK) {
            break;
        }

        sent++;
        last_sent++;
        crm_trace("CPG message sent, size=%llu",
                  (unsigned long long) iov->iov_len);

        cs_message_queue = g_list_remove(cs_message_queue, iov);
        free(iov->iov_base);
        free(iov);
    }

    queue_len -= sent;
    if (sent > 1 || cs_message_queue) {
        crm_info("Sent %d CPG messages  (%d remaining, last=%u): %s (%lld)",
                 sent, queue_len, last_sent, ais_error2text(rc),
                 (long long) rc);
    } else {
        crm_trace("Sent %d CPG messages  (%d remaining, last=%u): %s (%lld)",
                  sent, queue_len, last_sent, ais_error2text(rc),
                  (long long) rc);
    }

    if (cs_message_queue) {
        uint32_t delay_ms = 100;
        if(rc != CS_OK) {
            /* Proportionally more if sending failed but cap at 1s */
            delay_ms = QB_MIN(1000, CS_SEND_MAX + (10 * queue_len));
        }
        cs_message_timer = g_timeout_add(delay_ms, crm_cs_flush_cb, data);
    }

    return rc;
}

gboolean
send_cpg_iov(struct iovec * iov)
{
    static unsigned int queued = 0;

    queued++;
    crm_trace("Queueing CPG message %u (%llu bytes)",
              queued, (unsigned long long) iov->iov_len);
    cs_message_queue = g_list_append(cs_message_queue, iov);
    crm_cs_flush(&pcmk_cpg_handle);
    return TRUE;
}

static int
pcmk_cpg_dispatch(gpointer user_data)
{
    int rc = 0;
    crm_cluster_t *cluster = (crm_cluster_t*) user_data;

    rc = cpg_dispatch(cluster->cpg_handle, CS_DISPATCH_ONE);
    if (rc != CS_OK) {
        crm_err("Connection to the CPG API failed: %s (%d)", ais_error2text(rc), rc);
        cluster->cpg_handle = 0;
        return -1;

    } else if(cpg_evicted) {
        crm_err("Evicted from CPG membership");
        return -1;
    }
    return 0;
}

char *
pcmk_message_common_cs(cpg_handle_t handle, uint32_t nodeid, uint32_t pid, void *content,
                        uint32_t *kind, const char **from)
{
    char *data = NULL;
    AIS_Message *msg = (AIS_Message *) content;

    if(handle) {
        // Do filtering and field massaging
        uint32_t local_nodeid = get_local_nodeid(handle);
        const char *local_name = get_local_node_name();

        if (msg->sender.id > 0 && msg->sender.id != nodeid) {
            crm_err("Nodeid mismatch from %d.%d: claimed nodeid=%u", nodeid, pid, msg->sender.id);
            return NULL;

        } else if (msg->host.id != 0 && (local_nodeid != msg->host.id)) {
            /* Not for us */
            crm_trace("Not for us: %u != %u", msg->host.id, local_nodeid);
            return NULL;
        } else if (msg->host.size != 0 && safe_str_neq(msg->host.uname, local_name)) {
            /* Not for us */
            crm_trace("Not for us: %s != %s", msg->host.uname, local_name);
            return NULL;
        }

        msg->sender.id = nodeid;
        if (msg->sender.size == 0) {
            crm_node_t *peer = crm_get_peer(nodeid, NULL);

            if (peer == NULL) {
                crm_err("Peer with nodeid=%u is unknown", nodeid);

            } else if (peer->uname == NULL) {
                crm_err("No uname for peer with nodeid=%u", nodeid);

            } else {
                crm_notice("Fixing uname for peer with nodeid=%u", nodeid);
                msg->sender.size = strlen(peer->uname);
                memset(msg->sender.uname, 0, MAX_NAME);
                memcpy(msg->sender.uname, peer->uname, msg->sender.size);
            }
        }
    }

    crm_trace("Got new%s message (size=%d, %d, %d)",
              msg->is_compressed ? " compressed" : "",
              ais_data_len(msg), msg->size, msg->compressed_size);

    if (kind != NULL) {
        *kind = msg->header.id;
    }
    if (from != NULL) {
        *from = msg->sender.uname;
    }

    if (msg->is_compressed && msg->size > 0) {
        int rc = BZ_OK;
        char *uncompressed = NULL;
        unsigned int new_size = msg->size + 1;

        if (check_message_sanity(msg, NULL) == FALSE) {
            goto badmsg;
        }

        crm_trace("Decompressing message data");
        uncompressed = calloc(1, new_size);
        rc = BZ2_bzBuffToBuffDecompress(uncompressed, &new_size, msg->data, msg->compressed_size, 1, 0);

        if (rc != BZ_OK) {
            crm_err("Decompression failed: %s " CRM_XS " bzerror=%d",
                    bz2_strerror(rc), rc);
            free(uncompressed);
            goto badmsg;
        }

        CRM_ASSERT(rc == BZ_OK);
        CRM_ASSERT(new_size == msg->size);

        data = uncompressed;

    } else if (check_message_sanity(msg, data) == FALSE) {
        goto badmsg;

    } else if (safe_str_eq("identify", data)) {
        char *pid_s = crm_getpid_s();

        send_cluster_text(crm_class_cluster, pid_s, TRUE, NULL, crm_msg_ais);
        free(pid_s);
        return NULL;

    } else {
        data = strdup(msg->data);
    }

    // Is this necessary?
    crm_get_peer(msg->sender.id, msg->sender.uname);

    crm_trace("Payload: %.200s", data);
    return data;

  badmsg:
    crm_err("Invalid message (id=%d, dest=%s:%s, from=%s:%s.%d):"
            " min=%d, total=%d, size=%d, bz2_size=%d",
            msg->id, ais_dest(&(msg->host)), msg_type2text(msg->host.type),
            ais_dest(&(msg->sender)), msg_type2text(msg->sender.type),
            msg->sender.pid, (int)sizeof(AIS_Message),
            msg->header.size, msg->size, msg->compressed_size);

    free(data);
    return NULL;
}

#define PEER_NAME(peer) ((peer)? ((peer)->uname? (peer)->uname : "<unknown>") : "<none>")

void
pcmk_cpg_membership(cpg_handle_t handle,
                    const struct cpg_name *groupName,
                    const struct cpg_address *member_list, size_t member_list_entries,
                    const struct cpg_address *left_list, size_t left_list_entries,
                    const struct cpg_address *joined_list, size_t joined_list_entries)
{
    int i;
    gboolean found = FALSE;
    static int counter = 0;
    uint32_t local_nodeid = get_local_nodeid(handle);

    for (i = 0; i < left_list_entries; i++) {
        crm_node_t *peer = crm_find_peer(left_list[i].nodeid, NULL);

        crm_info("Group event %s.%d: node %u (%s) left",
                 groupName->value, counter, left_list[i].nodeid,
                 PEER_NAME(peer));
        if (peer) {
            crm_update_peer_proc(__FUNCTION__, peer, crm_proc_cpg, OFFLINESTATUS);
        }
    }

    for (i = 0; i < joined_list_entries; i++) {
        crm_info("Group event %s.%d: node %u joined",
                 groupName->value, counter, joined_list[i].nodeid);
    }

    for (i = 0; i < member_list_entries; i++) {
        crm_node_t *peer = crm_get_peer(member_list[i].nodeid, NULL);

        crm_info("Group event %s.%d: node %u (%s) is member",
                 groupName->value, counter, member_list[i].nodeid,
                 PEER_NAME(peer));

        /* If the caller left auto-reaping enabled, this will also update the
         * state to member.
         */
        peer = crm_update_peer_proc(__FUNCTION__, peer, crm_proc_cpg, ONLINESTATUS);

        if (peer && peer->state && strcmp(peer->state, CRM_NODE_MEMBER)) {
            /* The node is a CPG member, but we currently think it's not a
             * cluster member. This is possible only if auto-reaping was
             * disabled. The node may be joining, and we happened to get the CPG
             * notification before the quorum notification; or the node may have
             * just died, and we are processing its final messages; or a bug
             * has affected the peer cache.
             */
            time_t now = time(NULL);

            if (peer->when_lost == 0) {
                // Track when we first got into this contradictory state
                peer->when_lost = now;

            } else if (now > (peer->when_lost + 60)) {
                // If it persists for more than a minute, update the state
                crm_warn("Node %u member of group %s but believed offline",
                         member_list[i].nodeid, groupName->value);
                crm_update_peer_state(__FUNCTION__, peer, CRM_NODE_MEMBER, 0);
            }
        }

        if (local_nodeid == member_list[i].nodeid) {
            found = TRUE;
        }
    }

    if (!found) {
        crm_err("We're not part of CPG group '%s' anymore!", groupName->value);
        cpg_evicted = TRUE;
    }

    counter++;
}

gboolean
cluster_connect_cpg(crm_cluster_t *cluster)
{
    int rc = -1;
    int fd = 0;
    int retries = 0;
    uint32_t id = 0;
    crm_node_t *peer = NULL;
    cpg_handle_t handle = 0;
    const char *message_name = pcmk_message_name(crm_system_name);

    struct mainloop_fd_callbacks cpg_fd_callbacks = {
        .dispatch = pcmk_cpg_dispatch,
        .destroy = cluster->destroy,
    };

    cpg_callbacks_t cpg_callbacks = {
        .cpg_deliver_fn = cluster->cpg.cpg_deliver_fn,
        .cpg_confchg_fn = cluster->cpg.cpg_confchg_fn,
        /* .cpg_deliver_fn = pcmk_cpg_deliver, */
        /* .cpg_confchg_fn = pcmk_cpg_membership, */
    };

    cpg_evicted = FALSE;
    cluster->group.length = 0;
    cluster->group.value[0] = 0;

    /* group.value is char[128] */
    strncpy(cluster->group.value, message_name, 127);
    cluster->group.value[127] = 0;
    cluster->group.length = 1 + QB_MIN(127, strlen(cluster->group.value));

    cs_repeat(retries, 30, rc = cpg_initialize(&handle, &cpg_callbacks));
    if (rc != CS_OK) {
        crm_err("Could not connect to the Cluster Process Group API: %d", rc);
        goto bail;
    }

    id = get_local_nodeid(handle);
    if (id == 0) {
        crm_err("Could not get local node id from the CPG API");
        goto bail;

    }
    cluster->nodeid = id;

    retries = 0;
    cs_repeat(retries, 30, rc = cpg_join(handle, &cluster->group));
    if (rc != CS_OK) {
        crm_err("Could not join the CPG group '%s': %d", message_name, rc);
        goto bail;
    }

    rc = cpg_fd_get(handle, &fd);
    if (rc != CS_OK) {
        crm_err("Could not obtain the CPG API connection: %d", rc);
        goto bail;
    }

    pcmk_cpg_handle = handle;
    cluster->cpg_handle = handle;
    cluster->cpg_gsource = mainloop_add_fd("corosync-cpg", G_PRIORITY_MEDIUM, fd, cluster, &cpg_fd_callbacks);

  bail:
    if (rc != CS_OK) {
        cpg_finalize(handle);
        return FALSE;
    }

    peer = crm_get_peer(id, NULL);
    crm_update_peer_proc(__FUNCTION__, peer, crm_proc_cpg, ONLINESTATUS);
    return TRUE;
}

gboolean
send_cluster_message_cs(xmlNode * msg, gboolean local, crm_node_t * node, enum crm_ais_msg_types dest)
{
    gboolean rc = TRUE;
    char *data = NULL;

    data = dump_xml_unformatted(msg);
    rc = send_cluster_text(crm_class_cluster, data, local, node, dest);
    free(data);
    return rc;
}

gboolean
send_cluster_text(enum crm_ais_msg_class msg_class, const char *data,
                  gboolean local, crm_node_t *node, enum crm_ais_msg_types dest)
{
    static int msg_id = 0;
    static int local_pid = 0;
    static int local_name_len = 0;
    static const char *local_name = NULL;

    char *target = NULL;
    struct iovec *iov;
    AIS_Message *msg = NULL;
    enum crm_ais_msg_types sender = text2msg_type(crm_system_name);

    switch (msg_class) {
        case crm_class_cluster:
            break;
        default:
            crm_err("Invalid message class: %d", msg_class);
            return FALSE;
    }

    CRM_CHECK(dest != crm_msg_ais, return FALSE);

    if(local_name == NULL) {
        local_name = get_local_node_name();
    }
    if(local_name_len == 0 && local_name) {
        local_name_len = strlen(local_name);
    }

    if (data == NULL) {
        data = "";
    }

    if (local_pid == 0) {
        local_pid = getpid();
    }

    if (sender == crm_msg_none) {
        sender = local_pid;
    }

    msg = calloc(1, sizeof(AIS_Message));

    msg_id++;
    msg->id = msg_id;
    msg->header.id = msg_class;
    msg->header.error = CS_OK;

    msg->host.type = dest;
    msg->host.local = local;

    if (node) {
        if (node->uname) {
            target = strdup(node->uname);
            msg->host.size = strlen(node->uname);
            memset(msg->host.uname, 0, MAX_NAME);
            memcpy(msg->host.uname, node->uname, msg->host.size);
        } else {
            target = crm_strdup_printf("%u", node->id);
        }
        msg->host.id = node->id;
    } else {
        target = strdup("all");
    }

    msg->sender.id = 0;
    msg->sender.type = sender;
    msg->sender.pid = local_pid;
    msg->sender.size = local_name_len;
    memset(msg->sender.uname, 0, MAX_NAME);
    if(local_name && msg->sender.size) {
        memcpy(msg->sender.uname, local_name, msg->sender.size);
    }

    msg->size = 1 + strlen(data);
    msg->header.size = sizeof(AIS_Message) + msg->size;

    if (msg->size < CRM_BZ2_THRESHOLD) {
        msg = realloc_safe(msg, msg->header.size);
        memcpy(msg->data, data, msg->size);

    } else {
        char *compressed = NULL;
        unsigned int new_size = 0;
        char *uncompressed = strdup(data);

        if (crm_compress_string(uncompressed, msg->size, 0, &compressed, &new_size)) {

            msg->header.size = sizeof(AIS_Message) + new_size;
            msg = realloc_safe(msg, msg->header.size);
            memcpy(msg->data, compressed, new_size);

            msg->is_compressed = TRUE;
            msg->compressed_size = new_size;

        } else {
            msg = realloc_safe(msg, msg->header.size);
            memcpy(msg->data, data, msg->size);
        }

        free(uncompressed);
        free(compressed);
    }

    iov = calloc(1, sizeof(struct iovec));
    iov->iov_base = msg;
    iov->iov_len = msg->header.size;

    if (msg->compressed_size) {
        crm_trace("Queueing CPG message %u to %s (%llu bytes, %d bytes compressed payload): %.200s",
                  msg->id, target, (unsigned long long) iov->iov_len,
                  msg->compressed_size, data);
    } else {
        crm_trace("Queueing CPG message %u to %s (%llu bytes, %d bytes payload): %.200s",
                  msg->id, target, (unsigned long long) iov->iov_len,
                  msg->size, data);
    }
    free(target);

    send_cpg_iov(iov);

    return TRUE;
}

enum crm_ais_msg_types
text2msg_type(const char *text)
{
    int type = crm_msg_none;

    CRM_CHECK(text != NULL, return type);
    text = pcmk_message_name(text);
    if (safe_str_eq(text, "ais")) {
        type = crm_msg_ais;
    } else if (safe_str_eq(text, CRM_SYSTEM_CIB)) {
        type = crm_msg_cib;
    } else if (safe_str_eq(text, CRM_SYSTEM_CRMD)
               || safe_str_eq(text, CRM_SYSTEM_DC)) {
        type = crm_msg_crmd;
    } else if (safe_str_eq(text, CRM_SYSTEM_TENGINE)) {
        type = crm_msg_te;
    } else if (safe_str_eq(text, CRM_SYSTEM_PENGINE)) {
        type = crm_msg_pe;
    } else if (safe_str_eq(text, CRM_SYSTEM_LRMD)) {
        type = crm_msg_lrmd;
    } else if (safe_str_eq(text, CRM_SYSTEM_STONITHD)) {
        type = crm_msg_stonithd;
    } else if (safe_str_eq(text, "stonith-ng")) {
        type = crm_msg_stonith_ng;
    } else if (safe_str_eq(text, "attrd")) {
        type = crm_msg_attrd;

    } else {
        /* This will normally be a transient client rather than
         * a cluster daemon.  Set the type to the pid of the client
         */
        int scan_rc = sscanf(text, "%d", &type);

        if (scan_rc != 1 || type <= crm_msg_stonith_ng) {
            /* Ensure it's sane */
            type = crm_msg_none;
        }
    }
    return type;
}
