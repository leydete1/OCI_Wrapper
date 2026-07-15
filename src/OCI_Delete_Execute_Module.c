/*
 * OCI_Delete_Execute_Module.c
 *
 * Stage 3 - Delete Execute Module
 * --------------------------------
 * Executes a DELETE from a <Delete_Template> XML.
 * Based on OCI_Update_Execute_Module with the SET clause removed.
 *
 * Internal structure
 * ------------------
 *   parse_delete_xml()       - parse <where> key fields from XML
 *   build_delete_sql()       - build DELETE FROM ... WHERE key=:1 AND ...
 *   execute_delete_batch()   - orchestrate: validate -> parse -> bind ->
 *                              execute -> commit -> result XML -> cleanup
 *
 * Key differences from update
 * ---------------------------
 *   - No <row> blocks, no SET columns, no LOB handling.
 *   - No column metadata query (types come from <field_type> in XML).
 *   - WHERE bind positions start at :1 (not after a SET list).
 *   - Empty <where> block is rejected before any OCI call.
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
#include "XML_Helper.h"
#include "logger.h"
#include "metrics.h"
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
static void trim_del(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    { s[len - 1] = '\0'; len--; }
}

static int extract_tag_del(const char *src, const char *tag,
                            char *dest, size_t dest_max)
{
    char open[132], close[132];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *s = strstr(src, open);
    if (!s) return 0;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return 0;
    size_t len = (size_t)(e - s);
    if (len >= dest_max) len = dest_max - 1;
    memcpy(dest, s, len);
    dest[len] = '\0';
    trim_del(dest);
    return 1;
}

/* ================================================================== */
/*  parse_delete_xml                                                    */
/*  Extract table_name, owner, and all <key_field> entries from the   */
/*  <where> block.  Rejects missing or empty <where>.                 */
/* ================================================================== */
static int parse_delete_xml(oci_context_t *ctx,
                              const char    *xml,
                              delete_ctx_t  *dc)
{
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Entering parse_delete_xml");

    memset(dc, 0, sizeof(*dc));

    /* ---- table_name (required) ---- */
    if (!extract_tag_del(xml, "table_name",
                          dc->table_name, sizeof(dc->table_name)))
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Missing <table_name> in delete XML");
        return -1;
    }

    /* ---- owner (optional) ---- */
    extract_tag_del(xml, "owner", dc->owner, sizeof(dc->owner));

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "table='%s' owner='%s'", dc->table_name, dc->owner);

    /* ---- Locate <where> block ---- */
    const char *where_start = strstr(xml, "<where>");
    const char *where_end   = strstr(xml, "</where>");

    if (!where_start || !where_end || where_end <= where_start)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Missing or malformed <where> block in delete XML");
        return -1;
    }

    /* Copy the <where> block into a working buffer */
    size_t where_len = (size_t)(where_end - where_start) + 8;
    char  *where_buf = malloc(where_len + 1);
    if (!where_buf) return -1;
    memcpy(where_buf, where_start, where_len);
    where_buf[where_len] = '\0';

    /* ---- Walk every <key_field> entry ---- */
    const char *kp = where_buf;
    while ((kp = strstr(kp, "<key_field>")) != NULL)
    {
        const char *ke = strstr(kp, "</key_field>");
        if (!ke || dc->key_count >= MAX_DEL_KEY_COLS) break;

        size_t klen = (size_t)(ke - kp) + 12;
        char  *kbuf = malloc(klen + 1);
        if (!kbuf) { free(where_buf); return -1; }
        memcpy(kbuf, kp, klen);
        kbuf[klen] = '\0';

        del_key_field_t *kf = &dc->keys[dc->key_count];

        extract_tag_del(kbuf, "field_name", kf->field_name,
                        sizeof(kf->field_name));
        extract_tag_del(kbuf, "field_type", kf->field_type,
                        sizeof(kf->field_type));
        extract_tag_del(kbuf, "key_value",  kf->key_value,
                        sizeof(kf->key_value));

        logger_write(ctx->delete_logger, LOG_DEBUG, __func__, 0,
                     "Key field %d: name='%s' type='%s' value='%s'",
                     dc->key_count + 1,
                     kf->field_name, kf->field_type, kf->key_value);

        free(kbuf);
        dc->key_count++;
        kp = ke + 12;
    }

    free(where_buf);

    /* ---- Safety: reject empty WHERE to prevent full-table delete ---- */
    if (dc->key_count == 0)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "No <key_field> entries found in <where> block - "
                     "refusing to execute a full-table DELETE");
        return -1;
    }

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "parse_delete_xml OK: table='%s' owner='%s' keys=%d",
                 dc->table_name, dc->owner, dc->key_count);
    return 0;
}

