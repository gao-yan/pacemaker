/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <glib.h>
#include <dirent.h>

#include <crm/crm.h>
#include <crm/stonith-ng.h>
#include <crm/fencing/internal.h>

/*!
 * \internal
 * \brief Add available RHCS-compatible agents to a list
 *
 * \param[in,out]  List to add to
 *
 * \return Number of agents added
 */
int
stonith__list_rhcs_agents(stonith_key_value_t **devices)
{
    // Essentially: ls -1 @sbin_dir@/fence_*

    int count = 0, i;
    struct dirent **namelist;
    const int file_num = scandir(RH_STONITH_DIR, &namelist, 0, alphasort);

#if _POSIX_C_SOURCE < 200809L && !(defined(O_SEARCH) || defined(O_PATH))
    char buffer[FILENAME_MAX + 1];
#elif defined(O_SEARCH)
    const int dirfd = open(RH_STONITH_DIR, O_SEARCH);
#else
    const int dirfd = open(RH_STONITH_DIR, O_PATH);
#endif

    for (i = 0; i < file_num; i++) {
        struct stat prop;

        if (pcmk__starts_with(namelist[i]->d_name, RH_STONITH_PREFIX)) {
#if _POSIX_C_SOURCE < 200809L && !(defined(O_SEARCH) || defined(O_PATH))
            snprintf(buffer, sizeof(buffer), "%s/%s", RH_STONITH_DIR,
                     namelist[i]->d_name);
            if (stat(buffer, &prop) == 0 && S_ISREG(prop.st_mode)) {
#else
            if (dirfd == -1) {
                if (i == 0) {
                    crm_notice("Problem with listing %s directory"
                               CRM_XS "errno=%d", RH_STONITH_PREFIX, errno);
                }
                free(namelist[i]);
                continue;
            }
            /* note: we can possibly prevent following symlinks here,
                     which may be a good idea, but fall on the nose when
                     these agents are moved elsewhere & linked back */
            if (fstatat(dirfd, namelist[i]->d_name, &prop, 0) == 0
                    && S_ISREG(prop.st_mode)) {
#endif
                *devices = stonith_key_value_add(*devices, NULL,
                                                 namelist[i]->d_name);
                count++;
            }
        }
        free(namelist[i]);
    }
    if (file_num > 0) {
        free(namelist);
    }
#if _POSIX_C_SOURCE >= 200809L || defined(O_SEARCH) || defined(O_PATH)
    if (dirfd >= 0) {
        close(dirfd);
    }
#endif
    return count;
}

static void
stonith_rhcs_parameter_not_required(xmlNode *metadata, const char *parameter)
{
    char *xpath = NULL;
    xmlXPathObject *xpathObj = NULL;

    CRM_CHECK(metadata != NULL, return);
    CRM_CHECK(parameter != NULL, return);

    xpath = crm_strdup_printf("//parameter[@name='%s']", parameter);
    /* Fudge metadata so that the parameter isn't required in config
     * Pacemaker handles and adds it */
    xpathObj = xpath_search(metadata, xpath);
    if (numXpathResults(xpathObj) > 0) {
        xmlNode *tmp = getXpathResult(xpathObj, 0);

        crm_xml_add(tmp, "required", "0");
    }
    freeXpathObject(xpathObj);
    free(xpath);
}

/*!
 * \brief Execute RHCS-compatible agent's meta-data action
 *
 * \param[in]  agent    Agent to execute
 * \param[in]  timeout  Action timeout
 * \param[out] output   Where to store action output (or NULL to ignore)
 *
 * \todo timeout is currently ignored; shouldn't we use it?
 */
int
stonith__rhcs_metadata(const char *agent, int timeout, char **output)
{
    char *buffer = NULL;
    xmlNode *xml = NULL;
    xmlNode *actions = NULL;
    xmlXPathObject *xpathObj = NULL;
    stonith_action_t *action = stonith_action_create(agent, "metadata", NULL, 0,
                                                     5, NULL, NULL, NULL);
    int rc = stonith__execute(action);

    if (rc < 0) {
        crm_warn("Could not execute metadata action for %s: %s "
                 CRM_XS " rc=%d", agent, pcmk_strerror(rc), rc);
        stonith__destroy_action(action);
        return rc;
    }

    stonith__action_result(action, &rc, &buffer, NULL);
    stonith__destroy_action(action);
    if (rc < 0) {
        crm_warn("Metadata action for %s failed: %s " CRM_XS "rc=%d",
                 agent, pcmk_strerror(rc), rc);
        free(buffer);
        return rc;
    }

    if (buffer == NULL) {
        crm_warn("Metadata action for %s returned no data", agent);
        return -ENODATA;
    }

    xml = string2xml(buffer);
    free(buffer);
    buffer = NULL;
    if (xml == NULL) {
        crm_warn("Metadata for %s is invalid", agent);
        return -pcmk_err_schema_validation;
    }

    xpathObj = xpath_search(xml, "//actions");
    if (numXpathResults(xpathObj) > 0) {
        actions = getXpathResult(xpathObj, 0);
    }
    freeXpathObject(xpathObj);

    // Add start and stop (implemented by pacemaker, not agent) to meta-data
    xpathObj = xpath_search(xml, "//action[@name='stop']");
    if (numXpathResults(xpathObj) <= 0) {
        xmlNode *tmp = NULL;

        tmp = create_xml_node(actions, "action");
        crm_xml_add(tmp, "name", "stop");
        crm_xml_add(tmp, "timeout", CRM_DEFAULT_OP_TIMEOUT_S);

        tmp = create_xml_node(actions, "action");
        crm_xml_add(tmp, "name", "start");
        crm_xml_add(tmp, "timeout", CRM_DEFAULT_OP_TIMEOUT_S);
    }
    freeXpathObject(xpathObj);

    // Fudge metadata so parameters are not required in config (pacemaker adds them)
    stonith_rhcs_parameter_not_required(xml, "action");
    stonith_rhcs_parameter_not_required(xml, "plug");
    stonith_rhcs_parameter_not_required(xml, "port");

    buffer = dump_xml_formatted_with_text(xml);
    free_xml(xml);
    if (buffer == NULL) {
        return -pcmk_err_schema_validation;
    }
    if (output) {
        *output = buffer;
    } else {
        free(buffer);
    }
    return pcmk_ok;
}

bool
stonith__agent_is_rhcs(const char *agent)
{
    struct stat prop;
    char *buffer = crm_strdup_printf(RH_STONITH_DIR "/%s", agent);
    int rc = stat(buffer, &prop);

    free(buffer);
    return (rc >= 0) && S_ISREG(prop.st_mode);
}

int
stonith__rhcs_validate(stonith_t *st, int call_options, const char *target,
                       const char *agent, GHashTable *params, int timeout,
                       char **output, char **error_output)
{
    int rc = pcmk_ok;
    stonith_action_t *action = stonith_action_create(agent, "validate-all",
                                                     target, 0, timeout, params,
                                                     NULL, NULL);

    rc = stonith__execute(action);
    if (rc == pcmk_ok) {
        stonith__action_result(action, &rc, output, error_output);
    }
    stonith__destroy_action(action);
    return rc;
}
