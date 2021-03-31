/*
 * Copyright 2004-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <regex.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <crm/crm.h>
#include <crm/lrmd.h>           // lrmd_event_data_t, lrmd_rsc_info_t, etc.
#include <crm/services.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/pengine/rules.h>

#include <pacemaker-internal.h>
#include <pacemaker-controld.h>

#define START_DELAY_THRESHOLD 5 * 60 * 1000
#define MAX_LRM_REG_FAILS 30

struct delete_event_s {
    int rc;
    const char *rsc;
    lrm_state_t *lrm_state;
};

static gboolean is_rsc_active(lrm_state_t * lrm_state, const char *rsc_id);
static gboolean build_active_RAs(lrm_state_t * lrm_state, xmlNode * rsc_list);
static gboolean stop_recurring_actions(gpointer key, gpointer value, gpointer user_data);

static lrmd_event_data_t *construct_op(lrm_state_t * lrm_state, xmlNode * rsc_op,
                                       const char *rsc_id, const char *operation);
static void do_lrm_rsc_op(lrm_state_t *lrm_state, lrmd_rsc_info_t *rsc,
                          const char *operation, xmlNode *msg);

static gboolean lrm_state_verify_stopped(lrm_state_t * lrm_state, enum crmd_fsa_state cur_state,
                                         int log_level);
static int do_update_resource(const char *node_name, lrmd_rsc_info_t *rsc,
                              lrmd_event_data_t *op, time_t lock_time);

static void
lrm_connection_destroy(void)
{
    if (pcmk_is_set(fsa_input_register, R_LRM_CONNECTED)) {
        crm_crit("Connection to executor failed");
        register_fsa_input(C_FSA_INTERNAL, I_ERROR, NULL);
        controld_clear_fsa_input_flags(R_LRM_CONNECTED);

    } else {
        crm_info("Disconnected from executor");
    }

}

static char *
make_stop_id(const char *rsc, int call_id)
{
    return crm_strdup_printf("%s:%d", rsc, call_id);
}

static void
copy_instance_keys(gpointer key, gpointer value, gpointer user_data)
{
    if (strstr(key, CRM_META "_") == NULL) {
        g_hash_table_replace(user_data, strdup((const char *)key), strdup((const char *)value));
    }
}

static void
copy_meta_keys(gpointer key, gpointer value, gpointer user_data)
{
    if (strstr(key, CRM_META "_") != NULL) {
        g_hash_table_replace(user_data, strdup((const char *)key), strdup((const char *)value));
    }
}

/*!
 * \internal
 * \brief Remove a recurring operation from a resource's history
 *
 * \param[in,out] history  Resource history to modify
 * \param[in]     op       Operation to remove
 *
 * \return TRUE if the operation was found and removed, FALSE otherwise
 */
static gboolean
history_remove_recurring_op(rsc_history_t *history, const lrmd_event_data_t *op)
{
    GList *iter;

    for (iter = history->recurring_op_list; iter != NULL; iter = iter->next) {
        lrmd_event_data_t *existing = iter->data;

        if ((op->interval_ms == existing->interval_ms)
            && pcmk__str_eq(op->rsc_id, existing->rsc_id, pcmk__str_none)
            && pcmk__str_eq(op->op_type, existing->op_type, pcmk__str_casei)) {

            history->recurring_op_list = g_list_delete_link(history->recurring_op_list, iter);
            lrmd_free_event(existing);
            return TRUE;
        }
    }
    return FALSE;
}

/*!
 * \internal
 * \brief Free all recurring operations in resource history
 *
 * \param[in,out] history  Resource history to modify
 */
static void
history_free_recurring_ops(rsc_history_t *history)
{
    GList *iter;

    for (iter = history->recurring_op_list; iter != NULL; iter = iter->next) {
        lrmd_free_event(iter->data);
    }
    g_list_free(history->recurring_op_list);
    history->recurring_op_list = NULL;
}

/*!
 * \internal
 * \brief Free resource history
 *
 * \param[in,out] history  Resource history to free
 */
void
history_free(gpointer data)
{
    rsc_history_t *history = (rsc_history_t*)data;

    if (history->stop_params) {
        g_hash_table_destroy(history->stop_params);
    }

    /* Don't need to free history->rsc.id because it's set to history->id */
    free(history->rsc.type);
    free(history->rsc.standard);
    free(history->rsc.provider);

    lrmd_free_event(history->failed);
    lrmd_free_event(history->last);
    free(history->id);
    history_free_recurring_ops(history);
    free(history);
}

static void
update_history_cache(lrm_state_t * lrm_state, lrmd_rsc_info_t * rsc, lrmd_event_data_t * op)
{
    int target_rc = 0;
    rsc_history_t *entry = NULL;

    if (op->rsc_deleted) {
        crm_debug("Purged history for '%s' after %s", op->rsc_id, op->op_type);
        controld_delete_resource_history(op->rsc_id, lrm_state->node_name,
                                         NULL, crmd_cib_smart_opt());
        return;
    }

    if (pcmk__str_eq(op->op_type, RSC_NOTIFY, pcmk__str_casei)) {
        return;
    }

    crm_debug("Updating history for '%s' with %s op", op->rsc_id, op->op_type);

    entry = g_hash_table_lookup(lrm_state->resource_history, op->rsc_id);
    if (entry == NULL && rsc) {
        entry = calloc(1, sizeof(rsc_history_t));
        entry->id = strdup(op->rsc_id);
        g_hash_table_insert(lrm_state->resource_history, entry->id, entry);

        entry->rsc.id = entry->id;
        entry->rsc.type = strdup(rsc->type);
        entry->rsc.standard = strdup(rsc->standard);
        if (rsc->provider) {
            entry->rsc.provider = strdup(rsc->provider);
        } else {
            entry->rsc.provider = NULL;
        }

    } else if (entry == NULL) {
        crm_info("Resource %s no longer exists, not updating cache", op->rsc_id);
        return;
    }

    entry->last_callid = op->call_id;
    target_rc = rsc_op_expected_rc(op);
    if (op->op_status == PCMK_LRM_OP_CANCELLED) {
        if (op->interval_ms > 0) {
            crm_trace("Removing cancelled recurring op: " PCMK__OP_FMT,
                      op->rsc_id, op->op_type, op->interval_ms);
            history_remove_recurring_op(entry, op);
            return;
        } else {
            crm_trace("Skipping " PCMK__OP_FMT " rc=%d, status=%d",
                      op->rsc_id, op->op_type, op->interval_ms, op->rc,
                      op->op_status);
        }

    } else if (did_rsc_op_fail(op, target_rc)) {
        /* Store failed monitors here, otherwise the block below will cause them
         * to be forgotten when a stop happens.
         */
        if (entry->failed) {
            lrmd_free_event(entry->failed);
        }
        entry->failed = lrmd_copy_event(op);

    } else if (op->interval_ms == 0) {
        if (entry->last) {
            lrmd_free_event(entry->last);
        }
        entry->last = lrmd_copy_event(op);

        if (op->params && pcmk__strcase_any_of(op->op_type, CRMD_ACTION_START,
                                               "reload", CRMD_ACTION_STATUS, NULL)) {
            if (entry->stop_params) {
                g_hash_table_destroy(entry->stop_params);
            }
            entry->stop_params = crm_str_table_new();

            g_hash_table_foreach(op->params, copy_instance_keys, entry->stop_params);
        }
    }

    if (op->interval_ms > 0) {
        /* Ensure there are no duplicates */
        history_remove_recurring_op(entry, op);

        crm_trace("Adding recurring op: " PCMK__OP_FMT,
                  op->rsc_id, op->op_type, op->interval_ms);
        entry->recurring_op_list = g_list_prepend(entry->recurring_op_list, lrmd_copy_event(op));

    } else if (entry->recurring_op_list && !pcmk__str_eq(op->op_type, RSC_STATUS, pcmk__str_casei)) {
        crm_trace("Dropping %d recurring ops because of: " PCMK__OP_FMT,
                  g_list_length(entry->recurring_op_list), op->rsc_id,
                  op->op_type, op->interval_ms);
        history_free_recurring_ops(entry);
    }
}

/*!
 * \internal
 * \brief Send a direct OK ack for a resource task
 *
 * \param[in] lrm_state  LRM connection
 * \param[in] input      Input message being ack'ed
 * \param[in] rsc_id     ID of affected resource
 * \param[in] rsc        Affected resource (if available)
 * \param[in] task       Operation task being ack'ed
 * \param[in] ack_host   Name of host to send ack to
 * \param[in] ack_sys    IPC system name to ack
 */
static void
send_task_ok_ack(lrm_state_t *lrm_state, ha_msg_input_t *input,
                 const char *rsc_id, lrmd_rsc_info_t *rsc, const char *task,
                 const char *ack_host, const char *ack_sys)
{
    lrmd_event_data_t *op = construct_op(lrm_state, input->xml, rsc_id, task);

    op->rc = PCMK_OCF_OK;
    op->op_status = PCMK_LRM_OP_DONE;
    controld_ack_event_directly(ack_host, ack_sys, rsc, op, rsc_id);
    lrmd_free_event(op);
}

static inline const char *
op_node_name(lrmd_event_data_t *op)
{
    return op->remote_nodename? op->remote_nodename : fsa_our_uname;
}

void
lrm_op_callback(lrmd_event_data_t * op)
{
    CRM_CHECK(op != NULL, return);
    switch (op->type) {
        case lrmd_event_disconnect:
            if (op->remote_nodename == NULL) {
                /* If this is the local executor IPC connection, set the right
                 * bits in the controller when the connection goes down.
                 */
                lrm_connection_destroy();
            }
            break;

        case lrmd_event_exec_complete:
            {
                lrm_state_t *lrm_state = lrm_state_find(op_node_name(op));

                CRM_ASSERT(lrm_state != NULL);
                process_lrm_event(lrm_state, op, NULL, NULL);
            }
            break;

        default:
            break;
    }
}

/*	 A_LRM_CONNECT	*/
void
do_lrm_control(long long action,
               enum crmd_fsa_cause cause,
               enum crmd_fsa_state cur_state,
               enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    /* This only pertains to local executor connections. Remote connections are
     * handled as resources within the scheduler. Connecting and disconnecting
     * from remote executor instances is handled differently.
     */

    lrm_state_t *lrm_state = NULL;

    if(fsa_our_uname == NULL) {
        return; /* Nothing to do */
    }
    lrm_state = lrm_state_find_or_create(fsa_our_uname);
    if (lrm_state == NULL) {
        register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
        return;
    }

    if (action & A_LRM_DISCONNECT) {
        if (lrm_state_verify_stopped(lrm_state, cur_state, LOG_INFO) == FALSE) {
            if (action == A_LRM_DISCONNECT) {
                crmd_fsa_stall(FALSE);
                return;
            }
        }

        controld_clear_fsa_input_flags(R_LRM_CONNECTED);
        crm_info("Disconnecting from the executor");
        lrm_state_disconnect(lrm_state);
        lrm_state_reset_tables(lrm_state, FALSE);
        crm_notice("Disconnected from the executor");
    }

    if (action & A_LRM_CONNECT) {
        int ret = pcmk_ok;

        crm_debug("Connecting to the executor");
        ret = lrm_state_ipc_connect(lrm_state);

        if (ret != pcmk_ok) {
            if (lrm_state->num_lrm_register_fails < MAX_LRM_REG_FAILS) {
                crm_warn("Failed to connect to the executor %d time%s (%d max)",
                         lrm_state->num_lrm_register_fails,
                         pcmk__plural_s(lrm_state->num_lrm_register_fails),
                         MAX_LRM_REG_FAILS);

                controld_start_timer(wait_timer);
                crmd_fsa_stall(FALSE);
                return;
            }
        }

        if (ret != pcmk_ok) {
            crm_err("Failed to connect to the executor the max allowed %d time%s",
                    lrm_state->num_lrm_register_fails,
                    pcmk__plural_s(lrm_state->num_lrm_register_fails));
            register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
            return;
        }

        controld_set_fsa_input_flags(R_LRM_CONNECTED);
        crm_info("Connection to the executor established");
    }

    if (action & ~(A_LRM_CONNECT | A_LRM_DISCONNECT)) {
        crm_err("Unexpected action %s in %s", fsa_action2string(action),
                __func__);
    }
}

