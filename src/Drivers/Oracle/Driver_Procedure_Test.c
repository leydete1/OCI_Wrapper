/*
 * Driver_Procedure_Test.c
 *
 * Validates db_driver_t's dml_execute_procedure()/select_open_from_cursor()
 * against real stored procedures already in the schema (see
 * Create_Oracle_Test_Table.txt / Create_Unit_Test_Table.txt for their
 * own CREATE PROCEDURE text) - no new procedures created by this
 * harness. Same A/B philosophy as every harness in this series: every
 * check the driver's own work is measured against comes from plain,
 * direct OCI calls or independently-verifiable output, not just the
 * driver's own return codes.
 *
 * Genuinely different from every other harness in this series in one
 * respect: select_open_from_cursor()/select_fetch_batch()/select_close()
 * are ALSO exercised here (not just dml_execute_procedure()) - a
 * CURSOR OUT parameter only proves anything once actually fetched, so
 * this harness reuses the same fetch loop shape Driver_Select_Test.c
 * already proved, layered on top of a procedure call instead of a
 * plain SELECT.
 *
 * Reserved NUMBER_COL range for this harness: 999996001-999996099 -
 * deliberately separate from Driver_Delete_Test.c (999999xxx),
 * Driver_Update_Test.c (999998xxx), and Driver_Insert_Test.c
 * (999997xxx), used against UNIT_TEST_FIELD_TEST specifically (the
 * table UNIT_TEST_SAMPLE_PROC/UNIT_TEST_CURSOR_PROC actually query).
 *
 * Known, deliberate coverage gaps - not silently skipped, flagged here
 * and in the accompanying message:
 *   - No existing procedure has a genuine IN_OUT parameter (every one
 *     found is IN/OUT only) - dml_execute_procedure()'s IN_OUT path is
 *     covered by direct code review (mirrors bind_parameters() exactly)
 *     but not empirically proven by this harness. Would need a small,
 *     dedicated test procedure to close - not created here without
 *     asking first.
 *   - PURGE_OLD_ROWS (a real procedure that performs its own internal
 *     DELETE + COMMIT) is deliberately NOT called here - it operates
 *     on the whole OCI_FIELD_TEST table with no reserved-range
 *     boundary, and running it automatically risks deleting rows other
 *     harnesses or fixtures depend on. "Procedures manage their own
 *     transactions, uninterfered with" is instead covered passively -
 *     every test below succeeds with zero commit()/rollback() calls
 *     from this harness at all, matching the module's own "no commit
 *     issued" design by construction, not by exercising a destructive
 *     procedure to prove it.
 *
 * What it does:
 *
 *   Test 1 - No-parameter procedure (UNIT_TEST_NOOP_PROC): confirms
 *            dml_execute_procedure() handles param_count=0 cleanly.
 *
 *   Test 2 - Scalar IN + scalar OUT (GET_DEPT_STATUS): a NUMBER_COL
 *            value known to have zero matches in OCI_FIELD_TEST,
 *            confirms P_STATUS reports 0 - independent of any other
 *            test/harness's own data in that table.
 *
 *   Test 3 - IN + scalar OUT + CURSOR OUT together, against a seeded
 *            row (UNIT_TEST_SAMPLE_PROC): seeds one row into
 *            UNIT_TEST_FIELD_TEST first (direct OCI, independent of
 *            the driver), calls the procedure, checks P_OUT_NUM =
 *            P_IN_NUM*2 (proves the driver's own OUT-value read-back
 *            is correct, not just "no error"), then fetches the
 *            returned cursor via select_open_from_cursor()/
 *            select_fetch_batch() and independently verifies exactly
 *            one row with the seeded row's own values.
 *
 *   Test 4 - IN + scalar OUT + CURSOR OUT together, cross-checked
 *            (GET_ROWS_WITH_STATUS against OCI_FIELD_TEST): fetches
 *            the cursor fully, counts the real rows returned, and
 *            confirms that count matches P_ROW_COUNT exactly - a
 *            genuine internal-consistency check between the scalar OUT
 *            path and the cursor-fetch path, not just "both didn't
 *            error".
 *
 *   Test 5 - Deliberately unopened CURSOR OUT (UNIT_TEST_CURSOR_PROC,
 *            P_OPEN_CURSOR=0): confirms out_cursor_handle is genuinely
 *            NULL and out_is_null=1 - a real, valid outcome, not an
 *            error (see that procedure's own comment).
 *
 *   Test 6 - Genuinely opened CURSOR OUT (UNIT_TEST_CURSOR_PROC,
 *            P_OPEN_CURSOR=1): confirms out_cursor_handle is non-NULL
 *            and the cursor fetches cleanly (row count not asserted
 *            precisely - NUMBER_COL=1 in UNIT_TEST_FIELD_TEST isn't
 *            this harness's own reserved data, so 0 or 1 rows are both
 *            valid; what matters here is the fetch mechanism itself
 *            working, not a specific count).
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
#include <strings.h>
#include <stdlib.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "OCI_Resultset_Builder.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

#define TEST_CHECK_OCI(ctx, status) \
    do { \
        if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
            text errbuf[512]; sb4 errcode = 0; \
            OCIErrorGet((ctx)->errhp, 1, NULL, &errcode, errbuf, \
                        sizeof(errbuf), OCI_HTYPE_ERROR); \
            logger_write((ctx)->procedure_logger, LOG_ERROR, __func__, 0, \
                         "OCI Error %d: %s", errcode, (char *)errbuf); \
        } \
    } while (0)

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *connection_logger,
                     logger_t *connectionpool_logger, logger_t *procedure_logger,
                     logger_t *select_logger, logger_t *metadata_logger)
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

    if (logger_init_str2(procedure_logger, config->procedure_log_file_name,
                          config->procedure_log_file_max_size,
                          config->procedure_log_file_rotation_number,
                          config->procedure_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init procedure_logger\n"); return -1; }
    ctx->procedure_logger = procedure_logger;

    /* Needed too - select_open_from_cursor()/select_fetch_batch()/
     * select_close() are the existing SELECT-side driver functions
     * (unchanged by this pass), and they use ctx->select_logger
     * internally, not the explicit logger parameter dml_execute_
     * procedure() takes. Missing here in this harness's first attempt -
     * found via "Logger is NULL" appearing mid-run rather than a crash,
     * since logger_write() itself is NULL-safe; the real symptom was
     * silence, not a segfault. */
    if (logger_init_str2(select_logger, config->select_log_file_name,
                          config->select_log_file_max_size,
                          config->select_log_file_rotation_number,
                          config->select_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init select_logger\n"); return -1; }
    ctx->select_logger = select_logger;

    /* Found on the SECOND real run (the first fix - select_logger/
     * procedure_logger explicitly on worker_ctx - was necessary but not
     * sufficient). get_multi_metadata() (OCI_Table_Metadata_Module.c),
     * called unconditionally inside the shared describe/define path
     * both select_open() and select_open_from_cursor() go through, logs
     * via ctx->Metadata_logger specifically - a THIRD, different named
     * logger field, confirmed directly against that function's own
     * real code rather than guessed - with plenty of unconditional
     * (not just error-path) INFO-level logging, unlike ORACLE_CHECK_OCI's
     * error-only pattern. Missing this meant "Logger is NULL" kept
     * firing even after the first fix, and both procedure_Data_Manager.log
     * and select_Data_Manager.log stayed empty regardless - neither is
     * where get_multi_metadata() actually writes. */
    if (logger_init_str2(metadata_logger, config->Metadata_log_file_name,
                          config->Metadata_log_file_max_size,
                          config->Metadata_log_file_rotation_number,
                          config->Metadata_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init metadata_logger\n"); return -1; }
    ctx->Metadata_logger = metadata_logger;

    return 0;
}

/* ---- Direct-OCI helpers, independent of driver_oracle.c entirely ---- */

static int direct_delete_unit_test_row(oci_context_t *ctx, int number_val)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "DELETE FROM UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = %d",
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

static int direct_insert_unit_test_row(oci_context_t *ctx, int number_val,
                                        const char *varchar_val)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO UNIT_TEST_FIELD_TEST (NUMBER_COL, VARCHAR2_COL) "
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

/* Fetches a cursor to exhaustion via the driver, counting rows and
 * optionally checking the first row's NUMBER_COL/VARCHAR2_COL against
 * expected values (pass expected_number=-1 to skip the value check and
 * just count). Returns 0 on success (regardless of row count), -1 on
 * any driver failure. */
static int fetch_cursor_and_verify(const db_driver_t *driver, oci_context_t *ctx,
                                    logger_t *logger, void *cursor_handle,
                                    int *out_row_count,
                                    int expected_number, const char *expected_varchar,
                                    int *out_value_matched)
{
    *out_row_count = 0;
    if (out_value_matched) *out_value_matched = 0;

    db_select_cursor_t *cur = NULL;
    db_column_meta_t   *columns = NULL;
    int column_count = 0, batch_size = 0;

    if (driver->select_open_from_cursor(ctx, cursor_handle, &cur,
                                         &columns, &column_count,
                                         &batch_size) != 0)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "select_open_from_cursor failed");
        return -1;
    }

    int total_rows = 0;
    int checked_first_row = 0;

    while (1)
    {
        resultset_t *rs = NULL;
        int rows_fetched = 0;
        db_fetch_batch_stats_t stats;
        memset(&stats, 0, sizeof(stats));

        if (driver->select_fetch_batch(cur, &rs, &rows_fetched, &stats) != 0)
        {
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "select_fetch_batch failed");
            driver->select_close(cur);
            free(columns);
            return -1;
        }

        if (rows_fetched == 0) { if (rs) resultset_free(rs); break; }

        if (!checked_first_row && expected_number >= 0 && rs)
        {
            resultset_row_t *row = resultset_get_row(rs, 1);
            if (row && out_value_matched)
            {
                int match = 1;
                for (int c = 0; c < column_count; c++)
                {
                    if (strcasecmp(columns[c].field_name, "NUMBER_COL") == 0)
                    {
                        char expbuf[32];
                        snprintf(expbuf, sizeof(expbuf), "%d", expected_number);
                        if (strcmp(row->fields[c].value, expbuf) != 0) match = 0;
                    }
                    else if (expected_varchar &&
                             strcasecmp(columns[c].field_name, "VARCHAR2_COL") == 0)
                    {
                        if (strcmp(row->fields[c].value, expected_varchar) != 0) match = 0;
                    }
                }
                *out_value_matched = match;
            }
            checked_first_row = 1;
        }

        total_rows += rows_fetched;
        if (rs) resultset_free(rs);

        if (rows_fetched < batch_size) break;
    }

    driver->select_close(cur);
    free(columns);
    *out_row_count = total_rows;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, connpool_l, proc_l, sel_l, meta_l;
    if (init_ctx(&ctx, &config, &err_l, &conn_l, &connpool_l, &proc_l, &sel_l, &meta_l) != 0)
    {
        printf("INIT FAILED\n");
        return 1;
    }

    const db_driver_t *driver = db_driver_get(&ctx);
    if (!driver || !driver->connect || !driver->get_session ||
        !driver->dml_execute_procedure || !driver->select_open_from_cursor ||
        !driver->select_fetch_batch || !driver->select_close ||
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

    /* OCI_Pool_get_session() only explicitly copies a handful of
     * fields onto worker_ctx (svchp/errhp/ini/the generic logger/etc) -
     * confirmed directly against its own real code, not assumed - none
     * of the NAMED loggers (select_logger, procedure_logger, ...) are
     * among them. Every other harness in this series never needed this
     * (their driver calls take an explicit logger parameter and never
     * touch ctx->X_logger internally), but select_open_from_cursor()/
     * select_fetch_batch()/select_close() are the ORIGINAL SELECT
     * functions, unchanged by this pass, and they read ctx->select_logger
     * directly - without this, ORACLE_CHECK_OCI's own error-path
     * logger_write() call would silently swallow whatever real OCI
     * error actually occurs (found via "Logger is NULL" appearing
     * instead of a real error message on this harness's second run -
     * the first sign something was being hidden, not that nothing was
     * wrong). The real production server does the equivalent of this
     * explicitly too - see OCI_Execute_Procedure_Module.h's own
     * comment referencing Test_XML_Runner.c's initialise_loggers() and
     * its "worker_ctx needing every logger explicitly copied" gotcha. */
    worker_ctx.select_logger    = &sel_l;
    worker_ctx.procedure_logger = &proc_l;
    worker_ctx.Metadata_logger  = &meta_l;

    /* ---- Idempotent pre-cleanup ---- */
    for (int n = 999996001; n <= 999996003; n++)
        direct_delete_unit_test_row(&worker_ctx, n);

    /* ---- Test 1 - no-parameter procedure ---- */
    printf("Test 1 (no-parameter procedure)     ... ");
    {
        db_proc_execute_request_t req1;
        memset(&req1, 0, sizeof(req1));
        req1.plsql_block = "BEGIN UNIT_TEST_NOOP_PROC; END;";
        req1.param_count = 0;
        req1.params      = NULL;

        int rc1 = driver->dml_execute_procedure(&worker_ctx, &proc_l, &req1);
        if (rc1 != 0)
        {
            printf("FAILED - rc=%d\n", rc1);
            failed = 1;
        }
        else
            printf("OK\n");
    }

    /* ---- Test 2 - scalar IN + scalar OUT, zero-match case ---- */
    printf("Test 2 (scalar IN + scalar OUT)     ... ");
    {
        db_proc_param_t params2[2];
        memset(params2, 0, sizeof(params2));
        params2[0].name      = "P_DEPT_ID";
        params2[0].type      = DB_PROC_TYPE_INT;
        params2[0].direction = DB_PROC_DIR_IN;
        params2[0].in_value  = "999996099";   /* reserved, guaranteed absent */
        char p_dept_id_buf[64];
        params2[0].out_value      = p_dept_id_buf;
        params2[0].out_value_size = sizeof(p_dept_id_buf);

        params2[1].name      = "P_STATUS";
        params2[1].type      = DB_PROC_TYPE_INT;
        params2[1].direction = DB_PROC_DIR_OUT;

        db_proc_execute_request_t req2;
        memset(&req2, 0, sizeof(req2));
        req2.plsql_block = "BEGIN GET_DEPT_STATUS(:P_DEPT_ID, :P_STATUS); END;";
        req2.param_count = 2;
        req2.params      = params2;

        int rc2 = driver->dml_execute_procedure(&worker_ctx, &proc_l, &req2);

        if (rc2 != 0 || params2[1].out_is_null || params2[1].out_int != 0)
        {
            printf("FAILED - rc=%d out_is_null=%d out_int=%d (expected rc=0, "
                   "not null, 0)\n", rc2, params2[1].out_is_null, params2[1].out_int);
            failed = 1;
        }
        else
            printf("OK (P_STATUS=%d)\n", params2[1].out_int);
    }

    /* ---- Test 3 - IN + scalar OUT + CURSOR OUT, seeded row ---- */
    printf("Test 3 (IN+OUT+CURSOR, seeded row)  ... ");
    {
        if (direct_insert_unit_test_row(&worker_ctx, 999996001, "sample_proc_test") != 0)
        {
            printf("FAILED - seed insert failed\n");
            failed = 1;
        }
        else
        {
            db_proc_param_t params3[3];
            memset(params3, 0, sizeof(params3));
            params3[0].name      = "P_IN_NUM";
            params3[0].type      = DB_PROC_TYPE_INT;
            params3[0].direction = DB_PROC_DIR_IN;
            params3[0].in_value  = "999996001";
            char p_in_num_buf[64];
            params3[0].out_value      = p_in_num_buf;
            params3[0].out_value_size = sizeof(p_in_num_buf);

            params3[1].name      = "P_OUT_NUM";
            params3[1].type      = DB_PROC_TYPE_INT;
            params3[1].direction = DB_PROC_DIR_OUT;

            params3[2].name      = "P_RESULTS";
            params3[2].type      = DB_PROC_TYPE_CURSOR;
            params3[2].direction = DB_PROC_DIR_OUT;

            db_proc_execute_request_t req3;
            memset(&req3, 0, sizeof(req3));
            req3.plsql_block = "BEGIN UNIT_TEST_SAMPLE_PROC(:P_IN_NUM, :P_OUT_NUM, "
                               ":P_RESULTS); END;";
            req3.param_count = 3;
            req3.params      = params3;

            int rc3 = driver->dml_execute_procedure(&worker_ctx, &proc_l, &req3);

            if (rc3 != 0 || params3[1].out_is_null ||
                params3[1].out_int != 999996001 * 2 ||
                params3[2].out_is_null || !params3[2].out_cursor_handle)
            {
                printf("FAILED - rc=%d P_OUT_NUM=%d (expected %d) "
                       "cursor_null=%d\n",
                       rc3, params3[1].out_int, 999996001 * 2,
                       params3[2].out_is_null);
                failed = 1;
            }
            else
            {
                int row_count = 0, value_matched = 0;
                if (fetch_cursor_and_verify(driver, &worker_ctx, &proc_l,
                                             params3[2].out_cursor_handle,
                                             &row_count, 999996001,
                                             "sample_proc_test",
                                             &value_matched) != 0)
                {
                    printf("FAILED - cursor fetch failed\n");
                    failed = 1;
                }
                else if (row_count != 1 || !value_matched)
                {
                    printf("FAILED - row_count=%d value_matched=%d "
                           "(expected 1, matched)\n", row_count, value_matched);
                    failed = 1;
                }
                else
                    printf("OK (P_OUT_NUM=%d, 1 row, values verified)\n",
                           params3[1].out_int);
            }
        }
    }

    /* ---- Test 4 - scalar OUT + CURSOR OUT cross-checked ---- */
    printf("Test 4 (scalar/cursor cross-check)  ... ");
    {
        db_proc_param_t params4[3];
        memset(params4, 0, sizeof(params4));
        params4[0].name      = "P_MIN_NUM";
        params4[0].type      = DB_PROC_TYPE_INT;
        params4[0].direction = DB_PROC_DIR_IN;
        params4[0].in_value  = "1";   /* deliberately low - matches
                                          whatever real data already
                                          exists in OCI_FIELD_TEST; this
                                          test checks internal
                                          consistency, not an exact
                                          count. */
        char p_min_num_buf[64];
        params4[0].out_value      = p_min_num_buf;
        params4[0].out_value_size = sizeof(p_min_num_buf);

        params4[1].name      = "P_ROW_COUNT";
        params4[1].type      = DB_PROC_TYPE_INT;
        params4[1].direction = DB_PROC_DIR_OUT;

        params4[2].name      = "P_RESULTS";
        params4[2].type      = DB_PROC_TYPE_CURSOR;
        params4[2].direction = DB_PROC_DIR_OUT;

        db_proc_execute_request_t req4;
        memset(&req4, 0, sizeof(req4));
        req4.plsql_block = "BEGIN GET_ROWS_WITH_STATUS(:P_MIN_NUM, "
                           ":P_ROW_COUNT, :P_RESULTS); END;";
        req4.param_count = 3;
        req4.params      = params4;

        int rc4 = driver->dml_execute_procedure(&worker_ctx, &proc_l, &req4);

        if (rc4 != 0 || params4[1].out_is_null || !params4[2].out_cursor_handle)
        {
            printf("FAILED - rc=%d P_ROW_COUNT_null=%d cursor_null=%d\n",
                   rc4, params4[1].out_is_null, params4[2].out_is_null);
            failed = 1;
        }
        else
        {
            int row_count = 0;
            if (fetch_cursor_and_verify(driver, &worker_ctx, &proc_l,
                                         params4[2].out_cursor_handle,
                                         &row_count, -1, NULL, NULL) != 0)
            {
                printf("FAILED - cursor fetch failed\n");
                failed = 1;
            }
            else if (row_count != params4[1].out_int)
            {
                printf("FAILED - cursor fetched %d rows, P_ROW_COUNT reported %d "
                       "(should match exactly)\n", row_count, params4[1].out_int);
                failed = 1;
            }
            else
                printf("OK (P_ROW_COUNT=%d, cursor row count matches exactly)\n",
                       params4[1].out_int);
        }
    }

    /* ---- Test 5 - deliberately unopened CURSOR OUT ---- */
    printf("Test 5 (deliberately unopened cursor) ... ");
    {
        db_proc_param_t params5[2];
        memset(params5, 0, sizeof(params5));
        params5[0].name      = "P_OPEN_CURSOR";
        params5[0].type      = DB_PROC_TYPE_INT;
        params5[0].direction = DB_PROC_DIR_IN;
        params5[0].in_value  = "0";
        char p_open_cursor_buf5[64];
        params5[0].out_value      = p_open_cursor_buf5;
        params5[0].out_value_size = sizeof(p_open_cursor_buf5);

        params5[1].name      = "P_RESULTS";
        params5[1].type      = DB_PROC_TYPE_CURSOR;
        params5[1].direction = DB_PROC_DIR_OUT;

        db_proc_execute_request_t req5;
        memset(&req5, 0, sizeof(req5));
        req5.plsql_block = "BEGIN UNIT_TEST_CURSOR_PROC(:P_OPEN_CURSOR, "
                           ":P_RESULTS); END;";
        req5.param_count = 2;
        req5.params      = params5;

        int rc5 = driver->dml_execute_procedure(&worker_ctx, &proc_l, &req5);

        if (rc5 != 0 || !params5[1].out_is_null || params5[1].out_cursor_handle)
        {
            printf("FAILED - rc=%d out_is_null=%d cursor_handle=%p "
                   "(expected rc=0, is_null=1, handle=NULL)\n",
                   rc5, params5[1].out_is_null, params5[1].out_cursor_handle);
            failed = 1;
        }
        else
            printf("OK (genuinely NULL, not an error)\n");
    }

    /* ---- Test 6 - genuinely opened CURSOR OUT ---- */
    printf("Test 6 (genuinely opened cursor)    ... ");
    {
        db_proc_param_t params6[2];
        memset(params6, 0, sizeof(params6));
        params6[0].name      = "P_OPEN_CURSOR";
        params6[0].type      = DB_PROC_TYPE_INT;
        params6[0].direction = DB_PROC_DIR_IN;
        params6[0].in_value  = "1";
        char p_open_cursor_buf6[64];
        params6[0].out_value      = p_open_cursor_buf6;
        params6[0].out_value_size = sizeof(p_open_cursor_buf6);

        params6[1].name      = "P_RESULTS";
        params6[1].type      = DB_PROC_TYPE_CURSOR;
        params6[1].direction = DB_PROC_DIR_OUT;

        db_proc_execute_request_t req6;
        memset(&req6, 0, sizeof(req6));
        req6.plsql_block = "BEGIN UNIT_TEST_CURSOR_PROC(:P_OPEN_CURSOR, "
                           ":P_RESULTS); END;";
        req6.param_count = 2;
        req6.params      = params6;

        int rc6 = driver->dml_execute_procedure(&worker_ctx, &proc_l, &req6);

        if (rc6 != 0 || params6[1].out_is_null || !params6[1].out_cursor_handle)
        {
            printf("FAILED - rc=%d out_is_null=%d cursor_handle=%p "
                   "(expected rc=0, is_null=0, non-NULL handle)\n",
                   rc6, params6[1].out_is_null, params6[1].out_cursor_handle);
            failed = 1;
        }
        else
        {
            int row_count = 0;
            if (fetch_cursor_and_verify(driver, &worker_ctx, &proc_l,
                                         params6[1].out_cursor_handle,
                                         &row_count, -1, NULL, NULL) != 0)
            {
                printf("FAILED - cursor fetch failed\n");
                failed = 1;
            }
            else
                printf("OK (cursor genuinely open, fetched cleanly, %d row(s))\n",
                       row_count);
        }
    }

    /* ---- Final cleanup ---- */
    for (int n = 999996001; n <= 999996003; n++)
        direct_delete_unit_test_row(&worker_ctx, n);

    driver->release_session(&ctx, &worker_ctx);
    driver->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&err_l);
    logger_close(&conn_l);
    logger_close(&connpool_l);
    logger_close(&proc_l);
    logger_close(&sel_l);
    logger_close(&meta_l);

    return failed ? 1 : 0;
}
