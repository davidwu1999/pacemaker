/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>  /* U64T ~ PRIu64 */

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/cluster/internal.h>

#include <crm/common/xml.h>
#include <crm/common/remote_internal.h>

#include <pacemaker-based.h>

#define EXIT_ESCALATION_MS 10000

static unsigned long cib_local_bcast_num = 0;

typedef struct cib_local_notify_s {
    xmlNode *notify_src;
    char *client_id;
    gboolean from_peer;
    gboolean sync_reply;
} cib_local_notify_t;

int next_client_id = 0;

gboolean legacy_mode = FALSE;

qb_ipcs_service_t *ipcs_ro = NULL;
qb_ipcs_service_t *ipcs_rw = NULL;
qb_ipcs_service_t *ipcs_shm = NULL;

void send_cib_replace(const xmlNode * sync_request, const char *host);
static void cib_process_request(xmlNode* request, gboolean force_synchronous,
                                gboolean privileged,
                                pcmk__client_t *cib_client);


static int cib_process_command(xmlNode *request, xmlNode **reply,
                               xmlNode **cib_diff, gboolean privileged);

gboolean cib_common_callback(qb_ipcs_connection_t * c, void *data, size_t size,
                             gboolean privileged);

gboolean cib_legacy_mode(void)
{
    return legacy_mode;
}


static int32_t
cib_ipc_accept(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
    if (cib_shutdown_flag) {
        crm_info("Ignoring new IPC client [%d] during shutdown",
                 pcmk__client_pid(c));
        return -EPERM;
    }

    if (pcmk__new_client(c, uid, gid) == NULL) {
        return -EIO;
    }
    return 0;
}

static int32_t
cib_ipc_dispatch_rw(qb_ipcs_connection_t * c, void *data, size_t size)
{
    pcmk__client_t *client = pcmk__find_client(c);

    crm_trace("%p message from %s", c, client->id);
    return cib_common_callback(c, data, size, TRUE);
}

static int32_t
cib_ipc_dispatch_ro(qb_ipcs_connection_t * c, void *data, size_t size)
{
    pcmk__client_t *client = pcmk__find_client(c);

    crm_trace("%p message from %s", c, client->id);
    return cib_common_callback(c, data, size, FALSE);
}

/* Error code means? */
static int32_t
cib_ipc_closed(qb_ipcs_connection_t * c)
{
    pcmk__client_t *client = pcmk__find_client(c);

    if (client == NULL) {
        return 0;
    }
    crm_trace("Connection %p", c);
    pcmk__free_client(client);
    return 0;
}

static void
cib_ipc_destroy(qb_ipcs_connection_t * c)
{
    crm_trace("Connection %p", c);
    cib_ipc_closed(c);
    if (cib_shutdown_flag) {
        cib_shutdown(0);
    }
}

struct qb_ipcs_service_handlers ipc_ro_callbacks = {
    .connection_accept = cib_ipc_accept,
    .connection_created = NULL,
    .msg_process = cib_ipc_dispatch_ro,
    .connection_closed = cib_ipc_closed,
    .connection_destroyed = cib_ipc_destroy
};

struct qb_ipcs_service_handlers ipc_rw_callbacks = {
    .connection_accept = cib_ipc_accept,
    .connection_created = NULL,
    .msg_process = cib_ipc_dispatch_rw,
    .connection_closed = cib_ipc_closed,
    .connection_destroyed = cib_ipc_destroy
};

void
cib_common_callback_worker(uint32_t id, uint32_t flags, xmlNode * op_request,
                           pcmk__client_t *cib_client, gboolean privileged)
{
    const char *op = crm_element_value(op_request, F_CIB_OPERATION);

    if (crm_str_eq(op, CRM_OP_REGISTER, TRUE)) {
        if (flags & crm_ipc_client_response) {
            xmlNode *ack = create_xml_node(NULL, __FUNCTION__);

            crm_xml_add(ack, F_CIB_OPERATION, CRM_OP_REGISTER);
            crm_xml_add(ack, F_CIB_CLIENTID, cib_client->id);
            pcmk__ipc_send_xml(cib_client, id, ack, flags);
            cib_client->request_id = 0;
            free_xml(ack);
        }
        return;

    } else if (crm_str_eq(op, T_CIB_NOTIFY, TRUE)) {
        /* Update the notify filters for this client */
        int on_off = 0;
        long long bit = 0;
        const char *type = crm_element_value(op_request, F_CIB_NOTIFY_TYPE);

        crm_element_value_int(op_request, F_CIB_NOTIFY_ACTIVATE, &on_off);

        crm_debug("Setting %s callbacks for %s (%s): %s",
                  type, cib_client->name, cib_client->id, on_off ? "on" : "off");

        if (safe_str_eq(type, T_CIB_POST_NOTIFY)) {
            bit = cib_notify_post;

        } else if (safe_str_eq(type, T_CIB_PRE_NOTIFY)) {
            bit = cib_notify_pre;

        } else if (safe_str_eq(type, T_CIB_UPDATE_CONFIRM)) {
            bit = cib_notify_confirm;

        } else if (safe_str_eq(type, T_CIB_DIFF_NOTIFY)) {
            bit = cib_notify_diff;

        } else if (safe_str_eq(type, T_CIB_REPLACE_NOTIFY)) {
            bit = cib_notify_replace;
        }

        if (on_off) {
            set_bit(cib_client->options, bit);
        } else {
            clear_bit(cib_client->options, bit);
        }

        if (flags & crm_ipc_client_response) {
            /* TODO - include rc */
            pcmk__ipc_send_ack(cib_client, id, flags, "ack");
        }
        return;
    }

    cib_process_request(op_request, FALSE, privileged, cib_client);
}

