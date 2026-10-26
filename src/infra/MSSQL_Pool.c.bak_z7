/*
 * MSSQL_Pool.c
 *
 * SQL Server connections and connection pool for driver_mssql
 * (driver_mssql Stage 2, points P4/P5/P7, 2026-10-18). See MSSQL_Pool.h
 * for the contract; the pool logic follows Connection_Pool.c step for
 * step (same keys, same borrow / grow / wait / validate / release /
 * health-check order, same log lines where they apply), over ODBC.
 *
 * Differences from Connection_Pool.c, on purpose:
 *   - reconnect_session closes the caller's connection and opens a new
 *     one. (Connection_Pool.c releases and re-borrows, which can hand
 *     the same session straight back; a caller only reconnects when it
 *     believes its session is dead.)
 *   - no NLS ALTER SESSION: SQL Server has no NLS settings; dates are
 *     converted in SQL by the dialect (M5).
 *   - no POOL_INIT metrics record: the pool start-up is logged instead
 *     (metrics_write() for pool start-up only ever reached the metrics
 *     log file, not the metrics table).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include <sql.h>
#include <sqlext.h>

#include "MSSQL_Pool.h"
#include "Connection.h"
#include "ini_reader.h"
#include "ctx_utils.h"     /* copy_shared_ctx_fields() */
#include "logger.h"

#define MSSQL_ODBC_DRIVER "ODBC Driver 18 for SQL Server"

/* ================================================================== */
/*  Pool structures (private - ctx->pool_handle is opaque to callers)  */
/* ================================================================== */
typedef struct {
    SQLHDBC hdbc;          /* NULL = slot not open                    */
    int     in_use;
    time_t  created;
    time_t  last_used;
} mssql_slot_t;

typedef struct {
    SQLHENV          henv;
    mssql_slot_t    *slots;
    int              slot_count;     /* = pool_max_size                */
    int              slots_open;
    pthread_mutex_t  mutex;
    pthread_cond_t   cond;

    app_config_t     cfg;            /* copy taken at connect: credentials,
                                        server, timeouts               */
    int  pool_min_size;
    int  pool_max_size;
    int  connection_timeout;
    int  session_idle_timeout;
    int  session_max_lifetime;
    int  retries_on_connection_failure;
    int  connection_validation_on_borrow;
    int  rollback_on_return_to_pool;
    char nls_date_format[64];        /* handed to worker contexts, as the
                                        Oracle pool does                */
} mssql_pool_t;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ================================================================== */
/*  M7 - diagnostic text                                               */
/* ================================================================== */
void mssql_diag(int handle_type, void *handle, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!handle) return;

    size_t used = 0;
    for (SQLSMALLINT rec = 1; rec < 20; rec++)
    {
        SQLCHAR     state[6] = {0};
        SQLINTEGER  native   = 0;
        SQLCHAR     msg[1024];
        SQLSMALLINT len      = 0;

        SQLRETURN r = SQLGetDiagRec((SQLSMALLINT)handle_type, (SQLHANDLE)handle,
                                    rec, state, &native, msg,
                                    (SQLSMALLINT)sizeof(msg), &len);
        if (r != SQL_SUCCESS && r != SQL_SUCCESS_WITH_INFO)
            break;

        /* Drop the "[Microsoft][ODBC Driver 18 for SQL Server][SQL Server]"
         * prefix: every leading [...] group. */
        const char *text = (const char *)msg;
        while (*text == '[')
        {
            const char *close = strchr(text, ']');
            if (!close) break;
            text = close + 1;
        }
        while (*text == ' ') text++;

        int n = snprintf(out + used, out_size - used, "%sSQLSTATE %s [native %d]: %s",
                         used ? " | " : "", (const char *)state, (int)native, text);
        if (n < 0 || (size_t)n >= out_size - used)
        {
            out[out_size - 1] = '\0';
            return;
        }
        used += (size_t)n;
    }
}

static void log_diag(logger_t *logger, int level, const char *func,
                     int handle_type, void *handle, const char *what)
{
    char diag[2048];
    mssql_diag(handle_type, handle, diag, sizeof(diag));
    logger_write(logger, level, func, 0, "%s: %s", what,
                 diag[0] ? diag : "(no diagnostic record)");
}

/* ================================================================== */
/*  Connection string                                                   */
/* ================================================================== */

