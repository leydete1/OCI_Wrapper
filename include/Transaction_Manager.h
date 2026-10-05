/*
 * OCI_Transaction_Manager.h
 *
 * Transaction Manager Module
 * ---------------------------
 * Provides explicit transaction lifecycle management for the
 * Data_Manager project.
 *
 * Design overview
 * ---------------
 * Oracle's OCI operates in manual-commit mode by default (autocommit_mode=0
 * in config.ini).  Individual execute modules (insert/update/delete) each
 * commit their own work at the end of a successful operation.
 * The Transaction Manager overrides that implicit per-call commit model by
 * giving the CLIENT explicit control over when a logical transaction begins,
 * what work it encompasses across multiple DML calls, and when it is
 * finally committed, rolled back, or aborted.
 *
 * Because the OCI connection is "sticky" (the same oci_context_t / svchp
 * handle is reused for every call within a client session), work executed
 * on that context after tx_begin() and before tx_commit() / tx_rollback()
 * all belongs to the same Oracle transaction automatically.  This module
 * does not need to intercept those calls — it just:
 *   1. Marks the transaction as ACTIVE and assigns it a UUID.
 *   2. Returns the transaction ID to the client in XML so it can be passed
 *      back on subsequent DML calls for correlation / audit purposes.
 *   3. On commit / rollback / abort it asks the database driver
 *      (db_driver.h commit()/rollback()) and updates transaction state.
 *   4. Enforces a configurable idle timeout via tx_check_timeout(); the
 *      caller (heartbeat thread or request handler) should invoke this
 *      periodically.
 *
 * Session ID
 * ----------
 * The session_id field is accepted as a caller-supplied string.  When the
 * Session Manager module is complete the caller will pass the real session
 * UUID.  Until then the caller may pass NULL or an empty string and a
 * placeholder value ("-") will be recorded.  The UUID generation helper
 * tx_generate_uuid() is exposed so the Session Manager can call it for its
 * own IDs.
 *
 * Threading
 * ---------
 * The tx_handle_t is per-connection / per-context.  No global state is kept
 * in this module.  Thread safety is the caller's responsibility at the
 * context level (same rule as every other module in the project).
 *
 * XML output format
 * -----------------
 * All public functions return an XML string via result_xml / cfg->xml->OUTPUT_XML.
 * The caller is responsible for freeing the returned string.
 *
 *   tx_begin() success:
 *     <transaction>
 *       <operation>BEGIN</operation>
 *       <transaction_id>xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx</transaction_id>
 *       <session_id>...</session_id>
 *       <status>ACTIVE</status>
 *       <start_time>2026-06-10 21:08:11.159056</start_time>
 *       <timeout_seconds>300</timeout_seconds>
 *     </transaction>
 *
 *   tx_commit() / tx_rollback() / tx_abort() success:
 *     <transaction>
 *       <operation>COMMIT|ROLLBACK|ABORT</operation>
 *       <transaction_id>...</transaction_id>
 *       <session_id>...</session_id>
 *       <status>COMMITTED|ROLLED_BACK|ABORTED</status>
 *       <duration_us>12345</duration_us>
 *       <execution_time>0.012345</execution_time>
 *     </transaction>
 *
 * config.ini parameters (all under [transaction] section, loaded via ini_reader)
 * -------------------------------------------------------------------------------
 *   tx_timeout_seconds          = 300   # auto-rollback idle transactions
 *   tx_max_retries              = 3     # retry count on OCI transient errors
 *   tx_retry_delay_ms           = 500   # delay between retries in ms
 *   tx_log_begin                = 1     # log BEGIN events (0=off 1=on)
 *   tx_log_commit               = 1     # log COMMIT events
 *   tx_log_rollback             = 1     # log ROLLBACK events
 *   tx_log_timeout              = 1     # log TIMEOUT events
 *
 * Integration
 * -----------
 * Add to app_config_t in ini_reader.h:
 *   int  tx_timeout_seconds;
 *   int  tx_max_retries;
 *   int  tx_retry_delay_ms;
 *   int  tx_log_begin;
 *   int  tx_log_commit;
 *   int  tx_log_rollback;
 *   int  tx_log_timeout;
 *
 * Add to ctx_map[] in ini_reader.c (see bottom of this file).
 *
 * Add to config.ini (see OCI_Transaction_Manager_config.ini.patch).
 *
 * Compile additions
 * -----------------
 *   OCI_Transaction_Manager.c
 */