int32_t
cib_common_callback(qb_ipcs_connection_t * c, void *data, size_t size, gboolean privileged)
{
    uint32_t id = 0;
    uint32_t flags = 0;
    int call_options = 0;
    pcmk__client_t *cib_client = pcmk__find_client(c);
    xmlNode *op_request = pcmk__client_data2xml(cib_client, data, &id, &flags);

    if (op_request) {
        crm_element_value_int(op_request, F_CIB_CALLOPTS, &call_options);
    }

    if (op_request == NULL) {
        crm_trace("Invalid message from %p", c);
        pcmk__ipc_send_ack(cib_client, id, flags, "nack");
        return 0;

    } else if(cib_client == NULL) {
        crm_trace("Invalid client %p", c);
        return 0;
    }

    if (is_set(call_options, cib_sync_call)) {
        CRM_LOG_ASSERT(flags & crm_ipc_client_response);
        CRM_LOG_ASSERT(cib_client->request_id == 0);    /* This means the client has two synchronous events in-flight */
        cib_client->request_id = id;    /* Reply only to the last one */
    }

    if (cib_client->name == NULL) {
        const char *value = crm_element_value(op_request, F_CIB_CLIENTNAME);

        if (value == NULL) {
            cib_client->name = crm_itoa(cib_client->pid);
        } else {
            cib_client->name = strdup(value);
            if (crm_is_daemon_name(value)) {
                set_bit(cib_client->options, cib_is_daemon);
            }
        }
    }

    /* Allow cluster daemons more leeway before being evicted */
    if (is_set(cib_client->options, cib_is_daemon)) {
        const char *qmax = cib_config_lookup("cluster-ipc-limit");

        if (pcmk__set_client_queue_max(cib_client, qmax)) {
            crm_trace("IPC threshold for %s[%u] is now %u",
                      cib_client->name, cib_client->pid, cib_client->queue_max);
        }
    }

    crm_xml_add(op_request, F_CIB_CLIENTID, cib_client->id);
    crm_xml_add(op_request, F_CIB_CLIENTNAME, cib_client->name);

#if ENABLE_ACL
    CRM_LOG_ASSERT(cib_client->user != NULL);
    pcmk__update_acl_user(op_request, F_CIB_USER, cib_client->user);
#endif

    cib_common_callback_worker(id, flags, op_request, cib_client, privileged);
    free_xml(op_request);

    return 0;
}

static uint64_t ping_seq = 0;
static char *ping_digest = NULL;
static bool ping_modified_since = FALSE;
int sync_our_cib(xmlNode * request, gboolean all);

static gboolean
cib_digester_cb(gpointer data)
{
    if (cib_is_master) {
        char buffer[32];
        xmlNode *ping = create_xml_node(NULL, "ping");

        ping_seq++;
        free(ping_digest);
        ping_digest = NULL;
        ping_modified_since = FALSE;
        snprintf(buffer, 32, "%" U64T, ping_seq);
        crm_trace("Requesting peer digests (%s)", buffer);

        crm_xml_add(ping, F_TYPE, "cib");
        crm_xml_add(ping, F_CIB_OPERATION, CRM_OP_PING);
        crm_xml_add(ping, F_CIB_PING_ID, buffer);

        crm_xml_add(ping, XML_ATTR_CRM_VERSION, CRM_FEATURE_SET);
        send_cluster_message(NULL, crm_msg_cib, ping, TRUE);

        free_xml(ping);
    }
    return FALSE;
}

static void
process_ping_reply(xmlNode *reply) 
{
    uint64_t seq = 0;
    const char *host = crm_element_value(reply, F_ORIG);

    xmlNode *pong = get_message_xml(reply, F_CIB_CALLDATA);
    const char *seq_s = crm_element_value(pong, F_CIB_PING_ID);
    const char *digest = crm_element_value(pong, XML_ATTR_DIGEST);

    if (seq_s) {
        seq = (uint64_t) crm_parse_ll(seq_s, NULL);
    }

    if(digest == NULL) {
        crm_trace("Ignoring ping reply %s from %s with no digest", seq_s, host);

    } else if(seq != ping_seq) {
        crm_trace("Ignoring out of sequence ping reply %s from %s", seq_s, host);

    } else if(ping_modified_since) {
        crm_trace("Ignoring ping reply %s from %s: cib updated since", seq_s, host);

    } else {
        const char *version = crm_element_value(pong, XML_ATTR_CRM_VERSION);

        if(ping_digest == NULL) {
            crm_trace("Calculating new digest");
            ping_digest = calculate_xml_versioned_digest(the_cib, FALSE, TRUE, version);
        }

        crm_trace("Processing ping reply %s from %s (%s)", seq_s, host, digest);
        if(safe_str_eq(ping_digest, digest) == FALSE) {
            xmlNode *remote_cib = get_message_xml(pong, F_CIB_CALLDATA);

            crm_notice("Local CIB %s.%s.%s.%s differs from %s: %s.%s.%s.%s %p",
                       crm_element_value(the_cib, XML_ATTR_GENERATION_ADMIN),
                       crm_element_value(the_cib, XML_ATTR_GENERATION),
                       crm_element_value(the_cib, XML_ATTR_NUMUPDATES),
                       ping_digest, host,
                       remote_cib?crm_element_value(remote_cib, XML_ATTR_GENERATION_ADMIN):"_",
                       remote_cib?crm_element_value(remote_cib, XML_ATTR_GENERATION):"_",
                       remote_cib?crm_element_value(remote_cib, XML_ATTR_NUMUPDATES):"_",
                       digest, remote_cib);

            if(remote_cib && remote_cib->children) {
                /* Additional debug */
                xml_calculate_changes(the_cib, remote_cib);
                xml_log_changes(LOG_INFO, __FUNCTION__, remote_cib);
                crm_trace("End of differences");
            }

            free_xml(remote_cib);
            sync_our_cib(reply, FALSE);
        }
    }
}

static void
do_local_notify(xmlNode * notify_src, const char *client_id,
                gboolean sync_reply, gboolean from_peer)
{
    int rid = 0;
    int call_id = 0;
    pcmk__client_t *client_obj = NULL;

    CRM_ASSERT(notify_src && client_id);

    crm_element_value_int(notify_src, F_CIB_CALLID, &call_id);

    client_obj = pcmk__find_client_by_id(client_id);
    if (client_obj == NULL) {
        crm_debug("Could not send response %d: client %s not found",
                  call_id, client_id);
        return;
    }

    if (sync_reply) {
        if (client_obj->ipcs) {
            CRM_LOG_ASSERT(client_obj->request_id);

            rid = client_obj->request_id;
            client_obj->request_id = 0;

            crm_trace("Sending response %d to %s %s",
                      rid, client_obj->name,
                      from_peer ? "(originator of delegated request)" : "");
        } else {
            crm_trace("Sending response [call %d] to %s %s",
                      call_id, client_obj->name, from_peer ? "(originator of delegated request)" : "");
        }

    } else {
        crm_trace("Sending event %d to %s %s",
                  call_id, client_obj->name, from_peer ? "(originator of delegated request)" : "");
    }

    switch (client_obj->kind) {
        case PCMK__CLIENT_IPC:
            {
                int rc = pcmk__ipc_send_xml(client_obj, rid, notify_src,
                                            (sync_reply? crm_ipc_flags_none
                                             : crm_ipc_server_event));

                if (rc != pcmk_rc_ok) {
                    crm_warn("%s reply to %s failed: %s " CRM_XS " rc=%d",
                             (sync_reply? "Synchronous" : "Asynchronous"),
                             client_obj->name, pcmk_rc_str(rc), rc);
                }
            }
            break;
#ifdef HAVE_GNUTLS_GNUTLS_H
        case PCMK__CLIENT_TLS:
#endif
        case PCMK__CLIENT_TCP:
            pcmk__remote_send_xml(client_obj->remote, notify_src);
            break;
        default:
            crm_err("Unknown transport %d for %s", client_obj->kind, client_obj->name);
    }
}