/* Appends value in ODBC braces, doubling any '}' inside it. */
static int put_braced(char *out, size_t out_size, size_t *used, const char *value)
{
    if (*used + 1 >= out_size) return -1;
    out[(*used)++] = '{';
    for (const char *p = value ? value : ""; *p; p++)
    {
        size_t need = (*p == '}') ? 2 : 1;
        if (*used + need + 1 >= out_size) return -1;
        out[(*used)++] = *p;
        if (*p == '}') out[(*used)++] = '}';
    }
    if (*used + 1 >= out_size) return -1;
    out[(*used)++] = '}';
    out[*used] = '\0';
    return 0;
}

static int put_text(char *out, size_t out_size, size_t *used, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *used, out_size - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= out_size - *used) return -1;
    *used += (size_t)n;
    return 0;
}

int mssql_connection_string(const app_config_t *ini, int mask_password,
                            char *out, size_t out_size)
{
    if (!ini || !out || out_size == 0) return -1;
    size_t used = 0;
    out[0] = '\0';

    if (put_text(out, out_size, &used, "Driver={%s};Server=tcp:%s,%d;Database=",
                 MSSQL_ODBC_DRIVER, ini->mssql_server,
                 ini->mssql_port > 0 ? ini->mssql_port : 1433) != 0)       return -1;
    if (put_braced(out, out_size, &used, ini->dbname) != 0)                 return -1;
    if (put_text(out, out_size, &used, ";UID=") != 0)                       return -1;
    if (put_braced(out, out_size, &used, ini->username) != 0)               return -1;
    if (put_text(out, out_size, &used, ";PWD=") != 0)                       return -1;
    if (mask_password)
    {
        if (put_text(out, out_size, &used, "***") != 0)                     return -1;
    }
    else if (put_braced(out, out_size, &used, ini->password) != 0)          return -1;
    if (put_text(out, out_size, &used,
                 ";Encrypt=yes;TrustServerCertificate=%s;APP=DataSol_Gateway;",
                 ini->mssql_trust_server_certificate ? "yes" : "no") != 0)  return -1;
    return 0;
}

/* ================================================================== */
/*  P4 - one ODBC environment per process                               */
/* ================================================================== */
static pthread_mutex_t g_env_mutex = PTHREAD_MUTEX_INITIALIZER;
static SQLHENV         g_env       = SQL_NULL_HENV;
static int             g_env_refs  = 0;

void *mssql_env_acquire(logger_t *logger)
{
    pthread_mutex_lock(&g_env_mutex);

    if (g_env == SQL_NULL_HENV)
    {
        SQLHENV h = SQL_NULL_HENV;
        if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &h)))
        {
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "SQLAllocHandle(SQL_HANDLE_ENV) failed - is unixODBC installed?");
            pthread_mutex_unlock(&g_env_mutex);
            return NULL;
        }
        if (!SQL_SUCCEEDED(SQLSetEnvAttr(h, SQL_ATTR_ODBC_VERSION,
                                         (SQLPOINTER)(intptr_t)SQL_OV_ODBC3_80, 0)))
        {
            log_diag(logger, LOG_ERROR, __func__, SQL_HANDLE_ENV, h,
                     "SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION = 3.80) failed");
            SQLFreeHandle(SQL_HANDLE_ENV, h);
            pthread_mutex_unlock(&g_env_mutex);
            return NULL;
        }
        /* ODBC's own connection pooling stays off (the default): the
         * gateway pools connections itself, and a second pool underneath
         * would hide a lost connection from session_is_alive().        */
        g_env = h;
        logger_write(logger, LOG_INFO, __func__, 0,
                     "ODBC environment created (ODBC 3.80, ODBC pooling off)");
    }

    g_env_refs++;
    SQLHENV env = g_env;
    pthread_mutex_unlock(&g_env_mutex);
    return (void *)env;
}

void mssql_env_release(void)
{
    pthread_mutex_lock(&g_env_mutex);
    if (g_env_refs > 0 && --g_env_refs == 0 && g_env != SQL_NULL_HENV)
    {
        SQLFreeHandle(SQL_HANDLE_ENV, g_env);
        g_env = SQL_NULL_HENV;
    }
    pthread_mutex_unlock(&g_env_mutex);
}

