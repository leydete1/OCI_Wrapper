/*
 * OCI_Authz_Manager.c
 *
 * Authorization Manager - Implementation (Security Module Stage 5)
 * ---------------------------------------------------------------------
 * See OCI_Authz_Manager.h and Security_Module_Design_Specification.docx
 * Section 6.4/6.6 for the full design description.
 *
 * Oracle dialect extraction, Stage 5 (2026-10-08): the permission
 * query in authz_build_permission_cache() runs through the driver's
 * select cursor with the user id bound - no OCI calls here any more.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Authz_Manager.h"
#include "authz_cache.h"
#include "logger.h"
#include "metrics.h"          /* metrics_record_t, metrics_now_us() - 2026-09-01 */
#include "metrics_writer.h"   /* metrics_finalise_and_enqueue() */
#include "db_driver.h"        /* Stage 5 - select cursor, dialect   */
#include "Resultset_Builder.h" /* resultset_free()                  */

/* Comma-separated PERMISSION_CODE list buffer size - generous enough
 * for any realistic number of permissions per user (PERMISSION_CODE
 * is VARCHAR2(100); this comfortably holds several hundred distinct
 * permissions). If a user's real permission count ever exceeds this,
 * the list is truncated at the last complete code and a WARN is
 * logged - not a crash, not a silently-wrong partial code.            */
#define AUTHZ_PERMISSION_LIST_BUF_SIZE 8192

/* ================================================================== */
/*  authz_build_permission_cache                                        */
/* ================================================================== */
int authz_build_permission_cache(oci_context_t *ctx,
                                  const char    *session_id,
                                  int            user_id)
{
    if (!ctx || !session_id || !session_id[0] || user_id <= 0)
        return AUTHZ_ERR_INVALID_ARG;

    /* Stage 5 (2026-10-08): runs through the driver's select cursor,
     * user_id bound by position (placeholder from the dialect). The
     * driver logs the vendor detail of a failure on ctx->select_logger;
     * this function adds one line to ctx->security_logger. As before,
     * a failure stores nothing in the cache. */
    const db_driver_t *driver = db_driver_get(ctx);

    char ph[16];
    if (driver->dialect->bind_placeholder(1, ph, sizeof(ph)) != 0)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "dialect bind_placeholder(1) failed");
        return AUTHZ_ERR_DB_FAILURE;
    }

    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT p.PERMISSION_CODE "
        "FROM   USER_ROLE ur "
        "JOIN   ROLE_PERMISSION rp ON rp.ROLE_ID = ur.ROLE_ID "
        "JOIN   PERMISSION p ON p.PERMISSION_ID = rp.PERMISSION_ID "
        "WHERE  ur.USER_ID = %s "
        "ORDER BY p.PERMISSION_CODE", ph);

    char user_id_bind[16];
    snprintf(user_id_bind, sizeof(user_id_bind), "%d", user_id);
    const char *binds[1] = { user_id_bind };

    db_select_request_t req;
    memset(&req, 0, sizeof(req));
    req.sql              = sql;
    req.fetch_array_size = (ctx->ini && ctx->ini->query_fetch_batch_size > 0)
                           ? ctx->ini->query_fetch_batch_size : 50;
    req.bind_count       = 1;
    req.bind_values      = binds;
    req.text_lobs_inline = 0;   /* no LOB columns */

    db_select_cursor_t *cursor     = NULL;
    db_column_meta_t   *columns    = NULL;
    int                 col_count  = 0;
    int                 batch_size = 0;

    if (driver->select_open(ctx, &req, &cursor, &columns,
                            &col_count, &batch_size) != 0)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "permission query failed for user_id=%d (select_open) "
                     "- see the select log for the database error",
                     user_id);
        return AUTHZ_ERR_DB_FAILURE;
    }

    char permission_list[AUTHZ_PERMISSION_LIST_BUF_SIZE];
    permission_list[0] = '\0';
    size_t list_len = 0;
    int permission_count = 0;
    int truncated = 0;
    int fetch_failed = 0;

    while (!truncated)
    {
        resultset_t *rs   = NULL;
        int          rows = 0;

        if (driver->select_fetch_batch(cursor, &rs, &rows, NULL) != 0)
        {
            fetch_failed = 1;
            break;
        }
        if (rows == 0)
            break;

        for (int r = 0; r < rows && !truncated; r++)
        {
            const char *permission_code = rs->records[r].fields[0].value;
            size_t code_len = strlen(permission_code);
            /* +1 for the comma separator between entries (not needed
             * before the very first entry, accounted for below).        */
            size_t needed = code_len + (list_len > 0 ? 1 : 0);

            if (list_len + needed >= sizeof(permission_list))
            {
                truncated = 1;
                break;
            }

            if (list_len > 0)
            {
                permission_list[list_len++] = ',';
            }
            memcpy(permission_list + list_len, permission_code, code_len);
            list_len += code_len;
            permission_list[list_len] = '\0';
            permission_count++;
        }
        resultset_free(rs);
    }

    driver->select_close(cursor);
    free(columns);

    if (fetch_failed || col_count != 1)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "permission query failed for user_id=%d (%s) - "
                     "nothing cached", user_id,
                     fetch_failed ? "fetch - see the select log"
                                  : "unexpected column count");
        return AUTHZ_ERR_DB_FAILURE;
    }

    if (truncated)
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "user_id=%d has more permissions than fit in the "
                     "%d-byte cache buffer - list truncated at %d "
                     "entries. Consider raising "
                     "AUTHZ_PERMISSION_LIST_BUF_SIZE if this is "
                     "expected for this deployment.",
                     user_id, AUTHZ_PERMISSION_LIST_BUF_SIZE,
                     permission_count);

    int store_rc = authz_cache_store(ctx->authz_cache, session_id,
                                      permission_list);
    if (store_rc != 0)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "authz_cache_store failed for session_id=%s "
                     "user_id=%d - permission checks for this session "
                     "will deny until next login (see this function's "
                     "own doc comment on why this doesn't fail the "
                     "login itself)", session_id, user_id);
    }
    else
    {
        logger_write(ctx->security_logger, LOG_INFO, __func__, 0,
                     "Permission cache built for session_id=%s "
                     "user_id=%d: %d permission(s)%s",
                     session_id, user_id, permission_count,
                     truncated ? " (truncated)" : "");
    }

    return AUTHZ_OK;
}