static void
local_notify_destroy_callback(gpointer data)
{
    cib_local_notify_t *notify = data;

    free_xml(notify->notify_src);
    free(notify->client_id);
    free(notify);
}

static void
check_local_notify(int bcast_id)
{
    cib_local_notify_t *notify = NULL;

    if (!local_notify_queue) {
        return;
    }

    notify = g_hash_table_lookup(local_notify_queue, GINT_TO_POINTER(bcast_id));

    if (notify) {
        do_local_notify(notify->notify_src, notify->client_id, notify->sync_reply,
                        notify->from_peer);
        g_hash_table_remove(local_notify_queue, GINT_TO_POINTER(bcast_id));
    }
}

static void
queue_local_notify(xmlNode * notify_src, const char *client_id, gboolean sync_reply,
                   gboolean from_peer)
{
    cib_local_notify_t *notify = calloc(1, sizeof(cib_local_notify_t));

    notify->notify_src = notify_src;
    notify->client_id = strdup(client_id);
    notify->sync_reply = sync_reply;
    notify->from_peer = from_peer;

    if (!local_notify_queue) {
        local_notify_queue = g_hash_table_new_full(g_direct_hash,
                                                   g_direct_equal, NULL,
                                                   local_notify_destroy_callback);
    }

    g_hash_table_insert(local_notify_queue, GINT_TO_POINTER(cib_local_bcast_num), notify);
    // cppcheck doesn't know notify will get freed when hash table is destroyed
    // cppcheck-suppress memleak
}

static void
parse_local_options_v1(pcmk__client_t *cib_client, int call_type,
                       int call_options, const char *host, const char *op,
                       gboolean *local_notify, gboolean *needs_reply,
                       gboolean *process, gboolean *needs_forward)
{
    if (cib_op_modifies(call_type)
        && !(call_options & cib_inhibit_bcast)) {
        /* we need to send an update anyway */
        *needs_reply = TRUE;
    } else {
        *needs_reply = FALSE;
    }

    if (host == NULL && (call_options & cib_scope_local)) {
        crm_trace("Processing locally scoped %s op from %s", op, cib_client->name);
        *local_notify = TRUE;

    } else if (host == NULL && cib_is_master) {
        crm_trace("Processing master %s op locally from %s", op, cib_client->name);
        *local_notify = TRUE;

    } else if (safe_str_eq(host, cib_our_uname)) {
        crm_trace("Processing locally addressed %s op from %s", op, cib_client->name);
        *local_notify = TRUE;

    } else if (stand_alone) {
        *needs_forward = FALSE;
        *local_notify = TRUE;
        *process = TRUE;

    } else {
        crm_trace("%s op from %s needs to be forwarded to %s",
                  op, cib_client->name, host ? host : "the master instance");
        *needs_forward = TRUE;
        *process = FALSE;
    }
}

static void
parse_local_options_v2(pcmk__client_t *cib_client, int call_type,
                       int call_options, const char *host, const char *op,
                       gboolean *local_notify, gboolean *needs_reply,
                       gboolean *process, gboolean *needs_forward)
{
    if (cib_op_modifies(call_type)) {
        if(safe_str_eq(op, CIB_OP_MASTER) || safe_str_eq(op, CIB_OP_SLAVE)) {
            /* Always handle these locally */
            *process = TRUE;
            *needs_reply = FALSE;
            *local_notify = TRUE;
            *needs_forward = FALSE;
            return;

        } else {
            /* Redirect all other updates via CPG */
            *needs_reply = TRUE;
            *needs_forward = TRUE;
            *process = FALSE;
            crm_trace("%s op from %s needs to be forwarded to %s",
                      op, cib_client->name, host ? host : "the master instance");
            return;
        }
    }


    *process = TRUE;
    *needs_reply = FALSE;
    *local_notify = TRUE;
    *needs_forward = FALSE;

    if (stand_alone) {
        crm_trace("Processing %s op from %s (stand-alone)", op, cib_client->name);

    } else if (host == NULL) {
        crm_trace("Processing unaddressed %s op from %s", op, cib_client->name);

    } else if (safe_str_eq(host, cib_our_uname)) {
        crm_trace("Processing locally addressed %s op from %s", op, cib_client->name);

    } else {
        crm_trace("%s op from %s needs to be forwarded to %s", op, cib_client->name, host);
        *needs_forward = TRUE;
        *process = FALSE;
    }
}

static void
parse_local_options(pcmk__client_t *cib_client, int call_type,
                    int call_options, const char *host, const char *op,
                    gboolean *local_notify, gboolean *needs_reply,
                    gboolean *process, gboolean *needs_forward)
{
    if(cib_legacy_mode()) {
        parse_local_options_v1(cib_client, call_type, call_options, host,
                               op, local_notify, needs_reply, process, needs_forward);
    } else {
        parse_local_options_v2(cib_client, call_type, call_options, host,
                               op, local_notify, needs_reply, process, needs_forward);
    }
}

