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
 *   commit / rollback      SQLEndTran on the session's connection, then
 *                          back to autocommit (Z7, 2026-10-25: connections
 *                          sit in autocommit and each write transaction
 *                          runs in manual-commit - MSSQL_Pool.h).
 *                          commit never returns DB_TX_RETRYABLE (D2): a
 *                          deadlock (1205) has already rolled the whole
 *                          transaction back, and a lost connection
 *                          (08S01) leaves the outcome unknown - retrying
 *                          the commit alone would be wrong in both cases.
 *   everything else        filled in by stages 3-6 below (the
 *                          "not implemented yet" stubs are gone as of
 *                          stage 6).
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
 * Stage 5 (2026-10-21), points W1-W7: long columns in SELECT
 *                          (mssql_select.c), statement text as UTF-16
 *                          (mssql_text.c)
 *
 * Stage 6 (2026-10-21), points X1-X6:
 *   dml_execute_procedure / select_open_from_cursor / cursor_handle_free
 *                          mssql_proc.c - result sets read in order into
 *                          the CURSOR parameters, then the OUT values
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
#include "mssql_proc.h"        /* Stage 6 */
#include "logger.h"

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
/* Z7 (2026-10-25): back to autocommit only after SQLEndTran succeeded.
 * After a failed commit the connection stays in manual-commit - switching
 * autocommit on would commit the failed work - and core's rollback then
 * ends it. A failed switch back is logged (WARN) and does not fail the
 * commit or rollback, which did happen. */
static int mssql_commit(oci_context_t *ctx, logger_t *logger)
{
    if (!ctx) return -1;
    logger_write(logger, LOG_DEBUG, __func__, 0, "COMMIT (SQLEndTran)");
    if (mssql_end_tran(ctx->svchp, 1, logger, __func__) != 0) return -1;
    mssql_tx_end_write(ctx->svchp, ctx->ini, logger, __func__);
    return 0;
}

static int mssql_rollback(oci_context_t *ctx, logger_t *logger)
{
    if (!ctx) return -1;
    logger_write(logger, LOG_DEBUG, __func__, 0, "ROLLBACK (SQLEndTran)");
    if (mssql_end_tran(ctx->svchp, 0, logger, __func__) != 0) return -1;
    mssql_tx_end_write(ctx->svchp, ctx->ini, logger, __func__);
    return 0;
}

/* ================================================================== */
/*  Hooks for later stages (P8)                                         */
/* ================================================================== */
/* select_open / select_fetch_batch / select_close: mssql_select.c
 * (Stage 3). */

/* dml_execute, dml_execute_returning_rowids, rowid_result_free,
 * lob_write_by_rowid and table_key: mssql_dml.c (Stage 4). */

/* dml_execute_procedure, select_open_from_cursor and cursor_handle_free:
 * mssql_proc.c (Stage 6). */

/* describe_table: mssql_metadata.c (Stage 3). */

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
    .dml_execute_procedure        = mssql_proc_execute,          /* mssql_proc.c */
    .select_open_from_cursor      = mssql_proc_open_cursor,      /* mssql_proc.c */
    .describe_table               = mssql_describe_table,
    .cursor_handle_free           = mssql_proc_cursor_free,      /* mssql_proc.c */
    .table_key                    = mssql_table_key,             /* mssql_dml.c */
    .dialect                      = &mssql_dialect,   /* mssql_dialect.c */
};

const db_driver_t *mssql_driver_get(void)
{
    return &g_mssql_driver;
}