#ifndef OCI_TRANSACTION_MANAGER_H
#define OCI_TRANSACTION_MANAGER_H

#include <stdint.h>
#include <time.h>
#include "Connection.h"
#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Transaction status codes                                            */
/* ------------------------------------------------------------------ */
typedef enum {
    TX_STATUS_NONE        = 0,   /* no active transaction              */
    TX_STATUS_ACTIVE      = 1,   /* BEGIN issued, work in progress      */
    TX_STATUS_COMMITTED   = 2,   /* driver commit completed OK          */
    TX_STATUS_ROLLED_BACK = 3,   /* driver rollback completed OK        */
    TX_STATUS_ABORTED     = 4,   /* forced abort (error / timeout)      */
    TX_STATUS_TIMED_OUT   = 5    /* idle timeout exceeded               */
} tx_status_t;

/* ------------------------------------------------------------------ */
/*  Transaction result codes                                            */
/* ------------------------------------------------------------------ */
#define TX_OK               0
#define TX_ERR_INVALID_ARG -1
#define TX_ERR_ALREADY_ACTIVE -2   /* tx_begin when one already active */
#define TX_ERR_NO_ACTIVE    -3     /* commit/rollback with no active tx */
#define TX_ERR_OCI_FAILURE  -4     /* database call returned an error -
                                       name kept for existing callers  */
#define TX_ERR_TIMEOUT      -5     /* operation exceeded timeout        */
#define TX_ERR_ALLOC        -6     /* malloc / calloc failure           */

/* ------------------------------------------------------------------ */
/*  UUID string length (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx + NUL)   */
/* ------------------------------------------------------------------ */
#define TX_UUID_LEN  37

/* ------------------------------------------------------------------ */
/*  Transaction handle - one instance per oci_context_t                */
/*  Embed directly in the caller's context or allocate on the stack;  */
/*  do NOT share across threads without external locking.             */
/* ------------------------------------------------------------------ */
typedef struct tx_handle_s {

    /* Identity */
    char        transaction_id [TX_UUID_LEN];  /* RFC 4122 v4 UUID        */
    char        session_id     [TX_UUID_LEN];  /* caller-supplied or "-"  */
    char        tx_name        [128];          /* human-readable name e.g.
                                                  "Save Booking",
                                                  "Commit Payment"        */

    /* State */
    tx_status_t status;                        /* current lifecycle state */

    /* Timing */
    uint64_t    start_time_us;                 /* wall-clock at BEGIN      */
    uint64_t    end_time_us;                   /* wall-clock at resolution */
    time_t      last_activity_epoch;           /* for idle timeout check   */

    /* Configuration (copied from ctx->ini at BEGIN)                    */
    int         timeout_seconds;              /* 0 = no timeout           */
    int         max_retries;
    int         retry_delay_ms;

    /* Back-pointer to OCI context (not owned, do not free)             */
    oci_context_t *ctx;

} tx_handle_t;

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

/*
 * tx_generate_uuid()
 *
 * Generate a random RFC 4122 version-4 UUID string into buf[TX_UUID_LEN].
 * Uses /dev/urandom.  Falls back to rand()-based generation if the device
 * is unavailable (adequate for non-cryptographic transaction IDs).
 *
 * buf must be at least TX_UUID_LEN (37) bytes.
 * Returns buf for convenience.
 */
char *tx_generate_uuid(char *buf);

/*
 * tx_init()
 *
 * Zero-initialise a tx_handle_t and set status to TX_STATUS_NONE.
 * Must be called before any other tx_* function on a new handle.
 *
 * Parameters
 *   handle  - handle to initialise
 *   ctx     - OCI context the transaction will run on
 */
void tx_init(tx_handle_t *handle, oci_context_t *ctx);

/*
 * tx_begin()
 *
 * Start a new transaction.
 *
 *   - Assigns a fresh UUID as transaction_id.
 *   - Records session_id (caller-supplied; pass NULL or "" for placeholder).
 *   - Sets status to TX_STATUS_ACTIVE.
 *   - Records start_time_us and last_activity_epoch.
 *   - Copies timeout / retry parameters from ctx->ini.
 *   - Logs the event to ctx->transaction_logger.
 *   - Populates *result_xml with a heap-allocated XML string describing
 *     the transaction.  Caller must free() it.
 *
 * Returns TX_OK on success, TX_ERR_ALREADY_ACTIVE if a transaction is
 * already open on this handle, TX_ERR_INVALID_ARG for NULL inputs.
 *
 * Note: this function does not start anything in the database. The
 * database begins a transaction implicitly on the first DML statement
 * (an explicit start call is only needed for XA / distributed
 * transactions, which are not in scope here). Marking the handle
 * ACTIVE and returning the UUID is the correct and sufficient action.
 */
