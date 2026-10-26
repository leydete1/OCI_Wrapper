/*
 * MSSQL_Driver_Connect_Test.c   (driver_mssql Stage 2, point P10)
 *
 * Checks driver_mssql's connect / disconnect / commit / rollback and
 * its error text against a real SQL Server (the DataManager database
 * from Create_MSSQL_Test_Objects.sql). Same A/B idea as the Oracle
 * harnesses: whatever the driver claims is checked with plain ODBC
 * calls on a second, independent connection.
 *
 * Usage:  MSSQL_Driver_Connect_Test [config_mssql.ini]
 *         (default: .../OCI_Wrapper/Props/config_mssql.ini)
 *
 * Reserved UNIT_TEST_FIELD_TEST.NUMBER_COL range: 999101-999199.
 *
 *   Test 1  standalone connect: handles populated, SELECT 1 answers
 *   Test 2  wrong password: connect fails; the error text is
 *           "SQLSTATE 28000 [native 18456]: Login failed ..."
 *   Test 3  connection string logged with PWD=***, never the password
 *   Test 4  commit: an INSERT is visible from an independent connection
 *   Test 5  rollback: an INSERT is gone, checked independently
 *           (Z7, 25 Oct: Tests 4-5 write through dml_execute, which
 *           switches to manual-commit; Test 4 also checks the connection
 *           is back in autocommit after the commit)
 *   Test 6  commit with nothing pending returns 0
 *   Test 7  a failed commit returns -1, never DB_TX_RETRYABLE (D2)
 *
 * Prints PASS or FAIL as the last line; exit code 0 / 1.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI_MSSQL "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config_mssql.ini"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <sql.h>
#include <sqlext.h>

#include "Connection.h"
#include "db_driver.h"
#include "driver_mssql.h"
#include "MSSQL_Pool.h"
#include "ini_reader.h"
#include "logger.h"

static int init_ctx(const char *ini_path, oci_context_t *ctx, app_config_t *config,
                    logger_t *err_l, logger_t *conn_l, logger_t *pool_l)
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
    return 0;
}

/* Runs one statement on hdbc with plain ODBC. 0 = ok. */
static int raw_exec(void *hdbc, const char *sql)
{
    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)hdbc, &st))) return -1;
    SQLRETURN r = SQLExecDirect(st, (SQLCHAR *)sql, SQL_NTS);
    int rc = (SQL_SUCCEEDED(r) || r == SQL_NO_DATA) ? 0 : -1;
    if (rc != 0)
    {
        char d[1024];
        mssql_diag(SQL_HANDLE_STMT, st, d, sizeof(d));
        fprintf(stderr, "  raw_exec failed: %s -- %s\n", sql, d);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return rc;
}

/* Z7: a write goes through the driver's dml_execute, which switches the
 * connection to manual-commit first - a raw INSERT on a connection that
 * now sits in autocommit would commit at once. 0 = ok. */
static int drv_write(const db_driver_t *d, oci_context_t *c, logger_t *lg, const char *sql)
{
    db_dml_request_t r = { sql, 0, NULL };
    int  n = 0;
    char e[1024] = "";
    int  rc = d->dml_execute(c, lg, &r, &n, e, sizeof(e));
    if (rc != 0) fprintf(stderr, "  drv_write failed: %s -- %s\n", sql, e);
    return rc;
}