static gboolean
lrm_state_verify_stopped(lrm_state_t * lrm_state, enum crmd_fsa_state cur_state, int log_level)
{
    int counter = 0;
    gboolean rc = TRUE;
    const char *when = "lrm disconnect";

    GHashTableIter gIter;
    const char *key = NULL;
    rsc_history_t *entry = NULL;
    active_op_t *pending = NULL;

    crm_debug("Checking for active resources before exit");

    if (cur_state == S_TERMINATE) {
        log_level = LOG_ERR;
        when = "shutdown";

    } else if (pcmk_is_set(fsa_input_register, R_SHUTDOWN)) {
        when = "shutdown... waiting";
    }

    if (lrm_state->pending_ops && lrm_state_is_connected(lrm_state) == TRUE) {
        guint removed = g_hash_table_foreach_remove(
            lrm_state->pending_ops, stop_recurring_actions, lrm_state);
        guint nremaining = g_hash_table_size(lrm_state->pending_ops);

        if (removed || nremaining) {
            crm_notice("Stopped %u recurring operation%s at %s (%u remaining)",
                       removed, pcmk__plural_s(removed), when, nremaining);
        }
    }

    if (lrm_state->pending_ops) {
        g_hash_table_iter_init(&gIter, lrm_state->pending_ops);
        while (g_hash_table_iter_next(&gIter, NULL, (void **)&pending)) {
            /* Ignore recurring actions in the shutdown calculations */
            if (pending->interval_ms == 0) {
                counter++;
            }
        }
    }

    if (counter > 0) {
        do_crm_log(log_level, "%d pending executor operation%s at %s",
                   counter, pcmk__plural_s(counter), when);

        if ((cur_state == S_TERMINATE)
            || !pcmk_is_set(fsa_input_register, R_SENT_RSC_STOP)) {
            g_hash_table_iter_init(&gIter, lrm_state->pending_ops);
            while (g_hash_table_iter_next(&gIter, (gpointer*)&key, (gpointer*)&pending)) {
                do_crm_log(log_level, "Pending action: %s (%s)", key, pending->op_key);
            }

        } else {
            rc = FALSE;
        }
        return rc;
    }

    if (lrm_state->resource_history == NULL) {
        return rc;
    }

    if (pcmk_is_set(fsa_input_register, R_SHUTDOWN)) {
        /* At this point we're not waiting, we're just shutting down */
        when = "shutdown";
    }

    counter = 0;
    g_hash_table_iter_init(&gIter, lrm_state->resource_history);
    while (g_hash_table_iter_next(&gIter, NULL, (gpointer*)&entry)) {
        if (is_rsc_active(lrm_state, entry->id) == FALSE) {
            continue;
        }

        counter++;
        if (log_level == LOG_ERR) {
            crm_info("Found %s active at %s", entry->id, when);
        } else {
            crm_trace("Found %s active at %s", entry->id, when);
        }
        if (lrm_state->pending_ops) {
            GHashTableIter hIter;

            g_hash_table_iter_init(&hIter, lrm_state->pending_ops);
            while (g_hash_table_iter_next(&hIter, (gpointer*)&key, (gpointer*)&pending)) {
                if (pcmk__str_eq(entry->id, pending->rsc_id, pcmk__str_none)) {
                    crm_notice("%sction %s (%s) incomplete at %s",
                               pending->interval_ms == 0 ? "A" : "Recurring a",
                               key, pending->op_key, when);
                }
            }
        }
    }

    if (counter) {
        crm_err("%d resource%s active at %s",
                counter, (counter == 1)? " was" : "s were", when);
    }

    return rc;
}

static char *
build_parameter_list(const lrmd_event_data_t *op,
                     const struct ra_metadata_s *metadata,
                     xmlNode *result, enum ra_param_flags_e param_type,
                     bool invert_for_xml)
{
    char *list = NULL;
    size_t len = 0;

    for (GList *iter = metadata->ra_params; iter != NULL; iter = iter->next) {
        struct ra_param_s *param = (struct ra_param_s *) iter->data;
        bool accept = pcmk_is_set(param->rap_flags, param_type);

        if (accept) {
            crm_trace("Attr %s is %s", param->rap_name, ra_param_flag2text(param_type));

            if (list == NULL) {
                // We will later search for " WORD ", so start list with a space
                pcmk__add_word(&list, &len, " ");
            }
            pcmk__add_word(&list, &len, param->rap_name);

        } else {
            crm_trace("Rejecting %s for %s", param->rap_name, ra_param_flag2text(param_type));
        }

        if (result && (invert_for_xml? !accept : accept)) {
            const char *v = g_hash_table_lookup(op->params, param->rap_name);

            if (v != NULL) {
                crm_trace("Adding attr %s=%s to the xml result", param->rap_name, v);
                crm_xml_add(result, param->rap_name, v);
            }

        } else if (result && (invert_for_xml? accept : !accept)) {
                crm_trace("Removing attr %s from the xml result", param->rap_name);
                xml_remove_prop(result, param->rap_name);
        }
    }

    if (list != NULL) {
        // We will later search for " WORD ", so end list with a space
        pcmk__add_word(&list, &len, " ");
    }
    return list;
}

static void
append_restart_list(lrmd_event_data_t *op, struct ra_metadata_s *metadata,
                    xmlNode *update, const char *version)
{
    char *list = NULL;
    char *digest = NULL;
    xmlNode *restart = NULL;

    CRM_LOG_ASSERT(op->params != NULL);

    if (op->interval_ms > 0) {
        /* monitors are not reloadable */
        return;
    }

    if (pcmk_is_set(metadata->ra_flags, ra_supports_reload)) {
        restart = create_xml_node(NULL, XML_TAG_PARAMS);
        /* Add any parameters with unique="1" to the "op-force-restart" list.
         *
         * (Currently, we abuse "unique=0" to indicate reloadability. This is
         * nonstandard and should eventually be replaced once the OCF standard
         * is updated with something better.)
         */
        list = build_parameter_list(op, metadata, restart, ra_param_unique,
                                    FALSE);

    } else {
        /* Resource does not support reloads */
        return;
    }

    digest = calculate_operation_digest(restart, version);
    /* Add "op-force-restart" and "op-restart-digest" to indicate the resource supports reload,
     * no matter if it actually supports any parameters with unique="1"). */
    crm_xml_add(update, XML_LRM_ATTR_OP_RESTART, list? list: "");
    crm_xml_add(update, XML_LRM_ATTR_RESTART_DIGEST, digest);

    crm_trace("%s: %s, %s", op->rsc_id, digest, list);
    crm_log_xml_trace(restart, "restart digest source");

    free_xml(restart);
    free(digest);
    free(list);
}

static void
append_secure_list(lrmd_event_data_t *op, struct ra_metadata_s *metadata,
                   xmlNode *update, const char *version)
{
    char *list = NULL;
    char *digest = NULL;
    xmlNode *secure = NULL;

    CRM_LOG_ASSERT(op->params != NULL);

    /*
     * To keep XML_LRM_ATTR_OP_SECURE short, we want it to contain the
     * secure parameters but XML_LRM_ATTR_SECURE_DIGEST to be based on
     * the insecure ones
     */
    secure = create_xml_node(NULL, XML_TAG_PARAMS);
    g_hash_table_foreach(op->params, hash2field, secure);
    list = build_parameter_list(op, metadata, secure, ra_param_private, TRUE);

    if (list != NULL) {
        pcmk__filter_op_for_digest(secure);
        digest = calculate_operation_digest(secure, version);
        crm_xml_add(update, XML_LRM_ATTR_OP_SECURE, list);
        crm_xml_add(update, XML_LRM_ATTR_SECURE_DIGEST, digest);

        crm_trace("%s: %s, %s", op->rsc_id, digest, list);
        crm_log_xml_trace(secure, "secure digest source");
    } else {
        crm_trace("%s: no secure parameters", op->rsc_id);
    }

    free_xml(secure);
    free(digest);
    free(list);
}

static gboolean
build_operation_update(xmlNode * parent, lrmd_rsc_info_t * rsc, lrmd_event_data_t * op,
                       const char *node_name, const char *src)
{
    int target_rc = 0;
    xmlNode *xml_op = NULL;
    struct ra_metadata_s *metadata = NULL;
    const char *caller_version = NULL;
    lrm_state_t *lrm_state = NULL;

    if (op == NULL) {
        return FALSE;
    }

    target_rc = rsc_op_expected_rc(op);

    /* there is a small risk in formerly mixed clusters that it will
     * be sub-optimal.
     *
     * however with our upgrade policy, the update we send should
     * still be completely supported anyway
     */
    caller_version = g_hash_table_lookup(op->params, XML_ATTR_CRM_VERSION);
    CRM_LOG_ASSERT(caller_version != NULL);

    if(caller_version == NULL) {
        caller_version = CRM_FEATURE_SET;
    }

    crm_trace("Building %s operation update with originator version: %s", op->rsc_id, caller_version);
    xml_op = pcmk__create_history_xml(parent, op, caller_version, target_rc,
                                      fsa_our_uname, src, LOG_DEBUG);
    if (xml_op == NULL) {
        return TRUE;
    }

    if ((rsc == NULL) || (op->params == NULL)
        || !crm_op_needs_metadata(rsc->standard, op->op_type)) {

        crm_trace("No digests needed for %s action on %s (params=%p rsc=%p)",
                  op->op_type, op->rsc_id, op->params, rsc);
        return TRUE;
    }

    lrm_state = lrm_state_find(node_name);
    if (lrm_state == NULL) {
        crm_warn("Cannot calculate digests for operation " PCMK__OP_FMT
                 " because we have no connection to executor for %s",
                 op->rsc_id, op->op_type, op->interval_ms, node_name);
        return TRUE;
    }

    metadata = metadata_cache_get(lrm_state->metadata_cache, rsc);
    if (metadata == NULL) {
        /* For now, we always collect resource agent meta-data via a local,
         * synchronous, direct execution of the agent. This has multiple issues:
         * the executor should execute agents, not the controller; meta-data for
         * Pacemaker Remote nodes should be collected on those nodes, not
         * locally; and the meta-data call shouldn't eat into the timeout of the
         * real action being performed.
         *
         * These issues are planned to be addressed by having the scheduler
         * schedule a meta-data cache check at the beginning of each transition.
         * Once that is working, this block will only be a fallback in case the
         * initial collection fails.
         */
        char *metadata_str = NULL;

        int rc = lrm_state_get_metadata(lrm_state, rsc->standard,
                                        rsc->provider, rsc->type,
                                        &metadata_str, 0);

        if (rc != pcmk_ok) {
            crm_warn("Failed to get metadata for %s (%s:%s:%s)",
                     rsc->id, rsc->standard, rsc->provider, rsc->type);
            return TRUE;
        }

        metadata = metadata_cache_update(lrm_state->metadata_cache, rsc,
                                         metadata_str);
        free(metadata_str);
        if (metadata == NULL) {
            crm_warn("Failed to update metadata for %s (%s:%s:%s)",
                     rsc->id, rsc->standard, rsc->provider, rsc->type);
            return TRUE;
        }
    }

#if ENABLE_VERSIONED_ATTRS
    crm_xml_add(xml_op, XML_ATTR_RA_VERSION, metadata->ra_version);
#endif

    crm_trace("Including additional digests for %s:%s:%s",
              rsc->standard, rsc->provider, rsc->type);
    append_restart_list(op, metadata, xml_op, caller_version);
    append_secure_list(op, metadata, xml_op, caller_version);

    return TRUE;
}

static gboolean
is_rsc_active(lrm_state_t * lrm_state, const char *rsc_id)
{
    rsc_history_t *entry = NULL;

    entry = g_hash_table_lookup(lrm_state->resource_history, rsc_id);
    if (entry == NULL || entry->last == NULL) {
        return FALSE;
    }

    crm_trace("Processing %s: %s.%d=%d", rsc_id, entry->last->op_type,
              entry->last->interval_ms, entry->last->rc);
    if (entry->last->rc == PCMK_OCF_OK && pcmk__str_eq(entry->last->op_type, CRMD_ACTION_STOP, pcmk__str_casei)) {
        return FALSE;

    } else if (entry->last->rc == PCMK_OCF_OK
               && pcmk__str_eq(entry->last->op_type, CRMD_ACTION_MIGRATE, pcmk__str_casei)) {
        // A stricter check is too complex ... leave that to the scheduler
        return FALSE;

    } else if (entry->last->rc == PCMK_OCF_NOT_RUNNING) {
        return FALSE;

    } else if ((entry->last->interval_ms == 0)
               && (entry->last->rc == PCMK_OCF_NOT_CONFIGURED)) {
        /* Badly configured resources can't be reliably stopped */
        return FALSE;
    }

    return TRUE;
}

