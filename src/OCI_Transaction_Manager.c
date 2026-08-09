/*
 * OCI_Transaction_Manager.c
 *
 * Transaction Manager Module
 * ---------------------------
 * Implements explicit transaction lifecycle management:
 *   tx_generate_uuid()   - RFC 4122 v4 UUID generation
 *   tx_init()            - zero-initialise a tx_handle_t
 *   tx_begin()           - mark transaction ACTIVE, assign UUID, return XML
 *   tx_commit()          - OCITransCommit + result XML
 *   tx_rollback()        - OCITransRollback (client-initiated) + result XML
 *   tx_abort()           - force rollback on error path + result XML
 *   tx_check_timeout()   - idle timeout enforcement
 *   tx_touch()           - reset idle timer after a DML step
 *   tx_status_str()      - status enum -> string
 *   tx_get_id()          - safe accessor for transaction_id
 *   tx_is_active()       - convenience active check
 *
 * All logging goes to ctx->transaction_logger.
 * All OCI work uses ctx->svchp / ctx->errhp (standard project pattern).
 * All XML output is heap-allocated; caller is responsible for free().
 *
 * See OCI_Transaction_Manager.h for full design notes.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "OCI_Transaction_Manager.h"
#include "OCI_Connection.h"
#include "logger.h"
#include "metrics.h"
#include "metrics_writer.h"   /* metrics_finalise_and_enqueue() - closure item 5, Stage 2 */

/* ------------------------------------------------------------------ */
/*  Internal helpers - forward declarations                            */
/* ------------------------------------------------------------------ */
static char *tx_build_begin_xml   (const tx_handle_t *handle);
static char *tx_build_result_xml  (const tx_handle_t *handle,
                                    const char        *operation,
                                    const char        *extra_element,
                                    const char        *extra_value);
static void  tx_log_oci_error     (oci_context_t *ctx,
                                    const char    *func_name);

/* ------------------------------------------------------------------ */
/*  Microsecond wall-clock - mirrors metrics_now_us()                  */
/* ------------------------------------------------------------------ */
static uint64_t tx_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)(ts.tv_nsec / 1000);
}

/* ------------------------------------------------------------------ */
/*  Formatted timestamp string for XML                                  */
/*  Writes "YYYY-MM-DD HH:MM:SS.mmmmmm" into buf (must be >= 32 bytes)*/
/* ------------------------------------------------------------------ */
static void tx_timestamp_str(uint64_t us_epoch, char *buf, size_t len)
{
    time_t  sec = (time_t)(us_epoch / 1000000ULL);
    long    us  = (long)  (us_epoch % 1000000ULL);

    struct tm tm_info;
    gmtime_r(&sec, &tm_info);

    char tmp[32];
    strftime(tmp, sizeof(tmp), "%Y-%m-%d %H:%M:%S", &tm_info);
    snprintf(buf, len, "%s.%06ld", tmp, us);
}

/* ================================================================== */
/*  tx_generate_uuid                                                    */
/*  RFC 4122 version-4 (random) UUID.                                  */
/*  Primary source: /dev/urandom.                                      */
/*  Fallback: time-seeded rand() for environments without /dev/urandom */
/* ================================================================== */
char *tx_generate_uuid(char *buf)
{
    unsigned char rnd[16];
    int           used_urandom = 0;

    FILE *fp = fopen("/dev/urandom", "rb");
    if (fp)
    {
        if (fread(rnd, 1, sizeof(rnd), fp) == sizeof(rnd))
            used_urandom = 1;
        fclose(fp);
    }

    if (!used_urandom)
    {
        /* Fallback: deterministic but adequate for non-security IDs  */
        srand((unsigned int)(time(NULL) ^ (uintptr_t)buf));
        for (int i = 0; i < 16; i++)
            rnd[i] = (unsigned char)(rand() & 0xFF);
    }

    /* Apply RFC 4122 version and variant bits */
    rnd[6] = (rnd[6] & 0x0F) | 0x40;   /* version 4         */
    rnd[8] = (rnd[8] & 0x3F) | 0x80;   /* variant bits 10xx */

    snprintf(buf, TX_UUID_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             rnd[0],  rnd[1],  rnd[2],  rnd[3],
             rnd[4],  rnd[5],
             rnd[6],  rnd[7],
             rnd[8],  rnd[9],
             rnd[10], rnd[11], rnd[12],
             rnd[13], rnd[14], rnd[15]);

    return buf;
}

