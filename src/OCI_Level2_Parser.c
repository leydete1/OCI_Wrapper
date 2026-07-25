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

#include "OCI_Level2_Parser.h"
#include "logger.h"
#include "sql_dependency_extractor.h"

#include "OCI_Insert_Execute_Module.h"   /* insert_request_t, insert_row_t   */
#include "OCI_Update_Execute_Module.h"   /* update_request_t                 */
#include "OCI_Insert_Validate_Module.h"  /* parsed_field_t, validate_field() */
#include "OCI_Table_Metadata_Module.h"   /* col_metadata_t, metadata_request_t,
                                            MAX_TABLE_COLUMNS                */
#include "metadata_cache.h"
#include "metadata_cache_meta.h"         /* metadata_cache_get_or_fetch()    */

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
            const field_value_t *fv = &row->fields[f];

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
        const where_key_t *wk = &req->keys[k];

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
        const field_value_t *fv = &req->fields[f];

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
                level2_validate_select(ctx, op, &op->validation_status);
                break;

            case OP_INSERT:
                level2_validate_insert(ctx, op, &op->validation_status);
                break;

            case OP_UPDATE:
                level2_validate_update(ctx, op, &op->validation_status);
                break;

            /* Not yet implemented - fail closed. An operation type with
             * no validator here must never silently proceed to the
             * CRUD layer unvalidated.                                  */
            case OP_DELETE:
            case OP_GET_TEMPLATE:
            case OP_EXECUTE_PROCEDURE:
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