static gboolean
build_active_RAs(lrm_state_t * lrm_state, xmlNode * rsc_list)
{
    GHashTableIter iter;
    rsc_history_t *entry = NULL;

    g_hash_table_iter_init(&iter, lrm_state->resource_history);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&entry)) {

        GList *gIter = NULL;
        xmlNode *xml_rsc = create_xml_node(rsc_list, XML_LRM_TAG_RESOURCE);

        crm_xml_add(xml_rsc, XML_ATTR_ID, entry->id);
        crm_xml_add(xml_rsc, XML_ATTR_TYPE, entry->rsc.type);
        crm_xml_add(xml_rsc, XML_AGENT_ATTR_CLASS, entry->rsc.standard);
        crm_xml_add(xml_rsc, XML_AGENT_ATTR_PROVIDER, entry->rsc.provider);

        if (entry->last && entry->last->params) {
            const char *container = g_hash_table_lookup(entry->last->params, CRM_META"_"XML_RSC_ATTR_CONTAINER);
            if (container) {
                crm_trace("Resource %s is a part of container resource %s", entry->id, container);
                crm_xml_add(xml_rsc, XML_RSC_ATTR_CONTAINER, container);
            }
        }
        build_operation_update(xml_rsc, &(entry->rsc), entry->failed, lrm_state->node_name,
                               __func__);
        build_operation_update(xml_rsc, &(entry->rsc), entry->last, lrm_state->node_name,
                               __func__);
        for (gIter = entry->recurring_op_list; gIter != NULL; gIter = gIter->next) {
            build_operation_update(xml_rsc, &(entry->rsc), gIter->data, lrm_state->node_name,
                                   __func__);
        }
    }

    return FALSE;
}

static xmlNode *
do_lrm_query_internal(lrm_state_t *lrm_state, int update_flags)
{
    xmlNode *xml_state = NULL;
    xmlNode *xml_data = NULL;
    xmlNode *rsc_list = NULL;
    crm_node_t *peer = NULL;

    peer = crm_get_peer_full(0, lrm_state->node_name, CRM_GET_PEER_ANY);
    CRM_CHECK(peer != NULL, return NULL);

    xml_state = create_node_state_update(peer, update_flags, NULL,
                                         __func__);
    if (xml_state == NULL) {
        return NULL;
    }

    xml_data = create_xml_node(xml_state, XML_CIB_TAG_LRM);
    crm_xml_add(xml_data, XML_ATTR_ID, peer->uuid);
    rsc_list = create_xml_node(xml_data, XML_LRM_TAG_RESOURCES);

    /* Build a list of active (not always running) resources */
    build_active_RAs(lrm_state, rsc_list);

    crm_log_xml_trace(xml_state, "Current executor state");

    return xml_state;
}

xmlNode *
controld_query_executor_state(const char *node_name)
{
    lrm_state_t *lrm_state = lrm_state_find(node_name);

    if (!lrm_state) {
        crm_err("Could not find executor state for node %s", node_name);
        return NULL;
    }
    return do_lrm_query_internal(lrm_state,
                                 node_update_cluster|node_update_peer);
}

/*!
 * \internal
 * \brief Map standard Pacemaker return code to operation status and OCF code
 *
 * \param[out] event  Executor event whose status and return code should be set
 * \param[in]  rc     Standard Pacemaker return code
 */
void
controld_rc2event(lrmd_event_data_t *event, int rc)
{
    switch (rc) {
        case pcmk_rc_ok:
            event->rc = PCMK_OCF_OK;
            event->op_status = PCMK_LRM_OP_DONE;
            break;
        case EACCES:
            event->rc = PCMK_OCF_INSUFFICIENT_PRIV;
            event->op_status = PCMK_LRM_OP_ERROR;
            break;
        default:
            event->rc = PCMK_OCF_UNKNOWN_ERROR;
            event->op_status = PCMK_LRM_OP_ERROR;
            break;
    }
}

/*!
 * \internal
 * \brief Trigger a new transition after CIB status was deleted
 *
 * If a CIB status delete was not expected (as part of the transition graph),
 * trigger a new transition by updating the (arbitrary) "last-lrm-refresh"
 * cluster property.
 *
 * \param[in] from_sys  IPC name that requested the delete
 * \param[in] rsc_id    Resource whose status was deleted (for logging only)
 */
void
controld_trigger_delete_refresh(const char *from_sys, const char *rsc_id)
{
    if (!pcmk__str_eq(from_sys, CRM_SYSTEM_TENGINE, pcmk__str_casei)) {
        char *now_s = crm_strdup_printf("%lld", (long long) time(NULL));

        crm_debug("Triggering a refresh after %s cleaned %s", from_sys, rsc_id);
        update_attr_delegate(fsa_cib_conn, cib_none, XML_CIB_TAG_CRMCONFIG,
                             NULL, NULL, NULL, NULL, "last-lrm-refresh", now_s,
                             FALSE, NULL, NULL);
        free(now_s);
    }
}

static void
notify_deleted(lrm_state_t * lrm_state, ha_msg_input_t * input, const char *rsc_id, int rc)
{
    lrmd_event_data_t *op = NULL;
    const char *from_sys = crm_element_value(input->msg, F_CRM_SYS_FROM);
    const char *from_host = crm_element_value(input->msg, F_CRM_HOST_FROM);

    crm_info("Notifying %s on %s that %s was%s deleted",
             from_sys, (from_host? from_host : "localhost"), rsc_id,
             ((rc == pcmk_ok)? "" : " not"));
    op = construct_op(lrm_state, input->xml, rsc_id, CRMD_ACTION_DELETE);
    controld_rc2event(op, pcmk_legacy2rc(rc));
    controld_ack_event_directly(from_host, from_sys, NULL, op, rsc_id);
    lrmd_free_event(op);
    controld_trigger_delete_refresh(from_sys, rsc_id);
}

static gboolean
lrm_remove_deleted_rsc(gpointer key, gpointer value, gpointer user_data)
{
    struct delete_event_s *event = user_data;
    struct pending_deletion_op_s *op = value;

    if (pcmk__str_eq(event->rsc, op->rsc, pcmk__str_none)) {
        notify_deleted(event->lrm_state, op->input, event->rsc, event->rc);
        return TRUE;
    }
    return FALSE;
}

static gboolean
lrm_remove_deleted_op(gpointer key, gpointer value, gpointer user_data)
{
    const char *rsc = user_data;
    active_op_t *pending = value;

    if (pcmk__str_eq(rsc, pending->rsc_id, pcmk__str_none)) {
        crm_info("Removing op %s:%d for deleted resource %s",
                 pending->op_key, pending->call_id, rsc);
        return TRUE;
    }
    return FALSE;
}

static void
delete_rsc_entry(lrm_state_t * lrm_state, ha_msg_input_t * input, const char *rsc_id,
                 GHashTableIter * rsc_gIter, int rc, const char *user_name)
{
    struct delete_event_s event;

    CRM_CHECK(rsc_id != NULL, return);

    if (rc == pcmk_ok) {
        char *rsc_id_copy = strdup(rsc_id);

        if (rsc_gIter) {
            g_hash_table_iter_remove(rsc_gIter);
        } else {
            g_hash_table_remove(lrm_state->resource_history, rsc_id_copy);
        }
        controld_delete_resource_history(rsc_id_copy, lrm_state->node_name,
                                         user_name, crmd_cib_smart_opt());
        g_hash_table_foreach_remove(lrm_state->pending_ops, lrm_remove_deleted_op, rsc_id_copy);
        free(rsc_id_copy);
    }

    if (input) {
        notify_deleted(lrm_state, input, rsc_id, rc);
    }

    event.rc = rc;
    event.rsc = rsc_id;
    event.lrm_state = lrm_state;
    g_hash_table_foreach_remove(lrm_state->deletion_ops, lrm_remove_deleted_rsc, &event);
}

/*!
 * \internal
 * \brief Erase an LRM history entry from the CIB, given the operation data
 *
 * \param[in] lrm_state  LRM state of the desired node
 * \param[in] op         Operation whose history should be deleted
 */
static void
erase_lrm_history_by_op(lrm_state_t *lrm_state, lrmd_event_data_t *op)
{
    xmlNode *xml_top = NULL;

    CRM_CHECK(op != NULL, return);

    xml_top = create_xml_node(NULL, XML_LRM_TAG_RSC_OP);
    crm_xml_add_int(xml_top, XML_LRM_ATTR_CALLID, op->call_id);
    crm_xml_add(xml_top, XML_ATTR_TRANSITION_KEY, op->user_data);

    if (op->interval_ms > 0) {
        char *op_id = pcmk__op_key(op->rsc_id, op->op_type, op->interval_ms);

        /* Avoid deleting last_failure too (if it was a result of this recurring op failing) */
        crm_xml_add(xml_top, XML_ATTR_ID, op_id);
        free(op_id);
    }

    crm_debug("Erasing resource operation history for " PCMK__OP_FMT " (call=%d)",
              op->rsc_id, op->op_type, op->interval_ms, op->call_id);

    fsa_cib_conn->cmds->remove(fsa_cib_conn, XML_CIB_TAG_STATUS, xml_top,
                               cib_quorum_override);

    crm_log_xml_trace(xml_top, "op:cancel");
    free_xml(xml_top);
}

/* Define xpath to find LRM resource history entry by node and resource */
#define XPATH_HISTORY                                   \
    "/" XML_TAG_CIB "/" XML_CIB_TAG_STATUS              \
    "/" XML_CIB_TAG_STATE "[@" XML_ATTR_UNAME "='%s']"  \
    "/" XML_CIB_TAG_LRM "/" XML_LRM_TAG_RESOURCES       \
    "/" XML_LRM_TAG_RESOURCE "[@" XML_ATTR_ID "='%s']"  \
    "/" XML_LRM_TAG_RSC_OP

/* ... and also by operation key */
#define XPATH_HISTORY_ID XPATH_HISTORY \
    "[@" XML_ATTR_ID "='%s']"

/* ... and also by operation key and operation call ID */
#define XPATH_HISTORY_CALL XPATH_HISTORY \
    "[@" XML_ATTR_ID "='%s' and @" XML_LRM_ATTR_CALLID "='%d']"

/* ... and also by operation key and original operation key */
#define XPATH_HISTORY_ORIG XPATH_HISTORY \
    "[@" XML_ATTR_ID "='%s' and @" XML_LRM_ATTR_TASK_KEY "='%s']"

/*!
 * \internal
 * \brief Erase an LRM history entry from the CIB, given operation identifiers
 *
 * \param[in] lrm_state  LRM state of the node to clear history for
 * \param[in] rsc_id     Name of resource to clear history for
 * \param[in] key        Operation key of operation to clear history for
 * \param[in] orig_op    If specified, delete only if it has this original op
 * \param[in] call_id    If specified, delete entry only if it has this call ID
 */
static void
erase_lrm_history_by_id(lrm_state_t *lrm_state, const char *rsc_id,
                        const char *key, const char *orig_op, int call_id)
{
    char *op_xpath = NULL;

    CRM_CHECK((rsc_id != NULL) && (key != NULL), return);

    if (call_id > 0) {
        op_xpath = crm_strdup_printf(XPATH_HISTORY_CALL,
                                     lrm_state->node_name, rsc_id, key,
                                     call_id);

    } else if (orig_op) {
        op_xpath = crm_strdup_printf(XPATH_HISTORY_ORIG,
                                     lrm_state->node_name, rsc_id, key,
                                     orig_op);
    } else {
        op_xpath = crm_strdup_printf(XPATH_HISTORY_ID,
                                     lrm_state->node_name, rsc_id, key);
    }

    crm_debug("Erasing resource operation history for %s on %s (call=%d)",
              key, rsc_id, call_id);
    fsa_cib_conn->cmds->remove(fsa_cib_conn, op_xpath, NULL,
                               cib_quorum_override | cib_xpath);
    free(op_xpath);
}

static inline gboolean
last_failed_matches_op(rsc_history_t *entry, const char *op, guint interval_ms)
{
    if (entry == NULL) {
        return FALSE;
    }
    if (op == NULL) {
        return TRUE;
    }
    return (pcmk__str_eq(op, entry->failed->op_type, pcmk__str_casei)
            && (interval_ms == entry->failed->interval_ms));
}

/*!
 * \internal
 * \brief Clear a resource's last failure
 *
 * Erase a resource's last failure on a particular node from both the
 * LRM resource history in the CIB, and the resource history remembered
 * for the LRM state.
 *
 * \param[in] rsc_id      Resource name
 * \param[in] node_name   Node name
 * \param[in] operation   If specified, only clear if matching this operation
 * \param[in] interval_ms If operation is specified, it has this interval
 */
void
lrm_clear_last_failure(const char *rsc_id, const char *node_name,
                       const char *operation, guint interval_ms)
{
    char *op_key = NULL;
    char *orig_op_key = NULL;
    lrm_state_t *lrm_state = NULL;

    lrm_state = lrm_state_find(node_name);
    if (lrm_state == NULL) {
        return;
    }

    /* Erase from CIB */
    op_key = pcmk__op_key(rsc_id, "last_failure", 0);
    if (operation) {
        orig_op_key = pcmk__op_key(rsc_id, operation, interval_ms);
    }
    erase_lrm_history_by_id(lrm_state, rsc_id, op_key, orig_op_key, 0);
    free(op_key);
    free(orig_op_key);

    /* Remove from memory */
    if (lrm_state->resource_history) {
        rsc_history_t *entry = g_hash_table_lookup(lrm_state->resource_history,
                                                   rsc_id);

        if (last_failed_matches_op(entry, operation, interval_ms)) {
            lrmd_free_event(entry->failed);
            entry->failed = NULL;
        }
    }
}

