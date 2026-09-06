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
 *   dispatch_authenticate_new()      - new-pipeline AUTHENTICATE handler
 *   dispatch_check_permission_new()  - new-pipeline CHECK_PERMISSION handler
 *   dispatch_create_user_new()       - new-pipeline CREATE_USER handler
 *                                      (tgen only - no execute module yet,
 *                                      Independent DDL Module proposal
 *                                      03-Sep)
 *   dispatch_grant_new()             - new-pipeline GRANT handler
 *                                      (tgen only, same staged scope as
 *                                      CREATE_USER)
 *   dispatch_create_table_new()      - new-pipeline CREATE_TABLE handler
 *                                      (tgen only, same staged scope as
 *                                      CREATE_USER/GRANT - built for
 *                                      DROP testing, 05-Sep)
 *   dispatch_drop_table_new()        - new-pipeline DROP_TABLE handler
 *                                      (tgen only - preview text, does
 *                                      NOT execute; especially
 *                                      deliberate given DROP is
 *                                      destructive)
 *   dispatch_create_view_new()       - new-pipeline CREATE_VIEW handler
 *                                      (tgen only, same staged scope as
 *                                      the other DDL operations)
 *   dispatch_create_procedure_new()  - new-pipeline CREATE_PROCEDURE
 *                                      handler (tgen only - final
 *                                      operation of the Independent DDL
 *                                      Module proposal, 03-Sep)
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
#include "OCI_Auth_Manager.h"             /* auth_authenticate() - new,
                                            * Security Module Stage 2 */
#include "OCI_DDL_Create_User_Module.h"   /* create_user_request_t,
                                              get_create_user_template(),
                                              build_create_user_ddl_text() -
                                              new, Independent DDL Module
                                              proposal (03-Sep) */
#include "OCI_DDL_Grant_Module.h"         /* grant_request_t,
                                              get_grant_template(),
                                              build_grant_ddl_text() -
                                              new, Independent DDL Module
                                              proposal (03-Sep), second
                                              operation */
#include "OCI_DDL_Create_Table_Module.h"  /* create_table_request_t,
                                              get_create_table_template(),
                                              build_create_table_ddl_text() -
                                              new, Independent DDL Module
                                              proposal (03-Sep), third
                                              operation */
#include "OCI_DDL_Drop_Table_Module.h"    /* drop_table_request_t,
                                              get_drop_table_template(),
                                              build_drop_table_ddl_text() -
                                              new, Independent DDL Module
                                              proposal (03-Sep), fourth
                                              operation */
#include "OCI_DDL_Create_View_Module.h"   /* create_view_request_t,
                                              get_create_view_template(),
                                              build_create_view_ddl_text() -
                                              new, Independent DDL Module
                                              proposal (03-Sep), fifth
                                              operation */
#include "OCI_DDL_Create_Procedure_Module.h" /* create_procedure_request_t,
                                              get_create_procedure_template(),
                                              build_create_procedure_ddl_text() -
                                              new, Independent DDL Module
                                              proposal (03-Sep), sixth
                                              operation */
#include "OCI_Authz_Manager.h"            /* authz_has_permission() - new,
                                            * Security Module Stage 5 */
#include "logger.h"
#include "OCI_Session_Manager.h"   /* session_validate() - Session Manager
                                      proposal, Stage 3 (2026-08-08) */
#include "ini_reader.h"
#include "OCI_Level1_Parser.h"
#include "OCI_Level2_Parser.h"
#include "OCI_Request_Response_Types.h"
#include "OCI_Execute_Query_Batch_Module.h"
#include "async_callback_client.h"   /* Stage 5 - execute_async delivery */
#include "metrics.h"
#include "OCI_Transaction_Manager.h"   /* tx_begin/tx_commit/tx_rollback -
                                          per-request transaction scoping,
                                          File Consumer closure item
                                          2026-08-12 */

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
/*  Stage 5 - execute_async batch delivery                              */
/*  Bridges execute_query_batch()'s generic per-batch callback          */
/*  (execute_config_t.async_batch_callback - deliberately just a plain  */
/*  function pointer + void*, so OCI_Execute_Query_Batch_Module.c stays */
/*  consumer-agnostic and doesn't need to know async_callback_client.h  */
/*  exists at all) to the actual outbound POST.                         */
/* ================================================================== */

typedef struct {
    oci_context_t *ctx;
    const char     *url;
} async_dispatch_ctx_t;

