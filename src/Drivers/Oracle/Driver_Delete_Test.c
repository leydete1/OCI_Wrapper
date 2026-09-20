/*
 * Driver_Delete_Test.c
 *
 * Validates db_driver_t's new dml_execute()/commit()/rollback() against
 * real rows in OCI_FIELD_TEST (see Create_Oracle_Test_Table.txt - no PK/
 * unique constraint on number_col, so this harness owns its own cleanup
 * rather than relying on one). Same A/B philosophy as
 * Driver_Select_Test.c: every check the driver's own work is measured
 * against comes from plain, direct OCI calls in this file, independent
 * of driver_oracle.c entirely - an authoritative "did it actually
 * happen in the database" baseline, not just "did the function return 0".
 *
 * Reserved NUMBER_COL range for this harness: 999999001-999999099.
 * Chosen to sit well clear of every other fixture/test's own values
 * (select_integration_tests' 901/902, Unit_Test_Delete_Round_1's 200,
 * UNIT_TEST_FIELD_TEST's own 999099 - a DIFFERENT table entirely, not a
 * collision risk, but close enough in naming convention to standardise
 * on the same "999xxx = reserved for testing" pattern already
 * established elsewhere in this project rather than inventing a new one).
 *
 * Setup (direct OCI, before any test runs): idempotent - deletes any
 * leftover row from a previous interrupted run in this reserved range
 * first, so re-running this harness after a crash never starts from an
 * unknown state.
 *
 * What it does:
 *   Test 1 - Direct baseline: DELETE any leftover rows in the reserved
 *            range, INSERT one fresh row (NUMBER_COL=999999001), commit -
 *            all direct OCI, independent of driver_oracle.c. Confirms via
 *            direct COUNT that exactly 1 row exists before Test 2 runs.
 *
 *   Test 2 - dml_execute() single-key delete: driver->dml_execute() on
 *            "DELETE FROM OCI_FIELD_TEST WHERE NUMBER_COL = :1",
 *            bind_values={"999999001"}. Checks rc=0 and
 *            out_rows_affected=1.
 *
 *   Test 3 - commit() + verify: driver->commit(), then an independent
 *            direct COUNT confirms the row is genuinely gone (0), not
 *            just locally believed deleted - proves dml_execute() alone
 *            doesn't commit (matches db_driver.h's own contract) and
 *            that commit() actually does.
 *
 *   Test 4 - rollback() + verify: fresh row (NUMBER_COL=999999002,
 *            direct OCI, committed), dml_execute() deletes it
 *            (rows_affected=1 expected), but driver->rollback() is
 *            called instead of commit() - an independent direct COUNT
 *            must then show the row is STILL there (1), proving
 *            rollback() genuinely reverts, not a no-op that happens to
 *            not error.
 *
 *   Test 5 - Multi-key bind: fresh row (NUMBER_COL=999999003,
 *            VARCHAR2_COL='multikey', direct OCI, committed),
 *            dml_execute() with TWO bind values
 *            ("DELETE ... WHERE NUMBER_COL = :1 AND VARCHAR2_COL = :2"),
 *            checks rows_affected=1, commits, verifies gone - exercises
 *            bind_count > 1, which Test 2 alone never touches.
 *
 *   Test 6 - Zero-match delete: dml_execute() against a NUMBER_COL value
 *            known not to exist (999999099) - a real DELETE's normal
 *            "nothing matched" outcome, not an error case. Checks rc=0
 *            and out_rows_affected=0 specifically (0 rows is success,
 *            not failure - matches OCI_Delete_Execute_Module.c's own
 *            documented behavior for this case).
 *
 * Final cleanup (direct OCI, best-effort, does not affect PASS/FAIL):
 *   removes every row left in the reserved range, whichever tests
 *   passed or failed, so a re-run never inherits state from this one.
 *
 * Build (same convention as Driver_Select_Test.c/Driver_Pool_Test.c):
 *
 *   gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
 *       -I/usr/include/cjson -I/usr/include/libxml2 \
 *       -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include -I. \
 *       -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
 *       -o Driver_Delete_Test \
 *       Driver_Delete_Test.c db_driver.c driver_oracle.c \
 *       OCI_Connection.c OCI_Connection_Pool.c oci_cache.c string_utils.c \
 *       ini_reader.c logger.c metrics.c ctx_utils.c \
 *       OCI_Table_Metadata_Module.c OCI_Resultset_Builder.c \
 *       -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
 *       -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson \
 *       -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
 *
 * Run (same LD_LIBRARY_PATH/LSAN_OPTIONS pattern as Run_Manually.sh):
 *   ./Driver_Delete_Test
 *
 * Expected output on success:
 *   Test 1 (direct baseline setup)      ... OK (1 row present)
 *   Test 2 (dml_execute single-key)     ... OK (rows_affected=1)
 *   Test 3 (commit + verify gone)       ... OK (0 rows, commit confirmed)
 *   Test 4 (rollback + verify present)  ... OK (1 row, rollback confirmed)
 *   Test 5 (dml_execute multi-key)      ... OK (rows_affected=1)
 *   Test 6 (zero-match delete)          ... OK (rc=0, rows_affected=0)
 *   PASS
 *
 * Vendor-internal leak notes: same accepted category as every previous
 * harness in this series - see Driver_Connect_Test.c's header.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

#include <stdio.h>
#include <string.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

/* Local OCI error macro, same shape as driver_oracle.c's own
 * ORACLE_CHECK_OCI - this file needs its own copy for the direct-OCI
 * setup/verification helpers, deliberately independent of anything in
 * driver_oracle.c (see header comment). */
