/*
 * OCI_Delete_Execute_Module.c
 *
 * Stage 3 - Delete Execute Module
 * --------------------------------
 * Executes a DELETE from an already-validated delete_request_t (built
 * by Level 1). Based on OCI_Update_Execute_Module with the SET clause
 * removed entirely.
 *
 * level2_validate_delete() is called internally as this function's own
 * first step - same reasoning as execute_insert_batch()/
 * execute_update_batch(), see execute_insert_batch()'s own doc comment
 * in OCI_Insert_Execute_Module.h.
 *
 * Internal structure
 * ------------------
 *   build_delete_ctx_from_request() - populate delete_ctx_t from
 *                                      delete_request_t (replaces the
 *                                      old parse_delete_xml() - no XML
 *                                      parsing happens in this file at
 *                                      all any more)
 *   build_delete_sql()       - build DELETE FROM ... WHERE key=:1 AND ...
 *   execute_delete_batch()   - orchestrate: validate -> build context ->
 *                              before-image + audit -> bind -> execute
 *                              -> commit -> result -> cleanup
 *
 * Key differences from update - genuinely simpler
 * ------------------------------------------------
 *   - No <row>/<set> at all, no SET columns, no LOB handling.
 *   - WHERE key column types now resolved via metadata_cache (added as
 *     part of this refactor) - the pre-refactor version of this module
 *     trusted a client-supplied field_type for this; the new
 *     where_key_t carries no type information at all, matching the
 *     "client sends field_name+value, server resolves everything else"
 *     design used throughout this project.
 *   - WHERE bind positions start at :1 (not after a SET list).
 *   - Empty WHERE is rejected by level2_validate_delete() before any
 *     OCI call.
 *
 * Audit trail - added as part of this refactor, not carried over
 * ------------------------------------------------------------------
 * The pre-refactor version of this module had NO audit trail
 * integration at all. A before-image SELECT
 * (audit_trail_fetch_before_image(), reused unchanged from UPDATE)
 * captures the WHERE-key columns' values, and
 * audit_trail_insert_delete() (OCI_Audit_Trail_Manager.c) writes one
 * AUDIT_TRAIL row per matched row per WHERE-key column - both BEFORE
 * the actual DELETE statement executes, so the attempt is captured
 * even if Oracle itself then rejects the DELETE for lack of privilege.
 * See OCI_Delete_Execute_Module.h's own doc comment for the full GxP
 * reasoning.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "OCI_Delete_Execute_Module.h"
#include "OCI_Connection.h"
#include "oci_cache.h"
#include "OCI_Table_Metadata_Module.h"
#include "metadata_cache.h"
#include "metadata_cache_meta.h"
#include "OCI_Level2_Parser.h"          /* level2_validate_delete()      */
#include "OCI_Response_Writer.h"        /* response_write_dml_xml/json() */
#include "OCI_Audit_Trail_Manager.h"
#include "XML_Helper.h"
#include "logger.h"
#include "metrics.h"
#include "metrics_writer.h"   /* metrics_finalise_and_enqueue() - closure item 5, Stage 2 */
#include "OCI_Transaction_Manager.h"

