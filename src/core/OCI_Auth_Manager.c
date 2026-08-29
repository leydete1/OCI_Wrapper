/*
 * OCI_Auth_Manager.c
 *
 * Authentication Manager - Implementation
 * ------------------------------------------
 * See OCI_Auth_Manager.h and Security_Module_Design_Specification.docx
 * Section 6.3 for the full design description.
 *
 * Stage 2 (2026-08-27): LOCAL authentication source only. A user whose
 * AUTH_SOURCE.SOURCE_TYPE is 'LDAP' or 'AD' is rejected with
 * AUTH_ERR_DENIED here, logged clearly - Stage 3 replaces that branch
 * with a real LDAP bind, everything else in this file is unaffected.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "OCI_Auth_Manager.h"
#include "OCI_Session_Manager.h"
#include "crypt_helper.h"                 /* crypt_verify_password() - Stage 1 */
#include "logger.h"
#include "ini_reader.h"                   /* app_config_t - ctx->ini->auth_* */

/* NOTE (2026-08-27): auth_max_failed_attempts is now read from
 * ctx->ini->auth_max_failed_attempts (ini_reader.h/.c, config.ini) -
 * the local #define default this replaced is gone. No other TODO
 * remains from the previous revision of this file.                    */

/* ------------------------------------------------------------------ */
/*  OCI error macro - same shape as OCI_Table_Metadata_Module.c's own  */
/*  CHECK_OCI_META, logging to ctx->security_logger instead.           */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_AUTH(errhp, status, ctx, label)                       \
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

/* One row of the APP_USER x AUTH_SOURCE lookup. */
typedef struct {
    int  user_id;
    char display_name[255 + 1];
    char password_hash[255 + 1];
    char enabled[1 + 1];
    char locked[1 + 1];
    int  failed_attempts;
    char source_type[20 + 1];
} auth_user_row_t;

/*
 * lookup_user()
 *
 * Case-insensitive username lookup against the same UPPER(USERNAME)
 * unique index the schema defines (Security_Module_Design_
 * Specification.docx Section 4.2). Returns 1 if a row was found (out
 * populated), 0 if not found (out untouched, not an error - "no such
 * user" is folded into AUTH_ERR_DENIED by the caller), -1 on a genuine
 * OCI/DB failure.
 */