int tx_begin(tx_handle_t  *handle,
             const char   *session_id,
             const char   *tx_name,
             char        **result_xml);

/*
 * tx_commit()
 *
 * Commit the active transaction.
 *
 *   - Commits through tx_commit_with_retry() (below), which retries
 *     up to handle->max_retries times while the driver reports the
 *     failure as retryable. Which failures are retryable is the
 *     driver's decision; for Oracle (item 3c, 2026-10-05) only the
 *     transient lock/memory errors ORA-04020/04021/04031 are - a
 *     failure after which the server has already rolled back
 *     (ORA-02091) or the connection was lost is not retried.
 *   - On success sets status to TX_STATUS_COMMITTED.
 *   - On failure rolls back (driver rollback(), best effort) and sets
 *     status to TX_STATUS_ABORTED.
 *   - Logs to ctx->transaction_logger.
 *   - Populates *result_xml.  Caller must free() it.
 *
 * Returns TX_OK, TX_ERR_NO_ACTIVE, TX_ERR_OCI_FAILURE, TX_ERR_INVALID_ARG.
 */
int tx_commit(tx_handle_t  *handle,
              char        **result_xml);

/*
 * tx_rollback()
 *
 * Roll back the active transaction explicitly (client-initiated).
 *
 *   - Calls the driver's rollback() on ctx.
 *   - Sets status to TX_STATUS_ROLLED_BACK.
 *   - Logs to ctx->transaction_logger.
 *   - Populates *result_xml.  Caller must free() it.
 *
 * Returns TX_OK, TX_ERR_NO_ACTIVE, TX_ERR_OCI_FAILURE, TX_ERR_INVALID_ARG.
 */
int tx_rollback(tx_handle_t  *handle,
                char        **result_xml);

/*
 * tx_abort()
 *
 * Force-abort the active transaction (internal error path).
 *
 *   - Calls the driver's rollback(), best effort (errors logged only).
 *   - Sets status to TX_STATUS_ABORTED.
 *   - Logs to ctx->transaction_logger at LOG_ERROR level.
 *   - Populates *result_xml.  Caller must free() it.
 *   - Safe to call even when status is not ACTIVE (no-op with log entry).
 *
 * Returns TX_OK always (abort is fire-and-forget).
 */
int tx_abort(tx_handle_t  *handle,
             const char   *reason,
             char        **result_xml);

/*
 * tx_check_timeout()
 *
 * Check whether the active transaction has exceeded its idle timeout.
 * Call this from a heartbeat thread or at the start of each request handler.
 *
 *   - If handle->status != TX_STATUS_ACTIVE, returns TX_OK immediately.
 *   - If handle->timeout_seconds == 0, timeout checking is disabled;
 *     returns TX_OK immediately.
 *   - If (now - last_activity_epoch) >= timeout_seconds, calls tx_abort()
 *     internally, sets status to TX_STATUS_TIMED_OUT, and returns TX_ERR_TIMEOUT.
 *   - result_xml is populated when a timeout occurs; NULL is safe to pass
 *     when the caller does not need the XML on timeout.
 *
 * Returns TX_OK (not timed out) or TX_ERR_TIMEOUT (aborted due to idle).
 */
int tx_check_timeout(tx_handle_t  *handle,
                     char        **result_xml);

/*
 * tx_touch()
 *
 * Update last_activity_epoch to the current time.
 * Call this after every successful DML step that belongs to the transaction
 * to prevent premature idle timeout.
 *
 * Safe to call with a NULL or NONE-status handle (no-op).
 */
void tx_touch(tx_handle_t *handle);

/*
 * tx_status_str()
 *
 * Return a human-readable string for a tx_status_t value.
 * Returned pointer is a string literal — do NOT free.
 */
const char *tx_status_str(tx_status_t status);

/*
 * tx_get_id()
 *
 * Return the transaction_id string for the current handle.
 * Returns "-" if no transaction is active or handle is NULL.
 * Returned pointer is valid for the lifetime of the handle.
 */
const char *tx_get_id(const tx_handle_t *handle);

/*
 * tx_is_active()
 *
 * Returns 1 if the transaction is currently in TX_STATUS_ACTIVE state,
 * 0 otherwise.  Convenience wrapper for callers that gate DML on an
 * active transaction.
 */
