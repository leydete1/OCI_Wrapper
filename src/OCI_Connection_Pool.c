/*
 * OCI_Connection_Pool.c
 *
 * OCI Connection Pool Module
 * ---------------------------
 * Implements a thread-safe connection pool using a hand-managed array
 * of OCISvcCtx / OCISession handle pairs.  OCIEnv and OCIServer are
 * shared across all slots (safe per OCI Programmer's Guide for
 * OCI_THREADED environments).
 *
 * Slot lifecycle
 * --------------
 *   open_slot()    - allocates handles, OCISessionBegin, NLS ALTER SESSION
 *   close_slot()   - OCISessionEnd, frees all per-slot handles
 *   validate_slot()- OCIPing; returns 1=alive, 0=dead
 *
 * Pool growth
 * -----------
 * Starts at pool_min_size.  When get_session finds all slots in use
 * and slots_open < pool_max_size, it opens a new slot on the spot
 * (up to pool_increment at a time is not enforced per-call here;
 * one slot is opened per get_session call that hits the growth path,
 * which naturally respects pool_increment semantics under concurrent
 * load).  Shrink-back is handled by close_slot on health-check failure
 * or session_max_lifetime expiry.
 *
 * NLS session initialisation
 * --------------------------
 * After every OCISessionBegin, ALTER SESSION sets:
 *   NLS_DATE_FORMAT, NLS_LANGUAGE + NLS_TERRITORY, TIME_ZONE
 * and optionally AUTOCOMMIT.  This mirrors what OCI_Connect does and
 * ensures consistent behaviour whether or not the pool is in use.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include "OCI_Connection_Pool.h"
#include "OCI_Connection.h"
#include "ini_reader.h"
#include "logger.h"
#include "metrics.h"

/* ------------------------------------------------------------------ */
/*  OCI error macro - same style as the rest of the project            */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_POOL(errhp, status, logger, label)                    \
    do {                                                                 \
        if ((status) != OCI_SUCCESS &&                                  \
            (status) != OCI_SUCCESS_WITH_INFO)                          \
        {                                                                \
            text _errbuf[512];                                           \
            sb4  _errcode = 0;                                           \
            OCIErrorGet((errhp), 1, NULL, &_errcode,                    \
                        _errbuf, sizeof(_errbuf), OCI_HTYPE_ERROR);     \
            logger_write((logger), LOG_ERROR, __func__, 0,              \
                         "OCI Error %d: %s", _errcode,                  \
                         (char *)_errbuf);                               \
            rc = -1;                                                     \
            goto label;                                                  \
        }                                                                \
    } while (0)

