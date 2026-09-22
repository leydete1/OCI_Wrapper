/*
 * Driver_Metadata_Test.c
 *
 * Follow-up proposal item 2 (2026-09-20) - dedicated standalone-harness
 * pass for OCI_Table_Metadata_Module.c before any further decisions are
 * made about it. Chosen first because every CRUD/select module in this
 * project ultimately depends on it (get_multi_metadata() is already
 * exercised indirectly via Driver_Select_Test.c's select_open() path,
 * but get_request_metadata()/get_table_metadata()/get_object_metadata()
 * have no standalone coverage of their own today - this file closes
 * that gap the same way Driver_Connect_Test.c/Driver_Pool_Test.c did
 * for connection handling).
 *
 * This is NOT an extraction pass. Unlike the six DDL modules or
 * OCI_Insert_Validate_Module.c, this module's OCI calls are genuinely
 * Oracle-specific (ALL_TAB_COLUMNS / ALL_TABLES / ALL_OBJECTS queries,
 * OCIParamGet descriptor walks) and driver_oracle.c already calls
 * through to get_multi_metadata() rather than reimplementing it (see
 * driver_oracle.c's select_open() header comment) - so there is nothing
 * here that belongs inside driver_oracle.c itself. The goal of this
 * pass is narrower: confirm each of the four public functions still
 * behaves correctly in isolation, with an independent baseline to check
 * each one against, before the next decision (should
 * OCI_Insert_Template_Module.c / dispatcher.c / metadata_cache.c keep
 * calling this module directly, the same "direct caller bypasses the
 * driver" question flagged for OCI_Connection.c, or should there be a
 * driver-level metadata accessor?) gets made.
 *
 * Uses UNIT_TEST_FIELD_TEST (see Create_Unit_Test_Table.txt:
 * NUMBER_COL NOT NULL / PK, VARCHAR2_COL, DATE_COL, CLOB_COL - 4
 * columns, real PK, real CLOB) rather than OCI_FIELD_TEST, because a
 * small fixed-shape table makes the independent baseline queries in
 * Test 1/2/3 trivial to hand-verify; OCI_FIELD_TEST's full type sweep
 * is exactly what a later data-type-mapping pass (not this one) would
 * want instead.
 *
 * Connects via the pool (config.ini's use_connection_pool=1, matching
 * Driver_Select_Test.c/Driver_Pool_Test.c) and borrows a worker_ctx the
 * same way execute_query_batch() and select_open() are actually called
 * in production - not a bare direct connection. Per Terry's 2026-09-21
 * note, the non-pooled OCI_Connection.c path is flagged separately as a
 * likely-removable special case now that the pool is used exclusively;
 * this harness does not exercise that path at all.
 *
 * What it does:
 *
 *   Test 1 - get_request_metadata() on UNIT_TEST_FIELD_TEST with an
 *            explicit owner: independent baseline is a direct
 *            "SELECT COUNT(*) FROM ALL_TAB_COLUMNS WHERE TABLE_NAME=..
 *            AND OWNER=.." via plain OCI calls, checked against
 *            *col_count. Column names/order are then checked against
 *            the four expected columns in COLUMN_ID order.
 *
 *   Test 2 - get_request_metadata() again with req->owner left empty,
 *            to exercise the auto-resolve-from-ALL_TABLES path - checks
 *            that req->owner comes back populated and non-empty, and
 *            that the result matches Test 1's column list exactly.
 *
 *   Test 3 - get_table_metadata() on the same table: independent
 *            baseline is a direct "SELECT OWNER FROM ALL_TABLES WHERE
 *            TABLE_NAME=.." - checks the returned table_metadata_alltabs_t
 *            ->owner and ->table_name match, then free_table_metadata().
 *
 *   Test 4 - get_object_metadata() on the same table: checks
 *            ->object_type comes back "TABLE" and ->object_name matches,
 *            then free_object_metadata().
 *
 *   Test 5 - get_table_metadata() on a name that does not exist: must
 *            return NULL (error already logged to Metadata_logger, not
 *            a crash) - the not-found path matters as much as the
 *            happy path given how many callers treat a NULL return as
 *            "stop, do not proceed."
 *
 * NOT covered here (deliberately, see header above):
 *   - get_multi_metadata()/get_select_metadata() - already exercised by
 *     Driver_Select_Test.c and by OCI_Execute_Query_Batch_Module.c in
 *     production; re-proving the OCIParamGet/OCIDefineByPos path here
 *     would just duplicate that coverage.
 *   - get_object_metadata() against a VIEW/SYNONYM - no view fixture
 *     exists in this project's test DDL today (see Create_Unit_Test_Table.txt,
 *     Create_Oracle_Test_Table.txt); flagging as a follow-up rather than
 *     inventing a fixture inside this harness.
 *
 * Build (same convention as Driver_Select_Test.c - adds
 * OCI_Table_Metadata_Module.c itself plus its own dependencies,
 * checked against its #include list, nothing extra):
 *
 *   gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
 *       -I/usr/include/cjson -I/usr/include/libxml2 \
 *       -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include -I. \
 *       -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
 *       -o Driver_Metadata_Test \
 *       Driver_Metadata_Test.c db_driver.c driver_oracle.c \
 *       OCI_Connection.c OCI_Connection_Pool.c oci_cache.c string_utils.c \
 *       ini_reader.c logger.c metrics.c ctx_utils.c \
 *       OCI_Table_Metadata_Module.c OCI_Resultset_Builder.c \
 *       sql_dependency_extractor.c \
 *       -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
 *       -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson \
 *       -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
 *
 * Run (same LD_LIBRARY_PATH/LSAN_OPTIONS pattern as Run_Manually.sh):
 *   ./Driver_Metadata_Test
 *
 * Expected output on success:
 *   Test 1 (get_request_metadata, explicit owner) ... OK (4 columns)
 *   Test 2 (get_request_metadata, owner auto-resolve) ... OK (owner=DATA_MANAGER, matches Test 1)
 *   Test 3 (get_table_metadata)                    ... OK (owner=DATA_MANAGER, table=UNIT_TEST_FIELD_TEST)
 *   Test 4 (get_object_metadata)                   ... OK (object_type=TABLE)
 *   Test 5 (get_table_metadata, not-found)          ... OK (NULL returned, no crash)
 *   PASS
 *
 * A non-zero exit code means at least one test failed - check
 * config.Metadata_log_file_name (all five tests log there) for the
 * OCI/SQL error detail already logged by the module itself. Also check
 * config.connectionpool_log_file_name and config.log_file_name - Run 1
 * (2026-09-21) showed ORA-03114 starting at Test 2 with only error/
 * connection/Metadata wired, and per Driver_Procedure_Test.c's own
 * documented gotcha a narrow logger set can silently swallow the real
 * cause; all five relevant loggers are wired below for that reason.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

/* Adjust if the harness is pointed at a different schema than
 * DATA_MANAGER - kept as one constant rather than repeated literals so
 * Test 1's expected owner and Test 3/4's checks can't drift apart. */
