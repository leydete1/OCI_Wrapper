/*
 * OCI_Level2_Parser.c
 *
 * See OCI_Level2_Parser.h for the full design description.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp() - find_column(), row_has_field() */

#include "Level2_Parser.h"
#include "logger.h"
#include "date_normalize.h"   /* date_normalize() - Stage 1, replaces the
                                  Oracle TO_DATE round trip            */
#include "db_type_class.h"    /* db_type_class() - DATE/TIMESTAMP class
                                  checks instead of type-name compares */
#include "sql_dependency_extractor.h"

#include "Insert_Execute_Module.h"   /* insert_request_t, insert_row_t   */
#include "Update_Execute_Module.h"   /* update_request_t                 */
#include "Delete_Execute_Module.h"   /* delete_request_t                 */
#include "Execute_Procedure_Module.h" /* execute_procedure_request_t,
                                              procedure_param_t,
                                              MAX_PROC_PARAMS              */
#include "Insert_Validate_Module.h"  /* parsed_field_t, validate_field() */
#include "Table_Metadata_Module.h"   /* col_metadata_t, metadata_request_t,
                                            MAX_TABLE_COLUMNS                */
#include "metadata_cache.h"
#include "metadata_cache_meta.h"         /* metadata_cache_get_or_fetch()    */
#include <Auth_Manager.h>            /* authenticate_request_t - new,
                                           * Security Module Stage 2         */
#include "DDL_Modules.h"             /* create_user_request_t,
                                             grant_request_t,
                                             create_table_request_t,
                                             drop_table_request_t,
                                             create_view_request_t,
                                             create_procedure_request_t and
                                             every validate_*_request() -
                                             merge of the six Independent
                                             DDL Module headers (2026-09-20
                                             follow-up proposal, connection
                                             consolidation + DDL merge item)
                                             - see OCI_DDL_Modules.h itself
                                             for the six original headers'
                                             design notes, preserved there
                                             verbatim under each module's
                                             own section. */
#include "Authz_Manager.h"           /* check_permission_request_t -
                                           * new, Security Module Stage 5    */

/* ------------------------------------------------------------------ */
/*  set_error / set_ok - fill error_detail consistently                 */
/*  Same convention as OCI_Level1_Parser.c - kept as a local static      */
/*  copy rather than a shared helper, matching how each parser module    */
/*  in this project stays a fully independent, standalone module.        */
/* ------------------------------------------------------------------ */
static void set_error(operation_status_t *error_detail, int code,
                       const char *err_code, const char *err_text)
{
    if (!error_detail) return;
    error_detail->status_code = code;
    strncpy(error_detail->error_code, err_code, sizeof(error_detail->error_code) - 1);
    error_detail->error_code[sizeof(error_detail->error_code) - 1] = '\0';
    strncpy(error_detail->error_text, err_text, sizeof(error_detail->error_text) - 1);
    error_detail->error_text[sizeof(error_detail->error_text) - 1] = '\0';
}

static void set_ok(operation_status_t *error_detail)
{
    if (!error_detail) return;
    error_detail->status_code = 0;
    strncpy(error_detail->error_code, "-", sizeof(error_detail->error_code) - 1);
    error_detail->error_code[sizeof(error_detail->error_code) - 1] = '\0';
    strncpy(error_detail->error_text, "-", sizeof(error_detail->error_text) - 1);
    error_detail->error_text[sizeof(error_detail->error_text) - 1] = '\0';
}

/*
 * copy_from_logger_last_error()
 *
 * extract_sql_dependencies() reports failure by calling
 * logger_write(ctx->select_logger, LOG_ERROR, ...) internally - it does
 * not return an error string of its own (OCI_DEPENDENCY_LIST has no
 * err_msg field despite the header comment mentioning one). But every
 * LOG_ERROR call already populates the project-wide logger_last_error
 * global as a side effect (see logger.c) - the same mechanism
 * metrics.c already relies on for metrics.error_code/error_text. Reuse
 * it here rather than inventing a second error-propagation path.
 *
 * error_code[64]/error_text[256] on operation_status_t and
 * logger_last_error_t are the same sizes - a direct copy is safe.
 */
static void copy_from_logger_last_error(operation_status_t *error_detail, int code)
{
    if (!error_detail) return;
    error_detail->status_code = code;
    strncpy(error_detail->error_code, logger_last_error.error_code,
            sizeof(error_detail->error_code) - 1);
    error_detail->error_code[sizeof(error_detail->error_code) - 1] = '\0';
    strncpy(error_detail->error_text, logger_last_error.error_text,
            sizeof(error_detail->error_text) - 1);
    error_detail->error_text[sizeof(error_detail->error_text) - 1] = '\0';
}

/* ================================================================== */
/*  level2_validate_select                                              */
/* ================================================================== */
int level2_validate_select(oci_context_t        *ctx,
                            input_c_operation_t  *op,
                            operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing SELECT payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    select_request_t *req = (select_request_t *)op->payload;

    if (!req->sql[0])
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Level 2: SELECT operation has empty sql");
        set_error(error_detail, LEVEL2_ERR_EMPTY_SQL, "LEVEL2_EMPTY_SQL",
                  "Request aborted. Level 2 validation failed - sql is empty.");
        return LEVEL2_ERR_EMPTY_SQL;
    }

    /* Stage 5 (2026-08-22) - execute_async/async_call_back_url. TLS-only,
     * no exceptions - same stance as HTTP consumer's own inbound
     * listener (Terry, 2026-08-21: "No one would implement or tolerate
     * unencrypted traffic today"). Only checked when execute_async=1 -
     * async_call_back_url is simply ignored on a normal synchronous
     * request, exactly like every other optional field in this codebase. */
    if (req->execute_async)
    {
        if (!req->async_call_back_url[0])
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "Level 2: execute_async=1 but async_call_back_url "
                         "is empty");
            set_error(error_detail, LEVEL2_ERR_ASYNC_INVALID, "LEVEL2_ASYNC_INVALID",
                      "Request aborted. Level 2 validation failed - "
                      "execute_async=1 requires a non-empty async_call_back_url.");
            return LEVEL2_ERR_ASYNC_INVALID;
        }

        if (strncasecmp(req->async_call_back_url, "https://", 8) != 0)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "Level 2: async_call_back_url is not https:// "
                         "('%s')", req->async_call_back_url);
            set_error(error_detail, LEVEL2_ERR_ASYNC_INVALID, "LEVEL2_ASYNC_INVALID",
                      "Request aborted. Level 2 validation failed - "
                      "async_call_back_url must be https:// - plaintext "
                      "callback URLs are not permitted.");
            return LEVEL2_ERR_ASYNC_INVALID;
        }
    }

    OCI_DEPENDENCY_LIST deps;
    memset(&deps, 0, sizeof(deps));

    if (extract_sql_dependencies(req->sql, &deps, ctx) != 0)
    {
        /* Failure already logged by extract_sql_dependencies() via
         * ctx->select_logger - logger_last_error was populated as a
         * side effect of that LOG_ERROR call.                          */
        copy_from_logger_last_error(error_detail, LEVEL2_ERR_SQL_INVALID);
        return LEVEL2_ERR_SQL_INVALID;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Level 2: SELECT validated OK - objects=%d fields=%d "
                 "needs_expansion=%d",
                 deps.object_count, deps.field_count, deps.needs_expansion);

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_insert - helpers                                    */
/* ================================================================== */

