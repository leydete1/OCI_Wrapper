/*
 * Driver_Transaction_Test.c
 *
 * Follow-up proposal item 2 (2026-09-20) - standalone-harness pass for
 * OCI_Transaction_Manager.c, second module after OCI_Table_Metadata_Module.c
 * (see Driver_Metadata_Test.c). Chosen next because it is genuinely
 * load-bearing - real callers across OCI_Insert_Execute_Module.c,
 * OCI_Update_Execute_Module.c, OCI_Delete_Execute_Module.c,
 * OCI_DDL_Execute_Module.c, OCI_Audit_Trail_Manager.c,
 * OCI_Session_Manager.c, dispatcher.c and Data_Manager_Bootstrap.c - and
 * has never been wrapped by driver_oracle.c at all, unlike
 * get_multi_metadata()/get_request_metadata() which driver_oracle.c
 * already calls through to.
 *
 * Logger lesson from Driver_Metadata_Test.c Run 1/2 applied from the
 * start here: every logger the connect/pool/tx call chain touches
 * (logger, connection_logger, connectionpool_logger, transaction_logger,
 * error_logger) is wired on the worker before any tx_* call, not just
 * transaction_logger - avoids re-learning that gotcha a second time.
 *
 * What it does, one test per documented contract in
 * OCI_Transaction_Manager.h's Public API section:
 *
 *   Test 1  - tx_begin() on a fresh handle: TX_OK, status ACTIVE,
 *             transaction_id is a real UUID (not empty, not "-"),
 *             result_xml contains "<status>ACTIVE</status>".
 *   Test 2  - tx_begin() again on the same (already-active) handle:
 *             TX_ERR_ALREADY_ACTIVE, per the header's documented
 *             contract - the one case this project's OCI_Connection.c/
 *             OCI_Connection_Pool.c pass never got proven since that
 *             audit was about the driver split, not about a module
 *             correctly rejecting a bad call.
 *   Test 3  - the real proof that matters: insert a uniquely-tagged
 *             row (NUMBER_COL=TEST_ROLLBACK_KEY) via a plain OCI call
 *             on the SAME worker ctx (Oracle begins the transaction
 *             implicitly on first DML - see the header's own design
 *             note, no OCITransStart() involved), then tx_rollback().
 *             Independent baseline: SELECT COUNT(*) for that key on
 *             the same session afterwards must be 0 - proves
 *             tx_rollback() actually discards work, not just that it
 *             returns TX_OK.
 *   Test 4  - tx_rollback() with no active transaction: TX_ERR_NO_ACTIVE.
 *   Test 5  - the commit-side mirror of Test 3: insert
 *             NUMBER_COL=TEST_COMMIT_KEY, tx_commit(), then an
 *             independent baseline SELECT COUNT(*) must be 1 - proves
 *             tx_commit() actually persists work. Row is cleaned up
 *             (deleted + committed) at the end of this test so the
 *             harness is repeatable across runs without manual DB
 *             cleanup.
 *   Test 6  - tx_commit() with no active transaction: TX_ERR_NO_ACTIVE.
 *   Test 7  - tx_abort(): insert NUMBER_COL=TEST_ABORT_KEY on an active
 *             handle, then tx_abort() - always returns TX_OK per the
 *             header ("abort is fire-and-forget"), sets status
 *             ABORTED, and the row must NOT be persisted (same
 *             baseline-count proof as Test 3, since abort's
 *             OCITransRollback is best-effort but still a real
 *             rollback).
 *   Test 8  - tx_abort() on a handle with no active transaction: safe
 *             no-op, still returns TX_OK, per the header.
 *   Test 9  - tx_check_timeout() immediately after tx_begin() with the
 *             configured tx_timeout_seconds (300s, config.ini): TX_OK,
 *             not timed out.
 *   Test 10 - tx_check_timeout() forced past its deadline: rather than
 *             sleeping, backdate handle.last_activity_epoch (a plain
 *             time_t field, safe to poke directly on a stack handle in
 *             a single-threaded harness) so idle > timeout_seconds -
 *             deterministic and fast. Must return TX_ERR_TIMEOUT,
 *             leave status TIMED_OUT, and (same baseline-count proof
 *             again) not persist a row inserted before the forced
 *             timeout, since tx_check_timeout() calls tx_abort()
 *             internally.
 *   Test 11  - begin_standalone_tx_if_needed()/end_standalone_tx_if_owned():
 *             with ctx->active_tx NULL, must return 1, populate
 *             ctx->active_tx, and tx_get_id() must return a real UUID
 *             (not "-"); end_standalone_tx_if_owned(ctx, 1) must clear
 *             ctx->active_tx back to NULL. Then with ctx->active_tx
 *             already pointing at an outer handle, must return 0
 *             WITHOUT touching ctx->active_tx - the nested-call
 *             inheritance behaviour the header documents as the whole
 *             point of this pair.
 *
 * Uses UNIT_TEST_FIELD_TEST (NUMBER_COL PK, only NOT NULL column - see
 * Create_Unit_Test_Table.txt and Driver_Metadata_Test.c's Test 1
 * output) so every insert here only ever sets NUMBER_COL, nothing else
 * to satisfy.
 *
 * Pre-test cleanup: deletes any leftover TEST_ROLLBACK_KEY/
 * TEST_COMMIT_KEY/TEST_ABORT_KEY rows and commits before Test 1 runs,
 * in case a previous crashed run left one behind - makes reruns
 * idempotent without needing manual DB cleanup between attempts.
 *
 * Build (same convention as Driver_Metadata_Test.c, dependencies
 * checked against this file's own #include list):
 *
 *   gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
 *       -I/usr/include/cjson -I/usr/include/libxml2 \
 *       -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include -I. \
 *       -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
 *       -o Driver_Transaction_Test \
 *       Driver_Transaction_Test.c db_driver.c driver_oracle.c \
 *       OCI_Connection.c OCI_Connection_Pool.c oci_cache.c string_utils.c \
 *       ini_reader.c logger.c metrics.c ctx_utils.c \
 *       OCI_Transaction_Manager.c \
 *       -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
 *       -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson \
 *       -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
 *
 * Run (same LD_LIBRARY_PATH/LSAN_OPTIONS pattern as Run_Manually.sh):
 *   ./Driver_Transaction_Test
 *
 * Expected output on success:
 *   Test 1  (tx_begin)                          ... OK (tx_id=<uuid>)
 *   Test 2  (tx_begin, already active)          ... OK (TX_ERR_ALREADY_ACTIVE)
 *   Test 3  (tx_rollback discards work)         ... OK (row absent after rollback)
 *   Test 4  (tx_rollback, no active tx)         ... OK (TX_ERR_NO_ACTIVE)
 *   Test 5  (tx_commit persists work)           ... OK (row present after commit)
 *   Test 6  (tx_commit, no active tx)           ... OK (TX_ERR_NO_ACTIVE)
 *   Test 7  (tx_abort discards work)            ... OK (row absent after abort)
 *   Test 8  (tx_abort, no active tx)            ... OK (TX_OK, no-op)
 *   Test 9  (tx_check_timeout, not yet due)     ... OK (TX_OK)
 *   Test 10 (tx_check_timeout, forced timeout)  ... OK (TX_ERR_TIMEOUT, row absent)
 *   Test 11 (standalone tx helpers)             ... OK (owned=1 then 0, id inherited)
 *   PASS
 *
 * A non-zero exit code means at least one test failed - check
 * config.transaction_log_file_name (all tests log there) for the
 * OCI/SQL error detail already logged by the module itself, and
 * config.connectionpool_log_file_name/config.log_file_name for
 * anything upstream of that - same two files that turned out to
 * matter for Driver_Metadata_Test.c's real bug.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

#define TEST_TABLE          "UNIT_TEST_FIELD_TEST"
#define TEST_ROLLBACK_KEY   999001
#define TEST_COMMIT_KEY     999002
#define TEST_ABORT_KEY      999003
#define TEST_TIMEOUT_KEY    999004

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "OCI_Transaction_Manager.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

#define CHECK_OCI(ctx, status)                                            \
    do {                                                                  \
        if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) \
        {                                                                 \
            sb4  _errcode = 0;                                            \
            char _errbuf[512] = {0};                                      \
            OCIErrorGet((ctx)->errhp, 1, NULL, &_errcode,                 \
                        (unsigned char *)_errbuf, sizeof(_errbuf),        \
                        OCI_HTYPE_ERROR);                                 \
            fprintf(stderr, "OCI error %d: %s\n", _errcode, _errbuf);     \
            return -1;                                                    \
        }                                                                 \
    } while (0)

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *main_logger,
                     logger_t *connection_logger,
                     logger_t *connectionpool_logger,
                     logger_t *transaction_logger)
{
    memset(ctx,    0, sizeof(*ctx));
    memset(config, 0, sizeof(*config));
    ctx->ini             = config;
    ctx->pool_slot_index = -1;

    if (load_ini(CONFIG_INI, config, ctx) != 0)
    {
        fprintf(stderr, "Failed to load ini file: %s\n", CONFIG_INI);
        return -1;
    }

    if (logger_init_str(error_logger, config->error_log_file_name,
                         config->error_log_file_max_size,
                         config->error_log_file_rotation_number,
                         config->error_log_level) != 0)
    {
        fprintf(stderr, "Failed to init error_logger\n");
        return -1;
    }
    ctx->error_logger = error_logger;

    if (logger_init_str2(main_logger, config->log_file_name,
                          config->log_file_max_size,
                          config->log_file_rotation_number,
                          config->log_level, ctx->error_logger) != 0)
    {
        fprintf(stderr, "Failed to init main logger\n");
        return -1;
    }
    ctx->logger = main_logger;

    if (logger_init_str2(connection_logger, config->connection_log_file_name,
                          config->connection_log_file_max_size,
                          config->connection_log_file_rotation_number,
                          config->connection_log_level, ctx->error_logger) != 0)
    {
        fprintf(stderr, "Failed to init connection_logger\n");
        return -1;
    }
    ctx->connection_logger = connection_logger;

    if (logger_init_str2(connectionpool_logger, config->connectionpool_log_file_name,
                          config->connectionpool_log_file_max_size,
                          config->connectionpool_log_file_rotation_number,
                          config->connectionpool_log_level, ctx->error_logger) != 0)
    {
        fprintf(stderr, "Failed to init connectionpool_logger\n");
        return -1;
    }
    ctx->connectionpool_logger = connectionpool_logger;

    if (logger_init_str2(transaction_logger, config->transaction_log_file_name,
                          config->transaction_log_file_max_size,
                          config->transaction_log_file_rotation_number,
                          config->transaction_log_level, ctx->error_logger) != 0)
    {
        fprintf(stderr, "Failed to init transaction_logger\n");
        return -1;
    }
    ctx->transaction_logger = transaction_logger;

    return 0;
}

/* Plain OCI insert of one row, only NUMBER_COL set (the table's only
 * NOT NULL column) - runs on the current implicit transaction, no
 * commit/rollback of its own. Independent of anything
 * OCI_Transaction_Manager.c does. */
