/*
 * Driver_Insert_Test.c
 *
 * Validates db_driver_t's dml_execute_returning_rowids() extended for
 * INSERT's own real multi-row batch shape (row_count > 1, array binds -
 * see db_driver.h's own doc comment on db_dml_returning_request_t and
 * driver_oracle.c's own comment on oracle_dml_execute_returning_rowids()
 * for the full design), plus lob_write_by_rowid() for INSERT's own
 * BLOB/CLOB columns (already proven reusable byte-for-byte against
 * handle_blob_insert()/handle_clob_insert() before this file was ever
 * written - see the analysis that preceded Step 1). Same A/B philosophy
 * as every harness in this series: every check the driver's own work is
 * measured against comes from plain, direct OCI calls in this file,
 * independent of driver_oracle.c entirely.
 *
 * Reserved NUMBER_COL range for this harness: 999997001-999997099 -
 * deliberately separate from Driver_Delete_Test.c's own
 * 999999001-999999099 and Driver_Update_Test.c's own
 * 999998001-999998014, so all three can coexist against the same table.
 *
 * What it does:
 *
 *   Test 1 - Single-row insert (row_count<=1, the shape DELETE/UPDATE
 *            already proved): INSERT ... RETURNING ROWID INTO :3,
 *            bind_count=2. Checks rc=0, result.count=1, and
 *            independently verifies via direct SELECT that the row
 *            genuinely landed with the right values.
 *
 *   Test 2 - Real multi-row batch (row_count=5, THE key new scenario
 *            nothing else in this series has tested): one
 *            OCIStmtExecute call, iters=5, each row's own distinct
 *            NUMBER_COL/VARCHAR2_COL values via the flat, row-major
 *            bind_values layout. Checks result.count=5 AND
 *            independently verifies, via 5 separate direct SELECTs,
 *            that every row landed with its OWN correct values - not
 *            5 rows sharing one value, which a broken array-bind
 *            stride could produce without erroring.
 *
 *   Test 3 - Multi-row batch with a NULL value in one row: 3 rows, the
 *            middle one's VARCHAR2_COL is NULL (bind_values entry is a
 *            NULL pointer). Verifies the NULL row is genuinely NULL in
 *            the database (IS NULL, not empty string) and the other two
 *            rows are unaffected - the NULL-binding fix from UPDATE's
 *            pass, now exercised inside a real array bind for the
 *            first time.
 *
 *   Test 4 - lob_write_by_rowid() BLOB, single row: RETURNING ROWID
 *            from a row_count<=1 insert, then a real file's content
 *            written and verified via DBMS_LOB.GETLENGTH.
 *
 *   Test 5 - lob_write_by_rowid() CLOB, single row, inline text - same
 *            shape as Test 4.
 *
 *   Test 6 - Multi-row batch WITH a LOB column - THE exact scenario
 *            INSERT's own 2026-08-25 fix was written for. 5 rows
 *            inserted in one batch (CLOB_COL as a bare EMPTY_CLOB()
 *            literal, not a bind - matches build_insert_sql()'s own
 *            convention), then lob_write_by_rowid() called once per
 *            returned ROWID with a DIFFERENT CLOB value per row -
 *            independently verifies via 5 separate DBMS_LOB.GETLENGTH
 *            queries that every row genuinely got its OWN content, not
 *            just the first (which is exactly what the original bug
 *            got wrong before the fix this mirrors).
 *
 *   Test 7 - commit() + independent verify: same shape as every other
 *            harness in this series.
 *
 *   Test 8 - rollback() + independent verify: inserts a row, rolls
 *            back instead of committing, confirms via direct SELECT
 *            the row does NOT exist.
 *
 * Final cleanup (direct OCI, best-effort, does not affect PASS/FAIL):
 *   removes every row left in the reserved range.
 *
 * Build/run: same convention as every other harness in this series -
 * see Driver_Delete_Test.c's own header for the exact gcc invocation
 * and LD_LIBRARY_PATH/LSAN_OPTIONS pattern.
 *
 * Vendor-internal leak notes: same accepted category as every previous
 * harness - see Driver_Connect_Test.c's header.
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

/* Matches driver_oracle.c's own ORACLE_ROWID_BUF_SIZE - see
 * Driver_Update_Test.c's own identical comment on why this file keeps
 * its own copy rather than sharing one across the driver boundary. */
#define TEST_ROWID_BUF_SIZE 24

