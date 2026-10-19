/*
 * driver_mssql.c
 *
 * SQL Server implementation of db_driver_t - see driver_mssql.h and
 * driver_mssql_proposal_v1_0_18-oct.docx.
 *
 * Stage 2 (2026-10-18), points P5-P8:
 *   connect / disconnect   pooled or standalone, from ini->use_connection_pool
 *                          (the same switch driver_oracle.c uses)
 *   get_session / release_session / session_is_alive /
 *   reconnect_session / health_check
 *                          MSSQL_Pool.c
 *   commit / rollback      SQLEndTran on the session's connection.
 *                          commit never returns DB_TX_RETRYABLE (D2): a
 *                          deadlock (1205) has already rolled the whole
 *                          transaction back, and a lost connection
 *                          (08S01) leaves the outcome unknown - retrying
 *                          the commit alone would be wrong in both cases.
 *   everything else        present, but fails with
 *                          "driver_mssql: <hook> not implemented yet
 *                          (stage N)" - stages 3-6 fill these in.
 *
 * Never includes an Oracle header: m2_compile_proof.py compiles this
 * file with every Oracle header blocked.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver_mssql.h"
#include "db_driver.h"
#include "Connection.h"
#include "MSSQL_Pool.h"
#include "logger.h"

/* ================================================================== */
/*  Not implemented yet (P8)                                            */
/* ================================================================== */
static void not_yet(logger_t *logger, const char *hook, int stage)
{
    logger_write(logger, LOG_ERROR, hook, 0,
                 "driver_mssql: %s not implemented yet (stage %d)", hook, stage);
}

/* ================================================================== */
/*  Connections (P5)                                                    */
/* ================================================================== */
static int mssql_connect(oci_context_t *ctx)
{
    if (!ctx || !ctx->ini) return -1;
    if (ctx->ini->use_connection_pool)
        return MSSQL_Connect_pool(ctx);
    return MSSQL_Connect_standalone(ctx);
}

static void mssql_disconnect(oci_context_t *ctx)
{
    if (!ctx) return;
    if (ctx->pool_handle)
    {
        MSSQL_Disconnect_pool(ctx);
        return;
    }
    MSSQL_Disconnect_standalone(ctx);
}

static int mssql_get_session(oci_context_t *ctx, oci_context_t *worker_ctx)
{
    return MSSQL_Pool_get_session(ctx, worker_ctx);
}

static void mssql_release_session(oci_context_t *ctx, oci_context_t *worker_ctx)
{
    MSSQL_Pool_release_session(ctx, worker_ctx);
}

static int mssql_session_is_alive(oci_context_t *ctx)
{
    return MSSQL_Pool_session_is_alive(ctx);
}

static int mssql_reconnect_session(oci_context_t *base_ctx, oci_context_t *ctx)
{
    return MSSQL_Pool_reconnect_session(base_ctx, ctx);
}

static int mssql_health_check(oci_context_t *ctx)
{
    return MSSQL_Pool_health_check(ctx);
}

/* ================================================================== */
/*  Transactions (P6)                                                   */
/* ================================================================== */
static int mssql_commit(oci_context_t *ctx, logger_t *logger)
{
    if (!ctx) return -1;
    logger_write(logger, LOG_DEBUG, __func__, 0, "COMMIT (SQLEndTran)");
    return mssql_end_tran(ctx->svchp, 1, logger, __func__) == 0 ? 0 : -1;
}

static int mssql_rollback(oci_context_t *ctx, logger_t *logger)
{
    if (!ctx) return -1;
    logger_write(logger, LOG_DEBUG, __func__, 0, "ROLLBACK (SQLEndTran)");
    return mssql_end_tran(ctx->svchp, 0, logger, __func__) == 0 ? 0 : -1;
}

/* ================================================================== */
/*  Hooks for later stages (P8)                                         */
/* ================================================================== */
static int mssql_select_open(oci_context_t *ctx, const db_select_request_t *req,
                             db_select_cursor_t **out_cursor, db_column_meta_t **out_columns,
                             int *out_column_count, int *out_batch_size)
{
    (void)req;
    if (out_cursor)       *out_cursor = NULL;
    if (out_columns)      *out_columns = NULL;
    if (out_column_count) *out_column_count = 0;
    if (out_batch_size)   *out_batch_size = 0;
    not_yet(ctx ? ctx->select_logger : NULL, "select_open", 3);
    return -1;
}

static int mssql_select_fetch_batch(db_select_cursor_t *cursor, resultset_t **out_rs,
                                    int *out_rows_fetched, db_fetch_batch_stats_t *out_stats)
{
    (void)cursor; (void)out_stats;
    if (out_rs)           *out_rs = NULL;
    if (out_rows_fetched) *out_rows_fetched = 0;
    not_yet(NULL, "select_fetch_batch", 3);
    return -1;
}

static void mssql_select_close(db_select_cursor_t *cursor)
{
    (void)cursor;       /* select_open never hands out a cursor yet */
}

static int mssql_dml_execute(oci_context_t *ctx, logger_t *logger, const db_dml_request_t *req,
                             int *out_rows_affected, char *out_error_message,
                             size_t out_error_message_size)
{
    (void)ctx; (void)req;
    if (out_rows_affected) *out_rows_affected = 0;
    if (out_error_message && out_error_message_size)
        snprintf(out_error_message, out_error_message_size,
                 "driver_mssql: dml_execute not implemented yet (stage 4)");
    not_yet(logger, "dml_execute", 4);
    return -1;
}

static int mssql_dml_execute_returning_rowids(oci_context_t *ctx, logger_t *logger,
                                              const db_dml_returning_request_t *req,
                                              db_rowid_result_t *out_result)
{
    (void)ctx; (void)req;
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    not_yet(logger, "dml_execute_returning_rowids", 4);
    return -1;
}

