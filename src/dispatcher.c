/* ======================================================================
 * dispatcher.c
 *
 * Stage 1 extraction (File_Consumer_proposal v1.2). Contains everything
 * that was process_xml_file() and its dispatch chain in
 * Test_XML_Runner.c:
 *
 *   read_file()             - exported, see dispatcher.h for why
 *   extract_tag()            - internal helper, legacy flat-XML parsing
 *   upper()                  - internal helper
 *   dispatch_select()        - legacy flat-XML SELECT handler
 *   dispatch_select_new()    - new-pipeline SELECT handler
 *   dispatch_insert_new()    - new-pipeline INSERT handler
 *   dispatch_update_new()    - new-pipeline UPDATE handler
 *   dispatch_delete_new()    - new-pipeline DELETE handler
 *   dispatch_procedure_new() - new-pipeline EXECUTE_PROCEDURE handler
 *   process_xml_file()       - exported entry point, ties it all together
 *
 * Moved verbatim out of Test_XML_Runner.c - no logic changes. Verify
 * against the existing fixtures/logs before wiring this in to confirm
 * byte-identical behavior, per Stage 1 of the implementation plan.
 * ====================================================================== */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>

#include "dispatcher.h"

#include "OCI_Connection.h"
#include "OCI_Insert_Execute_Module.h"
#include "OCI_Update_Execute_Module.h"
#include "OCI_Delete_Execute_Module.h"
#include "OCI_Execute_Procedure_Module.h"
#include "logger.h"
#include "ini_reader.h"
#include "OCI_Level1_Parser.h"
#include "OCI_Level2_Parser.h"
#include "OCI_Request_Response_Types.h"
#include "OCI_Execute_Query_Batch_Module.h"
#include "metrics.h"

/* Moved from Test_XML_Runner.c unchanged */
#define MAX_XML_FILE_SIZE  (4 * 1024 * 1024)   /* 4 MB per file        */
#define MAX_OPERATION_LEN  32

/* ------------------------------------------------------------------ */
/*  Read entire file into a heap buffer.                               */
/*  Caller must free() the returned pointer.                           */
/*  Returns NULL on error.                                             */
/* ------------------------------------------------------------------ */
char *read_file(const char *path, long *out_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0 || sz > MAX_XML_FILE_SIZE)
    {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, fp);
    buf[n] = '\0';
    fclose(fp);

    if (out_len) *out_len = (long)n;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Helper: extract text between <tag> and </tag>.                     */
/*  Returns 1 on success, 0 if not found.                              */
/* ------------------------------------------------------------------ */
static int extract_tag(const char *src, const char *tag,
                        char *dest, size_t dest_max)
{
    char open[64], close[64];
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

    /* Trim whitespace */
    char *p = dest;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != dest) memmove(dest, p, strlen(p) + 1);
    int l = (int)strlen(dest);
    while (l > 0 && isspace((unsigned char)dest[l-1]))
    { dest[l-1] = '\0'; l--; }

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Helper: uppercase in-place                                          */
/* ------------------------------------------------------------------ */
static void upper(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* ================================================================== */
/* dispatch_insert (legacy, flat-XML <operation>INSERT</operation>
 * format) removed - unlike SELECT, where execute_query_batch()'s
 * signature never changed so the old and new dispatchers could keep
 * coexisting unchanged, execute_insert_batch()'s signature changed
 * from a raw XML string to insert_request_t* as part of this
 * refactor. The old flat format is retired for INSERT specifically;
 * see dispatch_insert_new() below for the new-pipeline replacement,
 * wired into the new-format branch the same way dispatch_select_new()
 * is. */

/* ================================================================== */
/*  dispatch_select                                                     */
/* ================================================================== */
static int dispatch_select(oci_context_t *ctx,
                            const char    *filename,
                            const char    *xml)
{
    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "Dispatching SELECT: %s", filename);


    /*Comment out these lines to remove tester for the parser*/
    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                  "***********Calling Test_sql_dependency_extractor ***********************");
    /*TL 21-June comment out this test as invalid sql has an issue with returing 0 when a previous phase of test fail*/
    /*test_sql_dependency_extractor(ctx);*/


    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                   "***********Finished Test_sql_dependency_extractor ***********************");

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                  "****** Calling get_table_metadata OCI_FIELD_TEST ******");

    table_metadata_alltabs_t *tm = get_table_metadata(ctx,
                                                "DATA_MANAGER",
                                                "OCI_FIELD_TEST");
     if (tm)
     {
         logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                      "get_table_metadata OK: owner='%s' table='%s' "
                      "status='%s' num_rows=%.0f compression='%s' "
                      "partitioned='%s' last_analyzed='%s'",
                      tm->owner,
                      tm->table_name,
                      tm->status,
                      tm->num_rows,
                      tm->compression,
                      tm->partitioned,
                      tm->last_analyzed);

         free_table_metadata(tm);
         tm = NULL;
     }
     else
     {
         logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                      "get_table_metadata FAILED for OCI_FIELD_TEST "
                      "- check Metadata_Data_Manager.log");
     }

     logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                  "****** Finished get_table_metadata ******");

    char sql_buf[8192] = {0};
    if (!extract_tag(xml, "sql", sql_buf, sizeof(sql_buf)))
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT]: %s - no <sql> element found",
                     filename);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.SQL                   = sql_buf;
    cfg.max_rows              = ctx->ini->query_max_record_count;
    cfg.fetch_array_size      = ctx->ini->query_fetch_batch_size;
    cfg.log_execution_results = 1;
    cfg.include_column_names  = 1;
    cfg.input_file_name = (char *)filename;   /* points to caller's string, no alloc needed */

    int rc = execute_query_batch(ctx, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "PASS [SELECT]: %s", filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT]: %s (rc=%d)", filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    return rc;
}

