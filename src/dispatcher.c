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
#include "response_object.h"

#include "OCI_Connection.h"
#include "OCI_Insert_Execute_Module.h"
#include "OCI_Update_Execute_Module.h"
#include "OCI_Delete_Execute_Module.h"
#include "OCI_Execute_Procedure_Module.h"
#include "logger.h"
#include "OCI_Session_Manager.h"   /* session_validate() - Session Manager
                                      proposal, Stage 3 (2026-08-08) */
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

/* ------------------------------------------------------------------ */
/*  xml_escape_alloc() / json_escape_alloc()                           */
/*                                                                      */
/*  Stage 3 helpers. error_text often echoes back field names or SQL    */
/*  fragments (see e.g. level2_validate_insert()'s "Field 'X': ..."     */
/*  messages) - those can legitimately contain <, >, &, ", or \, any    */
/*  of which would corrupt the envelope they're being embedded in if    */
/*  written verbatim. Both return a heap-allocated buffer the caller    */
/*  must free(); never return NULL (worst case, an empty string).       */
/* ------------------------------------------------------------------ */
static char *xml_escape_alloc(const char *s)
{
    if (!s) s = "";
    size_t len = strlen(s);
    /* Worst case every byte becomes "&quot;" (6 bytes) */
    char *out = malloc(len * 6 + 1);
    if (!out) return strdup("");

    char *w = out;
    for (const char *p = s; *p; p++)
    {
        switch (*p)
        {
            case '&':  memcpy(w, "&amp;",  5); w += 5; break;
            case '<':  memcpy(w, "&lt;",   4); w += 4; break;
            case '>':  memcpy(w, "&gt;",   4); w += 4; break;
            case '"':  memcpy(w, "&quot;", 6); w += 6; break;
            case '\'': memcpy(w, "&apos;", 6); w += 6; break;
            default:   *w++ = *p; break;
        }
    }
    *w = '\0';
    return out;
}

static char *json_escape_alloc(const char *s)
{
    if (!s) s = "";
    size_t len = strlen(s);
    /* Worst case every byte becomes "\u00XX" (6 bytes) */
    char *out = malloc(len * 6 + 1);
    if (!out) return strdup("");

    char *w = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        switch (*p)
        {
            case '"':  memcpy(w, "\\\"", 2); w += 2; break;
            case '\\': memcpy(w, "\\\\", 2); w += 2; break;
            case '\n': memcpy(w, "\\n",  2); w += 2; break;
            case '\r': memcpy(w, "\\r",  2); w += 2; break;
            case '\t': memcpy(w, "\\t",  2); w += 2; break;
            default:
                if (*p < 0x20)
                {
                    w += sprintf(w, "\\u%04x", *p);
                }
                else
                {
                    *w++ = (char)*p;
                }
                break;
        }
    }
    *w = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/*  build_error_envelope()                                              */