static gboolean
parse_peer_options_v1(int call_type, xmlNode * request,
                   gboolean * local_notify, gboolean * needs_reply, gboolean * process,
                   gboolean * needs_forward)
{
    const char *op = NULL;
    const char *host = NULL;
    const char *delegated = NULL;
    const char *originator = crm_element_value(request, F_ORIG);
    const char *reply_to = crm_element_value(request, F_CIB_ISREPLY);
    const char *update = crm_element_value(request, F_CIB_GLOBAL_UPDATE);

    gboolean is_reply = safe_str_eq(reply_to, cib_our_uname);

    if (crm_is_true(update)) {
        *needs_reply = FALSE;
        if (is_reply) {
            *local_notify = TRUE;
            crm_trace("Processing global/peer update from %s"
                      " that originated from us", originator);
        } else {
            crm_trace("Processing global/peer update from %s", originator);
        }
        return TRUE;
    }

    op = crm_element_value(request, F_CIB_OPERATION);
    crm_trace("Processing %s request sent by %s", op, originator);
    if (safe_str_eq(op, "cib_shutdown_req")) {
        /* Always process these */
        *local_notify = FALSE;
        if (reply_to == NULL || is_reply) {
            *process = TRUE;
        }
        if (is_reply) {
            *needs_reply = FALSE;
        }
        return *process;
    }

    if (is_reply && safe_str_eq(op, CRM_OP_PING)) {
        process_ping_reply(request);
        return FALSE;
    }

    if (is_reply) {
        crm_trace("Forward reply sent from %s to local clients", originator);
        *process = FALSE;
        *needs_reply = FALSE;
        *local_notify = TRUE;
        return TRUE;
    }

    host = crm_element_value(request, F_CIB_HOST);
    if (host != NULL && safe_str_eq(host, cib_our_uname)) {
        crm_trace("Processing %s request sent to us from %s", op, originator);
        return TRUE;

    } else if(is_reply == FALSE && safe_str_eq(op, CRM_OP_PING)) {
        crm_trace("Processing %s request sent to %s by %s", op, host?host:"everyone", originator);
        *needs_reply = TRUE;
        return TRUE;

    } else if (host == NULL && cib_is_master == TRUE) {
        crm_trace("Processing %s request sent to master instance from %s", op, originator);
        return TRUE;
    }

    delegated = crm_element_value(request, F_CIB_DELEGATED);
    if (delegated != NULL) {
        crm_trace("Ignoring msg for master instance");

    } else if (host != NULL) {
        /* this is for a specific instance and we're not it */
        crm_trace("Ignoring msg for instance on %s", crm_str(host));

    } else if (reply_to == NULL && cib_is_master == FALSE) {
        /* this is for the master instance and we're not it */
        crm_trace("Ignoring reply to %s", crm_str(reply_to));

    } else if (safe_str_eq(op, "cib_shutdown_req")) {
        if (reply_to != NULL) {
            crm_debug("Processing %s from %s", op, originator);
            *needs_reply = FALSE;

        } else {
            crm_debug("Processing %s reply from %s", op, originator);
        }
        return TRUE;

    } else {
        crm_err("Nothing for us to do?");
        crm_log_xml_err(request, "Peer[inbound]");
    }

    return FALSE;
}

static gboolean
parse_peer_options_v2(int call_type, xmlNode * request,
                   gboolean * local_notify, gboolean * needs_reply, gboolean * process,
                   gboolean * needs_forward)
{
    const char *host = NULL;
    const char *delegated = crm_element_value(request, F_CIB_DELEGATED);
    const char *op = crm_element_value(request, F_CIB_OPERATION);
    const char *originator = crm_element_value(request, F_ORIG);
    const char *reply_to = crm_element_value(request, F_CIB_ISREPLY);
    const char *update = crm_element_value(request, F_CIB_GLOBAL_UPDATE);

    gboolean is_reply = safe_str_eq(reply_to, cib_our_uname);

    if(safe_str_eq(op, CIB_OP_REPLACE)) {
        /* sync_our_cib() sets F_CIB_ISREPLY */
        if (reply_to) {
            delegated = reply_to;
        }
        goto skip_is_reply;

    } else if(safe_str_eq(op, CIB_OP_SYNC)) {

    } else if (is_reply && safe_str_eq(op, CRM_OP_PING)) {
        process_ping_reply(request);
        return FALSE;

    } else if (safe_str_eq(op, CIB_OP_UPGRADE)) {
        /* Only the DC (node with the oldest software) should process
         * this operation if F_CIB_SCHEMA_MAX is unset
         *
         * If the DC is happy it will then send out another
         * CIB_OP_UPGRADE which will tell all nodes to do the actual
         * upgrade.
         *
         * Except this time F_CIB_SCHEMA_MAX will be set which puts a
         * limit on how far newer nodes will go
         */
        const char *max = crm_element_value(request, F_CIB_SCHEMA_MAX);
        const char *upgrade_rc = crm_element_value(request, F_CIB_UPGRADE_RC);

        crm_trace("Parsing %s operation%s for %s with max=%s and upgrade_rc=%s",
                  op, (is_reply? " reply" : ""),
                  (cib_is_master? "master" : "slave"),
                  (max? max : "none"), (upgrade_rc? upgrade_rc : "none"));

        if (upgrade_rc != NULL) {
            // Our upgrade request was rejected by DC, notify clients of result
            crm_xml_add(request, F_CIB_RC, upgrade_rc);

        } else if ((max == NULL) && cib_is_master) {
            /* We are the DC, check if this upgrade is allowed */
            goto skip_is_reply;

        } else if(max) {
            /* Ok, go ahead and upgrade to 'max' */
            goto skip_is_reply;

        } else {
            // Ignore broadcast client requests when we're not DC
            return FALSE;
        }

    } else if (crm_is_true(update)) {
        crm_info("Detected legacy %s global update from %s", op, originator);
        send_sync_request(NULL);
        legacy_mode = TRUE;
        return FALSE;

    } else if (is_reply && cib_op_modifies(call_type)) {
        crm_trace("Ignoring legacy %s reply sent from %s to local clients", op, originator);
        return FALSE;

    } else if (safe_str_eq(op, "cib_shutdown_req")) {
        /* Legacy handling */
        crm_debug("Legacy handling of %s message from %s", op, originator);
        *local_notify = FALSE;
        if (reply_to == NULL) {
            *process = TRUE;
        }
        return *process;
    }

    if(is_reply) {
        crm_trace("Handling %s reply sent from %s to local clients", op, originator);
        *process = FALSE;
        *needs_reply = FALSE;
        *local_notify = TRUE;
        return TRUE;
    }

  skip_is_reply:
    *process = TRUE;
    *needs_reply = FALSE;

    if(safe_str_eq(delegated, cib_our_uname)) {
        *local_notify = TRUE;
    } else {
        *local_notify = FALSE;
    }

    host = crm_element_value(request, F_CIB_HOST);
    if (host != NULL && safe_str_eq(host, cib_our_uname)) {
        crm_trace("Processing %s request sent to us from %s", op, originator);
        *needs_reply = TRUE;
        return TRUE;

    } else if (host != NULL) {
        /* this is for a specific instance and we're not it */
        crm_trace("Ignoring %s operation for instance on %s", op, crm_str(host));
        return FALSE;

    } else if(is_reply == FALSE && safe_str_eq(op, CRM_OP_PING)) {
        *needs_reply = TRUE;
    }

    crm_trace("Processing %s request sent to everyone by %s/%s on %s %s", op,
              crm_element_value(request, F_CIB_CLIENTNAME),
              crm_element_value(request, F_CIB_CALLID),
              originator, (*local_notify)?"(notify)":"");
    return TRUE;
}