/* ================================================================== */
/*  dispatch_select_new                                                 */
/*  SELECT via the new format-agnostic pipeline (Level 1 already ran,   */
/*  Level 2 already validated req->sql - this is purely the thin        */
/*  adapter from select_request_t to execute_config_t, then the same    */
/*  execute_query_batch() call the old dispatch_select() already uses,  */
/*  unchanged).                                                          */
/* ================================================================== */
static int dispatch_select_new(oci_context_t       *ctx,
                                const char          *filename,
                                input_c_request_t   *request,
                                input_c_operation_t *op)
{
    select_request_t *req = (select_request_t *)op->payload;

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT/new]: %s - no select_request_t payload",
                     filename);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* req->sql is already a writable char[4096] buffer, not a pointer
     * to a literal - safe to point cfg.SQL directly at it. See
     * OCI_Session_Manager.c's session_reconcile_orphans() design note
     * for why that distinction matters (trim_sql_inplace() modifies
     * cfg->SQL in place).                                              */
    cfg.SQL = req->sql;

    /* 0 means "use the config default" per select_request_t's own doc
     * comment - Level 1 deliberately left these as 0 rather than
     * guessing, same as the old dispatch_select() already does.        */
    cfg.max_rows         = (req->max_rows > 0)
                            ? req->max_rows
                            : ctx->ini->query_max_record_count;
    cfg.fetch_array_size = (req->fetch_batch_size > 0)
                            ? req->fetch_batch_size
                            : ctx->ini->query_fetch_batch_size;

    cfg.log_execution_results = 1;
    cfg.include_column_names  = req->include_column_names;
    cfg.input_file_name        = (char *)filename;

    /* This is the actual fix for JSON requests never getting a JSON
     * response (and, downstream of that, metrics always showing XML
     * in output_response): ReturnFormat existed on execute_config_t
     * but nothing ever set it, so every format-aware check added later
     * (cache hit serving, response_writer_cache_store(), the metrics
     * output_response fix) always evaluated as "not JSON" regardless
     * of what was actually requested.                                  */
    cfg.ReturnFormat = (request->source_format == INPUT_FORMAT_JSON)
                        ? "JSON" : "XML";

    int rc = execute_query_batch(ctx, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "PASS [SELECT/new] audit_id=%s: %s",
                     request->external_audit_id, filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
        if (cfg.OUTPUT_JSON)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result JSON:\n%s", cfg.OUTPUT_JSON);
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }

    /* Now that ReturnFormat is actually set above, cfg.OUTPUT_JSON can
     * really be populated by execute_query_batch() - free it here or
     * this becomes a genuine leak on every JSON request.               */
    if (cfg.OUTPUT_JSON) free(cfg.OUTPUT_JSON);

    return rc;
}