/* Returns: gboolean - cancellation is in progress */
static gboolean
cancel_op(lrm_state_t * lrm_state, const char *rsc_id, const char *key, int op, gboolean remove)
{
    int rc = pcmk_ok;
    char *local_key = NULL;
    active_op_t *pending = NULL;

    CRM_CHECK(op != 0, return FALSE);
    CRM_CHECK(rsc_id != NULL, return FALSE);
    if (key == NULL) {
        local_key = make_stop_id(rsc_id, op);
        key = local_key;
    }
    pending = g_hash_table_lookup(lrm_state->pending_ops, key);

    if (pending) {
        if (remove && !pcmk_is_set(pending->flags, active_op_remove)) {
            controld_set_active_op_flags(pending, active_op_remove);
            crm_debug("Scheduling %s for removal", key);
        }

        if (pcmk_is_set(pending->flags, active_op_cancelled)) {
            crm_debug("Operation %s already cancelled", key);
            free(local_key);
            return FALSE;
        }
        controld_set_active_op_flags(pending, active_op_cancelled);

    } else {
        crm_info("No pending op found for %s", key);
        free(local_key);
        return FALSE;
    }

    crm_debug("Cancelling op %d for %s (%s)", op, rsc_id, key);
    rc = lrm_state_cancel(lrm_state, pending->rsc_id, pending->op_type,
                          pending->interval_ms);
    if (rc == pcmk_ok) {
        crm_debug("Op %d for %s (%s): cancelled", op, rsc_id, key);
        free(local_key);
        return TRUE;
    }

    crm_debug("Op %d for %s (%s): Nothing to cancel", op, rsc_id, key);
    /* The caller needs to make sure the entry is
     * removed from the pending_ops list
     *
     * Usually by returning TRUE inside the worker function
     * supplied to g_hash_table_foreach_remove()
     *
     * Not removing the entry from pending_ops will block
     * the node from shutting down
     */
    free(local_key);
    return FALSE;
}

struct cancel_data {
    gboolean done;
    gboolean remove;
    const char *key;
    lrmd_rsc_info_t *rsc;
    lrm_state_t *lrm_state;
};

static gboolean
cancel_action_by_key(gpointer key, gpointer value, gpointer user_data)
{
    gboolean remove = FALSE;
    struct cancel_data *data = user_data;
    active_op_t *op = value;

    if (pcmk__str_eq(op->op_key, data->key, pcmk__str_none)) {
        data->done = TRUE;
        remove = !cancel_op(data->lrm_state, data->rsc->id, key, op->call_id, data->remove);
    }
    return remove;
}

static gboolean
cancel_op_key(lrm_state_t * lrm_state, lrmd_rsc_info_t * rsc, const char *key, gboolean remove)
{
    guint removed = 0;
    struct cancel_data data;

    CRM_CHECK(rsc != NULL, return FALSE);
    CRM_CHECK(key != NULL, return FALSE);

    data.key = key;
    data.rsc = rsc;
    data.done = FALSE;
    data.remove = remove;
    data.lrm_state = lrm_state;

    removed = g_hash_table_foreach_remove(lrm_state->pending_ops, cancel_action_by_key, &data);
    crm_trace("Removed %u op cache entries, new size: %u",
              removed, g_hash_table_size(lrm_state->pending_ops));
    return data.done;
}

/*!
 * \internal
 * \brief Retrieve resource information from LRM
 *
 * \param[in]  lrm_state LRM connection to use
 * \param[in]  rsc_xml   XML containing resource configuration
 * \param[in]  do_create If true, register resource with LRM if not already
 * \param[out] rsc_info  Where to store resource information obtained from LRM
 *
 * \retval pcmk_ok   Success (and rsc_info holds newly allocated result)
 * \retval -EINVAL   Required information is missing from arguments
 * \retval -ENOTCONN No active connection to LRM
 * \retval -ENODEV   Resource not found
 * \retval -errno    Error communicating with executor when registering resource
 *
 * \note Caller is responsible for freeing result on success.
 */
static int
get_lrm_resource(lrm_state_t *lrm_state, xmlNode *rsc_xml, gboolean do_create,
                 lrmd_rsc_info_t **rsc_info)
{
    const char *id = ID(rsc_xml);

    CRM_CHECK(lrm_state && rsc_xml && rsc_info, return -EINVAL);
    CRM_CHECK(id, return -EINVAL);

    if (lrm_state_is_connected(lrm_state) == FALSE) {
        return -ENOTCONN;
    }

    crm_trace("Retrieving resource information for %s from the executor", id);
    *rsc_info = lrm_state_get_rsc_info(lrm_state, id, 0);

    // If resource isn't known by ID, try clone name, if provided
    if (!*rsc_info) {
        const char *long_id = crm_element_value(rsc_xml, XML_ATTR_ID_LONG);

        if (long_id) {
            *rsc_info = lrm_state_get_rsc_info(lrm_state, long_id, 0);
        }
    }

    if ((*rsc_info == NULL) && do_create) {
        const char *class = crm_element_value(rsc_xml, XML_AGENT_ATTR_CLASS);
        const char *provider = crm_element_value(rsc_xml, XML_AGENT_ATTR_PROVIDER);
        const char *type = crm_element_value(rsc_xml, XML_ATTR_TYPE);
        int rc;

        crm_trace("Registering resource %s with the executor", id);
        rc = lrm_state_register_rsc(lrm_state, id, class, provider, type,
                                    lrmd_opt_drop_recurring);
        if (rc != pcmk_ok) {
            fsa_data_t *msg_data = NULL;

            crm_err("Could not register resource %s with the executor on %s: %s "
                    CRM_XS " rc=%d",
                    id, lrm_state->node_name, pcmk_strerror(rc), rc);

            /* Register this as an internal error if this involves the local
             * executor. Otherwise, we're likely dealing with an unresponsive
             * remote node, which is not an FSA failure.
             */
            if (lrm_state_is_local(lrm_state) == TRUE) {
                register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);
            }
            return rc;
        }

        *rsc_info = lrm_state_get_rsc_info(lrm_state, id, 0);
    }
    return *rsc_info? pcmk_ok : -ENODEV;
}

static void
delete_resource(lrm_state_t * lrm_state,
                const char *id,
                lrmd_rsc_info_t * rsc,
                GHashTableIter * gIter,
                const char *sys,
                const char *user,
                ha_msg_input_t * request,
                gboolean unregister)
{
    int rc = pcmk_ok;

    crm_info("Removing resource %s from executor for %s%s%s",
             id, sys, (user? " as " : ""), (user? user : ""));

    if (rsc && unregister) {
        rc = lrm_state_unregister_rsc(lrm_state, id, 0);
    }

    if (rc == pcmk_ok) {
        crm_trace("Resource %s deleted from executor", id);
    } else if (rc == -EINPROGRESS) {
        crm_info("Deletion of resource '%s' from executor is pending", id);
        if (request) {
            struct pending_deletion_op_s *op = NULL;
            char *ref = crm_element_value_copy(request->msg, XML_ATTR_REFERENCE);

            op = calloc(1, sizeof(struct pending_deletion_op_s));
            op->rsc = strdup(rsc->id);
            op->input = copy_ha_msg_input(request);
            g_hash_table_insert(lrm_state->deletion_ops, ref, op);
        }
        return;
    } else {
        crm_warn("Could not delete '%s' from executor for %s%s%s: %s "
                 CRM_XS " rc=%d", id, sys, (user? " as " : ""),
                 (user? user : ""), pcmk_strerror(rc), rc);
    }

    delete_rsc_entry(lrm_state, request, id, gIter, rc, user);
}

static int
get_fake_call_id(lrm_state_t *lrm_state, const char *rsc_id)
{
    int call_id = 999999999;
    rsc_history_t *entry = NULL;

    if(lrm_state) {
        entry = g_hash_table_lookup(lrm_state->resource_history, rsc_id);
    }

    /* Make sure the call id is greater than the last successful operation,
     * otherwise the failure will not result in a possible recovery of the resource
     * as it could appear the failure occurred before the successful start */
    if (entry) {
        call_id = entry->last_callid + 1;
    }

    if (call_id < 0) {
        call_id = 1;
    }
    return call_id;
}

static void
fake_op_status(lrm_state_t *lrm_state, lrmd_event_data_t *op, int op_status,
               enum ocf_exitcode op_exitcode)
{
    op->call_id = get_fake_call_id(lrm_state, op->rsc_id);
    op->t_run = time(NULL);
    op->t_rcchange = op->t_run;
    op->op_status = op_status;
    op->rc = op_exitcode;
}

static void
force_reprobe(lrm_state_t *lrm_state, const char *from_sys,
              const char *from_host, const char *user_name,
              gboolean is_remote_node)
{
    GHashTableIter gIter;
    rsc_history_t *entry = NULL;

    crm_info("Clearing resource history on node %s", lrm_state->node_name);
    g_hash_table_iter_init(&gIter, lrm_state->resource_history);
    while (g_hash_table_iter_next(&gIter, NULL, (void **)&entry)) {
        /* only unregister the resource during a reprobe if it is not a remote connection
         * resource. otherwise unregistering the connection will terminate remote-node
         * membership */
        gboolean unregister = TRUE;

        if (is_remote_lrmd_ra(NULL, NULL, entry->id)) {
            lrm_state_t *remote_lrm_state = lrm_state_find(entry->id);
            if (remote_lrm_state) {
                /* when forcing a reprobe, make sure to clear remote node before
                 * clearing the remote node's connection resource */ 
                force_reprobe(remote_lrm_state, from_sys, from_host, user_name, TRUE);
            }
            unregister = FALSE;
        }

        delete_resource(lrm_state, entry->id, &entry->rsc, &gIter, from_sys,
                        user_name, NULL, unregister);
    }

    /* Now delete the copy in the CIB */
    controld_delete_node_state(lrm_state->node_name, controld_section_lrm,
                               cib_scope_local);

    /* Finally, _delete_ the value in pacemaker-attrd -- setting it to FALSE
     * would result in the scheduler sending us back here again
     */
    update_attrd(lrm_state->node_name, CRM_OP_PROBED, NULL, user_name, is_remote_node);
}

/*!
 * \internal
 * \brief Fail a requested action without actually executing it
 *
 * For an action that can't be executed, process it similarly to an actual
 * execution result, with specified error status (except for notify actions,
 * which will always be treated as successful).
 *
 * \param[in] lrm_state  Executor connection that action is for
 * \param[in] action     Action XML from request
 * \param[in] rc         Desired return code to use
 * \param[in] op_status  Desired operation status to use
 */
static void
synthesize_lrmd_failure(lrm_state_t *lrm_state, xmlNode *action,
                        int op_status, enum ocf_exitcode rc)
{
    lrmd_event_data_t *op = NULL;
    const char *operation = crm_element_value(action, XML_LRM_ATTR_TASK);
    const char *target_node = crm_element_value(action, XML_LRM_ATTR_TARGET);
    xmlNode *xml_rsc = find_xml_node(action, XML_CIB_TAG_RESOURCE, TRUE);

    if ((xml_rsc == NULL) || (ID(xml_rsc) == NULL)) {
        /* @TODO Should we do something else, like direct ack? */
        crm_info("Can't fake %s failure (%d) on %s without resource configuration",
                 crm_element_value(action, XML_LRM_ATTR_TASK_KEY), rc,
                 target_node);
        return;

    } else if(operation == NULL) {
        /* This probably came from crm_resource -C, nothing to do */
        crm_info("Can't fake %s failure (%d) on %s without operation",
                 ID(xml_rsc), rc, target_node);
        return;
    }

    op = construct_op(lrm_state, action, ID(xml_rsc), operation);

    if (pcmk__str_eq(operation, RSC_NOTIFY, pcmk__str_casei)) { // Notifications can't fail
        fake_op_status(lrm_state, op, PCMK_LRM_OP_DONE, PCMK_OCF_OK);
    } else {
        fake_op_status(lrm_state, op, op_status, rc);
    }

    crm_info("Faking " PCMK__OP_FMT " result (%d) on %s",
             op->rsc_id, op->op_type, op->interval_ms, op->rc, target_node);

    // Process the result as if it came from the LRM
    process_lrm_event(lrm_state, op, NULL, action);
    lrmd_free_event(op);
}

/*!
 * \internal
 * \brief Get target of an LRM operation
 *
 * \param[in] xml  LRM operation data XML
 *
 * \return LRM operation target node name (local node or Pacemaker Remote node)
 */
static const char *
lrm_op_target(xmlNode *xml)
{
    const char *target = NULL;

    if (xml) {
        target = crm_element_value(xml, XML_LRM_ATTR_TARGET);
    }
    if (target == NULL) {
        target = fsa_our_uname;
    }
    return target;
}