/* ================================================================== */
/*  One connection                                                      */
/* ================================================================== */
void *mssql_open_connection(void *henv, const app_config_t *ini, logger_t *logger)
{
    if (!henv || !ini) return NULL;

    char masked[1024];
    char cs[1024];
    if (mssql_connection_string(ini, 1, masked, sizeof(masked)) != 0 ||
        mssql_connection_string(ini, 0, cs, sizeof(cs)) != 0)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "connection string does not fit in %zu bytes", sizeof(cs));
        return NULL;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Connecting to SQL Server: %s", masked);

    SQLHDBC h = SQL_NULL_HDBC;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, (SQLHENV)henv, &h)))
    {
        log_diag(logger, LOG_ERROR, __func__, SQL_HANDLE_ENV, henv,
                 "SQLAllocHandle(SQL_HANDLE_DBC) failed");
        memset(cs, 0, sizeof(cs));
        return NULL;
    }

    int login_timeout = ini->mssql_login_timeout > 0 ? ini->mssql_login_timeout : 15;
    if (!SQL_SUCCEEDED(SQLSetConnectAttr(h, SQL_ATTR_LOGIN_TIMEOUT,
                                         (SQLPOINTER)(intptr_t)login_timeout, 0)))
        log_diag(logger, LOG_WARN, __func__, SQL_HANDLE_DBC, h,
                 "SQL_ATTR_LOGIN_TIMEOUT not set (continuing)");

    if (ini->network_read_write_timeout > 0 &&
        !SQL_SUCCEEDED(SQLSetConnectAttr(h, SQL_ATTR_CONNECTION_TIMEOUT,
                                         (SQLPOINTER)(intptr_t)ini->network_read_write_timeout, 0)))
        log_diag(logger, LOG_WARN, __func__, SQL_HANDLE_DBC, h,
                 "SQL_ATTR_CONNECTION_TIMEOUT not set (continuing)");

    /* M4: autocommit off unless autocommit_mode = 1, as on Oracle. */
    SQLULEN ac = ini->autocommit_mode ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
    if (!SQL_SUCCEEDED(SQLSetConnectAttr(h, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)ac, 0)))
    {
        log_diag(logger, LOG_ERROR, __func__, SQL_HANDLE_DBC, h,
                 "SQL_ATTR_AUTOCOMMIT could not be set");
        SQLFreeHandle(SQL_HANDLE_DBC, h);
        memset(cs, 0, sizeof(cs));
        return NULL;
    }

    SQLRETURN r = SQLDriverConnect(h, NULL, (SQLCHAR *)cs, SQL_NTS,
                                   NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    memset(cs, 0, sizeof(cs));          /* the password was in there */

    if (!SQL_SUCCEEDED(r))
    {
        log_diag(logger, LOG_ERROR, __func__, SQL_HANDLE_DBC, h,
                 "SQLDriverConnect failed");
        SQLFreeHandle(SQL_HANDLE_DBC, h);
        return NULL;
    }
    if (r == SQL_SUCCESS_WITH_INFO)
        log_diag(logger, LOG_DEBUG, __func__, SQL_HANDLE_DBC, h,
                 "SQLDriverConnect info");

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Connected (autocommit %s)", ini->autocommit_mode ? "on" : "off");
    return (void *)h;
}

void mssql_close_connection(void *hdbc, logger_t *logger)
{
    if (!hdbc) return;
    SQLRETURN r = SQLDisconnect((SQLHDBC)hdbc);
    if (!SQL_SUCCEEDED(r))
        log_diag(logger, LOG_WARN, __func__, SQL_HANDLE_DBC, hdbc,
                 "SQLDisconnect reported an error (continuing cleanup)");
    SQLFreeHandle(SQL_HANDLE_DBC, (SQLHDBC)hdbc);
}

int mssql_ping(void *hdbc, logger_t *logger)
{
    if (!hdbc) return 0;

    SQLHSTMT st = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)hdbc, &st)))
    {
        log_diag(logger, LOG_WARN, __func__, SQL_HANDLE_DBC, hdbc,
                 "ping: SQLAllocHandle(SQL_HANDLE_STMT) failed");
        return 0;
    }

    int ok = 0;
    if (SQL_SUCCEEDED(SQLExecDirect(st, (SQLCHAR *)"SELECT 1", SQL_NTS)) &&
        SQL_SUCCEEDED(SQLFetch(st)))
        ok = 1;
    else
        log_diag(logger, LOG_WARN, __func__, SQL_HANDLE_STMT, st, "ping failed");

    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return ok;
}

