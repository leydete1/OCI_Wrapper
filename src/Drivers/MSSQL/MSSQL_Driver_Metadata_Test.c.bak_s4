/*
 * MSSQL_Driver_Metadata_Test.c   (driver_mssql Stage 3, point T10)
 *
 * Checks driver_mssql's describe_table (mssql_metadata.c) against the
 * DataManager database that Create_MSSQL_Test_Objects.sql builds.
 * Reads only.
 *
 * Usage:  MSSQL_Driver_Metadata_Test [config_mssql.ini]
 *
 *   Test 1  standalone connect
 *   Test 2  OCI_FIELD_TEST: 20 columns in table order, and the type,
 *           length, precision, scale, nullable and default of the
 *           columns that matter (IDENTITY key, decimal, char, nvarchar,
 *           datetime2, datetimeoffset, varbinary, the three (MAX) ones)
 *   Test 3  every OCI_FIELD_TEST column has a class (none OTHER), the
 *           (MAX) columns are LOB, and lob_kind agrees with the
 *           dialect's lob_placeholder
 *   Test 4  owner left empty: resolved to DATA_MANAGER (written back)
 *   Test 5  lower-case names are upper-cased in place and still found
 *   Test 6  a table that does not exist: -1, no columns
 *   Test 7  max_cols smaller than the table: stops there (as Oracle)
 *   Test 8  the gateway and test tables: every one described, no
 *           column of class OTHER, defaults present where the schema
 *           has them
 *
 * Prints PASS or FAIL as the last line; exit code 0 / 1.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI_MSSQL "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config_mssql.ini"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Connection.h"
#include "db_driver.h"
#include "db_type_class.h"
#include "driver_mssql.h"
#include "MSSQL_Pool.h"
#include "ini_reader.h"
#include "logger.h"

static int init_ctx(const char *ini_path, oci_context_t *ctx, app_config_t *config,
                    logger_t *err_l, logger_t *conn_l, logger_t *pool_l,
                    logger_t *sel_l, logger_t *meta_l)
{
    memset(ctx, 0, sizeof(*ctx));
    memset(config, 0, sizeof(*config));
    ctx->ini             = config;
    ctx->pool_slot_index = -1;

    if (load_ini(ini_path, config, ctx) != 0)
    {
        fprintf(stderr, "Failed to load ini file: %s\n", ini_path);
        return -1;
    }
    if (strcmp(config->db_type, "mssql") != 0)
    {
        fprintf(stderr, "%s has db_type=%s - this harness needs db_type=mssql\n",
                ini_path, config->db_type);
        return -1;
    }
    if (logger_init_str(err_l, config->error_log_file_name, config->error_log_file_max_size,
                        config->error_log_file_rotation_number, config->error_log_level) != 0)
        return -1;
    ctx->error_logger = err_l;
    if (logger_init_str2(conn_l, config->connection_log_file_name, config->connection_log_file_max_size,
                         config->connection_log_file_rotation_number, config->connection_log_level,
                         err_l) != 0)
        return -1;
    ctx->connection_logger = conn_l;
    if (logger_init_str2(pool_l, config->connectionpool_log_file_name,
                         config->connectionpool_log_file_max_size,
                         config->connectionpool_log_file_rotation_number,
                         config->connectionpool_log_level, err_l) != 0)
        return -1;
    ctx->connectionpool_logger = pool_l;
    if (logger_init_str2(sel_l, config->select_log_file_name, config->select_log_file_max_size,
                         config->select_log_file_rotation_number, config->select_log_level,
                         err_l) != 0)
        return -1;
    ctx->select_logger = sel_l;
    if (logger_init_str2(meta_l, config->Metadata_log_file_name, config->Metadata_log_file_max_size,
                         config->Metadata_log_file_rotation_number, config->Metadata_log_level,
                         err_l) != 0)
        return -1;
    ctx->Metadata_logger = meta_l;
    return 0;
}

static const db_driver_t *g_drv;
static oci_context_t     *g_ctx;
static int                g_fail;

static void fail_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void fail_line(const char *fmt, ...)
{
    if (!g_fail) printf("FAILED\n");
    va_list ap;
    va_start(ap, fmt);
    printf("    ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_fail = 1;
}

static int describe(const char *owner, const char *table, col_metadata_t *cols,
                    int *n, int max_cols, metadata_request_t *req_out)
{
    metadata_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.owner, sizeof(req.owner), "%s", owner);
    snprintf(req.table_name, sizeof(req.table_name), "%s", table);
    *n = 0;
    int rc = g_drv->describe_table(g_ctx, &req, cols, n, max_cols);
    if (req_out) *req_out = req;
    return rc;
}

static const col_metadata_t *find(const col_metadata_t *cols, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(cols[i].col_name, name) == 0) return &cols[i];
    return NULL;
}

/* name type len prec scale nullable default(NULL = don't check, "" = none) */
static void check_col(const col_metadata_t *cols, int n, const char *name,
                      const char *type, int len, int prec, int scale,
                      const char *nullable, const char *dflt)
{
    const col_metadata_t *c = find(cols, n, name);
    if (!c) { fail_line("%s: missing", name); return; }
    if (strcmp(c->data_type, type) != 0 || c->data_length != len ||
        c->data_precision != prec || c->data_scale != scale ||
        strcmp(c->nullable, nullable) != 0 ||
        (dflt && strcmp(c->data_default, dflt) != 0))
        fail_line("%s: want %s len=%d prec=%d scale=%d null=%s default='%s'\n"
                  "      got  %s len=%d prec=%d scale=%d null=%s default='%s'",
                  name, type, len, prec, scale, nullable, dflt ? dflt : "(any)",
                  c->data_type, c->data_length, c->data_precision, c->data_scale,
                  c->nullable, c->data_default);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ini_path = argc > 1 ? argv[1] : CONFIG_INI_MSSQL;
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, pool_l, sel_l, meta_l;
    if (init_ctx(ini_path, &ctx, &config, &err_l, &conn_l, &pool_l, &sel_l, &meta_l) != 0)
    {
        printf("INIT FAILED\nFAIL\n");
        return 1;
    }
    config.use_connection_pool = 0;
    g_drv = mssql_driver_get();
    g_ctx = &ctx;

    static col_metadata_t cols[MAX_TABLE_COLUMNS];
    int n = 0;
    metadata_request_t req;

    /* ---- Test 1 ---- */
    printf("Test 1 (standalone connect)          ... ");
    if (g_drv->connect(&ctx) != 0 || !ctx.svchp)
    {
        printf("FAILED - connect(); see %s\nFAIL\n", config.connection_log_file_name);
        return 1;
    }
    printf("OK\n");

    /* ---- Test 2 ---- */
    printf("Test 2 (OCI_FIELD_TEST columns)      ... ");
    g_fail = 0;
    if (describe("DATA_MANAGER", "OCI_FIELD_TEST", cols, &n, MAX_TABLE_COLUMNS, NULL) != 0)
        fail_line("describe_table failed - see %s", config.Metadata_log_file_name);
    else
    {
        static const char *ORDER[] = {
            "FIELD_TEST_ID", "NUMBER_COL", "NUMBER_P_S_COL", "FLOAT_COL", "BINARY_FLOAT_COL",
            "BINARY_DOUBLE_COL", "CHAR_COL", "VARCHAR2_COL", "NCHAR_COL", "NVARCHAR2_COL",
            "DATE_COL", "TIMESTAMP_COL", "TIMESTAMP_TZ_COL", "TIMESTAMP_LTZ_COL",
            "INTERVAL_YM_COL", "INTERVAL_DS_COL", "RAW_COL", "CLOB_COL", "NCLOB_COL", "BLOB_COL" };
        if (n != 20) fail_line("want 20 columns, got %d", n);
        for (int i = 0; i < 20 && i < n; i++)
            if (strcmp(cols[i].col_name, ORDER[i]) != 0)
                fail_line("column %d: want %s, got %s", i + 1, ORDER[i], cols[i].col_name);

        check_col(cols, n, "FIELD_TEST_ID",    "BIGINT",         8,  19,  0, "N", "IDENTITY(1,1)");
        check_col(cols, n, "NUMBER_COL",       "BIGINT",         8,  19,  0, "Y", "");
        check_col(cols, n, "NUMBER_P_S_COL",   "DECIMAL",        9,  10,  2, "Y", "");
        check_col(cols, n, "FLOAT_COL",        "FLOAT",          8,  53, -1, "Y", "");
        check_col(cols, n, "CHAR_COL",         "CHAR",          10,  -1, -1, "Y", "");
        check_col(cols, n, "VARCHAR2_COL",     "VARCHAR",      100,  -1, -1, "Y", "");
        check_col(cols, n, "NVARCHAR2_COL",    "NVARCHAR",     100,  -1, -1, "Y", "");
        check_col(cols, n, "DATE_COL",         "DATETIME2",      6,  -1,  0, "Y", "");
        check_col(cols, n, "TIMESTAMP_COL",    "DATETIME2",      8,  -1,  6, "Y", "");
        check_col(cols, n, "TIMESTAMP_TZ_COL", "DATETIMEOFFSET",10,  -1,  6, "Y", "");
        check_col(cols, n, "RAW_COL",          "VARBINARY",     50,  -1, -1, "Y", "");
        check_col(cols, n, "CLOB_COL",         "VARCHAR(MAX)",   0,  -1, -1, "Y", "");
        check_col(cols, n, "NCLOB_COL",        "NVARCHAR(MAX)",  0,  -1, -1, "Y", "");
        check_col(cols, n, "BLOB_COL",         "VARBINARY(MAX)", 0,  -1, -1, "Y", "");
    }
    if (!g_fail) printf("OK (20 columns, in order)\n");
    failed |= g_fail;

    /* ---- Test 3 ---- */
    printf("Test 3 (classes and LOB kinds)       ... ");
    g_fail = 0;
    {
        const db_dialect_t *dl = g_drv->dialect;
        int lobs = 0;
        for (int i = 0; i < n; i++)
        {
            db_type_class_t c = db_type_class(cols[i].data_type);
            db_lob_kind_t   k = db_type_lob_kind(cols[i].data_type);
            if (c == DB_TYPE_CLASS_OTHER)
                fail_line("%s (%s): class OTHER", cols[i].col_name, cols[i].data_type);
            if ((dl->lob_placeholder(cols[i].data_type) != NULL) != (k != DB_LOB_NONE))
                fail_line("%s (%s): lob_placeholder and lob_kind disagree",
                          cols[i].col_name, cols[i].data_type);
            if (c == DB_TYPE_CLASS_LOB) lobs++;
        }
        const col_metadata_t *c;
        if ((c = find(cols, n, "CLOB_COL"))  && db_type_lob_kind(c->data_type) != DB_LOB_TEXT)
            fail_line("CLOB_COL: want lob kind TEXT");
        if ((c = find(cols, n, "NCLOB_COL")) && db_type_lob_kind(c->data_type) != DB_LOB_TEXT)
            fail_line("NCLOB_COL: want lob kind TEXT");
        if ((c = find(cols, n, "BLOB_COL"))  && db_type_lob_kind(c->data_type) != DB_LOB_BINARY)
            fail_line("BLOB_COL: want lob kind BINARY");
        if ((c = find(cols, n, "DATE_COL"))  && db_type_class(c->data_type) != DB_TYPE_CLASS_TIMESTAMP)
            fail_line("DATE_COL (datetime2(0)): want class TIMESTAMP");
        if (lobs != 3) fail_line("want 3 LOB columns, got %d", lobs);
        if (!g_fail) printf("OK (no OTHER; 3 LOB columns, kinds match the placeholders)\n");
    }
    failed |= g_fail;

    /* ---- Test 4 ---- */
    printf("Test 4 (owner resolved)              ... ");
    g_fail = 0;
    if (describe("", "OCI_SESSION", cols, &n, MAX_TABLE_COLUMNS, &req) != 0)
        fail_line("describe_table failed - see %s", config.Metadata_log_file_name);
    else if (strcmp(req.owner, "DATA_MANAGER") != 0)
        fail_line("owner: want DATA_MANAGER, got '%s'", req.owner);
    if (!g_fail) printf("OK (owner=%s, %d columns)\n", req.owner, n);
    failed |= g_fail;

    /* ---- Test 5 ---- */
    printf("Test 5 (lower-case names)            ... ");
    g_fail = 0;
    if (describe("data_manager", "oci_field_test", cols, &n, MAX_TABLE_COLUMNS, &req) != 0)
        fail_line("describe_table failed");
    else if (strcmp(req.owner, "DATA_MANAGER") != 0 || strcmp(req.table_name, "OCI_FIELD_TEST") != 0)
        fail_line("names not upper-cased in place: %s.%s", req.owner, req.table_name);
    else if (n != 20)
        fail_line("want 20 columns, got %d", n);
    if (!g_fail) printf("OK (DATA_MANAGER.OCI_FIELD_TEST)\n");
    failed |= g_fail;

    /* ---- Test 6 ---- */
    printf("Test 6 (unknown table)               ... ");
    g_fail = 0;
    if (describe("DATA_MANAGER", "NO_SUCH_TABLE", cols, &n, MAX_TABLE_COLUMNS, NULL) == 0 || n != 0)
        fail_line("want -1 and no columns, got %d columns", n);
    if (describe("", "NO_SUCH_TABLE", cols, &n, MAX_TABLE_COLUMNS, NULL) == 0 || n != 0)
        fail_line("owner lookup: want -1 and no columns, got %d columns", n);
    if (!g_fail) printf("OK (-1, with and without owner)\n");
    failed |= g_fail;

    /* ---- Test 7 ---- */
    printf("Test 7 (max_cols 5)                  ... ");
    g_fail = 0;
    if (describe("DATA_MANAGER", "OCI_FIELD_TEST", cols, &n, 5, NULL) != 0)
        fail_line("describe_table failed");
    else if (n != 5 || strcmp(cols[4].col_name, "BINARY_FLOAT_COL") != 0)
        fail_line("want the first 5 columns, got %d", n);
    if (!g_fail) printf("OK (first 5 columns)\n");
    failed |= g_fail;

    /* ---- Test 8 ---- */
    printf("Test 8 (gateway and test tables)     ... ");
    g_fail = 0;
    {
        static const char *TABLES[] = {
            "AUTH_SOURCE", "APP_USER", "ROLE", "PERMISSION", "USER_ROLE", "ROLE_PERMISSION",
            "OCI_SESSION", "OCI_METRICS", "AUDIT_TRAIL", "OCI_FIELD_TEST",
            "UNIT_TEST_FIELD_TEST", "OCI_LOB_TEST", "OCI_CLOB_TEST" };
        int ntab = (int)(sizeof(TABLES) / sizeof(TABLES[0]));
        int total = 0;
        for (int t = 0; t < ntab; t++)
        {
            if (describe("DATA_MANAGER", TABLES[t], cols, &n, MAX_TABLE_COLUMNS, NULL) != 0)
            {
                fail_line("%s: describe_table failed", TABLES[t]);
                continue;
            }
            total += n;
            for (int i = 0; i < n; i++)
                if (db_type_class(cols[i].data_type) == DB_TYPE_CLASS_OTHER)
                    fail_line("%s.%s (%s): class OTHER", TABLES[t], cols[i].col_name,
                              cols[i].data_type);
        }
        if (describe("DATA_MANAGER", "APP_USER", cols, &n, MAX_TABLE_COLUMNS, NULL) == 0)
        {
            const col_metadata_t *c = find(cols, n, "ENABLED");
            if (!c || strstr(c->data_default, "'Y'") == NULL)
                fail_line("APP_USER.ENABLED: want default ('Y'), got '%s'", c ? c->data_default : "?");
            c = find(cols, n, "CREATED_TS");
            if (!c || strstr(c->data_default, "sysdatetime") == NULL)
                fail_line("APP_USER.CREATED_TS: want default (sysdatetime()), got '%s'",
                          c ? c->data_default : "?");
            c = find(cols, n, "USER_ID");
            if (!c || strcmp(c->data_default, "IDENTITY(1,1)") != 0 || strcmp(c->nullable, "N") != 0)
                fail_line("APP_USER.USER_ID: want NOT NULL IDENTITY(1,1)");
        }
        if (!g_fail) printf("OK (%d tables, %d columns, none OTHER)\n", ntab, total);
    }
    failed |= g_fail;

    mssql_end_tran(ctx.svchp, 0, &conn_l, "harness end");
    g_drv->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&meta_l);
    logger_close(&sel_l);
    logger_close(&pool_l);
    logger_close(&conn_l);
    logger_close(&err_l);
    return failed ? 1 : 0;
}