#define TEST_OWNER "DATA_MANAGER"
#define TEST_TABLE "UNIT_TEST_FIELD_TEST"
#define MISSING_TABLE "UNIT_TEST_FIELD_TEST_DOES_NOT_EXIST"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "OCI_Table_Metadata_Module.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

/* Local OCI error macro - same shape/destination as
 * OCI_Table_Metadata_Module.c's own, used only for this file's
 * independent-baseline OCI calls (Test 1/3), never for calls made
 * through the module itself (the module logs its own errors). */
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

/* Run 1 finding (2026-09-21): the pool/connect path touches more named
 * loggers than just Metadata_logger - confirmed against
 * Driver_Procedure_Test.c's own documented gotcha ("a caller of any
 * function in this family must supply every named logger the call
 * chain actually touches, not just the one matching its own module
 * name") and against db_driver_Interface_and_New_Driver_Guide.docx's
 * write-up of the same lesson. Run 1 only wired error/connection/
 * Metadata and got ~28 "Logger is NULL" lines from logger.c's own
 * NULL-safe guard - meaning real diagnostic detail from get_session()/
 * the pool's own connectionpool_logger and the generic ctx->logger was
 * being silently swallowed, not that nothing was being logged at all.
 * Widened here to error/logger/connection/connectionpool/Metadata so
 * whatever actually caused Run 1's ORA-03114 is fully visible in the
 * logs on the next run, not masked by this harness's own omission. */