int mssql_end_tran(void *hdbc, int commit, logger_t *logger, const char *caller)
{
    if (!hdbc)
    {
        logger_write(logger, LOG_ERROR, caller, 0,
                     "%s: no connection on this context", commit ? "commit" : "rollback");
        return -1;
    }
    SQLRETURN r = SQLEndTran(SQL_HANDLE_DBC, (SQLHDBC)hdbc,
                             commit ? SQL_COMMIT : SQL_ROLLBACK);
    if (!SQL_SUCCEEDED(r))
    {
        char diag[2048];
        mssql_diag(SQL_HANDLE_DBC, hdbc, diag, sizeof(diag));
        logger_write(logger, LOG_ERROR, caller, 0, "%s failed: %s",
                     commit ? "COMMIT" : "ROLLBACK",
                     diag[0] ? diag : "(no diagnostic record)");
        return -1;
    }
    return 0;
}

static void warn_ignored_settings(const app_config_t *ini, logger_t *logger)
{
    if (ini->use_wallet)
        logger_write(logger, LOG_WARN, __func__, 0,
                     "use_wallet=1 is an Oracle setting - ignored on SQL Server "
                     "(username/password are used)");
    logger_write(logger, LOG_INFO, __func__, 0,
                 "nls_* settings are Oracle settings - not used on SQL Server");
}

/* ================================================================== */
/*  Standalone (use_connection_pool = 0)                                */
/* ================================================================== */
int MSSQL_Connect_standalone(oci_context_t *ctx)
{
    if (!ctx || !ctx->ini) return -1;
    logger_t *log = ctx->connection_logger;

    logger_write(log, LOG_INFO, __func__, 0,
                 "Entering MSSQL_Connect_standalone server=%s port=%d db=%s user=%s",
                 ctx->ini->mssql_server, ctx->ini->mssql_port,
                 ctx->ini->dbname, ctx->ini->username);
    warn_ignored_settings(ctx->ini, log);

    void *env = mssql_env_acquire(log);
    if (!env) return -1;

    void *dbc = mssql_open_connection(env, ctx->ini, log);
    if (!dbc)
    {
        mssql_env_release();
        return -1;
    }

    ctx->envhp           = env;
    ctx->svchp           = dbc;
    ctx->errhp           = NULL;
    ctx->srvhp           = NULL;
    ctx->authp           = NULL;
    ctx->pool_handle     = NULL;
    ctx->pool_slot_index = -1;

    logger_write(log, LOG_INFO, __func__, 0, "MSSQL_Connect_standalone OK");
    return 0;
}

void MSSQL_Disconnect_standalone(oci_context_t *ctx)
{
    if (!ctx) return;
    logger_t *log = ctx->connection_logger;

    if (ctx->svchp)
    {
        /* Nothing uncommitted survives a disconnect on Oracle either. */
        SQLEndTran(SQL_HANDLE_DBC, (SQLHDBC)ctx->svchp, SQL_ROLLBACK);
        mssql_close_connection(ctx->svchp, log);
        ctx->svchp = NULL;
    }
    if (ctx->envhp)
    {
        mssql_env_release();
        ctx->envhp = NULL;
    }
    logger_write(log, LOG_INFO, __func__, 0, "MSSQL_Disconnect_standalone complete");
}

/* ================================================================== */
/*  Pool                                                                */
/* ================================================================== */
static int open_slot(mssql_pool_t *pool, mssql_slot_t *slot, logger_t *log)
{
    void *dbc = mssql_open_connection(pool->henv, &pool->cfg, log);
    if (!dbc) return -1;
    slot->hdbc      = (SQLHDBC)dbc;
    slot->in_use    = 0;
    slot->created   = time(NULL);
    slot->last_used = slot->created;
    return 0;
}

static void close_slot(mssql_slot_t *slot, logger_t *log)
{
    if (slot->hdbc)
    {
        logger_write(log, LOG_INFO, __func__, 0, "Closing pool slot");
        mssql_close_connection(slot->hdbc, log);
    }
    slot->hdbc   = SQL_NULL_HDBC;
    slot->in_use = 0;
}

/* Reopens a closed slot, up to retries_on_connection_failure attempts
 * (at least one). 0 = open. Caller holds the mutex. */