static void
fail_lrm_resource(xmlNode *xml, lrm_state_t *lrm_state, const char *user_name,
                  const char *from_host, const char *from_sys)
{
    lrmd_event_data_t *op = NULL;
    lrmd_rsc_info_t *rsc = NULL;
    xmlNode *xml_rsc = find_xml_node(xml, XML_CIB_TAG_RESOURCE, TRUE);

    CRM_CHECK(xml_rsc != NULL, return);

    /* The executor simply executes operations and reports the results, without
     * any concept of success or failure, so to fail a resource, we must fake
     * what a failure looks like.
     *
     * To do this, we create a fake executor operation event for the resource,
     * and pass that event to the executor client callback so it will be
     * processed as if it came from the executor.
     */
    op = construct_op(lrm_state, xml, ID(xml_rsc), "asyncmon");
    fake_op_status(lrm_state, op, PCMK_LRM_OP_DONE, PCMK_OCF_UNKNOWN_ERROR);

    free((char*) op->user_data);
    op->user_data = NULL;
    op->interval_ms = 0;

    if (user_name && !pcmk__is_privileged(user_name)) {
        crm_err("%s does not have permission to fail %s", user_name, ID(xml_rsc));
        controld_ack_event_directly(from_host, from_sys, NULL, op, ID(xml_rsc));
        lrmd_free_event(op);
        return;
    }

    if (get_lrm_resource(lrm_state, xml_rsc, TRUE, &rsc) == pcmk_ok) {
        crm_info("Failing resource %s...", rsc->id);
        op->exit_reason = strdup("Simulated failure");
        process_lrm_event(lrm_state, op, NULL, xml);
        op->op_status = PCMK_LRM_OP_DONE;
        op->rc = PCMK_OCF_OK;
        lrmd_free_rsc_info(rsc);

    } else {
        crm_info("Cannot find/create resource in order to fail it...");
        crm_log_xml_warn(xml, "bad input");
    }

    controld_ack_event_directly(from_host, from_sys, NULL, op, ID(xml_rsc));
    lrmd_free_event(op);
}

static void
handle_refresh_op(lrm_state_t *lrm_state, const char *user_name,
                  const char *from_host, const char *from_sys)
{
    int rc = pcmk_ok;
    xmlNode *fragment = do_lrm_query_internal(lrm_state, node_update_all);

    fsa_cib_update(XML_CIB_TAG_STATUS, fragment, cib_quorum_override, rc, user_name);
    crm_info("Forced a local resource history refresh: call=%d", rc);

    if (!pcmk__str_eq(CRM_SYSTEM_CRMD, from_sys, pcmk__str_casei)) {
        xmlNode *reply = create_request(CRM_OP_INVOKE_LRM, fragment, from_host,
                                        from_sys, CRM_SYSTEM_LRMD,
                                        fsa_our_uuid);

        crm_debug("ACK'ing refresh from %s (%s)", from_sys, from_host);

        if (relay_message(reply, TRUE) == FALSE) {
            crm_log_xml_err(reply, "Unable to route reply");
        }
        free_xml(reply);
    }

    free_xml(fragment);
}

static void
handle_query_op(xmlNode *msg, lrm_state_t *lrm_state)
{
    xmlNode *data = do_lrm_query_internal(lrm_state, node_update_all);
    xmlNode *reply = create_reply(msg, data);

    if (relay_message(reply, TRUE) == FALSE) {
        crm_err("Unable to route reply");
        crm_log_xml_err(reply, "reply");
    }
    free_xml(reply);
    free_xml(data);
}

static void
handle_reprobe_op(lrm_state_t *lrm_state, const char *from_sys,
                  const char *from_host, const char *user_name,
                  gboolean is_remote_node)
{
    crm_notice("Forcing the status of all resources to be redetected");
    force_reprobe(lrm_state, from_sys, from_host, user_name, is_remote_node);

    if (!pcmk__strcase_any_of(from_sys, CRM_SYSTEM_PENGINE, CRM_SYSTEM_TENGINE, NULL)) {

        xmlNode *reply = create_request(CRM_OP_INVOKE_LRM, NULL, from_host,
                                        from_sys, CRM_SYSTEM_LRMD,
                                        fsa_our_uuid);

        crm_debug("ACK'ing re-probe from %s (%s)", from_sys, from_host);

        if (relay_message(reply, TRUE) == FALSE) {
            crm_log_xml_err(reply, "Unable to route reply");
        }
        free_xml(reply);
    }
}

static bool do_lrm_cancel(ha_msg_input_t *input, lrm_state_t *lrm_state,
              lrmd_rsc_info_t *rsc, const char *from_host, const char *from_sys)
{
    char *op_key = NULL;
    char *meta_key = NULL;
    int call = 0;
    const char *call_id = NULL;
    const char *op_task = NULL;
    guint interval_ms = 0;
    gboolean in_progress = FALSE;
    xmlNode *params = find_xml_node(input->xml, XML_TAG_ATTRS, TRUE);

    CRM_CHECK(params != NULL, return FALSE);

    meta_key = crm_meta_name(XML_LRM_ATTR_TASK);
    op_task = crm_element_value(params, meta_key);
    free(meta_key);
    CRM_CHECK(op_task != NULL, return FALSE);

    meta_key = crm_meta_name(XML_LRM_ATTR_INTERVAL_MS);
    if (crm_element_value_ms(params, meta_key, &interval_ms) != pcmk_ok) {
        free(meta_key);
        return FALSE;
    }
    free(meta_key);

    op_key = pcmk__op_key(rsc->id, op_task, interval_ms);

    meta_key = crm_meta_name(XML_LRM_ATTR_CALLID);
    call_id = crm_element_value(params, meta_key);
    free(meta_key);

    crm_debug("Scheduler requested op %s (call=%s) be cancelled",
              op_key, (call_id? call_id : "NA"));
    call = crm_parse_int(call_id, "0");
    if (call == 0) {
        // Normal case when the scheduler cancels a recurring op
        in_progress = cancel_op_key(lrm_state, rsc, op_key, TRUE);

    } else {
        // Normal case when the scheduler cancels an orphan op
        in_progress = cancel_op(lrm_state, rsc->id, NULL, call, TRUE);
    }

    // Acknowledge cancellation operation if for a remote connection resource
    if (!in_progress || is_remote_lrmd_ra(NULL, NULL, rsc->id)) {
        char *op_id = make_stop_id(rsc->id, call);

        if (is_remote_lrmd_ra(NULL, NULL, rsc->id) == FALSE) {
            crm_info("Nothing known about operation %d for %s", call, op_key);
        }
        erase_lrm_history_by_id(lrm_state, rsc->id, op_key, NULL, call);
        send_task_ok_ack(lrm_state, input, rsc->id, rsc, op_task,
                         from_host, from_sys);

        /* needed at least for cancellation of a remote operation */
        g_hash_table_remove(lrm_state->pending_ops, op_id);
        free(op_id);

    } else {
        /* No ack is needed since abcdaa8, but peers with older versions
         * in a rolling upgrade need one. We didn't bump the feature set
         * at that commit, so we can only compare against the previous
         * CRM version (3.0.8). If any peers have feature set 3.0.9 but
         * not abcdaa8, they will time out waiting for the ack (no
         * released versions of Pacemaker are affected).
         */
        const char *peer_version = crm_element_value(params, XML_ATTR_CRM_VERSION);

        if (compare_version(peer_version, "3.0.8") <= 0) {
            crm_info("Sending compatibility ack for %s cancellation to %s (CRM version %s)",
                     op_key, from_host, peer_version);
            send_task_ok_ack(lrm_state, input, rsc->id, rsc, op_task,
                             from_host, from_sys);
        }
    }

    free(op_key);
    return TRUE;
}

static void
do_lrm_delete(ha_msg_input_t *input, lrm_state_t *lrm_state,
              lrmd_rsc_info_t *rsc, const char *from_sys, const char *from_host,
              bool crm_rsc_delete, const char *user_name)
{
    gboolean unregister = TRUE;
    int cib_rc = controld_delete_resource_history(rsc->id, lrm_state->node_name,
                                                  user_name,
                                                  cib_dryrun|cib_sync_call);

    if (cib_rc != pcmk_rc_ok) {
        lrmd_event_data_t *op = NULL;

        op = construct_op(lrm_state, input->xml, rsc->id, CRMD_ACTION_DELETE);
        op->op_status = PCMK_LRM_OP_ERROR;

        if (cib_rc == EACCES) {
            op->rc = PCMK_OCF_INSUFFICIENT_PRIV;
        } else {
            op->rc = PCMK_OCF_UNKNOWN_ERROR;
        }
        controld_ack_event_directly(from_host, from_sys, NULL, op, rsc->id);
        lrmd_free_event(op);
        return;
    }

    if (crm_rsc_delete && is_remote_lrmd_ra(NULL, NULL, rsc->id)) {
        unregister = FALSE;
    }

    delete_resource(lrm_state, rsc->id, rsc, NULL, from_sys,
                    user_name, input, unregister);
}

/*	 A_LRM_INVOKE	*/
void
do_lrm_invoke(long long action,
              enum crmd_fsa_cause cause,
              enum crmd_fsa_state cur_state,
              enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    lrm_state_t *lrm_state = NULL;
    const char *crm_op = NULL;
    const char *from_sys = NULL;
    const char *from_host = NULL;
    const char *operation = NULL;
    ha_msg_input_t *input = fsa_typed_data(fsa_dt_ha_msg);
    const char *user_name = NULL;
    const char *target_node = NULL;
    gboolean is_remote_node = FALSE;
    bool crm_rsc_delete = FALSE;

    target_node = lrm_op_target(input->xml);
    is_remote_node = !pcmk__str_eq(target_node, fsa_our_uname,
                                   pcmk__str_casei);

    lrm_state = lrm_state_find(target_node);
    if ((lrm_state == NULL) && is_remote_node) {
        crm_err("Failing action because local node has never had connection to remote node %s",
                target_node);
        synthesize_lrmd_failure(NULL, input->xml, PCMK_LRM_OP_NOT_CONNECTED,
                                PCMK_OCF_UNKNOWN_ERROR);
        return;
    }
    CRM_ASSERT(lrm_state != NULL);

    user_name = pcmk__update_acl_user(input->msg, F_CRM_USER, NULL);
    crm_op = crm_element_value(input->msg, F_CRM_TASK);
    from_sys = crm_element_value(input->msg, F_CRM_SYS_FROM);
    if (!pcmk__str_eq(from_sys, CRM_SYSTEM_TENGINE, pcmk__str_casei)) {
        from_host = crm_element_value(input->msg, F_CRM_HOST_FROM);
    }
    crm_trace("Executor %s command from %s as user %s",
              crm_op, from_sys, user_name);

    if (pcmk__str_eq(crm_op, CRM_OP_LRM_DELETE, pcmk__str_casei)) {
        if (!pcmk__str_eq(from_sys, CRM_SYSTEM_TENGINE, pcmk__str_casei)) {
            crm_rsc_delete = TRUE; // from crm_resource
        }
        operation = CRMD_ACTION_DELETE;

    } else if (pcmk__str_eq(crm_op, CRM_OP_LRM_FAIL, pcmk__str_casei)) {
        fail_lrm_resource(input->xml, lrm_state, user_name, from_host,
                          from_sys);
        return;

    } else if (input->xml != NULL) {
        operation = crm_element_value(input->xml, XML_LRM_ATTR_TASK);
    }

    if (pcmk__str_eq(crm_op, CRM_OP_LRM_REFRESH, pcmk__str_casei)) {
        handle_refresh_op(lrm_state, user_name, from_host, from_sys);

    } else if (pcmk__str_eq(crm_op, CRM_OP_LRM_QUERY, pcmk__str_casei)) {
        handle_query_op(input->msg, lrm_state);

    } else if (pcmk__str_eq(operation, CRM_OP_PROBED, pcmk__str_casei)) {
        update_attrd(lrm_state->node_name, CRM_OP_PROBED, XML_BOOLEAN_TRUE,
                     user_name, is_remote_node);

    } else if (pcmk__str_eq(crm_op, CRM_OP_REPROBE, pcmk__str_casei)
               || pcmk__str_eq(operation, CRM_OP_REPROBE, pcmk__str_casei)) {
        handle_reprobe_op(lrm_state, from_sys, from_host, user_name,
                          is_remote_node);

    } else if (operation != NULL) {
        lrmd_rsc_info_t *rsc = NULL;
        xmlNode *xml_rsc = find_xml_node(input->xml, XML_CIB_TAG_RESOURCE, TRUE);
        gboolean create_rsc = !pcmk__str_eq(operation, CRMD_ACTION_DELETE,
                                            pcmk__str_casei);
        int rc;

        // We can't return anything meaningful without a resource ID
        CRM_CHECK(xml_rsc && ID(xml_rsc), return);

        rc = get_lrm_resource(lrm_state, xml_rsc, create_rsc, &rsc);
        if (rc == -ENOTCONN) {
            synthesize_lrmd_failure(lrm_state, input->xml,
                                    PCMK_LRM_OP_NOT_CONNECTED,
                                    PCMK_OCF_UNKNOWN_ERROR);
            return;

        } else if ((rc < 0) && !create_rsc) {
            /* Delete of malformed or nonexistent resource
             * (deleting something that does not exist is a success)
             */
            crm_notice("Not registering resource '%s' for a %s event "
                       CRM_XS " get-rc=%d (%s) transition-key=%s",
                       ID(xml_rsc), operation,
                       rc, pcmk_strerror(rc), ID(input->xml));
            delete_rsc_entry(lrm_state, input, ID(xml_rsc), NULL, pcmk_ok,
                             user_name);
            return;

        } else if (rc == -EINVAL) {
            // Resource operation on malformed resource
            crm_err("Invalid resource definition for %s", ID(xml_rsc));
            crm_log_xml_warn(input->msg, "invalid resource");
            synthesize_lrmd_failure(lrm_state, input->xml, PCMK_LRM_OP_ERROR,
                                    PCMK_OCF_NOT_CONFIGURED); // fatal error
            return;

        } else if (rc < 0) {
            // Error communicating with the executor
            crm_err("Could not register resource '%s' with executor: %s "
                    CRM_XS " rc=%d",
                    ID(xml_rsc), pcmk_strerror(rc), rc);
            crm_log_xml_warn(input->msg, "failed registration");
            synthesize_lrmd_failure(lrm_state, input->xml, PCMK_LRM_OP_ERROR,
                                    PCMK_OCF_INVALID_PARAM); // hard error
            return;
        }

        if (pcmk__str_eq(operation, CRMD_ACTION_CANCEL, pcmk__str_casei)) {
            if (!do_lrm_cancel(input, lrm_state, rsc, from_host, from_sys)) {
                crm_log_xml_warn(input->xml, "Bad command");
            }

        } else if (pcmk__str_eq(operation, CRMD_ACTION_DELETE, pcmk__str_casei)) {
            do_lrm_delete(input, lrm_state, rsc, from_sys, from_host,
                          crm_rsc_delete, user_name);

        } else {
            do_lrm_rsc_op(lrm_state, rsc, operation, input->xml);
        }

        lrmd_free_rsc_info(rsc);

    } else {
        crm_err("Cannot perform operation %s of unknown type", crm_str(crm_op));
        register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
    }
}

