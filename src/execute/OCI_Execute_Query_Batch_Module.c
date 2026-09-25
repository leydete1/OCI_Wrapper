/*
 * OCI_Execute_Query_Batch_Module.c
 *
 * Array-fetch batch SELECT module.
 * Produces identical XML output to execute_query() but fetches
 * ctx->ini->query_fetch_batch_size rows per round-trip instead of one,
 * dramatically reducing network overhead on larger result sets.
 *
 * Structure
 * ---------
 *   allocate_batch_buffers()     - heap allocate column buffers & indicators
 *   get_multi_metadata()         - OCIParamGet metadata + OCIDefineByPos
 *                                  + OCIDefineArrayOfStruct per column
 *                                  NOW IN OCI_Table_Metadata_Module.c
 *   handle_clob_column_batch()   - read one CLOB cell, write to disk, emit XML
 *   handle_blob_column_batch()   - read one BLOB cell, write to disk, emit XML
 *   build_row_xml_batch()        - iterate columns for one logical row
 *   execute_query_batch()        - orchestrate: validate -> prepare ->
 *                                  describe -> execute -> fetch loop -> XML
 *
 * Metadata change
 * ---------------
 * define_columns_batch() has been removed from this file.  Its logic
 * now lives in get_multi_metadata() inside OCI_Table_Metadata_Module.c.
 * This means all metadata code for the project lives in one place.
 * When the metadata cache is introduced, single-table SELECTs can be
 * served from cache via get_request_metadata() with no changes here.
 * Multi-table JOINs and views continue to use OCI descriptor metadata
 * via get_multi_metadata() - the correct approach for those cases.
 * See OCI_Table_Metadata_Module.h for the full design rationale.
 *
 * Changes from previous version
 * ------------------------------
 *   1. COUNT query uses SELECT 1 wrapper to avoid ORA-00932 with CLOB columns
 *   2. BLOB NULL/empty guard: indicator checked BEFORE OCILobGetLength
 *   3. CLOB handler writes to CLOB_output_dir with clob_default_extension
 *   4. Multiple CLOBs per record tracked via CLOB_index (mirrors BLOB pattern)
 *   5. define_columns_batch() moved to OCI_Table_Metadata_Module.c as
 *      get_multi_metadata()
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>
#include <inttypes.h>                    /* PRIu64 for row_count logging  */

#include "XML_Helper.h"
#include "Connection.h"
#include "Execute_Query_Batch_Module.h"
#include <string_utils.h>               /* trim_sql_inplace()                         */
#include "Clob_Utils.h"              /* build_clob_filename(), build_clob_url() -
                                            see OCI_Clob_Utils.h for why there's no
                                            write_clob_to_file() alongside these -
                                            write_blob_to_file() below is reused as-is */
#include "Blob_Utils.h"              /* lookup_blob_index(), write_blob_to_file(),
                                            build_filename_with_timestamp() - relocated
                                            from the now-removed OCI_Execute_Query_Module */
#include "Table_Metadata_Module.h"   /* get_multi_metadata() get_select_metadata() */
#include "sql_dependency_extractor.h"    /* extract_sql_dependencies()                 */
#include "db_driver.h"                   /* db_driver_get() - sync-path integration
                                            (Phase 2a, 2026-09-14). async path below is
                                            deliberately still on the raw OCI machinery
                                            this file already had - see the branch on
                                            is_async inside execute_query_batch() for
                                            the full reasoning, not a placeholder.
                                            Deliberately NOT including driver_oracle.h -
                                            core calls only the vendor-neutral
                                            db_driver_get(), never reaches for a named
                                            vendor directly. That dispatch decision
                                            belongs entirely to db_driver.c.           */
#include "logger.h"
#include "resultset_cache.h"
#include "OCI_Transaction_Manager.h"
#include "metrics.h"
#include "metrics_writer.h"   /* metrics_finalise_and_enqueue() - closure item 5, Stage 2 */
#include "OCI_Resultset_Builder.h"
#include "OCI_Response_Writer.h";
#include "cJSON.h"                       /* Stage 3c JSON verification only */

/* ------------------------------------------------------------------ */
/*  Local OCI error macro - mirrors execute_query style                */
/* ------------------------------------------------------------------ */
#define CHECK_OCI(errhp, status) \
    if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
        text errbuf[512]; sb4 errcode = 0; \
        OCIErrorGet((errhp), 1, NULL, &errcode, errbuf, \
                    sizeof(errbuf), OCI_HTYPE_ERROR); \
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0, \
                     "OCI Error %d: %s", errcode, (char *)errbuf); \
    }

/* ------------------------------------------------------------------ */
/*  Internal batch context - groups all per-column arrays together     */
/* ------------------------------------------------------------------ */
typedef struct {
    ub4              col_count;
    ub4              fetch_count;     /* rows per fetch batch           */

    OCIDefine      **def;             /* [col]                          */
    char           **buffers;         /* [col] flat [fetch_count*bsz]   */
    ub4             *buf_sizes;       /* [col] individual buffer width   */
    sb2            **indicators;      /* [col] -> [fetch_count]          */
    ub2             *data_types;      /* [col]                           */
    ub4             *data_sizes;      /* [col] from OCI metadata         */
    char           (*col_names)[256]; /* [col]                           */

    /*
     * BLOB locators: flat array per column, one slot per row in the batch.
     * col_blob_locs[col] points to a contiguous block of fetch_count
     * OCILobLocator* handles. OCI strides through this block using
     * sizeof(OCILobLocator*) as the value_skip in OCIDefineArrayOfStruct.
     * Access row r of column c as: col_blob_locs[c][r]
     */
    OCILobLocator ***col_blob_locs;   /* [col] -> flat [fetch_count]     */

    /* Single CLOB locator reused per row (CLOBs not array-fetchable)   */
    OCILobLocator   *clob_loc;
} batch_ctx_t;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
/* handle_clob_column_batch()/handle_blob_column_batch()/
 * build_row_xml_batch()/allocate_batch_buffers()/free_batch_ctx() were
 * removed here (Phase 2b, 2026-09-15) - no remaining caller anywhere in
 * this file once both sync and async fetch loops moved onto
 * driver->select_open()/select_fetch_batch()/select_close(). See the
 * v2 driver integration comment inside execute_query_batch() for the
 * full reasoning. batch_ctx_t itself is kept - bc.fetch_count is still
 * read (feeds db_select_request_t.fetch_array_size) even though every
 * other field on it is now unused.                                    */


/* ================================================================== */
/* ================================================================== */
/*  allocate_batch_buffers() / handle_clob_column_batch() /
 *  handle_blob_column_batch() / build_row_xml_batch() /
 *  free_batch_ctx() were removed here (Phase 2b, 2026-09-15).
 *
 *  No remaining caller anywhere in this file: both the sync and async
 *  branches of execute_query_batch() now fetch through
 *  driver->select_open()/select_fetch_batch()/select_close() (see the
 *  v2 driver integration comment inside execute_query_batch() itself),
 *  which do their own self-contained BLOB/CLOB handling
 *  (driver_oracle.c's oracle_fetch_blob_field()/oracle_fetch_clob_field())
 *  and never touch a core-owned batch_ctx_t or BLOB_list array.
 * ================================================================== */