/* ================================================================== */
/*  authz_has_permission                                                */
/* ================================================================== */
/*
 * finalise_and_enqueue_authz_metrics()
 *
 * Same shape as OCI_Auth_Manager.c's own finalise_and_enqueue_auth_
 * metrics() helper - authz_has_permission() has several return points
 * (cache disabled, cache miss, decode failure, ALLOWED, DENIED-in-
 * list) that all need the same "stamp end_time_us/status/enqueue"
 * sequence; factored out once rather than repeated five times.
 * ldap_bind_us/crypt_verify_us are never set here (left at 0 from
 * metrics_init()) - this operation never touches LDAP or Argon2id at
 * all, that's the whole point of it being a pure cache lookup.
 */
static void finalise_and_enqueue_authz_metrics(oci_context_t *ctx,
                                                metrics_record_t *metrics,
                                                int status_code,
                                                const char *error_code,
                                                const char *error_text)
{
    metrics->end_time_us  = metrics_now_us();
    metrics->execution_us = metrics->end_time_us - metrics->start_time_us;
    metrics->status_code  = status_code;
    if (error_code)
        strncpy(metrics->error_code, error_code, sizeof(metrics->error_code) - 1);
    if (error_text)
        strncpy(metrics->error_text, error_text, sizeof(metrics->error_text) - 1);
    metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger,
                                  metrics);
}

int authz_has_permission(oci_context_t *ctx,
                          const char    *session_id,
                          const char    *permission_code)
{
    if (!ctx || !session_id || !session_id[0] ||
        !permission_code || !permission_code[0])
        return AUTHZ_ERR_INVALID_ARG;

    metrics_record_t metrics;
    metrics_init(&metrics);
    metrics.start_time_us = metrics_now_us();
    strncpy(metrics.operation, "CHECK_PERMISSION", sizeof(metrics.operation) - 1);
    strncpy(metrics.object_name, permission_code, sizeof(metrics.object_name) - 1);
    strncpy(metrics.session_id, session_id, sizeof(metrics.session_id) - 1);
    metrics_set_context(&metrics, ctx);

    /* A disabled/failed-to-initialise authz_cache is the same as a
     * permanent, universal cache miss - fails closed, exactly like a
     * disabled session_cache would fail every session_validate()
     * call (see authz_cache_init()'s own doc comment).                */
    if (!ctx->authz_cache)
    {
        finalise_and_enqueue_authz_metrics(ctx, &metrics, AUTHZ_ERR_DENIED,
                                            "AUTHZ_ERR_DENIED",
                                            "authz_cache disabled");
        return AUTHZ_ERR_DENIED;
    }

    cache_entry_t *entry = authz_cache_lookup(ctx->authz_cache, session_id);
    if (!entry)
    {
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "DENIED session_id=%s permission_code='%s': no "
                     "cached permission list (unknown, expired, or "
                     "never-authenticated session)",
                     session_id, permission_code);
        finalise_and_enqueue_authz_metrics(ctx, &metrics, AUTHZ_ERR_DENIED,
                                            "AUTHZ_ERR_DENIED",
                                            "no cached permission list");
        return AUTHZ_ERR_DENIED;
    }

    char permission_list[AUTHZ_PERMISSION_LIST_BUF_SIZE];
    int decode_rc = authz_cache_decode(entry->output_document,
                                        permission_list,
                                        sizeof(permission_list));
    authz_cache_release(ctx->authz_cache, entry);

    if (decode_rc != 0)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "DENIED session_id=%s permission_code='%s': cached "
                     "entry failed to decode (stale encoding version?)",
                     session_id, permission_code);
        finalise_and_enqueue_authz_metrics(ctx, &metrics, AUTHZ_ERR_DENIED,
                                            "AUTHZ_ERR_DENIED",
                                            "cache entry failed to decode");
        return AUTHZ_ERR_DENIED;
    }

    /* Exact-token match against the comma-separated list - never a
     * substring match, so "CUSTOMER.READ" cannot be accidentally
     * satisfied by a cached "CUSTOMER.READALL" or similar.            */
    char *saveptr = NULL;
    char *token = strtok_r(permission_list, ",", &saveptr);
    while (token)
    {
        if (strcmp(token, permission_code) == 0)
        {
            logger_write(ctx->security_logger, LOG_INFO, __func__, 0,
                         "ALLOWED session_id=%s permission_code='%s'",
                         session_id, permission_code);
            finalise_and_enqueue_authz_metrics(ctx, &metrics, AUTHZ_OK,
                                                NULL, NULL);
            return AUTHZ_OK;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                 "DENIED session_id=%s permission_code='%s': not in "
                 "cached permission list", session_id, permission_code);
    finalise_and_enqueue_authz_metrics(ctx, &metrics, AUTHZ_ERR_DENIED,
                                        "AUTHZ_ERR_DENIED",
                                        "not in cached permission list");
    return AUTHZ_ERR_DENIED;
}