#if ENABLE_VERSIONED_ATTRS
static void
resolve_versioned_parameters(lrm_state_t *lrm_state, const char *rsc_id,
                             const xmlNode *rsc_op, GHashTable *params)
{
    /* Resource info *should* already be cached, so we don't get
     * executor call */
    lrmd_rsc_info_t *rsc = lrm_state_get_rsc_info(lrm_state, rsc_id, 0);
    struct ra_metadata_s *metadata;

    metadata = metadata_cache_get(lrm_state->metadata_cache, rsc);
    if (metadata) {
        xmlNode *versioned_attrs = NULL;
        GHashTable *hash = NULL;
        char *key = NULL;
        char *value = NULL;
        GHashTableIter iter;

        versioned_attrs = first_named_child(rsc_op, XML_TAG_OP_VER_ATTRS);
        hash = pe_unpack_versioned_parameters(versioned_attrs, metadata->ra_version);
        g_hash_table_iter_init(&iter, hash);
        while (g_hash_table_iter_next(&iter, (gpointer *) &key, (gpointer *) &value)) {
            g_hash_table_iter_steal(&iter);
            g_hash_table_replace(params, key, value);
        }
        g_hash_table_destroy(hash);

        versioned_attrs = first_named_child(rsc_op, XML_TAG_OP_VER_META);
        hash = pe_unpack_versioned_parameters(versioned_attrs, metadata->ra_version);
        g_hash_table_iter_init(&iter, hash);
        while (g_hash_table_iter_next(&iter, (gpointer *) &key, (gpointer *) &value)) {
            g_hash_table_replace(params, crm_meta_name(key), strdup(value));

            if (pcmk__str_eq(key, XML_ATTR_TIMEOUT, pcmk__str_casei)) {
                op->timeout = crm_parse_int(value, "0");
            } else if (pcmk__str_eq(key, XML_OP_ATTR_START_DELAY, pcmk__str_casei)) {
                op->start_delay = crm_parse_int(value, "0");
            }
        }
        g_hash_table_destroy(hash);

        versioned_attrs = first_named_child(rsc_op, XML_TAG_RSC_VER_ATTRS);
        hash = pe_unpack_versioned_parameters(versioned_attrs, metadata->ra_version);
        g_hash_table_iter_init(&iter, hash);
        while (g_hash_table_iter_next(&iter, (gpointer *) &key, (gpointer *) &value)) {
            g_hash_table_iter_steal(&iter);
            g_hash_table_replace(params, key, value);
        }
        g_hash_table_destroy(hash);
    }

    lrmd_free_rsc_info(rsc);
}
#endif

static lrmd_event_data_t *
construct_op(lrm_state_t *lrm_state, xmlNode *rsc_op, const char *rsc_id,
             const char *operation)
{
    lrmd_event_data_t *op = NULL;
    const char *op_delay = NULL;
    const char *op_timeout = NULL;
    GHashTable *params = NULL;

    xmlNode *primitive = NULL;
    const char *class = NULL;

    const char *transition = NULL;

    CRM_ASSERT(rsc_id && operation);

    op = lrmd_new_event(rsc_id, operation, 0);
    op->type = lrmd_event_exec_complete;
    op->op_status = PCMK_LRM_OP_PENDING;
    op->rc = -1;
    op->timeout = 0;
    op->start_delay = 0;

    if (rsc_op == NULL) {
        CRM_LOG_ASSERT(pcmk__str_eq(CRMD_ACTION_STOP, operation, pcmk__str_casei));
        op->user_data = NULL;
        /* the stop_all_resources() case
         * by definition there is no DC (or they'd be shutting
         *   us down).
         * So we should put our version here.
         */
        op->params = crm_str_table_new();

        g_hash_table_insert(op->params, strdup(XML_ATTR_CRM_VERSION), strdup(CRM_FEATURE_SET));

        crm_trace("Constructed %s op for %s", operation, rsc_id);
        return op;
    }

    params = xml2list(rsc_op);
    g_hash_table_remove(params, CRM_META "_op_target_rc");

    op_delay = crm_meta_value(params, XML_OP_ATTR_START_DELAY);
    op->start_delay = crm_parse_int(op_delay, "0");

    op_timeout = crm_meta_value(params, XML_ATTR_TIMEOUT);
    op->timeout = crm_parse_int(op_timeout, "0");

    if (pcmk__guint_from_hash(params, CRM_META "_" XML_LRM_ATTR_INTERVAL_MS, 0,
                              &(op->interval_ms)) != pcmk_rc_ok) {
        op->interval_ms = 0;
    }

    /* Use pcmk_monitor_timeout instead of meta timeout for stonith
       recurring monitor, if set */
    primitive = find_xml_node(rsc_op, XML_CIB_TAG_RESOURCE, FALSE);
    class = crm_element_value(primitive, XML_AGENT_ATTR_CLASS);

    if (pcmk_is_set(pcmk_get_ra_caps(class), pcmk_ra_cap_fence_params)
            && pcmk__str_eq(operation, CRMD_ACTION_STATUS, pcmk__str_casei)
            && (op->interval_ms > 0)) {

        op_timeout = g_hash_table_lookup(params, "pcmk_monitor_timeout");
        if (op_timeout != NULL) {
            op->timeout = crm_get_msec(op_timeout);
        }
    }

#if ENABLE_VERSIONED_ATTRS
    if (lrm_state && !is_remote_lrmd_ra(NULL, NULL, rsc_id)
        && !pcmk__strcase_any_of(op_type, CRMD_ACTION_METADATA, CRMD_ACTION_DELETE,
                                 NULL)) {
        resolve_versioned_parameters(lrm_state, rsc_id, rsc_op, params);
    }
#endif

    if (!pcmk__str_eq(operation, RSC_STOP, pcmk__str_casei)) {
        op->params = params;

    } else {
        rsc_history_t *entry = NULL;

        if (lrm_state) {
            entry = g_hash_table_lookup(lrm_state->resource_history, rsc_id);
        }

        /* If we do not have stop parameters cached, use
         * whatever we are given */
        if (!entry || !entry->stop_params) {
            op->params = params;
        } else {
            /* Copy the cached parameter list so that we stop the resource
             * with the old attributes, not the new ones */
            op->params = crm_str_table_new();

            g_hash_table_foreach(params, copy_meta_keys, op->params);
            g_hash_table_foreach(entry->stop_params, copy_instance_keys, op->params);
            g_hash_table_destroy(params);
            params = NULL;
        }
    }

    /* sanity */
    if (op->timeout <= 0) {
        op->timeout = op->interval_ms;
    }
    if (op->start_delay < 0) {
        op->start_delay = 0;
    }

    transition = crm_element_value(rsc_op, XML_ATTR_TRANSITION_KEY);
    CRM_CHECK(transition != NULL, return op);

    op->user_data = strdup(transition);

    if (op->interval_ms != 0) {
        if (pcmk__strcase_any_of(operation, CRMD_ACTION_START, CRMD_ACTION_STOP, NULL)) {
            crm_err("Start and Stop actions cannot have an interval: %u",
                    op->interval_ms);
            op->interval_ms = 0;
        }
    }

    crm_trace("Constructed %s op for %s: interval=%u",
              operation, rsc_id, op->interval_ms);

    return op;
}

/*!
 * \internal
 * \brief Send a (synthesized) event result
 *
 * Reply with a synthesized event result directly, as opposed to going through
 * the executor.
 *
 * \param[in] to_host  Host to send result to
 * \param[in] to_sys   IPC name to send result to (NULL for transition engine)
 * \param[in] rsc      Type information about resource the result is for
 * \param[in] op       Event with result to send
 * \param[in] rsc_id   ID of resource the result is for
 */
void
controld_ack_event_directly(const char *to_host, const char *to_sys,
                            lrmd_rsc_info_t *rsc, lrmd_event_data_t *op,
                            const char *rsc_id)
{
    xmlNode *reply = NULL;
    xmlNode *update, *iter;
    crm_node_t *peer = NULL;

    CRM_CHECK(op != NULL, return);
    if (op->rsc_id == NULL) {
        CRM_ASSERT(rsc_id != NULL);
        op->rsc_id = strdup(rsc_id);
    }
    if (to_sys == NULL) {
        to_sys = CRM_SYSTEM_TENGINE;
    }

    peer = crm_get_peer(0, fsa_our_uname);
    update = create_node_state_update(peer, node_update_none, NULL,
                                      __func__);

    iter = create_xml_node(update, XML_CIB_TAG_LRM);
    crm_xml_add(iter, XML_ATTR_ID, fsa_our_uuid);
    iter = create_xml_node(iter, XML_LRM_TAG_RESOURCES);
    iter = create_xml_node(iter, XML_LRM_TAG_RESOURCE);

    crm_xml_add(iter, XML_ATTR_ID, op->rsc_id);

    build_operation_update(iter, rsc, op, fsa_our_uname, __func__);
    reply = create_request(CRM_OP_INVOKE_LRM, update, to_host, to_sys, CRM_SYSTEM_LRMD, NULL);

    crm_log_xml_trace(update, "[direct ACK]");

    crm_debug("ACK'ing resource op " PCMK__OP_FMT " from %s: %s",
              op->rsc_id, op->op_type, op->interval_ms, op->user_data,
              crm_element_value(reply, XML_ATTR_REFERENCE));

    if (relay_message(reply, TRUE) == FALSE) {
        crm_log_xml_err(reply, "Unable to route reply");
    }

    free_xml(update);
    free_xml(reply);
}

gboolean
verify_stopped(enum crmd_fsa_state cur_state, int log_level)
{
    gboolean res = TRUE;
    GList *lrm_state_list = lrm_state_get_list();
    GList *state_entry;

    for (state_entry = lrm_state_list; state_entry != NULL; state_entry = state_entry->next) {
        lrm_state_t *lrm_state = state_entry->data;

        if (!lrm_state_verify_stopped(lrm_state, cur_state, log_level)) {
            /* keep iterating through all even when false is returned */
            res = FALSE;
        }
    }

    controld_set_fsa_input_flags(R_SENT_RSC_STOP);
    g_list_free(lrm_state_list); lrm_state_list = NULL;
    return res;
}

struct stop_recurring_action_s {
    lrmd_rsc_info_t *rsc;
    lrm_state_t *lrm_state;
};

