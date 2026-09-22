/*
 * Driver_Level2Parser_Test.c
 *
 * Follow-up proposal item 2 (2026-09-20) - standalone-harness pass for
 * OCI_Level2_Parser.c, third module after OCI_Table_Metadata_Module.c
 * (Driver_Metadata_Test.c, bug found and fixed) and
 * OCI_Transaction_Manager.c (Driver_Transaction_Test.c, clean pass).
 *
 * Scope note (see OCI_Level2_Parser.h's own header comment):
 * level2_validate_select() is deliberately connection-free - pure
 * syntax/structure analysis via extract_sql_dependencies(), already
 * covered by sql_dependency_extractor's own tests, out of scope here.
 * level2_validate_insert()/update()/delete() DO touch the database, via
 * a single shared static helper, normalize_client_date_value() - all 7
 * raw OCI calls in this module live there (a self-contained
 * SELECT TO_CHAR(TO_DATE(:1,:2),:3) FROM DUAL round trip used to
 * validate/canonicalise a client-supplied date string). This harness
 * exercises that helper through its real, public entry point -
 * level2_validate_insert() - rather than calling it directly (it's
 * static), which also happens to be exactly how it's actually invoked
 * in production (OCI_Insert_Template_Module.c/dispatcher.c never call
 * normalize_client_date_value() itself).
 *
 * No DB writes happen anywhere in this file. level2_validate_insert()
 * is validation-only - it resolves metadata via ctx->metadata_cache and
 * runs the date round-trip SELECT, but never executes the INSERT
 * itself (that's execute_insert_batch()'s job, a different module).
 * No baseline row-count helpers, no cleanup step needed - unlike
 * Driver_Transaction_Test.c, this table is never actually written to.
 *
 * Tests:
 *
 *   Test 1  - row_count=0: LEVEL2_ERR_ROW_COUNT_EXCEEDED. Pure struct
 *             check, no connection touched - confirms the
 *             connection-free checks still run before anything OCI-
 *             related, per Check 1's documented ordering.
 *   Test 2  - two rows with different column sets: LEVEL2_ERR_FIELD_
 *             INVALID (Check 1b). Also connection-free, also checked
 *             before the metadata_cache lookup, per the header's own
 *             ordering.
 *   Test 3  - unknown column name ('BOGUS_COL'): LEVEL2_ERR_FIELD_
 *             INVALID, "no such column". Proves the metadata_cache
 *             round trip itself works (a real column set has to come
 *             back correctly for "BOGUS_COL isn't in it" to be the
 *             actual reason this fails, not a metadata lookup error).
 *   Test 4  - NOT NULL column (NUMBER_COL) omitted entirely:
 *             LEVEL2_ERR_FIELD_INVALID, Check 4's "not supplied"
 *             message.
 *   Test 5  - the real target of this harness: DATE_COL set with an
 *             explicit client_date_format ("DD/MM/YYYY") on a
 *             genuinely valid date string ("25/12/2026") - must
 *             normalize cleanly to the canonical nls_date_format
 *             (config.ini) and return LEVEL2_OK. This is
 *             normalize_client_date_value()'s OCI round trip actually
 *             succeeding end to end.
 *   Test 6  - the failure-path mirror of Test 5: client_date_format
 *             "DD/MM/YYYY" with a value that isn't a valid date at all
 *             ("not-a-date") - Oracle must reject the TO_DATE() call
 *             (ORA-01858/ORA-01861), and that must surface as
 *             LEVEL2_ERR_FIELD_INVALID with the OCI error detail in
 *             error_detail->error_text, not a crash or a silently
 *             accepted bad value.
 *   Test 7  - DATE_COL set with NO client_date_format, value already
 *             in the canonical nls_date_format - exercises the
 *             source_fmt == canonical_fmt branch (source_fmt defaults
 *             to canonical_fmt when client_date_format is empty, per
 *             normalize_client_date_value()'s own doc comment) -
 *             LEVEL2_OK.
 *   Test 8  - a TIMESTAMP-shaped value would exercise the is_timestamp
 *             branch (".FF6" suffix) but UNIT_TEST_FIELD_TEST has no
 *             TIMESTAMP column (see Create_Unit_Test_Table.txt) - noted
 *             as a follow-up rather than inventing a fixture inside
 *             this harness, same caveat style as Driver_Metadata_
 *             Test.c's missing VIEW/SYNONYM fixture for
 *             get_object_metadata().
 *
 * Uses UNIT_TEST_FIELD_TEST (NUMBER_COL NOT NULL/PK, VARCHAR2_COL,
 * DATE_COL, CLOB_COL - all three non-key columns nullable, no
 * defaults - see Driver_Metadata_Test.c's Test 1 output) so Tests 5-7
 * only ever need to set NUMBER_COL + DATE_COL, nothing else.
 *
 * Build (same convention as Driver_Metadata_Test.c/Driver_Transaction_
 * Test.c - this module pulls in metadata_cache.c/metadata_cache_meta.c
 * and OCI_Insert_Validate_Module.c as real dependencies, not stubs,
 * since Test 3/4/5/6/7 all genuinely exercise them):
 *
 *   gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
 *       -I/usr/include/cjson -I/usr/include/libxml2 \
 *       -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include -I. \
 *       -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
 *       -o Driver_Level2Parser_Test \
 *       Driver_Level2Parser_Test.c db_driver.c driver_oracle.c \
 *       OCI_Connection.c OCI_Connection_Pool.c oci_cache.c string_utils.c \
 *       ini_reader.c logger.c metrics.c ctx_utils.c \
 *       OCI_Level2_Parser.c OCI_Table_Metadata_Module.c \
 *       OCI_Insert_Validate_Module.c metadata_cache.c metadata_cache_meta.c \
 *       sql_dependency_extractor.c \
 *       -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
 *       -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson \
 *       -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
 *
 * Run (same LD_LIBRARY_PATH/LSAN_OPTIONS pattern as Run_Manually.sh):
 *   ./Driver_Level2Parser_Test
 *
 * Expected output on success:
 *   Test 1 (row_count=0)                        ... OK (LEVEL2_ERR_ROW_COUNT_EXCEEDED)
 *   Test 2 (row column set mismatch)            ... OK (LEVEL2_ERR_FIELD_INVALID)
 *   Test 3 (unknown column)                     ... OK (LEVEL2_ERR_FIELD_INVALID)
 *   Test 4 (NOT NULL column omitted)            ... OK (LEVEL2_ERR_FIELD_INVALID)
 *   Test 5 (date normalize, valid + format)     ... OK (LEVEL2_OK)
 *   Test 6 (date normalize, invalid value)      ... OK (LEVEL2_ERR_FIELD_INVALID)
 *   Test 7 (date normalize, no client format)   ... OK (LEVEL2_OK)
 *   PASS
 *
 * A non-zero exit code means at least one test failed - check
 * config.insert_log_file_name (Level 2's INSERT checks all log there,
 * matching every existing Insert-path log call per the header) and
 * config.Metadata_log_file_name/config.cache_log_file_name for the
 * metadata_cache_get_or_fetch() round trip specifically.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"
#define TEST_TABLE "UNIT_TEST_FIELD_TEST"
#define TEST_OWNER "DATA_MANAGER"

#include <stdio.h>
#include <string.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "OCI_Level2_Parser.h"
#include "OCI_Insert_Execute_Module.h"
#include "OCI_Request_Response_Types.h"
#include "metadata_cache.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *main_logger,
                     logger_t *connection_logger,
                     logger_t *connectionpool_logger,
                     logger_t *insert_logger,
                     logger_t *metadata_logger,
                     logger_t *cache_logger)
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

    if (logger_init_str2(main_logger, config->log_file_name,
                          config->log_file_max_size,
                          config->log_file_rotation_number,
                          config->log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init main logger\n"); return -1; }
    ctx->logger = main_logger;

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

    if (logger_init_str2(metadata_logger, config->Metadata_log_file_name,
                          config->Metadata_log_file_max_size,
                          config->Metadata_log_file_rotation_number,
                          config->Metadata_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init Metadata_logger\n"); return -1; }
    ctx->Metadata_logger = metadata_logger;

    if (logger_init_str2(cache_logger, config->cache_log_file_name,
                          config->cache_log_file_max_size,
                          config->cache_log_file_rotation_number,
                          config->cache_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init cache_logger\n"); return -1; }
    ctx->cache_logger = cache_logger;

    return 0;
}

/* One field_value_t, zero-initialised, with the given name/value and
 * optional client_date_format ("" for none). */