static int reopen_slot(mssql_pool_t *pool, mssql_slot_t *slot, int idx, logger_t *log)
{
    int attempts = pool->retries_on_connection_failure > 0
                 ? pool->retries_on_connection_failure : 1;
    for (int a = 0; a < attempts; a++)
    {
        if (open_slot(pool, slot, log) == 0)
        {
            pool->slots_open++;
            return 0;
        }
        logger_write(log, LOG_WARN, __func__, 0,
                     "Slot %d open attempt %d/%d failed", idx, a + 1, attempts);
    }
    return -1;
}

static void log_pool_config(const mssql_pool_t *p, logger_t *log)
{
    logger_write(log, LOG_INFO, __func__, 0, "=== SQL Server Connection Pool Configuration ===");
    logger_write(log, LOG_INFO, __func__, 0, "  mssql_server                     = %s", p->cfg.mssql_server);
    logger_write(log, LOG_INFO, __func__, 0, "  mssql_port                       = %d", p->cfg.mssql_port);
    logger_write(log, LOG_INFO, __func__, 0, "  dbname (database)                = %s", p->cfg.dbname);
    logger_write(log, LOG_INFO, __func__, 0, "  username                         = %s", p->cfg.username);
    logger_write(log, LOG_INFO, __func__, 0, "  mssql_trust_server_certificate   = %d", p->cfg.mssql_trust_server_certificate);
    logger_write(log, LOG_INFO, __func__, 0, "  mssql_login_timeout              = %d s", p->cfg.mssql_login_timeout);
    logger_write(log, LOG_INFO, __func__, 0, "  network_read_write_timeout       = %d s", p->cfg.network_read_write_timeout);
    logger_write(log, LOG_INFO, __func__, 0, "  pool_min_size                    = %d", p->pool_min_size);
    logger_write(log, LOG_INFO, __func__, 0, "  pool_max_size                    = %d", p->pool_max_size);
    logger_write(log, LOG_INFO, __func__, 0, "  pool_connection_timeout          = %d s", p->connection_timeout);
    logger_write(log, LOG_INFO, __func__, 0, "  session_idle_timeout             = %d s", p->session_idle_timeout);
    logger_write(log, LOG_INFO, __func__, 0, "  session_max_lifetime             = %d s", p->session_max_lifetime);
    logger_write(log, LOG_INFO, __func__, 0, "  retries_on_connection_failure    = %d", p->retries_on_connection_failure);
    logger_write(log, LOG_INFO, __func__, 0, "  connection_validation_on_borrow  = %d", p->connection_validation_on_borrow);
    logger_write(log, LOG_INFO, __func__, 0, "  rollback_on_return_to_pool       = %d", p->rollback_on_return_to_pool);
    logger_write(log, LOG_INFO, __func__, 0, "  autocommit_mode                  = %d", p->cfg.autocommit_mode);
    logger_write(log, LOG_INFO, __func__, 0, "================================================");
}

static void free_pool(mssql_pool_t *pool, logger_t *log)
{
    if (!pool) return;
    if (pool->slots)
    {
        for (int i = 0; i < pool->slot_count; i++)
            if (pool->slots[i].hdbc)
                close_slot(&pool->slots[i], log);
        free(pool->slots);
    }
    if (pool->henv) mssql_env_release();
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    memset(&pool->cfg.password, 0, sizeof(pool->cfg.password));
    free(pool);
}