static int async_batch_dispatch_callback(void *user_data, const char *batch_body,
                                          int is_json, int is_final, int batch_number)
{
    async_dispatch_ctx_t *adc = (async_dispatch_ctx_t *)user_data;

    logger_write(adc->ctx->select_logger, LOG_INFO, __func__, 0,
                 "execute_async: delivering batch %d (%s) to '%s', %zu bytes",
                 batch_number, is_final ? "final" : "not final",
                 adc->url, strlen(batch_body));

    /* Best-effort - async_callback_post() itself already logs the
     * specific failure reason (timeout, connection refused, non-2xx)
     * at WARN. Nothing further to do here on failure - see this
     * module's own design note in execute_config_t (OCI_Connection.h)
     * on why retry is deliberately the caller's own responsibility,
     * not ours.                                                        */
    async_callback_post(adc->ctx, adc->url, batch_body, is_json);

    return 0;
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

    /* Stage 5 (2026-08-22) - execute_async. Level 2 has already
     * validated async_call_back_url (non-empty, https://, sole
     * operation in its transaction) before this function is ever
     * reached - see OCI_Level2_Parser.c's own validation for the full
     * contract. async_dispatch_ctx just needs to outlive the
     * execute_query_batch() call below, which is synchronous/blocking -
     * a stack local is safe here, the callback only ever fires DURING
     * that call, on this same thread.                                  */
    async_dispatch_ctx_t async_dispatch_ctx;
    if (req->execute_async)
    {
        async_dispatch_ctx.ctx = ctx;
        async_dispatch_ctx.url = req->async_call_back_url;
        cfg.async_batch_callback  = async_batch_dispatch_callback;
        cfg.async_batch_user_data = &async_dispatch_ctx;

        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "SELECT/new audit_id=%s: execute_async=1, batches "
                     "will be delivered to '%s' instead of one combined "
                     "response", request->external_audit_id,
                     req->async_call_back_url);
    }

    int rc = execute_query_batch(ctx, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                     "PASS [SELECT/new] audit_id=%s: %s",
                     request->external_audit_id, filename);

        if (req->execute_async)
        {
            /* Batches already went to async_call_back_url during
             * execute_query_batch() above - cfg.xml->OUTPUT_XML/
             * cfg.OUTPUT_JSON are deliberately left unpopulated for an
             * async request (see OCI_Execute_Query_Batch_Module.c's own
             * design note on why), so build a small, honest
             * acknowledgement instead of returning a misleadingly-empty
             * resultset. http_worker_pool.c returns HTTP 202 before this
             * is even built - see http_consumer.h's own note - but
             * process_xml_file()'s own contract (a real, complete
             * response_body on every call) still holds either way.      */
            char ack[256];
            snprintf(ack, sizeof(ack),
                     is_json
                       ? "{\"status\":\"ACCEPTED\",\"message\":\"Batches delivered to callback URL.\"}"
                       : "<output_xml><status>ACCEPTED</status>"
                         "<message>Batches delivered to callback URL.</message></output_xml>");
            resp->status  = RESPONSE_STATUS_PASS;
            resp->is_json = is_json;
            strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
            strncpy(resp->operation, "SELECT", sizeof(resp->operation) - 1);
            resp->response_body = strdup(ack);
        }
        else
        {
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
/*  dispatch_authenticate_new                                           */
/*  AUTHENTICATE via the same format-agnostic pipeline every other      */
/*  operation uses - Level 1/2 already parsed and validated             */
/*  authenticate_request_t; this is the thin adapter that calls         */
/*  auth_authenticate() (OCI_Auth_Manager.c) and renders the result,    */
/*  matching the shape of dispatch_select_new() etc. exactly. Security  */
/*  Module Stage 2 (2026-08-27) - LOCAL auth source only.               */
/* ================================================================== */
static int dispatch_authenticate_new(oci_context_t       *ctx,
                                      const char          *filename,
                                      input_c_request_t   *request,
                                      input_c_operation_t *op,
                                      response_object_t   *resp)
{
    authenticate_request_t *req = (authenticate_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [AUTHENTICATE/new]: %s - no authenticate_request_t "
                     "payload", filename);
        build_error_envelope(resp, request->external_audit_id, "AUTHENTICATE",
                              "NO_PAYLOAD", "No authenticate_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    char *session_id   = NULL;
    char *display_name = NULL;
    int   ttl_seconds  = 0;

    int rc = auth_authenticate(ctx, req, &session_id, &display_name, &ttl_seconds);

    if (rc == AUTH_OK)
    {
        logger_write(ctx->security_logger, LOG_INFO, __func__, 0,
                     "PASS [AUTHENTICATE/new] audit_id=%s: username=%s",
                     request->external_audit_id, req->username);

        char *body = malloc(1024);
        if (body)
        {
            if (is_json)
            {
                snprintf(body, 1024,
                    "{\"operation\":\"AUTHENTICATE\",\"status\":\"SUCCESS\","
                    "\"session_id\":\"%s\",\"display_name\":\"%s\","
                    "\"ttl_seconds\":%d}",
                    session_id, display_name, ttl_seconds);
            }
            else
            {
                snprintf(body, 1024,
                    "<auth>\n  <operation>AUTHENTICATE</operation>\n"
                    "  <status>SUCCESS</status>\n"
                    "  <session_id>%s</session_id>\n"
                    "  <display_name>%s</display_name>\n"
                    "  <ttl_seconds>%d</ttl_seconds>\n</auth>\n",
                    session_id, display_name, ttl_seconds);
            }
        }

        resp->status        = RESPONSE_STATUS_PASS;
        resp->is_json       = is_json;
        resp->response_body = body;   /* ownership transferred, same as every
                                        * other dispatch_*_new() in this file */
        rc = 0;
    }
    else
    {
        /* AUTH_ERR_DENIED covers unknown user / disabled / locked / bad
         * credential / (Stage 2 only) non-LOCAL source alike,
         * deliberately - see auth_authenticate()'s own doc comment and
         * Security_Module_Design_Specification.docx Section 5. Not
         * distinguished to the caller here either - the specific
         * reason is already in the security_logger line
         * auth_authenticate() itself wrote.                           */
        logger_write(ctx->dispatcher_logger, LOG_WARN, __func__, 0,
                     "FAIL [AUTHENTICATE/new] audit_id=%s: username=%s rc=%d",
                     request->external_audit_id, req->username, rc);
        build_error_envelope(resp, request->external_audit_id, "AUTHENTICATE",
                              "DENIED", "Authentication failed", is_json);
        rc = -1;
    }

    free(session_id);
    free(display_name);
    return rc;
}

/* ================================================================== */
/*  dispatch_check_permission_new                                        */
/*  CHECK_PERMISSION via the same format-agnostic pipeline every other  */
/*  operation uses - Level 1/2 already parsed and validated             */
/*  check_permission_request_t; this is the thin adapter that calls     */
/*  authz_has_permission() (OCI_Authz_Manager.c) and renders the        */
/*  result, matching dispatch_authenticate_new()'s own shape exactly.   */
/*  Security Module Stage 5 (2026-08-31).                               */
/*                                                                       */
/*  session_id comes from request->session_id (the envelope's own       */
/*  field, already parsed by Level 1 for every operation type) - not    */
/*  from op->payload, matching OCI_Authz_Manager.h's own doc comment    */
/*  on why check_permission_request_t doesn't duplicate it.             */
/* ================================================================== */
static int dispatch_check_permission_new(oci_context_t       *ctx,
                                          const char          *filename,
                                          input_c_request_t   *request,
                                          input_c_operation_t *op,
                                          response_object_t   *resp)
{
    check_permission_request_t *req = (check_permission_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CHECK_PERMISSION/new]: %s - no check_permission_"
                     "request_t payload", filename);
        build_error_envelope(resp, request->external_audit_id, "CHECK_PERMISSION",
                              "NO_PAYLOAD", "No check_permission_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    int rc = authz_has_permission(ctx, request->session_id, req->permission_code);

    char *body = malloc(512);
    if (body)
    {
        const char *result_str = (rc == AUTHZ_OK) ? "ALLOWED" : "DENIED";
        if (is_json)
        {
            snprintf(body, 512,
                "{\"operation\":\"CHECK_PERMISSION\",\"permission_code\":\"%s\","
                "\"result\":\"%s\"}",
                req->permission_code, result_str);
        }
        else
        {
            snprintf(body, 512,
                "<authz>\n  <operation>CHECK_PERMISSION</operation>\n"
                "  <permission_code>%s</permission_code>\n"
                "  <result>%s</result>\n</authz>\n",
                req->permission_code, result_str);
        }
    }

    /* Unlike AUTHENTICATE's DENIED (a real error - no session was
     * created), a CHECK_PERMISSION result of DENIED is still a
     * successfully-answered request - the caller asked a yes/no
     * question and got a real answer. resp->status is PASS either
     * way; ALLOWED/DENIED is carried in the body, matching Security_
     * Module_Design_Specification.docx Section 8.2's own response
     * shape (there is no separate error envelope for a plain denial
     * here, unlike AUTHENTICATE's).                                   */
    resp->status        = RESPONSE_STATUS_PASS;
    resp->is_json       = is_json;
    resp->response_body = body;

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "PASS [CHECK_PERMISSION/new] audit_id=%s: session_id=%s "
                 "permission_code=%s result=%s",
                 request->external_audit_id, request->session_id,
                 req->permission_code, (rc == AUTHZ_OK) ? "ALLOWED" : "DENIED");

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Static helper: minimal JSON string escaping (quote/backslash/       */
/*  newline) for embedding build_create_user_ddl_text()'s multi-line    */
/*  DDL text into a hand-built JSON body, same "just enough" approach   */
/*  as xml_escape() takes for XML elsewhere in this project.            */
/* ------------------------------------------------------------------ */
static char *json_escape_ddl(const char *src)
{
    if (!src) return strdup("");

    size_t len = strlen(src);
    char  *out = malloc(len * 2 + 1);   /* worst case: every char escaped */
    if (!out) return NULL;

    size_t o = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)src[i];
        switch (c)
        {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': break;   /* drop - \n alone is enough */
            default:   out[o++] = (char)c; break;
        }
    }
    out[o] = '\0';
    return out;
}

/* ================================================================== */
/*  dispatch_create_user_new                                            */
/*  CREATE_USER via the same format-agnostic pipeline every other       */
/*  operation uses. Independent DDL Module proposal (03-Sep), first     */
/*  operation - Level 1/2 already parsed and validated                  */
/*  create_user_request_t; this is the tgen adapter that calls          */
/*  get_create_user_template() (OCI_DDL_Create_User_Module.c) and       */
/*  renders the result.                                                  */
/*                                                                       */
/*  IMPORTANT - this does NOT execute against the database. There is    */
/*  no execute module for CREATE_USER yet (deliberately staged that     */
/*  way - see OCI_DDL_Create_User_Module.h's own header comment). The   */
/*  response is the generated/validated DDL text for review, exactly    */
/*  like GET_TEMPLATE's response is a template for the client to act    */
/*  on, not a side effect that already happened. resp->status is PASS   */
/*  once the DDL text has been generated and validation succeeded       */
/*  (Level 2 already ran level2_validate_create_user() before this      */
/*  function is ever reached) - PASS here means "here is your DDL",     */
/*  not "the user now exists in the database".                          */
/* ================================================================== */
static int dispatch_create_user_new(oci_context_t       *ctx,
                                     const char          *filename,
                                     input_c_request_t   *request,
                                     input_c_operation_t *op,
                                     response_object_t   *resp)
{
    create_user_request_t *req = (create_user_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CREATE_USER/new]: %s - no create_user_request_t "
                     "payload", filename);
        build_error_envelope(resp, request->external_audit_id, "CREATE_USER",
                              "NO_PAYLOAD", "No create_user_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    char *body = NULL;

    if (is_json)
    {
        char ddl_text[2048] = {0};
        build_create_user_ddl_text(req, ddl_text, sizeof(ddl_text));
        char *e_ddl = json_escape_ddl(ddl_text);

        body = malloc(4096);
        if (body && e_ddl)
        {
            snprintf(body, 4096,
                "{\"operation\":\"CREATE_USER\",\"status\":\"TEMPLATE_GENERATED\","
                "\"username\":\"%s\",\"role_count\":%d,\"generated_ddl\":\"%s\"}",
                req->username, req->role_count, e_ddl);
        }
        free(e_ddl);
    }
    else
    {
        xml_builder_t *xml = get_create_user_template(ctx, req);
        if (xml && xml->buffer)
            body = strdup(xml->buffer);
        if (xml) xml_free(xml);
    }

    if (!body)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CREATE_USER/new] audit_id=%s: %s - "
                     "get_create_user_template produced no body",
                     request->external_audit_id, filename);
        build_error_envelope(resp, request->external_audit_id, "CREATE_USER",
                              "NO_RESULT_BODY",
                              "get_create_user_template succeeded but produced no result body",
                              is_json);
        return -1;
    }

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "PASS [CREATE_USER/new] audit_id=%s: username=%s role_count=%d "
                 "(template generated, not yet executed - no execute module "
                 "for CREATE_USER exists)",
                 request->external_audit_id, req->username, req->role_count);

    resp->status        = RESPONSE_STATUS_PASS;
    resp->is_json       = is_json;
    strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
    strncpy(resp->operation, "CREATE_USER", sizeof(resp->operation) - 1);
    resp->response_body = body;

    return 0;
}