static field_value_t make_field(const char *name, const char *value,
                                 const char *date_fmt)
{
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, name, sizeof(fv.field_name) - 1);
    strncpy(fv.value,      value, sizeof(fv.value) - 1);
    if (date_fmt)
        strncpy(fv.client_date_format, date_fmt, sizeof(fv.client_date_format) - 1);
    return fv;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx_base;
    app_config_t  config;
    logger_t err_log, main_log, conn_log, poolconn_log,
             insert_log, meta_log, cache_log;

    if (init_ctx(&ctx_base, &config, &err_log, &main_log, &conn_log,
                 &poolconn_log, &insert_log, &meta_log, &cache_log) != 0)
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
    worker.insert_logger         = &insert_log;
    worker.Metadata_logger       = &meta_log;
    worker.cache_logger          = &cache_log;
    worker.connection_logger     = &conn_log;
    worker.connectionpool_logger = &poolconn_log;
    worker.error_logger          = &err_log;
    worker.logger                = &main_log;
    worker.ini                   = &config;

    cache_t *mcache = metadata_cache_init(&config, &cache_log);
    if (!mcache)
    {
        fprintf(stderr, "metadata_cache_init() failed\n");
        driver->release_session(&ctx_base, &worker);
        driver->disconnect(&ctx_base);
        return 1;
    }
    worker.metadata_cache = mcache;

    /* ---- Test 1: row_count=0 ---- */
    printf("Test 1 (row_count=0) ... ");
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 0;
        req.rows = NULL;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_ROW_COUNT_EXCEEDED)
        {
            printf("FAILED - expected LEVEL2_ERR_ROW_COUNT_EXCEEDED, got %d\n", rc);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_ROW_COUNT_EXCEEDED)\n");
    }

    /* ---- Test 2: two rows, different column sets ---- */
    printf("Test 2 (row column set mismatch) ... ");
    {
        field_value_t row0_fields[1] = { make_field("NUMBER_COL", "1", "") };
        field_value_t row1_fields[2] = { make_field("NUMBER_COL", "2", ""),
                                          make_field("VARCHAR2_COL", "x", "") };
        insert_row_t rows[2] = {
            { .field_count = 1, .fields = row0_fields },
            { .field_count = 2, .fields = row1_fields },
        };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 2;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_FIELD_INVALID)
        {
            printf("FAILED - expected LEVEL2_ERR_FIELD_INVALID, got %d\n", rc);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_FIELD_INVALID)\n");
    }

    /* ---- Test 3: unknown column ---- */
    printf("Test 3 (unknown column) ... ");
    {
        field_value_t fields[2] = { make_field("NUMBER_COL", "3", ""),
                                     make_field("BOGUS_COL", "x", "") };
        insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_FIELD_INVALID || !strstr(status.error_text, "no such column"))
        {
            printf("FAILED - rc=%d error_text='%s' (expected LEVEL2_ERR_FIELD_INVALID, "
                   "'no such column')\n", rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_FIELD_INVALID)\n");
    }

    /* ---- Test 4: NOT NULL column omitted ---- */
    printf("Test 4 (NOT NULL column omitted) ... ");
    {
        field_value_t fields[1] = { make_field("VARCHAR2_COL", "x", "") };
        insert_row_t rows[1] = { { .field_count = 1, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_FIELD_INVALID)
        {
            printf("FAILED - expected LEVEL2_ERR_FIELD_INVALID, got %d (error_text='%s')\n",
                   rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_FIELD_INVALID)\n");
    }

    /* ---- Test 5: date normalize, valid value + explicit format ---- */
    printf("Test 5 (date normalize, valid + format) ... ");
    {
        field_value_t fields[2] = {
            make_field("NUMBER_COL", "5", ""),
            make_field("DATE_COL",   "25/12/2026", "DD/MM/YYYY"),
        };
        insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_OK)
        {
            printf("FAILED - expected LEVEL2_OK, got %d (error_text='%s')\n",
                   rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_OK, normalized value='%s')\n", fields[1].value);
    }

    /* ---- Test 6: date normalize, invalid value ---- */
    printf("Test 6 (date normalize, invalid value) ... ");
    {
        field_value_t fields[2] = {
            make_field("NUMBER_COL", "6", ""),
            make_field("DATE_COL",   "not-a-date", "DD/MM/YYYY"),
        };
        insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_FIELD_INVALID || !strstr(status.error_text, "Invalid date"))
        {
            printf("FAILED - rc=%d error_text='%s' (expected LEVEL2_ERR_FIELD_INVALID, "
                   "'Invalid date')\n", rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_FIELD_INVALID)\n");
    }

    /* ---- Test 7: date normalize, no client_date_format ---- */
    printf("Test 7 (date normalize, no client format) ... ");
    {
        /* Already in config.ini's nls_date_format - see source_fmt
         * defaulting to canonical_fmt in normalize_client_date_value()
         * when client_date_format is empty. */
        field_value_t fields[2] = {
            make_field("NUMBER_COL", "7", ""),
            make_field("DATE_COL",   "2026-06-15 00:00:00", ""),
        };
        insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_OK)
        {
            printf("FAILED - expected LEVEL2_OK, got %d (error_text='%s')\n",
                   rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_OK)\n");
    }

    printf("%s\n", failed ? "FAIL" : "PASS");

    metadata_cache_destroy(mcache);
    driver->release_session(&ctx_base, &worker);
    driver->disconnect(&ctx_base);

    logger_close(&err_log);
    logger_close(&main_log);
    logger_close(&conn_log);
    logger_close(&poolconn_log);
    logger_close(&insert_log);
    logger_close(&meta_log);
    logger_close(&cache_log);

    return failed ? 1 : 0;
}
