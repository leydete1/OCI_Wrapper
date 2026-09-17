/*
 * OCI_DDL_Execute_Module.c
 *
 * See OCI_DDL_Execute_Module.h for the full design note, especially
 * the DDL auto-commit / ctx->active_tx guard - that's the one piece
 * of behaviour here that isn't just "OCIStmtPrepare2 + OCIStmtExecute,
 * same as every other execute module in this codebase."
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "OCI_DDL_Execute_Module.h"
#include "OCI_Transaction_Manager.h"   /* begin_standalone_tx_if_needed()/
                                        * end_standalone_tx_if_owned() -
                                        * metrics correlation only, see
                                        * their own doc comment; NOT a
                                        * real commit/rollback boundary
                                        * for DDL (Oracle auto-commits
                                        * DDL regardless)               */
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  OCI error macro - consistent with the rest of the project           */
/*  (OCI_Insert_Execute_Module.c's own CHECK_OCI_INS, adapted to        */
/*  write into result->error_message instead of goto-ing to a cleanup   */
/*  label - this function has no OCI resources to release except the    */
/*  one statement handle, freed unconditionally in Cleanup either way)  */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_DDL(errhp, status, result_ptr, label)                    \
    do {                                                                    \
        if ((status) != OCI_SUCCESS &&                                     \
            (status) != OCI_SUCCESS_WITH_INFO)                             \
        {                                                                   \
            text _errbuf[512];                                              \
            sb4  _errcode = 0;                                              \
            OCIErrorGet((errhp), 1, NULL, &_errcode,                       \
                        _errbuf, sizeof(_errbuf), OCI_HTYPE_ERROR);         \
            snprintf((result_ptr)->error_message,                          \
                     sizeof((result_ptr)->error_message),                  \
                     "OCI Error %d: %s", _errcode, (char *)_errbuf);       \
            (result_ptr)->success = 0;                                      \
            goto label;                                                    \
        }                                                                   \
    } while (0)

/* ==================================================================
 *  execute_ddl_statement
 * ================================================================== */
int execute_ddl_statement(oci_context_t            *ctx,
                           const char               *ddl_text,
                           const char               *operation_label,
                           int                       is_plsql_block,
                           ddl_execution_result_t   *result)
{
    if (!ctx || !ddl_text || !result)
    {
        if (ctx)
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx, ddl_text or result is NULL");
        return -1;
    }

    memset(result, 0, sizeof(*result));

    const char *label = operation_label ? operation_label : "DDL";

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering execute_ddl_statement operation=%s ddl_len=%zu "
                 "is_plsql_block=%d",
                 label, strlen(ddl_text), is_plsql_block);

    /* ---- Hard safety boundary - see header doc comment ---- */
    if (ctx->active_tx)
    {
        snprintf(result->error_message, sizeof(result->error_message),
                 "Refusing to execute %s: a managed transaction is "
                 "active (tx_id='%s'). Oracle DDL implicitly commits, "
                 "which would silently commit that transaction's "
                 "pending work. Commit or roll back the active "
                 "transaction first.",
                 label, tx_get_id(ctx->active_tx));
        result->success = 0;
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s",
                     result->error_message);
        return 0;
    }

    OCIStmt *stmt = NULL;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    /* Strip exactly one trailing ';' (and surrounding whitespace) - the
     * six build_*_ddl_text() functions append it for human-readable
     * display (the SQL*Plus convention), but OCIStmtPrepare2/
     * OCIStmtExecute execute exactly one statement and don't want a
     * terminator; Oracle's parser rejects it with different errors
     * depending on grammar context (ORA-03048 for GRANT/CREATE VIEW/
     * DROP TABLE, ORA-00922 for CREATE TABLE and CREATE USER) -
     * discovered against a live instance 07-Sep/08-Sep.
     *
     * NOT done when is_plsql_block is set (CREATE PROCEDURE/FUNCTION/
     * TRIGGER/PACKAGE) - there, the final ';' after "END [name]" is
     * syntactically mandatory, not an optional terminator, and
     * stripping it produces PLS-00103 ("end-of-file when expecting
     * ;") - also discovered against a live instance, 08-Sep. */
    char *exec_text = strdup(ddl_text);
    if (!exec_text)
    {
        snprintf(result->error_message, sizeof(result->error_message),
                 "strdup failed while preparing %s for execution", label);
        result->success = 0;
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s",
                     result->error_message);
        return 0;
    }
    if (!is_plsql_block)
    {
        size_t elen = strlen(exec_text);
        while (elen > 0 && isspace((unsigned char)exec_text[elen - 1])) elen--;
        if (elen > 0 && exec_text[elen - 1] == ';') elen--;
        while (elen > 0 && isspace((unsigned char)exec_text[elen - 1])) elen--;
        exec_text[elen] = '\0';
    }

    /* Metrics/audit correlation ID only - NOT a real commit/rollback
     * boundary for this call. See header doc comment and
     * begin_standalone_tx_if_needed()'s own doc comment
     * (OCI_Transaction_Manager.h) for why. ctx->active_tx is
     * guaranteed NULL here (checked above), so this always returns 1
     * and owns the standalone identity for the duration of this call. */
    tx_handle_t local_tx;
    int owns_standalone_tx = begin_standalone_tx_if_needed(ctx, &local_tx);

    result->success = 1;   /* CHECK_OCI_DDL flips this to 0 on failure */

    CHECK_OCI_DDL(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)exec_text, (ub4)strlen(exec_text),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        result, Cleanup);

    CHECK_OCI_DDL(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                       1, 0, NULL, NULL, OCI_DEFAULT),
        result, Cleanup);

    /* No explicit OCITransCommit - DDL auto-commits unconditionally,
     * see header doc comment. Issuing one here would be a harmless
     * no-op at best and misleading at worst (implies this call has a
     * commit step to skip/control, which it doesn't). */

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "execute_ddl_statement OK: operation=%s", label);