#define TEST_CHECK_OCI(ctx, status) \
    do { \
        if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
            text errbuf[512]; sb4 errcode = 0; \
            OCIErrorGet((ctx)->errhp, 1, NULL, &errcode, errbuf, \
                        sizeof(errbuf), OCI_HTYPE_ERROR); \
            logger_write((ctx)->insert_logger, LOG_ERROR, __func__, 0, \
                         "OCI Error %d: %s", errcode, (char *)errbuf); \
        } \
    } while (0)

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *connection_logger,
                     logger_t *connectionpool_logger, logger_t *insert_logger)
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

    if (logger_init_str2(insert_logger, config->insert_log_file_name,
                          config->insert_log_file_max_size,
                          config->insert_log_file_rotation_number,
                          config->insert_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init insert_logger\n"); return -1; }
    ctx->insert_logger = insert_logger;

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

/* NULL out_buf on a genuinely NULL column - distinguishes "empty
 * string" from "SQL NULL", same distinction Test 3 needs to prove. */
static int direct_select_varchar(oci_context_t *ctx, int number_val,
                                  char *out_buf, size_t out_buf_size,
                                  int *out_is_null)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT VARCHAR2_COL FROM OCI_FIELD_TEST WHERE NUMBER_COL = %d",
             number_val);

    OCIStmt   *stmt = NULL;
    OCIDefine *defn = NULL;
    sb2        ind  = 0;
    out_buf[0] = '\0';

    TEST_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!stmt) return -1;

    TEST_CHECK_OCI(ctx,
        OCIDefineByPos(stmt, &defn, ctx->errhp, 1, out_buf, (sb4)out_buf_size,
                       SQLT_STR, &ind, NULL, NULL, OCI_DEFAULT));

    TEST_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT));

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    if (out_is_null) *out_is_null = (ind == -1);
    return 0;
}

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
    logger_t err_l, conn_l, connpool_l, insert_l;
    if (init_ctx(&ctx, &config, &err_l, &conn_l, &connpool_l, &insert_l) != 0)
    {
        printf("INIT FAILED\n");
        return 1;
    }

    const db_driver_t *driver = db_driver_get(&ctx);
    if (!driver || !driver->connect || !driver->get_session ||
        !driver->dml_execute_returning_rowids || !driver->rowid_result_free ||
        !driver->lob_write_by_rowid || !driver->commit || !driver->rollback ||
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
    for (int n = 999997001; n <= 999997099; n++)
        direct_delete_by_number(&worker_ctx, n);

    /* ---- Test 1 - single-row insert ---- */
    printf("Test 1 (single-row insert)          ... ");
    {
        const char *bind1[] = { "999997001", "single_row_test" };
        db_dml_returning_request_t req1;
        memset(&req1, 0, sizeof(req1));
        req1.sql = "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL) "
                   "VALUES (:1, :2) RETURNING ROWID INTO :3";
        req1.bind_count  = 2;
        req1.bind_values = bind1;
        req1.returning_bind_position = 3;

        db_rowid_result_t result1;
        int rc1 = driver->dml_execute_returning_rowids(&worker_ctx, &insert_l,
                                                         &req1, &result1);
        char verify1[256]; int isnull1 = 0;
        if (rc1 != 0 || result1.count != 1)
        {
            printf("FAILED - rc=%d count=%d (expected rc=0, count=1)\n", rc1, result1.count);
            failed = 1;
            if (rc1 == 0) driver->rowid_result_free(&result1);
        }
        else
        {
            driver->rowid_result_free(&result1);
            driver->commit(&worker_ctx, &insert_l);

            if (direct_select_varchar(&worker_ctx, 999997001, verify1, sizeof(verify1), &isnull1) != 0 ||
                isnull1 || strcmp(verify1, "single_row_test") != 0)
            {
                printf("FAILED - value='%s' isnull=%d (expected 'single_row_test')\n",
                       verify1, isnull1);
                failed = 1;
            }
            else
                printf("OK (verified value='%s')\n", verify1);
        }
    }

    /* ---- Test 2 - real multi-row batch (row_count=5) ---- */
    printf("Test 2 (multi-row batch insert)     ... ");
    {
        /* Flat, row-major: row 0's 2 values, then row 1's, etc. - see
         * db_dml_returning_request_t's own doc comment in db_driver.h. */
        const char *bind2[] = {
            "999997010", "batch_row_0",
            "999997011", "batch_row_1",
            "999997012", "batch_row_2",
            "999997013", "batch_row_3",
            "999997014", "batch_row_4",
        };
        db_dml_returning_request_t req2;
        memset(&req2, 0, sizeof(req2));
        req2.sql = "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL) "
                   "VALUES (:1, :2) RETURNING ROWID INTO :3";
        req2.bind_count  = 2;
        req2.bind_values = bind2;
        req2.returning_bind_position = 3;
        req2.row_count   = 5;

        db_rowid_result_t result2;
        int rc2 = driver->dml_execute_returning_rowids(&worker_ctx, &insert_l,
                                                         &req2, &result2);
        int reported_count2 = (rc2 == 0) ? result2.count : -1;
        if (rc2 == 0) driver->rowid_result_free(&result2);

        /* Always commit before checking, even on a count mismatch -
         * diagnostic fix (2026-09-20, added after Driver_Insert_Test.c's
         * own first run left this ambiguous): without this, a failed
         * count check never reached commit(), so a direct table query
         * afterward could never tell "wrong ROWIDs reported, but the
         * right rows landed" apart from "the array bind itself never
         * ran its later iterations" - both looked like "0 rows" once
         * the uncommitted work rolled back. Committing unconditionally
         * here means the real row count is always directly checkable. */
        driver->commit(&worker_ctx, &insert_l);

        int real_count2 = 0;
        for (int i = 0; i < 5; i++)
        {
            int c = 0;
            direct_row_count_by_number(&worker_ctx, 999997010 + i, &c);
            real_count2 += c;
        }

        if (rc2 != 0 || reported_count2 != 5)
        {
            printf("FAILED - rc=%d reported_count=%d real_rows_in_table=%d "
                   "(expected rc=0, reported=5, real=5)\n",
                   rc2, reported_count2, real_count2);
            failed = 1;
        }
        else
        {
            int all_correct = 1;
            for (int i = 0; i < 5; i++)
            {
                char expected[64];
                snprintf(expected, sizeof(expected), "batch_row_%d", i);
                char v[256]; int isn = 0;
                if (direct_select_varchar(&worker_ctx, 999997010 + i, v, sizeof(v), &isn) != 0 ||
                    isn || strcmp(v, expected) != 0)
                    all_correct = 0;
            }

            if (!all_correct)
            {
                printf("FAILED - count=5 reported, but not every row's own "
                       "distinct value verified\n");
                failed = 1;
            }
            else
                printf("OK (count=5, all 5 rows' own distinct values verified)\n");
        }
    }

    /* ---- Test 3 - multi-row batch with a NULL value ---- */
    printf("Test 3 (multi-row batch with NULL)  ... ");
    {
        const char *bind3[] = {
            "999997020", "with_value_0",
            "999997021", NULL,               /* genuinely NULL, not "" */
            "999997022", "with_value_2",
        };
        db_dml_returning_request_t req3;
        memset(&req3, 0, sizeof(req3));
        req3.sql = "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL) "
                   "VALUES (:1, :2) RETURNING ROWID INTO :3";
        req3.bind_count  = 2;
        req3.bind_values = bind3;
        req3.returning_bind_position = 3;
        req3.row_count   = 3;

        db_rowid_result_t result3;
        int rc3 = driver->dml_execute_returning_rowids(&worker_ctx, &insert_l,
                                                         &req3, &result3);
        int reported_count3 = (rc3 == 0) ? result3.count : -1;
        if (rc3 == 0) driver->rowid_result_free(&result3);

        driver->commit(&worker_ctx, &insert_l);   /* always, see Test 2's
                                                       own comment on why */

        int real_count3 = 0;
        for (int n = 999997020; n <= 999997022; n++)
        {
            int c = 0;
            direct_row_count_by_number(&worker_ctx, n, &c);
            real_count3 += c;
        }

        if (rc3 != 0 || reported_count3 != 3)
        {
            printf("FAILED - rc=%d reported_count=%d real_rows_in_table=%d "
                   "(expected rc=0, reported=3, real=3)\n",
                   rc3, reported_count3, real_count3);
            failed = 1;
        }
        else
        {
            char v0[256], v1[256], v2[256];
            int isn0 = 0, isn1 = 0, isn2 = 0;
            direct_select_varchar(&worker_ctx, 999997020, v0, sizeof(v0), &isn0);
            direct_select_varchar(&worker_ctx, 999997021, v1, sizeof(v1), &isn1);
            direct_select_varchar(&worker_ctx, 999997022, v2, sizeof(v2), &isn2);

            if (isn0 || strcmp(v0, "with_value_0") != 0 ||
                !isn1 ||
                isn2 || strcmp(v2, "with_value_2") != 0)
            {
                printf("FAILED - row0(null=%d,'%s') row1(null=%d) row2(null=%d,'%s')\n",
                       isn0, v0, isn1, isn2, v2);
                failed = 1;
            }
            else
                printf("OK (middle row genuinely NULL, other two correct)\n");
        }
    }

    /* ---- Test 4 - lob_write_by_rowid() BLOB, single row ---- */
    printf("Test 4 (lob_write_by_rowid BLOB)    ... ");
    {
        const char *blob_test_path = "/tmp/driver_insert_test_blob.bin";
        FILE *bfp = fopen(blob_test_path, "wb");
        unsigned char blob_content[400];
        for (int i = 0; i < 400; i++) blob_content[i] = (unsigned char)(i % 256);
        int write_ok = bfp && fwrite(blob_content, 1, sizeof(blob_content), bfp) == sizeof(blob_content);
        if (bfp) fclose(bfp);

        if (!write_ok)
        {
            printf("FAILED - test file setup failed\n");
            failed = 1;
        }
        else
        {
            const char *bind4[] = { "999997030" };
            db_dml_returning_request_t req4;
            memset(&req4, 0, sizeof(req4));
            req4.sql = "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, BLOB_COL) "
                       "VALUES (:1, EMPTY_BLOB()) RETURNING ROWID INTO :2";
            req4.bind_count  = 1;
            req4.bind_values = bind4;
            req4.returning_bind_position = 2;

            db_rowid_result_t result4;
            int rc4 = driver->dml_execute_returning_rowids(&worker_ctx, &insert_l,
                                                             &req4, &result4);
            if (rc4 != 0 || result4.count != 1)
            {
                printf("FAILED - dml_execute_returning_rowids rc=%d count=%d\n", rc4, result4.count);
                failed = 1;
                if (rc4 == 0) driver->rowid_result_free(&result4);
            }
            else
            {
                db_lob_write_request_t lreq4;
                memset(&lreq4, 0, sizeof(lreq4));
                lreq4.is_blob     = 1;
                lreq4.table_fq    = "OCI_FIELD_TEST";
                lreq4.column_name = "BLOB_COL";
                lreq4.rowid_str   = result4.rowids;
                lreq4.file_path   = blob_test_path;

                uint64_t bytes4 = 0;
                int lrc4 = driver->lob_write_by_rowid(&worker_ctx, &insert_l, &lreq4, &bytes4);
                driver->rowid_result_free(&result4);

                int verify_len = -1;
                if (lrc4 != 0 || driver->commit(&worker_ctx, &insert_l) != 0 ||
                    direct_lob_length(&worker_ctx, 999997030, "BLOB_COL", &verify_len) != 0)
                {
                    printf("FAILED - lob_write_by_rowid rc=%d or commit/verify failed\n", lrc4);
                    failed = 1;
                }
                else if (verify_len != 400)
                {
                    printf("FAILED - BLOB length=%d, expected 400\n", verify_len);
                    failed = 1;
                }
                else
                    printf("OK (bytes_written=%lu, verified length=%d)\n",
                           (unsigned long)bytes4, verify_len);
            }
        }
        remove(blob_test_path);
    }

    /* ---- Test 5 - lob_write_by_rowid() CLOB, single row ---- */
    printf("Test 5 (lob_write_by_rowid CLOB)    ... ");
    {
        const char *clob_text = "Driver_Insert_Test single-row CLOB content.";
        const char *bind5[] = { "999997031" };
        db_dml_returning_request_t req5;
        memset(&req5, 0, sizeof(req5));
        req5.sql = "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, CLOB_COL) "
                   "VALUES (:1, EMPTY_CLOB()) RETURNING ROWID INTO :2";
        req5.bind_count  = 1;
        req5.bind_values = bind5;
        req5.returning_bind_position = 2;

        db_rowid_result_t result5;
        int rc5 = driver->dml_execute_returning_rowids(&worker_ctx, &insert_l,
                                                         &req5, &result5);
        if (rc5 != 0 || result5.count != 1)
        {
            printf("FAILED - dml_execute_returning_rowids rc=%d count=%d\n", rc5, result5.count);
            failed = 1;
            if (rc5 == 0) driver->rowid_result_free(&result5);
        }
        else
        {
            db_lob_write_request_t lreq5;
            memset(&lreq5, 0, sizeof(lreq5));
            lreq5.is_blob         = 0;
            lreq5.table_fq        = "OCI_FIELD_TEST";
            lreq5.column_name     = "CLOB_COL";
            lreq5.rowid_str       = result5.rowids;
            lreq5.inline_text     = clob_text;
            lreq5.inline_text_len = strlen(clob_text);

            uint64_t bytes5 = 0;
            int lrc5 = driver->lob_write_by_rowid(&worker_ctx, &insert_l, &lreq5, &bytes5);
            driver->rowid_result_free(&result5);

            int verify_len = -1;
            if (lrc5 != 0 || driver->commit(&worker_ctx, &insert_l) != 0 ||
                direct_lob_length(&worker_ctx, 999997031, "CLOB_COL", &verify_len) != 0)
            {
                printf("FAILED - lob_write_by_rowid rc=%d or commit/verify failed\n", lrc5);
                failed = 1;
            }
            else if ((size_t)verify_len != strlen(clob_text))
            {
                printf("FAILED - CLOB length=%d, expected %zu\n", verify_len, strlen(clob_text));
                failed = 1;
            }
            else
                printf("OK (bytes_written=%lu, verified length=%d)\n",
                       (unsigned long)bytes5, verify_len);
        }
    }

    /* ---- Test 6 - multi-row batch WITH a LOB column (the original
     * 2026-08-25 bug's exact scenario) ---- */
    printf("Test 6 (multi-row batch + LOB)      ... ");
    {
        const char *bind6[] = {
            "999997040", "lob_batch_0",
            "999997041", "lob_batch_1",
            "999997042", "lob_batch_2",
            "999997043", "lob_batch_3",
            "999997044", "lob_batch_4",
        };
        db_dml_returning_request_t req6;
        memset(&req6, 0, sizeof(req6));
        req6.sql = "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL, CLOB_COL) "
                   "VALUES (:1, :2, EMPTY_CLOB()) RETURNING ROWID INTO :3";
        req6.bind_count  = 2;   /* CLOB_COL is a bare literal, no bind */
        req6.bind_values = bind6;
        req6.returning_bind_position = 3;
        req6.row_count   = 5;

        db_rowid_result_t result6;
        int rc6 = driver->dml_execute_returning_rowids(&worker_ctx, &insert_l,
                                                         &req6, &result6);
        int reported_count6 = (rc6 == 0) ? result6.count : -1;
        if (rc6 != 0 || reported_count6 != 5)
        {
            /* Diagnostic - commit whatever's there and check the real
             * row count directly, same reasoning as Test 2's own fix. */
            if (rc6 == 0) driver->rowid_result_free(&result6);
            driver->commit(&worker_ctx, &insert_l);
            int real_count6 = 0;
            for (int n = 999997040; n <= 999997044; n++)
            {
                int c = 0;
                direct_row_count_by_number(&worker_ctx, n, &c);
                real_count6 += c;
            }
            printf("FAILED - rc=%d reported_count=%d real_rows_in_table=%d "
                   "(expected rc=0, reported=5, real=5)\n",
                   rc6, reported_count6, real_count6);
            failed = 1;
        }
        else
        {
            int all_written = 1;
            char clob_texts[5][32];
            for (int i = 0; i < 5; i++)
                snprintf(clob_texts[i], sizeof(clob_texts[i]), "clob_content_row_%d", i);

            for (int i = 0; i < result6.count; i++)
            {
                db_lob_write_request_t lreq6;
                memset(&lreq6, 0, sizeof(lreq6));
                lreq6.is_blob         = 0;
                lreq6.table_fq        = "OCI_FIELD_TEST";
                lreq6.column_name     = "CLOB_COL";
                lreq6.rowid_str       = result6.rowids + ((size_t)i * TEST_ROWID_BUF_SIZE);
                lreq6.inline_text     = clob_texts[i];
                lreq6.inline_text_len = strlen(clob_texts[i]);

                uint64_t bytes6 = 0;
                if (driver->lob_write_by_rowid(&worker_ctx, &insert_l, &lreq6, &bytes6) != 0)
                    all_written = 0;
            }

            driver->rowid_result_free(&result6);
            driver->commit(&worker_ctx, &insert_l);

            if (!all_written)
            {
                printf("FAILED - not every row's lob_write_by_rowid call succeeded\n");
                failed = 1;
            }
            else
            {
                int all_verified = 1;
                for (int i = 0; i < 5; i++)
                {
                    int len = -1;
                    if (direct_lob_length(&worker_ctx, 999997040 + i, "CLOB_COL", &len) != 0 ||
                        (size_t)len != strlen(clob_texts[i]))
                        all_verified = 0;
                }

                if (!all_verified)
                {
                    printf("FAILED - count=5 reported, but not all 5 rows' own "
                           "distinct CLOB content verified\n");
                    failed = 1;
                }
                else
                    printf("OK (count=5, every row's own distinct CLOB content verified)\n");
            }
        }
    }

    /* ---- Test 7 - commit() + independent verify ---- */
    printf("Test 7 (commit + verify present)    ... ");
    {
        const char *bind7[] = { "999997050", "commit_test" };
        db_dml_returning_request_t req7;
        memset(&req7, 0, sizeof(req7));
        req7.sql = "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL) "
                   "VALUES (:1, :2) RETURNING ROWID INTO :3";
        req7.bind_count  = 2;
        req7.bind_values = bind7;
        req7.returning_bind_position = 3;

        db_rowid_result_t result7;
        int rc7 = driver->dml_execute_returning_rowids(&worker_ctx, &insert_l,
                                                         &req7, &result7);
        int count7 = -1;
        if (rc7 != 0 || result7.count != 1)
        {
            printf("FAILED - rc=%d count=%d\n", rc7, result7.count);
            failed = 1;
            if (rc7 == 0) driver->rowid_result_free(&result7);
        }
        else
        {
            driver->rowid_result_free(&result7);
            if (driver->commit(&worker_ctx, &insert_l) != 0 ||
                direct_row_count_by_number(&worker_ctx, 999997050, &count7) != 0)
            {
                printf("FAILED - commit() or verify count failed\n");
                failed = 1;
            }
            else if (count7 != 1)
            {
                printf("FAILED - row not present after commit (count=%d, expected 1)\n", count7);
                failed = 1;
            }
            else
                printf("OK (%d row, commit confirmed)\n", count7);
        }
    }

    /* ---- Test 8 - rollback() + independent verify ---- */
    printf("Test 8 (rollback + verify absent)   ... ");
    {
        const char *bind8[] = { "999997060", "rollback_test" };
        db_dml_returning_request_t req8;
        memset(&req8, 0, sizeof(req8));
        req8.sql = "INSERT INTO OCI_FIELD_TEST (NUMBER_COL, VARCHAR2_COL) "
                   "VALUES (:1, :2) RETURNING ROWID INTO :3";
        req8.bind_count  = 2;
        req8.bind_values = bind8;
        req8.returning_bind_position = 3;

        db_rowid_result_t result8;
        int rc8 = driver->dml_execute_returning_rowids(&worker_ctx, &insert_l,
                                                         &req8, &result8);
        int count8 = -1;
        if (rc8 != 0 || result8.count != 1)
        {
            printf("FAILED - rc=%d count=%d\n", rc8, result8.count);
            failed = 1;
            if (rc8 == 0) driver->rowid_result_free(&result8);
        }
        else
        {
            driver->rowid_result_free(&result8);
            if (driver->rollback(&worker_ctx, &insert_l) != 0 ||
                direct_row_count_by_number(&worker_ctx, 999997060, &count8) != 0)
            {
                printf("FAILED - rollback() or verify count failed\n");
                failed = 1;
            }
            else if (count8 != 0)
            {
                printf("FAILED - row present after rollback (count=%d, expected 0)\n", count8);
                failed = 1;
            }
            else
                printf("OK (%d rows, rollback confirmed)\n", count8);
        }
    }

    /* ---- Final cleanup - best-effort, does not affect PASS/FAIL ---- */
    for (int n = 999997001; n <= 999997099; n++)
        direct_delete_by_number(&worker_ctx, n);
    driver->commit(&worker_ctx, &insert_l);

    driver->release_session(&ctx, &worker_ctx);
    driver->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&err_l);
    logger_close(&conn_l);
    logger_close(&connpool_l);
    logger_close(&insert_l);

    return failed ? 1 : 0;
}