int MSSQL_Connect_pool(oci_context_t *ctx)
{
    if (!ctx || !ctx->ini) return -1;
    logger_t *log = ctx->connectionpool_logger;

    logger_write(log, LOG_INFO, __func__, 0, "Entering MSSQL_Connect_pool");

    mssql_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool)
    {
        logger_write(log, LOG_ERROR, __func__, 0, "calloc failed for the pool handle");
        return -1;
    }

    const app_config_t *ini = ctx->ini;
    pool->cfg = *ini;

    pool->pool_min_size                   = ini->pool_min_size                 > 0  ? ini->pool_min_size                 : 1;
    pool->pool_max_size                   = ini->pool_max_size                 > 0  ? ini->pool_max_size                 : 10;
    pool->connection_timeout              = ini->pool_connection_timeout       > 0  ? ini->pool_connection_timeout       : 30;
    pool->session_idle_timeout            = ini->session_idle_timeout          > 0  ? ini->session_idle_timeout          : 300;
    pool->session_max_lifetime            = ini->session_max_lifetime          > 0  ? ini->session_max_lifetime          : 3600;
    pool->retries_on_connection_failure   = ini->retries_on_connection_failure >= 0 ? ini->retries_on_connection_failure : 3;
    pool->connection_validation_on_borrow = ini->connection_validation_on_borrow;
    pool->rollback_on_return_to_pool      = ini->rollback_on_return_to_pool;
    if (pool->pool_min_size > pool->pool_max_size)
        pool->pool_min_size = pool->pool_max_size;
    snprintf(pool->nls_date_format, sizeof(pool->nls_date_format), "%s",
             ini->nls_date_format[0] ? ini->nls_date_format : NLS_DATE_FORMAT_DEFAULT);

    log_pool_config(pool, log);
    warn_ignored_settings(ini, log);

    if (pthread_mutex_init(&pool->mutex, NULL) != 0 ||
        pthread_cond_init(&pool->cond, NULL) != 0)
    {
        logger_write(log, LOG_ERROR, __func__, 0, "pthread_mutex_init / pthread_cond_init failed");
        free(pool);
        return -1;
    }

    pool->slots = calloc((size_t)pool->pool_max_size, sizeof(mssql_slot_t));
    if (!pool->slots)
    {
        logger_write(log, LOG_ERROR, __func__, 0,
                     "calloc failed for pool slots (max_size=%d)", pool->pool_max_size);
        free_pool(pool, log);
        return -1;
    }
    pool->slot_count = pool->pool_max_size;

    pool->henv = (SQLHENV)mssql_env_acquire(log);
    if (!pool->henv)
    {
        free_pool(pool, log);
        return -1;
    }

    uint64_t t0 = now_us();
    logger_write(log, LOG_INFO, __func__, 0,
                 "Opening %d minimum pool slots", pool->pool_min_size);

    for (int i = 0; i < pool->pool_min_size; i++)
    {
        if (reopen_slot(pool, &pool->slots[i], i, log) != 0)
        {
            logger_write(log, LOG_ERROR, __func__, 0,
                         "Failed to open mandatory slot %d", i);
            free_pool(pool, log);
            logger_write(log, LOG_ERROR, __func__, 0,
                         "MSSQL_Connect_pool failed - resources released");
            return -1;
        }
        logger_write(log, LOG_INFO, __func__, 0, "Slot %d opened (%d/%d)",
                     i, pool->slots_open, pool->pool_min_size);
    }

    ctx->pool_handle     = pool;
    ctx->pool_slot_index = -1;

    logger_write(log, LOG_INFO, __func__, 0,
                 "MSSQL_Connect_pool complete: slots_open=%d pool_max=%d (%.1f ms)",
                 pool->slots_open, pool->pool_max_size, (now_us() - t0) / 1000.0);
    return 0;
}

void MSSQL_Disconnect_pool(oci_context_t *ctx)
{
    if (!ctx) return;
    logger_t *log = ctx->connectionpool_logger;
    logger_write(log, LOG_INFO, __func__, 0, "Entering MSSQL_Disconnect_pool");

    mssql_pool_t *pool = (mssql_pool_t *)ctx->pool_handle;
    if (!pool)
    {
        logger_write(log, LOG_WARN, __func__, 0, "No pool handle present - nothing to disconnect");
        return;
    }

    pthread_mutex_lock(&pool->mutex);
    logger_write(log, LOG_INFO, __func__, 0, "Closing %d open pool slots", pool->slots_open);
    for (int i = 0; i < pool->slot_count; i++)
        if (pool->slots[i].hdbc)
        {
            SQLEndTran(SQL_HANDLE_DBC, pool->slots[i].hdbc, SQL_ROLLBACK);
            close_slot(&pool->slots[i], log);
        }
    pool->slots_open = 0;
    pthread_mutex_unlock(&pool->mutex);

    free_pool(pool, log);
    ctx->pool_handle = NULL;
    logger_write(log, LOG_INFO, __func__, 0, "MSSQL_Disconnect_pool complete");
}

