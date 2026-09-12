/*
 * Driver_Select_Test.c
 *
 * Validates db_driver_t's new select_open()/select_fetch_batch()/
 * select_close() cursor against a real, already-populated table -
 * OCI_LOB_TEST (see Create_Oracle_Test_Table.txt: ID NUMBER,
 * DESCRIPTION VARCHAR2, FILE_NAME VARCHAR2, PHOTO BLOB - 6 rows already
 * loaded by the project's own photo-loading script). Chosen specifically
 * because it has real data AND a real BLOB column, so it can exercise
 * both the scalar happy path and the LOB-rejection path against actual
 * rows rather than a synthetic fixture.
 *
 * Connects via the pool (matches config.ini's use_connection_pool=1,
 * same as Driver_Pool_Test.c) and runs everything against a borrowed
 * worker_ctx - the same shape execute_query_batch() is actually called
 * with in production, not a bare direct connection.
 *
 * What it does:
 *   Test 1 - Direct row count: SELECT COUNT(*) FROM OCI_LOB_TEST via
 *            plain OCI calls (OCIStmtPrepare2/OCIDefineByPos/
 *            OCIStmtExecute), independent of anything in driver_oracle.c -
 *            an authoritative "how many rows are really there" baseline
 *            to check the cursor against, same reasoning as
 *            Driver_Connect_Test.c's own direct-vs-driver A/B shape.
 *
 *   Test 2 - Scalar cursor: select_open() on
 *            "SELECT ID, DESCRIPTION, FILE_NAME FROM OCI_LOB_TEST
 *             ORDER BY ID" with fetch_array_size=2 - deliberately
 *            smaller than the known 6-row table, forcing at least 3
 *            separate select_fetch_batch() calls so the loop itself
 *            (not just a single batch) is actually exercised. Prints
 *            every row fetched, sums the total across all batches, and
 *            checks that total against Test 1's independent count.
 *
 *   Test 3 - LOB rejection: select_open() on "SELECT * FROM OCI_LOB_TEST"
 *            (includes the real PHOTO BLOB column) must return exactly
 *            DB_SELECT_UNSUPPORTED_LOB, with *out_cursor left NULL -
 *            proves the v1 scalar-only boundary actually holds against
 *            a table that really does have a LOB column, not just an
 *            absence of one.
 *
 * KNOWN GAP, not fixed by this test: select_open()'s
 * db_select_request_t.max_rows/max_memory_bytes/query_timeout are
 * accepted but NOT YET ENFORCED by driver_oracle.c in this pass - see
 * db_driver.h's "hot point #1" from the cut-point analysis (the
 * COUNT(*)-wrapper row-count guard is genuinely Oracle-dialect-specific
 * and still needs its own design decision, deliberately deferred rather
 * than ported as-is). This test does not exercise that boundary at all
 * - OCI_LOB_TEST's 6 rows are comfortably under any real limit anyway.
 *
 * Build (same convention as Driver_Pool_Test.c - adds
 * OCI_Table_Metadata_Module.c and OCI_Resultset_Builder.c, both newly
 * pulled in by driver_oracle.c's select implementation; neither has any
 * further external dependency beyond what's already linked - checked
 * against their own #include lists before adding, not assumed):
 *
 *   gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
 *       -I/usr/include/cjson -I/usr/include/libxml2 \
 *       -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include -I. \
 *       -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
 *       -o Driver_Select_Test \
 *       Driver_Select_Test.c db_driver.c driver_oracle.c \
 *       OCI_Connection.c OCI_Connection_Pool.c oci_cache.c string_utils.c \
 *       ini_reader.c logger.c metrics.c ctx_utils.c \
 *       OCI_Table_Metadata_Module.c OCI_Resultset_Builder.c \
 *       -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
 *       -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson \
 *       -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
 *
 * Run (same LD_LIBRARY_PATH/LSAN_OPTIONS pattern as Run_Manually.sh):
 *   ./Driver_Select_Test
 *
 * Expected output on success:
 *   Test 1 (direct COUNT(*))            ... OK (6 rows)
 *   Test 2 (scalar cursor select_open)  ... OK (3 columns, batch_size=2)
 *     [row 1] ID=1 DESCRIPTION='Adams pizza' FILE_NAME='Adam_1.jpg'
 *     ... (6 rows printed) ...
 *   Test 2 total rows fetched           ... OK (6, matches Test 1)
 *   Test 3 (LOB rejection)              ... OK (DB_SELECT_UNSUPPORTED_LOB, cursor NULL)
 *   PASS
 *
 * Vendor-internal leak notes: same accepted category as every previous
 * harness in this series - not chased here, see Driver_Connect_Test.c's
 * header for the full reasoning.
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
#include "OCI_Resultset_Builder.h"

/* Local OCI error macro, same shape as driver_oracle.c's own
 * ORACLE_CHECK_OCI - this file needs its own copy for Test 1's direct
 * OCI count query, which is deliberately independent of anything in
 * driver_oracle.c (see header comment). */