static gboolean
parse_peer_options(int call_type, xmlNode * request,
                   gboolean * local_notify, gboolean * needs_reply, gboolean * process,
                   gboolean * needs_forward)
{
    /* TODO: What happens when an update comes in after node A
     * requests the CIB from node B, but before it gets the reply (and
     * sends out the replace operation)
     */
    if(cib_legacy_mode()) {
        return parse_peer_options_v1(
            call_type, request, local_notify, needs_reply, process, needs_forward);
    } else {
        return parse_peer_options_v2(
            call_type, request, local_notify, needs_reply, process, needs_forward);
    }
}

static void
forward_request(xmlNode * request, pcmk__client_t *cib_client, int call_options)
{
    const char *op = crm_element_value(request, F_CIB_OPERATION);
    const char *host = crm_element_value(request, F_CIB_HOST);

    crm_xml_add(request, F_CIB_DELEGATED, cib_our_uname);

    if (host != NULL) {
        crm_trace("Forwarding %s op to %s", op, host);
        send_cluster_message(crm_get_peer(0, host), crm_msg_cib, request, FALSE);

    } else {
        crm_trace("Forwarding %s op to master instance", op);
        send_cluster_message(NULL, crm_msg_cib, request, FALSE);
    }

    /* Return the request to its original state */
    xml_remove_prop(request, F_CIB_DELEGATED);

    if (call_options & cib_discard_reply) {
        crm_trace("Client not interested in reply");
    }
}

static gboolean
send_peer_reply(xmlNode * msg, xmlNode * result_diff, const char *originator, gboolean broadcast)
{
    CRM_ASSERT(msg != NULL);

    if (broadcast) {
        /* this (successful) call modified the CIB _and_ the
         * change needs to be broadcast...
         *   send via HA to other nodes
         */
        int diff_add_updates = 0;
        int diff_add_epoch = 0;
        int diff_add_admin_epoch = 0;

        int diff_del_updates = 0;
        int diff_del_epoch = 0;
        int diff_del_admin_epoch = 0;

        const char *digest = NULL;
        int format = 1;

        CRM_LOG_ASSERT(result_diff != NULL);
        digest = crm_element_value(result_diff, XML_ATTR_DIGEST);
        crm_element_value_int(result_diff, "format", &format);

        cib_diff_version_details(result_diff,
                                 &diff_add_admin_epoch, &diff_add_epoch, &diff_add_updates,
                                 &diff_del_admin_epoch, &diff_del_epoch, &diff_del_updates);

        crm_trace("Sending update diff %d.%d.%d -> %d.%d.%d %s",
                  diff_del_admin_epoch, diff_del_epoch, diff_del_updates,
                  diff_add_admin_epoch, diff_add_epoch, diff_add_updates, digest);

        crm_xml_add(msg, F_CIB_ISREPLY, originator);
        crm_xml_add(msg, F_CIB_GLOBAL_UPDATE, XML_BOOLEAN_TRUE);
        crm_xml_add(msg, F_CIB_OPERATION, CIB_OP_APPLY_DIFF);
        crm_xml_add(msg, F_CIB_USER, CRM_DAEMON_USER);

        if (format == 1) {
            CRM_ASSERT(digest != NULL);
        }

        add_message_xml(msg, F_CIB_UPDATE_DIFF, result_diff);
        crm_log_xml_explicit(msg, "copy");
        return send_cluster_message(NULL, crm_msg_cib, msg, TRUE);

    } else if (originator != NULL) {
        /* send reply via HA to originating node */
        crm_trace("Sending request result to %s only", originator);
        crm_xml_add(msg, F_CIB_ISREPLY, originator);
        return send_cluster_message(crm_get_peer(0, originator), crm_msg_cib, msg, FALSE);
    }

    return FALSE;
}

