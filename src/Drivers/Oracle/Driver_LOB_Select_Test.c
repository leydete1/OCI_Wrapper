/*
 * Driver_LOB_Select_Test.c
 *
 * Validates v2's BLOB/CLOB support in db_driver_t's cursor
 * (select_open/select_fetch_batch/select_close) against real,
 * already-populated tables - not synthetic fixtures.
 *
 * Tables used:
 *   OCI_LOB_TEST  (ID, DESCRIPTION, FILE_NAME, PHOTO BLOB) - 6 rows,
 *     same table Driver_Select_Test.c already validated the scalar
 *     path against.
 *   OCI_CLOB_TEST (ID, DESCRIPTION, FILE_NAME, LARGE_CLOB CLOB) - 4
 *     rows (see Create_Oracle_Test_Table.txt) - matches the
 *     LARGE_CLOB_row1..row4 files already seen in real http_consumer
 *     output during the CLOB extraction's own before/after comparison.
 *   OCI_FIELD_TEST (has both BLOB_COL and CLOB_COL) - used only for
 *     Test 5's mixed-type memory-safety check, same WHERE NUMBER_COL
 *     IN (901, 902) query already exercised by the real http_consumer
 *     test suite (see select_Data_Manager.log from the CLOB
 *     before/after comparison) - not a synthetic query either.
 *
 * What it does:
 *
 *   Test 1 - direct COUNT(*) on OCI_LOB_TEST (same pattern as
 *            Driver_Select_Test.c) - baseline row count.
 *
 *   Test 2 - cursor select including the real PHOTO BLOB column,
 *            fetch_array_size=2. Since no CLOB is present, batch_size
 *            should NOT be downgraded - checks *out_batch_size == 2.
 *            For each row fetched, captures blob_detail.file_size and
 *            cross-checks it against an INDEPENDENT
 *            DBMS_LOB.GETLENGTH(PHOTO) query for that same row's ID,
 *            issued via plain OCI - not anything the driver touches.
 *            Confirms total rows fetched == Test 1's count.
 *
 *   Test 3 - direct COUNT(*) on OCI_CLOB_TEST - baseline row count
 *            (expect 4).
 *
 *   Test 4 - cursor select including the real LARGE_CLOB column,
 *            fetch_array_size=2. Since CLOB IS present, checks
 *            *out_batch_size == 1 (the array-fetch quirk downgrade -
 *            see db_driver.h/driver_oracle.c) even though 2 was
 *            requested. For each row, the CLOB field's value is either
 *            a URL or a local filepath depending on config - this test
 *            does not assume which: it stat()s the value as a
 *            filesystem path, and if that succeeds, compares the
 *            file's real size on disk against an INDEPENDENT
 *            DBMS_LOB.GETLENGTH(LARGE_CLOB) query for that row's ID.
 *            If stat() fails (config is sharing a URL, not a host
 *            path), only confirms the value is non-empty - the content
 *            check is skipped for that row rather than guessed at.
 *            Confirms total rows fetched == Test 3's count.
 *
 *   Test 5 - mixed BLOB+CLOB in the SAME query (OCI_FIELD_TEST,
 *            NUMBER_COL IN (901,902)) - specifically exercises the
 *            alloc_batch_size fix: a BLOB column's locator array gets
 *            allocated at the originally-requested batch size, but the
 *            CLOB column in the same query forces the actual fetch
 *            batch size down to 1 - cursor teardown must still free
 *            every locator slot that was actually allocated, not just
 *            however many ended up used. Checks *out_batch_size == 1,
 *            fetch loop completes without error, select_close() runs
 *            clean. (Correctness of the values themselves is already
 *            covered by Tests 2 and 4 individually - this is a
 *            memory-safety/leak check, best judged by the ASan summary
 *            at the end of the run, same as every other harness in
 *            this series.)
 *
 * Vendor-internal leak notes: same accepted category as every previous
 * harness - not chased here, see Driver_Connect_Test.c's header for the
 * full reasoning. What DOES matter for this run specifically: the
 * leaked byte/allocation count should NOT grow beyond that same
 * accepted baseline just because Test 5 ran a mixed BLOB+CLOB query -
 * if it does, the alloc_batch_size fix did not fully close the leak it
 * was meant to close.
 *
 * Build (same convention as Driver_Select_Test.c - identical file
 * list, this just replaces the compiled test .c; OCI_Blob_Utils.c/
 * OCI_Clob_Utils.c must both be present, same as the real build):
 *
 *   gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
 *       -I/usr/include/cjson -I/usr/include/libxml2 \
 *       -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include -I. \
 *       -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
 *       -o Driver_LOB_Select_Test \
 *       Driver_LOB_Select_Test.c db_driver.c driver_oracle.c \
 *       OCI_Connection.c OCI_Connection_Pool.c oci_cache.c string_utils.c \
 *       ini_reader.c logger.c metrics.c ctx_utils.c \
 *       OCI_Table_Metadata_Module.c OCI_Resultset_Builder.c \
 *       OCI_Blob_Utils.c OCI_Clob_Utils.c XML_Helper.c \
 *       -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
 *       -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson \
 *       -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
 *
 * Note: XML_Helper.c is now needed too - driver_oracle.c forward-
 * declares get_mime_type() rather than including XML_Helper.h (see
 * driver_oracle.c's own comment on why), but the real symbol still has
 * to come from somewhere at link time, and XML_Helper.c is where it's
 * actually defined.
 *
 * Run (same LD_LIBRARY_PATH/LSAN_OPTIONS pattern as Run_Manually.sh):
 *   ./Driver_LOB_Select_Test
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"
#include "OCI_Resultset_Builder.h"

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
    { fprintf(stderr, "Failed to load ini file: %s\n", CONFIG_INI); return -1; }

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

static int direct_row_count(oci_context_t *ctx, const char *table, int *out_count)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);

    OCIStmt   *stmt = NULL;
    OCIDefine *defn = NULL;
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

/* Independent, single-row lookup - "SELECT NVL(DBMS_LOB.GETLENGTH(%s),0)
 * FROM %s WHERE ID=%s" - used as the authoritative cross-check for both
 * Test 2 (BLOB) and Test 4 (CLOB). Deliberately plain OCI, nothing this
 * driver touches. */