/* ------------------------------------------------------------------ */
/*  Stage 3c: verify response_write_json() output (temporary -         */
/*  verification only)                                                 */
/*                                                                      */
/*  Unlike Stage 3's XML check, there's no pre-existing JSON buffer to  */
/*  diff against - response_write_json() is brand new. So instead this  */
/*  parses its own output back with cJSON and compares every field      */
/*  against rs directly, the same struct both writers render from.      */
/*  This is a stricter check than comparing two rendered strings by     */
/*  eye: it catches the writer disagreeing with its own source data,    */
/*  not just disagreeing with the XML writer.                           */
/*                                                                      */
/*  Returns 1 on full match, 0 on any mismatch (with a printf/logger    */
/*  line identifying exactly what didn't match, same MATCH/MISMATCH     */
/*  pattern as Stage 3).                                                */
/* ------------------------------------------------------------------ */
static int verify_response_json_against_resultset(oci_context_t *ctx,
                                                    const resultset_t *rs,
                                                    const char *json_str)
{
    if (!rs || !json_str) return 0;

    cJSON *root = cJSON_Parse(json_str);
    if (!root)
    {
        printf("\n[STAGE3c] JSON did not parse: %s\n", cJSON_GetErrorPtr());
        return 0;
    }

    cJSON *resultset = cJSON_GetObjectItemCaseSensitive(root, "resultset");
    if (!cJSON_IsArray(resultset))
    {
        printf("\n[STAGE3c] MISMATCH - no \"resultset\" array in JSON\n");
        cJSON_Delete(root);
        return 0;
    }

    int json_row_count = cJSON_GetArraySize(resultset);
    if (json_row_count != rs->record_count)
    {
        printf("\n[STAGE3c] MISMATCH - row count: struct=%d json=%d\n",
               rs->record_count, json_row_count);
        cJSON_Delete(root);
        return 0;
    }

    int ok = 1;

    for (int r = 0; r < rs->record_count && ok; r++)
    {
        const resultset_row_t *row      = &rs->records[r];
        cJSON                 *row_obj  = cJSON_GetArrayItem(resultset, r);
        cJSON *row_number = cJSON_GetObjectItemCaseSensitive(row_obj, "row_number");
        cJSON *fields     = cJSON_GetObjectItemCaseSensitive(row_obj, "fields");

        if (!cJSON_IsNumber(row_number) || row_number->valueint != row->record_number)
        {
            printf("\n[STAGE3c] MISMATCH - row %d: row_number struct=%d json=%s\n",
                   r, row->record_number,
                   row_number ? cJSON_Print(row_number) : "(missing)");
            ok = 0;
            break;
        }

        if (!cJSON_IsArray(fields) || cJSON_GetArraySize(fields) != row->field_count)
        {
            printf("\n[STAGE3c] MISMATCH - row %d: field_count struct=%d json=%d\n",
                   r, row->field_count,
                   cJSON_IsArray(fields) ? cJSON_GetArraySize(fields) : -1);
            ok = 0;
            break;
        }

        for (int f = 0; f < row->field_count; f++)
        {
            const resultset_field_t *fld       = &row->fields[f];
            cJSON                   *field_obj = cJSON_GetArrayItem(fields, f);

            cJSON *fn = cJSON_GetObjectItemCaseSensitive(field_obj, "field_name");
            cJSON *ft = cJSON_GetObjectItemCaseSensitive(field_obj, "field_type");

            if (!cJSON_IsString(fn) || strcmp(fn->valuestring, fld->field_name) != 0)
            {
                printf("\n[STAGE3c] MISMATCH - row %d field %d: field_name struct=%s json=%s\n",
                       r, f, fld->field_name,
                       cJSON_IsString(fn) ? fn->valuestring : "(missing)");
                ok = 0;
                break;
            }

            if (!cJSON_IsString(ft) ||
                strcmp(ft->valuestring, fld->is_blob ? "BLOB" : fld->field_type) != 0)
            {
                printf("\n[STAGE3c] MISMATCH - row %d field %d (%s): field_type struct=%s json=%s\n",
                       r, f, fld->field_name,
                       fld->is_blob ? "BLOB" : fld->field_type,
                       cJSON_IsString(ft) ? ft->valuestring : "(missing)");
                ok = 0;
                break;
            }

            if (fld->is_blob)
            {
                cJSON *blob = cJSON_GetObjectItemCaseSensitive(field_obj, "blob");
                cJSON *bn   = blob ? cJSON_GetObjectItemCaseSensitive(blob, "file_name") : NULL;
                cJSON *bp   = blob ? cJSON_GetObjectItemCaseSensitive(blob, "file_path") : NULL;
                cJSON *bs   = blob ? cJSON_GetObjectItemCaseSensitive(blob, "file_size") : NULL;
                cJSON *bm   = blob ? cJSON_GetObjectItemCaseSensitive(blob, "mime_type") : NULL;

                char size_str[32];
                snprintf(size_str, sizeof(size_str), "%llu",
                         (unsigned long long)fld->blob_detail.file_size);

                const char *exp_name = fld->blob_detail.file_name[0] ? fld->blob_detail.file_name : "N/A";
                const char *exp_path = fld->blob_detail.file_path[0] ? fld->blob_detail.file_path : "N/A";
                const char *exp_mime = fld->blob_detail.mime_type[0] ? fld->blob_detail.mime_type : "application/octet-stream";

                if (!blob ||
                    !cJSON_IsString(bn) || strcmp(bn->valuestring, exp_name) != 0 ||
                    !cJSON_IsString(bp) || strcmp(bp->valuestring, exp_path) != 0 ||
                    !cJSON_IsString(bs) || strcmp(bs->valuestring, size_str) != 0 ||
                    !cJSON_IsString(bm) || strcmp(bm->valuestring, exp_mime) != 0)
                {
                    printf("\n[STAGE3c] MISMATCH - row %d field %d (%s): blob object differs\n",
                           r, f, fld->field_name);
                    ok = 0;
                    break;
                }

                /* file_url only expected when set - same conditional as
                 * the writer itself and Stage 3c stays in sync with it. */
                if (fld->blob_detail.file_url[0])
                {
                    cJSON *bu = cJSON_GetObjectItemCaseSensitive(blob, "file_url");
                    if (!cJSON_IsString(bu) || strcmp(bu->valuestring, fld->blob_detail.file_url) != 0)
                    {
                        printf("\n[STAGE3c] MISMATCH - row %d field %d (%s): file_url differs\n",
                               r, f, fld->field_name);
                        ok = 0;
                        break;
                    }
                }
            }
            else
            {
                cJSON *fv = cJSON_GetObjectItemCaseSensitive(field_obj, "field_value");
                if (!cJSON_IsString(fv) || strcmp(fv->valuestring, fld->value) != 0)
                {
                    printf("\n[STAGE3c] MISMATCH - row %d field %d (%s): field_value struct=%s json=%s\n",
                           r, f, fld->field_name, fld->value,
                           cJSON_IsString(fv) ? fv->valuestring : "(missing)");
                    ok = 0;
                    break;
                }
            }
        }
    }

    cJSON_Delete(root);

    if (ok)
        printf("\n[STAGE3c] MATCH - JSON matches source resultset struct field-for-field (%d rows)\n",
               rs->record_count);

    logger_write(ctx->select_logger, ok ? LOG_INFO : LOG_WARN, __func__, 0,
                 ok ? "STAGE3c MATCH - JSON matches resultset struct"
                    : "STAGE3c MISMATCH - JSON differs from resultset struct");

    return ok;
}