static void
cib_process_request(xmlNode *request, gboolean force_synchronous,
                    gboolean privileged, pcmk__client_t *cib_client)
{
    int call_type = 0;
    int call_options = 0;

    gboolean process = TRUE;
    gboolean is_update = TRUE;
    gboolean from_peer = TRUE;
    gboolean needs_reply = TRUE;
    gboolean local_notify = FALSE;
    gboolean needs_forward = FALSE;
    gboolean global_update = crm_is_true(crm_element_value(request, F_CIB_GLOBAL_UPDATE));

    xmlNode *op_reply = NULL;
    xmlNode *result_diff = NULL;

    int rc = pcmk_ok;
    const char *op = crm_element_value(request, F_CIB_OPERATION);
    const char *originator = crm_element_value(request, F_ORIG);
    const char *host = crm_element_value(request, F_CIB_HOST);
    const char *target = NULL;
    const char *call_id = crm_element_value(request, F_CIB_CALLID);
    const char *client_id = crm_element_value(request, F_CIB_CLIENTID);
    const char *client_name = crm_element_value(request, F_CIB_CLIENTNAME);
    const char *reply_to = crm_element_value(request, F_CIB_ISREPLY);

    if (cib_client) {
        from_peer = FALSE;
    }

    crm_element_value_int(request, F_CIB_CALLOPTS, &call_options);
    if (force_synchronous) {
        call_options |= cib_sync_call;
    }

    if (host != NULL && strlen(host) == 0) {
        host = NULL;
    }

    if (host) {
        target = host;

    } else if (call_options & cib_scope_local) {
        target = "local host";

    } else {
        target = "master";
    }

    if (from_peer) {
        crm_trace("Processing peer %s operation from %s/%s on %s intended for %s (reply=%s)",
                  op, client_name, call_id, originator, target, reply_to);
    } else {
        crm_xml_add(request, F_ORIG, cib_our_uname);
        crm_trace("Processing local %s operation from %s/%s intended for %s", op, client_name, call_id, target);
    }

    rc = cib_get_operation_id(op, &call_type);
    if (rc != pcmk_ok) {
        /* TODO: construct error reply? */
        crm_err("Pre-processing of command failed: %s", pcmk_strerror(rc));
        return;
    }

    if (from_peer == FALSE) {
        parse_local_options(cib_client, call_type, call_options, host, op,
                            &local_notify, &needs_reply, &process, &needs_forward);

    } else if (parse_peer_options(call_type, request, &local_notify,
                                  &needs_reply, &process, &needs_forward) == FALSE) {
        return;
    }

    is_update = cib_op_modifies(call_type);

    if (call_options & cib_discard_reply) {
        needs_reply = is_update;
        local_notify = FALSE;
    }

    if (needs_forward) {
        const char *host = crm_element_value(request, F_CIB_HOST);
        const char *section = crm_element_value(request, F_CIB_SECTION);
        int log_level = LOG_INFO;

        if (safe_str_eq(op, CRM_OP_NOOP)) {
            log_level = LOG_DEBUG;
        }

        do_crm_log(log_level,
                   "Forwarding %s operation for section %s to %s (origin=%s/%s/%s)",
                   op,
                   section ? section : "'all'",
                   host ? host : cib_legacy_mode() ? "master" : "all",
                   originator ? originator : "local",
                   client_name, call_id);

        forward_request(request, cib_client, call_options);
        return;
    }

    if (cib_status != pcmk_ok) {
        const char *call = crm_element_value(request, F_CIB_CALLID);

        rc = cib_status;
        crm_err("Operation ignored, cluster configuration is invalid."
                " Please repair and restart: %s", pcmk_strerror(cib_status));

        op_reply = create_xml_node(NULL, "cib-reply");
        crm_xml_add(op_reply, F_TYPE, T_CIB);
        crm_xml_add(op_reply, F_CIB_OPERATION, op);
        crm_xml_add(op_reply, F_CIB_CALLID, call);
        crm_xml_add(op_reply, F_CIB_CLIENTID, client_id);
        crm_xml_add_int(op_reply, F_CIB_CALLOPTS, call_options);
        crm_xml_add_int(op_reply, F_CIB_RC, rc);

        crm_trace("Attaching reply output");
        add_message_xml(op_reply, F_CIB_CALLDATA, the_cib);

        crm_log_xml_explicit(op_reply, "cib:reply");

    } else if (process) {
        time_t finished = 0;

        time_t now = time(NULL);
        int level = LOG_INFO;
        const char *section = crm_element_value(request, F_CIB_SECTION);

        rc = cib_process_command(request, &op_reply, &result_diff, privileged);

        if (is_update == FALSE) {
            level = LOG_TRACE;

        } else if (global_update) {
            switch (rc) {
                case pcmk_ok:
                    level = LOG_INFO;
                    break;
                case -pcmk_err_old_data:
                case -pcmk_err_diff_resync:
                case -pcmk_err_diff_failed:
                    level = LOG_TRACE;
                    break;
                default:
                    level = LOG_ERR;
            }

        } else if (rc != pcmk_ok && is_update) {
            level = LOG_WARNING;
        }

        do_crm_log(level,
                   "Completed %s operation for section %s: %s (rc=%d, origin=%s/%s/%s, version=%s.%s.%s)",
                   op, section ? section : "'all'", pcmk_strerror(rc), rc,
                   originator ? originator : "local", client_name, call_id,
                   the_cib ? crm_element_value(the_cib, XML_ATTR_GENERATION_ADMIN) : "0",
                   the_cib ? crm_element_value(the_cib, XML_ATTR_GENERATION) : "0",
                   the_cib ? crm_element_value(the_cib, XML_ATTR_NUMUPDATES) : "0");

        finished = time(NULL);
        if ((finished - now) > 3) {
            crm_trace("%s operation took %lds to complete", op, (long)(finished - now));
            crm_write_blackbox(0, NULL);
        }

        if (op_reply == NULL && (needs_reply || local_notify)) {
            crm_err("Unexpected NULL reply to message");
            crm_log_xml_err(request, "null reply");
            needs_reply = FALSE;
            local_notify = FALSE;
        }
    }

    /* from now on we are the server */
    if(is_update && cib_legacy_mode() == FALSE) {
        crm_trace("Completed pre-sync update from %s/%s/%s%s",
                  originator ? originator : "local", client_name, call_id,
                  local_notify?" with local notification":"");

    } else if (needs_reply == FALSE || stand_alone) {
        /* nothing more to do...
         * this was a non-originating slave update
         */
        crm_trace("Completed slave update");

    } else if (cib_legacy_mode() &&
               rc == pcmk_ok && result_diff != NULL && !(call_options & cib_inhibit_bcast)) {
        gboolean broadcast = FALSE;

        cib_local_bcast_num++;
        crm_xml_add_int(request, F_CIB_LOCAL_NOTIFY_ID, cib_local_bcast_num);
        broadcast = send_peer_reply(request, result_diff, originator, TRUE);

        if (broadcast && client_id && local_notify && op_reply) {

            /* If we have been asked to sync the reply,
             * and a bcast msg has gone out, we queue the local notify
             * until we know the bcast message has been received */
            local_notify = FALSE;
            crm_trace("Queuing local %ssync notification for %s",
                      (call_options & cib_sync_call) ? "" : "a-", client_id);

            queue_local_notify(op_reply, client_id, (call_options & cib_sync_call), from_peer);
            op_reply = NULL;    /* the reply is queued, so don't free here */
        }

    } else if (call_options & cib_discard_reply) {
        crm_trace("Caller isn't interested in reply");

    } else if (from_peer) {
        if (is_update == FALSE || result_diff == NULL) {
            crm_trace("Request not broadcast: R/O call");

        } else if (call_options & cib_inhibit_bcast) {
            crm_trace("Request not broadcast: inhibited");

        } else if (rc != pcmk_ok) {
            crm_trace("Request not broadcast: call failed: %s", pcmk_strerror(rc));

        } else {
            crm_trace("Directing reply to %s", originator);
        }

        send_peer_reply(op_reply, result_diff, originator, FALSE);
    }

    if (local_notify && client_id) {
        crm_trace("Performing local %ssync notification for %s",
                  (call_options & cib_sync_call) ? "" : "a-", client_id);
        if (process == FALSE) {
            do_local_notify(request, client_id, call_options & cib_sync_call, from_peer);
        } else {
            do_local_notify(op_reply, client_id, call_options & cib_sync_call, from_peer);
        }
    }

    free_xml(op_reply);
    free_xml(result_diff);

    return;
}