static gboolean
stop_recurring_action_by_rsc(gpointer key, gpointer value, gpointer user_data)
{
    gboolean remove = FALSE;
    struct stop_recurring_action_s *event = user_data;
    active_op_t *op = value;

    if ((op->interval_ms != 0)
        && pcmk__str_eq(op->rsc_id, event->rsc->id, pcmk__str_none)) {

        crm_debug("Cancelling op %d for %s (%s)", op->call_id, op->rsc_id, (char*)key);
        remove = !cancel_op(event->lrm_state, event->rsc->id, key, op->call_id, FALSE);
    }

    return remove;
}

static gboolean
stop_recurring_actions(gpointer key, gpointer value, gpointer user_data)
{
    gboolean remove = FALSE;
    lrm_state_t *lrm_state = user_data;
    active_op_t *op = value;

    if (op->interval_ms != 0) {
        crm_info("Cancelling op %d for %s (%s)", op->call_id, op->rsc_id,
                 (const char *) key);
        remove = !cancel_op(lrm_state, op->rsc_id, key, op->call_id, FALSE);
    }

    return remove;
}

static void
record_pending_op(const char *node_name, lrmd_rsc_info_t *rsc, lrmd_event_data_t *op)
{
    const char *record_pending = NULL;

    CRM_CHECK(node_name != NULL, return);
    CRM_CHECK(rsc != NULL, return);
    CRM_CHECK(op != NULL, return);

    // Never record certain operation types as pending
    if ((op->op_type == NULL) || (op->params == NULL)
        || !controld_action_is_recordable(op->op_type)) {
        return;
    }

    // defaults to true
    record_pending = crm_meta_value(op->params, XML_OP_ATTR_PENDING);
    if (record_pending && !crm_is_true(record_pending)) {
        return;
    }

    op->call_id = -1;
    op->op_status = PCMK_LRM_OP_PENDING;
    op->rc = PCMK_OCF_UNKNOWN;

    op->t_run = time(NULL);
    op->t_rcchange = op->t_run;

    /* write a "pending" entry to the CIB, inhibit notification */
    crm_debug("Recording pending op " PCMK__OP_FMT " on %s in the CIB",
              op->rsc_id, op->op_type, op->interval_ms, node_name);

    do_update_resource(node_name, rsc, op, 0);
}

static void
do_lrm_rsc_op(lrm_state_t *lrm_state, lrmd_rsc_info_t *rsc,
              const char *operation, xmlNode *msg)
{
    int call_id = 0;
    char *op_id = NULL;
    lrmd_event_data_t *op = NULL;
    lrmd_key_value_t *params = NULL;
    fsa_data_t *msg_data = NULL;
    const char *transition = NULL;
    gboolean stop_recurring = FALSE;
    bool send_nack = FALSE;

    CRM_CHECK(rsc != NULL, return);
    CRM_CHECK(operation != NULL, return);

    if (msg != NULL) {
        transition = crm_element_value(msg, XML_ATTR_TRANSITION_KEY);
        if (transition == NULL) {
            crm_log_xml_err(msg, "Missing transition number");
        }
    }

    op = construct_op(lrm_state, msg, rsc->id, operation);
    CRM_CHECK(op != NULL, return);

    if (is_remote_lrmd_ra(NULL, NULL, rsc->id)
        && (op->interval_ms == 0)
        && strcmp(operation, CRMD_ACTION_MIGRATE) == 0) {

        /* pcmk remote connections are a special use case.
         * We never ever want to stop monitoring a connection resource until
         * the entire migration has completed. If the connection is unexpectedly
         * severed, even during a migration, this is an event we must detect.*/
        stop_recurring = FALSE;

    } else if ((op->interval_ms == 0)
        && strcmp(operation, CRMD_ACTION_STATUS) != 0
        && strcmp(operation, CRMD_ACTION_NOTIFY) != 0) {

        /* stop any previous monitor operations before changing the resource state */
        stop_recurring = TRUE;
    }

    if (stop_recurring == TRUE) {
        guint removed = 0;
        struct stop_recurring_action_s data;

        data.rsc = rsc;
        data.lrm_state = lrm_state;
        removed = g_hash_table_foreach_remove(
            lrm_state->pending_ops, stop_recurring_action_by_rsc, &data);

        if (removed) {
            crm_debug("Stopped %u recurring operation%s in preparation for "
                      PCMK__OP_FMT, removed, pcmk__plural_s(removed),
                      rsc->id, operation, op->interval_ms);
        }
    }

    /* now do the op */
    crm_notice("Requesting local execution of %s operation for %s on %s "
               CRM_XS " transition_key=%s op_key=" PCMK__OP_FMT,
               crm_action_str(op->op_type, op->interval_ms), rsc->id, lrm_state->node_name,
               transition, rsc->id, operation, op->interval_ms);

    if (pcmk_is_set(fsa_input_register, R_SHUTDOWN)
        && pcmk__str_eq(operation, RSC_START, pcmk__str_casei)) {

        register_fsa_input(C_SHUTDOWN, I_SHUTDOWN, NULL);
        send_nack = TRUE;

    } else if (fsa_state != S_NOT_DC
               && fsa_state != S_POLICY_ENGINE /* Recalculating */
               && fsa_state != S_TRANSITION_ENGINE
               && !pcmk__str_eq(operation, CRMD_ACTION_STOP, pcmk__str_casei)) {
        send_nack = TRUE;
    }

    if(send_nack) {
        crm_notice("Discarding attempt to perform action %s on %s in state %s (shutdown=%s)",
                   operation, rsc->id, fsa_state2string(fsa_state),
                   pcmk__btoa(pcmk_is_set(fsa_input_register, R_SHUTDOWN)));

        op->rc = PCMK_OCF_UNKNOWN_ERROR;
        op->op_status = PCMK_LRM_OP_INVALID;
        controld_ack_event_directly(NULL, NULL, rsc, op, rsc->id);
        lrmd_free_event(op);
        free(op_id);
        return;
    }

    record_pending_op(lrm_state->node_name, rsc, op);

    op_id = pcmk__op_key(rsc->id, op->op_type, op->interval_ms);

    if (op->interval_ms > 0) {
        /* cancel it so we can then restart it without conflict */
        cancel_op_key(lrm_state, rsc, op_id, FALSE);
    }

    if (op->params) {
        char *key = NULL;
        char *value = NULL;
        GHashTableIter iter;

        g_hash_table_iter_init(&iter, op->params);
        while (g_hash_table_iter_next(&iter, (gpointer *) & key, (gpointer *) & value)) {
            params = lrmd_key_value_add(params, key, value);
        }
    }

    call_id = lrm_state_exec(lrm_state, rsc->id, op->op_type, op->user_data,
                             op->interval_ms, op->timeout, op->start_delay,
                             params);

    if (call_id <= 0 && lrm_state_is_local(lrm_state)) {
        crm_err("Operation %s on %s failed: %d", operation, rsc->id, call_id);
        register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);

    } else if (call_id <= 0) {
        crm_err("Operation %s on resource %s failed to execute on remote node %s: %d",
                operation, rsc->id, lrm_state->node_name, call_id);
        fake_op_status(lrm_state, op, PCMK_LRM_OP_DONE, PCMK_OCF_UNKNOWN_ERROR);
        process_lrm_event(lrm_state, op, NULL, NULL);

    } else {
        /* record all operations so we can wait
         * for them to complete during shutdown
         */
        char *call_id_s = make_stop_id(rsc->id, call_id);
        active_op_t *pending = NULL;

        pending = calloc(1, sizeof(active_op_t));
        crm_trace("Recording pending op: %d - %s %s", call_id, op_id, call_id_s);

        pending->call_id = call_id;
        pending->interval_ms = op->interval_ms;
        pending->op_type = strdup(operation);
        pending->op_key = strdup(op_id);
        pending->rsc_id = strdup(rsc->id);
        pending->start_time = time(NULL);
        pending->user_data = op->user_data? strdup(op->user_data) : NULL;
        if (crm_element_value_epoch(msg, XML_CONFIG_ATTR_SHUTDOWN_LOCK,
                                    &(pending->lock_time)) != pcmk_ok) {
            pending->lock_time = 0;
        }
        g_hash_table_replace(lrm_state->pending_ops, call_id_s, pending);

        if ((op->interval_ms > 0)
            && (op->start_delay > START_DELAY_THRESHOLD)) {
            int target_rc = 0;

            crm_info("Faking confirmation of %s: execution postponed for over 5 minutes", op_id);
            decode_transition_key(op->user_data, NULL, NULL, NULL, &target_rc);
            op->rc = target_rc;
            op->op_status = PCMK_LRM_OP_DONE;
            controld_ack_event_directly(NULL, NULL, rsc, op, rsc->id);
        }

        pending->params = op->params;
        op->params = NULL;
    }

    free(op_id);
    lrmd_free_event(op);
    return;
}

int last_resource_update = 0;

static void
cib_rsc_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    switch (rc) {
        case pcmk_ok:
        case -pcmk_err_diff_failed:
        case -pcmk_err_diff_resync:
            crm_trace("Resource update %d complete: rc=%d", call_id, rc);
            break;
        default:
            crm_warn("Resource update %d failed: (rc=%d) %s", call_id, rc, pcmk_strerror(rc));
    }

    if (call_id == last_resource_update) {
        last_resource_update = 0;
        trigger_fsa();
    }
}

/* Only successful stops, and probes that found the resource inactive, get locks
 * recorded in the history. This ensures the resource stays locked to the node
 * until it is active there again after the node comes back up.
 */
static bool
should_preserve_lock(lrmd_event_data_t *op)
{
    if (!controld_shutdown_lock_enabled) {
        return false;
    }
    if (!strcmp(op->op_type, RSC_STOP) && (op->rc == PCMK_OCF_OK)) {
        return true;
    }
    if (!strcmp(op->op_type, RSC_STATUS) && (op->rc == PCMK_OCF_NOT_RUNNING)) {
        return true;
    }
    return false;
}

static int
do_update_resource(const char *node_name, lrmd_rsc_info_t *rsc,
                   lrmd_event_data_t *op, time_t lock_time)
{
/*
  <status>
  <nodes_status id=uname>
  <lrm>
  <lrm_resources>
  <lrm_resource id=...>
  </...>
*/
    int rc = pcmk_ok;
    xmlNode *update, *iter = NULL;
    int call_opt = crmd_cib_smart_opt();
    const char *uuid = NULL;

    CRM_CHECK(op != NULL, return 0);

    iter = create_xml_node(iter, XML_CIB_TAG_STATUS);
    update = iter;
    iter = create_xml_node(iter, XML_CIB_TAG_STATE);

    if (pcmk__str_eq(node_name, fsa_our_uname, pcmk__str_casei)) {
        uuid = fsa_our_uuid;

    } else {
        /* remote nodes uuid and uname are equal */
        uuid = node_name;
        crm_xml_add(iter, XML_NODE_IS_REMOTE, "true");
    }

    CRM_LOG_ASSERT(uuid != NULL);
    if(uuid == NULL) {
        rc = -EINVAL;
        goto done;
    }

    crm_xml_add(iter, XML_ATTR_UUID,  uuid);
    crm_xml_add(iter, XML_ATTR_UNAME, node_name);
    crm_xml_add(iter, XML_ATTR_ORIGIN, __func__);

    iter = create_xml_node(iter, XML_CIB_TAG_LRM);
    crm_xml_add(iter, XML_ATTR_ID, uuid);

    iter = create_xml_node(iter, XML_LRM_TAG_RESOURCES);
    iter = create_xml_node(iter, XML_LRM_TAG_RESOURCE);
    crm_xml_add(iter, XML_ATTR_ID, op->rsc_id);

    build_operation_update(iter, rsc, op, node_name, __func__);

    if (rsc) {
        const char *container = NULL;

        crm_xml_add(iter, XML_ATTR_TYPE, rsc->type);
        crm_xml_add(iter, XML_AGENT_ATTR_CLASS, rsc->standard);
        crm_xml_add(iter, XML_AGENT_ATTR_PROVIDER, rsc->provider);
        if (lock_time != 0) {
            /* Actions on a locked resource should either preserve the lock by
             * recording it with the action result, or clear it.
             */
            if (!should_preserve_lock(op)) {
                lock_time = 0;
            }
            crm_xml_add_ll(iter, XML_CONFIG_ATTR_SHUTDOWN_LOCK,
                           (long long) lock_time);
        }

        if (op->params) {
            container = g_hash_table_lookup(op->params, CRM_META"_"XML_RSC_ATTR_CONTAINER);
        }
        if (container) {
            crm_trace("Resource %s is a part of container resource %s", op->rsc_id, container);
            crm_xml_add(iter, XML_RSC_ATTR_CONTAINER, container);
        }

    } else {
        crm_warn("Resource %s no longer exists in the executor", op->rsc_id);
        controld_ack_event_directly(NULL, NULL, rsc, op, op->rsc_id);
        goto cleanup;
    }

    crm_log_xml_trace(update, __func__);

    /* make it an asynchronous call and be done with it
     *
     * Best case:
     *   the resource state will be discovered during
     *   the next signup or election.
     *
     * Bad case:
     *   we are shutting down and there is no DC at the time,
     *   but then why were we shutting down then anyway?
     *   (probably because of an internal error)
     *
     * Worst case:
     *   we get shot for having resources "running" that really weren't
     *
     * the alternative however means blocking here for too long, which
     * isn't acceptable
     */
    fsa_cib_update(XML_CIB_TAG_STATUS, update, call_opt, rc, NULL);

    if (rc > 0) {
        last_resource_update = rc;
    }
  done:
    /* the return code is a call number, not an error code */
    crm_trace("Sent resource state update message: %d for %s=%u on %s",
              rc, op->op_type, op->interval_ms, op->rsc_id);
    fsa_register_cib_callback(rc, FALSE, NULL, cib_rsc_callback);

  cleanup:
    free_xml(update);
    return rc;
}

