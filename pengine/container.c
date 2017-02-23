/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
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

#include <crm/msg_xml.h>
#include <allocate.h>
#include <notif.h>
#include <utils.h>
#include <allocate.h>

#define VARIANT_CONTAINER 1
#include <lib/pengine/variant.h>

node_t *
container_color(resource_t * rsc, node_t * prefer, pe_working_set_t * data_set)
{
    container_variant_data_t *container_data = NULL;

    CRM_CHECK(rsc != NULL, return NULL);

    get_container_variant_data(container_data, rsc);
    for (GListPtr gIter = container_data->tuples; gIter != NULL; gIter = gIter->next) {
        container_grouping_t *tuple = (container_grouping_t *)gIter->data;

        CRM_ASSERT(tuple);
        if(tuple->docker) {
            tuple->docker->cmds->allocate(tuple->docker, prefer, data_set);
        }
        if(tuple->ip) {
            tuple->ip->cmds->allocate(tuple->ip, prefer, data_set);
        }
        if(tuple->remote) {
            tuple->remote->cmds->allocate(tuple->remote, prefer, data_set);
        }

        // TODO: Explicitly allocate tuple->child before the container->child?
    }

    if(container_data->child) {
        container_data->child->cmds->allocate(container_data->child, prefer, data_set);
    }

    return NULL;
}

void
container_create_actions(resource_t * rsc, pe_working_set_t * data_set)
{
    container_variant_data_t *container_data = NULL;

    CRM_CHECK(rsc != NULL, return);

    get_container_variant_data(container_data, rsc);
    for (GListPtr gIter = container_data->tuples; gIter != NULL; gIter = gIter->next) {
        container_grouping_t *tuple = (container_grouping_t *)gIter->data;

        CRM_ASSERT(tuple);
        if(tuple->ip) {
            tuple->ip->cmds->create_actions(tuple->ip, data_set);
        }
        if(tuple->docker) {
            tuple->docker->cmds->create_actions(tuple->docker, data_set);
        }
        if(tuple->remote) {
            tuple->remote->cmds->create_actions(tuple->remote, data_set);
        }
    }

    if(container_data->child) {
        container_data->child->cmds->create_actions(container_data->child, data_set);
    }
}

void
container_internal_constraints(resource_t * rsc, pe_working_set_t * data_set)
{
    container_variant_data_t *container_data = NULL;

    CRM_CHECK(rsc != NULL, return);

    get_container_variant_data(container_data, rsc);
    for (GListPtr gIter = container_data->tuples; gIter != NULL; gIter = gIter->next) {
        char *id = NULL;
        container_grouping_t *tuple = (container_grouping_t *)gIter->data;

        CRM_ASSERT(tuple);
        if(tuple->docker) {
            complex_set_cmds(tuple->docker);
            tuple->docker->cmds->internal_constraints(tuple->docker, data_set);
        }

        if(tuple->ip) {
            complex_set_cmds(tuple->ip);
            tuple->ip->cmds->internal_constraints(tuple->ip, data_set);

            // Start ip then docker
            new_rsc_order(tuple->ip, RSC_START, tuple->docker, RSC_START, pe_order_runnable_left, data_set);
            new_rsc_order(tuple->docker, RSC_STOP, tuple->ip, RSC_STOP, pe_order_implies_first, data_set);

            id = crm_strdup_printf("%s-ip-with-docker-%d", rsc->id, tuple->offset);
            rsc_colocation_new(id, NULL, INFINITY, tuple->ip, tuple->docker, NULL, NULL, data_set);
            free(id);
        }

        if(tuple->remote) {
            CRM_ASSERT(tuple->ip);
            complex_set_cmds(tuple->remote);
            tuple->remote->cmds->internal_constraints(tuple->remote, data_set);
            // Start docker then remote
            new_rsc_order(
                tuple->docker, RSC_START, tuple->remote, RSC_START, pe_order_runnable_left, data_set);
            new_rsc_order(
                tuple->remote, RSC_STOP, tuple->docker, RSC_STOP, pe_order_implies_first, data_set);

            id = crm_strdup_printf("%s-remote-with-ip-%d", rsc->id, tuple->offset);
            rsc_colocation_new(id, NULL, INFINITY, tuple->remote, tuple->ip, NULL, NULL, data_set);
            free(id);
        }

        if(tuple->child) {
            CRM_ASSERT(tuple->remote);

            // Start remote then child
            new_rsc_order(
                tuple->remote, RSC_START, tuple->child, RSC_START, pe_order_runnable_left, data_set);
            new_rsc_order(
                tuple->child, RSC_STOP, tuple->remote, RSC_STOP, pe_order_implies_first, data_set);

            // TODO: child _in_ remote
        }

    }

    if(container_data->child) {
        container_data->child->cmds->internal_constraints(container_data->child, data_set);
    }
}

