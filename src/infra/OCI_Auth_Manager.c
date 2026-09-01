/*
 * OCI_Auth_Manager.c
 *
 * Authentication Manager - Implementation
 * ------------------------------------------
 * See OCI_Auth_Manager.h and Security_Module_Design_Specification.docx
 * Section 6.3 for the full design description.
 *
 * Stage 3 (2026-08-29): LOCAL and delegated LDAP/AD authentication are
 * both implemented. LDAP/AD uses a real LDAP simple bind via
 * ldap_auth_helper.h/.c (deliberately isolated from this file - see
 * that header's own comment on why, re: this project's pre-existing
 * Oracle ldap.h vs OpenLDAP's ldap.h).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "OCI_Auth_Manager.h"
#include "OCI_Session_Manager.h"
#include "crypt_helper.h"                 /* crypt_verify_password() - Stage 1 */
#include "ldap_auth_helper.h"             /* ldap_auth_bind_check() - Stage 3 */
#include "cJSON.h"                        /* AUTH_SOURCE.CONFIGURATION parsing */
#include "OCI_Audit_Trail_Manager.h"      /* audit_trail_insert() - Stage 4 */
#include "OCI_Authz_Manager.h"            /* authz_build_permission_cache() -
                                            * Stage 5 */
#include "metrics.h"                      /* metrics_record_t, metrics_now_us() -
                                            * timing metrics, 2026-09-01 */
#include "metrics_writer.h"               /* metrics_finalise_and_enqueue() */
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
    char *ldap_configuration;   /* AUTH_SOURCE.CONFIGURATION CLOB, as a
                                  * plain C string - heap-allocated by
                                  * lookup_user(), NULL if the source is
                                  * LOCAL (CONFIGURATION is NULL/unused
                                  * for LOCAL rows, Security_Module_
                                  * Design_Specification.docx Section
                                  * 4.1) or empty. Caller must free().  */
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
    OCILobLocator *config_lob = NULL;
    sb2 config_ind = 0;   /* OCI NULL indicator for CONFIGURATION - see
                            * fix below: -1 means the column was NULL,
                            * which is the normal case for LOCAL rows */
    sb2 display_name_ind = 0;   /* DISPLAY_NAME is nullable in the schema */
    sb2 password_hash_ind = 0;  /* PASSWORD_HASH is NULL by design for
                                  * every non-LOCAL (LDAP/AD) user -
                                  * Security_Module_Design_Specification
                                  * .docx Section 4.2. Both of these
                                  * need an indicator for the same
                                  * reason CONFIGURATION does - see that
                                  * fix's own comment for why omitting
                                  * one throws ORA-01405 rather than
                                  * just fetching a NULL.              */

    out->ldap_configuration = NULL;

    const char *sql =
        "SELECT u.USER_ID, u.DISPLAY_NAME, u.PASSWORD_HASH, "
        "       u.ENABLED, u.LOCKED, u.FAILED_ATTEMPTS, s.SOURCE_TYPE, "
        "       s.CONFIGURATION "
        "FROM   APP_USER u "
        "JOIN   AUTH_SOURCE s ON s.AUTH_SOURCE_ID = u.AUTH_SOURCE_ID "
        "WHERE  UPPER(u.USERNAME) = UPPER(:username)";

    CHECK_OCI_AUTH(ctx->errhp,
        OCIDescriptorAlloc(ctx->envhp, (void **)&config_lob,
                           OCI_DTYPE_LOB, 0, NULL),
        ctx, Cleanup);

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
    OCIDefine *def_config          = NULL;

    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_user_id, ctx->errhp, 1,
                       &out->user_id, sizeof(out->user_id),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_display_name, ctx->errhp, 2,
                       out->display_name, sizeof(out->display_name),
                       SQLT_STR, &display_name_ind, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    CHECK_OCI_AUTH(ctx->errhp,
        OCIDefineByPos(stmt, &def_password_hash, ctx->errhp, 3,
                       out->password_hash, sizeof(out->password_hash),
                       SQLT_STR, &password_hash_ind, NULL, NULL, OCI_DEFAULT),
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
        OCIDefineByPos(stmt, &def_config, ctx->errhp, 8,
                       &config_lob, sizeof(config_lob),
                       SQLT_CLOB, &config_ind, NULL, NULL, OCI_DEFAULT),
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

        /* CONFIGURATION is NULL for LOCAL rows (Security_Module_
         * Design_Specification.docx Section 4.1) - config_ind == -1
         * is OCI's NULL indicator for that column on this fetch (see
         * the OCIDefineByPos above). A NULL LOB locator must not be
         * passed to OCILobGetLength()/OCILobRead() at all - doing so
         * on a NULL column is exactly what raised ORA-01405 before
         * this indicator was added.                                  */
        if (config_ind != -1)
        {
            ub4 config_len = 0;
            OCILobGetLength(ctx->svchp, ctx->errhp, config_lob, &config_len);

            if (config_len > 0)
            {
                char *buf = malloc((size_t)config_len + 1);
                if (buf)
                {
                    ub4 offset = 1;
                    ub4 remaining = config_len;
                    char *wp = buf;
                    while (remaining > 0)
                    {
                        ub4 amount = remaining;
                        sword lob_rc = OCILobRead(ctx->svchp, ctx->errhp,
                                                   config_lob, &amount, offset,
                                                   wp, remaining, NULL, NULL,
                                                   0, SQLCS_IMPLICIT);
                        if (lob_rc != OCI_SUCCESS &&
                            lob_rc != OCI_SUCCESS_WITH_INFO)
                            break;   /* leave whatever was read so far -
                                      * caller treats a short/garbled JSON
                                      * parse failure the same as "no
                                      * configuration", logged, not fatal */
                        wp        += amount;
                        offset    += amount;
                        remaining -= amount;
                    }
                    *wp = '\0';
                    out->ldap_configuration = buf;
                }
            }
        }
    }
    else
    {
        CHECK_OCI_AUTH(ctx->errhp, fetch_rc, ctx, Cleanup);
    }

