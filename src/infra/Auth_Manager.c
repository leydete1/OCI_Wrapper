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
 *
 * Oracle dialect extraction, Stage 5 (2026-10-08): no OCI calls here
 * any more. The APP_USER x AUTH_SOURCE lookup runs through the
 * driver's select cursor (select_open() with a bound username and
 * text_lobs_inline for the CONFIGURATION CLOB); the two APP_USER
 * updates run through dml_execute() and commit through
 * tx_commit_with_retry(). Bind placeholders and the server clock come
 * from the driver's dialect (bind_placeholder(), now_expr()). These
 * are deliberately NOT routed through execute_query_batch()/
 * execute_update_batch(): those write their own AUDIT_TRAIL rows,
 * which would duplicate the explicit audit_trail_insert() calls below.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <Auth_Manager.h>
#include "Session_Manager.h"
#include "crypt_helper.h"                 /* crypt_verify_password() - Stage 1 */
#include "ldap_auth_helper.h"             /* ldap_auth_bind_check() - Stage 3 */
#include "cJSON.h"                        /* AUTH_SOURCE.CONFIGURATION parsing */
#include <Audit_Trail_Manager.h>      /* audit_trail_insert() - Stage 4 */
#include <Authz_Manager.h>          /* authz_build_permission_cache() -
                                            * Stage 5 */
#include "metrics.h"                      /* metrics_record_t, metrics_now_us() -
                                            * timing metrics, 2026-09-01 */
#include "metrics_writer.h"               /* metrics_finalise_and_enqueue() */
#include "logger.h"
#include "ini_reader.h"                   /* app_config_t - ctx->ini->auth_* */
#include "db_driver.h"                    /* Stage 5 - select cursor,
                                            * dml_execute(), dialect     */
#include "Resultset_Builder.h"            /* resultset_free()            */
#include "Transaction_Manager.h"          /* tx_commit_with_retry()      */

/* NOTE (2026-08-27): auth_max_failed_attempts is now read from
 * ctx->ini->auth_max_failed_attempts (ini_reader.h/.c, config.ini) -
 * the local #define default this replaced is gone. No other TODO
 * remains from the previous revision of this file.                    */

/* ------------------------------------------------------------------ */
/*  Stage 5 helpers (2026-10-08) - replace the CHECK_OCI_AUTH macro.    */
/* ------------------------------------------------------------------ */

/* Bind placeholder for 1-based position pos, from the driver's dialect
 * (Oracle ":1"). Logs and returns -1 on failure. */
static int auth_placeholder(oci_context_t *ctx, int pos,
                            char *out, size_t out_max)
{
    if (db_driver_get(ctx)->dialect->bind_placeholder(pos, out, out_max) != 0)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "dialect bind_placeholder(%d) failed", pos);
        return -1;
    }
    return 0;
}

/* Whole number from a fetched field; "" (NULL) and junk are errors. */
static int auth_parse_int(const char *text, int *out)
{
    char *end = NULL;
    long  v;
    if (!text || !text[0]) return -1;
    v = strtol(text, &end, 10);
    while (end && *end == ' ') end++;
    if (end == text || (end && *end != '\0')) return -1;
    *out = (int)v;
    return 0;
}

/* Copies src into dst[dst_size]; -1 (dst untouched) if it does not fit. */
static int auth_copy(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_size) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

/*
 * auth_update_and_commit()
 *
 * Runs one APP_USER UPDATE through dml_execute() and commits it through
 * tx_commit_with_retry() - the same commit path, retry policy and log
 * lines as every other standalone commit (Stage 3/3c). Before Stage 5
 * the commit was a bare OCITransCommit() whose result was never
 * checked. The driver logs the vendor error detail on
 * ctx->security_logger.
 *
 * If the commit fails, the transaction is rolled back (best effort, a
 * no-op if the server already did) so that no uncommitted APP_USER
 * change is left on this session for a later commit to pick up.
 *
 * Returns 0 committed, -1 the UPDATE failed, -2 the commit failed.
 */