int MSSQL_Pool_get_session(oci_context_t *ctx, oci_context_t *worker_ctx)
{
    if (!ctx || !ctx->pool_handle || !worker_ctx)
    {
        if (ctx)
            logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments (no pool - is use_connection_pool=0?)");
        return -1;
    }

    logger_t     *log  = ctx->connectionpool_logger;
    mssql_pool_t *pool = (mssql_pool_t *)ctx->pool_handle;
    uint64_t      t0   = now_us();

    logger_write(log, LOG_INFO, __func__, 0,
                 "Requesting pool session (timeout=%ds)", pool->connection_timeout);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += pool->connection_timeout;

    pthread_mutex_lock(&pool->mutex);

    int idx = -1;
    for (;;)
    {
        for (int i = 0; i < pool->slot_count; i++)
            if (pool->slots[i].hdbc && !pool->slots[i].in_use) { idx = i; break; }
        if (idx >= 0) break;

        if (pool->slots_open < pool->pool_max_size)
        {
            for (int i = 0; i < pool->slot_count; i++)
            {
                if (!pool->slots[i].hdbc)
                {
                    logger_write(log, LOG_INFO, __func__, 0,
                                 "Pool growing: opening slot %d (%d/%d)",
                                 i, pool->slots_open + 1, pool->pool_max_size);
                    if (open_slot(pool, &pool->slots[i], log) == 0)
                    {
                        pool->slots_open++;
                        idx = i;
                    }
                    break;
                }
            }
            if (idx >= 0) break;
        }

        logger_write(log, LOG_INFO, __func__, 0,
                     "All %d pool slots in use - waiting for release", pool->slots_open);

        int w = pthread_cond_timedwait(&pool->cond, &pool->mutex, &deadline);
        if (w == ETIMEDOUT)
        {
            logger_write(log, LOG_ERROR, __func__, 0,
                         "Timed out waiting for pool slot after %ds", pool->connection_timeout);
            pthread_mutex_unlock(&pool->mutex);
            return -1;
        }
    }

    mssql_slot_t *slot = &pool->slots[idx];

    if (pool->connection_validation_on_borrow && !mssql_ping(slot->hdbc, log))
    {
        logger_write(log, LOG_WARN, __func__, 0,
                     "Slot %d failed validation - reopening", idx);
        close_slot(slot, log);
        pool->slots_open--;
        if (reopen_slot(pool, slot, idx, log) != 0)
        {
            logger_write(log, LOG_ERROR, __func__, 0, "Could not recover slot %d", idx);
            pthread_cond_signal(&pool->cond);
            pthread_mutex_unlock(&pool->mutex);
            return -1;
        }
    }

    uint64_t t_slot = now_us();
    slot->in_use = 1;
    pthread_mutex_unlock(&pool->mutex);
    uint64_t t_end = now_us();

    memset(worker_ctx, 0, sizeof(*worker_ctx));
    worker_ctx->envhp           = pool->henv;
    worker_ctx->svchp           = slot->hdbc;
    worker_ctx->ini             = ctx->ini;
    worker_ctx->logger          = ctx->logger;
    worker_ctx->NLS_DATE_FORMAT = pool->nls_date_format;
    worker_ctx->pool_handle     = ctx->pool_handle;
    worker_ctx->pool_slot_index = idx;
    worker_ctx->connection_wait_us    = t_slot - t0;
    worker_ctx->connection_create_us  = t_end  - t_slot;
    worker_ctx->connection_acquire_us = t_end  - t0;

    logger_write(log, LOG_INFO, __func__, 0, "Pool session granted: slot=%d", idx);
    return 0;
}

void MSSQL_Pool_release_session(oci_context_t *ctx, oci_context_t *worker_ctx)
{
    if (!ctx || !ctx->pool_handle || !worker_ctx)
    {
        if (ctx)
            logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                         "Invalid arguments - nothing released");
        return;
    }

    logger_t     *log  = ctx->connectionpool_logger;
    mssql_pool_t *pool = (mssql_pool_t *)ctx->pool_handle;
    int           idx  = worker_ctx->pool_slot_index;

    logger_write(log, LOG_INFO, __func__, 0, "Releasing pool session slot=%d", idx);

    if (idx < 0 || idx >= pool->slot_count)
    {
        logger_write(log, LOG_ERROR, __func__, 0, "Invalid pool_slot_index=%d", idx);
        return;
    }

    mssql_slot_t *slot = &pool->slots[idx];

    if (pool->rollback_on_return_to_pool && slot->hdbc)
    {
        logger_write(log, LOG_DEBUG, __func__, 0, "ROLLBACK on return slot=%d", idx);
        if (mssql_end_tran(slot->hdbc, 0, log, __func__) != 0)
            logger_write(log, LOG_WARN, __func__, 0,
                         "Rollback on return failed for slot %d (continuing)", idx);
    }

    pthread_mutex_lock(&pool->mutex);
    slot->last_used = time(NULL);
    slot->in_use    = 0;
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    memset(worker_ctx, 0, sizeof(*worker_ctx));
    logger_write(log, LOG_INFO, __func__, 0, "Pool session released: slot=%d", idx);
}