int tx_is_active(const tx_handle_t *handle);

/*
 * tx_commit_with_retry()
 *
 * Oracle dialect extraction, Stage 3 (2026-10-03). The one commit retry
 * loop in the project - replaces oci_trans_commit_retry(), which made
 * the OCI calls itself. Used by tx_commit() and by every standalone
 * commit site (Insert/Update/Delete when this call owns its own commit
 * boundary), so both paths keep the retry the standalone path gained
 * on 2026-09-23.
 *
 * Each attempt is one driver commit() (db_driver.h). The loop retries
 * only while the driver reports DB_TX_RETRYABLE, up to max_retries
 * extra attempts with retry_delay_ms between them; 0 ends it as
 * success, any other result ends it as failure. Which failures are
 * retryable is the driver's knowledge, not core's.
 *
 * ctx      - a connected session.
 * logger   - destination for the attempt trail ("Commit attempt n/m");
 *            ctx->transaction_logger from tx_commit(), the module's own
 *            logger (e.g. ctx->insert_logger) from a standalone caller.
 *            The driver logs its vendor detail on the same logger.
 * max_retries, retry_delay_ms - config.ini's tx_max_retries /
 *            tx_retry_delay_ms (tx_handle_t's copies from tx_commit()).
 * out_attempts - if non-NULL, set to the number of commit() calls made
 *            (1 on first-try success).
 *
 * Returns 0 on success, -1 on failure - caller decides what failure
 * means for it (tx_commit() rolls back and marks ABORTED; a standalone
 * caller rolls back on its own error path, as before).
 */
int tx_commit_with_retry(oci_context_t *ctx, logger_t *logger,
                         int max_retries, int retry_delay_ms,
                         int *out_attempts);


/*
 * begin_standalone_tx_if_needed() / end_standalone_tx_if_owned()
 *
 * Fixes a real GxP traceability gap found 2026-07-26: when
 * execute_insert_batch()/execute_update_batch()/execute_delete_batch()
 * runs standalone - not wrapped in an explicit tx_begin()/tx_commit()
 * transaction by its caller - ctx->active_tx is NULL, so
 * metrics_set_context() (metrics.c) has nothing to read and every
 * metrics row produced during that call reports transaction_id="-".
 * That includes the before-image SELECT, the AUDIT_TRAIL INSERT, and
 * the operation's own row - three rows that are clearly one atomic
 * unit (Oracle itself treats them as a single implicit transaction,
 * auto-committed together at the end of the call) with no shared
 * identifier linking them, which is exactly the kind of gap an
 * auditor would immediately question.
 *
 * begin_standalone_tx_if_needed() gives that call its own transaction
 * identity when it doesn't already have one:
 *   - If ctx->active_tx is already set (this call is itself nested
 *     inside another - e.g. the audit trail's own INSERT nested inside
 *     the business operation that triggered it), does nothing and
 *     returns 0 - the outer call's transaction_id is correctly
 *     inherited by every nested metrics_set_context() call unchanged,
 *     since ctx->active_tx is a single, shared context-level pointer
 *     every such call already reads from.
 *   - Otherwise, initialises local_tx (caller-owned, typically a
 *     stack variable), generates a fresh UUID for it, marks it
 *     TX_STATUS_ACTIVE (so tx_get_id() actually returns the UUID
 *     instead of "-" - see tx_get_id()'s own status check), points
 *     ctx->active_tx at it, and returns 1.
 *
 * end_standalone_tx_if_owned() must be called at the end of the same
 * function (Cleanup, or wherever it returns) with whatever
 * begin_standalone_tx_if_needed() returned - if that was 1, clears
 * ctx->active_tx back to NULL (local_tx itself just goes out of scope,
 * nothing to free); if 0, does nothing, since ctx->active_tx wasn't
 * this call's to touch in the first place.
 *
 * local_tx is never registered with any transaction table/commit
 * bookkeeping - it exists purely to give metrics_set_context() a
 * transaction_id to read for the duration of one standalone call, not
 * to become a real multi-call transaction a caller could later
 * tx_commit()/tx_rollback() themselves.
 */
int  begin_standalone_tx_if_needed(oci_context_t *ctx, tx_handle_t *local_tx);
void end_standalone_tx_if_owned(oci_context_t *ctx, int owned);

#ifdef __cplusplus
}
#endif

#endif /* OCI_TRANSACTION_MANAGER_H */