static int auth_update_and_commit(oci_context_t *ctx, const char *sql,
                                  const char **bind_values, int bind_count)
{
    const db_driver_t *driver = db_driver_get(ctx);

    db_dml_request_t req;
    memset(&req, 0, sizeof(req));
    req.sql         = sql;
    req.bind_count  = bind_count;
    req.bind_values = bind_values;

    int rows = 0;
    if (driver->dml_execute(ctx, ctx->security_logger, &req, &rows,
                            NULL, 0) != 0)
        return -1;

    if (tx_commit_with_retry(ctx, ctx->security_logger,
                             ctx->ini ? ctx->ini->tx_max_retries    : 0,
                             ctx->ini ? ctx->ini->tx_retry_delay_ms : 0,
                             NULL) != 0)
    {
        driver->rollback(ctx, ctx->security_logger);
        return -2;
    }
    return 0;
}

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
 * DB failure.
 *
 * Stage 5 (2026-10-08): runs through the driver's select cursor. The
 * username is bound, never concatenated. The cursor reports a NULL
 * column as "", so DISPLAY_NAME / PASSWORD_HASH / CONFIGURATION (the
 * only nullable columns read here) need no indicators - the ORA-01405
 * fixes this function used to carry are now the driver's job.
 * CONFIGURATION is read with text_lobs_inline, so the JSON comes back
 * in the field itself (no CLOB file is written); "" (NULL or empty)
 * leaves ldap_configuration NULL, exactly as before. The driver logs
 * the vendor detail of a failure on ctx->select_logger; this function
 * adds one line to ctx->security_logger.
 */
static int lookup_user(oci_context_t *ctx, const char *username,
                        auth_user_row_t *out)
{
    out->ldap_configuration = NULL;

    const db_driver_t *driver = db_driver_get(ctx);

    char ph[16];
    if (auth_placeholder(ctx, 1, ph, sizeof(ph)) != 0)
        return -1;

    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT u.USER_ID, u.DISPLAY_NAME, u.PASSWORD_HASH, "
        "       u.ENABLED, u.LOCKED, u.FAILED_ATTEMPTS, s.SOURCE_TYPE, "
        "       s.CONFIGURATION "
        "FROM   APP_USER u "
        "JOIN   AUTH_SOURCE s ON s.AUTH_SOURCE_ID = u.AUTH_SOURCE_ID "
        "WHERE  UPPER(u.USERNAME) = UPPER(%s)", ph);

    const char *binds[1] = { username };

    db_select_request_t req;
    memset(&req, 0, sizeof(req));
    req.sql              = sql;
    req.max_rows         = 1;
    req.fetch_array_size = 1;
    req.bind_count       = 1;
    req.bind_values      = binds;
    req.text_lobs_inline = 1;

    db_select_cursor_t *cursor     = NULL;
    db_column_meta_t   *columns    = NULL;
    int                 col_count  = 0;
    int                 batch_size = 0;

    if (driver->select_open(ctx, &req, &cursor, &columns,
                            &col_count, &batch_size) != 0)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "user lookup failed for username='%s' (select_open) "
                     "- see the select log for the database error",
                     username);
        return -1;
    }

    int          rc   = -1;
    resultset_t *rs   = NULL;
    int          rows = 0;

    if (col_count != 8)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "user lookup returned %d columns, expected 8",
                     col_count);
        goto Done;
    }

    if (driver->select_fetch_batch(cursor, &rs, &rows, NULL) != 0)
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "user lookup failed for username='%s' (fetch) - see "
                     "the select log for the database error", username);
        goto Done;
    }

    if (rows == 0)
    {
        rc = 0;   /* no such user - not a DB failure */
        goto Done;
    }

    {
        const resultset_field_t *f = rs->records[0].fields;

        if (auth_parse_int(f[0].value, &out->user_id) != 0 ||
            auth_parse_int(f[5].value, &out->failed_attempts) != 0)
        {
            logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                         "user lookup for username='%s': USER_ID '%s' or "
                         "FAILED_ATTEMPTS '%s' is not a whole number",
                         username, f[0].value, f[5].value);
            goto Done;
        }

        /* A value too long for its buffer is an error, never cut short
         * (the old OCI defines raised ORA-01406 in the same case). The
         * schema sizes all fit: VARCHAR2(255) / (1) / (20).            */
        if (auth_copy(out->display_name,  sizeof(out->display_name),  f[1].value) ||
            auth_copy(out->password_hash, sizeof(out->password_hash), f[2].value) ||
            auth_copy(out->enabled,       sizeof(out->enabled),       f[3].value) ||
            auth_copy(out->locked,        sizeof(out->locked),        f[4].value) ||
            auth_copy(out->source_type,   sizeof(out->source_type),   f[6].value))
        {
            logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                         "user lookup for username='%s': a column value is "
                         "longer than its buffer", username);
            goto Done;
        }

        if (f[7].value[0])
        {
            out->ldap_configuration = strdup(f[7].value);
            if (!out->ldap_configuration)
            {
                logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                             "strdup failed for AUTH_SOURCE.CONFIGURATION");
                goto Done;
            }
        }
        rc = 1;
    }