/* ================================================================== */
/*  get_del_key_wrapper                                                 */
/*  Returns a SQL conversion wrapper for date/time key types.          */
/*  Plain scalar types return NULL (bind as SQLT_STR directly).        */
/* ================================================================== */
static const char *get_del_key_wrapper(const char *dtype)
{
    if (strcmp(dtype, "DATE") == 0)
        return "TO_DATE(%s,'YYYY-MM-DD HH24:MI:SS')";
    if (strncmp(dtype, "TIMESTAMP", 9) == 0)
        return "TO_TIMESTAMP(%s,'YYYY-MM-DD HH24:MI:SS.FF6')";
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "MONTH"))
        return "TO_YMINTERVAL(%s)";
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "SECOND"))
        return "TO_DSINTERVAL(%s)";
    return NULL;   /* VARCHAR2, NUMBER, CHAR, RAW, etc. - no wrapper */
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

        const char *wrapper = get_del_key_wrapper(dc->keys[k].field_type);
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
int execute_delete_batch(oci_context_t    *ctx,
                          const char       *template_xml,
                          execute_config_t *cfg)
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

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Entering execute_delete_batch");

    if (!ctx || !template_xml || !cfg)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments");
        return -1;
    }
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
     *  Stage 1 - Parse XML
     *  Validate that a non-empty <where> block exists before touching
     *  the database.  Reject immediately on any parse failure.
     * ================================================================ */
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Parsing delete XML");

    dc = calloc(1, sizeof(delete_ctx_t));
    if (!dc)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for delete_ctx_t");
        rc = -1;
        goto Cleanup;
    }

    if (parse_delete_xml(ctx, template_xml, dc) != 0)
    {
        logger_write(ctx->delete_logger, LOG_ERROR, __func__, 0,
                     "parse_delete_xml failed");
        rc = -1;
        goto Cleanup;
    }
    strncpy(metrics.object_name, dc->table_name,
             sizeof(metrics.object_name) - 1);

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 1 parse OK: table='%s' owner='%s' keys=%d",
                 dc->table_name, dc->owner, dc->key_count);

    /* ================================================================
     *  Stage 2 - Build SQL and prepare statement
     * ================================================================ */
    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 2: Building SQL and preparing statement");

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
     *  Stage 6 - Build result XML
     * ================================================================ */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Stage 6: Building result XML elapsed=%.6f", elapsed);

    xml = xml_create(4096);
    if (!xml) { rc = -1; goto Cleanup; }

    xml_start_document(xml);
    xml_start_execution(xml);
    xml_append(xml, "<operation>DELETE</operation>\n");
    xml_append(xml, "<table_name>%s</table_name>\n", dc->table_name);
    xml_append(xml, "<owner>%s</owner>\n",            dc->owner);
    xml_append(xml, "<rows_deleted>%d</rows_deleted>\n",   rows_deleted);
    xml_append(xml, "<execution_time>%.6f</execution_time>\n", elapsed);

    /* Echo back the WHERE keys used so the caller can confirm */
    xml_append(xml, "<where_keys>\n");
    for (int k = 0; k < dc->key_count; k++)
    {
        xml_append(xml,
                   "  <key_field>"
                   "<field_name>%s</field_name>"
                   "<field_type>%s</field_type>"
                   "<key_value>%s</key_value>"
                   "</key_field>\n",
                   dc->keys[k].field_name,
                   dc->keys[k].field_type,
                   dc->keys[k].key_value);
    }
    xml_append(xml, "</where_keys>\n");

    xml_end_execution(xml);
    xml_finalize(xml);
    metrics.end_time_us      = metrics_now_us();
    metrics.status_code      = 0;
    strncpy(metrics.error_code, "-", sizeof(metrics.error_code) - 1);
    strncpy(metrics.error_text, "-", sizeof(metrics.error_text) - 1);
    metrics.rows_affected    = rows_affected;
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
	//printf("DEBUG : metrics.input_xml=%s\n",metrics.input_xml);

	//printf("DEBUG : OCI_execute_query)batch.c ctx->INPUT_XML=%s\n",ctx->INPUT_XML);
	//printf("DEBUG : OCI_execute_query)batch.c  ctx->ini->metrics_display_input_xml=%d\n",ctx->ini->metrics_display_input_xml);
	if (ctx->ini && ctx->ini->metrics_display_input_xml && ctx->INPUT_XML)
	    metrics.input_xml = flatten_for_csv3(ctx->INPUT_XML);
	//printf("DEBUG :  OCI_execute_query)batch.c  metrics.input_xml=%s\n",metrics.input_xml);


	//printf("DEBUG : ctx->OUTPUT_XML=%s\n",ctx->OUTPUT_XML);
	//printf("DEBUG : ctx->ini->metrics_display_output_xml=%d\n",ctx->ini->metrics_display_output_xml);
	if (ctx->ini && ctx->ini->metrics_display_output_xml &&
	    cfg->xml && cfg->xml->OUTPUT_XML)
	    metrics.output_xml = flatten_for_csv3(cfg->xml->OUTPUT_XML);
	//printf("DEBUG : metrics.output_xml=%s\n",metrics.output_xml);



	metrics_finalise(&metrics);
	metrics_write(ctx->metrics_logger, &metrics);
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

    if (stmt)
    {
        logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                     "OCIStmtRelease stmt");
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    }

    logger_write(ctx->delete_logger, LOG_INFO, __func__, 0,
                 "Cleanup complete rc=%d", rc);
    return rc;
}
