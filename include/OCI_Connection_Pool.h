/*
 * OCI_Connection_Pool.h
 *
 * OCI Connection Pool Module
 * ---------------------------
 * Wraps a hand-managed array of OCISvcCtx / OCISession pairs to
 * provide a thread-safe, configuration-driven connection pool for
 * the Data_Manager project.
 *
 * Why not OCIConnectionPool (OCI_CPOOL)?
 * --------------------------------------
 * OCIConnectionPool is a server-side mechanism requiring Oracle
 * shared-server / MTS configuration.  For the common dedicated-server
 * Oracle setup the correct approach is to manage a pool of
 * OCISession handles ourselves, sharing only the OCIEnv and
 * OCIServer handles across the pool (safe per OCI documentation).
 *
 * Design
 * ------
 *   - One pool per process, initialised at startup via OCI_Connect_pool.
 *   - pool_min_size sessions are opened at init; the pool grows up to
 *     pool_max_size on demand, adding pool_increment slots at a time.
 *   - OCI_Pool_get_session blocks on a pthread_cond_timedwait until a
 *     free slot is available or pool_connection_timeout expires.
 *   - OCI_Pool_release_session marks the slot free and signals waiters.
 *   - Optional OCIPing on borrow (connection_validation_on_borrow=1).
 *   - Optional rollback on return (rollback_on_return_to_pool=1).
 *   - Background health check via OCI_Pool_health_check() which can
 *     be called from a heartbeat thread.
 *
 * Worker thread usage pattern
 * ---------------------------
 *   oci_context_t worker_ctx;
 *   OCI_Pool_get_session(&master_ctx, &worker_ctx);
 *   // ... use worker_ctx with any execute module unchanged ...
 *   OCI_Pool_release_session(&master_ctx, &worker_ctx);
 *
 * All pool parameters are read from app_config_t (populated by
 * load_ini) - see ini_reader.h for the full parameter list.
 */

#ifndef OCI_CONNECTION_POOL_H
#define OCI_CONNECTION_POOL_H

#include <pthread.h>
#include <time.h>
#include "OCI_Connection.h"
#include "ini_reader.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Pool slot state                                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    POOL_SLOT_FREE   = 0,
    POOL_SLOT_IN_USE = 1
} pool_slot_state_t;

/* ------------------------------------------------------------------ */
/*  Per-slot descriptor - one authenticated OCI session                */
/* ------------------------------------------------------------------ */
typedef struct {
    pool_slot_state_t  state;
    OCISvcCtx         *svchp;      /* service context for this slot   */
    OCISession        *authp;      /* session handle for this slot    */
    OCIError          *errhp;      /* error handle (per-slot)         */
    time_t             created;    /* epoch when slot was first opened */
    time_t             last_used;  /* epoch of last release call      */
} pool_slot_t;

/* ------------------------------------------------------------------ */
/*  Pool handle - stored in master oci_context_t.pool_handle           */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Shared OCI handles (safe to share across threads per OCI docs) */
    OCIEnv    *envhp;
    OCIError  *errhp;        /* pool-level error handle               */
    OCIServer *srvhp;        /* single server attachment              */

    /* Slot array */
    pool_slot_t *slots;      /* [pool_max_size] heap array            */
    int          slot_count; /* allocated length of slots[]           */
    int          slots_open; /* number of slots currently open        */

    /* Thread synchronisation */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;

    /* ---- Configuration (copied from ini at init for logging) ---- */
    int  pool_min_size;
    int  pool_max_size;
    int  pool_increment;
    int  connection_timeout;          /* borrow wait limit (seconds)  */
    int  session_idle_timeout;        /* idle recycle threshold        */
    int  max_time_to_establish;       /* OCIServerAttach limit         */
    int  network_read_write_timeout;
    int  query_execution_timeout;
    int  authentication_handshake_timeout;
    int  retries_on_connection_failure;
    int  login_auth_timeout;
    int  session_max_lifetime;
    int  heartbeat_keepalive_interval;
    int  connection_validation_on_borrow;
    int  rollback_on_return_to_pool;
    int  autocommit_mode;

    char nls_date_format    [64];
    char nls_language       [64];
    char nls_territory      [64];
    char nls_characterset   [64];
    char nls_session_timezone[64];

    /* Credentials (copied from config at init) */
    char dbname  [128];
    char username[64];
    char password[64];
    int  use_wallet;              /* 0=legacy RDBMS  1=wallet OCI_CRED_EXT */
   char wallet_location[256];    /* path to Oracle Wallet directory        */

} oci_pool_handle_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * OCI_Connect_pool()
 *
 * Initialise the OCI environment (OCI_THREADED), open pool_min_size
 * sessions, apply NLS ALTER SESSION statements, and store the pool
 * handle in ctx->pool_handle.
 *
 * All parameters read from ctx->ini.
 *
 * Returns  0  success
 *         -1  error (logged)
 */
int OCI_Connect_pool(oci_context_t *ctx);

/*
 * OCI_Disconnect_pool()
 *
 * End all sessions, detach from server, free all OCI handles.
 * Must be called once at application shutdown.
 */
void OCI_Disconnect_pool(oci_context_t *ctx);

/*
 * OCI_Pool_get_session()
 *
 * Borrow a session from the pool.  Blocks up to pool_connection_timeout
 * seconds.  On success worker_ctx is fully populated and ready for use
 * by any existing execute module without modification.
 *
 * Returns  0  success - worker_ctx populated
 *         -1  timeout or error (logged)
 */
int OCI_Pool_get_session(oci_context_t *ctx, oci_context_t *worker_ctx);

/*
 * OCI_Pool_release_session()
 *
 * Return a borrowed session to the pool.
 * Issues OCITransRollback first if rollback_on_return_to_pool = 1.
 * Zeroes worker_ctx so stale handles cannot be reused.
 */
void OCI_Pool_release_session(oci_context_t *ctx,
                               oci_context_t *worker_ctx);

/*
 * OCI_Pool_health_check()
 *
 * Ping all FREE slots.  Expired (session_max_lifetime) or idle
 * (session_idle_timeout) slots are recycled.  Dead slots are reopened
 * with retries_on_connection_failure attempts.
 *
 * Returns number of slots that could not be recovered (0 = all healthy).
 * Safe to call from a background heartbeat thread.
 */
int OCI_Pool_health_check(oci_context_t *ctx);

#endif /* OCI_CONNECTION_POOL_H */