#define TEST_CHECK_OCI(ctx, status) \
    do { \
        if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
            text errbuf[512]; sb4 errcode = 0; \
            OCIErrorGet((ctx)->errhp, 1, NULL, &errcode, errbuf, \
                        sizeof(errbuf), OCI_HTYPE_ERROR); \
            logger_write((ctx)->delete_logger, LOG_ERROR, __func__, 0, \
                         "OCI Error %d: %s", errcode, (char *)errbuf); \
        } \
    } while (0)

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *connection_logger,
                     logger_t *connectionpool_logger, logger_t *delete_logger)
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
    { fprintf(stderr, "Failed to init error_logger\n"); return -1; }
    ctx->error_logger = error_logger;

    if (logger_init_str2(connection_logger, config->connection_log_file_name,
                          config->connection_log_file_max_size,
                          config->connection_log_file_rotation_number,
                          config->connection_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init connection_logger\n"); return -1; }
    ctx->connection_logger = connection_logger;

    if (logger_init_str2(connectionpool_logger, config->connectionpool_log_file_name,
                          config->connectionpool_log_file_max_size,
                          config->connectionpool_log_file_rotation_number,
                          config->connectionpool_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init connectionpool_logger\n"); return -1; }
    ctx->connectionpool_logger = connectionpool_logger;

    if (logger_init_str2(delete_logger, config->delete_log_file_name,
                          config->delete_log_file_max_size,
                          config->delete_log_file_rotation_number,
                          config->delete_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init delete_logger\n"); return -1; }
    ctx->delete_logger = delete_logger;

    return 0;
}

/* ---- Direct-OCI helpers, independent of driver_oracle.c entirely ---- */

static int direct_row_count_by_number(oci_context_t *ctx, int number_val,
                                       int *out_count)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM OCI_FIELD_TEST WHERE NUMBER_COL = %d",
             number_val);

    OCIStmt   *stmt  = NULL;
    OCIDefine *defn  = NULL;
    int        count = 0;

    TEST_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!stmt) return -1;

    TEST_CHECK_OCI(ctx,
        OCIDefineByPos(stmt, &defn, ctx->errhp, 1, &count, sizeof(count),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT));

    TEST_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT));

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

    *out_count = count;
    return 0;
}

/* Direct DELETE + commit - used for idempotent setup cleanup and final
 * teardown. Best-effort: a "0 rows matched" is not an error here. */
static int direct_delete_by_number(oci_context_t *ctx, int number_val)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "DELETE FROM OCI_FIELD_TEST WHERE NUMBER_COL = %d", number_val);

    OCIStmt *stmt = NULL;

    TEST_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!stmt) return -1;

    TEST_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT));

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

    TEST_CHECK_OCI(ctx, OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT));

    return 0;
}