static int
cib_process_command(xmlNode * request, xmlNode ** reply, xmlNode ** cib_diff, gboolean privileged)
{
    xmlNode *input = NULL;
    xmlNode *output = NULL;
    xmlNode *result_cib = NULL;
    xmlNode *current_cib = NULL;

    int call_type = 0;
    int call_options = 0;

    const char *op = NULL;
    const char *section = NULL;
    const char *call_id = crm_element_value(request, F_CIB_CALLID);

    int rc = pcmk_ok;
    int rc2 = pcmk_ok;

    gboolean send_r_notify = FALSE;
    gboolean global_update = FALSE;
    gboolean config_changed = FALSE;
    gboolean manage_counters = TRUE;

    static mainloop_timer_t *digest_timer = NULL;

    CRM_ASSERT(cib_status == pcmk_ok);

    if(digest_timer == NULL) {
        digest_timer = mainloop_timer_add("digester", 5000, FALSE, cib_digester_cb, NULL);
    }

    *reply = NULL;
    *cib_diff = NULL;
    current_cib = the_cib;

    /* Start processing the request... */
    op = crm_element_value(request, F_CIB_OPERATION);
    crm_element_value_int(request, F_CIB_CALLOPTS, &call_options);
    rc = cib_get_operation_id(op, &call_type);

    if (rc == pcmk_ok && privileged == FALSE) {
        rc = cib_op_can_run(call_type, call_options, privileged, global_update);
    }

    rc2 = cib_op_prepare(call_type, request, &input, &section);
    if (rc == pcmk_ok) {
        rc = rc2;
    }

    if (rc != pcmk_ok) {
        crm_trace("Call setup failed: %s", pcmk_strerror(rc));
        goto done;

    } else if (cib_op_modifies(call_type) == FALSE) {
        rc = cib_perform_op(op, call_options, cib_op_func(call_type), TRUE,
                            section, request, input, FALSE, &config_changed,
                            current_cib, &result_cib, NULL, &output);

        CRM_CHECK(result_cib == NULL, free_xml(result_cib));
        goto done;
    }

    /* Handle a valid write action */
    global_update = crm_is_true(crm_element_value(request, F_CIB_GLOBAL_UPDATE));
    if (global_update) {
        /* legacy code */
        manage_counters = FALSE;
        call_options |= cib_force_diff;
        crm_trace("Global update detected");

        CRM_CHECK(call_type == 3 || call_type == 4, crm_err("Call type: %d", call_type);
                  crm_log_xml_err(request, "bad op"));
    }

    if (rc == pcmk_ok) {
        ping_modified_since = TRUE;
        if (call_options & cib_inhibit_bcast) {
            /* skip */
            crm_trace("Skipping update: inhibit broadcast");
            manage_counters = FALSE;
        }

        if (is_not_set(call_options, cib_dryrun) && safe_str_eq(section, XML_CIB_TAG_STATUS)) {
            /* Copying large CIBs accounts for a huge percentage of our CIB usage */
            call_options |= cib_zero_copy;
        } else {
            clear_bit(call_options, cib_zero_copy);
        }

        /* result_cib must not be modified after cib_perform_op() returns */
        rc = cib_perform_op(op, call_options, cib_op_func(call_type), FALSE,
                            section, request, input, manage_counters, &config_changed,
                            current_cib, &result_cib, cib_diff, &output);

        if (manage_counters == FALSE) {
            int format = 1;
            /* Legacy code
             * If the diff is NULL at this point, it's because nothing changed
             */
            if (*cib_diff) {
                crm_element_value_int(*cib_diff, "format", &format);
            }

            if (format == 1) {
                config_changed = cib_config_changed(NULL, NULL, cib_diff);
            }
        }

        /* Always write to disk for replace ops,
         * this also negates the need to detect ordering changes
         */
        if (crm_str_eq(CIB_OP_REPLACE, op, TRUE)) {
            config_changed = TRUE;
        }
    }

    if (rc == pcmk_ok && is_not_set(call_options, cib_dryrun)) {
        crm_trace("Activating %s->%s%s%s",
                  crm_element_value(current_cib, XML_ATTR_NUMUPDATES),
                  crm_element_value(result_cib, XML_ATTR_NUMUPDATES),
                  (is_set(call_options, cib_zero_copy)? " zero-copy" : ""),
                  (config_changed? " changed" : ""));
        if(is_not_set(call_options, cib_zero_copy)) {
            rc = activateCibXml(result_cib, config_changed, op);
            crm_trace("Activated %s (%d)",
                      crm_element_value(current_cib, XML_ATTR_NUMUPDATES), rc);
        }

        if (rc == pcmk_ok && cib_internal_config_changed(*cib_diff)) {
            cib_read_config(config_hash, result_cib);
        }

        if (crm_str_eq(CIB_OP_REPLACE, op, TRUE)) {
            if (section == NULL) {
                send_r_notify = TRUE;

            } else if (safe_str_eq(section, XML_TAG_CIB)) {
                send_r_notify = TRUE;

            } else if (safe_str_eq(section, XML_CIB_TAG_NODES)) {
                send_r_notify = TRUE;

            } else if (safe_str_eq(section, XML_CIB_TAG_STATUS)) {
                send_r_notify = TRUE;
            }

        } else if (crm_str_eq(CIB_OP_ERASE, op, TRUE)) {
            send_r_notify = TRUE;
        }

        mainloop_timer_stop(digest_timer);
        mainloop_timer_start(digest_timer);

    } else if (rc == -pcmk_err_schema_validation) {
        CRM_ASSERT(is_not_set(call_options, cib_zero_copy));

        if (output != NULL) {
            crm_log_xml_info(output, "cib:output");
            free_xml(output);
        }

        output = result_cib;

    } else {
        crm_trace("Not activating %d %d %s", rc, is_set(call_options, cib_dryrun), crm_element_value(result_cib, XML_ATTR_NUMUPDATES));
        if(is_not_set(call_options, cib_zero_copy)) {
            free_xml(result_cib);
        }
    }

    if ((call_options & (cib_inhibit_notify|cib_dryrun)) == 0) {
        const char *client = crm_element_value(request, F_CIB_CLIENTNAME);

        crm_trace("Sending notifications %d", is_set(call_options, cib_dryrun));
        cib_diff_notify(call_options, client, call_id, op, input, rc, *cib_diff);
    }

    if (send_r_notify) {
        const char *origin = crm_element_value(request, F_ORIG);

        cib_replace_notify(origin, the_cib, rc, *cib_diff);
    }

    xml_log_patchset(LOG_TRACE, "cib:diff", *cib_diff);
  done:
    if ((call_options & cib_discard_reply) == 0) {
        const char *caller = crm_element_value(request, F_CIB_CLIENTID);

        *reply = create_xml_node(NULL, "cib-reply");
        crm_xml_add(*reply, F_TYPE, T_CIB);
        crm_xml_add(*reply, F_CIB_OPERATION, op);
        crm_xml_add(*reply, F_CIB_CALLID, call_id);
        crm_xml_add(*reply, F_CIB_CLIENTID, caller);
        crm_xml_add_int(*reply, F_CIB_CALLOPTS, call_options);
        crm_xml_add_int(*reply, F_CIB_RC, rc);

        if (output != NULL) {
            crm_trace("Attaching reply output");
            add_message_xml(*reply, F_CIB_CALLDATA, output);
        }

        crm_log_xml_explicit(*reply, "cib:reply");
    }

    crm_trace("cleanup");

    if (cib_op_modifies(call_type) == FALSE && output != current_cib) {
        free_xml(output);
        output = NULL;
    }

    if (call_type >= 0) {
        cib_op_cleanup(call_type, call_options, &input, &output);
    }

    crm_trace("done");
    return rc;
}