static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *main_logger,
                     logger_t *connection_logger,
                     logger_t *connectionpool_logger,
                     logger_t *metadata_logger)
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

    if (logger_init_str2(metadata_logger, config->Metadata_log_file_name,
                          config->Metadata_log_file_max_size,
                          config->Metadata_log_file_rotation_number,
                          config->Metadata_log_level, ctx->error_logger) != 0)
    {
        fprintf(stderr, "Failed to init Metadata_logger\n");
        return -1;
    }
    ctx->Metadata_logger = metadata_logger;

    return 0;
}

/* Independent baseline for Test 1: how many ALL_TAB_COLUMNS rows really
 * exist for TEST_OWNER.TEST_TABLE, via plain OCI calls, entirely
 * independent of get_request_metadata() - same reasoning as
 * Driver_Select_Test.c's Test 1 (direct COUNT(*)) baseline. */
static int baseline_column_count(oci_context_t *ctx, int *out_count)
{
    const char *sql =
        "SELECT COUNT(*) FROM ALL_TAB_COLUMNS "
        "WHERE TABLE_NAME = :tname AND OWNER = :towner";

    OCIStmt *stmt = NULL;
    sword s = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                               (const OraText *)sql, (ub4)strlen(sql),
                               NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    OCIBind *b1 = NULL, *b2 = NULL;
    s = OCIBindByName(stmt, &b1, ctx->errhp, (OraText *)":tname",
                       -1, (void *)TEST_TABLE, (sb4)strlen(TEST_TABLE) + 1,
                       SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
    CHECK_OCI(ctx, s);

    s = OCIBindByName(stmt, &b2, ctx->errhp, (OraText *)":towner",
                       -1, (void *)TEST_OWNER, (sb4)strlen(TEST_OWNER) + 1,
                       SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx_base;
    app_config_t  config;
    logger_t      err_log, main_log, conn_log, poolconn_log, meta_log;

    if (init_ctx(&ctx_base, &config, &err_log, &main_log, &conn_log,
                 &poolconn_log, &meta_log) != 0)
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

    /* Worker ctx borrowed from the pool, same shape production code
     * actually uses (execute_query_batch/select_open) - not the bare
     * master ctx. */
    oci_context_t worker;
    if (driver->get_session(&ctx_base, &worker) != 0)
    {
        fprintf(stderr, "get_session() failed\n");
        driver->disconnect(&ctx_base);
        return 1;
    }
    /* get_session() populates the OCI handles (svchp/errhp/etc.) but,
     * per the confirmed Driver_Procedure_Test.c gotcha, does NOT copy
     * the named loggers onto worker_ctx - every one this harness's own
     * call chain touches must be set explicitly here. */
    worker.Metadata_logger      = &meta_log;
    worker.connection_logger    = &conn_log;
    worker.connectionpool_logger = &poolconn_log;
    worker.error_logger         = &err_log;
    worker.logger                = &main_log;
    worker.ini                   = &config;

    /* ---- Independent baseline for Test 1/2 ---- */
    int baseline_count = 0;
    if (baseline_column_count(&worker, &baseline_count) != 0)
    {
        fprintf(stderr, "Baseline ALL_TAB_COLUMNS count query failed\n");
        driver->release_session(&ctx_base, &worker);
        driver->disconnect(&ctx_base);
        return 1;
    }

    /* ---- Test 1: get_request_metadata(), explicit owner ---- */
    printf("Test 1 (get_request_metadata, explicit owner) ... ");
    {
        metadata_request_t req; memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);

        static col_metadata_t cols[MAX_TABLE_COLUMNS];
        int col_count = 0;

        if (get_request_metadata(&worker, &req, cols, &col_count,
                                  MAX_TABLE_COLUMNS) != 0)
        {
            printf("FAILED - see %s\n", config.Metadata_log_file_name);
            failed = 1;
        }
        else if (col_count != baseline_count)
        {
            printf("FAILED - got %d columns, baseline ALL_TAB_COLUMNS says %d\n",
                   col_count, baseline_count);
            failed = 1;
        }
        else
        {
            printf("OK (%d columns)\n", col_count);
            for (int i = 0; i < col_count; i++)
                printf("  [%d] %-16s %-12s len=%d prec=%d scale=%d null=%s\n",
                       i, cols[i].col_name, cols[i].data_type,
                       cols[i].data_length, cols[i].data_precision,
                       cols[i].data_scale, cols[i].nullable);
        }
    }

    /* ---- Test 2: get_request_metadata(), owner auto-resolve ---- */
    printf("Test 2 (get_request_metadata, owner auto-resolve) ... ");
    {
        metadata_request_t req; memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        /* req.owner left empty on purpose - exercises the
         * resolve-from-ALL_TABLES branch. */

        static col_metadata_t cols[MAX_TABLE_COLUMNS];
        int col_count = 0;

        if (get_request_metadata(&worker, &req, cols, &col_count,
                                  MAX_TABLE_COLUMNS) != 0)
        {
            printf("FAILED - see %s\n", config.Metadata_log_file_name);
            failed = 1;
        }
        else if (req.owner[0] == '\0')
        {
            printf("FAILED - owner not written back after auto-resolve\n");
            failed = 1;
        }
        else if (col_count != baseline_count)
        {
            printf("FAILED - got %d columns, baseline says %d\n",
                   col_count, baseline_count);
            failed = 1;
        }
        else
        {
            printf("OK (owner=%s, matches Test 1)\n", req.owner);
        }
    }

    /* ---- Test 3: get_table_metadata() ---- */
    printf("Test 3 (get_table_metadata) ... ");
    {
        table_metadata_alltabs_t *tm =
            get_table_metadata(&worker, TEST_OWNER, TEST_TABLE);

        if (!tm)
        {
            printf("FAILED - see %s\n", config.Metadata_log_file_name);
            failed = 1;
        }
        else if (strcasecmp(tm->table_name, TEST_TABLE) != 0)
        {
            printf("FAILED - table_name mismatch: got '%s'\n", tm->table_name);
            failed = 1;
            free_table_metadata(tm);
        }
        else
        {
            printf("OK (owner=%s, table=%s)\n", TEST_OWNER, tm->table_name);
            free_table_metadata(tm);
        }
    }

    /* ---- Test 4: get_object_metadata() ---- */
    printf("Test 4 (get_object_metadata) ... ");
    {
        object_metadata_allobjs_t *om =
            get_object_metadata(&worker, TEST_OWNER, TEST_TABLE);

        if (!om)
        {
            printf("FAILED - see %s\n", config.Metadata_log_file_name);
            failed = 1;
        }
        else if (strcasecmp(om->object_type, "TABLE") != 0)
        {
            printf("FAILED - expected object_type=TABLE, got '%s'\n", om->object_type);
            failed = 1;
            free_object_metadata(om);
        }
        else
        {
            printf("OK (object_type=%s)\n", om->object_type);
            free_object_metadata(om);
        }
    }

    /* ---- Test 5: get_table_metadata(), not-found path ----
     * Run 1 caveat: a NULL return here is only meaningful proof of the
     * "not found, logged, no crash" path if Tests 2-4 above actually
     * succeeded - in Run 1 every call after Test 1 failed with
     * ORA-03114 (not connected), so this test's NULL "passed" for the
     * wrong reason. Read Test 5's result together with Tests 2-4, not
     * in isolation. */
    printf("Test 5 (get_table_metadata, not-found) ... ");
    {
        table_metadata_alltabs_t *tm =
            get_table_metadata(&worker, TEST_OWNER, MISSING_TABLE);

        if (tm != NULL)
        {
            printf("FAILED - expected NULL for a nonexistent table, got a result\n");
            failed = 1;
            free_table_metadata(tm);
        }
        else
        {
            printf("OK (NULL returned, no crash)\n");
        }
    }

    printf("%s\n", failed ? "FAIL" : "PASS");

    driver->release_session(&ctx_base, &worker);
    driver->disconnect(&ctx_base);

    /* ---- Cleanup - same reasoning as Driver_Connect_Test.c's own
     * cleanup section: only the loggers this file created are this
     * file's responsibility. */
    logger_close(&err_log);
    logger_close(&main_log);
    logger_close(&conn_log);
    logger_close(&poolconn_log);
    logger_close(&meta_log);

    return failed ? 1 : 0;
}