/* ================================================================== */
/*  dispatch_grant_new                                                   */
/*  GRANT via the same format-agnostic pipeline every other operation    */
/*  uses. Independent DDL Module proposal (03-Sep), second operation -   */
/*  same tgen-only shape as dispatch_create_user_new(): calls            */
/*  get_grant_template() (OCI_DDL_Grant_Module.c) and renders the        */
/*  result. Does NOT execute against the database - no execute module    */
/*  for GRANT exists yet, same staged boundary as CREATE_USER.           */
/* ================================================================== */
static int dispatch_grant_new(oci_context_t       *ctx,
                               const char          *filename,
                               input_c_request_t   *request,
                               input_c_operation_t *op,
                               response_object_t   *resp)
{
    grant_request_t *req = (grant_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [GRANT/new]: %s - no grant_request_t payload",
                     filename);
        build_error_envelope(resp, request->external_audit_id, "GRANT",
                              "NO_PAYLOAD", "No grant_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    char *body = NULL;

    if (is_json)
    {
        char ddl_text[2048] = {0};
        build_grant_ddl_text(req, ddl_text, sizeof(ddl_text));
        char *e_ddl = json_escape_ddl(ddl_text);

        body = malloc(4096);
        if (body && e_ddl)
        {
            snprintf(body, 4096,
                "{\"operation\":\"GRANT\",\"status\":\"TEMPLATE_GENERATED\","
                "\"grantee\":\"%s\",\"object_name\":\"%s\","
                "\"privilege_count\":%d,\"generated_ddl\":\"%s\"}",
                req->grantee, req->object_name, req->privilege_count, e_ddl);
        }
        free(e_ddl);
    }
    else
    {
        xml_builder_t *xml = get_grant_template(ctx, req);
        if (xml && xml->buffer)
            body = strdup(xml->buffer);
        if (xml) xml_free(xml);
    }

    if (!body)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [GRANT/new] audit_id=%s: %s - "
                     "get_grant_template produced no body",
                     request->external_audit_id, filename);
        build_error_envelope(resp, request->external_audit_id, "GRANT",
                              "NO_RESULT_BODY",
                              "get_grant_template succeeded but produced no result body",
                              is_json);
        return -1;
    }

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "PASS [GRANT/new] audit_id=%s: grantee=%s object=%s.%s "
                 "privilege_count=%d (template generated, not yet executed - "
                 "no execute module for GRANT exists)",
                 request->external_audit_id, req->grantee, req->owner,
                 req->object_name, req->privilege_count);

    resp->status        = RESPONSE_STATUS_PASS;
    resp->is_json       = is_json;
    strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
    strncpy(resp->operation, "GRANT", sizeof(resp->operation) - 1);
    resp->response_body = body;

    return 0;
}

