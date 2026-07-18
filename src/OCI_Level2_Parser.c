/*
 * OCI_Level2_Parser.c
 *
 * See OCI_Level2_Parser.h for the full design description.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OCI_Level2_Parser.h"
#include "logger.h"
#include "sql_dependency_extractor.h"

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

            /* Not yet implemented - fail closed. An operation type with
             * no validator here must never silently proceed to the
             * CRUD layer unvalidated.                                  */
            case OP_INSERT:
            case OP_UPDATE:
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