Cleanup:
    if (stmt) OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    if (config_lob) OCIDescriptorFree(config_lob, OCI_DTYPE_LOB);
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
 *
 * Stage 4 (2026-08-30): also writes an AUDIT_TRAIL row for this
 * change, via audit_trail_insert() directly (OCI_Audit_Trail_Manager.h)
 * - the same function OCI_Insert_Execute_Module.c calls for a plain
 * business INSERT. This bypasses execute_update_batch()'s own before-
 * image-fetch machinery deliberately: that pipeline exists for
 * generic client-driven UPDATE requests where the caller doesn't
 * already know the old values, which isn't the case here - this
 * function already knows both the old and new FAILED_ATTEMPTS/LOCKED
 * values itself, so fetching them again via a SELECT would be pure
 * overhead. Only FAILED_ATTEMPTS and (when it changes) LOCKED are
 * audited - LOCKED_TS/MODIFIED_TS are server-generated SYSTIMESTAMP
 * values this function never reads back, and are already implicit in
 * the audit row's own timestamp; auditing them would need an extra
 * round-trip for no real audit value.
 */
/*
 * finalise_and_enqueue_auth_metrics()
 *
 * Small shared helper (2026-09-01) - the same six-line "stamp end_time_us/
 * ldap_bind_us/crypt_verify_us/status_code/error/enqueue" sequence was
 * about to get copy-pasted at four separate return points inside
 * auth_authenticate() (credential denied, session_create() failure,
 * session_id-parse failure, alloc failure) - all four happen AFTER
 * credential verification, so ctx->ldap_bind_us/crypt_verify_us are
 * already meaningfully set by the time any of them fire. Factored out
 * once here instead, so all four - and the final success path, which
 * calls this too - stay in lockstep by construction rather than by
 * four separately-maintained copies.
 */
static void finalise_and_enqueue_auth_metrics(oci_context_t *ctx,
                                               metrics_record_t *metrics,
                                               int status_code,
                                               const char *error_code,
                                               const char *error_text)
{
    metrics->end_time_us     = metrics_now_us();
    metrics->ldap_bind_us    = ctx->ldap_bind_us;
    metrics->crypt_verify_us = ctx->crypt_verify_us;
    metrics->execution_us    = ctx->ldap_bind_us + ctx->crypt_verify_us;
    metrics->status_code     = status_code;
    if (error_code)
        strncpy(metrics->error_code, error_code, sizeof(metrics->error_code) - 1);
    if (error_text)
        strncpy(metrics->error_text, error_text, sizeof(metrics->error_text) - 1);
    metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger,
                                  metrics);
}

