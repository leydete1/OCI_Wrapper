/*
 * MSSQL_Driver_Pool_Test.c   (driver_mssql Stage 2, point P10)
 *
 * Checks driver_mssql's connection pool (MSSQL_Pool.c) against a real
 * SQL Server. Pool settings are forced here so the result does not
 * depend on config_mssql.ini's sizing: min 2, max 4, borrow timeout
 * 2 s, validation on borrow and rollback on return both on.
 *
 * Usage:  MSSQL_Driver_Pool_Test [config_mssql.ini]
 *
 * Reserved UNIT_TEST_FIELD_TEST.NUMBER_COL range: 999201-999299.
 *
 *   Test 1  pool connect: pool_handle set, master ctx holds no connection
 *   Test 2  borrow 4 (the maximum): 4 different slots, 4 different @@SPID
 *   Test 3  a 5th borrow times out after ~2 s and returns -1
 *   Test 4  8 threads x 50 borrow/release: no slot ever lent twice at once
 *   Test 5  rollback on return: uncommitted INSERT is gone (independent
 *           check), and the slot is back in autocommit (Z7, 25 Oct)
 *   Test 6  session_is_alive = 1 on a borrowed session
 *   Test 7  reconnect_session closes the caller's connection and hands
 *           back a different live one (different @@SPID)
 *   Test 8  health_check reports 0 failures
 *
 * Prints PASS or FAIL as the last line; exit code 0 / 1.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI_MSSQL "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config_mssql.ini"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include <sql.h>
#include <sqlext.h>

#include "Connection.h"
#include "db_driver.h"
#include "driver_mssql.h"
#include "MSSQL_Pool.h"
#include "ini_reader.h"
#include "logger.h"

#define POOL_MAX   4
#define THREADS    8
#define CYCLES     50

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

/* Z7: a write through the driver's dml_execute (manual-commit first). */
static int drv_write(const db_driver_t *d, oci_context_t *c, logger_t *lg, const char *sql)
{
    db_dml_request_t r = { sql, 0, NULL };
    int  n = 0;
    char e[1024] = "";
    int  rc = d->dml_execute(c, lg, &r, &n, e, sizeof(e));
    if (rc != 0) fprintf(stderr, "  drv_write failed: %s -- %s\n", sql, e);
    return rc;
}

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