/* ================================================================== */
/*  tx_init                                                             */
/* ================================================================== */
void tx_init(tx_handle_t *handle, oci_context_t *ctx)
{
    if (!handle) return;

    memset(handle, 0, sizeof(*handle));

    /* Safe string defaults */
    strncpy(handle->transaction_id, "-", TX_UUID_LEN - 1);
    strncpy(handle->session_id,     "-", TX_UUID_LEN - 1);
    strncpy(handle->tx_name,        "-", sizeof(handle->tx_name) - 1);

    handle->status = TX_STATUS_NONE;
    handle->ctx    = ctx;

    if (ctx && ctx->transaction_logger)
        logger_write(ctx->transaction_logger, LOG_DEBUG, __func__, 0,
                     "tx_handle_t initialised ctx=%p", (void *)ctx);
}

/* ================================================================== */
/*  tx_status_str                                                       */
/* ================================================================== */
const char *tx_status_str(tx_status_t status)
{
    switch (status)
    {
        case TX_STATUS_NONE:        return "NONE";
        case TX_STATUS_ACTIVE:      return "ACTIVE";
        case TX_STATUS_COMMITTED:   return "COMMITTED";
        case TX_STATUS_ROLLED_BACK: return "ROLLED_BACK";
        case TX_STATUS_ABORTED:     return "ABORTED";
        case TX_STATUS_TIMED_OUT:   return "TIMED_OUT";
        default:                    return "UNKNOWN";
    }
}

/* ================================================================== */
/*  tx_get_id                                                           */
/* ================================================================== */
const char *tx_get_id(const tx_handle_t *handle)
{
    if (!handle || handle->status == TX_STATUS_NONE)
        return "-";
    return handle->transaction_id;
}

/* ================================================================== */
/*  tx_is_active                                                        */
/* ================================================================== */
int tx_is_active(const tx_handle_t *handle)
{
    return (handle && handle->status == TX_STATUS_ACTIVE) ? 1 : 0;
}

/* ================================================================== */
/*  begin_standalone_tx_if_needed / end_standalone_tx_if_owned          */
/*  See doc comment in OCI_Transaction_Manager.h for the full           */
/*  reasoning - fixes the 2026-07-26 GxP traceability gap.              */
/* ================================================================== */
int begin_standalone_tx_if_needed(oci_context_t *ctx, tx_handle_t *local_tx)
{
    if (!ctx) return 0;

    if (ctx->active_tx)
    {
        /* Already have one - this call is nested inside another that
         * already gave itself (or was given) a transaction identity.
         * Not ours to touch.                                          */
        return 0;
    }

    tx_init(local_tx, ctx);
    tx_generate_uuid(local_tx->transaction_id);
    local_tx->status = TX_STATUS_ACTIVE;
    ctx->active_tx = local_tx;

    if (ctx->transaction_logger)
        logger_write(ctx->transaction_logger, LOG_DEBUG, __func__, 0,
                     "Standalone call given its own transaction identity "
                     "for metrics/audit traceability: tx_id='%s'",
                     local_tx->transaction_id);

    return 1;
}

void end_standalone_tx_if_owned(oci_context_t *ctx, int owned)
{
    if (!ctx || !owned) return;
    ctx->active_tx = NULL;
}

/* ================================================================== */
/*  tx_touch                                                            */
/* ================================================================== */
void tx_touch(tx_handle_t *handle)
{
    if (!handle || handle->status != TX_STATUS_ACTIVE)
        return;

    handle->last_activity_epoch = time(NULL);

    if (handle->ctx && handle->ctx->transaction_logger)
        logger_write(handle->ctx->transaction_logger, LOG_DEBUG,
                     __func__, 0,
                     "tx_id='%s' activity timestamp updated epoch=%ld",
                     handle->transaction_id,
                     (long)handle->last_activity_epoch);
}