/* ================================================================== */
/*  6.  execute_query_batch                                            */
/*      Main entry point - orchestrates the full batch fetch cycle     */
/* ================================================================== */
int execute_query_batch(oci_context_t *ctx, execute_config_t *cfg)
{
    int        rc         = 0;
    OCIStmt   *stmt       = NULL;
    OCIStmt   *stmt_count = NULL;
    lob_item_t *BLOB_list = NULL;
    resultset_t *rs = NULL;
    xml_builder_t *xml    = NULL;
    char parse_msg[256];

    OCI_DEPENDENCY_LIST deps;
    memset(&deps, 0, sizeof(deps));
    memset(&parse_msg, 0, sizeof(parse_msg));

    batch_ctx_t bc;
    memset(&bc, 0, sizeof(bc));

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering execute_query_batch sql=%s", cfg->SQL);

    if (!ctx || !cfg || !cfg->SQL)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments: ctx, cfg or cfg->SQL is NULL");
        rc = -1;
        goto Cleanup;
    }

    /* ---- Resolve fetch batch size ---- */
    bc.fetch_count = (ub4)ctx->ini->query_fetch_batch_size;
    if (bc.fetch_count < 1)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "query_fetch_batch_size=%d < 1, defaulting to 1",
                     ctx->ini->query_fetch_batch_size);
        bc.fetch_count = 1;
    }
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "fetch_count=%u", bc.fetch_count);

    /* ---- Clean SQL ---- */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling trim_sql_inplace");
    trim_sql_inplace(cfg->SQL, ctx);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Cleaned SQL: %s", cfg->SQL);

    /* ============Metrics INIT BLOCK -           */
    /* ================================================================== */

        metrics_record_t metrics;
        metrics_init(&metrics);
        metrics_set_context(&metrics, ctx);

        metrics.start_time_us = metrics_now_us();

        strncpy(metrics.operation,   "SELECT",  sizeof(metrics.operation)   - 1);
        /* object_name: filled after sql dependency extraction when available */
        /* For now use the first 127 chars of the SQL as a fallback          */
        strncpy(metrics.object_name, cfg->SQL,  sizeof(metrics.object_name) - 1);

        /* Set transaction_id immediately so every write path carries it  */
            if (ctx->active_tx)
                strncpy(metrics.transaction_id,
                        tx_get_id(ctx->active_tx),
                        sizeof(metrics.transaction_id) - 1);
            else
                strncpy(metrics.transaction_id, "-",
                        sizeof(metrics.transaction_id) - 1);
            /* Same source as transaction_id above, just the name -
             * closure item 5 follow-up (2026-08-10).                  */
            strncpy(metrics.transaction_name,
                    ctx->active_tx ? ctx->active_tx->tx_name : "-",
                    sizeof(metrics.transaction_name) - 1);


    /* ================================================================
     *  TL:6-June - Stage 0: Parse SQL dependencies
     *  Extract every table/view and field reference from the cleaned
     *  SQL.  On failure return -1 immediately with a descriptive error
     *  already written to sql_parser_logger by the extractor.
     *  On success deps is fully populated and passed to
     *  get_select_metadata() later so it can call get_table_metadata()
     *  per source table before delegating to get_multi_metadata().
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 0: Parsing SQL dependencies");

    uint64_t sql_parse_start = metrics_now_us();
    int sql_parse_rc = extract_sql_dependencies(cfg->SQL, &deps, ctx);
    metrics.sql_parse_us = metrics_now_us() - sql_parse_start;

    if (sql_parse_rc != 0)
    {

        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "extract_sql_dependencies failed for SQL: %s",
                     cfg->SQL);

        rc = -1;
        goto Cleanup;
    }


    strncpy(metrics.object_name, deps.objects[0].object_name ,  sizeof(metrics.object_name) - 1);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 0 OK: objects=%d fields=%d",
                 deps.object_count, deps.field_count);







    /* ================================================================
     * BLOCK A - Cache lookup
     * Place this immediately after trim_sql_inplace() call,
     * before the Stage 1 count query.
     * ================================================================ */

        /* ---- Resultset cache lookup ---- */
       logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Checking Resultsetcache setup");

        /* Bug fix (2026-08-23), found via real testing - moved up from
         * just before the fetch loop, where it used to be computed too
         * late to matter here. An execute_async=1 request must NEVER
         * take the cache-hit shortcut a few lines below (which jumps
         * straight to Cleanup and returns) - that shortcut skips the
         * fetch loop entirely, and the fetch loop is where every batch
         * actually gets built and delivered. A cache hit on an async
         * request would otherwise return HTTP 202 and then silently
         * deliver zero batches, with no error at all - worse than the
         * separate cache-poisoning bug fixed alongside this one, since
         * there's nothing to even notice went wrong. Treating async
         * requests as an automatic cache miss forces them to always run
         * for real. */
        int is_async = (cfg->async_batch_callback != NULL);

       char cache_key[8192] = {0};
        int  served_from_cache = 0;

        if (ctx->resultset_cache && !is_async &&
            resultset_cache_make_key(cfg->SQL, cache_key, sizeof(cache_key)))
        {
            /* Compute both hashes as soon as the key is available     */
            metrics.sql_hash       = cache_hash_string(ctx->resultset_cache,
                                                        cfg->SQL);
            metrics.cache_key_hash = cache_hash_string(ctx->resultset_cache,
                                                        cache_key);

            uint64_t lookup_start = metrics_now_us();
            cache_entry_t *hit = resultset_cache_lookup(ctx->resultset_cache,
                                                         cache_key);
            metrics.cache_lookup_us = metrics_now_us() - lookup_start;

            if (hit)
            {
                int want_json = (cfg->ReturnFormat &&
                                 strcasecmp(cfg->ReturnFormat, "JSON") == 0);

                if (want_json && !hit->output_document_json)
                {
                    /* This entry predates JSON caching (or JSON
                     * rendering failed at store time) - do not serve
                     * XML to a JSON request. Treat as a miss and fall
                     * through to normal execution below rather than
                     * goto Cleanup, so this becomes a genuine store
                     * with JSON included.                              */
                    logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                                 "CACHE HIT key='%.80s' but no cached JSON "
                                 "for a JSON request - treating as miss",
                                 cache_key);
                    metrics.cache_hit = 0;
                    resultset_cache_release(ctx->resultset_cache, hit);
                }
                else
                {
                    metrics.cache_hit = 1;

                    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                 "CACHE HIT key='%.80s' doc_len=%zu rows=%"PRIu64
                                 " format=%s",
                                 cache_key, hit->output_length, hit->row_count,
                                 want_json ? "JSON" : "XML");

                    /* Return cached content directly - no OCI work needed */
                    if (want_json)
                    {
                        cfg->OUTPUT_JSON = strdup(hit->output_document_json);
                    }
                    else
                    {
                        if (!cfg->xml)
                            cfg->xml = calloc(1, sizeof(*cfg->xml));

                        if (cfg->xml)
                            cfg->xml->OUTPUT_XML = strdup(hit->output_document);
                    }

                    metrics.rows_affected = hit->row_count;

                    uint64_t hit_row_count = hit->row_count;

                    resultset_cache_release(ctx->resultset_cache, hit);

                    cache_update_exec_stats(ctx->resultset_cache,
                                            0.0, 0.0,
                                            hit_row_count,  /* rows from cache entry */
                                            1,     /* was_cache_hit = 1        */
                                            1);    /* success                  */
                    served_from_cache = 1;
                    rc = 0;
                    goto Cleanup;
                }
            }
            else
            {
                metrics.cache_hit = 0;
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "CACHE MISS key='%.80s'", cache_key);
            }
        }





    /* ================================================================
     *  Stage 1 - Validate: row count guard
     *
     *  Use SELECT COUNT(*) FROM (SELECT 1 FROM (original_sql)) to avoid
     *  ORA-00932 which Oracle raises when a CLOB column appears in a
     *  COUNT(*) subquery. The SELECT 1 strips all column types.
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Validate record count");

    int        record_count = 0;
    OCIDefine *defn_count   = NULL;
    char       query_count[4096];

    snprintf(query_count, sizeof(query_count),
             "SELECT COUNT(*) FROM (SELECT 1 FROM (%s))", cfg->SQL);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Count query: %s", query_count);

    CHECK_OCI(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt_count, ctx->errhp,
                        (text *)query_count, (ub4)strlen(query_count),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));

    CHECK_OCI(ctx->errhp,
        OCIDefineByPos(stmt_count, &defn_count, ctx->errhp,
                       1, &record_count, sizeof(record_count),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT));

    CHECK_OCI(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt_count, ctx->errhp,
                       1, 0, NULL, NULL, OCI_DEFAULT));

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "record_count=%d max=%d",
                 record_count, ctx->ini->query_max_record_count);

    /* Closure item 4 (2026-08-09) - this guard used to treat "zero
     * rows" and "exceeds max" identically, aborting the whole request
     * with a generic error for both. Neither is actually an error:
     *
     *  - Zero rows is a completely normal, valid outcome for a SELECT
     *    whose WHERE clause simply matches nothing - it isn't a
     *    failure to be reported as one. Confirmed as a real, repeated
     *    point of confusion during 2026-07 testing (this exact log
     *    line being mistaken for a genuine problem - see
     *    OCI_Session_Manager.c's own reconcile_orphans() comment on
     *    the same issue) and worth fixing at the source rather than
     *    re-explaining every time it comes up.
     *
     *  - Exceeding max shouldn't block the caller outright either -
     *    return what's allowed and say so, rather than forcing a
     *    ticket/retry cycle for something the caller can already see
     *    and adjust for themselves (their own request's row count is
     *    knowable in advance).
     *
     * IMPORTANT SAFETY NOTE for the "exceeds max" branch: capping only
     * the record_count VARIABLE here would NOT actually bound how many
     * rows get fetched below - Stage 5's fetch loop runs unbounded
     * until OCIStmtFetch2 itself reports rows_fetched=0 (cursor
     * exhausted), with no check against record_count inside the loop
     * at all. If the underlying query would still return more rows
     * than the cap, capping just this variable would leave
     * resultset_create() allocating arrays sized for the CAPPED count,
     * while the fetch loop kept writing past that bound - a genuine
     * buffer overflow. The safe fix constrains the actual SQL executed
     * below (see fetch_sql construction a few lines down), not just
     * this count.                                                      */
    int truncated = 0;

    if (record_count < 1)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "record_count=0 - no rows matched. This is a valid, "
                     "normal outcome, not an error - proceeding with an "
                     "empty result set.");
        /* record_count stays 0 - resultset_create(0, ...) and the
         * max_lobs/max_clobs calculations below all handle this
         * correctly (0 * anything = 0), and Stage 5's fetch loop will
         * simply see rows_fetched=0 on its first call and exit
         * immediately with nothing processed.                         */
    }
    else if (record_count > ctx->ini->query_max_record_count)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "record_count=%d exceeds max=%d - returning the "
                     "first %d row(s) rather than aborting. Caller "
                     "should check the <truncated> flag on the response "
                     "and narrow their own query if the full result set "
                     "is actually needed.",
                     record_count, ctx->ini->query_max_record_count,
                     ctx->ini->query_max_record_count);
        record_count = ctx->ini->query_max_record_count;
        truncated = 1;
    }

    /* BLOB/CLOB tracking - max_lobs/max_clobs and the whole-query
     * BLOB_list array they used to size are gone (Phase 2b,
     * 2026-09-15): both sync and async now go through driver_oracle.c's
     * own self-contained per-field BLOB/CLOB handling (see
     * oracle_fetch_blob_field()/oracle_fetch_clob_field()), which never
     * touches a core-owned tracking array at all - nothing left here to
     * size in advance. */
    int BLOB_index = 0;
    int CLOB_index = 0;
    uint64_t lob_bytes  = 0;   /* total BLOB bytes read               */
    uint64_t clob_bytes = 0;   /* total CLOB bytes read               */

    /* Closure item 4 (2026-08-09) - when truncated, the statement
     * actually fetched below must itself be bounded to record_count
     * rows (now the capped value) - not just the variable. A separate
     * buffer, not an in-place rewrite of cfg->SQL: that pointer is
     * also used later for extract_sql_dependencies() (already run,
     * above this point, on the real original query - unaffected
     * either way) and for the <sql_query> tag in the response itself,
     * which should show the caller's actual query, not a wrapper this
     * code injected around it. Same ROWNUM-subquery idiom already used
     * a few lines up for the COUNT(*) guard query, for consistency
     * within this file.                                                */
    char  fetch_sql_buf[4096];
    const char *fetch_sql = cfg->SQL;

    if (truncated)
    {
        snprintf(fetch_sql_buf, sizeof(fetch_sql_buf),
                 "SELECT * FROM (%s) WHERE ROWNUM <= %d",
                 cfg->SQL, record_count);
        fetch_sql = fetch_sql_buf;
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Truncated fetch SQL: %s", fetch_sql);
    }

    /* ================================================================
     * v2 driver integration - Phase 2b (2026-09-15).
     *
     * sync and async now share ONE describe+fetch setup via
     * driver->select_open()/select_fetch_batch()/select_close() -
     * is_async only decides what happens with each batch's rows
     * (accumulate into the whole-query rs vs render+deliver+discard
     * immediately), not the setup itself. This replaces Phase 2a's
     * "two parallel implementations" scaffolding entirely.
     * handle_blob_column_batch()/handle_clob_column_batch()/
     * build_row_xml_batch()/allocate_batch_buffers()/free_batch_ctx()
     * have no remaining caller anywhere in this file and have been
     * removed outright, not just bypassed.
     *
     * is_final_batch (async only) uses the driver-agnostic condition
     * from the 2026-08-23 bug fix directly (abs_rownum >= record_count) -
     * it never depended on raw OCI fetch status once that fix landed,
     * so there is nothing to adapt for the driver here.
     *
     * Bug fix applied during this port (2026-09-15, found by inspection,
     * not by a failing test): the async per-batch fold below now also
     * accumulates stats.blob_bytes into lob_bytes. The original async
     * code only ever folded clob_bytes - metrics.lob_bytes was silently
     * never populated for async requests. Sync already folded both
     * (Phase 2a) - this brings async to the same correctness rather
     * than preserving a latent gap.
     * ================================================================ */
    unsigned int abs_rownum = 0;

    const db_driver_t *driver = db_driver_get(ctx);
    if (!driver || !driver->select_open || !driver->select_fetch_batch ||
        !driver->select_close)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "db_driver_get() returned an incomplete driver");
        rc = -1;
        goto Cleanup;
    }

    db_select_request_t req;
    memset(&req, 0, sizeof(req));
    req.sql                  = fetch_sql;
    req.max_rows             = record_count;
    req.max_memory_bytes     = cfg->max_memory_bytes;
    req.fetch_array_size     = (int)bc.fetch_count;
    req.query_timeout        = cfg->query_timeout;
    req.include_column_names = cfg->include_column_names;

    db_select_cursor_t *cursor  = NULL;
    db_column_meta_t   *columns = NULL;
    int col_count_driver  = 0;
    int batch_size_driver = 0;

    struct timespec ts_start, ts_end;
    uint64_t exec_start_us;

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int open_rc = driver->select_open(ctx, &req, &cursor, &columns,
                                       &col_count_driver, &batch_size_driver);
    if (open_rc != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "driver select_open failed rc=%d for sql=%s",
                     open_rc, fetch_sql);
        rc = -1;
        goto Cleanup;
    }

    exec_start_us = metrics_now_us();

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "driver select_open OK: columns=%d batch_size=%d",
                 col_count_driver, batch_size_driver);

    /* Bug fix (2026-09-15, root-caused via a second ASan crash report -
     * SEGV in ensure_capacity, from Stage 6's xml_start_execution(xml)
     * further down). rs/xml creation was WRONGLY gated behind
     * "if (!is_async)" here in Phase 2a/2b - that was a genuine design
     * mistake, not a defensive nicety. The original, already-proven
     * design (see the 2026-08-23 comment preserved a few hundred lines
     * down, at the response_writer_cache_store() call) always created
     * both unconditionally - an async request leaves rs "deliberately
     * empty" (real row data goes to per-batch batch_rs instead), not
     * NULL. Stage 6 onward was written assuming that invariant and was
     * never wrong itself - this creation block was. Row POPULATION
     * (copying batch_rs into rs) is correctly still gated on
     * "if (!is_async)" further down in the fetch loop - only creation
     * needed to stop being conditional. */
    rs = resultset_create(record_count, col_count_driver);
    if (!rs)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "resultset_create failed - record_count=%d "
                     "fields_per_row=%d", record_count, col_count_driver);
        driver->select_close(cursor);
        free(columns);
        rc = -1;
        goto Cleanup;
    }

    xml = xml_create(16384);
    if (!xml)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "xml_create failed");
        driver->select_close(cursor);
        free(columns);
        rc = -1;
        goto Cleanup;
    }
    xml_start_document(xml);
    xml_start_execution(xml);
    xml_append(xml, "<sql_query>%s</sql_query>\n", cfg->SQL);
    xml_append(xml, "<truncated>%s</truncated>\n", truncated ? "true" : "false");
    xml_end_execution(xml);

    int is_json_async = (cfg->ReturnFormat &&
                          strcasecmp(cfg->ReturnFormat, "JSON") == 0);
    int batch_number = 0;

    for (;;)
    {
        resultset_t *batch_rs = NULL;
        int          rows_fetched = 0;
        db_fetch_batch_stats_t stats = {0};

        if (driver->select_fetch_batch(cursor, &batch_rs, &rows_fetched, &stats) != 0)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "driver select_fetch_batch failed");
            driver->select_close(cursor);
            free(columns);
            rc = -1;
            goto Cleanup;
        }

        /* Unconditional fold, sync and async alike - see db_driver.h's
         * doc comment on out_stats for why this is necessary, not
         * cosmetic (feeds <blobs_extracted>/<clobs_extracted> and
         * metrics.lob_bytes/clob_bytes either way). */
        BLOB_index += stats.blob_count;
        CLOB_index += stats.clob_count;
        lob_bytes  += stats.blob_bytes;
        clob_bytes += stats.clob_bytes;

        if (rows_fetched == 0)
            break;

        if (!is_async)
        {
            for (int r = 0; r < rows_fetched; r++)
            {
                abs_rownum++;
                resultset_row_t *src_row = &batch_rs->records[r];
                resultset_row_t *dst_row = resultset_get_row(rs, (int)abs_rownum);

                if (!dst_row)
                {
                    /* Should not happen - fetch_sql above is already
                     * bounded to record_count when truncated - defensive
                     * check anyway, matching this codebase's own style. */
                    logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                                 "resultset_get_row failed for abs_rownum=%u "
                                 "(record_count=%d)", abs_rownum, record_count);
                    resultset_free(batch_rs);
                    driver->select_close(cursor);
                    free(columns);
                    rc = -1;
                    goto Cleanup;
                }

                for (int f = 0; f < src_row->field_count; f++)
                {
                    resultset_field_t *sf = &src_row->fields[f];

                    if (sf->is_blob)
                    {
                        resultset_set_blob_field(dst_row, f, sf->field_name,
                                                  sf->blob_detail.file_name,
                                                  sf->blob_detail.file_path,
                                                  sf->blob_detail.file_url,
                                                  sf->blob_detail.file_size,
                                                  sf->blob_detail.mime_type);
                    }
                    else
                    {
                        resultset_set_field(dst_row, f, sf->field_name,
                                             sf->field_type, sf->value);
                    }
                }
            }

            resultset_free(batch_rs);
        }
        else
        {
            /* async: batch_rs is already exactly what response_write_xml/
             * response_write_json need - batch-relative row numbers
             * (1..rows_fetched), fully populated by the driver, same
             * shape the original inline fetch loop built by hand via
             * build_row_xml_batch(). Render, deliver, discard - never
             * accumulated into a whole-query rs (nobody renders one for
             * an async request - see the original design comment this
             * replaced, preserved in spirit below). */
            abs_rownum += (unsigned int)rows_fetched;
            batch_number++;

            int is_final_batch = (abs_rownum >= (unsigned int)record_count);

            char *resultset_body = is_json_async
                ? response_write_json(ctx, batch_rs)
                : response_write_xml(ctx, batch_rs);

            if (!resultset_body)
            {
                logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                             "async batch %d: response_write_%s returned "
                             "NULL - batch not delivered",
                             batch_number, is_json_async ? "json" : "xml");
            }
            else if (is_json_async)
            {
                /* Genuine JSON envelope - envelope fields and the
                 * resultset's own "resultset" key together at the same
                 * top level (2026-08-24 bug fix, preserved as-is). */
                clock_gettime(CLOCK_MONOTONIC, &ts_end);
                double elapsed_so_far =
                    (ts_end.tv_sec  - ts_start.tv_sec) +
                    (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

                size_t body_len = strlen(resultset_body);
                const char *inner_start = resultset_body;
                size_t      inner_len   = body_len;
                if (body_len >= 2 && resultset_body[0] == '{' &&
                    resultset_body[body_len - 1] == '}')
                {
                    inner_start = resultset_body + 1;
                    inner_len   = body_len - 2;
                }

                size_t buf_size = inner_len + 512;
                char  *json_buf = malloc(buf_size);
                if (json_buf)
                {
                    int written = snprintf(json_buf, buf_size,
                        "{\"batch_number\":%d,\"final\":%s,"
                        "\"blobs_extracted\":%d,\"clobs_extracted\":%d,",
                        batch_number, is_final_batch ? "true" : "false",
                        stats.blob_count, stats.clob_count);

                    if (is_final_batch && written > 0 && (size_t)written < buf_size)
                    {
                        written += snprintf(json_buf + written, buf_size - written,
                            "\"num_rows\":%u,\"execution_time_total\":%.6f,"
                            "\"truncated\":%s,",
                            abs_rownum, elapsed_so_far,
                            truncated ? "true" : "false");
                    }

                    if (written > 0 && (size_t)written < buf_size)
                    {
                        snprintf(json_buf + written, buf_size - written,
                                 "%.*s}", (int)inner_len, inner_start);
                    }

                    /* Best-effort, per this module's own design contract -
                     * return value intentionally not checked here. */
                    cfg->async_batch_callback(cfg->async_batch_user_data,
                                               json_buf,
                                               is_json_async,
                                               is_final_batch,
                                               batch_number);
                    free(json_buf);
                }
                free(resultset_body);
            }
            else
            {
                xml_builder_t *batch_xml = xml_create(strlen(resultset_body) + 1024);
                if (batch_xml)
                {
                    xml_start_document(batch_xml);
                    xml_start_execution(batch_xml);
                    xml_append(batch_xml, "<batch_number>%d</batch_number>\n", batch_number);
                    xml_append(batch_xml, "<final>%s</final>\n",
                               is_final_batch ? "true" : "false");
                    xml_append(batch_xml, "<blobs_extracted>%d</blobs_extracted>\n",
                               stats.blob_count);
                    xml_append(batch_xml, "<clobs_extracted>%d</clobs_extracted>\n",
                               stats.clob_count);

                    if (is_final_batch)
                    {
                        clock_gettime(CLOCK_MONOTONIC, &ts_end);
                        double elapsed_so_far =
                            (ts_end.tv_sec  - ts_start.tv_sec) +
                            (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
                        xml_append(batch_xml, "<num_rows>%u</num_rows>\n", abs_rownum);
                        xml_append(batch_xml,
                                   "<execution_time_total>%.6f</execution_time_total>\n",
                                   elapsed_so_far);
                        xml_append(batch_xml, "<truncated>%s</truncated>\n",
                                   truncated ? "true" : "false");
                    }

                    xml_end_execution(batch_xml);
                    xml_append_raw(batch_xml, resultset_body);
                    xml_finalize(batch_xml);

                    /* Best-effort, per this module's own design contract -
                     * a failed delivery is the callback's own concern to
                     * log; it must never stop this fetch loop or fail
                     * the request. */
                    cfg->async_batch_callback(cfg->async_batch_user_data,
                                               batch_xml->buffer,
                                               is_json_async,
                                               is_final_batch,
                                               batch_number);

                    xml_free(batch_xml);
                }
                free(resultset_body);
            }

            resultset_free(batch_rs);
        }
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Fetch loop complete. Total rows=%u BLOBS=%d CLOBS=%d",
                 abs_rownum, BLOB_index, CLOB_index);

    driver->select_close(cursor);
    free(columns);

    metrics.execution_us  = metrics_now_us() - exec_start_us;
    metrics.rows_affected = (uint64_t)abs_rownum;

    /* ================================================================
     *  Stage 6 - Finalise XML
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 6: Finalise XML");

    /* xml_end_resultset(xml); */ /* Unused: replaced below - resultset body now comes from response_write_xml(ctx, rs), the new parsing-layer writer that supports both XML and JSON from the same resultset_t */
    {
        char *resultset_xml = response_write_xml(ctx, rs);
        if (resultset_xml)
        {
            xml_append_raw(xml, resultset_xml);
            free(resultset_xml);
        }
        else
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "response_write_xml returned NULL - resultset "
                         "section will be missing from OUTPUT_XML");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    xml_start_execution(xml);
    xml_append(xml, "<num_rows>%u</num_rows>\n",               abs_rownum);
    xml_append(xml, "<execution_time_total>%.6f</execution_time_total>\n",
               elapsed);
    xml_append(xml, "<fetch_batch_size>%u</fetch_batch_size>\n",
               bc.fetch_count);
    xml_append(xml, "<blobs_extracted>%d</blobs_extracted>\n", BLOB_index);
    xml_append(xml, "<clobs_extracted>%d</clobs_extracted>\n", CLOB_index);
    xml_end_execution(xml);
    xml_finalize(xml);


    /* ---- Stage 3: compare old and new resultset XML (temporary - verification only) ---- */
     /*   char *new_response_xml = response_write_xml(ctx, rs);

        if (new_response_xml)
        {
            printf("\n===== OLD (existing xml->buffer) =====\n%s\n", xml->buffer);
            printf("\n===== NEW (response_write_xml) =====\n%s\n", new_response_xml);

            if (strcmp(xml->buffer, new_response_xml) == 0)
            {
                printf("\n[STAGE3] MATCH - old and new resultset XML are identical\n");
                logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                             "STAGE3 MATCH - old and new resultset XML identical");
            }
            else
            {
                printf("\n[STAGE3] MISMATCH - old and new resultset XML differ\n");
                logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                             "STAGE3 MISMATCH - old and new resultset XML differ");
            }

            free(new_response_xml);
        }
        else
        {
            printf("\n[STAGE3] response_write_xml returned NULL\n");
        }

	*/


    /* ---- Stage 3: compare old and new resultset XML (temporary - verification only) ----
     * Unused/dead: this block existed to verify response_write_xml(ctx, rs)
     * produced the same <resultset> fragment as the old manually-built
     * xml->buffer. Now that xml->buffer's resultset section IS
     * response_write_xml(ctx, rs)'s output (spliced in above), there is no
     * separate "old" implementation left to diff against - this would only
     * ever compare the new output to itself. */