/* ------------------------------------------------------------------ */
/*  OCI error macro - same pattern as rest of project                  */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_DEL(errhp, status, ctx, label)                        \
    do {                                                                 \
        if ((status) != OCI_SUCCESS &&                                  \
            (status) != OCI_SUCCESS_WITH_INFO)                          \
        {                                                                \
            text   _errbuf[512];                                         \
            sb4    _errcode = 0;                                         \
            OCIErrorGet((errhp), 1, NULL, &_errcode,                    \
                        _errbuf, sizeof(_errbuf), OCI_HTYPE_ERROR);     \
            logger_write((ctx)->logger, LOG_ERROR, __func__, 0,         \
                         "OCI Error %d: %s", _errcode,                  \
                         (char *)_errbuf);                               \
            rc = -1;                                                     \
            goto label;                                                  \
        }                                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define MAX_DEL_KEY_COLS     32
#define MAX_COL_VALUE_SIZE   32768

/* ------------------------------------------------------------------ */
/*  Parsed WHERE key field                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    char field_name[128];
    char field_type[128];
    char key_value [MAX_COL_VALUE_SIZE];
} del_key_field_t;

/* ------------------------------------------------------------------ */
/*  Parsed delete context                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    int              key_count;
    char             table_name[128];
    char             owner     [128];
    del_key_field_t  keys[MAX_DEL_KEY_COLS];
} delete_ctx_t;

/* ------------------------------------------------------------------ */
/*  Static helpers                                                      */
/* ------------------------------------------------------------------ */
/*  build_delete_ctx_from_request                                       */
/*  Populates delete_ctx_t directly from an already-parsed               */
/*  delete_request_t - replaces the old parse_delete_xml(); no XML       */
/*  parsing happens in this file at all any more.                        */
/*                                                                         */
/*  Also resolves each WHERE key's real column type via cols[] (already   */
/*  fetched by execute_delete_batch() via metadata_cache before calling   */
/*  this) - the new where_key_t carries no type information at all,       */
/*  unlike the old client-supplied <field_type>, so this is the one       */
/*  place that type gets attached, for build_delete_sql()'s              */
/*  TO_DATE()/TO_TIMESTAMP()/etc bind wrapping.                           */
/* ------------------------------------------------------------------ */
static int build_delete_ctx_from_request(oci_context_t          *ctx,
                                          const delete_request_t *req,
                                          const col_metadata_t   *cols,
                                          int                     col_count,
                                          delete_ctx_t           *dc)
{
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Entering build_delete_ctx_from_request");

    memset(dc, 0, sizeof(*dc));

    strncpy(dc->table_name, req->table_name, sizeof(dc->table_name) - 1);
    strncpy(dc->owner,      req->owner,      sizeof(dc->owner) - 1);

    /* level2_validate_delete() already checked key_count > 0 and range
     * bounds - this is defense-in-depth, same reasoning as INSERT/
     * UPDATE's equivalent re-checks in their own build_*_ctx_from_
     * request() functions.                                             */
    if (req->key_count <= 0 || req->key_count > MAX_DEL_KEY_COLS)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "key_count=%d out of range (1..%d) - "
                     "level2_validate_delete() should have caught this",
                     req->key_count, MAX_DEL_KEY_COLS);
        return -1;
    }
    dc->key_count = req->key_count;

    for (int k = 0; k < req->key_count; k++)
    {
        del_key_field_t *kf = &dc->keys[k];

        strncpy(kf->field_name, req->keys[k].field_name,
                sizeof(kf->field_name) - 1);
        strncpy(kf->key_value, req->keys[k].key_value,
                sizeof(kf->key_value) - 1);

        /* Resolve real type from metadata_cache - never trust the
         * client (there is nothing to trust here any more anyway,
         * where_key_t has no type field at all).                      */
        strncpy(kf->field_type, "VARCHAR2", sizeof(kf->field_type) - 1);
        for (int m = 0; m < col_count; m++)
        {
            if (strcasecmp(cols[m].col_name, kf->field_name) == 0)
            {
                strncpy(kf->field_type, cols[m].data_type,
                        sizeof(kf->field_type) - 1);
                break;
            }
        }

        logger_write(ctx->delete_logger, LOG_DEBUG, __func__, 0,
                     "Key field %d: name='%s' type='%s' value='%s'",
                     k + 1, kf->field_name, kf->field_type, kf->key_value);
    }

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "build_delete_ctx_from_request OK: table='%s' owner='%s' "
                 "keys=%d",
                 dc->table_name, dc->owner, dc->key_count);
    return 0;
}

/* ================================================================== */
/*  get_del_key_wrapper                                                 */
/*  Returns a SQL conversion wrapper for date/time key types.          */
/*  Plain scalar types return NULL (bind as SQLT_STR directly).        */
/* ================================================================== */
/*
 * get_del_key_wrapper()
 * Same design as OCI_Insert_Execute_Module.c's get_bind_wrapper() -
 * see that function's own doc comment for the full 2026-07-28
 * reasoning (no hardcoded date format literal any more; reads
 * ctx->ini->nls_date_format fresh on every call instead).
 */