/* ================================================================== */
/*  tx_begin                                                            */
/* ================================================================== */
int tx_begin(tx_handle_t  *handle,
             const char   *session_id,
             const char   *tx_name,
             char        **result_xml)
{
    if (!handle || !handle->ctx)
    {
        return TX_ERR_INVALID_ARG;
    }

    oci_context_t *ctx = handle->ctx;

    logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                 "Entering tx_begin");

    /* Guard: reject if a transaction is already active               */
    if (handle->status == TX_STATUS_ACTIVE)
    {
        logger_write(ctx->transaction_logger, LOG_ERROR, __func__, 0,
                     "tx_begin called but transaction '%s' is already ACTIVE",
                     handle->transaction_id);

        if (result_xml)
            *result_xml = NULL;
        return TX_ERR_ALREADY_ACTIVE;
    }

    /* ---- Assign transaction UUID ---- */
    tx_generate_uuid(handle->transaction_id);

    /* ---- Session ID ---- */
    if (session_id && session_id[0] != '\0')
        strncpy(handle->session_id, session_id, TX_UUID_LEN - 1);
    else
        strncpy(handle->session_id, "-", TX_UUID_LEN - 1);
    handle->session_id[TX_UUID_LEN - 1] = '\0';

    /* ---- Transaction name (business label) ---- */
    if (tx_name && tx_name[0] != '\0')
        strncpy(handle->tx_name, tx_name, sizeof(handle->tx_name) - 1);
    else
        strncpy(handle->tx_name, "-", sizeof(handle->tx_name) - 1);
    handle->tx_name[sizeof(handle->tx_name) - 1] = '\0';

    /* ---- Timing ---- */
    handle->start_time_us       = tx_now_us();
    handle->end_time_us         = 0;
    handle->last_activity_epoch = time(NULL);

    /* ---- Read config from ini (safe defaults if ini unavailable) ---- */
    if (ctx->ini)
    {
        handle->timeout_seconds = ctx->ini->tx_timeout_seconds;
        handle->max_retries     = ctx->ini->tx_max_retries;
        handle->retry_delay_ms  = ctx->ini->tx_retry_delay_ms;
    }
    else
    {
        handle->timeout_seconds = 300;
        handle->max_retries     = 3;
        handle->retry_delay_ms  = 500;
    }

    /* ---- Set status ACTIVE ---- */
    handle->status = TX_STATUS_ACTIVE;

    /* ---- Register as active transaction on the context ---- */
    ctx->active_tx = handle;

    /* Trace context (2026-08-06): set for the calling thread now that
     * the transaction is genuinely live - every subsequent
     * logger_write() call on this thread, across every module it
     * touches, picks it up automatically until tx_commit()/
     * tx_rollback()/tx_abort() clears it below. */
    logger_set_txid(handle->transaction_id);

    /* ---- Metrics: write a BEGIN record ---- */
    {
        metrics_record_t m;
        metrics_init(&m);
        metrics_set_context(&m, ctx);
        m.start_time_us = handle->start_time_us;
        m.end_time_us   = handle->start_time_us;   /* point-in-time event */
        strncpy(m.operation,         "TX_BEGIN",
                sizeof(m.operation)         - 1);
        strncpy(m.object_name,       handle->tx_name,
                sizeof(m.object_name)       - 1);
        strncpy(m.transaction_id,    handle->transaction_id,
                sizeof(m.transaction_id)    - 1);
        strncpy(m.transaction_name,  handle->tx_name,
                sizeof(m.transaction_name)  - 1);
        strncpy(m.error_code, "-", sizeof(m.error_code) - 1);
        strncpy(m.error_text, "-", sizeof(m.error_text) - 1);
        m.status_code = 0;
        metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &m);
    }

    /* ---- Log ---- */
    int do_log = ctx->ini ? ctx->ini->tx_log_begin : 1;
    if (do_log)
    {
        char ts_buf[32];
        tx_timestamp_str(handle->start_time_us, ts_buf, sizeof(ts_buf));

        logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                     "TRANSACTION BEGIN  tx_id='%s'  tx_name='%s'  "
                     "session_id='%s'  start=%s  timeout=%ds  max_retries=%d",
                     handle->transaction_id,
                     handle->tx_name,
                     handle->session_id,
                     ts_buf,
                     handle->timeout_seconds,
                     handle->max_retries);
    }

    /* ---- Build result XML ---- */
    if (result_xml)
        *result_xml = tx_build_begin_xml(handle);

    logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                 "tx_begin OK tx_id='%s'", handle->transaction_id);

    return TX_OK;
}