void
do_lrm_event(long long action,
             enum crmd_fsa_cause cause,
             enum crmd_fsa_state cur_state, enum crmd_fsa_input cur_input, fsa_data_t * msg_data)
{
    CRM_CHECK(FALSE, return);
}

static char *
unescape_newlines(const char *string)
{
    char *pch = NULL;
    char *ret = NULL;
    static const char *escaped_newline = "\\n";

    if (!string) {
        return NULL;
    }

    ret = strdup(string);
    pch = strstr(ret, escaped_newline);
    while (pch != NULL) {
        /* Replace newline escape pattern with actual newline (and a space so we
         * don't have to shuffle the rest of the buffer)
         */
        pch[0] = '\n';
        pch[1] = ' ';
        pch = strstr(pch, escaped_newline);
    }

    return ret;
}

static bool
did_lrm_rsc_op_fail(lrm_state_t *lrm_state, const char * rsc_id,
                    const char * op_type, guint interval_ms)
{
    rsc_history_t *entry = NULL;

    CRM_CHECK(lrm_state != NULL, return FALSE);
    CRM_CHECK(rsc_id != NULL, return FALSE);
    CRM_CHECK(op_type != NULL, return FALSE);

    entry = g_hash_table_lookup(lrm_state->resource_history, rsc_id);
    if (entry == NULL || entry->failed == NULL) {
        return FALSE;
    }

    if (pcmk__str_eq(entry->failed->rsc_id, rsc_id, pcmk__str_none)
        && pcmk__str_eq(entry->failed->op_type, op_type, pcmk__str_casei)
        && entry->failed->interval_ms == interval_ms) {
        return TRUE;
    }

    return FALSE;
}

void
process_lrm_event(lrm_state_t *lrm_state, lrmd_event_data_t *op,
                  active_op_t *pending, xmlNode *action_xml)
{
    char *op_id = NULL;
    char *op_key = NULL;

    int update_id = 0;
    gboolean remove = FALSE;
    gboolean removed = FALSE;
    bool need_direct_ack = FALSE;
    lrmd_rsc_info_t *rsc = NULL;
    const char *node_name = NULL;

    CRM_CHECK(op != NULL, return);
    CRM_CHECK(op->rsc_id != NULL, return);

    // Remap new status codes for older DCs
    if (compare_version(fsa_our_dc_version, "3.2.0") < 0) {
        switch (op->op_status) {
            case PCMK_LRM_OP_NOT_CONNECTED:
                op->op_status = PCMK_LRM_OP_ERROR;
                op->rc = PCMK_OCF_CONNECTION_DIED;
                break;
            case PCMK_LRM_OP_INVALID:
                op->op_status = PCMK_LRM_OP_ERROR;
                op->rc = CRM_DIRECT_NACK_RC;
                break;
            default:
                break;
        }
    }

    op_id = make_stop_id(op->rsc_id, op->call_id);
    op_key = pcmk__op_key(op->rsc_id, op->op_type, op->interval_ms);

    // Get resource info if available (from executor state or action XML)
    if (lrm_state) {
        rsc = lrm_state_get_rsc_info(lrm_state, op->rsc_id, 0);
    }
    if ((rsc == NULL) && action_xml) {
        xmlNode *xml = find_xml_node(action_xml, XML_CIB_TAG_RESOURCE, TRUE);

        const char *standard = crm_element_value(xml, XML_AGENT_ATTR_CLASS);
        const char *provider = crm_element_value(xml, XML_AGENT_ATTR_PROVIDER);
        const char *type = crm_element_value(xml, XML_ATTR_TYPE);

        if (standard && type) {
            crm_info("%s agent information not cached, using %s%s%s:%s from action XML",
                     op->rsc_id, standard,
                     (provider? ":" : ""), (provider? provider : ""), type);
            rsc = lrmd_new_rsc_info(op->rsc_id, standard, provider, type);
        } else {
            crm_err("Can't process %s result because %s agent information not cached or in XML",
                    op_key, op->rsc_id);
        }
    }

    // Get node name if available (from executor state or action XML)
    if (lrm_state) {
        node_name = lrm_state->node_name;
    } else if (action_xml) {
        node_name = crm_element_value(action_xml, XML_LRM_ATTR_TARGET);
    }

    if(pending == NULL) {
        remove = TRUE;
        if (lrm_state) {
            pending = g_hash_table_lookup(lrm_state->pending_ops, op_id);
        }
    }

    if (op->op_status == PCMK_LRM_OP_ERROR) {
        switch(op->rc) {
            case PCMK_OCF_NOT_RUNNING:
            case PCMK_OCF_RUNNING_MASTER:
            case PCMK_OCF_DEGRADED:
            case PCMK_OCF_DEGRADED_MASTER:
                // Leave it to the TE/scheduler to decide if this is an error
                op->op_status = PCMK_LRM_OP_DONE;
                break;
            default:
                /* Nothing to do */
                break;
        }
    }

    if (op->op_status != PCMK_LRM_OP_CANCELLED) {
        /* We might not record the result, so directly acknowledge it to the
         * originator instead, so it doesn't time out waiting for the result
         * (especially important if part of a transition).
         */
        need_direct_ack = TRUE;

        if (controld_action_is_recordable(op->op_type)) {
            if (node_name && rsc) {
                // We should record the result, and happily, we can
                update_id = do_update_resource(node_name, rsc, op,
                                               pending? pending->lock_time : 0);
                need_direct_ack = FALSE;

            } else if (op->rsc_deleted) {
                /* We shouldn't record the result (likely the resource was
                 * refreshed, cleaned, or removed while this operation was
                 * in flight).
                 */
                crm_notice("Not recording %s result in CIB because "
                           "resource information was removed since it was initiated",
                           op_key);
            } else {
                /* This shouldn't be possible; the executor didn't consider the
                 * resource deleted, but we couldn't find resource or node
                 * information.
                 */
                crm_err("Unable to record %s result in CIB: %s", op_key,
                        (node_name? "No resource information" : "No node name"));
            }
        }

    } else if (op->interval_ms == 0) {
        /* A non-recurring operation was cancelled. Most likely, the
         * never-initiated action was removed from the executor's pending
         * operations list upon resource removal.
         */
        need_direct_ack = TRUE;

    } else if (pending == NULL) {
        /* This recurring operation was cancelled, but was not pending. No
         * transition actions are waiting on it, nothing needs to be done.
         */

    } else if (op->user_data == NULL) {
        /* This recurring operation was cancelled and pending, but we don't
         * have a transition key. This should never happen.
         */
        crm_err("Recurring operation %s was cancelled without transition information",
                op_key);

    } else if (pcmk_is_set(pending->flags, active_op_remove)) {
        /* This recurring operation was cancelled (by us) and pending, and we
         * have been waiting for it to finish.
         */
        if (lrm_state) {
            erase_lrm_history_by_op(lrm_state, op);
        }

        /* If the recurring operation had failed, the lrm_rsc_op is recorded as
         * "last_failure" which won't get erased from the cib given the logic on
         * purpose in erase_lrm_history_by_op(). So that the cancel action won't
         * have a chance to get confirmed by DC with process_op_deletion().
         * Cluster transition would get stuck waiting for the remaining action
         * timer to time out.
         *
         * Directly acknowledge the cancel operation in this case.
         */
        if (did_lrm_rsc_op_fail(lrm_state, pending->rsc_id,
                                pending->op_type, pending->interval_ms)) {
            need_direct_ack = TRUE;
        }

    } else if (op->rsc_deleted) {
        /* This recurring operation was cancelled (but not by us, and the
         * executor does not have resource information, likely due to resource
         * cleanup, refresh, or removal) and pending.
         */
        crm_debug("Recurring op %s was cancelled due to resource deletion",
                  op_key);
        need_direct_ack = TRUE;

    } else {
        /* This recurring operation was cancelled (but not by us, likely by the
         * executor before stopping the resource) and pending. We don't need to
         * do anything special.
         */
    }

    if (need_direct_ack) {
        controld_ack_event_directly(NULL, NULL, NULL, op, op->rsc_id);
    }

    if(remove == FALSE) {
        /* The caller will do this afterwards, but keep the logging consistent */
        removed = TRUE;

    } else if (lrm_state && ((op->interval_ms == 0)
                             || (op->op_status == PCMK_LRM_OP_CANCELLED))) {

        gboolean found = g_hash_table_remove(lrm_state->pending_ops, op_id);

        if (op->interval_ms != 0) {
            removed = TRUE;
        } else if (found) {
            removed = TRUE;
            crm_trace("Op %s (call=%d, stop-id=%s, remaining=%u): Confirmed",
                      op_key, op->call_id, op_id,
                      g_hash_table_size(lrm_state->pending_ops));
        }
    }

    if (node_name == NULL) {
        node_name = "unknown node"; // for logging
    }

    switch (op->op_status) {
        case PCMK_LRM_OP_CANCELLED:
            crm_info("Result of %s operation for %s on %s: %s "
                     CRM_XS " call=%d key=%s confirmed=%s",
                     crm_action_str(op->op_type, op->interval_ms),
                     op->rsc_id, node_name,
                     services_lrm_status_str(op->op_status),
                     op->call_id, op_key, pcmk__btoa(removed));
            break;

        case PCMK_LRM_OP_DONE:
            crm_notice("Result of %s operation for %s on %s: %s "
                       CRM_XS " rc=%d call=%d key=%s confirmed=%s cib-update=%d",
                       crm_action_str(op->op_type, op->interval_ms),
                       op->rsc_id, node_name,
                       services_ocf_exitcode_str(op->rc), op->rc,
                       op->call_id, op_key, pcmk__btoa(removed), update_id);
            break;

        case PCMK_LRM_OP_TIMEOUT:
            crm_err("Result of %s operation for %s on %s: %s "
                    CRM_XS " call=%d key=%s timeout=%dms",
                    crm_action_str(op->op_type, op->interval_ms),
                    op->rsc_id, node_name,
                    services_lrm_status_str(op->op_status),
                    op->call_id, op_key, op->timeout);
            break;

        default:
            crm_err("Result of %s operation for %s on %s: %s "
                    CRM_XS " call=%d key=%s confirmed=%s status=%d cib-update=%d",
                    crm_action_str(op->op_type, op->interval_ms),
                    op->rsc_id, node_name,
                    services_lrm_status_str(op->op_status), op->call_id, op_key,
                    pcmk__btoa(removed), op->op_status, update_id);
    }

    if (op->output) {
        char *prefix =
            crm_strdup_printf("%s-" PCMK__OP_FMT ":%d", node_name,
                              op->rsc_id, op->op_type, op->interval_ms,
                              op->call_id);

        if (op->rc) {
            crm_log_output(LOG_NOTICE, prefix, op->output);
        } else {
            crm_log_output(LOG_DEBUG, prefix, op->output);
        }
        free(prefix);
    }

    if (lrm_state) {
        if (!pcmk__str_eq(op->op_type, RSC_METADATA, pcmk__str_casei)) {
            crmd_alert_resource_op(lrm_state->node_name, op);
        } else if (rsc && (op->rc == PCMK_OCF_OK)) {
            char *metadata = unescape_newlines(op->output);

            metadata_cache_update(lrm_state->metadata_cache, rsc, metadata);
            free(metadata);
        }
    }

    if (op->rsc_deleted) {
        crm_info("Deletion of resource '%s' complete after %s", op->rsc_id, op_key);
        if (lrm_state) {
            delete_rsc_entry(lrm_state, NULL, op->rsc_id, NULL, pcmk_ok, NULL);
        }
    }

    /* If a shutdown was escalated while operations were pending,
     * then the FSA will be stalled right now... allow it to continue
     */
    mainloop_set_trigger(fsa_source);
    if (lrm_state && rsc) {
        update_history_cache(lrm_state, rsc, op);
    }

    lrmd_free_rsc_info(rsc);
    free(op_key);
    free(op_id);
}