/* ================================================================== */
/*  Static helper: run one ALTER SESSION DDL, log it, ignore errors    */
/*  (non-fatal - a bad NLS string should not abort the connection)     */
/* ================================================================== */
static void run_nls_sql(pool_slot_t *slot, logger_t *logger,
                         const char *sql)
{
    if (!sql || sql[0] == '\0') return;

    logger_write(logger, LOG_DEBUG, __func__, 0, "NLS: %s", sql);

    OCIStmt *stmt = NULL;
    sword s = OCIStmtPrepare2(slot->svchp, &stmt, slot->errhp,
                               (text *)sql, (ub4)strlen(sql),
                               NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (s != OCI_SUCCESS && s != OCI_SUCCESS_WITH_INFO)
    {
        logger_write(logger, LOG_WARN, __func__, 0,
                     "NLS prepare failed (status=%d) sql='%s'", s, sql);
        return;
    }

    s = OCIStmtExecute(slot->svchp, stmt, slot->errhp,
                       1, 0, NULL, NULL, OCI_DEFAULT);
    if (s != OCI_SUCCESS && s != OCI_SUCCESS_WITH_INFO)
    {
        text buf[512]; sb4 code = 0;
        OCIErrorGet(slot->errhp, 1, NULL, &code,
                    buf, sizeof(buf), OCI_HTYPE_ERROR);
        logger_write(logger, LOG_WARN, __func__, 0,
                     "NLS execute error %d: %s sql='%s'", code, buf, sql);
    }

    OCIStmtRelease(stmt, slot->errhp, NULL, 0, OCI_DEFAULT);
}

/* ================================================================== */
/*  Static helper: open one pool slot                                   */
/*  Authenticates a new OCI session and sets NLS session parameters.   */
/* ================================================================== */
static int open_slot(oci_pool_handle_t *pool,
                     pool_slot_t       *slot,
                     logger_t          *logger)
{
    int rc = 0;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Opening pool slot - user='%s' db='%s'",
                 pool->username, pool->dbname);

    /* ---- Per-slot error handle ---- */
    CHECK_OCI_POOL(pool->errhp,
        OCIHandleAlloc(pool->envhp, (void **)&slot->errhp,
                       OCI_HTYPE_ERROR, 0, NULL),
        logger, Cleanup);

    /* ---- Service context ---- */
    CHECK_OCI_POOL(slot->errhp,
        OCIHandleAlloc(pool->envhp, (void **)&slot->svchp,
                       OCI_HTYPE_SVCCTX, 0, NULL),
        logger, Cleanup);

    /* ---- Session handle ---- */
    CHECK_OCI_POOL(slot->errhp,
        OCIHandleAlloc(pool->envhp, (void **)&slot->authp,
                       OCI_HTYPE_SESSION, 0, NULL),
        logger, Cleanup);

    /* ---- Attach shared server handle to service context ---- */
    CHECK_OCI_POOL(slot->errhp,
        OCIAttrSet(slot->svchp, OCI_HTYPE_SVCCTX,
                   pool->srvhp, 0,
                   OCI_ATTR_SERVER, slot->errhp),
        logger, Cleanup);

    /* ---- Set username (legacy mode only) ---- */
    if (!pool->use_wallet)
    {
        logger_write(logger, LOG_DEBUG, __func__, 0,
                     "OCIAttrSet OCI_ATTR_USERNAME (legacy mode)");
        CHECK_OCI_POOL(slot->errhp,
            OCIAttrSet(slot->authp, OCI_HTYPE_SESSION,
                       (dvoid *)pool->username, (ub4)strlen(pool->username),
                       OCI_ATTR_USERNAME, slot->errhp),
            logger, Cleanup);

        /* ---- Set password ---- */
        logger_write(logger, LOG_DEBUG, __func__, 0,
                     "OCIAttrSet OCI_ATTR_PASSWORD (legacy mode)");
        CHECK_OCI_POOL(slot->errhp,
            OCIAttrSet(slot->authp, OCI_HTYPE_SESSION,
                       (dvoid *)pool->password, (ub4)strlen(pool->password),
                       OCI_ATTR_PASSWORD, slot->errhp),
            logger, Cleanup);
    }

    /* ---- Begin session ---- */
    logger_write(logger, LOG_INFO, __func__, 0,
                 "Calling OCISessionBegin (%s)",
                 pool->use_wallet ? "OCI_CRED_EXT - wallet"
                                  : "OCI_CRED_RDBMS - legacy");
    CHECK_OCI_POOL(slot->errhp,
        OCISessionBegin(slot->svchp, slot->errhp,
                        slot->authp,
                        pool->use_wallet ? OCI_CRED_EXT : OCI_CRED_RDBMS,
                        OCI_DEFAULT),
        logger, Cleanup);

    /* ---- Attach session to service context ---- */
    CHECK_OCI_POOL(slot->errhp,
        OCIAttrSet(slot->svchp, OCI_HTYPE_SVCCTX,
                   slot->authp, 0,
                   OCI_ATTR_SESSION, slot->errhp),
        logger, Cleanup);

    logger_write(logger, LOG_INFO, __func__, 0,
                 "OCISessionBegin OK - applying NLS session settings");

    /* ---- NLS: NLS_DATE_FORMAT ---- */
    if (pool->nls_date_format[0])
    {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "ALTER SESSION SET NLS_DATE_FORMAT = '%s'",
                 pool->nls_date_format);
        run_nls_sql(slot, logger, sql);
    }

    /* ---- NLS: NLS_LANGUAGE + NLS_TERRITORY ---- */
    if (pool->nls_language[0] && pool->nls_territory[0])
    {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "ALTER SESSION SET NLS_LANGUAGE = '%s' "
                 "NLS_TERRITORY = '%s'",
                 pool->nls_language, pool->nls_territory);
        run_nls_sql(slot, logger, sql);
    }

    /* ---- NLS: TIME_ZONE ---- */
    if (pool->nls_session_timezone[0])
    {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "ALTER SESSION SET TIME_ZONE = '%s'",
                 pool->nls_session_timezone);
        run_nls_sql(slot, logger, sql);
    }

    /* ---- AUTOCOMMIT ---- */
    if (pool->autocommit_mode)
        run_nls_sql(slot, logger,
                    "ALTER SESSION SET AUTOCOMMIT = TRUE");

    /* ---- Record timestamps ---- */
    slot->created   = time(NULL);
    slot->last_used = slot->created;
    slot->state     = POOL_SLOT_FREE;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Pool slot opened OK created=%ld",
                 (long)slot->created);
    return 0;