static int direct_lob_length(oci_context_t *ctx, const char *table,
                              const char *lob_col, const char *id_value,
                              int *out_length)
{
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT NVL(DBMS_LOB.GETLENGTH(%s),0) FROM %s WHERE ID=%s",
             lob_col, table, id_value);

    OCIStmt   *stmt = NULL;
    OCIDefine *defn = NULL;
    int        length = 0;

    TEST_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!stmt) return -1;

    TEST_CHECK_OCI(ctx,
        OCIDefineByPos(stmt, &defn, ctx->errhp, 1, &length, sizeof(length),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT));

    TEST_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT));

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

    *out_length = length;
    return 0;
}

/* Drains a cursor completely, calling per_row_cb (if non-NULL) once per
 * fetched row with the row's field array and its ID column value
 * (assumed to be field 0). Returns total rows fetched, or -1 on error. */
typedef void (*row_cb_t)(resultset_row_t *row, const char *id_value, void *user_data);

static int drain_cursor(const db_driver_t *driver, db_select_cursor_t *cursor,
                         row_cb_t per_row_cb, void *user_data, int *failed)
{
    int total_rows = 0;
    for (;;)
    {
        resultset_t *rs = NULL;
        int rows_fetched = 0;

        if (driver->select_fetch_batch(cursor, &rs, &rows_fetched) != 0)
        {
            printf("  FAILED - select_fetch_batch\n");
            *failed = 1;
            return total_rows;
        }

        if (rows_fetched == 0)
            break;

        for (int r = 0; r < rows_fetched; r++)
        {
            resultset_row_t *row = &rs->records[r];
            const char *id_value = (row->field_count > 0) ? row->fields[0].value : "";

            if (per_row_cb)
                per_row_cb(row, id_value, user_data);
        }

        total_rows += rows_fetched;
        resultset_free(rs);
    }

    return total_rows;
}

/* ---- Test 2 per-row callback: BLOB size cross-check ---- */
struct blob_check_state { oci_context_t *ctx; int failed; int checked; };

