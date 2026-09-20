/*
 * Driver_DDL_Test.c
 *
 * Validates db_driver_t's dml_execute() reused for DDL - bind_count=0,
 * since none of the six DDL types (CREATE_TABLE/VIEW/USER/PROCEDURE,
 * DROP_TABLE, GRANT) ever bind a variable - and specifically the new
 * out_error_message/out_error_message_size capability added for this
 * pass (see db_driver.h's own doc comment on dml_execute() for why:
 * execute_ddl_statement()'s own callers embed the real Oracle error
 * text directly in the client-facing response, unlike every prior
 * caller of this function). Same A/B philosophy as every harness in
 * this series: every check the driver's own work is measured against
 * comes from plain, direct OCI calls (querying USER_VIEWS directly),
 * not just the driver's own return code.
 *
 * Unlike every other harness in this series, there is no commit()/
 * rollback() test here - execute_ddl_statement() never calls either
 * (Oracle auto-commits DDL unconditionally, confirmed directly against
 * that module's own real code and its own header doc comment on the
 * ctx->active_tx guard) - so this driver call is never followed by one
 * for DDL, and there is nothing to test there for this module.
 *
 * Reserved object name for this harness: DRIVER_DDL_TEST_VIEW - a
 * dedicated, disposable view, created and dropped by this harness
 * alone, never touching real schema objects.
 *
 * What it does:
 *
 *   Test 1 - CREATE VIEW (success case): creates
 *            DRIVER_DDL_TEST_VIEW for the first time (idempotent pre-
 *            cleanup drops it first if a previous interrupted run left
 *            it behind). Checks rc=0 and, independently via a direct
 *            query against USER_VIEWS, that the view genuinely exists
 *            afterward - not just trusting the driver's own return
 *            code.
 *
 *   Test 2 - CREATE VIEW again, deliberately without OR REPLACE
 *            (failure case) - the view from Test 1 already exists, so
 *            this must fail with a real Oracle error (ORA-00955,
 *            "name is already used by an existing object" - the exact
 *            error the design doc's own Section 12 documents this
 *            module already handles for GRANT/CREATE VIEW/DROP TABLE
 *            specifically). Checks rc != 0 AND that out_error_message
 *            actually contains real Oracle error text, not empty -
 *            proving the new capability genuinely round-trips, not
 *            just that failure is detected.
 *
 *   Test 3 - DROP VIEW (a second DDL type, and cleanup): drops
 *            DRIVER_DDL_TEST_VIEW. Checks rc=0 and, independently,
 *            that the view genuinely no longer exists afterward.
 *
 * Final cleanup: best-effort DROP VIEW, does not affect PASS/FAIL -
 * Test 3 already does this as part of its own normal run, but this
 * covers the case where Test 3 itself failed partway through.
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

#define TEST_CHECK_OCI(ctx, status) \
    do { \
        if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
            text errbuf[512]; sb4 errcode = 0; \
            OCIErrorGet((ctx)->errhp, 1, NULL, &errcode, errbuf, \
                        sizeof(errbuf), OCI_HTYPE_ERROR); \
            logger_write((ctx)->ddl_logger, LOG_ERROR, __func__, 0, \
                         "OCI Error %d: %s", errcode, (char *)errbuf); \
        } \
    } while (0)

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *connection_logger,
                     logger_t *connectionpool_logger, logger_t *ddl_logger)
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

    if (logger_init_str2(ddl_logger, config->ddl_log_file_name,
                          config->ddl_log_file_max_size,
                          config->ddl_log_file_rotation_number,
                          config->ddl_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init ddl_logger\n"); return -1; }
    ctx->ddl_logger = ddl_logger;

    return 0;
}

/* ---- Direct-OCI helper, independent of driver_oracle.c entirely ---- */

static int direct_view_exists(oci_context_t *ctx, const char *view_name,
                               int *out_exists)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM USER_VIEWS WHERE VIEW_NAME = '%s'",
             view_name);

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
    *out_exists = (count > 0);
    return 0;
}

/* Best-effort, direct DROP VIEW - used for idempotent pre-cleanup and
 * final teardown. A "view doesn't exist" failure here is expected and
 * fine, not checked. */