Cleanup:
    /* Partial cleanup on failure */
    if (slot->authp) { OCIHandleFree(slot->authp, OCI_HTYPE_SESSION); slot->authp = NULL; }
    if (slot->svchp) { OCIHandleFree(slot->svchp, OCI_HTYPE_SVCCTX);  slot->svchp = NULL; }
    if (slot->errhp) { OCIHandleFree(slot->errhp, OCI_HTYPE_ERROR);   slot->errhp = NULL; }
    return rc;
}

/* ================================================================== */
/*  Static helper: close one pool slot                                  */
/* ================================================================== */
static void close_slot(pool_slot_t *slot, logger_t *logger)
{
    logger_write(logger, LOG_INFO, __func__, 0, "Closing pool slot");

    if (slot->svchp && slot->authp && slot->errhp)
    {
        sword s = OCISessionEnd(slot->svchp, slot->errhp,
                                slot->authp, OCI_DEFAULT);
        if (s != OCI_SUCCESS && s != OCI_SUCCESS_WITH_INFO)
        {
            text buf[512]; sb4 code = 0;
            OCIErrorGet(slot->errhp, 1, NULL, &code,
                        buf, sizeof(buf), OCI_HTYPE_ERROR);
            logger_write(logger, LOG_WARN, __func__, 0,
                         "OCISessionEnd error %d: %s (continuing cleanup)",
                         code, buf);
        }
    }

    if (slot->authp) { OCIHandleFree(slot->authp, OCI_HTYPE_SESSION); slot->authp = NULL; }
    if (slot->svchp) { OCIHandleFree(slot->svchp, OCI_HTYPE_SVCCTX);  slot->svchp = NULL; }
    if (slot->errhp) { OCIHandleFree(slot->errhp, OCI_HTYPE_ERROR);   slot->errhp = NULL; }
    slot->state = POOL_SLOT_FREE;

    logger_write(logger, LOG_INFO, __func__, 0, "Pool slot closed");
}

/* ================================================================== */
/*  Static helper: validate slot with OCIPing                          */
/*  Returns 1 if alive, 0 if dead                                      */
/* ================================================================== */
static int validate_slot(pool_slot_t *slot, logger_t *logger)
{
    if (!slot->svchp || !slot->errhp)
    {
        logger_write(logger, LOG_WARN, __func__, 0,
                     "validate_slot: null handles - slot dead");
        return 0;
    }

    logger_write(logger, LOG_DEBUG, __func__, 0, "Calling OCIPing");
    sword s = OCIPing(slot->svchp, slot->errhp, OCI_DEFAULT);
    if (s == OCI_SUCCESS || s == OCI_SUCCESS_WITH_INFO)
    {
        logger_write(logger, LOG_DEBUG, __func__, 0, "OCIPing OK");
        return 1;
    }

    text buf[512]; sb4 code = 0;
    OCIErrorGet(slot->errhp, 1, NULL, &code,
                buf, sizeof(buf), OCI_HTYPE_ERROR);
    logger_write(logger, LOG_WARN, __func__, 0,
                 "OCIPing failed error %d: %s", code, buf);
    return 0;
}

/* ================================================================== */
/*  Static helper: log all pool configuration at startup               */
/* ================================================================== */
static void log_pool_config(const oci_pool_handle_t *pool,
                             logger_t                *logger)
{
    logger_write(logger, LOG_INFO, __func__, 0,
                 "=== Connection Pool Configuration ===");
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  dbname                           = %s", pool->dbname);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  username                         = %s", pool->username);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  pool_min_size                    = %d", pool->pool_min_size);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  pool_max_size                    = %d", pool->pool_max_size);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  pool_increment                   = %d", pool->pool_increment);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  pool_connection_timeout          = %d s", pool->connection_timeout);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  session_idle_timeout             = %d s", pool->session_idle_timeout);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  max_time_to_establish            = %d s", pool->max_time_to_establish);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  network_read_write_timeout       = %d s", pool->network_read_write_timeout);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  query_execution_timeout          = %d s (0=unlimited)", pool->query_execution_timeout);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  authentication_handshake_timeout = %d s", pool->authentication_handshake_timeout);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  retries_on_connection_failure    = %d", pool->retries_on_connection_failure);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  login_auth_timeout               = %d s", pool->login_auth_timeout);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  session_max_lifetime             = %d s", pool->session_max_lifetime);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  heartbeat_keepalive_interval     = %d s", pool->heartbeat_keepalive_interval);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  connection_validation_on_borrow  = %d", pool->connection_validation_on_borrow);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  rollback_on_return_to_pool       = %d", pool->rollback_on_return_to_pool);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  autocommit_mode                  = %d", pool->autocommit_mode);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  nls_date_format                  = %s", pool->nls_date_format);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  nls_language                     = %s", pool->nls_language);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  nls_territory                    = %s", pool->nls_territory);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  nls_characterset                 = %s", pool->nls_characterset);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "  nls_session_timezone             = %s", pool->nls_session_timezone);
    logger_write(logger, LOG_INFO, __func__, 0,
                 "=====================================");
}