/* ================================================================== */
/*  dispatch_create_table_new                                            */
/*  CREATE_TABLE via the same format-agnostic pipeline every other       */
/*  operation uses. Independent DDL Module proposal (03-Sep), third      */
/*  operation - built specifically so DROP has something real to        */
/*  target in testing (Terry, 05-Sep). Same tgen-only shape as           */
/*  dispatch_create_user_new()/dispatch_grant_new(): calls               */
/*  get_create_table_template() (OCI_DDL_Create_Table_Module.c) and      */
/*  renders the result. Does NOT execute against the database - no      */
/*  execute module for CREATE_TABLE exists yet, same staged boundary    */
/*  as CREATE_USER/GRANT.                                                 */
/* ================================================================== */
static int dispatch_create_table_new(oci_context_t       *ctx,
                                      const char          *filename,
                                      input_c_request_t   *request,
                                      input_c_operation_t *op,
                                      response_object_t   *resp)
{
    create_table_request_t *req = (create_table_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CREATE_TABLE/new]: %s - no create_table_request_t "
                     "payload", filename);
        build_error_envelope(resp, request->external_audit_id, "CREATE_TABLE",
                              "NO_PAYLOAD", "No create_table_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    char *body = NULL;

    if (is_json)
    {
        char ddl_text[8192] = {0};
        build_create_table_ddl_text(req, ddl_text, sizeof(ddl_text));
        char *e_ddl = json_escape_ddl(ddl_text);

        body = malloc(16384);
        if (body && e_ddl)
        {
            snprintf(body, 16384,
                "{\"operation\":\"CREATE_TABLE\",\"status\":\"TEMPLATE_GENERATED\","
                "\"table_name\":\"%s\",\"column_count\":%d,\"generated_ddl\":\"%s\"}",
                req->table_name, req->column_count, e_ddl);
        }
        free(e_ddl);
    }
    else
    {
        xml_builder_t *xml = get_create_table_template(ctx, req);
        if (xml && xml->buffer)
            body = strdup(xml->buffer);
        if (xml) xml_free(xml);
    }

    if (!body)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CREATE_TABLE/new] audit_id=%s: %s - "
                     "get_create_table_template produced no body",
                     request->external_audit_id, filename);
        build_error_envelope(resp, request->external_audit_id, "CREATE_TABLE",
                              "NO_RESULT_BODY",
                              "get_create_table_template succeeded but produced no result body",
                              is_json);
        return -1;
    }

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "PASS [CREATE_TABLE/new] audit_id=%s: table_name=%s "
                 "column_count=%d (template generated, not yet executed - "
                 "no execute module for CREATE_TABLE exists)",
                 request->external_audit_id, req->table_name, req->column_count);

    resp->status        = RESPONSE_STATUS_PASS;
    resp->is_json       = is_json;
    strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
    strncpy(resp->operation, "CREATE_TABLE", sizeof(resp->operation) - 1);
    resp->response_body = body;

    return 0;
}