void
container_rsc_colocation_lh(resource_t * rsc_lh, resource_t * rsc_rh, rsc_colocation_t * constraint)
{
    pe_err("Container %s cannot be colocated with anything", rsc_lh->id);
}

void
container_rsc_colocation_rh(resource_t * rsc_lh, resource_t * rsc_rh, rsc_colocation_t * constraint)
{
    pe_err("Container %s cannot be colocated with anything", rsc_rh->id);
}

enum pe_action_flags
container_action_flags(action_t * action, node_t * node)
{
    enum pe_action_flags flags = (pe_action_optional | pe_action_runnable | pe_action_pseudo);
    return flags;
}


enum pe_graph_flags
container_update_actions(action_t * first, action_t * then, node_t * node, enum pe_action_flags flags,
                     enum pe_action_flags filter, enum pe_ordering type)
{
    enum pe_graph_flags changed = pe_graph_none;
    return changed;
}

void
container_rsc_location(resource_t * rsc, rsc_to_node_t * constraint)
{
    GListPtr gIter = rsc->children;

    pe_rsc_trace(rsc, "Processing location constraint %s for %s", constraint->id, rsc->id);

    native_rsc_location(rsc, constraint);

    for (; gIter != NULL; gIter = gIter->next) {
        resource_t *child_rsc = (resource_t *) gIter->data;

        child_rsc->cmds->rsc_location(child_rsc, constraint);
    }
}

void
container_expand(resource_t * rsc, pe_working_set_t * data_set)
{
    container_variant_data_t *container_data = NULL;

    CRM_CHECK(rsc != NULL, return);

    get_container_variant_data(container_data, rsc);
    for (GListPtr gIter = container_data->tuples; gIter != NULL; gIter = gIter->next) {
        container_grouping_t *tuple = (container_grouping_t *)gIter->data;

        CRM_ASSERT(tuple);
        if(tuple->ip) {
            tuple->ip->cmds->expand(tuple->ip, data_set);
        }
        if(tuple->child) {
            tuple->child->cmds->expand(tuple->child, data_set);
        }
        if(tuple->docker) {
            tuple->docker->cmds->expand(tuple->docker, data_set);
        }
        if(tuple->remote) {
            tuple->remote->cmds->expand(tuple->remote, data_set);
        }
    }
}

gboolean
container_create_probe(resource_t * rsc, node_t * node, action_t * complete,
                   gboolean force, pe_working_set_t * data_set)
{
    bool any_created = FALSE;
    container_variant_data_t *container_data = NULL;

    CRM_CHECK(rsc != NULL, return FALSE);

    get_container_variant_data(container_data, rsc);
    for (GListPtr gIter = container_data->tuples; gIter != NULL; gIter = gIter->next) {
        container_grouping_t *tuple = (container_grouping_t *)gIter->data;

        CRM_ASSERT(tuple);
        if(tuple->ip) {
            any_created |= tuple->ip->cmds->create_probe(tuple->ip, node, complete, force, data_set);
        }
        if(tuple->child) {
            any_created |= tuple->child->cmds->create_probe(tuple->child, node, complete, force, data_set);
        }
        if(tuple->docker) {
            any_created |= tuple->docker->cmds->create_probe(tuple->docker, node, complete, force, data_set);
        }
        if(FALSE && tuple->remote) {
            // TODO: Needed?
            any_created |= tuple->remote->cmds->create_probe(tuple->remote, node, complete, force, data_set);
        }
    }
    return any_created;
}

void
container_append_meta(resource_t * rsc, xmlNode * xml)
{
}

GHashTable *
container_merge_weights(resource_t * rsc, const char *rhs, GHashTable * nodes, const char *attr,
                    float factor, enum pe_weights flags)
{
    return rsc_merge_weights(rsc, rhs, nodes, attr, factor, flags);
}