/* ================================================================== */
/*  OCI_Connect_pool                                                    */
/* ================================================================== */
int OCI_Connect_pool(oci_context_t *ctx)
{
    int rc = 0;

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Entering OCI_Connect_pool");

    if (!ctx || !ctx->ini)
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "ctx or ctx->ini is NULL");
        return -1;
    }

    /* ---- Allocate pool handle ---- */
    oci_pool_handle_t *pool = calloc(1, sizeof(oci_pool_handle_t));
    if (!pool)
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for oci_pool_handle_t");
        return -1;
    }

    /* ---- Copy all config values in one place before logging ---- */
    app_config_t *ini = ctx->ini;

    strncpy(pool->dbname,   ini->dbname,   sizeof(pool->dbname)   - 1);
    strncpy(pool->username, ini->username, sizeof(pool->username) - 1);
    strncpy(pool->password, ini->password, sizeof(pool->password) - 1);
    pool->use_wallet = ini->use_wallet;
    if (ini->use_wallet)
        strncpy(pool->wallet_location, ini->wallet_location,
                sizeof(pool->wallet_location) - 1);

    /* ---- Set TNS_ADMIN before any OCI call if using wallet ---- */
    if (pool->use_wallet)
    {
        if (pool->wallet_location[0] == '\0')
        {
            logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                         "use_wallet=1 but wallet_location is empty");
            free(pool->slots);
            free(pool);
            return -1;
        }
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "Setting TNS_ADMIN='%s' for wallet authentication",
                     pool->wallet_location);
        setenv("TNS_ADMIN", pool->wallet_location, 1);
    }

    pool->pool_min_size                    = ini->pool_min_size                    > 0  ? ini->pool_min_size                    : 1;
    pool->pool_max_size                    = ini->pool_max_size                    > 0  ? ini->pool_max_size                    : 10;
    pool->pool_increment                   = ini->pool_increment                   > 0  ? ini->pool_increment                   : 1;
    pool->connection_timeout               = ini->pool_connection_timeout          > 0  ? ini->pool_connection_timeout          : 30;
    pool->session_idle_timeout             = ini->session_idle_timeout             > 0  ? ini->session_idle_timeout             : 300;
    pool->max_time_to_establish            = ini->max_time_to_establish            > 0  ? ini->max_time_to_establish            : 15;
    pool->network_read_write_timeout       = ini->network_read_write_timeout       > 0  ? ini->network_read_write_timeout       : 60;
    pool->query_execution_timeout          = ini->query_execution_timeout          >= 0 ? ini->query_execution_timeout          : 0;
    pool->authentication_handshake_timeout = ini->authentication_handshake_timeout > 0  ? ini->authentication_handshake_timeout : 10;
    pool->retries_on_connection_failure    = ini->retries_on_connection_failure    >= 0 ? ini->retries_on_connection_failure    : 3;
    pool->login_auth_timeout               = ini->login_auth_timeout               > 0  ? ini->login_auth_timeout               : 10;
    pool->session_max_lifetime             = ini->session_max_lifetime             > 0  ? ini->session_max_lifetime             : 3600;
    pool->heartbeat_keepalive_interval     = ini->heartbeat_keepalive_interval     > 0  ? ini->heartbeat_keepalive_interval     : 60;
    pool->connection_validation_on_borrow  = ini->connection_validation_on_borrow;
    pool->rollback_on_return_to_pool       = ini->rollback_on_return_to_pool;
    pool->autocommit_mode                  = ini->autocommit_mode;