static long raw_long(void *hdbc, const char *sql)
{
    SQLHSTMT st = SQL_NULL_HSTMT;
    SQLINTEGER v = -1;
    SQLLEN ind = 0;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)hdbc, &st))) return -1;
    if (!(SQL_SUCCEEDED(SQLExecDirect(st, (SQLCHAR *)sql, SQL_NTS)) &&
          SQL_SUCCEEDED(SQLFetch(st)) &&
          SQL_SUCCEEDED(SQLGetData(st, 1, SQL_C_SLONG, &v, 0, &ind)) && ind != SQL_NULL_DATA))
        v = -1;
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return (long)v;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- Test 4 shared state ---- */
static const db_driver_t *g_driver;
static oci_context_t     *g_base;
static int                g_owner[POOL_MAX];     /* 1 = lent out right now */
static int                g_double_lend;
static int                g_borrow_fail;
static int                g_ping_fail;

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < CYCLES; i++)
    {
        oci_context_t w;
        memset(&w, 0, sizeof(w));
        if (g_driver->get_session(g_base, &w) != 0)
        {
            __atomic_add_fetch(&g_borrow_fail, 1, __ATOMIC_SEQ_CST);
            continue;
        }
        int idx = w.pool_slot_index;
        int expected = 0;
        if (idx < 0 || idx >= POOL_MAX ||
            !__atomic_compare_exchange_n(&g_owner[idx], &expected, 1, 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            __atomic_add_fetch(&g_double_lend, 1, __ATOMIC_SEQ_CST);

        if (i % 10 == 0 && !mssql_ping(w.svchp, NULL))
            __atomic_add_fetch(&g_ping_fail, 1, __ATOMIC_SEQ_CST);

        if (idx >= 0 && idx < POOL_MAX)
            __atomic_store_n(&g_owner[idx], 0, __ATOMIC_SEQ_CST);
        g_driver->release_session(g_base, &w);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ini_path = argc > 1 ? argv[1] : CONFIG_INI_MSSQL;
    int failed = 0;

    oci_context_t base; app_config_t config;
    logger_t err_l, conn_l, pool_l;
    if (init_ctx(ini_path, &base, &config, &err_l, &conn_l, &pool_l) != 0)
    {
        printf("INIT FAILED\nFAIL\n");
        return 1;
    }
    config.use_connection_pool              = 1;
    config.pool_min_size                    = 2;
    config.pool_max_size                    = POOL_MAX;
    config.pool_increment                   = 1;
    config.pool_connection_timeout          = 2;
    config.connection_validation_on_borrow  = 1;
    config.rollback_on_return_to_pool       = 1;
    config.autocommit_mode                  = 0;

    const db_driver_t *driver = mssql_driver_get();
    g_driver = driver;
    g_base   = &base;

    void *env = mssql_env_acquire(&conn_l);
    void *chk = env ? mssql_open_connection(env, &config, &conn_l) : NULL;
    if (!chk)
    {
        printf("FAILED - checker connection could not open; see %s\nFAIL\n",
               config.connection_log_file_name);
        return 1;
    }
    raw_exec(chk, "DELETE FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST WHERE NUMBER_COL BETWEEN 999201 AND 999299");
    mssql_end_tran(chk, 1, &conn_l, "pre-clean");
    /* The checker reads with locks for the rollback checks (READ
     * COMMITTED SNAPSHOT would never show an uncommitted row anyway): a
     * row still locked by an open transaction makes that read time out
     * after 2 s instead of reading 0. */
    raw_exec(chk, "SET LOCK_TIMEOUT 2000");

    /* ---- Test 1 ---- */
    printf("Test 1 (pool connect)                ... ");
    if (driver->connect(&base) != 0)
    {
        printf("FAILED - connect(); see %s\nFAIL\n", config.connectionpool_log_file_name);
        return 1;
    }
    if (!base.pool_handle || base.svchp)
    {
        printf("FAILED - pool_handle=%p svchp=%p\n", base.pool_handle, base.svchp);
        failed = 1;
    }
    else
        printf("OK (pool_handle populated)\n");

    /* ---- Test 2 ---- */
    printf("Test 2 (borrow all %d)                ... ", POOL_MAX);
    oci_context_t w[POOL_MAX];
    long spid[POOL_MAX];
    int got = 0, distinct = 1;
    for (int i = 0; i < POOL_MAX; i++)
    {
        memset(&w[i], 0, sizeof(w[i]));
        if (driver->get_session(&base, &w[i]) != 0) break;
        spid[i] = raw_long(w[i].svchp, "SELECT @@SPID");
        got++;
    }
    for (int i = 0; i < got; i++)
        for (int j = i + 1; j < got; j++)
            if (w[i].pool_slot_index == w[j].pool_slot_index || spid[i] == spid[j] || spid[i] < 0)
                distinct = 0;
    if (got != POOL_MAX || !distinct)
    {
        printf("FAILED - borrowed %d, distinct slots/SPIDs=%d\n", got, distinct);
        failed = 1;
    }
    else
        printf("OK (4 slots, SPIDs %ld %ld %ld %ld)\n", spid[0], spid[1], spid[2], spid[3]);

    /* ---- Test 3 ---- */
    printf("Test 3 (5th borrow times out)        ... ");
    {
        oci_context_t extra;
        memset(&extra, 0, sizeof(extra));
        double t0 = now_s();
        int rc = driver->get_session(&base, &extra);
        double dt = now_s() - t0;
        if (rc == 0)
        {
            printf("FAILED - a 5th session was lent\n");
            driver->release_session(&base, &extra);
            failed = 1;
        }
        else if (dt < 1.5 || dt > 4.0)
        {
            printf("FAILED - returned after %.2f s (expected about 2)\n", dt);
            failed = 1;
        }
        else
            printf("OK (-1 after %.2f s)\n", dt);
    }
    for (int i = 0; i < got; i++)
        driver->release_session(&base, &w[i]);

    /* ---- Test 4 ---- */
    printf("Test 4 (%d threads x %d cycles)        ... ", THREADS, CYCLES);
    {
        pthread_t t[THREADS];
        for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, worker, NULL);
        for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
        if (g_double_lend || g_borrow_fail || g_ping_fail)
        {
            printf("FAILED - double lends=%d borrow failures=%d ping failures=%d\n",
                   g_double_lend, g_borrow_fail, g_ping_fail);
            failed = 1;
        }
        else
            printf("OK (%d borrows, no slot lent twice)\n", THREADS * CYCLES);
    }

    /* ---- Test 5 ---- */
    printf("Test 5 (rollback on return)          ... ");
    {
        oci_context_t s;
        memset(&s, 0, sizeof(s));
        long n = -1;
        if (driver->get_session(&base, &s) != 0 ||
            drv_write(driver, &s, &conn_l, "INSERT INTO DATA_MANAGER.UNIT_TEST_FIELD_TEST (NUMBER_COL) VALUES (999201)") != 0)
        {
            printf("FAILED - borrow or INSERT failed\n");
            failed = 1;
        }
        else
        {
            void *slot_hdbc = s.svchp;
            driver->release_session(&base, &s);
            n = raw_long(chk, "SELECT COUNT(*) FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST WITH (READCOMMITTEDLOCK) WHERE NUMBER_COL = 999201");
            int manual = mssql_tx_in_manual(slot_hdbc);    /* Z7: back in autocommit */
            if (n != 0)      { printf("FAILED - count %ld after release (expected 0)\n", n); failed = 1; }
            else if (manual) { printf("FAILED - released slot left in manual-commit (Z7)\n"); failed = 1; }
            else               printf("OK (uncommitted row gone; slot back in autocommit)\n");
        }
    }

    /* ---- Tests 6 and 7 ---- */
    {
        oci_context_t s;
        memset(&s, 0, sizeof(s));
        printf("Test 6 (session_is_alive)            ... ");
        if (driver->get_session(&base, &s) != 0)
        {
            printf("FAILED - borrow failed\n");
            printf("Test 7 (reconnect_session)           ... SKIPPED\n");
            failed = 1;
        }
        else
        {
            int alive = driver->session_is_alive(&s);
            if (alive != 1) { printf("FAILED - returned %d\n", alive); failed = 1; }
            else              printf("OK (1)\n");

            printf("Test 7 (reconnect_session)           ... ");
            long a = raw_long(s.svchp, "SELECT @@SPID");
            int rc = driver->reconnect_session(&base, &s);
            long b = rc == 0 ? raw_long(s.svchp, "SELECT @@SPID") : -1;
            if (rc != 0 || a < 0 || b < 0 || a == b)
            {
                printf("FAILED - rc=%d SPID before=%ld after=%ld\n", rc, a, b);
                failed = 1;
            }
            else
                printf("OK (SPID %ld -> %ld)\n", a, b);
            if (rc == 0) driver->release_session(&base, &s);
        }
    }

    /* ---- Test 8 ---- */
    printf("Test 8 (health_check)                ... ");
    {
        int f = driver->health_check(&base);
        if (f != 0) { printf("FAILED - %d slot(s) could not be recovered\n", f); failed = 1; }
        else          printf("OK (0 failures)\n");
    }

    /* ---- Cleanup ---- */
    raw_exec(chk, "DELETE FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST WHERE NUMBER_COL BETWEEN 999201 AND 999299");
    mssql_end_tran(chk, 1, &conn_l, "cleanup");
    mssql_close_connection(chk, &conn_l);
    mssql_env_release();
    driver->disconnect(&base);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&pool_l);
    logger_close(&conn_l);
    logger_close(&err_l);
    return failed ? 1 : 0;
}