/* ================================================================== */
/*  dispatch_drop_table_new                                              */
/*  DROP_TABLE via the same format-agnostic pipeline every other        */
/*  operation uses. Independent DDL Module proposal (03-Sep), fourth    */
/*  operation. Same tgen-only shape as the other three DDL dispatch     */
/*  functions: calls get_drop_table_template()                          */
/*  (OCI_DDL_Drop_Table_Module.c) and renders the result. Does NOT      */
/*  execute against the database - no execute module for DROP_TABLE     */
/*  exists yet. Worth repeating here specifically: DROP is destructive  */
/*  and irreversible once executed, so this preview-only boundary is    */
/*  especially deliberate for this operation - resp->status = PASS      */
/*  means "here is your DROP statement for review," not "the table is  */
/*  gone."                                                                */
/* ================================================================== */
static int dispatch_drop_table_new(oci_context_t       *ctx,
                                    const char          *filename,
                                    input_c_request_t   *request,
                                    input_c_operation_t *op,
                                    response_object_t   *resp)
{
    drop_table_request_t *req = (drop_table_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [DROP_TABLE/new]: %s - no drop_table_request_t "
                     "payload", filename);
        build_error_envelope(resp, request->external_audit_id, "DROP_TABLE",
                              "NO_PAYLOAD", "No drop_table_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    char *body = NULL;

    if (is_json)
    {
        char ddl_text[512] = {0};
        build_drop_table_ddl_text(req, ddl_text, sizeof(ddl_text));
        char *e_ddl = json_escape_ddl(ddl_text);

        body = malloc(1024);
        if (body && e_ddl)
        {
            snprintf(body, 1024,
                "{\"operation\":\"DROP_TABLE\",\"status\":\"TEMPLATE_GENERATED\","
                "\"table_name\":\"%s\",\"generated_ddl\":\"%s\"}",
                req->table_name, e_ddl);
        }
        free(e_ddl);
    }
    else
    {
        xml_builder_t *xml = get_drop_table_template(ctx, req);
        if (xml && xml->buffer)
            body = strdup(xml->buffer);
        if (xml) xml_free(xml);
    }

    if (!body)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [DROP_TABLE/new] audit_id=%s: %s - "
                     "get_drop_table_template produced no body",
                     request->external_audit_id, filename);
        build_error_envelope(resp, request->external_audit_id, "DROP_TABLE",
                              "NO_RESULT_BODY",
                              "get_drop_table_template succeeded but produced no result body",
                              is_json);
        return -1;
    }

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "PASS [DROP_TABLE/new] audit_id=%s: table_name=%s owner=%s "
                 "(template generated, not yet executed - no execute module "
                 "for DROP_TABLE exists)",
                 request->external_audit_id, req->table_name, req->owner);

    resp->status        = RESPONSE_STATUS_PASS;
    resp->is_json       = is_json;
    strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
    strncpy(resp->operation, "DROP_TABLE", sizeof(resp->operation) - 1);
    resp->response_body = body;

    return 0;
}

/* ================================================================== */
/*  dispatch_create_view_new                                             */
/*  CREATE_VIEW via the same format-agnostic pipeline every other       */
/*  operation uses. Independent DDL Module proposal (03-Sep), fifth     */
/*  operation. Same tgen-only shape as the other DDL dispatch           */
/*  functions - calls get_create_view_template()                       */
/*  (OCI_DDL_Create_View_Module.c) and renders the result. Does NOT     */
/*  execute against the database - no execute module for CREATE_VIEW    */
/*  exists yet.                                                          */
/* ================================================================== */
static int dispatch_create_view_new(oci_context_t       *ctx,
                                     const char          *filename,
                                     input_c_request_t   *request,
                                     input_c_operation_t *op,
                                     response_object_t   *resp)
{
    create_view_request_t *req = (create_view_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CREATE_VIEW/new]: %s - no create_view_request_t "
                     "payload", filename);
        build_error_envelope(resp, request->external_audit_id, "CREATE_VIEW",
                              "NO_PAYLOAD", "No create_view_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    char *body = NULL;

    if (is_json)
    {
        char ddl_text[8192] = {0};
        build_create_view_ddl_text(req, ddl_text, sizeof(ddl_text));
        char *e_ddl = json_escape_ddl(ddl_text);

        body = malloc(16384);
        if (body && e_ddl)
        {
            snprintf(body, 16384,
                "{\"operation\":\"CREATE_VIEW\",\"status\":\"TEMPLATE_GENERATED\","
                "\"view_name\":\"%s\",\"generated_ddl\":\"%s\"}",
                req->view_name, e_ddl);
        }
        free(e_ddl);
    }
    else
    {
        xml_builder_t *xml = get_create_view_template(ctx, req);
        if (xml && xml->buffer)
            body = strdup(xml->buffer);
        if (xml) xml_free(xml);
    }

    if (!body)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CREATE_VIEW/new] audit_id=%s: %s - "
                     "get_create_view_template produced no body",
                     request->external_audit_id, filename);
        build_error_envelope(resp, request->external_audit_id, "CREATE_VIEW",
                              "NO_RESULT_BODY",
                              "get_create_view_template succeeded but produced no result body",
                              is_json);
        return -1;
    }

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "PASS [CREATE_VIEW/new] audit_id=%s: view_name=%s "
                 "(template generated, not yet executed - no execute module "
                 "for CREATE_VIEW exists)",
                 request->external_audit_id, req->view_name);

    resp->status        = RESPONSE_STATUS_PASS;
    resp->is_json       = is_json;
    strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
    strncpy(resp->operation, "CREATE_VIEW", sizeof(resp->operation) - 1);
    resp->response_body = body;

    return 0;
}