/* NLS strings: use ini value if set, fall back to safe default */
#define COPY_NLS(dst, src, dflt) \
    strncpy((dst), ((src)[0] ? (src) : (dflt)), sizeof(dst) - 1)

    COPY_NLS(pool->nls_date_format,     ini->nls_date_format,     "YYYY-MM-DD HH24:MI:SS");
    COPY_NLS(pool->nls_language,        ini->nls_language,        "AMERICAN");
    COPY_NLS(pool->nls_territory,       ini->nls_territory,       "AMERICA");
    COPY_NLS(pool->nls_characterset,    ini->nls_characterset,    "AL32UTF8");
    COPY_NLS(pool->nls_session_timezone,ini->nls_session_timezone,"UTC");
#undef COPY_NLS

    /* Log everything before the first OCI call */
    log_pool_config(pool, ctx->connectionpool_logger);


    metrics_record_t pool_metrics;
    metrics_init(&pool_metrics);
    metrics_set_context(&pool_metrics, ctx);
    pool_metrics.start_time_us = metrics_now_us();
    strncpy(pool_metrics.operation, "POOL_INIT",
            sizeof(pool_metrics.operation) - 1);
    strncpy(pool_metrics.object_name, pool->dbname,
            sizeof(pool_metrics.object_name) - 1);




    /* ---- Allocate slot array ---- */
    pool->slots = calloc((size_t)pool->pool_max_size, sizeof(pool_slot_t));
    if (!pool->slots)
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for pool slots (max_size=%d)",
                     pool->pool_max_size);
        free(pool);
        return -1;
    }
    pool->slot_count = pool->pool_max_size;

    /* ---- Initialise mutex and condvar ---- */
    if (pthread_mutex_init(&pool->mutex, NULL) != 0 ||
        pthread_cond_init (&pool->cond,  NULL) != 0)
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "pthread_mutex_init / pthread_cond_init failed");
        free(pool->slots);
        free(pool);
        return -1;
    }

    /* ---- OCI environment (thread-safe) ---- */
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Calling OCIEnvCreate (OCI_THREADED|OCI_OBJECT)");

    sword s = OCIEnvCreate(&pool->envhp,
                           OCI_THREADED | OCI_OBJECT,
                           NULL, NULL, NULL, NULL, 0, NULL);
    if (s != OCI_SUCCESS && s != OCI_SUCCESS_WITH_INFO)
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "OCIEnvCreate failed status=%d", s);
        rc = -1;
        goto Cleanup;
    }
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "OCIEnvCreate OK");

    /* ---- Pool-level error handle ---- */
    CHECK_OCI_POOL(pool->envhp,
        OCIHandleAlloc(pool->envhp, (void **)&pool->errhp,
                       OCI_HTYPE_ERROR, 0, NULL),
        ctx->connectionpool_logger, Cleanup);

    /* ---- Server handle ---- */
    CHECK_OCI_POOL(pool->errhp,
        OCIHandleAlloc(pool->envhp, (void **)&pool->srvhp,
                       OCI_HTYPE_SERVER, 0, NULL),
        ctx->connectionpool_logger, Cleanup);

    /* ---- Attach to Oracle server (shared by all slots) ---- */
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Calling OCIServerAttach dbname='%s'", pool->dbname);

    CHECK_OCI_POOL(pool->errhp,
        OCIServerAttach(pool->srvhp, pool->errhp,
                        (text *)pool->dbname,
                        (sb4)strlen(pool->dbname),
                        OCI_DEFAULT),
        ctx->connectionpool_logger, Cleanup);

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "OCIServerAttach OK");

    /* ---- Open pool_min_size slots ---- */
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Opening %d minimum pool slots", pool->pool_min_size);

    for (int i = 0; i < pool->pool_min_size; i++)
    {
        int slot_rc = -1;
        for (int attempt = 0;
             attempt < pool->retries_on_connection_failure;
             attempt++)
        {
            slot_rc = open_slot(pool, &pool->slots[i],
                                ctx->connectionpool_logger);
            if (slot_rc == 0) break;
            logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                         "Slot %d open attempt %d/%d failed - retrying",
                         i, attempt + 1,
                         pool->retries_on_connection_failure);
        }

        if (slot_rc != 0)
        {
            logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                         "Failed to open mandatory slot %d after %d attempts",
                         i, pool->retries_on_connection_failure);
            rc = -1;
            goto Cleanup;
        }

        pool->slots_open++;
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "Slot %d opened (%d/%d)",
                     i, pool->slots_open, pool->pool_min_size);
    }

    pool_metrics.end_time_us = metrics_now_us();
    pool_metrics.status_code = 0;
    pool_metrics.connection_create_us =
        pool_metrics.end_time_us - pool_metrics.start_time_us;
    pool_metrics.connection_acquire_us =
        pool_metrics.connection_create_us;
    metrics_finalise(&pool_metrics);
    metrics_write(ctx->connectionpool_logger ?
                  /* metrics_logger not in pool ctx yet — use a
                   * dedicated metrics_logger once it is wired in.
                   * For now write via connectionpool_logger file ptr
                   * by passing it directly; swap to metrics_logger
                   * once that field is copied into worker_ctx.       */
                  ctx->metrics_logger : NULL,
                  &pool_metrics);




    /* ---- Store pool handle in master context ---- */
    ctx->pool_handle    = pool;
    ctx->pool_slot_index = -1;  /* master context is not a worker */

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "OCI_Connect_pool complete: "
                 "slots_open=%d pool_max=%d",
                 pool->slots_open, pool->pool_max_size);
    return 0;