int MSSQL_Pool_session_is_alive(oci_context_t *ctx)
{
    if (!ctx || !ctx->svchp) return 0;

    SQLUINTEGER dead = SQL_CD_TRUE;
    SQLRETURN r = SQLGetConnectAttr((SQLHDBC)ctx->svchp, SQL_ATTR_CONNECTION_DEAD,
                                    &dead, 0, NULL);
    if (!SQL_SUCCEEDED(r))
        return 0;           /* cannot confirm alive - treat as not alive,
                               as OCI_Pool_session_is_alive() does       */
    return dead == SQL_CD_FALSE ? 1 : 0;
}

int MSSQL_Pool_reconnect_session(oci_context_t *base_ctx, oci_context_t *ctx)
{
    if (!base_ctx || !ctx || !base_ctx->pool_handle) return -1;

    logger_t     *log  = base_ctx->connectionpool_logger;
    mssql_pool_t *pool = (mssql_pool_t *)base_ctx->pool_handle;
    int           idx  = ctx->pool_slot_index;

    if (idx >= 0 && idx < pool->slot_count)
    {
        pthread_mutex_lock(&pool->mutex);
        mssql_slot_t *slot = &pool->slots[idx];
        if (slot->hdbc && slot->hdbc == (SQLHDBC)ctx->svchp)
        {
            logger_write(log, LOG_WARN, __func__, 0,
                         "Reconnect requested for slot %d - closing its connection", idx);
            close_slot(slot, log);
            pool->slots_open--;
        }
        pthread_cond_signal(&pool->cond);
        pthread_mutex_unlock(&pool->mutex);
    }
    memset(ctx, 0, sizeof(*ctx));

    if (MSSQL_Pool_get_session(base_ctx, ctx) != 0)
        return -1;

    copy_shared_ctx_fields(ctx, base_ctx);
    return 0;
}

int MSSQL_Pool_health_check(oci_context_t *ctx)
{
    if (!ctx || !ctx->pool_handle) return -1;

    logger_t     *log  = ctx->connectionpool_logger;
    mssql_pool_t *pool = (mssql_pool_t *)ctx->pool_handle;
    int           failures = 0;

    logger_write(log, LOG_INFO, __func__, 0, "Starting pool health check");

    pthread_mutex_lock(&pool->mutex);
    time_t now = time(NULL);

    for (int i = 0; i < pool->slot_count; i++)
    {
        mssql_slot_t *slot = &pool->slots[i];
        if (!slot->hdbc || slot->in_use)
            continue;

        if (pool->session_max_lifetime > 0 &&
            now - slot->created > (time_t)pool->session_max_lifetime)
        {
            logger_write(log, LOG_INFO, __func__, 0,
                         "Slot %d age=%lds exceeds session_max_lifetime=%ds - recycling",
                         i, (long)(now - slot->created), pool->session_max_lifetime);
            close_slot(slot, log);
            pool->slots_open--;
            if (reopen_slot(pool, slot, i, log) != 0)
            {
                logger_write(log, LOG_ERROR, __func__, 0,
                             "Slot %d reopen after max_lifetime failed", i);
                failures++;
            }
            continue;
        }

        if (pool->session_idle_timeout > 0 &&
            now - slot->last_used > (time_t)pool->session_idle_timeout)
            logger_write(log, LOG_DEBUG, __func__, 0,
                         "Slot %d idle %lds (limit %ds) - will ping",
                         i, (long)(now - slot->last_used), pool->session_idle_timeout);

        if (!mssql_ping(slot->hdbc, log))
        {
            logger_write(log, LOG_WARN, __func__, 0, "Slot %d ping failed - reopening", i);
            close_slot(slot, log);
            pool->slots_open--;
            if (reopen_slot(pool, slot, i, log) != 0)
            {
                logger_write(log, LOG_ERROR, __func__, 0, "Slot %d could not be recovered", i);
                failures++;
            }
            else
                logger_write(log, LOG_INFO, __func__, 0, "Slot %d recovered OK", i);
        }
        else
            logger_write(log, LOG_DEBUG, __func__, 0, "Slot %d healthy", i);
    }

    pthread_mutex_unlock(&pool->mutex);
    logger_write(log, LOG_INFO, __func__, 0,
                 "Pool health check complete failures=%d", failures);
    return failures;
}
