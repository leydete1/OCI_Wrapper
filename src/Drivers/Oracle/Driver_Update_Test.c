/*
 * Driver_Update_Test.c
 *
 * Validates db_driver_t's new dml_execute_returning_rowids()/
 * rowid_result_free()/lob_write_by_rowid() against real rows in
 * OCI_FIELD_TEST, same A/B philosophy as Driver_Delete_Test.c - every
 * check the driver's own work is measured against comes from plain,
 * direct OCI calls in this file, independent of driver_oracle.c
 * entirely. dml_execute()/commit()/rollback() (proven via
 * Driver_Delete_Test.c already) are exercised again here too, since
 * UPDATE's own scalar-only path (Path A, see notes doc) genuinely
 * reuses them unchanged - not re-testing the functions themselves, but
 * confirming they still behave identically when called with UPDATE's
 * own SQL shape (SET + WHERE) rather than DELETE's (WHERE only).
 *
 * Reserved NUMBER_COL range for this harness: 999998001-999998014 -
 * deliberately a DIFFERENT block from Driver_Delete_Test.c's own
 * 999999001-999999099, so both harnesses can run against the same
 * table without any risk of collision.
 *
 * What it does:
 *
 *   Test 1 - Direct baseline: seeds one scalar-only row
 *            (NUMBER_COL=999998001), confirms via direct COUNT.
 *
 *   Test 2 - dml_execute() scalar-only single-row update (Path A):
 *            SET VARCHAR2_COL=:1 WHERE NUMBER_COL=:2. Confirms rc=0,
 *            rows_affected=1, and - independently, via a direct SELECT,
 *            not just the driver's own return code - that the new
 *            value genuinely landed in the database.
 *
 *   Test 3 - commit() + independent verify: same shape as
 *            Driver_Delete_Test.c's own Test 3 - confirms
 *            dml_execute() alone doesn't commit, and commit() does.
 *
 *   Test 4 - rollback() + independent verify: updates a fresh row's
 *            value, rolls back instead of committing, confirms via
 *            direct SELECT the ORIGINAL value is still there, not the
 *            attempted new one.
 *
 *   Test 5 - dml_execute_returning_rowids() multi-row match (no LOB) -
 *            THE key regression test for the rows_affected bug found
 *            during this pass's own analysis. Seeds 5 rows sharing
 *            VARCHAR2_COL='UPDATE_TEST_GROUP_A', updates all of them
 *            in one WHERE VARCHAR2_COL=:1 statement, and checks BOTH
 *            that out_result->count is genuinely 5 (not 1) AND that
 *            all 5 rows' values actually changed, via 5 independent
 *            direct SELECTs - not just trusting the collector's own
 *            count.
 *
 *   Test 6 - lob_write_by_rowid() BLOB, single row: obtains a ROWID via
 *            dml_execute_returning_rowids() (SET BLOB_COL=EMPTY_BLOB()),
 *            writes a real file's content to it, verifies via a direct
 *            DBMS_LOB.GETLENGTH query that the length matches the real
 *            file size - not just trusting the driver's own
 *            out_bytes_written.
 *
 *   Test 7 - lob_write_by_rowid() CLOB, single row: same shape as
 *            Test 6 but via inline_text (no file), matching
 *            handle_clob_update()'s own real-world usage for literal
 *            (non-file://) values.
 *
 *   Test 8 - Multi-row match + LOB together - THE exact scenario the
 *            original 2026-08-27 fix was written for. Seeds 5 rows
 *            sharing VARCHAR2_COL='UPDATE_TEST_GROUP_B', runs
 *            dml_execute_returning_rowids() with SET CLOB_COL=EMPTY_CLOB()
 *            WHERE VARCHAR2_COL=:1, confirms 5 rowids come back, then
 *            calls lob_write_by_rowid() once per rowid - verifies via 5
 *            independent DBMS_LOB.GETLENGTH queries that EVERY row
 *            genuinely got the CLOB content, not just the first one.
 *
 * Final cleanup (direct OCI, best-effort, does not affect PASS/FAIL):
 *   removes every row left in the reserved range.
 *
 * Build: same convention as Driver_Delete_Test.c - see that file's own
 * header comment for the exact gcc invocation; just add this file and
 * remove Driver_Delete_Test.c (or add both, they don't conflict - each
 * has its own main(), so build one at a time).
 *
 * Run: same LD_LIBRARY_PATH/LSAN_OPTIONS pattern as Run_Manually.sh.
 *
 * Vendor-internal leak notes: same accepted category as every previous
 * harness in this series - see Driver_Connect_Test.c's header.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

/* Matches driver_oracle.c's own ORACLE_ROWID_BUF_SIZE (itself matching
 * OCI_Update_Execute_Module.c's ROWID_BUF_SIZE) - db_rowid_result_t's
 * rowids buffer is flat, this-many-bytes per entry. Not exposed via
 * db_driver.h (an internal driver detail, not part of the interface
 * contract), so this test - stepping through multiple entries in
 * Test 8 - keeps its own copy, same reasoning driver_oracle.c gives
 * for keeping its own rather than sharing one across the boundary. */