/* One whole number from a single-row query. -1 on failure. */
static long raw_long(void *hdbc, const char *sql)
{
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLINTEGER v = -1;
    SQLLEN ind = 0;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)hdbc, &st))) return -1;
    if (SQL_SUCCEEDED(SQLExecDirect(st, (SQLCHAR *)sql, SQL_NTS)) &&
        SQL_SUCCEEDED(SQLFetch(st)) &&
        SQL_SUCCEEDED(SQLGetData(st, 1, SQL_C_SLONG, &v, 0, &ind)) && ind != SQL_NULL_DATA)
        ;
    else
        v = -1;
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return (long)v;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ini_path = argc > 1 ? argv[1] : CONFIG_INI_MSSQL;
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, pool_l;
    if (init_ctx(ini_path, &ctx, &config, &err_l, &conn_l, &pool_l) != 0)
    {
        printf("INIT FAILED\nFAIL\n");
        return 1;
    }
    config.use_connection_pool = 0;               /* standalone for this harness */

    const db_driver_t *driver = mssql_driver_get();

    /* Independent checker connection - plain ODBC, never the driver's. */
    void *env = mssql_env_acquire(&conn_l);
    void *chk = env ? mssql_open_connection(env, &config, &conn_l) : NULL;
    if (!chk)
    {
        printf("FAILED - checker connection could not open; see %s\nFAIL\n",
               config.connection_log_file_name);
        return 1;
    }
    raw_exec(chk, "DELETE FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST WHERE NUMBER_COL BETWEEN 999101 AND 999199");
    mssql_end_tran(chk, 1, &conn_l, "pre-clean");
    /* The checker reads with locks for the rollback checks (READ
     * COMMITTED SNAPSHOT would never show an uncommitted row anyway): a
     * row still locked by an open transaction makes that read time out
     * after 2 s instead of reading 0. */
    raw_exec(chk, "SET LOCK_TIMEOUT 2000");

    /* ---- Test 1 ---- */
    printf("Test 1 (standalone connect)          ... ");
    int connected = 0;
    if (driver->connect(&ctx) != 0)
    {
        printf("FAILED - connect(); see %s\n", config.connection_log_file_name);
        failed = 1;
    }
    else if (!ctx.envhp || !ctx.svchp || ctx.pool_handle)
    {
        printf("FAILED - connect() returned 0 but handles are wrong\n");
        failed = 1;
        connected = 1;
    }
    else if (!mssql_ping(ctx.svchp, &conn_l))
    {
        printf("FAILED - SELECT 1 on the new connection did not answer\n");
        failed = 1;
        connected = 1;
    }
    else
    {
        printf("OK (envhp/svchp populated, SELECT 1 answered)\n");
        connected = 1;
    }

    /* ---- Test 2 ---- */
    printf("Test 2 (wrong password)              ... ");
    {
        app_config_t bad = config;
        snprintf(bad.password, sizeof(bad.password), "%s", "not-the-password-2026");
        oci_context_t bctx;
        memset(&bctx, 0, sizeof(bctx));
        bctx.ini = &bad;
        bctx.connection_logger = &conn_l;
        bctx.pool_slot_index = -1;

        int rc = driver->connect(&bctx);

        /* The text the driver logged, reproduced with the same helper. */
        char diag[1024] = "";
        SQLHDBC h = SQL_NULL_HDBC;
        char cs[1024];
        if (SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, (SQLHENV)env, &h)) &&
            mssql_connection_string(&bad, 0, cs, sizeof(cs)) == 0)
        {
            SQLSetConnectAttr(h, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER)(intptr_t)15, 0);
            if (!SQL_SUCCEEDED(SQLDriverConnect(h, NULL, (SQLCHAR *)cs, SQL_NTS, NULL, 0,
                                                NULL, SQL_DRIVER_NOPROMPT)))
                mssql_diag(SQL_HANDLE_DBC, h, diag, sizeof(diag));
            else
                SQLDisconnect(h);
        }
        if (h) SQLFreeHandle(SQL_HANDLE_DBC, h);

        if (rc == 0)
        {
            printf("FAILED - connect() succeeded with a wrong password\n");
            driver->disconnect(&bctx);
            failed = 1;
        }
        else if (strncmp(diag, "SQLSTATE 28000 [native 18456]: ", 31) != 0)
        {
            printf("FAILED - unexpected error text: '%s'\n", diag);
            failed = 1;
        }
        else
            printf("OK (%.70s)\n", diag);
    }

    /* ---- Test 3 ---- */
    printf("Test 3 (password masked in logs)     ... ");
    {
        char masked[1024];
        if (mssql_connection_string(&config, 1, masked, sizeof(masked)) != 0)
        {
            printf("FAILED - connection string did not fit\n");
            failed = 1;
        }
        else if (!strstr(masked, "PWD=***;") ||
                 (config.password[0] && strstr(masked, config.password)))
        {
            printf("FAILED - masked string is '%s'\n", masked);
            failed = 1;
        }
        else
            printf("OK (%.60s...)\n", masked);
    }

    if (connected)
    {
        /* ---- Test 4 ---- */
        printf("Test 4 (commit + independent check)  ... ");
        long n = -1;
        if (drv_write(driver, &ctx, &conn_l, "INSERT INTO DATA_MANAGER.UNIT_TEST_FIELD_TEST (NUMBER_COL) VALUES (999101)") != 0 ||
            driver->commit(&ctx, &conn_l) != 0 ||
            (n = raw_long(chk, "SELECT COUNT(*) FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = 999101")) != 1)
        {
            printf("FAILED - count after commit = %ld (expected 1)\n", n);
            failed = 1;
        }
        else if (mssql_tx_in_manual(ctx.svchp) != 0)
        {
            printf("FAILED - connection not back in autocommit after commit (Z7)\n");
            failed = 1;
        }
        else
            printf("OK (1 row, commit confirmed; back in autocommit)\n");

        /* ---- Test 5 ---- */
        printf("Test 5 (rollback + independent check)... ");
        n = -1;
        if (drv_write(driver, &ctx, &conn_l, "INSERT INTO DATA_MANAGER.UNIT_TEST_FIELD_TEST (NUMBER_COL) VALUES (999102)") != 0 ||
            driver->rollback(&ctx, &conn_l) != 0 ||
            (n = raw_long(chk, "SELECT COUNT(*) FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST WITH (READCOMMITTEDLOCK) WHERE NUMBER_COL = 999102")) != 0)
        {
            printf("FAILED - count after rollback = %ld (expected 0)\n", n);
            failed = 1;
        }
        else
            printf("OK (0 rows, rollback confirmed)\n");

        /* ---- Test 6 ---- */
        printf("Test 6 (commit, nothing pending)     ... ");
        int rc6 = driver->commit(&ctx, &conn_l);
        if (rc6 != 0) { printf("FAILED - rc=%d (expected 0)\n", rc6); failed = 1; }
        else           printf("OK\n");
    }
    else
        printf("Tests 4-6 skipped - no connection\n");

    /* ---- Test 7 ---- */
    printf("Test 7 (failed commit not retryable) ... ");
    {
        void *dead = mssql_open_connection(env, &config, &conn_l);
        if (!dead)
        {
            printf("FAILED - could not open the extra connection\n");
            failed = 1;
        }
        else
        {
            SQLDisconnect((SQLHDBC)dead);          /* handle stays allocated, not connected */
            oci_context_t dctx;
            memset(&dctx, 0, sizeof(dctx));
            dctx.svchp = dead;
            int rc7 = driver->commit(&dctx, &conn_l);
            SQLFreeHandle(SQL_HANDLE_DBC, (SQLHDBC)dead);
            if (rc7 == DB_TX_RETRYABLE) { printf("FAILED - returned DB_TX_RETRYABLE\n"); failed = 1; }
            else if (rc7 != -1)         { printf("FAILED - rc=%d (expected -1)\n", rc7); failed = 1; }
            else                          printf("OK (-1, error logged)\n");
        }
    }

    /* ---- Cleanup ---- */
    raw_exec(chk, "DELETE FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST WHERE NUMBER_COL BETWEEN 999101 AND 999199");
    mssql_end_tran(chk, 1, &conn_l, "cleanup");
    mssql_close_connection(chk, &conn_l);
    mssql_env_release();
    if (connected) driver->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&pool_l);
    logger_close(&conn_l);
    logger_close(&err_l);
    return failed ? 1 : 0;
}