static int get_del_key_wrapper(oci_context_t *ctx, const char *dtype,
                                char *dest, size_t dest_max)
{
    if (strcmp(dtype, "DATE") == 0)
    {
        snprintf(dest, dest_max, "TO_DATE(%%s,'%s')", ctx->ini->nls_date_format);
        return 1;
    }
    if (strncmp(dtype, "TIMESTAMP", 9) == 0)
    {
        snprintf(dest, dest_max, "TO_TIMESTAMP(%%s,'%s.FF6')", ctx->ini->nls_date_format);
        return 1;
    }
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "MONTH"))
    {
        snprintf(dest, dest_max, "TO_YMINTERVAL(%%s)");
        return 1;
    }
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "SECOND"))
    {
        snprintf(dest, dest_max, "TO_DSINTERVAL(%%s)");
        return 1;
    }
    return 0;   /* VARCHAR2, NUMBER, CHAR, RAW, etc. - no wrapper */
}

/* ================================================================== */
/*  build_delete_sql                                                    */
/*  Produces:                                                           */
/*    DELETE FROM [owner.]table WHERE key1=:1 AND key2=:2 ...          */
/*  Date/timestamp keys are wrapped with the appropriate Oracle        */
/*  conversion function so no NLS session dependency exists.           */
/* ================================================================== */
static int build_delete_sql(oci_context_t      *ctx,
                              const delete_ctx_t *dc,
                              char               *sql_buf,
                              size_t              sql_max)
{
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Building DELETE SQL table='%s'", dc->table_name);

    char where_list[MAX_DEL_KEY_COLS * 256] = {0};

    for (int k = 0; k < dc->key_count; k++)
    {
        if (k > 0)
            strncat(where_list, " AND ",
                    sizeof(where_list) - strlen(where_list) - 1);

        char bind_ph[16];
        snprintf(bind_ph, sizeof(bind_ph), ":%d", k + 1);

        char wrapper_buf[128] = {0};
        int  has_wrapper = get_del_key_wrapper(ctx, dc->keys[k].field_type,
                                                wrapper_buf, sizeof(wrapper_buf));
        const char *wrapper = has_wrapper ? wrapper_buf : NULL;
        char cond[256] = {0};

        if (wrapper)
        {
            char expr[128] = {0};
            snprintf(expr, sizeof(expr), wrapper, bind_ph);
            snprintf(cond, sizeof(cond),
                     "%s=%s", dc->keys[k].field_name, expr);
        }
        else
        {
            snprintf(cond, sizeof(cond),
                     "%s=%s", dc->keys[k].field_name, bind_ph);
        }

        strncat(where_list, cond,
                sizeof(where_list) - strlen(where_list) - 1);
    }

    int n;
    if (strlen(dc->owner) > 0)
        n = snprintf(sql_buf, sql_max,
                     "DELETE FROM %s.%s WHERE %s",
                     dc->owner, dc->table_name, where_list);
    else
        n = snprintf(sql_buf, sql_max,
                     "DELETE FROM %s WHERE %s",
                     dc->table_name, where_list);

    if (n < 0 || (size_t)n >= sql_max)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "DELETE SQL truncated - increase sql_buf size");
        return -1;
    }

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "DELETE SQL: %s", sql_buf);
    return 0;
}