Done:
    if (rs) resultset_free(rs);
    driver->select_close(cursor);
    free(columns);
    return rc;
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
    int new_failed_attempts = current_failed_attempts + 1;
    int should_lock = (new_failed_attempts >= max_failed_attempts);

    /* Stage 5: placeholders and server clock from the driver's dialect
     * (Oracle ":1"/":2" and SYSTIMESTAMP - same statement as before). */
    char p1[16], p2[16];
    if (auth_placeholder(ctx, 1, p1, sizeof(p1)) != 0 ||
        auth_placeholder(ctx, 2, p2, sizeof(p2)) != 0)
    {
        db_failure = 1;
        goto Cleanup;
    }
    const char *now = db_driver_get(ctx)->dialect->now_expr();

    char sql[512];
    if (should_lock)
        snprintf(sql, sizeof(sql),
            "UPDATE APP_USER "
            "SET    FAILED_ATTEMPTS = %s, "
            "       LOCKED = 'Y', LOCKED_TS = %s, "
            "       MODIFIED_TS = %s "
            "WHERE  USER_ID = %s", p1, now, now, p2);
    else
        snprintf(sql, sizeof(sql),
            "UPDATE APP_USER "
            "SET    FAILED_ATTEMPTS = %s, "
            "       MODIFIED_TS = %s "
            "WHERE  USER_ID = %s", p1, now, p2);

    char failed_str[16], user_id_bind[16];
    snprintf(failed_str,   sizeof(failed_str),   "%d", new_failed_attempts);
    snprintf(user_id_bind, sizeof(user_id_bind), "%d", user_id);
    const char *binds[2] = { failed_str, user_id_bind };

    int upd_rc = auth_update_and_commit(ctx, sql, binds, 2);
    if (upd_rc == -2)
    {
        /* Stage 5: the change was not saved, so no audit row is
         * written for it (before Stage 5 the commit was unchecked and
         * the audit row was written regardless). */
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "record_auth_failure: commit failed for user_id=%d "
                     "- FAILED_ATTEMPTS not persisted, no AUDIT_TRAIL row "
                     "written", user_id);
        return;
    }
    if (upd_rc != 0)
    {
        db_failure = 1;
        goto Cleanup;
    }

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

    /* Stage 5: placeholder and server clock from the driver's dialect. */
    char p1[16];
    if (auth_placeholder(ctx, 1, p1, sizeof(p1)) != 0)
    {
        db_failure = 1;
        goto Cleanup;
    }
    const char *now = db_driver_get(ctx)->dialect->now_expr();

    char sql[512];
    snprintf(sql, sizeof(sql),
        "UPDATE APP_USER "
        "SET    FAILED_ATTEMPTS = 0, LAST_LOGIN_TS = %s, "
        "       MODIFIED_TS = %s "
        "WHERE  USER_ID = %s", now, now, p1);

    char user_id_bind[16];
    snprintf(user_id_bind, sizeof(user_id_bind), "%d", user_id);
    const char *binds[1] = { user_id_bind };

    int upd_rc = auth_update_and_commit(ctx, sql, binds, 1);
    if (upd_rc == -2)
    {
        /* Stage 5: not saved, so no audit row - see record_auth_failure(). */
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "record_auth_success: commit failed for user_id=%d "
                     "- FAILED_ATTEMPTS/LAST_LOGIN_TS not persisted, no "
                     "AUDIT_TRAIL row written (session was still created)",
                     user_id);
        return;
    }
    if (upd_rc != 0)
    {
        db_failure = 1;
        goto Cleanup;
    }

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