/* ================================================================== */
/*  tx_commit                                                           */
/* ================================================================== */

/*
 * ORA error codes that mean "there is nothing left to commit".
 * This happens in the test runner because each execute module
 * (execute_insert_batch, execute_update_batch, etc.) calls
 * OCITransCommit itself before returning.  By the time the outer
 * tx_commit() runs, Oracle has already ended the transaction and
 * there is no work outstanding.  These are treated as success
 * rather than an abort so the tx_handle reflects COMMITTED.
 *
 *   ORA-00000  "normal, successful completion"  (nothing to commit)
 *   ORA-01456  "may not perform insert/delete/update operation inside
 *               a READ ONLY transaction"  (autocommit edge case)
 *   ORA-25402  "transaction must roll back"  (connection-level error)
 *   ORA-25408  "can not safely replay call"  (TAF replay edge case)
 *
 * In practice on a local non-XA connection with no outstanding work,
 * OCITransCommit returns OCI_SUCCESS immediately (it is a no-op).
 * If it ever does return an error here, logging the OCI error code
 * first makes the root cause immediately visible in the log.
 */
#define ORA_NOTHING_TO_COMMIT  0      /* OCI_SUCCESS - already clean  */

int tx_commit(tx_handle_t  *handle,
              char        **result_xml)
{
    if (!handle || !handle->ctx)
        return TX_ERR_INVALID_ARG;

    oci_context_t *ctx = handle->ctx;

    logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                 "Entering tx_commit tx_id='%s'",
                 handle->transaction_id);

    if (handle->status != TX_STATUS_ACTIVE)
    {
        logger_write(ctx->transaction_logger, LOG_ERROR, __func__, 0,
                     "tx_commit called but status is %s (expected ACTIVE)",
                     tx_status_str(handle->status));
        if (result_xml) *result_xml = NULL;
        return TX_ERR_NO_ACTIVE;
    }

    int      rc          = TX_OK;
    int      attempt     = 0;
    sword    oci_status  = OCI_ERROR;
    sb4      ora_code    = 0;          /* ORA-XXXXX from OCIErrorGet  */

    /* ---- Retry loop for transient OCI errors ---- */
    while (attempt <= handle->max_retries)
    {
        logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                     "OCITransCommit attempt %d/%d tx_id='%s'",
                     attempt + 1, handle->max_retries + 1,
                     handle->transaction_id);

        oci_status = OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT);

        if (oci_status == OCI_SUCCESS ||
            oci_status == OCI_SUCCESS_WITH_INFO)
        {
            break;   /* success                                        */
        }

        /* ---- Extract OCI error code BEFORE deciding what to do ---- */
        /*
         * Always log the OCI error immediately on every failure so the
         * root cause appears in the transaction log even if we later
         * decide to treat it as a soft success.  The previous version
         * only logged during the retry wait, which meant the final
         * failure had no Oracle error detail in the log at all.
         */
        text errbuf[512] = {0};
        ora_code = 0;
        OCIErrorGet(ctx->errhp, 1, NULL, &ora_code,
                    errbuf, sizeof(errbuf), OCI_HTYPE_ERROR);

        logger_write(ctx->transaction_logger, LOG_WARN, __func__, 0,
                     "OCITransCommit attempt %d returned non-success  "
                     "oci_status=%d  ORA-%05d: %s  tx_id='%s'",
                     attempt + 1, (int)oci_status, (int)ora_code,
                     (char *)errbuf,
                     handle->transaction_id);

        /*
         * If the individual execute modules have already committed the
         * work (which is the current behaviour - each module calls
         * OCITransCommit internally), Oracle may return an error here
         * because there is genuinely no open transaction to commit.
         * On a local non-XA connection this usually comes back as
         * OCI_SUCCESS (no-op), but if for any reason an error is
         * returned and the ORA code indicates "nothing outstanding",
         * treat it as COMMITTED rather than ABORTED.
         *
         * Codes handled as soft success:
         *   ORA-00000  normal successful completion
         *   ORA-01085  preceding call did not execute  (already clean)
         */
        if (ora_code == 0 || ora_code == 1085)
        {
            logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                         "ORA-%05d treated as soft success "
                         "(no outstanding work to commit) tx_id='%s'",
                         (int)ora_code, handle->transaction_id);
            oci_status = OCI_SUCCESS;   /* rewrite so success path runs */
            break;
        }

        if (attempt >= handle->max_retries)
            break;   /* exhausted retries - fall through to abort      */

        attempt++;

        /* Delay before retry */
        if (handle->retry_delay_ms > 0)
        {
            struct timespec delay;
            delay.tv_sec  = handle->retry_delay_ms / 1000;
            delay.tv_nsec = (long)(handle->retry_delay_ms % 1000) * 1000000L;
            nanosleep(&delay, NULL);
        }
    }

    handle->end_time_us = tx_now_us();
    uint64_t duration   = handle->end_time_us - handle->start_time_us;

    /* ---- Always clear the active_tx pointer on resolution ---- */
    ctx->active_tx = NULL;

    /* Trace context (2026-08-06): clear unconditionally here too - this
     * one spot covers both the commit-succeeded and the commit-failed-
     * so-rolled-back-and-ABORTED branches immediately below, since both
     * mean the transaction is no longer active on this thread. */
    logger_clear_txid();

    if (oci_status == OCI_SUCCESS || oci_status == OCI_SUCCESS_WITH_INFO)
    {
        handle->status = TX_STATUS_COMMITTED;
        rc             = TX_OK;

        int do_log = ctx->ini ? ctx->ini->tx_log_commit : 1;
        if (do_log)
            logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                         "TRANSACTION COMMITTED  tx_id='%s'  "
                         "session_id='%s'  duration_us=%llu  attempts=%d",
                         handle->transaction_id,
                         handle->session_id,
                         (unsigned long long)duration,
                         attempt + 1);
    }
    else
    {
        logger_write(ctx->transaction_logger, LOG_ERROR, __func__, 0,
                     "OCITransCommit FAILED after %d attempt(s) - "
                     "rolling back and setting ABORTED  "
                     "ORA-%05d  tx_id='%s'",
                     attempt + 1, (int)ora_code,
                     handle->transaction_id);

        /* Best-effort rollback */
        OCITransRollback(ctx->svchp, ctx->errhp, OCI_DEFAULT);
        handle->status = TX_STATUS_ABORTED;
        rc             = TX_ERR_OCI_FAILURE;
    }

    /* ---- Metrics: write a COMMIT/ABORTED record ---- */
    {
        metrics_record_t m;
        metrics_init(&m);
        metrics_set_context(&m, ctx);
        m.start_time_us   = handle->start_time_us;
        m.end_time_us     = handle->end_time_us;
        m.execution_us    = duration;
        m.status_code     = rc;
        strncpy(m.operation,        (rc == TX_OK) ? "TX_COMMIT" : "TX_ABORT",
                sizeof(m.operation)        - 1);
        strncpy(m.object_name,      handle->tx_name,
                sizeof(m.object_name)      - 1);
        strncpy(m.transaction_id,   handle->transaction_id,
                sizeof(m.transaction_id)   - 1);
        strncpy(m.transaction_name, handle->tx_name,
                sizeof(m.transaction_name) - 1);
        strncpy(m.error_code, "-", sizeof(m.error_code) - 1);
        strncpy(m.error_text, "-", sizeof(m.error_text) - 1);
        metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &m);
    }

    /* ---- Build result XML ---- */
    if (result_xml)
    {
        char dur_str[32];
        snprintf(dur_str, sizeof(dur_str), "%llu",
                 (unsigned long long)duration);

        char elapsed_str[32];
        snprintf(elapsed_str, sizeof(elapsed_str), "%.6f",
                 (double)duration / 1e6);

        /* Pack duration into extra elements for the XML builder     */
        char extra[128];
        snprintf(extra, sizeof(extra),
                 "<duration_us>%s</duration_us>\n"
                 "<execution_time>%s</execution_time>",
                 dur_str, elapsed_str);

        *result_xml = tx_build_result_xml(handle, "COMMIT", NULL, extra);
    }

    return rc;
}