/* ================================================================== */
/*  execute_delete_batch - main entry point                            */
/* ================================================================== */
int execute_delete_batch(oci_context_t     *ctx,
                          delete_request_t  *req,
                          execute_config_t  *cfg)
{
    int            rc           = 0;
    OCIStmt       *stmt         = NULL;
    xml_builder_t *xml          = NULL;
    delete_ctx_t  *dc           = NULL;
    OCIBind      **bind_hdls    = NULL;
    char         **scalar_bufs  = NULL;
    sb2           *indicators   = NULL;
    int            rows_deleted = 0;
    struct timespec ts_start, ts_end;
    audit_old_value_t *old_values    = NULL;  /* before-image for audit */
    int                old_row_count = 0;

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Entering execute_delete_batch");

    if (!ctx || !req || !cfg)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments");
        return -1;
    }

    /* Give this call its own transaction identity if it doesn't already
     * have one - fixes the 2026-07-26 GxP traceability gap. See
     * execute_insert_batch()'s identical fix and the full reasoning in
     * OCI_Transaction_Manager.h's own doc comment for these functions.
     * Particularly relevant here: the before-image SELECT, the
     * AUDIT_TRAIL INSERT, and the DELETE itself are three statements
     * that need to be traceable as one unit even when this call runs
     * standalone.                                                       */
    tx_handle_t local_tx;
    int owns_standalone_tx = begin_standalone_tx_if_needed(ctx, &local_tx);

    metrics_record_t metrics;
    metrics_init(&metrics);
    metrics_set_context(&metrics, ctx);
    metrics.start_time_us = metrics_now_us();
    strncpy(metrics.operation, "DELETE", sizeof(metrics.operation) - 1);

    /* Set transaction_id immediately so every write path carries it  */
           if (ctx->active_tx)
               strncpy(metrics.transaction_id,
                       tx_get_id(ctx->active_tx),
                       sizeof(metrics.transaction_id) - 1);
           else
               strncpy(metrics.transaction_id, "-",
                       sizeof(metrics.transaction_id) - 1);


    /* ================================================================
     *  Stage 1 - Validate
     *  Called internally rather than trusted to have already run in
     *  the caller - see this file's own top-of-file doc comment.
     * ================================================================ */
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Validating request");

    input_c_operation_t validate_op;
    memset(&validate_op, 0, sizeof(validate_op));
    validate_op.type    = OP_DELETE;
    validate_op.payload = (void *)req;

    operation_status_t val_status;
    memset(&val_status, 0, sizeof(val_status));

    if (level2_validate_delete(ctx, &validate_op, &val_status) != LEVEL2_OK)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Stage 1 validation failed: %s", val_status.error_text);
        end_standalone_tx_if_owned(ctx, owns_standalone_tx);
        return -1;
    }
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 1 validation passed");

    /* ================================================================
     *  Stage 2 - Resolve column metadata, build context and SQL
     * ================================================================ */
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 2: Resolving metadata and preparing statement");

    dc = calloc(1, sizeof(delete_ctx_t));
    if (!dc)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for delete_ctx_t");
        rc = -1;
        goto Cleanup;
    }

    /* ---- Resolve real column metadata ----
     * Added as part of this refactor - the pre-refactor version of
     * this module had no metadata_cache lookup at all, trusting a
     * client-supplied field_type instead. See this file's own top
     * comment.                                                         */
    col_metadata_t     cols[MAX_TABLE_COLUMNS];
    int                col_meta_count = 0;
    metadata_request_t meta_req;

    memset(&meta_req, 0, sizeof(meta_req));
    strncpy(meta_req.table_name, req->table_name, sizeof(meta_req.table_name) - 1);
    strncpy(meta_req.owner,      req->owner,      sizeof(meta_req.owner)      - 1);

    metadata_cache_result_t meta_result;
    memset(&meta_result, 0, sizeof(meta_result));

    if (metadata_cache_get_or_fetch(ctx->metadata_cache, ctx, &meta_req,
                                     cols, &col_meta_count, MAX_TABLE_COLUMNS,
                                     &meta_result) != 0)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "metadata_cache_get_or_fetch failed");
        rc = -1;
        goto Cleanup;
    }

    metrics.cache_hit       = meta_result.was_cache_hit;
    metrics.cache_lookup_us = meta_result.cache_lookup_us;
    metrics.cache_key_hash  = meta_result.cache_key_hash;

    if (build_delete_ctx_from_request(ctx, req, cols, col_meta_count, dc) != 0)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "build_delete_ctx_from_request failed");
        rc = -1;
        goto Cleanup;
    }
    strncpy(metrics.object_name, dc->table_name,
             sizeof(metrics.object_name) - 1);

    char sql_buf[8192] = {0};
    if (build_delete_sql(ctx, dc, sql_buf, sizeof(sql_buf)) != 0)
    {
        rc = -1;
        goto Cleanup;
    }

    /* sql_hash: hash the built SQL for traceability in metrics        */
    if (ctx->metadata_cache)
        metrics.sql_hash = cache_hash_string(ctx->metadata_cache, sql_buf);

    CHECK_OCI_DEL(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql_buf, (ub4)strlen(sql_buf),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "OCIStmtPrepare2 OK");

    /* ================================================================
     *  Stage 2 Audit - Before-image + AUDIT_TRAIL write, BEFORE the
     *  actual DELETE executes.
     *
     *  Scoped to the WHERE-key columns only (2026-07-26 design
     *  decision) - not every column on the table. Written before the
     *  DELETE runs so the attempt is captured even if Oracle itself
     *  then rejects the DELETE for lack of privilege - see
     *  OCI_Delete_Execute_Module.h's own doc comment for the full GxP
     *  reasoning.
     * ================================================================ */
    if (!audit_trail_in_progress)
    {
        char (*key_names)[128]   = NULL;
        char (*key_vals) [32768] = NULL;
        char (*key_types)[128]   = NULL;

        key_names = calloc((size_t)dc->key_count, sizeof(*key_names));
        key_vals  = calloc((size_t)dc->key_count, sizeof(*key_vals));
        key_types = calloc((size_t)dc->key_count, sizeof(*key_types));

        if (key_names && key_vals && key_types)
        {
            for (int k = 0; k < dc->key_count; k++)
            {
                strncpy(key_names[k], dc->keys[k].field_name,
                        sizeof(key_names[k]) - 1);
                strncpy(key_vals[k],  dc->keys[k].key_value,
                        sizeof(key_vals[k])  - 1);
                /* dc->keys[k].field_type is already the real, resolved
                 * type - build_delete_ctx_from_request() populates it
                 * from cols[] directly, never from the client.         */
                strncpy(key_types[k], dc->keys[k].field_type,
                        sizeof(key_types[k]) - 1);
            }

            /* col_names == key_names here deliberately - the columns
             * being captured for audit ARE the WHERE-key columns
             * themselves, per the 2026-07-26 design decision.          */
            int fetch_rc =
                audit_trail_fetch_before_image(ctx,
                                               dc->table_name,
                                               dc->owner,
                                               key_names,
                                               dc->key_count,
                                               key_names,
                                               key_vals,
                                               key_types,
                                               dc->key_count,
                                               &old_values,
                                               &old_row_count);
            if (fetch_rc != 0)
            {
                logger_write(ctx->delete_logger, LOG_WARN, __func__, 0,
                             "Before-image fetch failed (rc=%d) for "
                             "table='%s' - DELETE will proceed but "
                             "audit trail will be unavailable",
                             fetch_rc, dc->table_name);
            }
            else
            {
                logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                             "Before-image captured: %d row(s) "
                             "for table='%s'",
                             old_row_count, dc->table_name);

                audit_trail_request_t atr;
                memset(&atr, 0, sizeof(atr));

                strncpy(atr.table_name,  dc->table_name,
                        sizeof(atr.table_name)  - 1);
                strncpy(atr.changed_by,  ctx->ini->username,
                        sizeof(atr.changed_by)  - 1);
                strncpy(atr.module_name, "OCI_Delete_Execute",
                        sizeof(atr.module_name) - 1);

                if (ctx->active_tx && ctx->active_tx->tx_name[0] &&
                    strcmp(ctx->active_tx->tx_name, "-") != 0)
                    strncpy(atr.change_reason, ctx->active_tx->tx_name,
                            sizeof(atr.change_reason) - 1);
                else
                    strncpy(atr.change_reason, "Business DELETE via Data_Manager",
                            sizeof(atr.change_reason) - 1);

                strncpy(atr.record_id, dc->keys[0].key_value,
                        sizeof(atr.record_id) - 1);

                atr.col_names  = key_names;
                atr.col_types  = cols;
                atr.old_values = NULL;   /* supplied via old_values arg below */
                atr.new_values = NULL;   /* deleted - no new value, ever      */
                atr.col_count  = dc->key_count;
                atr.row_count  = old_row_count;

                int audit_rc = audit_trail_insert_delete(ctx, &atr, old_values);
                if (audit_rc != 0)
                    logger_write(ctx->delete_logger, LOG_WARN, __func__, 0,
                                 "Audit trail DELETE failed (rc=%d) for "
                                 "table='%s' - DELETE will proceed anyway",
                                 audit_rc, dc->table_name);
            }
        }

        free(key_names);
        free(key_vals);
        free(key_types);
    }

    /* ================================================================
     *  Stage 3 - Allocate bind structures
     *  One bind slot per WHERE key column.
     *  All key values are bound as SQLT_STR scalars; Oracle conversion
     *  functions in the SQL handle DATE/TIMESTAMP correctly.
     * ================================================================ */
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 3: Allocating bind structures keys=%d",
                 dc->key_count);

    bind_hdls  = calloc(dc->key_count, sizeof(OCIBind *));
    scalar_bufs= calloc(dc->key_count, sizeof(char *));
    indicators  = calloc(dc->key_count, sizeof(sb2));

    if (!bind_hdls || !scalar_bufs || !indicators)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for bind structures");
        rc = -1;
        goto Cleanup;
    }

    for (int k = 0; k < dc->key_count; k++)
    {
        scalar_bufs[k] = calloc(1, MAX_COL_VALUE_SIZE);
        if (!scalar_bufs[k])
        {
            logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                         "calloc failed for scalar_bufs[%d]", k);
            rc = -1;
            goto Cleanup;
        }
    }

    /* ================================================================
     *  Stage 4 - Bind key values and execute
     * ================================================================ */
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 4: Binding key values and executing");

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int k = 0; k < dc->key_count; k++)
    {
        strncpy(scalar_bufs[k], dc->keys[k].key_value,
                MAX_COL_VALUE_SIZE - 1);
        scalar_bufs[k][MAX_COL_VALUE_SIZE - 1] = '\0';
        indicators[k] = 0;   /* never NULL for a key */

        logger_write(ctx->delete_logger, LOG_DEBUG, __func__, 0,
                     "Binding WHERE key=%d name='%s' type='%s' "
                     "value='%s' bind_pos=%d",
                     k, dc->keys[k].field_name,
                     dc->keys[k].field_type,
                     dc->keys[k].key_value,
                     k + 1);

        CHECK_OCI_DEL(ctx->errhp,
            OCIBindByPos(stmt, &bind_hdls[k], ctx->errhp,
                         (ub4)(k + 1),
                         scalar_bufs[k],
                         (sb4)MAX_COL_VALUE_SIZE,
                         SQLT_STR,
                         &indicators[k],
                         NULL, NULL, 0, NULL, OCI_DEFAULT),
            ctx, Cleanup);
    }

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Calling OCIStmtExecute iters=1");

    CHECK_OCI_DEL(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                       1, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    metrics.execution_us  = metrics_now_us() - metrics.start_time_us;
    metrics.rows_affected = (uint64_t)rows_deleted;


    /* ---- Retrieve affected row count ---- */
    ub4 rows_affected = 0;
    CHECK_OCI_DEL(ctx->errhp,
        OCIAttrGet(stmt, OCI_HTYPE_STMT,
                   &rows_affected, NULL,
                   OCI_ATTR_ROW_COUNT, ctx->errhp),
        ctx, Cleanup);

    rows_deleted = (int)rows_affected;

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "OCIStmtExecute OK rows_deleted=%d", rows_deleted);

    /* ================================================================
     *  Stage 5 - Commit
     * ================================================================ */
    /* ---- Commit (skipped when a managed transaction is active) ---- */
    /*
     * If ctx->active_tx is set, the caller (e.g. Test_XML_Runner) has
     * opened an explicit transaction via tx_begin().  In that case the
     * work inserted here must stay uncommitted so the caller can batch
     * it with other DML steps and commit or roll back the whole unit
     * atomically via tx_commit() / tx_rollback().
     *
     * If ctx->active_tx is NULL this module owns the commit, which is
     * the original standalone behaviour.
     */
    if (ctx->active_tx)
    {
        logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                     "Commit successful rows_deleted=%d",
                     rows_deleted);
     }
    else
    {
        /* ---- Commit ---- */
        logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                     "Stage 5: Calling OCITransCommit");

        CHECK_OCI_DEL(ctx->errhp,
            OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT),
            ctx, Cleanup);

        logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                     "Commit successful rows_deleted=%d", rows_deleted);
    }



    /* ================================================================
     *  Stage 6 - Build result response
     *  Uses response_write_dml_xml()/response_write_dml_json() - same
     *  writers built for INSERT/UPDATE, reused unchanged here.
     *
     *  No WHERE-key echo any more - the old <where_keys> block is
     *  removed per the 2026-07-15 decision documented in
     *  Data_Manager_Request_Definitions.docx (UPDATE never echoed its
     *  WHERE clause either; removed from DELETE for consistency rather
     *  than added to UPDATE).
     * ================================================================ */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 6: Building result response elapsed=%.6f", elapsed);

    dml_response_t resp;
    memset(&resp, 0, sizeof(resp));
    strncpy(resp.table_name, dc->table_name, sizeof(resp.table_name) - 1);
    strncpy(resp.owner,      dc->owner,      sizeof(resp.owner) - 1);
    resp.rows_affected          = rows_deleted;
    resp.execution_time_seconds = elapsed;
    /* lobs_written stays 0 - DELETE has no LOB handling at all */

    char *dml_xml_fragment = response_write_dml_xml(ctx, OP_DELETE, &resp);
    if (!dml_xml_fragment)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "response_write_dml_xml returned NULL");
        rc = -1;
        goto Cleanup;
    }

    xml = xml_create(4096);
    if (!xml) { free(dml_xml_fragment); rc = -1; goto Cleanup; }

    xml_start_document(xml);
    xml_start_execution(xml);
    /* xml_append_raw(), not xml_append(xml,"%s",...) - see the
     * 2026-07-22 fix in OCI_Execute_Query_Batch_Module.c for why.      */
    xml_append_raw(xml, dml_xml_fragment);
    xml_end_execution(xml);
    xml_finalize(xml);
    free(dml_xml_fragment);

    /* cfg->OUTPUT_JSON's own doc comment in OCI_Connection.h: "set
     * only when ReturnFormat is JSON. NULL otherwise."                  */
    if (cfg->ReturnFormat && strcasecmp(cfg->ReturnFormat, "JSON") == 0)
    {
        cfg->OUTPUT_JSON = response_write_dml_json(ctx, OP_DELETE, &resp);
        if (!cfg->OUTPUT_JSON)
            logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                         "response_write_dml_json returned NULL - "
                         "OUTPUT_JSON will be missing for this JSON-format request");
    }

    metrics.end_time_us      = metrics_now_us();
    metrics.status_code      = 0;
    strncpy(metrics.error_code, "-", sizeof(metrics.error_code) - 1);
    strncpy(metrics.error_text, "-", sizeof(metrics.error_text) - 1);
    metrics.rows_affected    = (uint64_t)rows_deleted;
    metrics.output_xml_bytes = xml ? (uint64_t)strlen(xml->buffer) : 0;
    /* DELETE has no LOB/CLOB handling - lob_bytes and clob_bytes = 0  */
    /* transaction_id already set at init time                         */



    if (!cfg->xml) cfg->xml = calloc(1, sizeof(*cfg->xml));
    cfg->xml->OUTPUT_XML = strdup(xml->buffer);

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "execute_delete_batch complete: table='%s' "
                 "rows_deleted=%d elapsed=%.6f",
                 dc->table_name, rows_deleted, elapsed);