#if 0
    char *new_response_xml = response_write_xml(ctx, rs);

    if (new_response_xml)
    {
        /* Extract just <resultset>...</resultset> from the old buffer -
         * response_write_xml() only ever produces that fragment, not the
         * surrounding <output_xml>/<execution_envelope> wrapper, so
         * comparing against the whole old buffer was never a fair
         * like-for-like check.                                          */
        const char *old_start = strstr(xml->buffer, "<resultset>");
        const char *old_end   = old_start ? strstr(old_start, "</resultset>") : NULL;

        if (old_start && old_end)
        {
            old_end += strlen("</resultset>");
            size_t old_fragment_len = (size_t)(old_end - old_start);

            char *old_fragment = malloc(old_fragment_len + 1);
            if (old_fragment)
            {
                memcpy(old_fragment, old_start, old_fragment_len);
                old_fragment[old_fragment_len] = '\0';


                /* Trim trailing whitespace/newline from both before comparing -
                 * xml_end_resultset() appends "</resultset>\n" with the
                 * newline as part of that one write, which response_write_xml()'s
                 * strdup(xml->buffer) naturally picks up but this substring
                 * extraction does not - a trivial difference, not a real one. */
                size_t ol = strlen(old_fragment);
                while (ol > 0 && (old_fragment[ol-1] == '\n' || old_fragment[ol-1] == '\r' || old_fragment[ol-1] == ' '))
                    old_fragment[--ol] = '\0';

                size_t nl = strlen(new_response_xml);
                while (nl > 0 && (new_response_xml[nl-1] == '\n' || new_response_xml[nl-1] == '\r' || new_response_xml[nl-1] == ' '))
                    new_response_xml[--nl] = '\0';


                printf("\n===== OLD resultset fragment only =====\n%s\n", old_fragment);
                printf("\n===== NEW (response_write_xml) =====\n%s\n", new_response_xml);

                if (strcmp(old_fragment, new_response_xml) == 0)
                {
                    printf("\n[STAGE3] MATCH - old and new resultset XML are identical\n");
                    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                 "STAGE3 MATCH - old and new resultset XML identical");
                }
                else
                {
                    printf("\n[STAGE3] MISMATCH - old and new resultset XML differ\n");
                    logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                                 "STAGE3 MISMATCH - old and new resultset XML differ");
                }

                free(old_fragment);
            }
        }
        else
        {
            printf("\n[STAGE3] Could not find <resultset> in old buffer\n");
        }

        free(new_response_xml);
    }
    else
    {
        printf("\n[STAGE3] response_write_xml returned NULL\n");
    }