static void record_auth_failure(oci_context_t *ctx, int user_id,
                                 const char *username,
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

    /* Stage 4: AUDIT_TRAIL row for this change - see this function's
     * own doc comment above for why audit_trail_insert() is called
     * directly rather than routing through execute_update_batch().
     *
     * IMPORTANT (2026-08-30 crash fix): audit_trail_insert()'s
     * UPDATE/DELETE path casts atr->new_values/old_values (both
     * declared void* in audit_trail_request_t) to an INTERNAL,
     * private struct - audit_field_value_t { char value[32768];
     * int is_empty; } - defined only inside OCI_Audit_Trail_Manager.c
     * itself, not exposed in its header at all. The first version of
     * this fix used field_value_t (OCI_Request_Response_Types.h)
     * instead, which has a completely different size and layout -
     * audit_trail_insert() then read far past the end of those much
     * smaller stack arrays, corrupting memory and crashing the whole
     * process. auth_field_value_t below MUST be kept byte-for-byte in
     * sync with OCI_Audit_Trail_Manager.c's own audit_field_value_t -
     * this is a genuinely fragile, undocumented contract (the
     * project's own code mirrors this same private struct per-file
     * rather than exposing it), not something introduced by this fix.
     */
    {
        typedef struct { char value[32768]; int is_empty; } auth_field_value_t;

        char user_id_str[32];
        snprintf(user_id_str, sizeof(user_id_str), "%d", user_id);

        char col_names[2][128];
        auth_field_value_t old_values[2];
        auth_field_value_t new_values[2];
        memset(old_values, 0, sizeof(old_values));
        memset(new_values, 0, sizeof(new_values));
        int col_count = 0;

        strncpy(col_names[col_count], "FAILED_ATTEMPTS", sizeof(col_names[0]) - 1);
        snprintf(old_values[col_count].value, sizeof(old_values[0].value),
                 "%d", current_failed_attempts);
        snprintf(new_values[col_count].value, sizeof(new_values[0].value),
                 "%d", new_failed_attempts);
        col_count++;

        if (should_lock)
        {
            strncpy(col_names[col_count], "LOCKED", sizeof(col_names[0]) - 1);
            strncpy(old_values[col_count].value, "N", sizeof(old_values[0].value) - 1);
            strncpy(new_values[col_count].value, "Y", sizeof(new_values[0].value) - 1);
            col_count++;
        }

        audit_trail_request_t atr;
        memset(&atr, 0, sizeof(atr));
        strncpy(atr.table_name, "APP_USER", sizeof(atr.table_name) - 1);
        strncpy(atr.action_type, "UPDATE", sizeof(atr.action_type) - 1);
        strncpy(atr.record_id, user_id_str, sizeof(atr.record_id) - 1);
        strncpy(atr.changed_by, username ? username : "-",
                sizeof(atr.changed_by) - 1);
        strncpy(atr.module_name, "OCI_Auth_Manager", sizeof(atr.module_name) - 1);
        if (should_lock)
            snprintf(atr.change_reason, sizeof(atr.change_reason),
                     "Account locked after %d failed login attempts "
                     "(manual unlock only)", new_failed_attempts);
        else
            strncpy(atr.change_reason, "Failed login attempt",
                    sizeof(atr.change_reason) - 1);
        strncpy(atr.session_id, "-", sizeof(atr.session_id) - 1);
        strncpy(atr.client_ip, "-", sizeof(atr.client_ip) - 1);

        atr.col_names  = col_names;
        atr.col_types  = NULL;
        atr.new_values = new_values;
        atr.old_values = old_values;
        atr.row_count  = 1;
        atr.col_count  = col_count;
        atr.audit_mode = AUDIT_MODE_FIELD;

        int audit_rc = audit_trail_insert(ctx, &atr);
        if (audit_rc != 0)
            logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                         "AUDIT_TRAIL insert failed (rc=%d) for user_id=%d "
                         "- lockout bookkeeping is NOT rolled back",
                         audit_rc, user_id);
    }

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
 *
 * Stage 4 (2026-08-30): also writes an AUDIT_TRAIL row when
 * FAILED_ATTEMPTS actually changes (i.e. was non-zero) - a login that
 * resets an existing failure count is the security-relevant fact
 * worth a record; a routine login with no prior failures produces no
 * audit noise. LAST_LOGIN_TS itself is not audited (server-generated
 * SYSTIMESTAMP this function never reads back, already implicit in
 * the audit row's own timestamp - same reasoning as record_auth_
 * failure()'s own doc comment on LOCKED_TS/MODIFIED_TS).
 */
static void record_auth_success(oci_context_t *ctx, int user_id,
                                 const char *username,
                                 int previous_failed_attempts)
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

    /* Stage 4: only audit this when it's actually meaningful - a
     * login that resets a genuine prior failure count, not every
     * routine successful login (see this function's own doc comment
     * above). See record_auth_failure()'s own comment on why
     * auth_field_value_t must exactly mirror OCI_Audit_Trail_
     * Manager.c's private audit_field_value_t struct - the crash this
     * fixed was this exact mismatch.                                 */
    if (previous_failed_attempts > 0)
    {
        typedef struct { char value[32768]; int is_empty; } auth_field_value_t;

        char user_id_str[32];
        snprintf(user_id_str, sizeof(user_id_str), "%d", user_id);

        char col_names[1][128];
        auth_field_value_t old_values[1];
        auth_field_value_t new_values[1];
        memset(old_values, 0, sizeof(old_values));
        memset(new_values, 0, sizeof(new_values));

        strncpy(col_names[0], "FAILED_ATTEMPTS", sizeof(col_names[0]) - 1);
        snprintf(old_values[0].value, sizeof(old_values[0].value),
                 "%d", previous_failed_attempts);
        strncpy(new_values[0].value, "0", sizeof(new_values[0].value) - 1);

        audit_trail_request_t atr;
        memset(&atr, 0, sizeof(atr));
        strncpy(atr.table_name, "APP_USER", sizeof(atr.table_name) - 1);
        strncpy(atr.action_type, "UPDATE", sizeof(atr.action_type) - 1);
        strncpy(atr.record_id, user_id_str, sizeof(atr.record_id) - 1);
        strncpy(atr.changed_by, username ? username : "-",
                sizeof(atr.changed_by) - 1);
        strncpy(atr.module_name, "OCI_Auth_Manager", sizeof(atr.module_name) - 1);
        strncpy(atr.change_reason,
                "Successful login - failed attempt counter reset",
                sizeof(atr.change_reason) - 1);
        strncpy(atr.session_id, "-", sizeof(atr.session_id) - 1);
        strncpy(atr.client_ip, "-", sizeof(atr.client_ip) - 1);

        atr.col_names  = col_names;
        atr.col_types  = NULL;
        atr.new_values = new_values;
        atr.old_values = old_values;
        atr.row_count  = 1;
        atr.col_count  = 1;
        atr.audit_mode = AUDIT_MODE_FIELD;

        int audit_rc = audit_trail_insert(ctx, &atr);
        if (audit_rc != 0)
            logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                         "AUDIT_TRAIL insert failed (rc=%d) for user_id=%d "
                         "- successful-login bookkeeping is NOT rolled back",
                         audit_rc, user_id);
    }

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

    /* Metrics (2026-09-01) - deliberately built here, not at the very
     * top, and deliberately only finalised/enqueued (via the shared
     * finalise_and_enqueue_auth_metrics() helper below) from the
     * credential-denied point onward - the shared credential_ok
     * check, every failure point after it (session_create() failure,
     * session_id-parse failure, alloc failure), and this function's
     * own final AUTH_OK return - not at any of this function's
     * several EARLIER return points (invalid arg, unknown user,
     * disabled, locked, the initial lookup_user() DB failure). Those
     * earlier paths never call crypt_verify_password()/ldap_auth_
     * bind_check() at all, so ldap_bind_us/crypt_verify_us would
     * always be 0 for them - a metrics row there wouldn't answer the
     * actual question this was built for ("was it us or was it
     * LDAP"). Scoped deliberately, not an oversight.                  */
    metrics_record_t metrics;
    metrics_init(&metrics);
    metrics.start_time_us = metrics_now_us();
    strncpy(metrics.operation, "AUTHENTICATE", sizeof(metrics.operation) - 1);
    strncpy(metrics.object_name, req->username, sizeof(metrics.object_name) - 1);
    metrics_set_context(&metrics, ctx);

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
        free(user.ldap_configuration);
        return AUTH_ERR_DENIED;
    }

    if (strcmp(user.locked, "Y") == 0)
    {
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "DENIED username='%s' user_id=%d: account locked "
                     "(manual unlock only)", req->username, user.user_id);
        free(user.ldap_configuration);
        return AUTH_ERR_DENIED;
    }

    int credential_ok = 0;

    if (strcasecmp(user.source_type, "LOCAL") == 0)
    {
        uint64_t crypt_start = metrics_now_us();
        int verify_rc = crypt_verify_password(ctx, req->credential,
                                               user.password_hash);
        ctx->crypt_verify_us = metrics_now_us() - crypt_start;
        if (verify_rc == CRYPT_OK)
        {
            credential_ok = 1;
        }
        else
        {
            logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                         "DENIED username='%s' user_id=%d: bad credential",
                         req->username, user.user_id);
            record_auth_failure(ctx, user.user_id, req->username,
                                 user.failed_attempts,
                                 ctx->ini->auth_max_failed_attempts);
        }
    }
    else if (strcasecmp(user.source_type, "LDAP") == 0 ||
             strcasecmp(user.source_type, "AD") == 0)
    {
        /* Stage 3 (2026-08-29): delegated LDAP/AD authentication via a
         * real LDAP simple bind - see ldap_auth_helper.h/.c and
         * Security_Module_Design_Specification.docx Section 5.
         *
         * Deliberately NOT calling record_auth_failure() on a bind
         * failure here - Section 2 of the spec scopes local lockout
         * policy to "local-authentication attempts" specifically;
         * the directory server owns its own account-lockout policy
         * for LDAP/AD users, and duplicating that state locally in
         * APP_USER would create two independent, possibly
         * conflicting lockout mechanisms for the same identity. This
         * is a real design decision, not an oversight - worth
         * revisiting if local lockout tracking for LDAP/AD users
         * turns out to be wanted after all.                          */
        if (!user.ldap_configuration)
        {
            logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                         "DENIED username='%s' user_id=%d: source_type='%s' "
                         "but AUTH_SOURCE.CONFIGURATION is empty/NULL - "
                         "cannot build an LDAP connection", req->username,
                         user.user_id, user.source_type);
        }
        else
        {
            cJSON *config_json = cJSON_Parse(user.ldap_configuration);
            if (!config_json)
            {
                logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                             "DENIED username='%s' user_id=%d: "
                             "AUTH_SOURCE.CONFIGURATION is not valid JSON",
                             req->username, user.user_id);
            }
            else
            {
                cJSON *host_json    = cJSON_GetObjectItemCaseSensitive(config_json, "host");
                cJSON *port_json    = cJSON_GetObjectItemCaseSensitive(config_json, "port");
                cJSON *use_tls_json = cJSON_GetObjectItemCaseSensitive(config_json, "use_tls");
                cJSON *pattern_json = cJSON_GetObjectItemCaseSensitive(config_json, "bind_dn_pattern");

                if (!cJSON_IsString(host_json) || !host_json->valuestring ||
                    !cJSON_IsNumber(port_json) ||
                    !cJSON_IsString(pattern_json) || !pattern_json->valuestring)
                {
                    logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                                 "DENIED username='%s' user_id=%d: "
                                 "AUTH_SOURCE.CONFIGURATION missing required "
                                 "host/port/bind_dn_pattern fields",
                                 req->username, user.user_id);
                }
                else
                {
                    int use_tls = cJSON_IsTrue(use_tls_json);
                    char ldap_url[256];
                    snprintf(ldap_url, sizeof(ldap_url), "%s://%s:%d",
                             use_tls ? "ldaps" : "ldap",
                             host_json->valuestring, port_json->valueint);

                    /* bind_dn_pattern e.g. "uid=%s,ou=people,dc=example,dc=com"
                     * (OpenLDAP-style) or "%s@corp.local" (AD UPN-style) -
                     * exactly one %s, filled with the raw username. Not
                     * validated beyond snprintf's own bounds - a
                     * malformed pattern just produces a DN the
                     * directory itself will reject as invalid, which
                     * still folds into the same generic DENIED.       */
                    char bind_dn[384];
                    snprintf(bind_dn, sizeof(bind_dn),
                             pattern_json->valuestring, req->username);

                    char ldap_err[256] = {0};
                    uint64_t ldap_start = metrics_now_us();
                    int bind_rc = ldap_auth_bind_check(ldap_url, bind_dn,
                                                        req->credential,
                                                        ldap_err, sizeof(ldap_err));
                    ctx->ldap_bind_us = metrics_now_us() - ldap_start;
                    if (bind_rc == LDAP_AUTH_BIND_OK)
                    {
                        credential_ok = 1;
                    }
                    else
                    {
                        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                                     "DENIED username='%s' user_id=%d: LDAP "
                                     "bind failed against %s as '%s' (%s)",
                                     req->username, user.user_id, ldap_url,
                                     bind_dn, ldap_err[0] ? ldap_err : "unknown");
                    }
                }
                cJSON_Delete(config_json);
            }
        }
    }
    else
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "DENIED username='%s' user_id=%d: unrecognized "
                     "source_type='%s'", req->username, user.user_id,
                     user.source_type);
    }

    if (!credential_ok)
    {
        finalise_and_enqueue_auth_metrics(ctx, &metrics, AUTH_ERR_DENIED,
                                           "AUTH_ERR_DENIED",
                                           "Authentication denied");
        free(user.ldap_configuration);
        return AUTH_ERR_DENIED;
    }

    free(user.ldap_configuration);
    user.ldap_configuration = NULL;

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
        finalise_and_enqueue_auth_metrics(ctx, &metrics, AUTH_ERR_DB_FAILURE,
                                           "AUTH_ERR_DB_FAILURE",
                                           "session_create() failed");
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
        finalise_and_enqueue_auth_metrics(ctx, &metrics, AUTH_ERR_DB_FAILURE,
                                           "AUTH_ERR_DB_FAILURE",
                                           "session_id not parseable from session_create() result");
        return AUTH_ERR_DB_FAILURE;
    }

    *session_id_out   = strdup(session_id);
    *display_name_out = strdup(user.display_name[0] ? user.display_name
                                                      : req->username);
    *ttl_seconds_out  = ttl_str[0] ? atoi(ttl_str) : 0;

    if (!*session_id_out || !*display_name_out)
    {
        finalise_and_enqueue_auth_metrics(ctx, &metrics, AUTH_ERR_ALLOC,
                                           "AUTH_ERR_ALLOC",
                                           "strdup() failed");
        free(*session_id_out);
        free(*display_name_out);
        *session_id_out = NULL;
        *display_name_out = NULL;
        return AUTH_ERR_ALLOC;
    }

    /* Best-effort bookkeeping - see record_auth_success()'s own doc
     * comment on why this never turns a successful login into a
     * denial. */
    record_auth_success(ctx, user.user_id, req->username, user.failed_attempts);

    /* Stage 5: build the permission cache for this session, right here
     * at the point Security_Module_Design_Specification.docx Section
     * 6.6 means by "built once per session at session_create() time" -
     * this function already has user.user_id and *session_id_out at
     * hand, so there's no need to route this through OCI_Session_
     * Manager.c at all (which has no reason to know about ROLE/
     * PERMISSION tables). Same best-effort philosophy as record_auth_
     * success() immediately above - a failure here is logged but does
     * not turn this successful authentication into a denial; the
     * resulting session simply has no cached permissions (every
     * authz_has_permission() call for it will deny) until the user
     * logs in again.                                                 */
    int authz_rc = authz_build_permission_cache(ctx, *session_id_out,
                                                 user.user_id);
    if (authz_rc != AUTHZ_OK)
        logger_write(ctx->security_logger, LOG_WARN, __func__, 0,
                     "authz_build_permission_cache failed (rc=%d) for "
                     "session_id=%s user_id=%d - CHECK_PERMISSION will "
                     "deny every request on this session until next "
                     "login", authz_rc, *session_id_out, user.user_id);

    logger_write(ctx->security_logger, LOG_INFO, __func__, 0,
                 "SUCCESS username='%s' user_id=%d session_id=%s",
                 req->username, user.user_id, session_id);

    strncpy(metrics.session_id, *session_id_out, sizeof(metrics.session_id) - 1);
    finalise_and_enqueue_auth_metrics(ctx, &metrics, AUTH_OK, NULL, NULL);

    return AUTH_OK;
}