/*                                                                      */
/*  Stage 3. Synthesizes resp->response_body as a generic error         */
/*  envelope (XML or JSON, matching is_json) for any failure path that  */
/*  doesn't already have a real result body to hand back - which today  */
/*  is every failure path except none (see response_object.h's own      */
/*  doc comment on why execute_*_batch() failures don't carry           */
/*  structured detail yet). Always leaves resp in a valid, writable     */
/*  state - response_body is guaranteed non-NULL after this returns.    */
/* ------------------------------------------------------------------ */
void build_error_envelope(response_object_t *resp,
                                  const char         *audit_id,
                                  const char         *operation,
                                  const char         *error_code,
                                  const char         *error_text,
                                  int                 is_json)
{
    resp->status = RESPONSE_STATUS_ERROR;
    resp->is_json = is_json;

    strncpy(resp->audit_id,  audit_id  ? audit_id  : "-", sizeof(resp->audit_id)  - 1);
    strncpy(resp->operation, operation ? operation : "-", sizeof(resp->operation) - 1);
    strncpy(resp->error_code, error_code ? error_code : "UNKNOWN_ERROR",
            sizeof(resp->error_code) - 1);
    strncpy(resp->error_text, error_text ? error_text : "-",
            sizeof(resp->error_text) - 1);

    if (resp->response_body) { free(resp->response_body); resp->response_body = NULL; }

    if (is_json)
    {
        char *esc_audit = json_escape_alloc(resp->audit_id);
        char *esc_op    = json_escape_alloc(resp->operation);
        char *esc_code  = json_escape_alloc(resp->error_code);
        char *esc_text  = json_escape_alloc(resp->error_text);

        size_t bufsize = strlen(esc_audit) + strlen(esc_op) +
                          strlen(esc_code) + strlen(esc_text) + 128;
        resp->response_body = malloc(bufsize);
        if (resp->response_body)
            snprintf(resp->response_body, bufsize,
                     "{\"status\":\"ERROR\",\"audit_id\":\"%s\","
                     "\"operation\":\"%s\",\"error_code\":\"%s\","
                     "\"error_text\":\"%s\"}",
                     esc_audit, esc_op, esc_code, esc_text);

        free(esc_audit); free(esc_op); free(esc_code); free(esc_text);
    }
    else
    {
        char *esc_audit = xml_escape_alloc(resp->audit_id);
        char *esc_op    = xml_escape_alloc(resp->operation);
        char *esc_code  = xml_escape_alloc(resp->error_code);
        char *esc_text  = xml_escape_alloc(resp->error_text);

        size_t bufsize = strlen(esc_audit) + strlen(esc_op) +
                          strlen(esc_code) + strlen(esc_text) + 256;
        resp->response_body = malloc(bufsize);
        if (resp->response_body)
            snprintf(resp->response_body, bufsize,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<output_xml>\n<execution_envelope>\n"
                     "<status>ERROR</status>\n"
                     "<audit_id>%s</audit_id>\n"
                     "<operation>%s</operation>\n"
                     "<error_code>%s</error_code>\n"
                     "<error_text>%s</error_text>\n"
                     "</execution_envelope>\n</output_xml>\n",
                     esc_audit, esc_op, esc_code, esc_text);

        free(esc_audit); free(esc_op); free(esc_code); free(esc_text);
    }

    /* malloc() above could theoretically fail on a genuinely starved
     * system - don't hand back a NULL body in that case, since every
     * caller (Response Manager included) relies on response_body
     * always being non-NULL per response_object.h's contract.         */
    if (!resp->response_body)
        resp->response_body = strdup(is_json ? "{\"status\":\"ERROR\"}"
                                              : "<output_xml><execution_envelope>"
                                                "<status>ERROR</status>"
                                                "</execution_envelope></output_xml>");
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
static int dispatch_select(oci_context_t      *ctx,
                            const char         *filename,
                            const char         *xml,
                            response_object_t  *resp)
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
        build_error_envelope(resp, "-", "SELECT", "NO_SQL_ELEMENT",
                              "No <sql> element found in legacy-format request",
                              0);
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
        {
            logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
            resp->status = RESPONSE_STATUS_PASS;
            resp->is_json = 0;
            strncpy(resp->operation, "SELECT", sizeof(resp->operation) - 1);
            resp->response_body = cfg.xml->OUTPUT_XML;  /* ownership transferred */
            cfg.xml->OUTPUT_XML = NULL;                 /* don't double-free below */
        }
        else
        {
            build_error_envelope(resp, "-", "SELECT", "NO_RESULT_BODY",
                                  "execute_query_batch succeeded but produced no XML body",
                                  0);
        }
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT]: %s (rc=%d)", filename, rc);
        char errtext[256];
        snprintf(errtext, sizeof(errtext),
                 "execute_query_batch failed (rc=%d) - see select_Data_Manager.log", rc);
        build_error_envelope(resp, "-", "SELECT", "EXECUTE_FAILED", errtext, 0);
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
                                input_c_operation_t *op,
                                response_object_t   *resp)
{
    select_request_t *req = (select_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT/new]: %s - no select_request_t payload",
                     filename);
        build_error_envelope(resp, request->external_audit_id, "SELECT",
                              "NO_PAYLOAD", "No select_request_t payload after Level 1/2",
                              is_json);
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
    cfg.ReturnFormat = is_json ? "JSON" : "XML";

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

        char *body = is_json ? cfg.OUTPUT_JSON
                              : (cfg.xml ? cfg.xml->OUTPUT_XML : NULL);
        if (body)
        {
            resp->status  = RESPONSE_STATUS_PASS;
            resp->is_json = is_json;
            strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
            strncpy(resp->operation, "SELECT", sizeof(resp->operation) - 1);
            resp->response_body = body;
            if (is_json) cfg.OUTPUT_JSON = NULL;
            else         cfg.xml->OUTPUT_XML = NULL;
        }
        else
        {
            build_error_envelope(resp, request->external_audit_id, "SELECT",
                                  "NO_RESULT_BODY",
                                  "execute_query_batch succeeded but produced no result body",
                                  is_json);
        }
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
        char errtext[256];
        snprintf(errtext, sizeof(errtext),
                 "execute_query_batch failed (rc=%d) - see select_Data_Manager.log", rc);
        build_error_envelope(resp, request->external_audit_id, "SELECT",
                              "EXECUTE_FAILED", errtext, is_json);
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
                                input_c_operation_t *op,
                                response_object_t   *resp)
{
    insert_request_t *req = (insert_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [INSERT/new]: %s - no insert_request_t payload",
                     filename);
        build_error_envelope(resp, request->external_audit_id, "INSERT",
                              "NO_PAYLOAD", "No insert_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;

    /* Same reasoning as dispatch_select_new()'s own ReturnFormat note -
     * without this, JSON-format INSERT requests would silently only
     * ever get an XML response back.                                  */
    cfg.ReturnFormat = is_json ? "JSON" : "XML";

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

        char *body = is_json ? cfg.OUTPUT_JSON
                              : (cfg.xml ? cfg.xml->OUTPUT_XML : NULL);
        if (body)
        {
            resp->status  = RESPONSE_STATUS_PASS;
            resp->is_json = is_json;
            strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
            strncpy(resp->operation, "INSERT", sizeof(resp->operation) - 1);
            resp->response_body = body;
            if (is_json) cfg.OUTPUT_JSON = NULL;
            else         cfg.xml->OUTPUT_XML = NULL;
        }
        else
        {
            build_error_envelope(resp, request->external_audit_id, "INSERT",
                                  "NO_RESULT_BODY",
                                  "execute_insert_batch succeeded but produced no result body",
                                  is_json);
        }
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [INSERT/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
        char errtext[256];
        snprintf(errtext, sizeof(errtext),
                 "execute_insert_batch failed (rc=%d) - see insert_Data_Manager.log", rc);
        build_error_envelope(resp, request->external_audit_id, "INSERT",
                              "EXECUTE_FAILED", errtext, is_json);
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
                                input_c_operation_t *op,
                                response_object_t   *resp)
{
    update_request_t *req = (update_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [UPDATE/new]: %s - no update_request_t payload",
                     filename);
        build_error_envelope(resp, request->external_audit_id, "UPDATE",
                              "NO_PAYLOAD", "No update_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;

    /* Same reasoning as dispatch_select_new()'s own ReturnFormat note -
     * without this, JSON-format UPDATE requests would silently only
     * ever get an XML response back.                                  */
    cfg.ReturnFormat = is_json ? "JSON" : "XML";

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

        char *body = is_json ? cfg.OUTPUT_JSON
                              : (cfg.xml ? cfg.xml->OUTPUT_XML : NULL);
        if (body)
        {
            resp->status  = RESPONSE_STATUS_PASS;
            resp->is_json = is_json;
            strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
            strncpy(resp->operation, "UPDATE", sizeof(resp->operation) - 1);
            resp->response_body = body;
            if (is_json) cfg.OUTPUT_JSON = NULL;
            else         cfg.xml->OUTPUT_XML = NULL;
        }
        else
        {
            build_error_envelope(resp, request->external_audit_id, "UPDATE",
                                  "NO_RESULT_BODY",
                                  "execute_update_batch succeeded but produced no result body",
                                  is_json);
        }
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [UPDATE/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
        char errtext[256];
        snprintf(errtext, sizeof(errtext),
                 "execute_update_batch failed (rc=%d) - see update_Data_Manager.log", rc);
        build_error_envelope(resp, request->external_audit_id, "UPDATE",
                              "EXECUTE_FAILED", errtext, is_json);
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
                                input_c_operation_t *op,
                                response_object_t   *resp)
{
    delete_request_t *req = (delete_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [DELETE/new]: %s - no delete_request_t payload",
                     filename);
        build_error_envelope(resp, request->external_audit_id, "DELETE",
                              "NO_PAYLOAD", "No delete_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;

    /* Same reasoning as dispatch_select_new()'s own ReturnFormat note -
     * without this, JSON-format DELETE requests would silently only
     * ever get an XML response back.                                  */
    cfg.ReturnFormat = is_json ? "JSON" : "XML";

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

        char *body = is_json ? cfg.OUTPUT_JSON
                              : (cfg.xml ? cfg.xml->OUTPUT_XML : NULL);
        if (body)
        {
            resp->status  = RESPONSE_STATUS_PASS;
            resp->is_json = is_json;
            strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
            strncpy(resp->operation, "DELETE", sizeof(resp->operation) - 1);
            resp->response_body = body;
            if (is_json) cfg.OUTPUT_JSON = NULL;
            else         cfg.xml->OUTPUT_XML = NULL;
        }
        else
        {
            build_error_envelope(resp, request->external_audit_id, "DELETE",
                                  "NO_RESULT_BODY",
                                  "execute_delete_batch succeeded but produced no result body",
                                  is_json);
        }
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [DELETE/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
        char errtext[256];
        snprintf(errtext, sizeof(errtext),
                 "execute_delete_batch failed (rc=%d) - see delete_Data_Manager.log", rc);
        build_error_envelope(resp, request->external_audit_id, "DELETE",
                              "EXECUTE_FAILED", errtext, is_json);
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
                                   input_c_operation_t *op,
                                   response_object_t   *resp)
{
    execute_procedure_request_t *req = (execute_procedure_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [EXECUTE_PROCEDURE/new]: %s - no "
                     "execute_procedure_request_t payload",
                     filename);
        build_error_envelope(resp, request->external_audit_id, "EXECUTE_PROCEDURE",
                              "NO_PAYLOAD", "No execute_procedure_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;

    /* Same reasoning as dispatch_select_new()'s own ReturnFormat note -
     * without this, JSON-format EXECUTE_PROCEDURE requests would
     * silently only ever get an XML response back.                     */
    cfg.ReturnFormat = is_json ? "JSON" : "XML";

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

        char *body = is_json ? cfg.OUTPUT_JSON
                              : (cfg.xml ? cfg.xml->OUTPUT_XML : NULL);
        if (body)
        {
            resp->status  = RESPONSE_STATUS_PASS;
            resp->is_json = is_json;
            strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
            strncpy(resp->operation, "EXECUTE_PROCEDURE", sizeof(resp->operation) - 1);
            resp->response_body = body;
            if (is_json) cfg.OUTPUT_JSON = NULL;
            else         cfg.xml->OUTPUT_XML = NULL;
        }
        else
        {
            build_error_envelope(resp, request->external_audit_id, "EXECUTE_PROCEDURE",
                                  "NO_RESULT_BODY",
                                  "execute_procedure succeeded but produced no result body",
                                  is_json);
        }
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [EXECUTE_PROCEDURE/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
        char errtext[256];
        snprintf(errtext, sizeof(errtext),
                 "execute_procedure failed (rc=%d) - see procedure_Data_Manager.log", rc);
        build_error_envelope(resp, request->external_audit_id, "EXECUTE_PROCEDURE",
                              "EXECUTE_FAILED", errtext, is_json);
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
int process_xml_file(oci_context_t      *ctx,
                      const char         *payload,
                      long                payload_length,
                      const char         *filename,
                      const char         *session_id_override,
                      response_object_t  *resp)
{
    /* xml/len aliases keep the rest of this function's body (which
     * pervasively used these names when it did its own read_file()
     * call) unchanged below - payload is caller-owned now, not
     * allocated here, so there's deliberately no free(xml) anywhere
     * in this function anymore (Stage 4 - see dispatcher.h's own doc
     * comment on the Payload Ownership addendum).                     */
    const char *xml = payload;
    long        len = payload_length;

    if (!xml || len <= 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "process_xml_file() called with empty/NULL payload "
                     "for '%s' - this violates the caller's contract",
                     filename ? filename : "-");
        build_error_envelope(resp, "-", "-", "EMPTY_PAYLOAD",
                              "process_xml_file() received an empty payload", 0);
        return -1;
    }

    {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "Processing '%s'.  Updating ctx->INPUT_XML", filename);
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
            /* Genuinely don't know XML vs JSON here - level1_parse()
             * failed before format-specific parsing got anywhere, so
             * default to XML (same reasoning as the read-failure case
             * above). */
            build_error_envelope(resp, "-", "-", level1_error.error_code,
                                  level1_error.error_text, 0);
            return -1;
        }

        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "File='%s' matched new request format - audit_id=%s "
                     "operations=%d", filename, new_request.external_audit_id,
                     new_request.operation_count);

        /* Session Manager proposal, Stage 1 (2026-08-06): stamp the
         * caller's real session_id onto the parsed request, overriding
         * whatever the raw payload itself carried (almost always "-").
         * Must happen before logger_set_sid() below, so tracing picks
         * up the real value too - and before Level 2/dispatch, since
         * this field is what a future Stage 3 validation check will
         * actually validate. NULL/empty override (e.g. the legacy
         * Test_XML_Runner harness, which doesn't participate in this
         * yet) leaves the parsed payload's own session_id untouched. */
        if (session_id_override && session_id_override[0])
        {
            strncpy(new_request.session_id, session_id_override,
                    sizeof(new_request.session_id) - 1);
            new_request.session_id[sizeof(new_request.session_id) - 1] = '\0';
        }

        /* Trace context (2026-08-06): set for the calling (worker)
         * thread now that the request's session_id is known - every
         * subsequent logger_write() call on this thread, across every
         * module it touches for the rest of this request, picks it up
         * automatically. Cleared in worker.c once this request is
         * fully done, so it never leaks into the next request handled
         * by the same thread. */
        logger_set_sid(new_request.session_id);

        int is_json = (new_request.source_format == INPUT_FORMAT_JSON);

        /* Session Manager proposal, Stage 3 (2026-08-08): hard session
         * validation - the actual "reject requests without a valid
         * session" behaviour the whole proposal was building toward.
         * Cache-only (session_validate() never touches the DB - same
         * "keep the critical path cheap" reasoning as every stage
         * before this). Gated by session_validation_enabled so UAT can
         * run with it off (ad-hoc/fixture traffic often has no real
         * session handshake) and production can flip it off too, as a
         * disaster-recovery lever, if session validation itself is
         * ever what's blocking otherwise-legitimate traffic. Session
         * creation/tracking (Stages 1-2) stay on regardless of this
         * switch - only the rejection behaviour is gated.
         *
         * Checked after the override above, so this validates the
         * FINAL session_id (whichever consumer stamped it, or the raw
         * payload's own value if nothing overrode it) - and before
         * Level 2, so an invalid session is rejected without spending
         * effort on field-level validation for a request that's being
         * rejected regardless.                                        */
        if (ctx->ini->session_validation_enabled)
        {
            int session_rc = session_validate(ctx, new_request.session_id, NULL);
            if (session_rc != SESSION_OK)
            {
                const char *session_error_code =
                    (session_rc == SESSION_ERR_NOT_FOUND) ? "SESSION_NOT_FOUND"
                                                           : "SESSION_INVALID";

                logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                             "File='%s' REJECTED - session_id='%s' failed "
                             "validation (rc=%d, %s)", filename,
                             new_request.session_id, session_rc,
                             session_error_code);

                build_error_envelope(resp, new_request.external_audit_id,
                                      "SESSION_VALIDATION", session_error_code,
                                      "The session_id on this request is "
                                      "missing, unknown, or expired - a "
                                      "valid session is required before "
                                      "this request can be processed.",
                                      is_json);
                level1_free_request(&new_request);
                return -1;
            }
        }

        uint64_t level2_start = metrics_now_us();
        int level2_rc = level2_validate(ctx, &new_request);
        ctx->level2_parse_us = metrics_now_us() - level2_start;
        int rc = 0;

        if (level2_rc == LEVEL2_OK)
        {
            /* Note: a request can carry multiple operations
             * (operation_count > 1), but ResponseObject is one-per-file.
             * Same pre-existing limitation as rc itself already had
             * before Stage 3 (rc was only ever the *last* operation's
             * result too) - resp ends up reflecting the last operation
             * dispatched, not a merge of all of them. Worth revisiting
             * if multi-operation files turn out to matter in practice;
             * not changed here since Stage 3's job is wiring the
             * Response Manager, not redesigning multi-op semantics.    */
            for (int i = 0; i < new_request.operation_count; i++)
            {
                input_c_operation_t *op = &new_request.operations[i];

                if (op->type == OP_SELECT)
                {
                    rc = dispatch_select_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_INSERT)
                {
                    rc = dispatch_insert_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_UPDATE)
                {
                    rc = dispatch_update_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_DELETE)
                {
                    rc = dispatch_delete_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_EXECUTE_PROCEDURE)
                {
                    rc = dispatch_procedure_new(ctx, filename, &new_request, op, resp);
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
                    build_error_envelope(resp, new_request.external_audit_id, "-",
                                         "UNSUPPORTED_OPERATION_TYPE",
                                         "This operation type isn't implemented by the "
                                         "new dispatch pipeline yet", is_json);
                    rc = -1;
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
            {
                logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                             "FAIL [Level2] %s operation[%d] error_code=%s error_text=%s",
                             filename, failed_op,
                             new_request.operations[failed_op].validation_status.error_code,
                             new_request.operations[failed_op].validation_status.error_text);
                build_error_envelope(resp, new_request.external_audit_id, "-",
                                     new_request.operations[failed_op].validation_status.error_code,
                                     new_request.operations[failed_op].validation_status.error_text,
                                     is_json);
            }
            else
            {
                build_error_envelope(resp, new_request.external_audit_id, "-",
                                     "LEVEL2_VALIDATION_FAILED",
                                     "Level 2 validation failed - see dispatcher log",
                                     is_json);
            }
            rc = -1;
        }

        level1_free_request(&new_request);
        return rc;
    }


    char operation[MAX_OPERATION_LEN] = {0};
    if (!extract_tag(xml, "operation", operation, sizeof(operation)))
    {
        logger_write(ctx->dispatcher_logger, LOG_WARN, __func__, 0,
                     "No <operation> found in %s - skipping", filename);
        /* rc==0 here (not a failure) - resp needs to agree, so this is
         * a minimal PASS envelope rather than an error one, honestly
         * reflecting "nothing to dispatch" rather than fabricating a
         * failure that didn't happen. */
        resp->status = RESPONSE_STATUS_PASS;
        resp->is_json = 0;
        strncpy(resp->operation, "-", sizeof(resp->operation) - 1);
        resp->response_body = strdup(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<output_xml><execution_envelope><status>SKIPPED</status>"
            "<note>No &lt;operation&gt; element found in input</note>"
            "</execution_envelope></output_xml>\n");
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
        build_error_envelope(resp, "-", "INSERT", "LEGACY_FORMAT_UNSUPPORTED",
                              "Old flat-XML INSERT format is no longer supported - "
                              "convert to the new <request version=\"1.0\"> format", 0);
        rc = -1;
    }
    else if (strcmp(operation, "SELECT") == 0)
        rc = dispatch_select(ctx, filename, xml, resp);
    else if (strcmp(operation, "UPDATE") == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "File='%s' is old flat-XML UPDATE format - no longer "
                     "supported (execute_update_batch() now requires "
                     "update_request_t via the new pipeline). Convert this "
                     "fixture to the new <request version=\"1.0\">...<operation "
                     "type=\"UPDATE\"> format.", filename);
        build_error_envelope(resp, "-", "UPDATE", "LEGACY_FORMAT_UNSUPPORTED",
                              "Old flat-XML UPDATE format is no longer supported - "
                              "convert to the new <request version=\"1.0\"> format", 0);
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
        build_error_envelope(resp, "-", "DELETE", "LEGACY_FORMAT_UNSUPPORTED",
                              "Old flat-XML DELETE format is no longer supported - "
                              "convert to the new <request version=\"1.0\"> format", 0);
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
        build_error_envelope(resp, "-", "EXECUTE_PROCEDURE", "LEGACY_FORMAT_UNSUPPORTED",
                              "Old flat-XML EXECUTE_PROCEDURE format is no longer "
                              "supported - convert to the new <request version=\"1.0\"> "
                              "format", 0);
        rc = -1;
    }
    else
    {
        logger_write(ctx->dispatcher_logger, LOG_WARN, __func__, 0,
                     "Unknown operation '%s' in %s - skipping",
                     operation, filename);
        /* rc stays 0 (not a failure) - same reasoning as the missing
         * <operation> case above: PASS envelope, not a fabricated
         * error. */
        resp->status = RESPONSE_STATUS_PASS;
        resp->is_json = 0;
        strncpy(resp->operation, operation, sizeof(resp->operation) - 1);
        char *esc_op = xml_escape_alloc(operation);
        size_t bufsize = strlen(esc_op) + 200;
        resp->response_body = malloc(bufsize);
        if (resp->response_body)
            snprintf(resp->response_body, bufsize,
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<output_xml><execution_envelope><status>SKIPPED</status>"
                     "<note>Unknown operation '%s'</note>"
                     "</execution_envelope></output_xml>\n", esc_op);
        else
            resp->response_body = strdup("<output_xml><execution_envelope>"
                                          "<status>SKIPPED</status></execution_envelope>"
                                          "</output_xml>");
        free(esc_op);
    }

    return rc;
}