static void blob_row_cb(resultset_row_t *row, const char *id_value, void *user_data)
{
    struct blob_check_state *st = (struct blob_check_state *)user_data;

    /* Columns: ID, DESCRIPTION, FILE_NAME, PHOTO - PHOTO is field 3 */
    if (row->field_count < 4 || !row->fields[3].is_blob)
    {
        printf("  [row id=%s] FAILED - expected field 3 to be a BLOB field\n", id_value);
        st->failed = 1;
        return;
    }

    int expected_len = -1;
    if (direct_lob_length(st->ctx, "OCI_LOB_TEST", "PHOTO", id_value, &expected_len) != 0)
    {
        printf("  [row id=%s] FAILED - direct_lob_length query failed\n", id_value);
        st->failed = 1;
        return;
    }

    uint64_t actual_len = row->fields[3].blob_detail.file_size;
    printf("  [row id=%s] PHOTO size=%llu expected=%d  %s\n",
           id_value, (unsigned long long)actual_len, expected_len,
           ((int64_t)actual_len == expected_len) ? "OK" : "MISMATCH");

    if ((int64_t)actual_len != expected_len)
        st->failed = 1;

    st->checked++;
}

/* ---- Test 4 per-row callback: CLOB length cross-check via stat() ---- */
struct clob_check_state { oci_context_t *ctx; int failed; int checked; int stat_skipped; };