/* ================================================================== */
/*  dispatch_create_procedure_new                                        */
/*  CREATE_PROCEDURE via the same format-agnostic pipeline every other  */
/*  operation uses. Independent DDL Module proposal (03-Sep), sixth and */
/*  final operation. Same tgen-only shape - calls                        */
/*  get_create_procedure_template()                                      */
/*  (OCI_DDL_Create_Procedure_Module.c) and renders the result. Does NOT */
/*  execute against the database - no execute module for                 */
/*  CREATE_PROCEDURE exists yet.                                          */
/* ================================================================== */
static int dispatch_create_procedure_new(oci_context_t       *ctx,
                                          const char          *filename,
                                          input_c_request_t   *request,
                                          input_c_operation_t *op,
                                          response_object_t   *resp)
{
    create_procedure_request_t *req = (create_procedure_request_t *)op->payload;
    int is_json = (request->source_format == INPUT_FORMAT_JSON);

    if (!req)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CREATE_PROCEDURE/new]: %s - no "
                     "create_procedure_request_t payload", filename);
        build_error_envelope(resp, request->external_audit_id, "CREATE_PROCEDURE",
                              "NO_PAYLOAD", "No create_procedure_request_t payload after Level 1/2",
                              is_json);
        return -1;
    }

    char *body = NULL;

    if (is_json)
    {
        char ddl_text[8192 + PROCEDURE_BODY_LEN] = {0};
        build_create_procedure_ddl_text(req, ddl_text, sizeof(ddl_text));
        char *e_ddl = json_escape_ddl(ddl_text);

        size_t body_cap = strlen(e_ddl ? e_ddl : "") + 4096;
        body = malloc(body_cap);
        if (body && e_ddl)
        {
            snprintf(body, body_cap,
                "{\"operation\":\"CREATE_PROCEDURE\",\"status\":\"TEMPLATE_GENERATED\","
                "\"procedure_name\":\"%s\",\"parameter_count\":%d,\"generated_ddl\":\"%s\"}",
                req->procedure_name, req->parameter_count, e_ddl);
        }
        free(e_ddl);
    }
    else
    {
        xml_builder_t *xml = get_create_procedure_template(ctx, req);
        if (xml && xml->buffer)
            body = strdup(xml->buffer);
        if (xml) xml_free(xml);
    }

    if (!body)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "FAIL [CREATE_PROCEDURE/new] audit_id=%s: %s - "
                     "get_create_procedure_template produced no body",
                     request->external_audit_id, filename);
        build_error_envelope(resp, request->external_audit_id, "CREATE_PROCEDURE",
                              "NO_RESULT_BODY",
                              "get_create_procedure_template succeeded but produced no result body",
                              is_json);
        return -1;
    }

    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                 "PASS [CREATE_PROCEDURE/new] audit_id=%s: procedure_name=%s "
                 "parameter_count=%d (template generated, not yet executed - "
                 "no execute module for CREATE_PROCEDURE exists)",
                 request->external_audit_id, req->procedure_name,
                 req->parameter_count);

    resp->status        = RESPONSE_STATUS_PASS;
    resp->is_json       = is_json;
    strncpy(resp->audit_id,  request->external_audit_id, sizeof(resp->audit_id) - 1);
    strncpy(resp->operation, "CREATE_PROCEDURE", sizeof(resp->operation) - 1);
    resp->response_body = body;

    return 0;
}