Cleanup:
	if (rc != 0)
	{
		 pool_metrics.end_time_us = metrics_now_us();
		 pool_metrics.status_code = rc;
		 metrics_finalise(&pool_metrics);
		 metrics_write(ctx->metrics_logger, &pool_metrics);
	}


    logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                 "OCI_Connect_pool failed - releasing resources");

    if (pool->slots)
    {
        for (int i = 0; i < pool->slot_count; i++)
            if (pool->slots[i].svchp)
                close_slot(&pool->slots[i], ctx->connectionpool_logger);
        free(pool->slots);
    }

    if (pool->srvhp)
    {
        OCIServerDetach(pool->srvhp, pool->errhp, OCI_DEFAULT);
        OCIHandleFree(pool->srvhp, OCI_HTYPE_SERVER);
    }
    if (pool->errhp) OCIHandleFree(pool->errhp, OCI_HTYPE_ERROR);
    if (pool->envhp) OCIHandleFree(pool->envhp, OCI_HTYPE_ENV);

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy (&pool->cond);
    free(pool);
    return rc;
}

/* ================================================================== */
/*  OCI_Disconnect_pool                                                 */
/* ================================================================== */
void OCI_Disconnect_pool(oci_context_t *ctx)
{
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Entering OCI_Disconnect_pool");

    if (!ctx || !ctx->pool_handle)
    {
        logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                     "No pool handle present - nothing to disconnect");
        return;
    }

    oci_pool_handle_t *pool = (oci_pool_handle_t *)ctx->pool_handle;

    pthread_mutex_lock(&pool->mutex);

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Closing %d open pool slots", pool->slots_open);

    for (int i = 0; i < pool->slot_count; i++)
        if (pool->slots[i].svchp)
            close_slot(&pool->slots[i], ctx->connectionpool_logger);

    free(pool->slots);
    pool->slots = NULL;

    pthread_mutex_unlock(&pool->mutex);

    if (pool->srvhp)
    {
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "Calling OCIServerDetach");
        OCIServerDetach(pool->srvhp, pool->errhp, OCI_DEFAULT);
        OCIHandleFree(pool->srvhp, OCI_HTYPE_SERVER);
        pool->srvhp = NULL;
    }
    if (pool->errhp) { OCIHandleFree(pool->errhp, OCI_HTYPE_ERROR); pool->errhp = NULL; }
    if (pool->envhp) { OCIHandleFree(pool->envhp, OCI_HTYPE_ENV);   pool->envhp = NULL; }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy (&pool->cond);

    free(pool);
    ctx->pool_handle = NULL;

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "OCI_Disconnect_pool complete");
}