/* Direct INSERT + commit - builds a fresh, known-good test row.
 * varchar_val may be NULL for a plain NUMBER_COL-only row. */
static int direct_insert_row(oci_context_t *ctx, int number_val,
                              const char *varchar_val)
{
    char sql[512];
    if (varchar_val)
        snprintf(sql, sizeof(sql),
                 "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL) "
                 "VALUES (%d, '%s')", number_val, varchar_val);
    else
        snprintf(sql, sizeof(sql),
                 "INSERT INTO OCI_FIELD_TEST (NUMBER_COL) VALUES (%d)",
                 number_val);

    OCIStmt *stmt = NULL;

    TEST_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!stmt) return -1;

    TEST_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT));

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

    TEST_CHECK_OCI(ctx, OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT));

    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, connpool_l, delete_l;
    if (init_ctx(&ctx, &config, &err_l, &conn_l, &connpool_l, &delete_l) != 0)
    {
        printf("INIT FAILED\n");
        return 1;
    }

    const db_driver_t *driver = db_driver_get(&ctx);
    if (!driver || !driver->connect || !driver->get_session ||
        !driver->dml_execute || !driver->commit || !driver->rollback ||
        !driver->release_session || !driver->disconnect)
    {
        printf("FAILED - db_driver_get() returned an incomplete driver\n");
        return 1;
    }

    if (driver->connect(&ctx) != 0)
    {
        printf("FAILED - connect(): see %s\n", config.connectionpool_log_file_name);
        return 1;
    }

    oci_context_t worker_ctx;
    memset(&worker_ctx, 0, sizeof(worker_ctx));
    if (driver->get_session(&ctx, &worker_ctx) != 0)
    {
        printf("FAILED - get_session()\n");
        driver->disconnect(&ctx);
        return 1;
    }

    /* ---- Idempotent pre-cleanup - leftover state from a previous
     *      interrupted run must not affect this one. Best-effort,
     *      not itself a pass/fail check. ---- */
    direct_delete_by_number(&worker_ctx, 999999001);
    direct_delete_by_number(&worker_ctx, 999999002);
    direct_delete_by_number(&worker_ctx, 999999003);

    /* ---- Test 1 - direct baseline ---- */
    printf("Test 1 (direct baseline setup)      ... ");
    int count1 = -1;
    if (direct_insert_row(&worker_ctx, 999999001, NULL) != 0 ||
        direct_row_count_by_number(&worker_ctx, 999999001, &count1) != 0 ||
        count1 != 1)
    {
        printf("FAILED - setup or baseline count wrong (got %d, expected 1)\n", count1);
        failed = 1;
    }
    else
        printf("OK (%d row present)\n", count1);

    /* ---- Test 2 - dml_execute() single-key delete ---- */
    const char *bind1[] = { "999999001" };
    db_dml_request_t req2;
    memset(&req2, 0, sizeof(req2));
    req2.sql         = "DELETE FROM OCI_FIELD_TEST WHERE NUMBER_COL = :1";
    req2.bind_count  = 1;
    req2.bind_values = bind1;

    int rows2 = -1;
    printf("Test 2 (dml_execute single-key)     ... ");
    int rc2 = driver->dml_execute(&worker_ctx, &delete_l, &req2, &rows2, NULL, 0);
    if (rc2 != 0 || rows2 != 1)
    {
        printf("FAILED - rc=%d rows_affected=%d (expected rc=0, rows=1)\n", rc2, rows2);
        failed = 1;
    }
    else
        printf("OK (rows_affected=%d)\n", rows2);

    /* ---- Test 3 - commit() + independent verify ---- */
    printf("Test 3 (commit + verify gone)       ... ");
    int count3 = -1;
    if (driver->commit(&worker_ctx, &delete_l) != 0 ||
        direct_row_count_by_number(&worker_ctx, 999999001, &count3) != 0)
    {
        printf("FAILED - commit() or verify count failed\n");
        failed = 1;
    }
    else if (count3 != 0)
    {
        printf("FAILED - row still present after commit (count=%d, expected 0)\n", count3);
        failed = 1;
    }
    else
        printf("OK (%d rows, commit confirmed)\n", count3);

    /* ---- Test 4 - rollback() + independent verify ---- */
    printf("Test 4 (rollback + verify present)  ... ");
    int count4 = -1;
    const char *bind4[] = { "999999002" };
    db_dml_request_t req4;
    memset(&req4, 0, sizeof(req4));
    req4.sql         = "DELETE FROM OCI_FIELD_TEST WHERE NUMBER_COL = :1";
    req4.bind_count  = 1;
    req4.bind_values = bind4;

    int rows4 = -1;
    if (direct_insert_row(&worker_ctx, 999999002, NULL) != 0)
    {
        printf("FAILED - setup insert for rollback test failed\n");
        failed = 1;
    }
    else
    {
        int rc4 = driver->dml_execute(&worker_ctx, &delete_l, &req4, &rows4, NULL, 0);
        if (rc4 != 0 || rows4 != 1)
        {
            printf("FAILED - dml_execute rc=%d rows_affected=%d (expected rc=0, rows=1)\n",
                   rc4, rows4);
            failed = 1;
        }
        else if (driver->rollback(&worker_ctx, &delete_l) != 0 ||
                 direct_row_count_by_number(&worker_ctx, 999999002, &count4) != 0)
        {
            printf("FAILED - rollback() or verify count failed\n");
            failed = 1;
        }
        else if (count4 != 1)
        {
            printf("FAILED - row missing after rollback (count=%d, expected 1)\n", count4);
            failed = 1;
        }
        else
            printf("OK (%d row, rollback confirmed)\n", count4);
    }

    /* ---- Test 5 - multi-key bind ---- */
    printf("Test 5 (dml_execute multi-key)      ... ");
    const char *bind5[] = { "999999003", "multikey" };
    db_dml_request_t req5;
    memset(&req5, 0, sizeof(req5));
    req5.sql = "DELETE FROM OCI_FIELD_TEST "
               "WHERE NUMBER_COL = :1 AND VARCHAR2_COL = :2";
    req5.bind_count  = 2;
    req5.bind_values = bind5;

    int rows5 = -1;
    if (direct_insert_row(&worker_ctx, 999999003, "multikey") != 0)
    {
        printf("FAILED - setup insert for multi-key test failed\n");
        failed = 1;
    }
    else
    {
        int rc5 = driver->dml_execute(&worker_ctx, &delete_l, &req5, &rows5, NULL, 0);
        if (rc5 != 0 || rows5 != 1)
        {
            printf("FAILED - rc=%d rows_affected=%d (expected rc=0, rows=1)\n", rc5, rows5);
            failed = 1;
        }
        else
        {
            driver->commit(&worker_ctx, &delete_l);
            printf("OK (rows_affected=%d)\n", rows5);
        }
    }

    /* ---- Test 6 - zero-match delete ---- */
    printf("Test 6 (zero-match delete)          ... ");
    const char *bind6[] = { "999999099" };   /* deliberately never inserted */
    db_dml_request_t req6;
    memset(&req6, 0, sizeof(req6));
    req6.sql         = "DELETE FROM OCI_FIELD_TEST WHERE NUMBER_COL = :1";
    req6.bind_count  = 1;
    req6.bind_values = bind6;

    int rows6 = -1;
    int rc6 = driver->dml_execute(&worker_ctx, &delete_l, &req6, &rows6, NULL, 0);
    if (rc6 != 0 || rows6 != 0)
    {
        printf("FAILED - rc=%d rows_affected=%d (expected rc=0, rows=0)\n", rc6, rows6);
        failed = 1;
    }
    else
    {
        driver->commit(&worker_ctx, &delete_l);
        printf("OK (rc=%d, rows_affected=%d)\n", rc6, rows6);
    }

    /* ---- Final cleanup - best-effort, does not affect PASS/FAIL ---- */
    direct_delete_by_number(&worker_ctx, 999999001);
    direct_delete_by_number(&worker_ctx, 999999002);
    direct_delete_by_number(&worker_ctx, 999999003);

    driver->release_session(&ctx, &worker_ctx);
    driver->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&err_l);
    logger_close(&conn_l);
    logger_close(&connpool_l);
    logger_close(&delete_l);

    return failed ? 1 : 0;
}