/* ================================================================== */
/*  tx_rollback                                                         */
/* ================================================================== */
int tx_rollback(tx_handle_t  *handle,
                char        **result_xml)
{
    if (!handle || !handle->ctx)
        return TX_ERR_INVALID_ARG;

    oci_context_t *ctx = handle->ctx;

    logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                 "Entering tx_rollback tx_id='%s'",
                 handle->transaction_id);

    if (handle->status != TX_STATUS_ACTIVE)
    {
        logger_write(ctx->transaction_logger, LOG_ERROR, __func__, 0,
                     "tx_rollback called but status is %s (expected ACTIVE)",
                     tx_status_str(handle->status));
        if (result_xml) *result_xml = NULL;
        return TX_ERR_NO_ACTIVE;
    }

    sword oci_status = OCITransRollback(ctx->svchp, ctx->errhp, OCI_DEFAULT);

    handle->end_time_us = tx_now_us();
    uint64_t duration   = handle->end_time_us - handle->start_time_us;

    /* ---- Clear active transaction on the context ---- */
    ctx->active_tx = NULL;

    /* Trace context (2026-08-06) */
    logger_clear_txid();

    if (oci_status != OCI_SUCCESS && oci_status != OCI_SUCCESS_WITH_INFO)
    {
        tx_log_oci_error(ctx, __func__);
        logger_write(ctx->transaction_logger, LOG_WARN, __func__, 0,
                     "OCITransRollback returned non-success status=%d "
                     "(proceeding with ROLLED_BACK state) tx_id='%s'",
                     oci_status, handle->transaction_id);
    }

    handle->status = TX_STATUS_ROLLED_BACK;

    int do_log = ctx->ini ? ctx->ini->tx_log_rollback : 1;
    if (do_log)
        logger_write(ctx->transaction_logger, LOG_INFO, __func__, 0,
                     "TRANSACTION ROLLED_BACK  tx_id='%s'  session_id='%s'  "
                     "duration_us=%llu",
                     handle->transaction_id,
                     handle->session_id,
                     (unsigned long long)duration);

    /* ---- Metrics: write a ROLLBACK record ---- */
    {
        metrics_record_t m;
        metrics_init(&m);
        metrics_set_context(&m, ctx);
        m.start_time_us   = handle->start_time_us;
        m.end_time_us     = handle->end_time_us;
        m.execution_us    = duration;
        m.status_code     = 0;
        strncpy(m.operation,        "TX_ROLLBACK",
                sizeof(m.operation)        - 1);
        strncpy(m.object_name,      handle->tx_name,
                sizeof(m.object_name)      - 1);
        strncpy(m.transaction_id,   handle->transaction_id,
                sizeof(m.transaction_id)   - 1);
        strncpy(m.transaction_name, handle->tx_name,
                sizeof(m.transaction_name) - 1);
        strncpy(m.error_code, "-", sizeof(m.error_code) - 1);
        strncpy(m.error_text, "-", sizeof(m.error_text) - 1);
        metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &m);
    }

    if (result_xml)
    {
        char extra[128];
        snprintf(extra, sizeof(extra),
                 "<duration_us>%llu</duration_us>\n"
                 "<execution_time>%.6f</execution_time>",
                 (unsigned long long)duration,
                 (double)duration / 1e6);

        *result_xml = tx_build_result_xml(handle, "ROLLBACK", NULL, extra);
    }

    return TX_OK;
}