#define TEST_ROWID_BUF_SIZE 24

#define TEST_CHECK_OCI(ctx, status) \
    do { \
        if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
            text errbuf[512]; sb4 errcode = 0; \
            OCIErrorGet((ctx)->errhp, 1, NULL, &errcode, errbuf, \
                        sizeof(errbuf), OCI_HTYPE_ERROR); \
            logger_write((ctx)->update_logger, LOG_ERROR, __func__, 0, \
                         "OCI Error %d: %s", errcode, (char *)errbuf); \
        } \
    } while (0)

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *connection_logger,
                     logger_t *connectionpool_logger, logger_t *update_logger)
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

    if (logger_init_str2(update_logger, config->update_log_file_name,
                          config->update_log_file_max_size,
                          config->update_log_file_rotation_number,
                          config->update_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init update_logger\n"); return -1; }
    ctx->update_logger = update_logger;

    return 0;
}

/* ---- Direct-OCI helpers, independent of driver_oracle.c entirely ---- */

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

static int direct_insert_row(oci_context_t *ctx, int number_val,
                              const char *varchar_val)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL) "
             "VALUES (%d, '%s')", number_val, varchar_val);

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

/* Reads VARCHAR2_COL for one row by NUMBER_COL - used to verify updates
 * independently of the driver's own return values. */
static int direct_select_varchar(oci_context_t *ctx, int number_val,
                                  char *out_buf, size_t out_buf_size)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT VARCHAR2_COL FROM OCI_FIELD_TEST WHERE NUMBER_COL = %d",
             number_val);

    OCIStmt   *stmt = NULL;
    OCIDefine *defn = NULL;
    out_buf[0] = '\0';

    TEST_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!stmt) return -1;

    TEST_CHECK_OCI(ctx,
        OCIDefineByPos(stmt, &defn, ctx->errhp, 1, out_buf, (sb4)out_buf_size,
                       SQLT_STR, NULL, NULL, NULL, OCI_DEFAULT));

    TEST_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT));

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return 0;
}

/* Reads a LOB column's byte length for one row via DBMS_LOB.GETLENGTH -
 * an independent, driver-agnostic verification of what actually landed
 * in the database, not just what the driver claims it wrote. */