#endif


    /* ---- Stage 3b: verify JSON writer (temporary - verification only) ---- */
    char *new_response_json = response_write_json(ctx, rs);

    if (new_response_json)
    {
        printf("\n===== NEW (response_write_json) =====\n%s\n", new_response_json);
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "STAGE3b response_write_json produced output len=%zu",
                     strlen(new_response_json));

        verify_response_json_against_resultset(ctx, rs, new_response_json);

        free(new_response_json);
    }
    else
    {
        printf("\n[STAGE3b] response_write_json returned NULL\n");
    }






    if (!cfg->xml)
        cfg->xml = calloc(1, sizeof(*cfg->xml));

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Setting cfg->xml->OUTPUT_XML");
    cfg->xml->OUTPUT_XML = strdup(xml->buffer);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "execute_query_batch complete rows=%u elapsed=%.6f",
                 abs_rownum, elapsed);

    /* ---- Render JSON once, store both formats in cache ----
     * response_writer_cache_store() renders JSON from rs and, when
     * caching is enabled, stores it alongside the XML string above on
     * one cache entry - so a later hit, in either format, is served
     * directly with no re-render and no re-execution.
     *
     * This is also the actual fix for JSON requests never receiving a
     * JSON response: previously nothing populated a JSON output field
     * at all, cached or not, regardless of ReturnFormat.               */

    /* Table-level cache invalidation (closure item 5 follow-up,
     * 2026-08-12) - build the comma-separated table dependency tag
     * from deps (Stage 0's own output, already computed above - no
     * new parsing) and pass it through opts, so this entry can later
     * be found and expired by resultset_cache_invalidate_by_table()
     * when a write modifies one of these same tables. Regression fix
     * for UT-SEL-004 (a post-UPDATE SELECT could serve a stale cached
     * result up to resultset_cache_ttl_seconds after the write, since
     * nothing previously invalidated anything on write at all).       */
    char table_tag[512];
    table_tag[0] = '\0';
    for (int di = 0; di < deps.object_count; di++)
    {
        if (!deps.objects[di].object_name[0]) continue;
        size_t used = strlen(table_tag);
        if (used > 0 && used < sizeof(table_tag) - 1)
        {
            table_tag[used] = ',';
            table_tag[used + 1] = '\0';
        }
        strncat(table_tag, deps.objects[di].object_name,
                sizeof(table_tag) - strlen(table_tag) - 1);
    }

    cache_entry_opts_t cache_opts;
    memset(&cache_opts, 0, sizeof(cache_opts));
    if (table_tag[0]) cache_opts.table_dependency_tag = table_tag;

    char *json_output = NULL;
    int   json_rc = response_writer_cache_store(
                        ctx,
                        /* Bug fix (2026-08-23), found via real cache
                         * poisoning - an execute_async request leaves
                         * rs deliberately empty (all row data went to
                         * per-batch batch_rs instead - see the fetch
                         * loop above), but this call ran unconditionally
                         * regardless, storing that empty rs into
                         * resultset_cache under the request's own SQL
                         * text. Any LATER request sharing that exact SQL
                         * - sync or async, this fixture or a completely
                         * different one - then hit the same cache entry
                         * and got the empty result back. Confirmed
                         * directly: Test_Async_Valid.xml and
                         * Request_Test_Select_2.json/.xml both use
                         * "SELECT * FROM OCI_FIELD_TEST" - one poisoned
                         * the cache, the other one silently inherited
                         * it. !is_async added to the existing skip
                         * condition below, same pattern already used for
                         * served_from_cache/cache_key[0].              */
                        (ctx->resultset_cache && !served_from_cache &&
                         cache_key[0] && !is_async)
                            ? ctx->resultset_cache : NULL,
                        cache_key,
                        rs,
                        cfg->xml->OUTPUT_XML,
                        (uint64_t)abs_rownum,
                        table_tag[0] ? &cache_opts : NULL,
                        &json_output);

    if (json_rc == 0 && json_output)
    {
        if (cfg->ReturnFormat && strcasecmp(cfg->ReturnFormat, "JSON") == 0)
            cfg->OUTPUT_JSON = json_output;   /* ownership transferred to cfg */
        else
            free(json_output);                /* not requested this call     */
    }
    else
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "JSON rendering failed key='%.80s' - a JSON request "
                     "this call would not receive a response body",
                     cache_key);
    }

    /* ---- Update cache exec stats on success ----
     * The actual cache_insert (both formats) already happened above via
     * response_writer_cache_store(); this block only records execution
     * stats, matching the miss-path timing previously recorded here.  */
    if (ctx->resultset_cache && !served_from_cache &&
        cfg->xml && cfg->xml->OUTPUT_XML && cache_key[0])
    {
        if (json_rc == 0)
            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                         "Stored result in cache key='%.80s' rows=%u "
                         "(xml+json)", cache_key, abs_rownum);
        else
            logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                         "Cache store incomplete (JSON render failed, "
                         "non-fatal) key='%.80s'", cache_key);

        cache_update_exec_stats(ctx->resultset_cache,
                                elapsed * 1000.0,  /* execution_ms    */
                                0.0,               /* fetch_ms        */
                                (uint64_t)abs_rownum,
                                0,                 /* was_cache_hit   */
                                1);                /* success         */
    }

     /* ================================================================== */
     /*  WRITE BLOCK - place at the end of Stage 6 (after xml_finalize)    */
     /*  and also in the Cleanup label for the error path                   */
     /* ================================================================== */

         /* Success path - after xml_finalize() */
         metrics.end_time_us  = metrics_now_us();
         metrics.status_code  = 0;
         strncpy(metrics.error_code, "-",  sizeof(metrics.error_code) - 1);
         strncpy(metrics.error_text, "-",  sizeof(metrics.error_text) - 1);
         metrics.rows_affected    = abs_rownum;
         metrics.output_xml_bytes = (xml && xml->buffer)
                                    ? (uint64_t)strlen(xml->buffer) : 0;

         /* Bug fix (2026-09-15, found via a second ASan crash report -
          * SEGV reading BLOB_list[_bi] here). This loop pre-dates
          * Phase 2b and was already redundant by the time this pass
          * landed: lob_bytes is already correctly accumulated in the
          * shared fetch loop above (lob_bytes += stats.blob_bytes,
          * folded per batch, same as clob_bytes already was in
          * Phase 2a). BLOB_list itself is permanently NULL now (its
          * only populator, handle_blob_column_batch(), was removed in
          * Phase 2b) - but BLOB_index is NOT always 0 any more (it's
          * genuinely incremented via out_stats), so this unguarded
          * loop dereferenced a NULL array the moment any real query
          * touched a BLOB column. Every OTHER BLOB_list[...] access in
          * this file (Cleanup's own free loop) is already correctly
          * guarded behind "if (BLOB_list)" - this was the one spot
          * that wasn't, because it never needed to be until BLOB_list
          * stopped being populated. Removed outright rather than
          * guarded, since the value it computed is already computed
          * correctly elsewhere - guarding it would just make it a
          * silent no-op that still looks like it's doing something. */
         metrics.lob_bytes  = lob_bytes;
         metrics.clob_bytes = clob_bytes;
         /* transaction_id already set at init time                    */