/* ================================================================== */
/*  tx_abort                                                            */
/* ================================================================== */
int tx_abort(tx_handle_t  *handle,
             const char   *reason,
             char        **result_xml)
{
    if (!handle || !handle->ctx)
        return TX_ERR_INVALID_ARG;

    oci_context_t *ctx = handle->ctx;
    const char    *abort_reason = (reason && reason[0]) ? reason : "unspecified";

    logger_write(ctx->transaction_logger, LOG_ERROR, __func__, 0,
                 "Entering tx_abort tx_id='%s' reason='%s'",
                 handle->transaction_id, abort_reason);

    /* Safe to call regardless of current status - best-effort rollback */
    if (handle->status == TX_STATUS_ACTIVE)
    {
        sword oci_status = OCITransRollback(ctx->svchp, ctx->errhp,
                                             OCI_DEFAULT);
        if (oci_status != OCI_SUCCESS && oci_status != OCI_SUCCESS_WITH_INFO)
        {
            tx_log_oci_error(ctx, __func__);
            logger_write(ctx->transaction_logger, LOG_WARN, __func__, 0,
                         "OCITransRollback in tx_abort returned non-success "
                         "(continuing with ABORTED status) tx_id='%s'",
                         handle->transaction_id);
        }
    }

    handle->end_time_us = tx_now_us();
    uint64_t duration   = handle->end_time_us - handle->start_time_us;
    handle->status      = TX_STATUS_ABORTED;

    /* ---- Clear active transaction on the context ---- */
    ctx->active_tx = NULL;

    /* Trace context (2026-08-06) */
    logger_clear_txid();

    logger_write(ctx->transaction_logger, LOG_ERROR, __func__, 0,
                 "TRANSACTION ABORTED  tx_id='%s'  session_id='%s'  "
                 "reason='%s'  duration_us=%llu",
                 handle->transaction_id,
                 handle->session_id,
                 abort_reason,
                 (unsigned long long)duration);

    if (result_xml)
    {
        char extra[256];
        snprintf(extra, sizeof(extra),
                 "<duration_us>%llu</duration_us>\n"
                 "<execution_time>%.6f</execution_time>\n"
                 "<abort_reason>%s</abort_reason>",
                 (unsigned long long)duration,
                 (double)duration / 1e6,
                 abort_reason);

        *result_xml = tx_build_result_xml(handle, "ABORT", NULL, extra);
    }

    return TX_OK;
}