/* ================================================================== */
/*  dispatch_insert_new                                                 */
/*  INSERT via the new format-agnostic pipeline (Level 1 already ran,   */
/*  Level 2 already validated req - level2_validate_insert() also runs  */
/*  again internally as execute_insert_batch()'s own first step, so     */
/*  this is genuinely just the thin adapter from insert_request_t to    */
/*  execute_config_t, mirroring dispatch_select_new() exactly).         */
/* ================================================================== */
static int dispatch_insert_new(oci_context_t       *ctx,
                                const char          *filename,
                                input_c_request_t   *request,
                                input_c_operation_t *op)
{
    insert_request_t *req = (insert_request_t *)op->payload;

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [INSERT/new]: %s - no insert_request_t payload",
                     filename);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;

    /* Same reasoning as dispatch_select_new()'s own ReturnFormat note -
     * without this, JSON-format INSERT requests would silently only
     * ever get an XML response back.                                  */
    cfg.ReturnFormat = (request->source_format == INPUT_FORMAT_JSON)
                        ? "JSON" : "XML";

    int rc = execute_insert_batch(ctx, req, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "PASS [INSERT/new] audit_id=%s: %s",
                     request->external_audit_id, filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
        if (cfg.OUTPUT_JSON)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result JSON:\n%s", cfg.OUTPUT_JSON);
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [INSERT/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    if (cfg.OUTPUT_JSON) free(cfg.OUTPUT_JSON);

    return rc;
}

/* ================================================================== */
/*  dispatch_update_new                                                 */
/*  UPDATE via the new format-agnostic pipeline - mirrors               */
/*  dispatch_insert_new() exactly. level2_validate_update() also runs   */
/*  again internally as execute_update_batch()'s own first step, so     */
/*  this is genuinely just the thin adapter from update_request_t to    */
/*  execute_config_t.                                                    */
/* ================================================================== */
static int dispatch_update_new(oci_context_t       *ctx,
                                const char          *filename,
                                input_c_request_t   *request,
                                input_c_operation_t *op)
{
    update_request_t *req = (update_request_t *)op->payload;

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [UPDATE/new]: %s - no update_request_t payload",
                     filename);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;

    /* Same reasoning as dispatch_select_new()'s own ReturnFormat note -
     * without this, JSON-format UPDATE requests would silently only
     * ever get an XML response back.                                  */
    cfg.ReturnFormat = (request->source_format == INPUT_FORMAT_JSON)
                        ? "JSON" : "XML";

    int rc = execute_update_batch(ctx, req, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "PASS [UPDATE/new] audit_id=%s: %s",
                     request->external_audit_id, filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
        if (cfg.OUTPUT_JSON)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result JSON:\n%s", cfg.OUTPUT_JSON);
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [UPDATE/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    if (cfg.OUTPUT_JSON) free(cfg.OUTPUT_JSON);

    return rc;
}

/* ================================================================== */
/*  dispatch_delete_new                                                 */
/*  DELETE via the new format-agnostic pipeline - mirrors               */
/*  dispatch_update_new() exactly. level2_validate_delete() also runs   */
/*  again internally as execute_delete_batch()'s own first step, so     */
/*  this is genuinely just the thin adapter from delete_request_t to    */
/*  execute_config_t.                                                    */
/* ================================================================== */
static int dispatch_delete_new(oci_context_t       *ctx,
                                const char          *filename,
                                input_c_request_t   *request,
                                input_c_operation_t *op)
{
    delete_request_t *req = (delete_request_t *)op->payload;

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [DELETE/new]: %s - no delete_request_t payload",
                     filename);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;

    /* Same reasoning as dispatch_select_new()'s own ReturnFormat note -
     * without this, JSON-format DELETE requests would silently only
     * ever get an XML response back.                                  */
    cfg.ReturnFormat = (request->source_format == INPUT_FORMAT_JSON)
                        ? "JSON" : "XML";

    int rc = execute_delete_batch(ctx, req, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "PASS [DELETE/new] audit_id=%s: %s",
                     request->external_audit_id, filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
        if (cfg.OUTPUT_JSON)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result JSON:\n%s", cfg.OUTPUT_JSON);
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [DELETE/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    if (cfg.OUTPUT_JSON) free(cfg.OUTPUT_JSON);

    return rc;
}

/* ================================================================== */
/*  dispatch_procedure_new                                              */
/*  EXECUTE_PROCEDURE via the new format-agnostic pipeline - mirrors    */
/*  dispatch_delete_new() exactly. level2_validate_procedure() also     */
/*  runs again internally as execute_procedure()'s own first step, so   */
/*  this is genuinely just the thin adapter from                        */
/*  execute_procedure_request_t to execute_config_t.                    */
/* ================================================================== */
static int dispatch_procedure_new(oci_context_t       *ctx,
                                   const char          *filename,
                                   input_c_request_t   *request,
                                   input_c_operation_t *op)
{
    execute_procedure_request_t *req = (execute_procedure_request_t *)op->payload;

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [EXECUTE_PROCEDURE/new]: %s - no "
                     "execute_procedure_request_t payload",
                     filename);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;

    /* Same reasoning as dispatch_select_new()'s own ReturnFormat note -
     * without this, JSON-format EXECUTE_PROCEDURE requests would
     * silently only ever get an XML response back.                     */
    cfg.ReturnFormat = (request->source_format == INPUT_FORMAT_JSON)
                        ? "JSON" : "XML";

    int rc = execute_procedure(ctx, req, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "PASS [EXECUTE_PROCEDURE/new] audit_id=%s: %s",
                     request->external_audit_id, filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
        if (cfg.OUTPUT_JSON)
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result JSON:\n%s", cfg.OUTPUT_JSON);
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [EXECUTE_PROCEDURE/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    if (cfg.OUTPUT_JSON) free(cfg.OUTPUT_JSON);

    return rc;
}

/* ================================================================== */
/*  process_xml_file                                                    */
/*  Read file, extract operation, dispatch to correct handler.         */
/* ================================================================== */
int process_xml_file(oci_context_t *ctx,
                      const char    *filepath,
                      const char    *filename)
{
    long  len = 0;

    char *xml = read_file(filepath, &len);

    if (!xml)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "Failed to read file: %s", filepath);
        return -1;
    }
    if (xml) {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "Read file: %s.  Updating ctx->INPUT_XML", filepath);
        free(ctx->INPUT_XML);                  /* clear any previous value  */
        ctx->INPUT_XML = strdup(xml);          /* heap copy - ctx owns it   */
    }


    /* ---- Try the new format-agnostic pipeline first (SELECT only for
     * now, per 2026-07-19 decision) - the cheap pre-check means
     * level1_parse() is only ever called on files that already look
     * like new-format requests, so its own error logging stays
     * meaningful rather than firing on every old-format file. Old
     * dispatch below is untouched and stays the fallback for
     * everything else, both formats coexisting in xml_input_dir. */
    if (level1_looks_like_new_format(xml, (size_t)len))
    {
        input_c_request_t   new_request;
        operation_status_t  level1_error;
        memset(&new_request, 0, sizeof(new_request));
        memset(&level1_error, 0, sizeof(level1_error));

        uint64_t level1_start = metrics_now_us();
        int level1_rc = level1_parse(ctx, xml, (size_t)len,
                                      &new_request, &level1_error);
        ctx->level1_parse_us = metrics_now_us() - level1_start;

        if (level1_rc != LEVEL1_OK)
        {
            logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                         "FAIL [Level1] %s error_code=%s error_text=%s",
                         filename, level1_error.error_code, level1_error.error_text);
            free(xml);
            return -1;
        }

        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "File='%s' matched new request format - audit_id=%s "
                     "operations=%d", filename, new_request.external_audit_id,
                     new_request.operation_count);

        uint64_t level2_start = metrics_now_us();
        int level2_rc = level2_validate(ctx, &new_request);
        ctx->level2_parse_us = metrics_now_us() - level2_start;
        int rc = 0;

        if (level2_rc == LEVEL2_OK)
        {
            for (int i = 0; i < new_request.operation_count; i++)
            {
                input_c_operation_t *op = &new_request.operations[i];

                if (op->type == OP_SELECT)
                {
                    rc = dispatch_select_new(ctx, filename, &new_request, op);
                }
                else if (op->type == OP_INSERT)
                {
                    rc = dispatch_insert_new(ctx, filename, &new_request, op);
                }
                else if (op->type == OP_UPDATE)
                {
                    rc = dispatch_update_new(ctx, filename, &new_request, op);
                }
                else if (op->type == OP_DELETE)
                {
                    rc = dispatch_delete_new(ctx, filename, &new_request, op);
                }
                else if (op->type == OP_EXECUTE_PROCEDURE)
                {
                    rc = dispatch_procedure_new(ctx, filename, &new_request, op);
                }
                else
                {
                    /* Every other operation type still runs through the
                     * old dispatch below for now - not applicable here,
                     * since a new-format file's body doesn't match what
                     * the old extract_tag()-based dispatch expects
                     * anyway. Logged clearly rather than silently
                     * skipped, so it's obvious in the log why nothing
                     * happened for this operation.                     */
                    logger_write(ctx->dispatcher_logger, LOG_WARN, __func__, 0,
                                 "File='%s' operation[%d] type=%d - new "
                                 "pipeline only implements SELECT/INSERT/"
                                 "UPDATE/DELETE/EXECUTE_PROCEDURE so far, "
                                 "skipping", filename, i, (int)op->type);
                }
            }
        }
        else
        {
            int failed_op = -1;
            for (int i = 0; i < new_request.operation_count; i++)
                if (new_request.operations[i].validation_status.status_code != 0)
                { failed_op = i; break; }

            if (failed_op >= 0)
                logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                             "FAIL [Level2] %s operation[%d] error_code=%s error_text=%s",
                             filename, failed_op,
                             new_request.operations[failed_op].validation_status.error_code,
                             new_request.operations[failed_op].validation_status.error_text);
            rc = -1;
        }

        level1_free_request(&new_request);
        free(xml);
        return rc;
    }



    char operation[MAX_OPERATION_LEN] = {0};
    if (!extract_tag(xml, "operation", operation, sizeof(operation)))
    {
        logger_write(ctx->dispatcher_logger, LOG_WARN, __func__, 0,
                     "No <operation> found in %s - skipping", filename);
        free(xml);
        return 0;
    }
    upper(operation);

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "File='%s' operation='%s' size=%ld bytes",
                 filename, operation, len);

    int rc = 0;

    if      (strcmp(operation, "INSERT") == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "File='%s' is old flat-XML INSERT format - no longer "
                     "supported (execute_insert_batch() now requires "
                     "insert_request_t via the new pipeline). Convert this "
                     "fixture to the new <request version=\"1.0\">...<operation "
                     "type=\"INSERT\"> format.", filename);
        rc = -1;
    }
    else if (strcmp(operation, "SELECT") == 0)
        rc = dispatch_select(ctx, filename, xml);
    else if (strcmp(operation, "UPDATE") == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "File='%s' is old flat-XML UPDATE format - no longer "
                     "supported (execute_update_batch() now requires "
                     "update_request_t via the new pipeline). Convert this "
                     "fixture to the new <request version=\"1.0\">...<operation "
                     "type=\"UPDATE\"> format.", filename);
        rc = -1;
    }
    else if (strcmp(operation, "DELETE") == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "File='%s' is old flat-XML DELETE format - no longer "
                     "supported (execute_delete_batch() now requires "
                     "delete_request_t via the new pipeline). Convert this "
                     "fixture to the new <request version=\"1.0\">...<operation "
                     "type=\"DELETE\"> format.", filename);
        rc = -1;
    }
    else if (strcmp(operation, "EXECUTE_PROCEDURE") == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "File='%s' is old flat-XML EXECUTE_PROCEDURE format - "
                     "no longer supported (execute_procedure() now requires "
                     "execute_procedure_request_t via the new pipeline). "
                     "Convert this fixture to the new <request version=\"1.0\">"
                     "...<operation type=\"EXECUTE_PROCEDURE\"> format.", filename);
        rc = -1;
    }
    else
        logger_write(ctx->dispatcher_logger, LOG_WARN, __func__, 0,
                     "Unknown operation '%s' in %s - skipping",
                     operation, filename);

    free(xml);
    return rc;
}