Cleanup:
    /* ================================================================
     *  Stage 7 - Cleanup: reverse allocation order, all guards
     * ================================================================ */
	metrics.end_time_us = metrics_now_us();
	metrics.status_code = rc;

	if (rc != 0)
	{
	    strncpy(metrics.error_code,
	            logger_last_error.error_code,
	            sizeof(metrics.error_code) - 1);
	    strncpy(metrics.error_text,
	            logger_last_error.error_text,
	            sizeof(metrics.error_text) - 1);
	}
	if(ctx->active_tx)
		strncpy(metrics.transaction_id , tx_get_id(ctx->active_tx),sizeof(tx_get_id(ctx->active_tx))-1);
	else
		strncpy(metrics.transaction_id , "-",sizeof("-")-1);
	metrics.connection_wait_us    = ctx->connection_wait_us;
	metrics.connection_create_us  = ctx->connection_create_us;
	metrics.connection_acquire_us = ctx->connection_acquire_us;


	//Process final 3 metrics
	//printf("DEBUG : cfg->input_file_name=%s\n",cfg->input_file_name);
	//printf("DEBUG : ctx->ini->metrics_display_input_file_name=%d\n",ctx->ini->metrics_display_input_file_name);
	if (ctx->ini && ctx->ini->metrics_display_input_file_name && cfg->input_file_name)
	    metrics.input_file_name = flatten_for_csv(cfg->input_file_name);
	//printf("DEBUG : metrics.input_request=%s\n",metrics.input_request);

	//printf("DEBUG : OCI_execute_query)batch.c ctx->INPUT_XML=%s\n",ctx->INPUT_XML);
	//printf("DEBUG : OCI_execute_query)batch.c  ctx->ini->metrics_display_input_request=%d\n",ctx->ini->metrics_display_input_request);
	if (ctx->ini && ctx->ini->metrics_display_input_request && ctx->INPUT_XML)
	    metrics.input_request = flatten_for_csv3(ctx->INPUT_XML);
	//printf("DEBUG :  OCI_execute_query)batch.c  metrics.input_request=%s\n",metrics.input_request);


	//printf("DEBUG : ctx->OUTPUT_XML=%s\n",ctx->OUTPUT_XML);
	//printf("DEBUG : ctx->ini->metrics_display_output_response=%d\n",ctx->ini->metrics_display_output_response);
	if (ctx->ini && ctx->ini->metrics_display_output_response)
	{
	    /* DELETE now renders a real JSON response too (Stage 6 above,
	     * via response_write_dml_json()) when cfg->ReturnFormat is
	     * JSON - cfg->OUTPUT_JSON is genuinely populated in that case,
	     * not a placeholder. This check's own logic didn't need to
	     * change - same as INSERT/UPDATE's identical fix.              */
	    int is_json = (cfg->ReturnFormat &&
	                   strcasecmp(cfg->ReturnFormat, "JSON") == 0);

	    if (is_json && cfg->OUTPUT_JSON)
	        metrics.output_response = flatten_for_csv3(cfg->OUTPUT_JSON);
	    else if (cfg->xml && cfg->xml->OUTPUT_XML)
	        metrics.output_response = flatten_for_csv3(cfg->xml->OUTPUT_XML);
	}
	//printf("DEBUG : metrics.output_response=%s\n",metrics.output_response);



	metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &metrics);
	logger_clear_last_error();   // reset for next operation



    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0, "Stage 7: Cleanup");

    /* Rollback on any error that occurred after the execute */
    if (rc != 0 && rows_deleted > 0)
    {
        logger_write(ctx->delete_logger, LOG_WARN, __func__, 0,
                     "Rolling back due to error");
        OCITransRollback(ctx->svchp, ctx->errhp, OCI_DEFAULT);
    }

    if (scalar_bufs)
    {
        for (int k = 0; k < dc->key_count; k++)
        {
            if (scalar_bufs[k])
            {
                logger_write(ctx->delete_logger, LOG_DEBUG, __func__, 0,
                             "free(scalar_bufs[%d])", k);
                free(scalar_bufs[k]);
            }
        }
        free(scalar_bufs);
    }

    if (indicators) free(indicators);
    if (bind_hdls)  free(bind_hdls);
    if (dc)         free(dc);
    if (xml)        xml_free(xml);
    if (old_values) free(old_values);

    if (stmt)
    {
        logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                     "OCIStmtRelease stmt");
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    }

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Cleanup complete rc=%d", rc);

    end_standalone_tx_if_owned(ctx, owns_standalone_tx);

    return rc;
}