void
cib_peer_callback(xmlNode * msg, void *private_data)
{
    const char *reason = NULL;
    const char *originator = crm_element_value(msg, F_ORIG);

    if (cib_legacy_mode() && (originator == NULL || crm_str_eq(originator, cib_our_uname, TRUE))) {
        /* message is from ourselves */
        int bcast_id = 0;

        if (!(crm_element_value_int(msg, F_CIB_LOCAL_NOTIFY_ID, &bcast_id))) {
            check_local_notify(bcast_id);
        }
        return;

    } else if (crm_peer_cache == NULL) {
        reason = "membership not established";
        goto bail;
    }

    if (crm_element_value(msg, F_CIB_CLIENTNAME) == NULL) {
        crm_xml_add(msg, F_CIB_CLIENTNAME, originator);
    }

    /* crm_log_xml_trace("Peer[inbound]", msg); */
    cib_process_request(msg, FALSE, TRUE, NULL);
    return;

  bail:
    if (reason) {
        const char *seq = crm_element_value(msg, F_SEQ);
        const char *op = crm_element_value(msg, F_CIB_OPERATION);

        crm_warn("Discarding %s message (%s) from %s: %s", op, seq, originator, reason);
    }
}

static gboolean
cib_force_exit(gpointer data)
{
    crm_notice("Forcing exit!");
    terminate_cib(__FUNCTION__, CRM_EX_ERROR);
    return FALSE;
}

static void
disconnect_remote_client(gpointer key, gpointer value, gpointer user_data)
{
    pcmk__client_t *a_client = value;

    crm_err("Disconnecting %s... Not implemented", crm_str(a_client->name));
}

void
cib_shutdown(int nsig)
{
    struct qb_ipcs_stats srv_stats;

    if (cib_shutdown_flag == FALSE) {
        int disconnects = 0;
        qb_ipcs_connection_t *c = NULL;

        cib_shutdown_flag = TRUE;

        c = qb_ipcs_connection_first_get(ipcs_rw);
        while (c != NULL) {
            qb_ipcs_connection_t *last = c;

            c = qb_ipcs_connection_next_get(ipcs_rw, last);

            crm_debug("Disconnecting r/w client %p...", last);
            qb_ipcs_disconnect(last);
            qb_ipcs_connection_unref(last);
            disconnects++;
        }

        c = qb_ipcs_connection_first_get(ipcs_ro);
        while (c != NULL) {
            qb_ipcs_connection_t *last = c;

            c = qb_ipcs_connection_next_get(ipcs_ro, last);

            crm_debug("Disconnecting r/o client %p...", last);
            qb_ipcs_disconnect(last);
            qb_ipcs_connection_unref(last);
            disconnects++;
        }

        c = qb_ipcs_connection_first_get(ipcs_shm);
        while (c != NULL) {
            qb_ipcs_connection_t *last = c;

            c = qb_ipcs_connection_next_get(ipcs_shm, last);

            crm_debug("Disconnecting non-blocking r/w client %p...", last);
            qb_ipcs_disconnect(last);
            qb_ipcs_connection_unref(last);
            disconnects++;
        }

        disconnects += pcmk__ipc_client_count();

        crm_debug("Disconnecting %d remote clients", pcmk__ipc_client_count());
        pcmk__foreach_ipc_client(disconnect_remote_client, NULL);
        crm_info("Disconnected %d clients", disconnects);
    }

    qb_ipcs_stats_get(ipcs_rw, &srv_stats, QB_FALSE);

    if (pcmk__ipc_client_count() == 0) {
        crm_info("All clients disconnected (%d)", srv_stats.active_connections);
        initiate_exit();

    } else {
        crm_info("Waiting on %d clients to disconnect (%d)",
                 pcmk__ipc_client_count(), srv_stats.active_connections);
    }
}

void
initiate_exit(void)
{
    int active = 0;
    xmlNode *leaving = NULL;

    active = crm_active_peers();
    if (active < 2) {
        terminate_cib(__FUNCTION__, 0);
        return;
    }

    crm_info("Sending disconnect notification to %d peers...", active);

    leaving = create_xml_node(NULL, "exit-notification");
    crm_xml_add(leaving, F_TYPE, "cib");
    crm_xml_add(leaving, F_CIB_OPERATION, "cib_shutdown_req");

    send_cluster_message(NULL, crm_msg_cib, leaving, TRUE);
    free_xml(leaving);

    g_timeout_add(EXIT_ESCALATION_MS, cib_force_exit, NULL);
}

extern int remote_fd;
extern int remote_tls_fd;

/*!
 * \internal
 * \brief Close remote sockets, free the global CIB and quit
 *
 * \param[in] caller           Name of calling function (for log message)
 * \param[in] fast             If -1, skip disconnect; if positive, exit that
 */
void
terminate_cib(const char *caller, int fast)
{
    crm_info("%s: Exiting%s...", caller,
             (fast > 0)? " fast" : mainloop ? " from mainloop" : "");

    if (remote_fd > 0) {
        close(remote_fd);
        remote_fd = 0;
    }
    if (remote_tls_fd > 0) {
        close(remote_tls_fd);
        remote_tls_fd = 0;
    }

    uninitializeCib();

    if (fast > 0) {
        /* Quit fast on error */
        pcmk__stop_based_ipc(ipcs_ro, ipcs_rw, ipcs_shm);
        crm_exit(fast);

    } else if ((mainloop != NULL) && g_main_loop_is_running(mainloop)) {
        /* Quit via returning from the main loop. If fast == -1, we skip the
         * disconnect here, and it will be done when the main loop returns
         * (this allows the peer status callback to avoid messing with the
         * peer caches).
         */
        if (fast == 0) {
            crm_cluster_disconnect(&crm_cluster);
        }
        g_main_loop_quit(mainloop);

    } else {
        /* Quit via clean exit. Even the peer status callback can disconnect
         * here, because we're not returning control to the caller. */
        crm_cluster_disconnect(&crm_cluster);
        pcmk__stop_based_ipc(ipcs_ro, ipcs_rw, ipcs_shm);
        crm_exit(CRM_EX_OK);
    }
}