#define TEST_CHECK_OCI(ctx, status) \
    do { \
        if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
            text errbuf[512]; sb4 errcode = 0; \
            OCIErrorGet((ctx)->errhp, 1, NULL, &errcode, errbuf, \
                        sizeof(errbuf), OCI_HTYPE_ERROR); \
            logger_write((ctx)->select_logger, LOG_ERROR, __func__, 0, \
                         "OCI Error %d: %s", errcode, (char *)errbuf); \
        } \
    } while (0)

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *connection_logger,
                     logger_t *connectionpool_logger, logger_t *select_logger)
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

    if (logger_init_str2(select_logger, config->select_log_file_name,
                          config->select_log_file_max_size,
                          config->select_log_file_rotation_number,
                          config->select_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init select_logger\n"); return -1; }
    ctx->select_logger = select_logger;

    return 0;
}

/* Test 1 - independent of driver_oracle.c entirely, deliberately plain
 * OCI, to give Test 2 something authoritative to check itself against. */
static int direct_row_count(oci_context_t *ctx, const char *table, int *out_count)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);

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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, connpool_l, select_l;
    if (init_ctx(&ctx, &config, &err_l, &conn_l, &connpool_l, &select_l) != 0)
    {
        printf("INIT FAILED\n");
        return 1;
    }

    const db_driver_t *driver = db_driver_get(&ctx);
    if (!driver || !driver->connect || !driver->get_session ||
        !driver->select_open || !driver->select_fetch_batch ||
        !driver->select_close || !driver->release_session || !driver->disconnect)
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

    /* ---- Test 1 ---- */
    int expected_count = -1;
    printf("Test 1 (direct COUNT(*))            ... ");
    if (direct_row_count(&worker_ctx, "OCI_LOB_TEST", &expected_count) != 0)
    {
        printf("FAILED\n");
        failed = 1;
    }
    else
        printf("OK (%d rows)\n", expected_count);

    /* ---- Test 2 ---- */
    db_select_request_t req2;
    memset(&req2, 0, sizeof(req2));
    req2.sql              = "SELECT ID, DESCRIPTION, FILE_NAME FROM OCI_LOB_TEST ORDER BY ID";
    req2.fetch_array_size  = 2;   /* deliberately smaller than the known 6 rows */

    db_select_cursor_t *cursor2      = NULL;
    db_column_meta_t   *columns2     = NULL;
    int                  col_count2  = 0;
    int                  batch_size2 = 0;

    printf("Test 2 (scalar cursor select_open)  ... ");
    int open_rc2 = driver->select_open(&worker_ctx, &req2, &cursor2,
                                        &columns2, &col_count2, &batch_size2);

    int total_rows2 = 0;

    if (open_rc2 != 0)
    {
        printf("FAILED - select_open rc=%d\n", open_rc2);
        failed = 1;
    }
    else
    {
        printf("OK (%d columns, batch_size=%d)\n", col_count2, batch_size2);

        for (int c = 0; c < col_count2; c++)
            printf("  col[%d] %-20s %s\n", c, columns2[c].field_name, columns2[c].field_type);

        for (;;)
        {
            resultset_t *rs = NULL;
            int rows_fetched = 0;

            if (driver->select_fetch_batch(cursor2, &rs, &rows_fetched) != 0)
            {
                printf("  FAILED - select_fetch_batch\n");
                failed = 1;
                break;
            }

            if (rows_fetched == 0)
                break;   /* cursor exhausted */

            for (int r = 0; r < rows_fetched; r++)
            {
                resultset_row_t *row = &rs->records[r];
                printf("  [row %d]", total_rows2 + r + 1);
                for (int f = 0; f < row->field_count; f++)
                    printf(" %s='%s'", row->fields[f].field_name, row->fields[f].value);
                printf("\n");
            }

            total_rows2 += rows_fetched;
            resultset_free(rs);
        }

        driver->select_close(cursor2);
        free(columns2);

        printf("Test 2 total rows fetched           ... ");
        if (!failed && expected_count >= 0 && total_rows2 == expected_count)
            printf("OK (%d, matches Test 1)\n", total_rows2);
        else
        {
            printf("FAILED - got %d, expected %d\n", total_rows2, expected_count);
            failed = 1;
        }
    }

    /* ---- Test 3 ---- */
    db_select_request_t req3;
    memset(&req3, 0, sizeof(req3));
    req3.sql             = "SELECT * FROM OCI_LOB_TEST";   /* includes PHOTO BLOB */
    req3.fetch_array_size = 2;

    db_select_cursor_t *cursor3     = NULL;
    db_column_meta_t   *columns3    = NULL;
    int                  col_count3 = 0;
    int                  batch_size3 = 0;

    printf("Test 3 (LOB rejection)              ... ");
    int open_rc3 = driver->select_open(&worker_ctx, &req3, &cursor3,
                                        &columns3, &col_count3, &batch_size3);

    if (open_rc3 == DB_SELECT_UNSUPPORTED_LOB && cursor3 == NULL)
        printf("OK (DB_SELECT_UNSUPPORTED_LOB, cursor NULL)\n");
    else
    {
        printf("FAILED - rc=%d cursor=%p (expected DB_SELECT_UNSUPPORTED_LOB, NULL)\n",
               open_rc3, (void *)cursor3);
        failed = 1;
        if (cursor3) driver->select_close(cursor3);
        if (columns3) free(columns3);
    }

    driver->release_session(&ctx, &worker_ctx);
    driver->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&err_l);
    logger_close(&conn_l);
    logger_close(&connpool_l);
    logger_close(&select_l);

    return failed ? 1 : 0;
}