/* ================================================================== */
/*  tx_check_timeout                                                    */
/* ================================================================== */
int tx_check_timeout(tx_handle_t  *handle,
                     char        **result_xml)
{
    if (!handle)
        return TX_OK;

    if (handle->status != TX_STATUS_ACTIVE)
        return TX_OK;

    if (handle->timeout_seconds <= 0)
        return TX_OK;   /* timeout disabled                           */

    time_t  now     = time(NULL);
    long    idle    = (long)(now - handle->last_activity_epoch);

    if (idle < (long)handle->timeout_seconds)
        return TX_OK;   /* not yet timed out                          */

    /* ---- Timeout triggered ---- */
    oci_context_t *ctx = handle->ctx;

    int do_log = ctx && ctx->ini ? ctx->ini->tx_log_timeout : 1;
    if (do_log && ctx)
        logger_write(ctx->transaction_logger, LOG_WARN, __func__, 0,
                     "TRANSACTION TIMEOUT  tx_id='%s'  session_id='%s'  "
                     "idle=%lds  timeout=%ds",
                     handle->transaction_id,
                     handle->session_id,
                     idle,
                     handle->timeout_seconds);

    /* Abort internally */
    char *abort_xml = NULL;
    tx_abort(handle, "idle_timeout", &abort_xml);
    handle->status = TX_STATUS_TIMED_OUT;

    if (result_xml)
    {
        /* Return the abort XML or build a timeout-specific one       */
        if (abort_xml)
        {
            *result_xml = abort_xml;
        }
        else
        {
            char extra[128];
            snprintf(extra, sizeof(extra),
                     "<idle_seconds>%ld</idle_seconds>\n"
                     "<timeout_seconds>%d</timeout_seconds>",
                     idle, handle->timeout_seconds);
            *result_xml = tx_build_result_xml(handle, "TIMEOUT", NULL, extra);
        }
    }
    else
    {
        if (abort_xml) free(abort_xml);
    }

    return TX_ERR_TIMEOUT;
}