/* Case-insensitive lookup of one column's real metadata by name.
 * Returns NULL if col_name matches no column on the table - the
 * FIELD_UNKNOWN_COLUMN case.                                          */
static const col_metadata_t *
find_column(const col_metadata_t *cols, int col_count, const char *col_name)
{
    for (int i = 0; i < col_count; i++)
        if (strcasecmp(cols[i].col_name, col_name) == 0)
            return &cols[i];
    return NULL;
}

/* Does this row already have a (possibly empty) value for col_name? -
 * used by the FIELD_MISSING_REQUIRED_COLUMN check, which needs to know
 * whether a NOT NULL/no-default column was left out of the request
 * entirely, as distinct from being present with an empty value (that
 * second case is already FIELD_NULL_VIOLATION, from inside
 * validate_field() itself).                                           */
static int row_has_field(const insert_row_t *row, const char *col_name)
{
    for (int i = 0; i < row->field_count; i++)
        if (strcasecmp(row->fields[i].field_name, col_name) == 0)
            return 1;
    return 0;
}

/* Does row's set of field_names exactly match reference's set? Order-
 * independent - Stage 3's SQL builder looks columns up by name per
 * row, not by position, so two rows setting the same columns in a
 * different order are equally valid; only a genuinely different SET
 * of columns is a problem (see the Check 1b doc comment below for
 * why). O(n^2) in field count, which is fine - field counts here are
 * bounded by MAX_TABLE_COLUMNS, not by row_count.                     */