Cleanup:

	resultset_free(rs);   /* ADD THIS LINE - not yet consumed, just avoiding a leak */

    /* ================================================================
     *  Stage 7 - Cleanup: reverse allocation order, guard all frees
     * ================================================================ */
	metrics.end_time_us = metrics_now_us();
	metrics.status_code = rc;

	/* Error path - in Cleanup label when rc != 0 */
	if (rc!=0){
		    strncpy(metrics.error_code,
		            logger_last_error.error_code,
		            sizeof(metrics.error_code) - 1);
		    strncpy(metrics.error_text,
		            logger_last_error.error_text,
		            sizeof(metrics.error_text) - 1);
	}
	if(ctx->active_tx)
		strncpy(metrics.transaction_id , tx_get_id(ctx->active_tx),sizeof(metrics.transaction_id)-1);
	else
		strncpy(metrics.transaction_id , "-",sizeof(metrics.transaction_id)-1);
	strncpy(metrics.transaction_name , ctx->active_tx ? ctx->active_tx->tx_name : "-", sizeof(metrics.transaction_name)-1);
	metrics.connection_wait_us    = ctx->connection_wait_us;
	metrics.connection_create_us  = ctx->connection_create_us;
	metrics.connection_acquire_us = ctx->connection_acquire_us;

	//Process final 3 metrics
	//printf("DEBUG : cfg->input_file_name=%s\n",cfg->input_file_name);
	if (ctx->ini && ctx->ini->metrics_display_input_file_name && cfg->input_file_name)
	    metrics.input_file_name = flatten_for_csv(cfg->input_file_name);

	if (ctx->ini && ctx->ini->metrics_display_input_request && ctx->INPUT_XML)
	    metrics.input_request = flatten_for_csv3(ctx->INPUT_XML);


	if (ctx->ini && ctx->ini->metrics_display_output_response)
	{
	    /* Serve whichever format was actually returned to the caller -
	     * this used to always read cfg->xml->OUTPUT_XML regardless of
	     * ReturnFormat, so a JSON request's metrics row showed the XML
	     * rendering instead of the JSON it actually got back.          */
	    int is_json = (cfg->ReturnFormat &&
	                   strcasecmp(cfg->ReturnFormat, "JSON") == 0);

	    if (is_json && cfg->OUTPUT_JSON)
	        metrics.output_response = flatten_for_csv3(cfg->OUTPUT_JSON);
	    else if (cfg->xml && cfg->xml->OUTPUT_XML)
	        metrics.output_response = flatten_for_csv3(cfg->xml->OUTPUT_XML);
	}

	metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &metrics);
    logger_clear_last_error();   // reset for next operation


    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Stage 7: Cleanup");

    if (stmt_count)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Calling OCIStmtRelease for stmt_count");
        OCIStmtRelease(stmt_count, ctx->errhp, NULL, 0, OCI_DEFAULT);
        stmt_count = NULL;
    }

    if (BLOB_list)
    {
        for (int i = 0; i < BLOB_index; i++)
        {
            if (BLOB_list[i].lob_loc)
            {
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "OCIDescriptorFree BLOB_list[%d].lob_loc", i);
                OCIDescriptorFree(BLOB_list[i].lob_loc, OCI_DTYPE_LOB);
                BLOB_list[i].lob_loc = NULL;
            }
            if (BLOB_list[i].blob_data)
            {
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "free(BLOB_list[%d].blob_data)", i);
                free(BLOB_list[i].blob_data);
                BLOB_list[i].blob_data = NULL;
            }
            if (BLOB_list[i].file_name)
            {
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "free(BLOB_list[%d].file_name)", i);
                free(BLOB_list[i].file_name);
                BLOB_list[i].file_name = NULL;
            }
        }
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "free(BLOB_list)");
        free(BLOB_list);
        BLOB_list = NULL;
    }

    /* free_batch_ctx(ctx, &bc) removed (Phase 2b, 2026-09-15) - the
     * function itself is gone, and bc's only field anything still
     * populates is bc.fetch_count (a plain int, nothing to free).
     * Every other bc field stays permanently NULL now, same as
     * stmt/stmt_count/BLOB_list below - nothing left to release here. */

    if (xml)
    {
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "xml_free(xml)");
        xml_free(xml);
        xml = NULL;
    }

    if (stmt)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Calling OCIStmtRelease for stmt");
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        stmt = NULL;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Cleanup complete. rc=%d", rc);
    return rc;
}