Cleanup:
    if (stmt)
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    free(exec_text);

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    result->execution_time_seconds =
        (double)(ts_end.tv_sec - ts_start.tv_sec) +
        (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    if (!result->success)
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "execute_ddl_statement FAILED: operation=%s error=%s",
                     label, result->error_message);

    end_standalone_tx_if_owned(ctx, owns_standalone_tx);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Static helper: minimal JSON string escaping - same "just enough"    */
/*  approach dispatcher.c's own json_escape_ddl() takes, duplicated     */
/*  here since that one is private to dispatcher.c and this module      */
/*  needs the identical behaviour for its own JSON response builder.    */
/* ------------------------------------------------------------------ */
static char *json_escape_string(const char *src)
{
    if (!src) return strdup("");

    size_t len = strlen(src);
    char  *out = malloc(len * 2 + 1);
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
            case '\r': break;
            default:   out[o++] = (char)c; break;
        }
    }
    out[o] = '\0';
    return out;
}

/* ==================================================================
 *  get_ddl_execution_response_xml
 * ================================================================== */
char *get_ddl_execution_response_xml(oci_context_t                  *ctx,
                                      const char                     *operation_label,
                                      const char                     *ddl_text,
                                      const ddl_execution_result_t   *result)
{
    if (!ctx || !operation_label || !ddl_text || !result)
        return NULL;

    xml_builder_t *xml = xml_create(2048);
    if (!xml) return NULL;

    char *e_op  = xml_escape(operation_label);
    char *e_ddl = xml_escape(ddl_text);

    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<DDL_Execution_Result>\n");
    xml_append(xml, "  <operation>%s</operation>\n", e_op);
    xml_append(xml, "  <status>%s</status>\n",
               result->success ? "SUCCESS" : "FAILED");
    xml_append(xml, "  <executed_ddl>%s</executed_ddl>\n", e_ddl);
    xml_append(xml, "  <execution_time>%.6f</execution_time>\n",
               result->execution_time_seconds);
    if (!result->success)
    {
        char *e_err = xml_escape(result->error_message);
        xml_append(xml, "  <error_message>%s</error_message>\n", e_err);
        free(e_err);
    }
    xml_append(xml, "</DDL_Execution_Result>\n");

    free(e_op);
    free(e_ddl);

    char *out = xml->buffer ? strdup(xml->buffer) : NULL;
    xml_free(xml);
    return out;
}

/* ==================================================================
 *  get_ddl_execution_response_json
 * ================================================================== */
char *get_ddl_execution_response_json(oci_context_t                  *ctx,
                                       const char                     *operation_label,
                                       const char                     *ddl_text,
                                       const ddl_execution_result_t   *result)
{
    if (!ctx || !operation_label || !ddl_text || !result)
        return NULL;

    char *e_ddl = json_escape_string(ddl_text);
    if (!e_ddl) return NULL;

    size_t cap = strlen(e_ddl) + strlen(operation_label) +
                 sizeof(result->error_message) + 256;
    char *out = malloc(cap);
    if (!out)
    {
        free(e_ddl);
        return NULL;
    }

    if (result->success)
    {
        snprintf(out, cap,
            "{\"operation\":\"%s\",\"status\":\"SUCCESS\","
            "\"executed_ddl\":\"%s\",\"execution_time\":%.6f}",
            operation_label, e_ddl, result->execution_time_seconds);
    }
    else
    {
        char *e_err = json_escape_string(result->error_message);
        snprintf(out, cap,
            "{\"operation\":\"%s\",\"status\":\"FAILED\","
            "\"executed_ddl\":\"%s\",\"execution_time\":%.6f,"
            "\"error_message\":\"%s\"}",
            operation_label, e_ddl, result->execution_time_seconds,
            e_err ? e_err : "");
        free(e_err);
    }

    free(e_ddl);
    return out;
}