static void clob_row_cb(resultset_row_t *row, const char *id_value, void *user_data)
{
    struct clob_check_state *st = (struct clob_check_state *)user_data;

    /* Columns: ID, DESCRIPTION, FILE_NAME, LARGE_CLOB - LARGE_CLOB is field 3 */
    if (row->field_count < 4)
    {
        printf("  [row id=%s] FAILED - expected 4 fields\n", id_value);
        st->failed = 1;
        return;
    }

    const char *value = row->fields[3].value;
    if (!value || value[0] == '\0')
    {
        printf("  [row id=%s] FAILED - CLOB field value is empty\n", id_value);
        st->failed = 1;
        return;
    }

    struct stat sb;
    if (stat(value, &sb) != 0)
    {
        printf("  [row id=%s] value='%s' - not a local path (URL sharing?), "
               "skipping content check\n", id_value, value);
        st->stat_skipped++;
        st->checked++;
        return;
    }

    int expected_len = -1;
    if (direct_lob_length(st->ctx, "OCI_CLOB_TEST", "LARGE_CLOB", id_value, &expected_len) != 0)
    {
        printf("  [row id=%s] FAILED - direct_lob_length query failed\n", id_value);
        st->failed = 1;
        return;
    }

    printf("  [row id=%s] file='%s' size=%lld expected=%d  %s\n",
           id_value, value, (long long)sb.st_size, expected_len,
           ((long long)sb.st_size == expected_len) ? "OK" : "MISMATCH");

    if ((long long)sb.st_size != expected_len)
        st->failed = 1;

    st->checked++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, connpool_l, select_l;
    if (init_ctx(&ctx, &config, &err_l, &conn_l, &connpool_l, &select_l) != 0)
    { printf("INIT FAILED\n"); return 1; }

    const db_driver_t *driver = db_driver_get(&ctx);
    if (!driver || !driver->connect || !driver->get_session ||
        !driver->select_open || !driver->select_fetch_batch ||
        !driver->select_close || !driver->release_session || !driver->disconnect)
    { printf("FAILED - incomplete driver\n"); return 1; }

    if (driver->connect(&ctx) != 0)
    { printf("FAILED - connect()\n"); return 1; }

    oci_context_t worker_ctx;
    memset(&worker_ctx, 0, sizeof(worker_ctx));
    if (driver->get_session(&ctx, &worker_ctx) != 0)
    { printf("FAILED - get_session()\n"); driver->disconnect(&ctx); return 1; }

    /* ---- Test 1 ---- */
    int lob_test_count = -1;
    printf("Test 1 (direct COUNT OCI_LOB_TEST)  ... ");
    if (direct_row_count(&worker_ctx, "OCI_LOB_TEST", &lob_test_count) != 0)
    { printf("FAILED\n"); failed = 1; }
    else printf("OK (%d rows)\n", lob_test_count);

    /* ---- Test 2 ---- */
    db_select_request_t req2 = {0};
    req2.sql              = "SELECT ID, DESCRIPTION, FILE_NAME, PHOTO FROM OCI_LOB_TEST ORDER BY ID";
    req2.fetch_array_size  = 2;

    db_select_cursor_t *cur2 = NULL;
    db_column_meta_t   *cols2 = NULL;
    int col_count2 = 0, batch_size2 = 0;

    printf("Test 2 (cursor select w/ BLOB)      ... ");
    int rc2 = driver->select_open(&worker_ctx, &req2, &cur2, &cols2, &col_count2, &batch_size2);
    if (rc2 != 0)
    { printf("FAILED - select_open rc=%d\n", rc2); failed = 1; }
    else
    {
        printf("OK (%d columns, batch_size=%d, expected 2 - no CLOB present)\n",
               col_count2, batch_size2);
        if (batch_size2 != 2) { printf("  FAILED - expected batch_size 2, got %d\n", batch_size2); failed = 1; }

        struct blob_check_state st = { &worker_ctx, 0, 0 };
        int rows2 = drain_cursor(driver, cur2, blob_row_cb, &st, &failed);
        driver->select_close(cur2);
        free(cols2);

        if (st.failed) failed = 1;

        printf("Test 2 total rows                   ... ");
        if (rows2 == lob_test_count) printf("OK (%d, %d checked)\n", rows2, st.checked);
        else { printf("FAILED - got %d expected %d\n", rows2, lob_test_count); failed = 1; }
    }

    /* ---- Test 3 ---- */
    int clob_test_count = -1;
    printf("Test 3 (direct COUNT OCI_CLOB_TEST) ... ");
    if (direct_row_count(&worker_ctx, "OCI_CLOB_TEST", &clob_test_count) != 0)
    { printf("FAILED\n"); failed = 1; }
    else printf("OK (%d rows)\n", clob_test_count);

    /* ---- Test 4 ---- */
    db_select_request_t req4 = {0};
    req4.sql              = "SELECT ID, DESCRIPTION, FILE_NAME, LARGE_CLOB FROM OCI_CLOB_TEST ORDER BY ID";
    req4.fetch_array_size  = 2;

    db_select_cursor_t *cur4 = NULL;
    db_column_meta_t   *cols4 = NULL;
    int col_count4 = 0, batch_size4 = 0;

    printf("Test 4 (cursor select w/ CLOB)      ... ");
    int rc4 = driver->select_open(&worker_ctx, &req4, &cur4, &cols4, &col_count4, &batch_size4);
    if (rc4 != 0)
    { printf("FAILED - select_open rc=%d\n", rc4); failed = 1; }
    else
    {
        printf("OK (%d columns, batch_size=%d, expected 1 - CLOB array-fetch quirk)\n",
               col_count4, batch_size4);
        if (batch_size4 != 1) { printf("  FAILED - expected batch_size 1, got %d\n", batch_size4); failed = 1; }

        struct clob_check_state st = { &worker_ctx, 0, 0, 0 };
        int rows4 = drain_cursor(driver, cur4, clob_row_cb, &st, &failed);
        driver->select_close(cur4);
        free(cols4);

        if (st.failed) failed = 1;

        printf("Test 4 total rows                   ... ");
        if (rows4 == clob_test_count)
            printf("OK (%d, %d checked, %d stat-skipped)\n", rows4, st.checked, st.stat_skipped);
        else
        { printf("FAILED - got %d expected %d\n", rows4, clob_test_count); failed = 1; }
    }

    /* ---- Test 5: mixed BLOB+CLOB, same query - alloc_batch_size regression check ---- */
    db_select_request_t req5 = {0};
    req5.sql              = "SELECT NUMBER_COL, VARCHAR2_COL, BLOB_COL, CLOB_COL "
                             "FROM OCI_FIELD_TEST WHERE NUMBER_COL IN (901, 902)";
    req5.fetch_array_size  = 2;

    db_select_cursor_t *cur5 = NULL;
    db_column_meta_t   *cols5 = NULL;
    int col_count5 = 0, batch_size5 = 0;

    printf("Test 5 (mixed BLOB+CLOB one query)  ... ");
    int rc5 = driver->select_open(&worker_ctx, &req5, &cur5, &cols5, &col_count5, &batch_size5);
    if (rc5 != 0)
    { printf("FAILED - select_open rc=%d\n", rc5); failed = 1; }
    else
    {
        printf("OK (%d columns, batch_size=%d, expected 1)\n", col_count5, batch_size5);
        if (batch_size5 != 1) { printf("  FAILED - expected batch_size 1, got %d\n", batch_size5); failed = 1; }

        int rows5 = drain_cursor(driver, cur5, NULL, NULL, &failed);
        driver->select_close(cur5);
        free(cols5);

        printf("Test 5 rows fetched                 ... OK (%d)\n", rows5);
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