static void mssql_rowid_result_free(db_rowid_result_t *result)
{
    (void)result;       /* nothing is ever allocated into it yet */
}

static int mssql_lob_write_by_rowid(oci_context_t *ctx, logger_t *logger,
                                    const db_lob_write_request_t *req,
                                    uint64_t *out_bytes_written)
{
    (void)ctx; (void)req;
    if (out_bytes_written) *out_bytes_written = 0;
    not_yet(logger, "lob_write_by_rowid", 4);
    return -1;
}

static int mssql_dml_execute_procedure(oci_context_t *ctx, logger_t *logger,
                                       db_proc_execute_request_t *req)
{
    (void)ctx; (void)req;
    not_yet(logger, "dml_execute_procedure", 6);
    return -1;
}

static int mssql_select_open_from_cursor(oci_context_t *ctx, void *cursor_handle,
                                         db_select_cursor_t **out_cursor,
                                         db_column_meta_t **out_columns,
                                         int *out_column_count, int *out_batch_size)
{
    (void)cursor_handle;
    if (out_cursor)       *out_cursor = NULL;
    if (out_columns)      *out_columns = NULL;
    if (out_column_count) *out_column_count = 0;
    if (out_batch_size)   *out_batch_size = 0;
    not_yet(ctx ? ctx->procedure_logger : NULL, "select_open_from_cursor", 6);
    return -1;
}

static int mssql_describe_table(oci_context_t *ctx, metadata_request_t *req,
                                col_metadata_t *cols, int *col_count, int max_cols)
{
    (void)req; (void)cols; (void)max_cols;
    if (col_count) *col_count = 0;
    not_yet(ctx ? ctx->Metadata_logger : NULL, "describe_table", 3);
    return -1;
}

static void mssql_cursor_handle_free(void *cursor_handle)
{
    (void)cursor_handle;    /* no cursor handle is ever handed out yet */
}

/* ---- Dialect (M5) - stage 3. No ctx/logger in these hooks; core
 * logs the failure where it calls them. ---------------------------- */
static int d_bind_placeholder(int pos, char *out, size_t out_max)
{ (void)pos; if (out && out_max) out[0] = '\0'; return -1; }

static int d_value_expr(const char *dtype, const char *operand, char *out, size_t out_max)
{ (void)dtype; (void)operand; if (out && out_max) out[0] = '\0'; return -1; }

static const char *d_lob_placeholder(const char *dtype)
{ (void)dtype; return NULL; }

static int d_row_locator_clauses(db_dml_kind_t kind, const db_row_locator_ctx_t *loc, int pos,
                                 char *mid, size_t mid_max, char *tail, size_t tail_max)
{
    (void)kind; (void)loc; (void)pos;
    if (mid && mid_max)   mid[0] = '\0';
    if (tail && tail_max) tail[0] = '\0';
    return -1;
}

static int d_row_limit_sql(const char *sql, int max_rows, char *out, size_t out_max)
{ (void)sql; (void)max_rows; if (out && out_max) out[0] = '\0'; return -1; }

static int d_count_rows_sql(const char *sql, char *out, size_t out_max)
{ (void)sql; if (out && out_max) out[0] = '\0'; return -1; }

/* now_expr is plain text with no inputs, so it is real already (it
 * never returns NULL to a caller that formats it straight into SQL). */
static const char *d_now_expr(void)
{ return "SYSDATETIME()"; }

static int d_add_seconds_expr(const char *base_expr, const char *seconds_expr,
                              char *out, size_t out_max)
{ (void)base_expr; (void)seconds_expr; if (out && out_max) out[0] = '\0'; return -1; }

static int d_procedure_call_sql(const char *proc_name, const char * const *param_names,
                                int count, char *out, size_t out_max)
{ (void)proc_name; (void)param_names; (void)count; if (out && out_max) out[0] = '\0'; return -1; }

static const db_dialect_t g_mssql_dialect = {
    .bind_placeholder    = d_bind_placeholder,
    .value_expr          = d_value_expr,
    .lob_placeholder     = d_lob_placeholder,
    .row_locator_clauses = d_row_locator_clauses,
    .row_limit_sql       = d_row_limit_sql,
    .count_rows_sql      = d_count_rows_sql,
    .now_expr            = d_now_expr,
    .add_seconds_expr    = d_add_seconds_expr,
    .procedure_call_sql  = d_procedure_call_sql,
};

static const db_driver_t g_mssql_driver = {
    .driver_name                  = "mssql",
    .connect                      = mssql_connect,
    .disconnect                   = mssql_disconnect,
    .get_session                  = mssql_get_session,
    .release_session              = mssql_release_session,
    .session_is_alive             = mssql_session_is_alive,
    .reconnect_session            = mssql_reconnect_session,
    .health_check                 = mssql_health_check,
    .select_open                  = mssql_select_open,
    .select_fetch_batch           = mssql_select_fetch_batch,
    .select_close                 = mssql_select_close,
    .dml_execute                  = mssql_dml_execute,
    .commit                       = mssql_commit,
    .rollback                     = mssql_rollback,
    .dml_execute_returning_rowids = mssql_dml_execute_returning_rowids,
    .rowid_result_free            = mssql_rowid_result_free,
    .lob_write_by_rowid           = mssql_lob_write_by_rowid,
    .dml_execute_procedure        = mssql_dml_execute_procedure,
    .select_open_from_cursor      = mssql_select_open_from_cursor,
    .describe_table               = mssql_describe_table,
    .cursor_handle_free           = mssql_cursor_handle_free,
    .dialect                      = &g_mssql_dialect,
};

const db_driver_t *mssql_driver_get(void)
{
    return &g_mssql_driver;
}