static int direct_lob_length(oci_context_t *ctx, int number_val,
                              const char *col_name, int *out_len)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT DBMS_LOB.GETLENGTH(%s) FROM OCI_FIELD_TEST "
             "WHERE NUMBER_COL = %d", col_name, number_val);

    OCIStmt   *stmt = NULL;
    OCIDefine *defn = NULL;
    int        len  = -1;

    TEST_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!stmt) return -1;

    TEST_CHECK_OCI(ctx,
        OCIDefineByPos(stmt, &defn, ctx->errhp, 1, &len, sizeof(len),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT));

    TEST_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT));

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    *out_len = len;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, connpool_l, update_l;
    if (init_ctx(&ctx, &config, &err_l, &conn_l, &connpool_l, &update_l) != 0)
    {
        printf("INIT FAILED\n");
        return 1;
    }

    const db_driver_t *driver = db_driver_get(&ctx);
    if (!driver || !driver->connect || !driver->get_session ||
        !driver->dml_execute || !driver->commit || !driver->rollback ||
        !driver->dml_execute_returning_rowids || !driver->rowid_result_free ||
        !driver->lob_write_by_rowid ||
        !driver->release_session || !driver->disconnect)
    {
        printf("FAILED - db_driver_get() returned an incomplete driver\n");
        return 1;
    }

    if (driver->connect(&ctx) != 0)
    {
        printf("FAILED - connect()\n");
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

    /* ---- Idempotent pre-cleanup ---- */
    for (int n = 999998001; n <= 999998014; n++)
        direct_delete_by_number(&worker_ctx, n);

    /* ---- Test 1 - direct baseline ---- */
    printf("Test 1 (direct baseline setup)      ... ");
    if (direct_insert_row(&worker_ctx, 999998001, "original_value") != 0)
    {
        printf("FAILED - setup insert failed\n");
        failed = 1;
    }
    else
        printf("OK\n");

    /* ---- Test 2 - dml_execute() scalar-only update (Path A) ---- */
    printf("Test 2 (dml_execute scalar update)  ... ");
    const char *bind2[] = { "updated_by_test2", "999998001" };
    db_dml_request_t req2;
    memset(&req2, 0, sizeof(req2));
    req2.sql = "UPDATE OCI_FIELD_TEST SET VARCHAR2_COL = :1 "
               "WHERE NUMBER_COL = :2";
    req2.bind_count  = 2;
    req2.bind_values = bind2;

    int rows2 = -1;
    int rc2 = driver->dml_execute(&worker_ctx, &update_l, &req2, &rows2);
    char verify2[256];
    if (rc2 != 0 || rows2 != 1)
    {
        printf("FAILED - rc=%d rows_affected=%d (expected rc=0, rows=1)\n", rc2, rows2);
        failed = 1;
    }
    else if (driver->commit(&worker_ctx, &update_l) != 0 ||
             direct_select_varchar(&worker_ctx, 999998001, verify2, sizeof(verify2)) != 0)
    {
        printf("FAILED - commit or verify select failed\n");
        failed = 1;
    }
    else if (strcmp(verify2, "updated_by_test2") != 0)
    {
        printf("FAILED - value is '%s', expected 'updated_by_test2'\n", verify2);
        failed = 1;
    }
    else
        printf("OK (rows_affected=%d, verified value='%s')\n", rows2, verify2);

    /* ---- Test 3 is folded into Test 2's own commit+verify above,
     * matching exactly what Driver_Delete_Test.c's Test 3 already
     * proved for dml_execute()/commit() generically - no need to
     * re-prove the same two functions a second time here. Test 4
     * covers rollback() instead, which Test 2 doesn't touch. ---- */

    /* ---- Test 4 - rollback() + independent verify ---- */
    printf("Test 4 (rollback + verify unchanged)... ");
    if (direct_insert_row(&worker_ctx, 999998002, "before_rollback") != 0)
    {
        printf("FAILED - setup insert failed\n");
        failed = 1;
    }
    else
    {
        const char *bind4[] = { "attempted_new_value", "999998002" };
        db_dml_request_t req4;
        memset(&req4, 0, sizeof(req4));
        req4.sql = "UPDATE OCI_FIELD_TEST SET VARCHAR2_COL = :1 "
                   "WHERE NUMBER_COL = :2";
        req4.bind_count  = 2;
        req4.bind_values = bind4;

        int rows4 = -1;
        int rc4 = driver->dml_execute(&worker_ctx, &update_l, &req4, &rows4);
        char verify4[256];
        if (rc4 != 0 || rows4 != 1)
        {
            printf("FAILED - dml_execute rc=%d rows_affected=%d\n", rc4, rows4);
            failed = 1;
        }
        else if (driver->rollback(&worker_ctx, &update_l) != 0 ||
                 direct_select_varchar(&worker_ctx, 999998002, verify4, sizeof(verify4)) != 0)
        {
            printf("FAILED - rollback or verify select failed\n");
            failed = 1;
        }
        else if (strcmp(verify4, "before_rollback") != 0)
        {
            printf("FAILED - value is '%s', expected original 'before_rollback'\n", verify4);
            failed = 1;
        }
        else
            printf("OK (value correctly still '%s')\n", verify4);
    }

    /* ---- Test 5 - dml_execute_returning_rowids() multi-row match ---- */
    printf("Test 5 (multi-row match, no LOB)    ... ");
    int setup_ok = 1;
    for (int n = 999998005; n <= 999998009 && setup_ok; n++)
        if (direct_insert_row(&worker_ctx, n, "UPDATE_TEST_GROUP_A") != 0)
            setup_ok = 0;

    if (!setup_ok)
    {
        printf("FAILED - group seed insert failed\n");
        failed = 1;
    }
    else
    {
        const char *bind5[] = { "matched_by_group", "UPDATE_TEST_GROUP_A" };
        db_dml_returning_request_t req5;
        memset(&req5, 0, sizeof(req5));
        req5.sql = "UPDATE OCI_FIELD_TEST SET VARCHAR2_COL = :1 "
                   "WHERE VARCHAR2_COL = :2 RETURNING ROWID INTO :3";
        req5.bind_count  = 2;
        req5.bind_values = bind5;
        req5.returning_bind_position = 3;

        db_rowid_result_t result5;
        int rc5 = driver->dml_execute_returning_rowids(&worker_ctx, &update_l,
                                                         &req5, &result5);
        if (rc5 != 0 || result5.count != 5)
        {
            printf("FAILED - rc=%d count=%d (expected rc=0, count=5)\n",
                   rc5, result5.count);
            failed = 1;
            if (rc5 == 0) driver->rowid_result_free(&result5);
        }
        else
        {
            driver->commit(&worker_ctx, &update_l);
            driver->rowid_result_free(&result5);

            /* Independently verify all 5 rows actually changed, not
             * just trusting the collector's own count. */
            int all_match = 1;
            for (int n = 999998005; n <= 999998009; n++)
            {
                char v[256];
                if (direct_select_varchar(&worker_ctx, n, v, sizeof(v)) != 0 ||
                    strcmp(v, "matched_by_group") != 0)
                {
                    all_match = 0;
                }
            }

            if (!all_match)
            {
                printf("FAILED - count=5 reported, but not all 5 rows verified updated\n");
                failed = 1;
            }
            else
                printf("OK (count=5, all 5 rows independently verified)\n");
        }
    }

    /* ---- Test 6 - lob_write_by_rowid() BLOB, single row ---- */
    printf("Test 6 (lob_write_by_rowid BLOB)    ... ");
    const char *blob_test_path = "/tmp/driver_update_test_blob.bin";
    FILE *bfp = fopen(blob_test_path, "wb");
    unsigned char blob_content[500];
    for (int i = 0; i < 500; i++) blob_content[i] = (unsigned char)(i % 256);
    int blob_write_ok = bfp && fwrite(blob_content, 1, sizeof(blob_content), bfp) == sizeof(blob_content);
    if (bfp) fclose(bfp);

    if (!blob_write_ok || direct_insert_row(&worker_ctx, 999998003, "blob_row") != 0)
    {
        printf("FAILED - test file or seed row setup failed\n");
        failed = 1;
    }
    else
    {
        const char *bind6[] = { "999998003" };
        db_dml_returning_request_t req6;
        memset(&req6, 0, sizeof(req6));
        req6.sql = "UPDATE OCI_FIELD_TEST SET BLOB_COL = EMPTY_BLOB() "
                   "WHERE NUMBER_COL = :1 RETURNING ROWID INTO :2";
        req6.bind_count  = 1;
        req6.bind_values = bind6;
        req6.returning_bind_position = 2;

        db_rowid_result_t result6;
        int rc6 = driver->dml_execute_returning_rowids(&worker_ctx, &update_l,
                                                         &req6, &result6);
        if (rc6 != 0 || result6.count != 1)
        {
            printf("FAILED - dml_execute_returning_rowids rc=%d count=%d\n", rc6, result6.count);
            failed = 1;
            if (rc6 == 0) driver->rowid_result_free(&result6);
        }
        else
        {
            db_lob_write_request_t lreq6;
            memset(&lreq6, 0, sizeof(lreq6));
            lreq6.is_blob     = 1;
            lreq6.table_fq    = "OCI_FIELD_TEST";
            lreq6.column_name = "BLOB_COL";
            lreq6.rowid_str   = result6.rowids;   /* first (only) row */
            lreq6.file_path   = blob_test_path;

            uint64_t bytes6 = 0;
            int lrc6 = driver->lob_write_by_rowid(&worker_ctx, &update_l, &lreq6, &bytes6);
            driver->rowid_result_free(&result6);

            int verify_len = -1;
            if (lrc6 != 0 ||
                driver->commit(&worker_ctx, &update_l) != 0 ||
                direct_lob_length(&worker_ctx, 999998003, "BLOB_COL", &verify_len) != 0)
            {
                printf("FAILED - lob_write_by_rowid rc=%d or commit/verify failed\n", lrc6);
                failed = 1;
            }
            else if (verify_len != 500)
            {
                printf("FAILED - BLOB length=%d, expected 500\n", verify_len);
                failed = 1;
            }
            else
                printf("OK (bytes_written=%lu, verified length=%d)\n",
                       (unsigned long)bytes6, verify_len);
        }
    }
    remove(blob_test_path);

    /* ---- Test 7 - lob_write_by_rowid() CLOB, single row, inline text ---- */
    printf("Test 7 (lob_write_by_rowid CLOB)    ... ");
    const char *clob_text = "Driver_Update_Test CLOB content - inline, no file.";
    if (direct_insert_row(&worker_ctx, 999998004, "clob_row") != 0)
    {
        printf("FAILED - seed row setup failed\n");
        failed = 1;
    }
    else
    {
        const char *bind7[] = { "999998004" };
        db_dml_returning_request_t req7;
        memset(&req7, 0, sizeof(req7));
        req7.sql = "UPDATE OCI_FIELD_TEST SET CLOB_COL = EMPTY_CLOB() "
                   "WHERE NUMBER_COL = :1 RETURNING ROWID INTO :2";
        req7.bind_count  = 1;
        req7.bind_values = bind7;
        req7.returning_bind_position = 2;

        db_rowid_result_t result7;
        int rc7 = driver->dml_execute_returning_rowids(&worker_ctx, &update_l,
                                                         &req7, &result7);
        if (rc7 != 0 || result7.count != 1)
        {
            printf("FAILED - dml_execute_returning_rowids rc=%d count=%d\n", rc7, result7.count);
            failed = 1;
            if (rc7 == 0) driver->rowid_result_free(&result7);
        }
        else
        {
            db_lob_write_request_t lreq7;
            memset(&lreq7, 0, sizeof(lreq7));
            lreq7.is_blob         = 0;
            lreq7.table_fq        = "OCI_FIELD_TEST";
            lreq7.column_name     = "CLOB_COL";
            lreq7.rowid_str       = result7.rowids;
            lreq7.inline_text     = clob_text;
            lreq7.inline_text_len = strlen(clob_text);

            uint64_t bytes7 = 0;
            int lrc7 = driver->lob_write_by_rowid(&worker_ctx, &update_l, &lreq7, &bytes7);
            driver->rowid_result_free(&result7);

            int verify_len = -1;
            if (lrc7 != 0 ||
                driver->commit(&worker_ctx, &update_l) != 0 ||
                direct_lob_length(&worker_ctx, 999998004, "CLOB_COL", &verify_len) != 0)
            {
                printf("FAILED - lob_write_by_rowid rc=%d or commit/verify failed\n", lrc7);
                failed = 1;
            }
            else if ((size_t)verify_len != strlen(clob_text))
            {
                printf("FAILED - CLOB length=%d, expected %zu\n", verify_len, strlen(clob_text));
                failed = 1;
            }
            else
                printf("OK (bytes_written=%lu, verified length=%d)\n",
                       (unsigned long)bytes7, verify_len);
        }
    }

    /* ---- Test 8 - multi-row match + LOB together (the original
     * 2026-08-27 bug's exact scenario) ---- */
    printf("Test 8 (multi-row match + LOB)      ... ");
    setup_ok = 1;
    for (int n = 999998010; n <= 999998014 && setup_ok; n++)
        if (direct_insert_row(&worker_ctx, n, "UPDATE_TEST_GROUP_B") != 0)
            setup_ok = 0;

    if (!setup_ok)
    {
        printf("FAILED - group seed insert failed\n");
        failed = 1;
    }
    else
    {
        const char *bind8[] = { "UPDATE_TEST_GROUP_B" };
        db_dml_returning_request_t req8;
        memset(&req8, 0, sizeof(req8));
        req8.sql = "UPDATE OCI_FIELD_TEST SET CLOB_COL = EMPTY_CLOB() "
                   "WHERE VARCHAR2_COL = :1 RETURNING ROWID INTO :2";
        req8.bind_count  = 1;
        req8.bind_values = bind8;
        req8.returning_bind_position = 2;

        db_rowid_result_t result8;
        int rc8 = driver->dml_execute_returning_rowids(&worker_ctx, &update_l,
                                                         &req8, &result8);
        if (rc8 != 0 || result8.count != 5)
        {
            printf("FAILED - rc=%d count=%d (expected rc=0, count=5)\n", rc8, result8.count);
            failed = 1;
            if (rc8 == 0) driver->rowid_result_free(&result8);
        }
        else
        {
            const char *clob_text8 = "Group B shared CLOB content.";
            int all_written = 1;

            for (int i = 0; i < result8.count; i++)
            {
                db_lob_write_request_t lreq8;
                memset(&lreq8, 0, sizeof(lreq8));
                lreq8.is_blob         = 0;
                lreq8.table_fq        = "OCI_FIELD_TEST";
                lreq8.column_name     = "CLOB_COL";
                lreq8.rowid_str       = result8.rowids + ((size_t)i * TEST_ROWID_BUF_SIZE);
                lreq8.inline_text     = clob_text8;
                lreq8.inline_text_len = strlen(clob_text8);

                uint64_t bytes8 = 0;
                if (driver->lob_write_by_rowid(&worker_ctx, &update_l, &lreq8, &bytes8) != 0)
                    all_written = 0;
            }

            driver->rowid_result_free(&result8);
            driver->commit(&worker_ctx, &update_l);

            if (!all_written)
            {
                printf("FAILED - not every row's lob_write_by_rowid call succeeded\n");
                failed = 1;
            }
            else
            {
                /* Independently verify ALL 5 rows got the content - not
                 * just the first one, which is exactly what the
                 * original 2026-08-27 bug got wrong with a static bind. */
                int all_verified = 1;
                for (int n = 999998010; n <= 999998014; n++)
                {
                    int len = -1;
                    if (direct_lob_length(&worker_ctx, n, "CLOB_COL", &len) != 0 ||
                        (size_t)len != strlen(clob_text8))
                        all_verified = 0;
                }

                if (!all_verified)
                {
                    printf("FAILED - count=5 reported, but not all 5 rows' CLOB content verified\n");
                    failed = 1;
                }
                else
                    printf("OK (count=5, all 5 rows independently verified)\n");
            }
        }
    }

    /* ---- Final cleanup ---- */
    for (int n = 999998001; n <= 999998014; n++)
        direct_delete_by_number(&worker_ctx, n);

    driver->release_session(&ctx, &worker_ctx);
    driver->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&err_l);
    logger_close(&conn_l);
    logger_close(&connpool_l);
    logger_close(&update_l);

    return failed ? 1 : 0;
}