/* ================================================================== */
/*  Internal: tx_log_oci_error                                          */
/*  Extract and log the OCI error code and message.                    */
/* ================================================================== */
static void tx_log_oci_error(oci_context_t *ctx, const char *func_name)
{
    if (!ctx || !ctx->errhp || !ctx->transaction_logger)
        return;

    text errbuf[512];
    sb4  errcode = 0;

    OCIErrorGet(ctx->errhp, 1, NULL, &errcode,
                errbuf, sizeof(errbuf), OCI_HTYPE_ERROR);

    logger_write(ctx->transaction_logger, LOG_ERROR, func_name, 0,
                 "OCI Error %d: %s", (int)errcode, (char *)errbuf);
}

/* ================================================================== */
/*  Internal: tx_build_begin_xml                                        */
/*  Heap-allocate and return the BEGIN response XML.                   */
/* ================================================================== */
static char *tx_build_begin_xml(const tx_handle_t *handle)
{
    char ts_buf[32];
    tx_timestamp_str(handle->start_time_us, ts_buf, sizeof(ts_buf));

    /* Generous buffer: field values are bounded, total is well under 1KB */
    size_t  sz  = 1024;
    char   *buf = malloc(sz);
    if (!buf) return NULL;

    snprintf(buf, sz,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<transaction>\n"
             "  <operation>BEGIN</operation>\n"
             "  <transaction_id>%s</transaction_id>\n"
             "  <transaction_name>%s</transaction_name>\n"
             "  <session_id>%s</session_id>\n"
             "  <status>%s</status>\n"
             "  <start_time>%s</start_time>\n"
             "  <timeout_seconds>%d</timeout_seconds>\n"
             "  <max_retries>%d</max_retries>\n"
             "</transaction>\n",
             handle->transaction_id,
             handle->tx_name,
             handle->session_id,
             tx_status_str(handle->status),
             ts_buf,
             handle->timeout_seconds,
             handle->max_retries);

    return buf;
}

/* ================================================================== */
/*  Internal: tx_build_result_xml                                       */
/*  Generic result XML builder for COMMIT / ROLLBACK / ABORT / TIMEOUT */
/*  extra_value: raw XML fragment injected between <status> and </transaction>
/*  Pass NULL to omit.                                                 */
/* ================================================================== */
static char *tx_build_result_xml(const tx_handle_t *handle,
                                  const char        *operation,
                                  const char        *extra_element,
                                  const char        *extra_value)
{
    (void)extra_element;   /* not used in this simplified builder      */

    size_t  sz  = 2048;
    char   *buf = malloc(sz);
    if (!buf) return NULL;

    int written = snprintf(buf, sz,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<transaction>\n"
             "  <operation>%s</operation>\n"
             "  <transaction_id>%s</transaction_id>\n"
             "  <session_id>%s</session_id>\n"
             "  <status>%s</status>\n"
             "%s"
             "</transaction>\n",
             operation    ? operation    : "-",
             handle->transaction_id,
             handle->session_id,
             tx_status_str(handle->status),
             extra_value  ? extra_value  : "");

    if (written < 0 || (size_t)written >= sz)
    {
        /* Truncation guard: reallocate and retry once                */
        free(buf);
        sz  = (size_t)written + 64;
        buf = malloc(sz);
        if (!buf) return NULL;

        snprintf(buf, sz,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<transaction>\n"
                 "  <operation>%s</operation>\n"
                 "  <transaction_id>%s</transaction_id>\n"
                 "  <session_id>%s</session_id>\n"
                 "  <status>%s</status>\n"
                 "%s"
                 "</transaction>\n",
                 operation    ? operation    : "-",
                 handle->transaction_id,
                 handle->session_id,
                 tx_status_str(handle->status),
                 extra_value  ? extra_value  : "");
    }

    return buf;
}