static int lookup_user(oci_context_t *ctx, const char *username,
                        auth_user_row_t *out)
{
    int    rc = 0;
    int    db_failure = 0;
    OCIStmt *stmt = NULL;

    const char *sql =
        "SELECT u.USER_ID, u.DISPLAY_NAME, u.PASSWORD_HASH, "
        "       u.ENABLED, u.LOCKED, u.FAILED_ATTEMPTS, s.SOURCE_TYPE "
        "FROM   APP_USER u "
        "JOIN   AUTH_SOURCE s ON s.AUTH_SOURCE_ID = u.AUTH_SOURCE_ID "
        "WHERE  UPPER(u.USERNAME) = UPPER(:username)";

    CHECK_OCI_AUTH(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_username = NULL;
    CHECK_OCI_AUTH(ctx->errhp,
        OCIBindByName(stmt, &bind_username, ctx->errhp,
                      (text *)":username", -1,
                      (dvoid *)username, (sb4)(strlen(username) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    OCIDefine *def_user_id         = NULL;
    OCIDefine *def_display_name    = NULL;
    OCIDefine *def_password_hash   = NULL;
    OCIDefine *def_enabled         = NULL;
    OCIDefine *def_locked          = NULL;
    OCIDefine *def_failed_attempts = NULL;
    OCIDefine *def_source_type     = NULL;

    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_user_id, ctx->errhp, 1,
                       &out->user_id, sizeof(out->user_id),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_display_name, ctx->errhp, 2,
                       out->display_name, sizeof(out->display_name),
                       SQLT_STR, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_password_hash, ctx->errhp, 3,
                       out->password_hash, sizeof(out->password_hash),
                       SQLT_STR, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_enabled, ctx->errhp, 4,
                       out->enabled, sizeof(out->enabled),
                       SQLT_STR, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_locked, ctx->errhp, 5,
                       out->locked, sizeof(out->locked),
                       SQLT_STR, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_failed_attempts, ctx->errhp, 6,
                       &out->failed_attempts, sizeof(out->failed_attempts),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_source_type, ctx->errhp, 7,
                       out->source_type, sizeof(out->source_type),
                       SQLT_STR, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_AUTH(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 0, 0, NULL, NULL,
                       OCI_DEFAULT),
        ctx, Cleanup);

    sword fetch_rc = OCIStmtFetch2(stmt, ctx->errhp, 1, OCI_FETCH_NEXT,
                                    0, OCI_DEFAULT);
    if (fetch_rc == OCI_NO_DATA)
    {
        rc = 0;   /* no such user - not a DB failure */
    }
    else if (fetch_rc == OCI_SUCCESS || fetch_rc == OCI_SUCCESS_WITH_INFO)
    {
        rc = 1;
    }
    else
    {
        CHECK_OCI_AUTH(ctx->errhp, fetch_rc, ctx, Cleanup);
    }

Cleanup:
    if (stmt) OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return db_failure ? -1 : rc;
}

/*
 * record_auth_failure()
 *
 * Increments FAILED_ATTEMPTS; if it reaches max_failed_attempts, also
 * sets LOCKED = 'Y' and LOCKED_TS = SYSTIMESTAMP in the same update -
 * matches the spec's "There is no automatic unlock" stance (Section
 * 5): once written, only a manual administrator action can clear
 * LOCKED again, nothing in this codebase does so automatically.
 */
static void record_auth_failure(oci_context_t *ctx, int user_id,
                                 int current_failed_attempts,
                                 int max_failed_attempts)
{
    int db_failure = 0;
    OCIStmt *stmt = NULL;
    int new_failed_attempts = current_failed_attempts + 1;
    int should_lock = (new_failed_attempts >= max_failed_attempts);

    const char *sql = should_lock
        ? "UPDATE APP_USER "
          "SET    FAILED_ATTEMPTS = :failed_attempts, "
          "       LOCKED = 'Y', LOCKED_TS = SYSTIMESTAMP, "
          "       MODIFIED_TS = SYSTIMESTAMP "
          "WHERE  USER_ID = :user_id"
        : "UPDATE APP_USER "
          "SET    FAILED_ATTEMPTS = :failed_attempts, "
          "       MODIFIED_TS = SYSTIMESTAMP "
          "WHERE  USER_ID = :user_id";

    CHECK_OCI_AUTH(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_failed = NULL;
    OCIBind *bind_userid = NULL;
    CHECK_OCI_AUTH(ctx->errhp,
        OCIBindByName(stmt, &bind_failed, ctx->errhp,
                      (text *)":failed_attempts", -1,
                      &new_failed_attempts, sizeof(new_failed_attempts),
                      SQLT_INT, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIBindByName(stmt, &bind_userid, ctx->errhp,
                      (text *)":user_id", -1,
                      &user_id, sizeof(user_id),
                      SQLT_INT, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_AUTH(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL,
                       OCI_DEFAULT),
        ctx, Cleanup);

    OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT);

    if (should_lock)
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "user_id=%d LOCKED after %d failed attempts "
                     "(manual unlock only - no auto-expiry)",
                     user_id, new_failed_attempts);

Cleanup:
    if (stmt) OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    if (db_failure)
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "record_auth_failure: DB update failed for "
                     "user_id=%d - FAILED_ATTEMPTS not persisted this "
                     "attempt", user_id);
}

/*
 * record_auth_success()
 *
 * Resets FAILED_ATTEMPTS to 0 and stamps LAST_LOGIN_TS. Best-effort -
 * a failure here is logged but does not fail the authentication
 * itself (the user already proved their credential; losing this
 * bookkeeping update is not a reason to deny them).
 */
static void record_auth_success(oci_context_t *ctx, int user_id)
{
    int db_failure = 0;
    OCIStmt *stmt = NULL;

    const char *sql =
        "UPDATE APP_USER "
        "SET    FAILED_ATTEMPTS = 0, LAST_LOGIN_TS = SYSTIMESTAMP, "
        "       MODIFIED_TS = SYSTIMESTAMP "
        "WHERE  USER_ID = :user_id";

    CHECK_OCI_AUTH(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_userid = NULL;
    CHECK_OCI_AUTH(ctx->errhp,
        OCIBindByName(stmt, &bind_userid, ctx->errhp,
                      (text *)":user_id", -1,
                      &user_id, sizeof(user_id),
                      SQLT_INT, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_AUTH(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL,
                       OCI_DEFAULT),
        ctx, Cleanup);

    OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT);

Cleanup:
    if (stmt) OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    if (db_failure)
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "record_auth_success: DB update failed for "
                     "user_id=%d - FAILED_ATTEMPTS/LAST_LOGIN_TS not "
                     "persisted this login (session was still created)",
                     user_id);
}

/*
 * extract_session_field()
 *
 * Tiny local helper to pull <tag>value</tag> out of session_create()'s
 * result_xml - deliberately not reusing extract_tag() from
 * OCI_Insert_Validate_Module.c, which is static to that file; this
 * mirrors it at the same scope every other module's own small parsing
 * helpers live at, rather than exporting a shared one for two callers.
 */
static int extract_session_field(const char *xml, const char *tag,
                                  char *out, size_t out_size)
{
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return 0;
    start += strlen(open_tag);

    const char *end = strstr(start, close_tag);
    if (!end || end < start) return 0;

    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

/* ================================================================== */
/*  auth_authenticate                                                    */
/* ================================================================== */
int auth_authenticate(oci_context_t                 *ctx,
                       const authenticate_request_t  *req,
                       char                         **session_id_out,
                       char                         **display_name_out,
                       int                            *ttl_seconds_out)
{
    if (session_id_out)   *session_id_out = NULL;
    if (display_name_out) *display_name_out = NULL;

    if (!ctx || !req || !req->username[0] || !req->credential[0] ||
        !session_id_out || !display_name_out || !ttl_seconds_out)
        return AUTH_ERR_INVALID_ARG;

    auth_user_row_t user;
    memset(&user, 0, sizeof(user));

    int found = lookup_user(ctx, req->username, &user);
    if (found < 0)
        return AUTH_ERR_DB_FAILURE;

    if (found == 0)
    {
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "DENIED username='%s': no such user", req->username);
        return AUTH_ERR_DENIED;
    }

    if (strcmp(user.enabled, "Y") != 0)
    {
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "DENIED username='%s' user_id=%d: account disabled",
                     req->username, user.user_id);
        return AUTH_ERR_DENIED;
    }

    if (strcmp(user.locked, "Y") == 0)
    {
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "DENIED username='%s' user_id=%d: account locked "
                     "(manual unlock only)", req->username, user.user_id);
        return AUTH_ERR_DENIED;
    }

    if (strcasecmp(user.source_type, "LOCAL") != 0)
    {
        /* Stage 3 territory - not implemented yet. Logged distinctly
         * from a real credential failure so this is easy to find in
         * the logs during Stage 2 testing, but the caller still only
         * ever sees the same generic AUTH_ERR_DENIED.                */
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "DENIED username='%s' user_id=%d: source_type='%s' "
                     "not yet implemented (LDAP/AD is Stage 3)",
                     req->username, user.user_id, user.source_type);
        return AUTH_ERR_DENIED;
    }

    int verify_rc = crypt_verify_password(ctx, req->credential,
                                           user.password_hash);
    if (verify_rc != CRYPT_OK)
    {
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "DENIED username='%s' user_id=%d: bad credential",
                     req->username, user.user_id);
        record_auth_failure(ctx, user.user_id, user.failed_attempts,
                             ctx->ini->auth_max_failed_attempts);
        return AUTH_ERR_DENIED;
    }

    /* Credential verified - build the session exactly as any other
     * session_create() caller would (OCI_Session_Manager.h, unchanged;
     * Security Module Design Specification, Section 7).              */
    session_request_t session_req;
    memset(&session_req, 0, sizeof(session_req));
    strncpy(session_req.operation, "CREATE_SESSION",
            sizeof(session_req.operation) - 1);
    strncpy(session_req.client_id, req->username,
            sizeof(session_req.client_id) - 1);

    char *session_xml = NULL;
    int session_rc = session_create(ctx, &session_req, &session_xml);
    if (session_rc != SESSION_OK || !session_xml)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "username='%s' user_id=%d: credential verified but "
                     "session_create() failed (rc=%d) - treating as a DB "
                     "failure, not a denial",
                     req->username, user.user_id, session_rc);
        free(session_xml);
        return AUTH_ERR_DB_FAILURE;
    }

    char session_id[64]   = {0};
    char ttl_str[16]      = {0};
    extract_session_field(session_xml, "session_id", session_id, sizeof(session_id));
    extract_session_field(session_xml, "ttl_seconds", ttl_str, sizeof(ttl_str));
    free(session_xml);

    if (!session_id[0])
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "username='%s' user_id=%d: session_create() succeeded "
                     "but session_id could not be parsed from its result",
                     req->username, user.user_id);
        return AUTH_ERR_DB_FAILURE;
    }

    *session_id_out   = strdup(session_id);
    *display_name_out = strdup(user.display_name[0] ? user.display_name
                                                      : req->username);
    *ttl_seconds_out  = ttl_str[0] ? atoi(ttl_str) : 0;

    if (!*session_id_out || !*display_name_out)
    {
        free(*session_id_out);
        free(*display_name_out);
        *session_id_out = NULL;
        *display_name_out = NULL;
        return AUTH_ERR_ALLOC;
    }

    /* Best-effort bookkeeping - see record_auth_success()'s own doc
     * comment on why this never turns a successful login into a
     * denial. */
    record_auth_success(ctx, user.user_id);

    logger_write(ctx->security_logger, LOG_INFO, __func__, 0,
                 "SUCCESS username='%s' user_id=%d session_id=%s",
                 req->username, user.user_id, session_id);

    return AUTH_OK;
}
