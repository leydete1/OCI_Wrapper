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
 * Stage 3 (2026-10-19), points T1-T10:
 *   select_open / select_fetch_batch / select_close
 *                          mssql_select.c - scalar columns; a long
 *                          (max/text/xml) column is refused until stage 5
 *   describe_table         mssql_metadata.c
 *   dialect                mssql_dialect.c - all hooks except
 *                          row_locator_clauses (stage 4)
 *
 * Stage 4 (2026-10-20), points U1-U6:
 *   dml_execute / dml_execute_returning_rowids / rowid_result_free /
 *   lob_write_by_rowid / table_key
 *                          mssql_dml.c
 *   dialect                row_locator_clauses (OUTPUT inserted.<key>),
 *                          text LOBs bound in the statement
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
#include "mssql_dialect.h"     /* Stage 3 */
#include "mssql_select.h"      /* Stage 3 */
#include "mssql_metadata.h"    /* Stage 3 */
#include "mssql_dml.h"         /* Stage 4 */
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
/* select_open / select_fetch_batch / select_close: mssql_select.c
 * (Stage 3). */

/* dml_execute, dml_execute_returning_rowids, rowid_result_free,
 * lob_write_by_rowid and table_key: mssql_dml.c (Stage 4). */

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

/* describe_table: mssql_metadata.c (Stage 3). */

static void mssql_cursor_handle_free(void *cursor_handle)
{
    (void)cursor_handle;    /* no cursor handle is ever handed out yet */
}

/* Dialect (M5): mssql_dialect.c (Stage 3). */

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
    .dml_execute                  = mssql_dml_execute,           /* mssql_dml.c */
    .commit                       = mssql_commit,
    .rollback                     = mssql_rollback,
    .dml_execute_returning_rowids = mssql_dml_execute_returning_rowids,
    .rowid_result_free            = mssql_rowid_result_free,
    .lob_write_by_rowid           = mssql_lob_write_by_rowid,
    .dml_execute_procedure        = mssql_dml_execute_procedure,
    .select_open_from_cursor      = mssql_select_open_from_cursor,
    .describe_table               = mssql_describe_table,
    .cursor_handle_free           = mssql_cursor_handle_free,
    .table_key                    = mssql_table_key,             /* mssql_dml.c */
    .dialect                      = &mssql_dialect,   /* mssql_dialect.c */
};

const db_driver_t *mssql_driver_get(void)
{
    return &g_mssql_driver;
}