static int raw_insert(oci_context_t *ctx, int number_col)
{
    const char *sql = "INSERT INTO " TEST_TABLE " (NUMBER_COL) VALUES (:1)";
    OCIStmt *stmt = NULL;
    sword s = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                               (const OraText *)sql, (ub4)strlen(sql),
                               NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    OCIBind *b = NULL;
    s = OCIBindByPos(stmt, &b, ctx->errhp, 1, &number_col, sizeof(number_col),
                      SQLT_INT, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    s = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL,
                        OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return 0;
}

/* Independent baseline: how many rows really exist for this key, via
 * plain OCI, entirely independent of the module under test. */
static int raw_count(oci_context_t *ctx, int number_col, int *out_count)
{
    const char *sql = "SELECT COUNT(*) FROM " TEST_TABLE " WHERE NUMBER_COL = :1";
    OCIStmt *stmt = NULL;
    sword s = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                               (const OraText *)sql, (ub4)strlen(sql),
                               NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    OCIBind *b = NULL;
    s = OCIBindByPos(stmt, &b, ctx->errhp, 1, &number_col, sizeof(number_col),
                      SQLT_INT, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    int cnt = 0;
    OCIDefine *dfn = NULL;
    s = OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, &cnt, sizeof(cnt),
                        SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    s = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL,
                        OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    *out_count = cnt;
    return 0;
}

/* Delete + commit directly (not via the module under test - this is
 * cleanup plumbing, must work independently of whatever Test 3/5/7/10
 * find). */
static int raw_delete_and_commit(oci_context_t *ctx, int number_col)
{
    const char *sql = "DELETE FROM " TEST_TABLE " WHERE NUMBER_COL = :1";
    OCIStmt *stmt = NULL;
    sword s = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                               (const OraText *)sql, (ub4)strlen(sql),
                               NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    OCIBind *b = NULL;
    s = OCIBindByPos(stmt, &b, ctx->errhp, 1, &number_col, sizeof(number_col),
                      SQLT_INT, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    s = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL,
                        OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

    s = OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT);
    CHECK_OCI(ctx, s);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx_base;
    app_config_t  config;
    logger_t      err_log, main_log, conn_log, poolconn_log, tx_log;

    if (init_ctx(&ctx_base, &config, &err_log, &main_log, &conn_log,
                 &poolconn_log, &tx_log) != 0)
        return 1;

    const db_driver_t *driver = db_driver_get(&ctx_base);
    if (!driver || !driver->connect || !driver->disconnect ||
        !driver->get_session || !driver->release_session)
    {
        fprintf(stderr, "db_driver_get() returned an incomplete driver\n");
        return 1;
    }

    if (driver->connect(&ctx_base) != 0)
    {
        fprintf(stderr, "connect() failed - see %s\n", config.connection_log_file_name);
        return 1;
    }

    oci_context_t worker;
    if (driver->get_session(&ctx_base, &worker) != 0)
    {
        fprintf(stderr, "get_session() failed\n");
        driver->disconnect(&ctx_base);
        return 1;
    }
    worker.transaction_logger    = &tx_log;
    worker.connection_logger     = &conn_log;
    worker.connectionpool_logger = &poolconn_log;
    worker.error_logger          = &err_log;
    worker.logger                = &main_log;
    worker.ini                   = &config;
    worker.active_tx              = NULL;

    /* ---- Idempotent pre-test cleanup ---- */
    raw_delete_and_commit(&worker, TEST_ROLLBACK_KEY);
    raw_delete_and_commit(&worker, TEST_COMMIT_KEY);
    raw_delete_and_commit(&worker, TEST_ABORT_KEY);
    raw_delete_and_commit(&worker, TEST_TIMEOUT_KEY);

    tx_handle_t tx;

    /* ---- Test 1: tx_begin() ---- */
    printf("Test 1 (tx_begin) ... ");
    {
        tx_init(&tx, &worker);
        char *xml = NULL;
        int rc = tx_begin(&tx, "test-session", "Driver_Transaction_Test", &xml);
        if (rc != TX_OK || tx.status != TX_STATUS_ACTIVE ||
            tx.transaction_id[0] == '\0' || strcmp(tx.transaction_id, "-") == 0)
        {
            printf("FAILED - rc=%d status=%d tx_id='%s'\n",
                   rc, tx.status, tx.transaction_id);
            failed = 1;
        }
        else if (!xml || !strstr(xml, "<status>ACTIVE</status>"))
        {
            printf("FAILED - result_xml missing ACTIVE status\n");
            failed = 1;
        }
        else
        {
            printf("OK (tx_id=%s)\n", tx.transaction_id);
        }
        if (xml) free(xml);
    }

    /* ---- Test 2: tx_begin() while already active ---- */
    printf("Test 2 (tx_begin, already active) ... ");
    {
        char *xml = NULL;
        int rc = tx_begin(&tx, "test-session", "second begin", &xml);
        if (rc != TX_ERR_ALREADY_ACTIVE)
        {
            printf("FAILED - expected TX_ERR_ALREADY_ACTIVE, got %d\n", rc);
            failed = 1;
        }
        else
        {
            printf("OK (TX_ERR_ALREADY_ACTIVE)\n");
        }
        if (xml) free(xml);
    }

    /* ---- Test 3: tx_rollback() actually discards work ---- */
    printf("Test 3 (tx_rollback discards work) ... ");
    {
        if (raw_insert(&worker, TEST_ROLLBACK_KEY) != 0)
        {
            printf("FAILED - raw_insert errored\n");
            failed = 1;
        }
        else
        {
            char *xml = NULL;
            int rc = tx_rollback(&tx, &xml);
            int cnt = -1;
            raw_count(&worker, TEST_ROLLBACK_KEY, &cnt);
            if (rc != TX_OK || tx.status != TX_STATUS_ROLLED_BACK)
            {
                printf("FAILED - rc=%d status=%d\n", rc, tx.status);
                failed = 1;
            }
            else if (cnt != 0)
            {
                printf("FAILED - row still present after rollback (count=%d)\n", cnt);
                failed = 1;
            }
            else
            {
                printf("OK (row absent after rollback)\n");
            }
            if (xml) free(xml);
        }
    }

    /* ---- Test 4: tx_rollback() with no active tx ---- */
    printf("Test 4 (tx_rollback, no active tx) ... ");
    {
        char *xml = NULL;
        int rc = tx_rollback(&tx, &xml);
        if (rc != TX_ERR_NO_ACTIVE)
        {
            printf("FAILED - expected TX_ERR_NO_ACTIVE, got %d\n", rc);
            failed = 1;
        }
        else
        {
            printf("OK (TX_ERR_NO_ACTIVE)\n");
        }
        if (xml) free(xml);
    }

    /* ---- Test 5: tx_commit() actually persists work ---- */
    printf("Test 5 (tx_commit persists work) ... ");
    {
        tx_init(&tx, &worker);
        char *begin_xml = NULL;
        tx_begin(&tx, "test-session", "commit test", &begin_xml);
        if (begin_xml) free(begin_xml);

        if (raw_insert(&worker, TEST_COMMIT_KEY) != 0)
        {
            printf("FAILED - raw_insert errored\n");
            failed = 1;
        }
        else
        {
            char *xml = NULL;
            int rc = tx_commit(&tx, &xml);
            int cnt = -1;
            raw_count(&worker, TEST_COMMIT_KEY, &cnt);
            if (rc != TX_OK || tx.status != TX_STATUS_COMMITTED)
            {
                printf("FAILED - rc=%d status=%d\n", rc, tx.status);
                failed = 1;
            }
            else if (cnt != 1)
            {
                printf("FAILED - expected row present after commit (count=%d)\n", cnt);
                failed = 1;
            }
            else
            {
                printf("OK (row present after commit)\n");
            }
            if (xml) free(xml);
        }
        /* Clean up regardless of pass/fail so the table is left clean. */
        raw_delete_and_commit(&worker, TEST_COMMIT_KEY);
    }

    /* ---- Test 6: tx_commit() with no active tx ---- */
    printf("Test 6 (tx_commit, no active tx) ... ");
    {
        char *xml = NULL;
        int rc = tx_commit(&tx, &xml);
        if (rc != TX_ERR_NO_ACTIVE)
        {
            printf("FAILED - expected TX_ERR_NO_ACTIVE, got %d\n", rc);
            failed = 1;
        }
        else
        {
            printf("OK (TX_ERR_NO_ACTIVE)\n");
        }
        if (xml) free(xml);
    }

    /* ---- Test 7: tx_abort() actually discards work ---- */
    printf("Test 7 (tx_abort discards work) ... ");
    {
        tx_init(&tx, &worker);
        char *begin_xml = NULL;
        tx_begin(&tx, "test-session", "abort test", &begin_xml);
        if (begin_xml) free(begin_xml);

        if (raw_insert(&worker, TEST_ABORT_KEY) != 0)
        {
            printf("FAILED - raw_insert errored\n");
            failed = 1;
        }
        else
        {
            char *xml = NULL;
            int rc = tx_abort(&tx, "harness test", &xml);
            int cnt = -1;
            raw_count(&worker, TEST_ABORT_KEY, &cnt);
            if (rc != TX_OK || tx.status != TX_STATUS_ABORTED)
            {
                printf("FAILED - rc=%d status=%d (abort is documented as always TX_OK)\n",
                       rc, tx.status);
                failed = 1;
            }
            else if (cnt != 0)
            {
                printf("FAILED - row still present after abort (count=%d)\n", cnt);
                failed = 1;
            }
            else
            {
                printf("OK (row absent after abort)\n");
            }
            if (xml) free(xml);
        }
    }

    /* ---- Test 8: tx_abort() with no active tx (safe no-op) ---- */
    printf("Test 8 (tx_abort, no active tx) ... ");
    {
        char *xml = NULL;
        int rc = tx_abort(&tx, "no-op check", &xml);
        if (rc != TX_OK)
        {
            printf("FAILED - expected TX_OK (fire-and-forget), got %d\n", rc);
            failed = 1;
        }
        else
        {
            printf("OK (TX_OK, no-op)\n");
        }
        if (xml) free(xml);
    }

    /* ---- Test 9: tx_check_timeout(), not yet due ---- */
    printf("Test 9 (tx_check_timeout, not yet due) ... ");
    {
        tx_init(&tx, &worker);
        char *begin_xml = NULL;
        tx_begin(&tx, "test-session", "timeout test 1", &begin_xml);
        if (begin_xml) free(begin_xml);

        int rc = tx_check_timeout(&tx, NULL);
        if (rc != TX_OK || tx.status != TX_STATUS_ACTIVE)
        {
            printf("FAILED - rc=%d status=%d\n", rc, tx.status);
            failed = 1;
        }
        else
        {
            printf("OK (TX_OK)\n");
        }
    }

    /* ---- Test 10: tx_check_timeout(), forced past its deadline ---- */
    printf("Test 10 (tx_check_timeout, forced timeout) ... ");
    {
        /* Reuses the still-active handle from Test 9 rather than
         * beginning a fresh one, so this exercises the real ACTIVE ->
         * TIMED_OUT transition on a handle that's actually mid-life,
         * not one freshly constructed just for this test. */
        if (raw_insert(&worker, TEST_TIMEOUT_KEY) != 0)
        {
            printf("FAILED - raw_insert errored\n");
            failed = 1;
        }
        else
        {
            tx.last_activity_epoch = time(NULL) - (tx.timeout_seconds + 5);

            char *xml = NULL;
            int rc = tx_check_timeout(&tx, &xml);
            int cnt = -1;
            raw_count(&worker, TEST_TIMEOUT_KEY, &cnt);
            if (rc != TX_ERR_TIMEOUT || tx.status != TX_STATUS_TIMED_OUT)
            {
                printf("FAILED - rc=%d status=%d\n", rc, tx.status);
                failed = 1;
            }
            else if (cnt != 0)
            {
                printf("FAILED - row still present after forced timeout (count=%d)\n", cnt);
                failed = 1;
            }
            else
            {
                printf("OK (TX_ERR_TIMEOUT, row absent)\n");
            }
            if (xml) free(xml);
        }
    }

    /* ---- Test 11: begin_standalone_tx_if_needed()/end_standalone_tx_if_owned() ---- */
    printf("Test 11 (standalone tx helpers) ... ");
    {
        tx_handle_t local_tx;
        worker.active_tx = NULL;

        int owned = begin_standalone_tx_if_needed(&worker, &local_tx);
        const char *inner_id = tx_get_id(worker.active_tx);

        if (owned != 1 || worker.active_tx != &local_tx ||
            !inner_id || strcmp(inner_id, "-") == 0)
        {
            printf("FAILED - owned=%d active_tx=%p inner_id='%s' "
                   "(expected owned=1, active_tx set, real uuid)\n",
                   owned, (void *)worker.active_tx,
                   inner_id ? inner_id : "(null)");
            failed = 1;
        }
        else
        {
            end_standalone_tx_if_owned(&worker, owned);
            if (worker.active_tx != NULL)
            {
                printf("FAILED - active_tx not cleared after end_standalone_tx_if_owned\n");
                failed = 1;
            }
            else
            {
                /* Second half: an outer transaction already active -
                 * must be inherited, not overwritten. */
                tx_handle_t outer;
                tx_init(&outer, &worker);
                char *outer_xml = NULL;
                tx_begin(&outer, "test-session", "outer", &outer_xml);
                if (outer_xml) free(outer_xml);
                worker.active_tx = &outer;

                tx_handle_t nested_local;
                int nested_owned = begin_standalone_tx_if_needed(&worker, &nested_local);
                const char *outer_id_seen = tx_get_id(worker.active_tx);

                if (nested_owned != 0 || worker.active_tx != &outer ||
                    strcmp(outer_id_seen, outer.transaction_id) != 0)
                {
                    printf("FAILED - nested_owned=%d active_tx=%p "
                           "(expected owned=0, outer handle untouched)\n",
                           nested_owned, (void *)worker.active_tx);
                    failed = 1;
                }
                else
                {
                    printf("OK (owned=1 then 0, id inherited)\n");
                }

                end_standalone_tx_if_owned(&worker, nested_owned);
                char *outer_rb_xml = NULL;
                tx_rollback(&outer, &outer_rb_xml);
                if (outer_rb_xml) free(outer_rb_xml);
                worker.active_tx = NULL;
            }
        }
    }

    printf("%s\n", failed ? "FAIL" : "PASS");

    driver->release_session(&ctx_base, &worker);
    driver->disconnect(&ctx_base);

    logger_close(&err_log);
    logger_close(&main_log);
    logger_close(&conn_log);
    logger_close(&poolconn_log);
    logger_close(&tx_log);

    return failed ? 1 : 0;
}