static int row_field_sets_match(const insert_row_t *reference, const insert_row_t *row)
{
    if (reference->field_count != row->field_count) return 0;

    for (int i = 0; i < reference->field_count; i++)
    {
        int found = 0;
        for (int j = 0; j < row->field_count; j++)
        {
            if (strcasecmp(reference->fields[i].field_name, row->fields[j].field_name) == 0)
            {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/*
 * normalize_client_date_value()
 *
 * Part of the 2026-07-27/28 date-handling design. For every DATE/
 * TIMESTAMP-class field, validates (and where needed, converts) the
 * value so that by the time build_insert_ctx_from_request()/
 * build_update_ctx_from_request()/build_delete_ctx_from_request() see
 * it, it is already in the one canonical shape:
 *
 *   DATE       YYYY-MM-DD HH:MM:SS
 *   TIMESTAMP  YYYY-MM-DD HH:MM:SS.ffffff
 *
 * If the client supplied a <client_date_format>, that is the SOURCE
 * format. If not, the value is validated against the canonical format
 * itself rather than assumed correct (the 2026-07-28 gap fix - this is
 * the one authoritative date check, for every date value, always).
 *
 * Stage 1 of the Oracle dialect extraction (2026-09-30): the check is
 * now plain C (date_normalize(), date_normalize.c) instead of a
 * SELECT TO_CHAR(TO_DATE(:1,:2),:3) FROM DUAL round trip per value per
 * pass. The parser reproduces TO_DATE/TO_TIMESTAMP's default matching
 * rules for the mask elements clients send; Driver_Level2Parser_Test.c
 * runs the old Oracle path and this one side by side over the same
 * corpus. Two client-visible differences, both intended:
 *   - rejection text keeps the prefix "Invalid date: value='...' does
 *     not match <fmt>='...'" but ends in a plain reason, e.g.
 *     "(not a valid month)", instead of "(ORA-nnnnn: ...)" (proposal
 *     decision 0.4);
 *   - a mask element outside the supported set (DY, J, FX, ...) is
 *     reported as "unsupported client_date_format" instead of being
 *     passed to Oracle.
 *
 * Since Stage 2 (2026-10-02) the driver converts these values with a
 * fixed ISO mask (db_dialect_t value_expr(), driver_oracle.c), so the
 * canonical form no longer depends on nls_date_format at all. The
 * Stage 1 guard that refused to run unless nls_date_format matched
 * has been removed; nls_date_format is now only the Oracle session's
 * display format for SELECT results. The rejection text still names
 * "nls_date_format" when no client format was given, so the message
 * prefix clients see is unchanged (proposal decision 0.4).
 *
 * client_date_format is deliberately mutable (not const) - see the
 * 2026-07-29 fix at the success path below: this function is called
 * twice per request (once from the dispatcher's own top-level
 * validation pass, again inside execute_insert_batch()/
 * execute_update_batch()/execute_delete_batch()'s own Stage 1 as
 * defense-in-depth), and rewrites value in place on success. Without
 * clearing client_date_format after that, the second pass would see an
 * already-canonical value but a stale format label and reject it -
 * exactly what a real end-to-end UPDATE test with a European
 * DD/MM/YYYY WHERE-key value hit on its first run. Unchanged in Stage 1.
 *
 * real_data_type is the REAL column type already resolved via
 * metadata_cache by the caller - never trust anything client-supplied
 * for this. Classed with db_type_class(): DATE -> date, anything
 * starting TIMESTAMP (including WITH [LOCAL] TIME ZONE) -> timestamp,
 * exactly as the old strcmp/strncmp checks did. Any other class is a
 * no-op - a format hint on a non-date column is ignored (logged).
 *
 * Returns  0 - validated (and normalised, if needed), or nothing to do
 *         -1 - the value does not match the format, or the format
 *              itself is unsupported.
 *              err_msg is populated; fail closed rather than let a bad
 *              date reach the database in an unpredictable shape.
 */
static int normalize_client_date_value(oci_context_t *ctx,
                                        logger_t      *op_logger,
                                        const char    *real_data_type,
                                        char          *client_date_format,
                                        char          *value,
                                        size_t         value_max,
                                        char          *err_msg,
                                        size_t         err_msg_max)
{
    db_type_class_t cls = db_type_class(real_data_type);
    int is_timestamp = (cls == DB_TYPE_CLASS_TIMESTAMP);
    int is_date      = (cls == DB_TYPE_CLASS_DATE);

    if (!is_date && !is_timestamp)
    {
        if (client_date_format && client_date_format[0])
            /* Format hint on a non-date column - not this function's
             * concern; ignore rather than risk a nonsensical conversion
             * attempt against an incompatible column type.            */
            logger_write(op_logger, LOG_WARN, __func__, 0,
                         "client_date_format='%s' supplied for a non-date "
                         "column (real type='%s') - ignored",
                         client_date_format, real_data_type);
        return 0;
    }

    if (!value[0])
        return 0;   /* empty value - a nullable/required-ness concern
                     * handled elsewhere, not a date-format one         */

    (void)ctx;   /* no longer needed here since Stage 2 - see doc comment */

    int has_client_fmt = (client_date_format && client_date_format[0]);
    const char *source_fmt = has_client_fmt
                              ? client_date_format
                              : (is_timestamp ? DATE_CANONICAL_MASK_TS
                                              : DATE_CANONICAL_MASK);

    char normalized[DATE_NORMALIZE_OUT_MAX];
    char reason[256];

    int rc = date_normalize(value, has_client_fmt ? client_date_format : NULL,
                            is_timestamp, normalized, sizeof(normalized),
                            reason, sizeof(reason));

    if (rc == DATE_NORM_BAD_MASK)
    {
        /* Keeps the "Invalid date" prefix clients already match on. */
        snprintf(err_msg, err_msg_max,
                 "Invalid date: unsupported client_date_format='%s' (%s)",
                 source_fmt, reason);
        logger_write(op_logger, LOG_ERROR, __func__, 0, "%s", err_msg);
        return -1;
    }

    if (rc != DATE_NORM_OK)
    {
        /* Same prefix as before Stage 1; plain reason instead of the
         * old "(ORA-nnnnn: ...)" suffix (proposal decision 0.4). */
        snprintf(err_msg, err_msg_max,
                 "Invalid date: value='%.80s' does not match "
                 "%s='%s' (%s)",
                 value,
                 has_client_fmt ? "client_date_format" : "nls_date_format",
                 source_fmt, reason);
        logger_write(op_logger, LOG_ERROR, __func__, 0, "%s", err_msg);
        return -1;
    }

    strncpy(value, normalized, value_max - 1);
    value[value_max - 1] = '\0';

    /* Logged before the clear below - source_fmt may point into
     * client_date_format. */
    logger_write(op_logger, LOG_DEBUG, __func__, 0,
                 "Normalized date value to '%s' (source format '%s', %s)",
                 value, source_fmt, is_timestamp ? "TIMESTAMP" : "DATE");

    /* Clear client_date_format after a successful conversion - see
     * this function's doc comment for the 2026-07-29 bug this fixes
     * (second validation pass re-reading an already-converted value
     * against the original, now stale, format).                       */
    if (client_date_format) client_date_format[0] = '\0';

    return 0;
}

/* ================================================================== */
/*  level2_validate_insert                                              */
/* ================================================================== */
int level2_validate_insert(oci_context_t        *ctx,
                            input_c_operation_t  *op,
                            operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing INSERT payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    insert_request_t *req = (insert_request_t *)op->payload;

    /* ---- Check 1: row_count vs max_bulk_inserts ----
     * The one check here that doesn't need a connection - both numbers
     * are already sitting in memory. Does NOT replace the equivalent
     * check inside execute_insert_batch() itself - see this function's
     * own doc comment in OCI_Level2_Parser.h.                          */
    int max_batch = ctx->ini ? ctx->ini->max_bulk_inserts : 0;

    if (req->row_count <= 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Level 2: INSERT has row_count=%d", req->row_count);
        set_error(error_detail, LEVEL2_ERR_ROW_COUNT_EXCEEDED, "LEVEL2_ROW_COUNT",
                  "Request aborted. Level 2 validation failed - at least one row is required.");
        return LEVEL2_ERR_ROW_COUNT_EXCEEDED;
    }

    if (max_batch > 0 && req->row_count > max_batch)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Level 2: INSERT row_count=%d exceeds max_bulk_inserts=%d",
                     req->row_count, max_batch);
        set_error(error_detail, LEVEL2_ERR_ROW_COUNT_EXCEEDED, "LEVEL2_ROW_COUNT",
                  "Request aborted. Level 2 validation failed - row_count exceeds max_bulk_inserts.");
        return LEVEL2_ERR_ROW_COUNT_EXCEEDED;
    }

    if (!req->table_name[0])
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Level 2: INSERT operation has empty table_name");
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - table_name is empty.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    /* ---- Check 1b: every row sets the same columns ----
     * A single bulk INSERT is one SQL statement with one fixed column
     * list, executed via OCI array binding - "INSERT INTO t (a,b,c)
     * VALUES (:1,:2,:3)" applied across every row in one execute call.
     * That's inherent to array binding, not a limitation this project
     * is choosing to impose: if two rows in one request genuinely need
     * different columns set, that's two separate INSERT statements,
     * not one. Order doesn't have to match between rows (Stage 3's SQL
     * builder looks columns up by name per row), but the SET of
     * columns does. Checked here, before the metadata_cache lookup,
     * since it's a pure struct comparison - no need to touch the
     * database to find out the request is already structurally
     * invalid.                                                        */
    for (int r = 1; r < req->row_count; r++)
    {
        if (!row_field_sets_match(&req->rows[0], &req->rows[r]))
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "Level 2: INSERT row %d's column set differs from row 1's - "
                         "a single bulk INSERT requires every row to set the same columns",
                         r + 1);
            set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                      "Request aborted. Level 2 validation failed - all rows in one "
                      "INSERT must set the same columns (row column set mismatch).");
            return LEVEL2_ERR_FIELD_INVALID;
        }
    }

    /* ---- Check 2: resolve real column metadata ----
     * Never trust anything the client sent - field_value_t carries no
     * metadata of its own by design (see field_value_t's doc comment
     * in OCI_Insert_Execute_Module.h). This is the one part of this
     * function that touches (or may touch, on a cache miss) the
     * database - the metadata cache is what makes that acceptable on
     * the common path, same reasoning as this header's own SELECT
     * row-count-guard carve-out, just cutting the other way here.      */
    col_metadata_t     cols[MAX_TABLE_COLUMNS];
    int                col_count = 0;
    metadata_request_t meta_req;

    memset(&meta_req, 0, sizeof(meta_req));
    strncpy(meta_req.table_name, req->table_name, sizeof(meta_req.table_name) - 1);
    strncpy(meta_req.owner,      req->owner,      sizeof(meta_req.owner)      - 1);

    metadata_cache_result_t meta_result;
    memset(&meta_result, 0, sizeof(meta_result));

    if (metadata_cache_get_or_fetch(ctx->metadata_cache, ctx, &meta_req,
                                     cols, &col_count, MAX_TABLE_COLUMNS,
                                     &meta_result) != 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Level 2: metadata_cache_get_or_fetch failed for %s.%s",
                     req->owner, req->table_name);
        set_error(error_detail, LEVEL2_ERR_METADATA_LOOKUP, "LEVEL2_METADATA_LOOKUP",
                  "Request aborted. Level 2 validation failed - could not resolve "
                  "column metadata for the target table.");
        return LEVEL2_ERR_METADATA_LOOKUP;
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Level 2: resolved %d columns for %s.%s (cache_hit=%d)",
                 col_count, req->owner, req->table_name, meta_result.was_cache_hit);

    /* ---- Checks 3 & 4: every row, every field ----
     * First failure wins, same fail-fast convention as
     * validate_insert_template() and level2_validate_select() - full
     * detail logged for every field regardless, only the first
     * surfaced to the caller.                                         */
    int failures = 0;
    char first_failure_msg[512] = {0};

    for (int r = 0; r < req->row_count; r++)
    {
        const insert_row_t *row = &req->rows[r];

        for (int f = 0; f < row->field_count; f++)
        {
            field_value_t *fv = &row->fields[f];

            const col_metadata_t *col = find_column(cols, col_count, fv->field_name);

            if (!col)
            {
                failures++;
                char msg[512];
                snprintf(msg, sizeof(msg),
                         "Row %d, field '%s': no such column on %s.%s",
                         r + 1, fv->field_name, req->owner, req->table_name);
                logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0, "%s", msg);
                if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
                continue;
            }

            /* Normalize a client-declared date format into this
             * project's one canonical format BEFORE anything else
             * reads this field's value - see normalize_client_date_
             * value()'s own doc comment for the full 2026-07-27
             * design. Runs for every DATE/TIMESTAMP-class value,
             * with or without fv->client_date_format.                   */
            {
                char date_err[512];
                if (normalize_client_date_value(ctx, ctx->insert_logger,
                                                col->data_type,
                                                fv->client_date_format,
                                                fv->value, sizeof(fv->value),
                                                date_err, sizeof(date_err)) != 0)
                {
                    failures++;
                    char msg[512];
                    snprintf(msg, sizeof(msg), "Row %d, field '%s': %s",
                             r + 1, fv->field_name, date_err);
                    if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
                    continue;
                }
            }

            parsed_field_t pf;
            memset(&pf, 0, sizeof(pf));
            pf.field_number = f + 1;
            strncpy(pf.field_name,     fv->field_name,      sizeof(pf.field_name)      - 1);
            strncpy(pf.field_type,     col->data_type,       sizeof(pf.field_type)      - 1);
            pf.field_length    = col->data_length;
            pf.field_precision = col->data_precision;
            pf.field_scale     = col->data_scale;
            strncpy(pf.field_nullable, col->nullable,        sizeof(pf.field_nullable)  - 1);
            strncpy(pf.field_default,  col->data_default,    sizeof(pf.field_default)   - 1);
            strncpy(pf.insert_value,   field_value_get(fv), sizeof(pf.insert_value)    - 1);
            /* pf.insert_value is itself only char[1024] - a further
             * truncation is possible here for a CLOB value longer than
             * that, but validate_field()'s CLOB path only checks
             * presence/absence, not exact length, so this doesn't lose
             * anything validation actually needs. The untruncated
             * value (field_value_get(fv)) is what actually reaches
             * Stage 3's build_insert_ctx_from_request() - this
             * parsed_field_t copy is for validation only.               */

            char field_msg[512] = {0};
            field_validation_result_t result = validate_field(ctx, &pf, field_msg, sizeof(field_msg));

            if (result == FIELD_VALID)
            {
                logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                             "Row %d, field '%s': VALID", r + 1, fv->field_name);
            }
            else
            {
                failures++;
                char msg[512];
                snprintf(msg, sizeof(msg), "Row %d: %s", r + 1, field_msg);
                logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                             "Row %d, field '%s' FAILED (result=%d): %s",
                             r + 1, fv->field_name, result, field_msg);
                if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
            }
        }

        /* ---- Check 4: NOT NULL columns with no default, omitted entirely ----
         * The old client-echoes-everything model never needed this -
         * every column always had a <field> block whether the client
         * was setting it or not. The new slim wire format only sends
         * columns the client actually wants to set, so a NOT NULL
         * column left out completely needs catching here, or Oracle
         * would reject the whole INSERT later as ORA-01400 instead of
         * a clean validation failure now.                             */
        for (int c = 0; c < col_count; c++)
        {
            if (cols[c].nullable[0] != 'N') continue;          /* nullable - fine to omit */
            if (cols[c].data_default[0] != '\0') continue;     /* has a default - fine to omit */
            if (row_has_field(row, cols[c].col_name)) continue; /* present - handled above */

            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "Row %d: NOT NULL column '%s' (no default) was not "
                     "supplied", r + 1, cols[c].col_name);
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
        }
    }

    if (failures > 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Level 2: INSERT validation failed - %d failure(s), "
                     "first: %s", failures, first_failure_msg);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  first_failure_msg);
        return LEVEL2_ERR_FIELD_INVALID;
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Level 2: INSERT validated OK - table=%s.%s rows=%d",
                 req->owner, req->table_name, req->row_count);

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_update                                              */
/* ================================================================== */
int level2_validate_update(oci_context_t        *ctx,
                            input_c_operation_t  *op,
                            operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing UPDATE payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    update_request_t *req = (update_request_t *)op->payload;

    if (!req->table_name[0])
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Level 2: UPDATE operation has empty table_name");
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - table_name is empty.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    /* ---- Check: at least one WHERE key ----
     * An UPDATE with no WHERE clause matches every row in the table -
     * almost certainly a client mistake, and a dangerous one to let
     * through silently. Requiring at least one key here is deliberate,
     * not an arbitrary restriction - a client that genuinely needs to
     * update every row can still do so by supplying a key that's
     * always true (e.g. a primary key IS NOT NULL check upstream of
     * this), but the common case of "forgot the WHERE clause" fails
     * loudly here instead of silently rewriting the whole table.       */
    if (req->key_count <= 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Level 2: UPDATE has no WHERE keys - refusing to "
                     "risk a whole-table update");
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - at least one "
                  "WHERE key is required.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    /* ---- Check: at least one SET field ----
     * An UPDATE with nothing to set is meaningless - almost certainly
     * a malformed request rather than an intentional no-op.            */
    if (req->field_count <= 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Level 2: UPDATE has no SET fields");
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - at least one "
                  "SET field is required.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    /* ---- Resolve real column metadata ----
     * Same reasoning as level2_validate_insert()'s own Check 2 - never
     * trust anything the client sent.                                  */
    col_metadata_t     cols[MAX_TABLE_COLUMNS];
    int                col_count = 0;
    metadata_request_t meta_req;

    memset(&meta_req, 0, sizeof(meta_req));
    strncpy(meta_req.table_name, req->table_name, sizeof(meta_req.table_name) - 1);
    strncpy(meta_req.owner,      req->owner,      sizeof(meta_req.owner)      - 1);

    metadata_cache_result_t meta_result;
    memset(&meta_result, 0, sizeof(meta_result));

    if (metadata_cache_get_or_fetch(ctx->metadata_cache, ctx, &meta_req,
                                     cols, &col_count, MAX_TABLE_COLUMNS,
                                     &meta_result) != 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Level 2: metadata_cache_get_or_fetch failed for %s.%s",
                     req->owner, req->table_name);
        set_error(error_detail, LEVEL2_ERR_METADATA_LOOKUP, "LEVEL2_METADATA_LOOKUP",
                  "Request aborted. Level 2 validation failed - could not resolve "
                  "column metadata for the target table.");
        return LEVEL2_ERR_METADATA_LOOKUP;
    }

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Level 2: resolved %d columns for %s.%s (cache_hit=%d)",
                 col_count, req->owner, req->table_name, meta_result.was_cache_hit);

    int  failures = 0;
    char first_failure_msg[512] = {0};

    /* ---- Validate WHERE keys: field_name must be a real column,
     * key_value must be non-empty (a predicate needs an actual value
     * to match against). No validate_field() type/length checking
     * here deliberately - that's about SET clause correctness (data
     * being written), not WHERE predicate correctness; a WHERE value
     * is compared, not stored, so length/nullable/precision limits
     * that apply to writing a column don't apply to matching one.      */
    for (int k = 0; k < req->key_count; k++)
    {
        where_key_t *wk = &req->keys[k];

        const col_metadata_t *col = find_column(cols, col_count, wk->field_name);
        if (!col)
        {
            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "WHERE key '%s': no such column on %s.%s",
                     wk->field_name, req->owner, req->table_name);
            logger_write(ctx->update_logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
            continue;
        }

        {
            char date_err[512];
            if (normalize_client_date_value(ctx, ctx->update_logger,
                                            col->data_type,
                                            wk->client_date_format,
                                            wk->key_value, sizeof(wk->key_value),
                                            date_err, sizeof(date_err)) != 0)
            {
                failures++;
                char msg[512];
                snprintf(msg, sizeof(msg), "WHERE key '%s': %s",
                         wk->field_name, date_err);
                if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
                continue;
            }
        }

        if (!wk->key_value[0])
        {
            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "WHERE key '%s': key_value is empty", wk->field_name);
            logger_write(ctx->update_logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
        }
    }

    /* ---- Validate SET fields: field_name must be a real column,
     * then validate_field() - same rules as INSERT's Stage 3/4, reused
     * unchanged. No "missing required column" check here (unlike
     * INSERT's Check 4) - an UPDATE only touches the columns actually
     * listed in SET; the row already exists with everything else
     * already populated, so there's no equivalent "was a NOT NULL
     * column omitted" concern for UPDATE at all.                       */
    for (int f = 0; f < req->field_count; f++)
    {
        field_value_t *fv = &req->fields[f];

        const col_metadata_t *col = find_column(cols, col_count, fv->field_name);

        if (!col)
        {
            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "SET field '%s': no such column on %s.%s",
                     fv->field_name, req->owner, req->table_name);
            logger_write(ctx->update_logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
            continue;
        }

        {
            char date_err[512];
            if (normalize_client_date_value(ctx, ctx->update_logger,
                                            col->data_type,
                                            fv->client_date_format,
                                            fv->value, sizeof(fv->value),
                                            date_err, sizeof(date_err)) != 0)
            {
                failures++;
                char msg[512];
                snprintf(msg, sizeof(msg), "SET field '%s': %s",
                         fv->field_name, date_err);
                if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
                continue;
            }
        }

        parsed_field_t pf;
        memset(&pf, 0, sizeof(pf));
        pf.field_number = f + 1;
        strncpy(pf.field_name,     fv->field_name,   sizeof(pf.field_name)     - 1);
        strncpy(pf.field_type,     col->data_type,    sizeof(pf.field_type)     - 1);
        pf.field_length    = col->data_length;
        pf.field_precision = col->data_precision;
        pf.field_scale     = col->data_scale;
        strncpy(pf.field_nullable, col->nullable,     sizeof(pf.field_nullable) - 1);
        strncpy(pf.field_default,  col->data_default, sizeof(pf.field_default)  - 1);
        strncpy(pf.insert_value,   field_value_get(fv), sizeof(pf.insert_value) - 1);

        char field_msg[512] = {0};
        field_validation_result_t result = validate_field(ctx, &pf, field_msg, sizeof(field_msg));

        if (result == FIELD_VALID)
        {
            logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                         "SET field '%s': VALID", fv->field_name);
        }
        else
        {
            failures++;
            logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                         "SET field '%s' FAILED (result=%d): %s",
                         fv->field_name, result, field_msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", field_msg);
        }
    }

    if (failures > 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Level 2: UPDATE validation failed - %d failure(s), "
                     "first: %s", failures, first_failure_msg);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  first_failure_msg);
        return LEVEL2_ERR_FIELD_INVALID;
    }

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Level 2: UPDATE validated OK - table=%s.%s keys=%d fields=%d",
                 req->owner, req->table_name, req->key_count, req->field_count);

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_delete                                              */
/* ================================================================== */
int level2_validate_delete(oci_context_t        *ctx,
                            input_c_operation_t  *op,
                            operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing DELETE payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    delete_request_t *req = (delete_request_t *)op->payload;

    if (!req->table_name[0])
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Level 2: DELETE operation has empty table_name");
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - table_name is empty.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    /* ---- Check: at least one WHERE key ----
     * Same reasoning as level2_validate_update()'s own equivalent check
     * - a DELETE with no WHERE clause matches (and removes) every row
     * in the table. Refused outright rather than silently allowed;
     * almost certainly a client mistake, not an intentional whole-
     * table delete, and there is no equivalent of "at least it's
     * reversible" the way an accidental whole-table UPDATE at least
     * leaves the rows in place - an accidental whole-table DELETE does
     * not.                                                              */
    if (req->key_count <= 0)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Level 2: DELETE has no WHERE keys - refusing to "
                     "risk a whole-table delete");
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - at least one "
                  "WHERE key is required.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    /* ---- Resolve real column metadata ----
     * Needed here for the same reason as UPDATE's WHERE keys - the new
     * where_key_t carries no type information at all, so the real
     * column type (for TO_DATE()/TO_TIMESTAMP()/etc bind wrapping in
     * build_delete_ctx_from_request()) has to come from metadata_cache,
     * never from the client. The pre-refactor version of this module
     * trusted a client-supplied field_type for exactly this - see this
     * file's own design note in OCI_Delete_Execute_Module.h.           */
    col_metadata_t     cols[MAX_TABLE_COLUMNS];
    int                col_count = 0;
    metadata_request_t meta_req;

    memset(&meta_req, 0, sizeof(meta_req));
    strncpy(meta_req.table_name, req->table_name, sizeof(meta_req.table_name) - 1);
    strncpy(meta_req.owner,      req->owner,      sizeof(meta_req.owner)      - 1);

    metadata_cache_result_t meta_result;
    memset(&meta_result, 0, sizeof(meta_result));

    if (metadata_cache_get_or_fetch(ctx->metadata_cache, ctx, &meta_req,
                                     cols, &col_count, MAX_TABLE_COLUMNS,
                                     &meta_result) != 0)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Level 2: metadata_cache_get_or_fetch failed for %s.%s",
                     req->owner, req->table_name);
        set_error(error_detail, LEVEL2_ERR_METADATA_LOOKUP, "LEVEL2_METADATA_LOOKUP",
                  "Request aborted. Level 2 validation failed - could not resolve "
                  "column metadata for the target table.");
        return LEVEL2_ERR_METADATA_LOOKUP;
    }

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Level 2: resolved %d columns for %s.%s (cache_hit=%d)",
                 col_count, req->owner, req->table_name, meta_result.was_cache_hit);

    int  failures = 0;
    char first_failure_msg[512] = {0};

    /* ---- Validate WHERE keys: field_name must be a real column,
     * key_value must be non-empty. No SET-field validation at all -
     * DELETE has no SET clause, so there is nothing to run
     * validate_field() against; the WHERE-key checks below are this
     * function's entire scope, same reasoning as
     * level2_validate_update()'s own WHERE-key checks.                 */
    for (int k = 0; k < req->key_count; k++)
    {
        where_key_t *wk = &req->keys[k];

        const col_metadata_t *col = find_column(cols, col_count, wk->field_name);
        if (!col)
        {
            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "WHERE key '%s': no such column on %s.%s",
                     wk->field_name, req->owner, req->table_name);
            logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
            continue;
        }

        {
            char date_err[512];
            if (normalize_client_date_value(ctx, ctx->delete_logger,
                                            col->data_type,
                                            wk->client_date_format,
                                            wk->key_value, sizeof(wk->key_value),
                                            date_err, sizeof(date_err)) != 0)
            {
                failures++;
                char msg[512];
                snprintf(msg, sizeof(msg), "WHERE key '%s': %s",
                         wk->field_name, date_err);
                if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
                continue;
            }
        }

        if (!wk->key_value[0])
        {
            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "WHERE key '%s': key_value is empty", wk->field_name);
            logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
        }
    }

    if (failures > 0)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Level 2: DELETE validation failed - %d failure(s), "
                     "first: %s", failures, first_failure_msg);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  first_failure_msg);
        return LEVEL2_ERR_FIELD_INVALID;
    }

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Level 2: DELETE validated OK - table=%s.%s keys=%d",
                 req->owner, req->table_name, req->key_count);

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_procedure                                           */
/*  Deliberately light - see this function's own doc comment in         */
/*  OCI_Level2_Parser.h for why (no metadata_cache equivalent exists     */
/*  for a procedure's own parameter signature the way it does for a      */
/*  table's columns).                                                    */
/* ================================================================== */
int level2_validate_procedure(oci_context_t        *ctx,
                               input_c_operation_t  *op,
                               operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "EXECUTE_PROCEDURE payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    execute_procedure_request_t *req = (execute_procedure_request_t *)op->payload;

    if (!req->procedure_name[0])
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 2: EXECUTE_PROCEDURE operation has empty "
                     "procedure_name");
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - "
                  "procedure_name is empty.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    if (req->param_count < 0 || req->param_count > MAX_PROC_PARAMS)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 2: EXECUTE_PROCEDURE param_count=%d out of "
                     "range (0..%d)", req->param_count, MAX_PROC_PARAMS);
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - "
                  "param_count out of range.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    int  failures = 0;
    char first_failure_msg[512] = {0};

    /* ---- Per-parameter structural checks ----
     * No validate_field()-style type/length checking here at all -
     * there is no real column metadata to validate a procedure
     * parameter against, only what the client itself declared. This is
     * the one place in the whole project where a client-declared type
     * is trusted rather than resolved against something authoritative,
     * simply because there is nothing authoritative available to
     * resolve it against - see this function's own doc comment in
     * OCI_Level2_Parser.h.                                             */
    for (int i = 0; i < req->param_count; i++)
    {
        const procedure_param_t *pp = &req->parameters[i];

        if (!pp->param_name[0])
        {
            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "Parameter %d: param_name is empty", i + 1);
            logger_write(ctx->logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
            continue;
        }

        int is_recognised_type =
            (strcasecmp(pp->param_type, "NUMBER")    == 0 ||
             strcasecmp(pp->param_type, "INTEGER")   == 0 ||
             strcasecmp(pp->param_type, "VARCHAR2")  == 0 ||
             strcasecmp(pp->param_type, "DATE")      == 0 ||
             strcasecmp(pp->param_type, "TIMESTAMP") == 0 ||
             strcasecmp(pp->param_type, "CURSOR")    == 0);

        if (!is_recognised_type)
        {
            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "Parameter '%s': param_type '%s' is not one of "
                     "NUMBER/INTEGER/VARCHAR2/DATE/TIMESTAMP/CURSOR",
                     pp->param_name, pp->param_type);
            logger_write(ctx->logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
            continue;
        }

        /* CURSOR is always OUT - a REFCURSOR can't be passed in, and
         * doesn't round-trip as IN_OUT either (there is nothing
         * meaningful the caller could pass in for it).                 */
        if (strcasecmp(pp->param_type, "CURSOR") == 0 &&
            pp->direction != PARAM_DIR_OUT)
        {
            failures++;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "Parameter '%s': CURSOR parameters must be OUT "
                     "(a cursor cannot be passed in)", pp->param_name);
            logger_write(ctx->logger, LOG_ERROR, __func__, 0, "%s", msg);
            if (failures == 1) snprintf(first_failure_msg, sizeof(first_failure_msg), "%s", msg);
        }
    }

    if (failures > 0)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 2: EXECUTE_PROCEDURE validation failed - "
                     "%d failure(s), first: %s", failures, first_failure_msg);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  first_failure_msg);
        return LEVEL2_ERR_FIELD_INVALID;
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Level 2: EXECUTE_PROCEDURE validated OK - proc='%s' "
                 "params=%d", req->procedure_name, req->param_count);

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_authenticate                                        */
/*  Security Module Stage 2 (2026-08-27). Deliberately shallow - just   */
/*  "are both fields present" - everything else (does the user exist,   */
/*  is the credential correct, is the account locked/disabled) is       */
/*  auth_authenticate()'s job (OCI_Auth_Manager.c), not Level 2's,       */
/*  exactly as SQL correctness is execute_query_batch()'s job rather    */
/*  than level2_validate_select()'s.                                    */
/* ================================================================== */
int level2_validate_authenticate(oci_context_t        *ctx,
                                  input_c_operation_t  *op,
                                  operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "AUTHENTICATE payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    authenticate_request_t *req = (authenticate_request_t *)op->payload;

    if (!req->username[0])
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "Level 2: AUTHENTICATE operation has empty username");
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  "Request aborted. Level 2 validation failed - username "
                  "is required.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    if (!req->credential[0])
    {
        /* Deliberately does not echo username here or anywhere in this
         * error path beyond the internal log line above - an empty-
         * credential rejection and a wrong-credential rejection should
         * look the same to anything downstream of Level 2 too.        */
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "Level 2: AUTHENTICATE operation has empty credential "
                     "(username='%s')", req->username);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  "Request aborted. Level 2 validation failed - credential "
                  "is required.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_check_permission                                     */