static void direct_drop_view_best_effort(oci_context_t *ctx, const char *view_name)
{
    char sql[256];
    snprintf(sql, sizeof(sql), "DROP VIEW %s", view_name);

    OCIStmt *stmt = NULL;
    if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) != OCI_SUCCESS)
        return;
    if (!stmt) return;
    OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, connpool_l, ddl_l;
    if (init_ctx(&ctx, &config, &err_l, &conn_l, &connpool_l, &ddl_l) != 0)
    {
        printf("INIT FAILED\n");
        return 1;
    }

    const db_driver_t *driver = db_driver_get(&ctx);
    if (!driver || !driver->connect || !driver->get_session ||
        !driver->dml_execute || !driver->release_session || !driver->disconnect)
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

    const char *view_name = "DRIVER_DDL_TEST_VIEW";

    /* ---- Idempotent pre-cleanup ---- */
    direct_drop_view_best_effort(&worker_ctx, view_name);

    /* ---- Test 1 - CREATE VIEW (success case) ---- */
    printf("Test 1 (CREATE VIEW success)        ... ");
    {
        char sql1[256];
        snprintf(sql1, sizeof(sql1),
                 "CREATE VIEW %s AS SELECT 1 AS DUMMY_COL FROM DUAL",
                 view_name);

        db_dml_request_t req1;
        memset(&req1, 0, sizeof(req1));
        req1.sql         = sql1;
        req1.bind_count  = 0;
        req1.bind_values = NULL;

        int  rows1 = -1;
        char err1[512] = {0};
        int rc1 = driver->dml_execute(&worker_ctx, &ddl_l, &req1, &rows1,
                                       err1, sizeof(err1));

        int exists1 = 0;
        if (rc1 != 0)
        {
            printf("FAILED - rc=%d err='%s' (expected rc=0)\n", rc1, err1);
            failed = 1;
        }
        else if (direct_view_exists(&worker_ctx, view_name, &exists1) != 0 ||
                 !exists1)
        {
            printf("FAILED - rc=0 but view does not genuinely exist "
                   "(exists=%d)\n", exists1);
            failed = 1;
        }
        else
            printf("OK (view genuinely exists, verified independently)\n");
    }

    /* ---- Test 2 - CREATE VIEW again, no OR REPLACE (failure case) ---- */
    printf("Test 2 (CREATE VIEW failure+errtext)... ");
    {
        char sql2[256];
        snprintf(sql2, sizeof(sql2),
                 "CREATE VIEW %s AS SELECT 1 AS DUMMY_COL FROM DUAL",
                 view_name);

        db_dml_request_t req2;
        memset(&req2, 0, sizeof(req2));
        req2.sql         = sql2;
        req2.bind_count  = 0;
        req2.bind_values = NULL;

        int  rows2 = -1;
        char err2[512] = {0};
        int rc2 = driver->dml_execute(&worker_ctx, &ddl_l, &req2, &rows2,
                                       err2, sizeof(err2));

        if (rc2 == 0)
        {
            printf("FAILED - rc=0, expected a failure (duplicate view name)\n");
            failed = 1;
        }
        else if (strlen(err2) == 0)
        {
            printf("FAILED - rc=%d correctly, but out_error_message is "
                   "empty (expected real Oracle error text)\n", rc2);
            failed = 1;
        }
        else if (!strstr(err2, "ORA-"))
        {
            printf("FAILED - rc=%d, out_error_message='%s' doesn't look "
                   "like a real Oracle error\n", rc2, err2);
            failed = 1;
        }
        else
            printf("OK (rc=%d, real error text captured: '%s')\n", rc2, err2);
    }

    /* ---- Test 3 - DROP VIEW (second DDL type + cleanup) ---- */
    printf("Test 3 (DROP VIEW success)          ... ");
    {
        char sql3[256];
        snprintf(sql3, sizeof(sql3), "DROP VIEW %s", view_name);

        db_dml_request_t req3;
        memset(&req3, 0, sizeof(req3));
        req3.sql         = sql3;
        req3.bind_count  = 0;
        req3.bind_values = NULL;

        int  rows3 = -1;
        char err3[512] = {0};
        int rc3 = driver->dml_execute(&worker_ctx, &ddl_l, &req3, &rows3,
                                       err3, sizeof(err3));

        int exists3 = 1;
        if (rc3 != 0)
        {
            printf("FAILED - rc=%d err='%s' (expected rc=0)\n", rc3, err3);
            failed = 1;
        }
        else if (direct_view_exists(&worker_ctx, view_name, &exists3) != 0 ||
                 exists3)
        {
            printf("FAILED - rc=0 but view still exists (exists=%d)\n", exists3);
            failed = 1;
        }
        else
            printf("OK (view genuinely gone, verified independently)\n");
    }

    /* ---- Final cleanup - best-effort ---- */
    direct_drop_view_best_effort(&worker_ctx, view_name);

    driver->release_session(&ctx, &worker_ctx);
    driver->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&err_l);
    logger_close(&conn_l);
    logger_close(&connpool_l);
    logger_close(&ddl_l);

    return failed ? 1 : 0;
}