/* ================================================================== */
/*  process_xml_file                                                    */
/*  Read file, extract operation, dispatch to correct handler.         */
/* ================================================================== */
/* ================================================================== */
/*  validate_async_select_request                                       */
/*  Stage 5 fix (2026-08-24)                                            */
/* ================================================================== */
/*
 * Runs Level 1 parse + Level 2 validate ONLY - no session validation,
 * no transaction, no dispatch/execute. Built specifically to let
 * http_consumer.c know, cheaply and synchronously, whether an
 * execute_async=1 request will actually be accepted BEFORE deciding to
 * route it onto the fire-and-forget path and tell the client 202.
 *
 * The bug this closes (found via real testing, 2026-08-23/24): the
 * fire-and-forget path decided sync-vs-async purely from a text sniff
 * on the raw payload ("does it contain execute_async=1"), with no idea
 * whether Level 2 would actually accept the request. A request with an
 * empty or non-HTTPS async_call_back_url, or execute_async=1 alongside
 * a sibling write in the same transaction, would still get told
 * "202 Accepted - batches will be delivered" - then get correctly
 * rejected by Level 2 on the worker thread a moment later, with nowhere
 * for that rejection to go (fire-and-forget has no completion signal to
 * report through). The client was told success; nothing was ever
 * delivered, silently.
 *
 * Cost of running Level 1/Level 2 twice (once here, once again on the
 * worker thread once accepted) is genuinely small - both are pure
 * parsing/validation, no database round trip at all - versus the
 * multi-second cost of blocking on the full fetch+deliver loop this
 * gate exists specifically to avoid.
 *
 * Returns 0 if the request would be accepted (Level 1 and Level 2 both
 * pass, or the payload isn't even the new envelope format Level 1/
 * Level 2 apply to at all) - resp is left untouched, caller should
 * proceed to dispatch normally.
 *
 * Returns -1 if rejected - resp is populated with a complete, ready-to-
 * send error envelope (the exact same shape build_error_envelope()
 * always produces, matching every other rejection path in this file) -
 * caller should send that directly as a normal response and must not
 * dispatch this request at all.
 */