/*  Security Module Stage 5 (2026-08-31). Same shallow-validation      */
/*  philosophy as level2_validate_authenticate() above - "is           */
/*  permission_code present", nothing more. The actual authorization   */
/*  decision (is this permission granted for this session) is          */
/*  authz_has_permission()'s job (OCI_Authz_Manager.c), not Level 2's.  */
/* ================================================================== */
int level2_validate_check_permission(oci_context_t        *ctx,
                                      input_c_operation_t  *op,
                                      operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "CHECK_PERMISSION payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    check_permission_request_t *req = (check_permission_request_t *)op->payload;

    if (!req->permission_code[0])
    {
        logger_write(ctx->security_logger, LOG_ERROR, __func__, 0,
                     "Level 2: CHECK_PERMISSION operation has empty "
                     "permission_code");
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  "Request aborted. Level 2 validation failed - "
                  "permission_code is required.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_create_user                                          */
/*  Independent DDL Module proposal (03-Sep), first operation. Thin      */
/*  adapter over validate_create_user_request() - see this function's    */
/*  own doc comment in OCI_Level2_Parser.h for why the actual field      */
/*  rules live in OCI_DDL_Create_User_Module.c rather than here.         */
/* ================================================================== */
int level2_validate_create_user(oci_context_t        *ctx,
                                 input_c_operation_t  *op,
                                 operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "CREATE_USER payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    create_user_request_t *req = (create_user_request_t *)op->payload;
    char error_buf[256] = {0};

    if (validate_create_user_request(ctx, req, error_buf, sizeof(error_buf)) != 0)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Level 2: CREATE_USER validation failed - %s", error_buf);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  error_buf[0] ? error_buf :
                  "Request aborted. Level 2 validation failed - invalid "
                  "CREATE_USER fields.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_grant                                                 */
/*  Independent DDL Module proposal (03-Sep), second operation. Thin      */
/*  adapter over validate_grant_request() - same shape as                */
/*  level2_validate_create_user() above.                                  */
/* ================================================================== */
int level2_validate_grant(oci_context_t        *ctx,
                           input_c_operation_t  *op,
                           operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "GRANT payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    grant_request_t *req = (grant_request_t *)op->payload;
    char error_buf[256] = {0};

    if (validate_grant_request(ctx, req, error_buf, sizeof(error_buf)) != 0)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Level 2: GRANT validation failed - %s", error_buf);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  error_buf[0] ? error_buf :
                  "Request aborted. Level 2 validation failed - invalid "
                  "GRANT fields.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_create_table                                         */
/*  Independent DDL Module proposal (03-Sep), third operation. Thin       */
/*  adapter over validate_create_table_request() - same shape as          */
/*  level2_validate_create_user()/level2_validate_grant() above.          */
/* ================================================================== */
int level2_validate_create_table(oci_context_t        *ctx,
                                  input_c_operation_t  *op,
                                  operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "CREATE_TABLE payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    create_table_request_t *req = (create_table_request_t *)op->payload;
    char error_buf[256] = {0};

    if (validate_create_table_request(ctx, req, error_buf, sizeof(error_buf)) != 0)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Level 2: CREATE_TABLE validation failed - %s", error_buf);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  error_buf[0] ? error_buf :
                  "Request aborted. Level 2 validation failed - invalid "
                  "CREATE_TABLE fields.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_drop_table                                            */
/*  Independent DDL Module proposal (03-Sep), fourth operation. Thin      */
/*  adapter over validate_drop_table_request() - same shape as the        */
/*  other three DDL validators above.                                     */
/* ================================================================== */
int level2_validate_drop_table(oci_context_t        *ctx,
                                input_c_operation_t  *op,
                                operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "DROP_TABLE payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    drop_table_request_t *req = (drop_table_request_t *)op->payload;
    char error_buf[256] = {0};

    if (validate_drop_table_request(ctx, req, error_buf, sizeof(error_buf)) != 0)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Level 2: DROP_TABLE validation failed - %s", error_buf);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  error_buf[0] ? error_buf :
                  "Request aborted. Level 2 validation failed - invalid "
                  "DROP_TABLE fields.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_create_view                                          */
/*  Independent DDL Module proposal (03-Sep), fifth operation. Thin       */
/*  adapter over validate_create_view_request().                          */
/* ================================================================== */
int level2_validate_create_view(oci_context_t        *ctx,
                                 input_c_operation_t  *op,
                                 operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "CREATE_VIEW payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    create_view_request_t *req = (create_view_request_t *)op->payload;
    char error_buf[256] = {0};

    if (validate_create_view_request(ctx, req, error_buf, sizeof(error_buf)) != 0)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Level 2: CREATE_VIEW validation failed - %s", error_buf);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  error_buf[0] ? error_buf :
                  "Request aborted. Level 2 validation failed - invalid "
                  "CREATE_VIEW fields.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate_create_procedure                                     */
/*  Independent DDL Module proposal (03-Sep), sixth and final operation. */
/*  Thin adapter over validate_create_procedure_request().               */
/* ================================================================== */
int level2_validate_create_procedure(oci_context_t        *ctx,
                                      input_c_operation_t  *op,
                                      operation_status_t   *error_detail)
{
    if (!ctx || !op || !op->payload)
    {
        set_error(error_detail, LEVEL2_ERR_INVALID_ARG, "LEVEL2_INVALID_ARG",
                  "Request aborted. Level 2 validation failed - missing "
                  "CREATE_PROCEDURE payload.");
        return LEVEL2_ERR_INVALID_ARG;
    }

    create_procedure_request_t *req = (create_procedure_request_t *)op->payload;
    char error_buf[256] = {0};

    if (validate_create_procedure_request(ctx, req, error_buf, sizeof(error_buf)) != 0)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Level 2: CREATE_PROCEDURE validation failed - %s", error_buf);
        set_error(error_detail, LEVEL2_ERR_FIELD_INVALID, "LEVEL2_FIELD_INVALID",
                  error_buf[0] ? error_buf :
                  "Request aborted. Level 2 validation failed - invalid "
                  "CREATE_PROCEDURE fields.");
        return LEVEL2_ERR_FIELD_INVALID;
    }

    set_ok(error_detail);
    return LEVEL2_OK;
}

/* ================================================================== */
/*  level2_validate                                                      */
/* ================================================================== */
int level2_validate(oci_context_t *ctx, input_c_request_t *request)
{
    if (!ctx || !request)
        return LEVEL2_ERR_INVALID_ARG;

    for (int i = 0; i < request->operation_count; i++)
    {
        input_c_operation_t *op = &request->operations[i];

        switch (op->type)
        {
            case OP_SELECT:
            {
                /* Stage 5 (2026-08-22) - execute_async=1 is only valid
                 * when this SELECT is the ONLY operation in its
                 * transaction. Streaming batches mid-transaction while
                 * other operations are still pending doesn't have a
                 * coherent meaning - what would "async batching"
                 * signify if the whole transaction hasn't committed
                 * yet, and what happens to already-streamed batches if
                 * a later operation fails and the transaction rolls
                 * back? Checked here, not in level2_validate_select()
                 * itself, since only this dispatch loop knows
                 * request->operation_count. */
                select_request_t *sel_req = (select_request_t *)op->payload;
                if (sel_req && sel_req->execute_async && request->operation_count > 1)
                {
                    logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                                 "Level 2: execute_async=1 on a SELECT that "
                                 "is not the only operation in its "
                                 "transaction (operation_count=%d)",
                                 request->operation_count);
                    set_error(&op->validation_status, LEVEL2_ERR_ASYNC_INVALID,
                              "LEVEL2_ASYNC_INVALID",
                              "Request aborted. Level 2 validation failed - "
                              "execute_async=1 is only valid when the SELECT "
                              "is the only operation in its transaction.");
                    break;
                }
                level2_validate_select(ctx, op, &op->validation_status);
                break;
            }

            case OP_INSERT:
                level2_validate_insert(ctx, op, &op->validation_status);
                break;

            case OP_UPDATE:
                level2_validate_update(ctx, op, &op->validation_status);
                break;

            case OP_DELETE:
                level2_validate_delete(ctx, op, &op->validation_status);
                break;

            case OP_EXECUTE_PROCEDURE:
                level2_validate_procedure(ctx, op, &op->validation_status);
                break;

            case OP_AUTHENTICATE:
                level2_validate_authenticate(ctx, op, &op->validation_status);
                break;

            case OP_CHECK_PERMISSION:
                level2_validate_check_permission(ctx, op, &op->validation_status);
                break;

            case OP_CREATE_USER:
                level2_validate_create_user(ctx, op, &op->validation_status);
                break;

            case OP_GRANT:
                level2_validate_grant(ctx, op, &op->validation_status);
                break;

            case OP_CREATE_TABLE:
                level2_validate_create_table(ctx, op, &op->validation_status);
                break;

            case OP_DROP_TABLE:
                level2_validate_drop_table(ctx, op, &op->validation_status);
                break;

            case OP_CREATE_VIEW:
                level2_validate_create_view(ctx, op, &op->validation_status);
                break;

            case OP_CREATE_PROCEDURE:
                level2_validate_create_procedure(ctx, op, &op->validation_status);
                break;

            /* Not yet implemented - fail closed. An operation type with
             * no validator here must never silently proceed to the
             * CRUD layer unvalidated.                                  */
            case OP_GET_TEMPLATE:
            case OP_CREATE_SESSION:
            case OP_END_SESSION:
            case OP_UNKNOWN:
            default:
                logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                             "Level 2: no validator implemented yet for "
                             "operation[%d] type=%d", i, (int)op->type);
                set_error(&op->validation_status, LEVEL2_ERR_NOT_IMPLEMENTED,
                          "LEVEL2_NOT_IMPLEMENTED",
                          "Request aborted. Level 2 validation not yet "
                          "implemented for this operation type.");
                break;
        }

        if (op->validation_status.status_code != 0)
            return LEVEL2_ERR_VALIDATION_FAILED;
    }

    return LEVEL2_OK;
}
