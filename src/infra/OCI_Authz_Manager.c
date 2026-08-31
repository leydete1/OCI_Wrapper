/*
 * OCI_Authz_Manager.c
 *
 * Authorization Manager - Implementation (Security Module Stage 5)
 * ---------------------------------------------------------------------
 * See OCI_Authz_Manager.h and Security_Module_Design_Specification.docx
 * Section 6.4/6.6 for the full design description.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OCI_Authz_Manager.h"
#include "authz_cache.h"
#include "logger.h"

/* Same shape as OCI_Table_Metadata_Module.c's own CHECK_OCI_META and
 * OCI_Auth_Manager.c's own CHECK_OCI_AUTH - logs to ctx->security_logger
 * instead, matching this file's own logging convention (authorization
 * decisions and their DB-side plumbing both belong in the same log as
 * authentication decisions, not a separate one).                      */
#define CHECK_OCI_AUTHZ(errhp, status, ctx, label)                      \
    do {                                                                \
        if ((status) != OCI_SUCCESS &&                                  \
            (status) != OCI_SUCCESS_WITH_INFO)                          \
        {                                                                \
            text _errbuf[512];                                          \
            sb4  _errcode = 0;                                          \
            OCIErrorGet((errhp), 1, NULL, &_errcode,                    \
                        _errbuf, sizeof(_errbuf), OCI_HTYPE_ERROR);      \
            logger_write((ctx)->security_logger, LOG_ERROR, __func__, 0,\
                         "OCI Error %d: %s", _errcode, (char *)_errbuf); \
            db_failure = 1;                                             \
            goto label;                                                 \
        }                                                                \
    } while (0)

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

    int      db_failure = 0;
    OCIStmt *stmt = NULL;

    const char *sql =
        "SELECT p.PERMISSION_CODE "
        "FROM   USER_ROLE ur "
        "JOIN   ROLE_PERMISSION rp ON rp.ROLE_ID = ur.ROLE_ID "
        "JOIN   PERMISSION p ON p.PERMISSION_ID = rp.PERMISSION_ID "
        "WHERE  ur.USER_ID = :user_id "
        "ORDER BY p.PERMISSION_CODE";

    CHECK_OCI_AUTHZ(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_userid = NULL;
    CHECK_OCI_AUTHZ(ctx->errhp,
        OCIBindByName(stmt, &bind_userid, ctx->errhp,
                      (text *)":user_id", -1,
                      &user_id, sizeof(user_id),
                      SQLT_INT, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    char permission_code[101];
    OCIDefine *def_permission_code = NULL;
    CHECK_OCI_AUTHZ(ctx->errhp,
        OCIDefineByPos(stmt, &def_permission_code, ctx->errhp, 1,
                       permission_code, sizeof(permission_code),
                       SQLT_STR, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_AUTHZ(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 0, 0, NULL, NULL,
                       OCI_DEFAULT),
        ctx, Cleanup);

    char permission_list[AUTHZ_PERMISSION_LIST_BUF_SIZE];
    permission_list[0] = '\0';
    size_t list_len = 0;
    int permission_count = 0;
    int truncated = 0;

    for (;;)
    {
        sword fetch_rc = OCIStmtFetch2(stmt, ctx->errhp, 1, OCI_FETCH_NEXT,
                                        0, OCI_DEFAULT);
        if (fetch_rc == OCI_NO_DATA)
            break;
        if (fetch_rc != OCI_SUCCESS && fetch_rc != OCI_SUCCESS_WITH_INFO)
            CHECK_OCI_AUTHZ(ctx->errhp, fetch_rc, ctx, Cleanup);

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

Cleanup:
    if (stmt) OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return db_failure ? AUTHZ_ERR_DB_FAILURE : AUTHZ_OK;
}

/* ================================================================== */
/*  authz_has_permission                                                */
/* ================================================================== */
int authz_has_permission(oci_context_t *ctx,
                          const char    *session_id,
                          const char    *permission_code)
{
    if (!ctx || !session_id || !session_id[0] ||
        !permission_code || !permission_code[0])
        return AUTHZ_ERR_INVALID_ARG;

    /* A disabled/failed-to-initialise authz_cache is the same as a
     * permanent, universal cache miss - fails closed, exactly like a
     * disabled session_cache would fail every session_validate()
     * call (see authz_cache_init()'s own doc comment).                */
    if (!ctx->authz_cache)
        return AUTHZ_ERR_DENIED;

    cache_entry_t *entry = authz_cache_lookup(ctx->authz_cache, session_id);
    if (!entry)
    {
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "DENIED session_id=%s permission_code='%s': no "
                     "cached permission list (unknown, expired, or "
                     "never-authenticated session)",
                     session_id, permission_code);
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
            return AUTHZ_OK;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }

    logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                 "DENIED session_id=%s permission_code='%s': not in "
                 "cached permission list", session_id, permission_code);
    return AUTHZ_ERR_DENIED;
}