/* ================================================================== */
/*  OCI_Pool_health_check                                               */
/* ================================================================== */
int OCI_Pool_health_check(oci_context_t *ctx)
{
    if (!ctx || !ctx->pool_handle) return -1;

    oci_pool_handle_t *pool = (oci_pool_handle_t *)ctx->pool_handle;
    int failures = 0;

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Starting pool health check");

    pthread_mutex_lock(&pool->mutex);

    time_t now = time(NULL);

    for (int i = 0; i < pool->slot_count; i++)
    {

        pool_slot_t *slot = &pool->slots[i];
        if (!slot->svchp || slot->state == POOL_SLOT_IN_USE)
            continue;   /* skip empty or borrowed slots */



        /* ---- session_max_lifetime check ---- */
        if (pool->session_max_lifetime > 0)
        {
            time_t age = now - slot->created;
            if (age > (time_t)pool->session_max_lifetime)
            {
                logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                             "Slot %d age=%lds exceeds session_max_lifetime=%ds"
                             " - recycling",
                             i, (long)age, pool->session_max_lifetime);
                close_slot(slot, ctx->connectionpool_logger);
                pool->slots_open--;
                if (open_slot(pool, slot, ctx->connectionpool_logger) == 0)
                    pool->slots_open++;
                else
                {
                    logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                                 "Slot %d reopen after max_lifetime failed",
                                 i);
                    failures++;
                }
                continue;
            }
        }

        /* ---- session_idle_timeout log ---- */
        if (pool->session_idle_timeout > 0)
        {
            time_t idle = now - slot->last_used;
            if (idle > (time_t)pool->session_idle_timeout)
                logger_write(ctx->connectionpool_logger, LOG_DEBUG, __func__, 0,
                             "Slot %d idle %lds (limit %ds) - will ping",
                             i, (long)idle, pool->session_idle_timeout);
        }

        /* ---- OCIPing validation ---- */
        if (!validate_slot(slot, ctx->connectionpool_logger))
        {
            logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                         "Slot %d ping failed - reopening", i);
            close_slot(slot, ctx->connectionpool_logger);
            pool->slots_open--;

            int recovered = 0;
            for (int attempt = 0;
                 attempt < pool->retries_on_connection_failure;
                 attempt++)
            {
                if (open_slot(pool, slot, ctx->connectionpool_logger) == 0)
                {
                    pool->slots_open++;
                    recovered = 1;
                    break;
                }
                logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                             "Slot %d reopen attempt %d/%d failed",
                             i, attempt + 1,
                             pool->retries_on_connection_failure);
            }

            if (!recovered)
            {
                logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                             "Slot %d could not be recovered", i);
                failures++;
            }
            else
            {
                logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                             "Slot %d recovered OK", i);
            }
        }
        else
        {
            logger_write(ctx->connectionpool_logger, LOG_DEBUG, __func__, 0,
                         "Slot %d healthy", i);
        }
    }

    pthread_mutex_unlock(&pool->mutex);

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Pool health check complete failures=%d", failures);
    return failures;
}