int validate_async_select_request(oci_context_t     *ctx,
                                   const char        *payload,
                                   long               payload_length,
                                   response_object_t *resp)
{
    if (!level1_looks_like_new_format(payload, (size_t)payload_length))
    {
        /* Not the new envelope format - nothing for this gate to
         * validate; let the normal path handle whatever this actually
         * is. */
        return 0;
    }

    input_c_request_t  new_request;
    operation_status_t level1_error;
    memset(&new_request, 0, sizeof(new_request));
    memset(&level1_error, 0, sizeof(level1_error));

    int level1_rc = level1_parse(ctx, payload, (size_t)payload_length,
                                  &new_request, &level1_error);
    if (level1_rc != LEVEL1_OK)
    {
        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "execute_async pre-check FAIL [Level1] error_code=%s "
                     "error_text=%s", level1_error.error_code,
                     level1_error.error_text);
        build_error_envelope(resp, "-", "-", level1_error.error_code,
                              level1_error.error_text, 0);
        return -1;
    }

    int is_json = (new_request.source_format == INPUT_FORMAT_JSON);

    int level2_rc = level2_validate(ctx, &new_request);
    if (level2_rc != LEVEL2_OK)
    {
        /* Same one-response-per-file limitation process_xml_file()
         * itself already has (see its own comment on that) - the
         * first operation's error is what surfaces as the whole
         * request's result. */
        const char *err_code = "-";
        const char *err_text = "Level 2 validation failed";
        if (new_request.operation_count > 0)
        {
            err_code = new_request.operations[0].validation_status.error_code;
            err_text = new_request.operations[0].validation_status.error_text;
        }

        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                     "execute_async pre-check FAIL [Level2] audit_id=%s "
                     "error_code=%s error_text=%s",
                     new_request.external_audit_id, err_code, err_text);

        build_error_envelope(resp, new_request.external_audit_id, "-",
                              err_code, err_text, is_json);
        level1_free_request(&new_request);
        return -1;
    }

    level1_free_request(&new_request);
    return 0;
}

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
         * rejected regardless.
         *
         * EXEMPTION (Security Module, 2026-08-31): a standalone
         * OP_AUTHENTICATE request is exempt from this gate entirely.
         * This predates the Security Module and had no reason to
         * account for AUTHENTICATE when it was written (2026-08-08) -
         * but by definition, a client calling AUTHENTICATE does not
         * have a session yet; that's the entire point of the call.
         * Requiring a valid session just to attempt authentication is
         * backwards - it would mean a genuinely new client, with no
         * prior connection/session at all, could never successfully
         * authenticate in the first place. This was masked in every
         * round of Security Module testing so far because Run.sh's
         * own tester keeps one persistent connection open for its
         * whole run and always has a real session_id_override
         * available (from its own initial CREATE_SESSION) by the time
         * any AUTHENTICATE fixture runs on that connection - a
         * genuinely fresh client (or a standalone script issuing one
         * cold request, like test_check_permission.sh) has no such
         * override and was being rejected here, never reaching
         * auth_authenticate() at all. Scoped narrowly: only a request
         * containing EXACTLY ONE operation, of type OP_AUTHENTICATE -
         * matching how CREATE_SESSION/END_SESSION are already
         * standalone-only by convention elsewhere in this file, not a
         * blanket "skip validation whenever AUTHENTICATE appears
         * anywhere in a transaction" exemption.                       */
        int is_standalone_authenticate =
            (new_request.operation_count == 1 &&
             new_request.operations[0].type == OP_AUTHENTICATE);

        if (ctx->ini->session_validation_enabled && !is_standalone_authenticate)
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

        /* Closure item 5 follow-up (2026-08-10) - the actual fix for a
         * genuine metrics gap: session_id/audit_id were "-" in every
         * metrics row because nothing ever stamped them onto a
         * worker's own ctx, per request. active_session_id/
         * active_audit_id (OCI_Connection.h) were purpose-built for
         * exactly this - their own doc comments already said
         * "metrics.c reads these two fields directly" - but the write
         * side (here) was never actually wired up. Unconditional,
         * regardless of session_validation_enabled: metrics tagging
         * shouldn't depend on whether hard validation happens to be
         * on, and a request that fails validation above already
         * returned before reaching here, so this only ever runs for a
         * request that's actually about to be dispatched.              */
        strncpy(ctx->active_session_id, new_request.session_id,
                sizeof(ctx->active_session_id) - 1);
        ctx->active_session_id[sizeof(ctx->active_session_id) - 1] = '\0';

        strncpy(ctx->active_audit_id, new_request.external_audit_id,
                sizeof(ctx->active_audit_id) - 1);
        ctx->active_audit_id[sizeof(ctx->active_audit_id) - 1] = '\0';

        uint64_t level2_start = metrics_now_us();
        int level2_rc = level2_validate(ctx, &new_request);
        ctx->level2_parse_us = metrics_now_us() - level2_start;
        int rc = 0;

        if (level2_rc == LEVEL2_OK)
        {
            /* Per-request transaction scoping (File Consumer closure
             * item, 2026-08-12). transaction_required=1 wraps every
             * operation below in one tx_begin()/tx_commit()/
             * tx_rollback() - the design already described in this
             * header's own pipeline comment, and already fully
             * anticipated by every CRUD execute module (each already
             * checks ctx->active_tx and skips its own OCITransCommit,
             * and already reads ctx->active_tx->tx_name into metrics
             * and audit trail CHANGE_REASON) - only the actual
             * tx_begin() call was ever missing here. transaction_name
             * defaults to "No Name Specified" at Level 1 when the
             * client didn't supply one, so it's never empty. */
            tx_handle_t tx;
            int tx_active = 0;

            if (new_request.transaction_required)
            {
                tx_init(&tx, ctx);
                char *tx_begin_xml = NULL;
                int tx_rc = tx_begin(&tx, new_request.session_id,
                                      new_request.transaction_name, &tx_begin_xml);
                free(tx_begin_xml);

                if (tx_rc == TX_OK)
                {
                    ctx->active_tx = &tx;
                    tx_active = 1;
                    logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                                 "File='%s' transaction_required=1 - began "
                                 "transaction '%s' name='%s'", filename,
                                 tx.transaction_id, new_request.transaction_name);
                }
                else
                {
                    logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                                 "File='%s' transaction_required=1 but tx_begin() "
                                 "failed (rc=%d) - aborting request rather than "
                                 "running its operations unmanaged", filename, tx_rc);
                    build_error_envelope(resp, new_request.external_audit_id, "-",
                                         "TX_BEGIN_FAILED",
                                         "Could not start the required transaction "
                                         "for this request", is_json);
                    level1_free_request(&new_request);
                    return -1;
                }
            }

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
                else if (op->type == OP_AUTHENTICATE)
                {
                    rc = dispatch_authenticate_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_CHECK_PERMISSION)
                {
                    rc = dispatch_check_permission_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_CREATE_USER)
                {
                    rc = dispatch_create_user_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_GRANT)
                {
                    rc = dispatch_grant_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_CREATE_TABLE)
                {
                    rc = dispatch_create_table_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_DROP_TABLE)
                {
                    rc = dispatch_drop_table_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_CREATE_VIEW)
                {
                    rc = dispatch_create_view_new(ctx, filename, &new_request, op, resp);
                }
                else if (op->type == OP_CREATE_PROCEDURE)
                {
                    rc = dispatch_create_procedure_new(ctx, filename, &new_request, op, resp);
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
                                 "UPDATE/DELETE/EXECUTE_PROCEDURE/"
                                 "AUTHENTICATE/CHECK_PERMISSION/"
                                 "CREATE_USER/GRANT/CREATE_TABLE/"
                                 "DROP_TABLE/CREATE_VIEW/CREATE_PROCEDURE "
                                 "so far, skipping",
                                 filename, i, (int)op->type);
                    build_error_envelope(resp, new_request.external_audit_id, "-",
                                         "UNSUPPORTED_OPERATION_TYPE",
                                         "This operation type isn't implemented by the "
                                         "new dispatch pipeline yet", is_json);
                    rc = -1;
                }

                /* Transactional request, one operation failed - stop
                 * here rather than running (and immediately discarding,
                 * via the rollback below) further operations that are
                 * doomed regardless. Non-transactional requests are
                 * unaffected - they keep running every operation
                 * independently, exactly as before. */
                if (tx_active && rc != 0)
                    break;
            }

            if (tx_active)
            {
                char *tx_result_xml = NULL;

                if (rc == 0)
                {
                    int commit_rc = tx_commit(&tx, &tx_result_xml);
                    if (commit_rc != TX_OK)
                    {
                        logger_write(ctx->dispatcher_logger, LOG_ERROR, __func__, 0,
                                     "File='%s' tx_commit() failed (rc=%d) on "
                                     "transaction '%s' - treating request as FAIL",
                                     filename, commit_rc, tx.transaction_id);
                        build_error_envelope(resp, new_request.external_audit_id, "-",
                                             "TX_COMMIT_FAILED",
                                             "The transaction failed to commit",
                                             is_json);
                        rc = -1;
                    }
                    else
                        logger_write(ctx->dispatcher_logger, LOG_INFO, __func__, 0,
                                     "File='%s' transaction '%s' committed",
                                     filename, tx.transaction_id);
                }
                else
                {
                    int rollback_rc = tx_rollback(&tx, &tx_result_xml);
                    logger_write(ctx->dispatcher_logger,
                                 rollback_rc == TX_OK ? LOG_WARN : LOG_ERROR,
                                 __func__, 0,
                                 "File='%s' operation failed - transaction '%s' "
                                 "rolled back (tx_rollback rc=%d)", filename,
                                 tx.transaction_id, rollback_rc);
                    /* resp already carries the triggering operation's own
                     * error envelope from the loop above - not overwritten
                     * here, per Data_Manager_Request_Definitions.docx's
                     * rollback behaviour: the client sees whichever
                     * operation actually caused the rollback. */
                }

                free(tx_result_xml);
                ctx->active_tx = NULL;
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