/* ================================================================== */
/*  OCI_Pool_get_session                                                */
/* ================================================================== */
int OCI_Pool_get_session(oci_context_t *ctx, oci_context_t *worker_ctx)
{
    if (!ctx || !ctx->pool_handle || !worker_ctx)
    {
        if (ctx)
            logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments");
        return -1;
    }
    uint64_t get_session_start_us = metrics_now_us();



    oci_pool_handle_t *pool = (oci_pool_handle_t *)ctx->pool_handle;

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Requesting pool session (timeout=%ds)",
                 pool->connection_timeout);

    /* Absolute timeout for pthread_cond_timedwait */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += pool->connection_timeout;

    pthread_mutex_lock(&pool->mutex);

    int slot_idx = -1;

    for (;;)
    {
        /* --- Scan for a free slot --- */
        for (int i = 0; i < pool->slot_count; i++)
        {
            if (pool->slots[i].svchp &&
                pool->slots[i].state == POOL_SLOT_FREE)
            {
                slot_idx = i;
                break;
            }
        }
        if (slot_idx >= 0) break;

        /* --- Grow the pool if allowed --- */
        if (pool->slots_open < pool->pool_max_size)
        {
            for (int i = 0; i < pool->slot_count; i++)
            {
                if (!pool->slots[i].svchp)
                {
                    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                                 "Pool growing: opening slot %d (%d/%d)",
                                 i, pool->slots_open + 1,
                                 pool->pool_max_size);

                    if (open_slot(pool, &pool->slots[i],
                                  ctx->connectionpool_logger) == 0)
                    {
                        pool->slots_open++;
                        slot_idx = i;
                    }
                    break;
                }
            }
            if (slot_idx >= 0) break;
        }

        /* --- All slots busy - wait --- */
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "All %d pool slots in use - waiting for release",
                     pool->slots_open);

        int wait_rc = pthread_cond_timedwait(&pool->cond,
                                              &pool->mutex,
                                              &deadline);
        if (wait_rc == ETIMEDOUT)
        {
            logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                         "Timed out waiting for pool slot after %ds",
                         pool->connection_timeout);
            pthread_mutex_unlock(&pool->mutex);
            return -1;
        }
    }

    pool_slot_t *slot = &pool->slots[slot_idx];

    /* ---- Optional validation on borrow ---- */
    if (pool->connection_validation_on_borrow)
    {
        logger_write(ctx->connectionpool_logger, LOG_DEBUG, __func__, 0,
                     "Validating slot %d before borrow", slot_idx);

        if (!validate_slot(slot, ctx->connectionpool_logger))
        {
            logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                         "Slot %d failed validation - reopening",
                         slot_idx);
            close_slot(slot, ctx->connectionpool_logger);
            pool->slots_open--;

            int reopen_rc = -1;
            for (int attempt = 0;
                 attempt < pool->retries_on_connection_failure;
                 attempt++)
            {
                reopen_rc = open_slot(pool, slot, ctx->connectionpool_logger);
                if (reopen_rc == 0) { pool->slots_open++; break; }
                logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                             "Reopen attempt %d/%d failed",
                             attempt + 1,
                             pool->retries_on_connection_failure);
            }

            if (reopen_rc != 0)
            {
                logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                             "Could not recover slot %d", slot_idx);
                pthread_mutex_unlock(&pool->mutex);
                return -1;
            }
        }
    }
    /* Mark slot in-use before releasing the lock */
	uint64_t get_session_slot_us = metrics_now_us();   /* slot secured  */
	slot->state = POOL_SLOT_IN_USE;

	pthread_mutex_unlock(&pool->mutex);
	uint64_t get_session_end_us = metrics_now_us();


    /* ---- Populate worker context ---- */
    memset(worker_ctx, 0, sizeof(oci_context_t));
    worker_ctx->svchp           = slot->svchp;
    worker_ctx->errhp           = slot->errhp;
    worker_ctx->envhp           = pool->envhp;
    worker_ctx->ini             = ctx->ini;
    worker_ctx->logger          = ctx->logger;
    worker_ctx->NLS_DATE_FORMAT = pool->nls_date_format;
    worker_ctx->pool_handle     = ctx->pool_handle; /* needed by release */
    worker_ctx->pool_slot_index = slot_idx;
    worker_ctx->connection_create_us  = get_session_slot_us   - get_session_slot_us;
    worker_ctx->connection_acquire_us = get_session_end_us   - get_session_start_us;

    worker_ctx->connection_wait_us    = get_session_slot_us - get_session_start_us;
    worker_ctx->connection_create_us  = get_session_end_us  - get_session_slot_us;
    worker_ctx->connection_acquire_us = get_session_end_us  - get_session_start_us;

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Pool session granted: slot=%d", slot_idx);
    return 0;
}

/* ================================================================== */
/*  OCI_Pool_release_session                                            */
/* ================================================================== */
void OCI_Pool_release_session(oci_context_t *ctx,
                               oci_context_t *worker_ctx)
{
    if (!ctx || !ctx->pool_handle || !worker_ctx)
    {
        if (ctx)
            logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                         "Invalid arguments - nothing released");
        return;
    }

    oci_pool_handle_t *pool = (oci_pool_handle_t *)ctx->pool_handle;
    int slot_idx = worker_ctx->pool_slot_index;

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Releasing pool session slot=%d", slot_idx);

    if (slot_idx < 0 || slot_idx >= pool->slot_count)
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "Invalid pool_slot_index=%d", slot_idx);
        return;
    }

    pool_slot_t *slot = &pool->slots[slot_idx];

    /* ---- Rollback on return ---- */
    if (pool->rollback_on_return_to_pool &&
        slot->svchp && slot->errhp)
    {
        logger_write(ctx->connectionpool_logger, LOG_DEBUG, __func__, 0,
                     "OCITransRollback on return slot=%d", slot_idx);
        sword s = OCITransRollback(slot->svchp, slot->errhp, OCI_DEFAULT);
        if (s != OCI_SUCCESS && s != OCI_SUCCESS_WITH_INFO)
        {
            text buf[512]; sb4 code = 0;
            OCIErrorGet(slot->errhp, 1, NULL, &code,
                        buf, sizeof(buf), OCI_HTYPE_ERROR);
            logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                         "Rollback on return error %d: %s (continuing)",
                         code, buf);
        }
    }

    pthread_mutex_lock(&pool->mutex);
    slot->last_used = time(NULL);
    slot->state     = POOL_SLOT_FREE;
    pthread_cond_signal(&pool->cond);   /* wake one waiting get_session */
    pthread_mutex_unlock(&pool->mutex);

    /* Wipe worker context so stale handles cannot be accidentally reused */
    memset(worker_ctx, 0, sizeof(oci_context_t));

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Pool session released: slot=%d", slot_idx);
}
