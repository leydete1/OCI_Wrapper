/* ======================================================================
 * metrics_writer.c
 *
 * See metrics_writer.h for the full design rationale.
 * ====================================================================== */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "metrics_writer.h"
#include "generic_queue.h"
#include "ctx_utils.h"
#include "logger.h"
#include "db_driver.h"            /* dml_execute(), dialect - item 3b */
#include "Transaction_Manager.h"  /* tx_commit_with_retry() - item 3b */

struct metrics_writer {
    generic_queue_t *file_queue;   /* NULL if metrics_file_enabled=0 */
    generic_queue_t *db_queue;     /* NULL if metrics_db_enabled=0   */
    pthread_t         file_thread;
    pthread_t         db_thread;
    int               file_thread_started;
    int               db_thread_started;
};

/* Fixed queue depth for both destinations - no config key for this
 * yet, matching Stage 2's own "keep it simple" precedent already set
 * by session_touch_queue's original SESSION_TOUCH_QUEUE_DEPTH.        */
#define METRICS_QUEUE_DEPTH 2000

/* ================================================================== */
/*  Deep copy / deep free                                               */
/*                                                                      */
/*  A plain shallow struct copy (metrics_record_t b = *a;) would work   */
/*  fine for every fixed-size char[]/numeric field - those are value    */
/*  types, genuinely independent after a struct copy. It would NOT be   */
/*  safe for the three heap-string pointer fields (input_file_name/     */
/*  input_request/output_response) - both copies would end up pointing */
/*  at the exact same heap allocation, and metrics_write() frees these  */
/*  fields after writing. Handing two independent destination threads   */
/*  a shallow copy each would mean both eventually calling free() on    */
/*  the same pointer - a genuine double-free, not a hypothetical one.   */
/*  Each deep copy gets its own independently-strdup()'d strings        */
/*  instead, so each destination thread's own free() is safe no matter  */
/*  what order the two threads finish in.                               */
/* ================================================================== */
static metrics_record_t *metrics_record_deep_copy(const metrics_record_t *src)
{
    metrics_record_t *copy = malloc(sizeof(metrics_record_t));
    if (!copy) return NULL;

    *copy = *src;   /* shallow copy - correct for every field except the
                        three below, which get overwritten next        */

    copy->input_file_name = src->input_file_name ? strdup(src->input_file_name) : NULL;
    copy->input_request   = src->input_request   ? strdup(src->input_request)   : NULL;
    copy->output_response = src->output_response ? strdup(src->output_response) : NULL;

    return copy;
}

static void metrics_record_deep_free(void *item)
{
    metrics_record_t *m = (metrics_record_t *)item;
    if (!m) return;

    free(m->input_file_name);
    free(m->input_request);
    free(m->output_response);
    free(m);
}

/* ================================================================== */
/*  File writer thread                                                  */
/*                                                                      */
/*  Deliberately simple - one row per record, no batching. File I/O is  */
/*  cheap, local, no round-trip cost worth batching for (see            */
/*  metrics_writer.h's own doc comment on why DB batches and file       */
/*  doesn't).                                                           */
/* ================================================================== */
typedef struct {
    generic_queue_t *q;
    logger_t         *file_logger;     /* CSV data ONLY - metrics_write() */
    logger_t         *writer_logger;   /* this thread's own operational
                                           messages - never the CSV file */
} file_writer_args_t;

static void *file_writer_thread_main(void *arg)
{
    file_writer_args_t *args = (file_writer_args_t *)arg;
    generic_queue_t *q = args->q;
    logger_t *file_logger   = args->file_logger;
    logger_t *writer_logger = args->writer_logger;
    free(arg);

    logger_write(writer_logger, LOG_INFO, __func__, 0,
                 "Metrics file writer thread started");

    int written = 0;
    void *item;

    while ((item = generic_queue_dequeue_blocking(q)) != NULL)
    {
        metrics_record_t *m = (metrics_record_t *)item;
        metrics_write(file_logger, m);   /* existing, unchanged function -
                                             also frees m's own three
                                             heap-string fields at the end,
                                             per its own existing contract */
        free(m);
        written++;
    }

    logger_write(writer_logger, LOG_INFO, __func__, 0,
                 "Metrics file writer thread exiting after %d record(s) "
                 "written", written);

    return NULL;
}

/* ================================================================== */
/*  DB writer thread                                                    */
/*                                                                      */
/*  Batches: flushes whichever comes first - metrics_per_write records  */
/*  accumulated, or metrics_max_insert_delay_ms elapsed since the       */
/*  FIRST record in the current batch arrived (not reset per record -   */
/*  a trickle of records arriving just under the wire, one at a time,   */
/*  must not be able to starve the flush indefinitely - the deadline is */
/*  computed once, when the batch's first record arrives, and shrinks   */
/*  from there).                                                        */
/* ================================================================== */
typedef struct {
    oci_context_t   *metrics_base_ctx;   /* the metrics DB's OWN pool -
                                             see metrics_writer.h's note
                                             on metrics_writer_start()   */
    generic_queue_t *q;
    int               per_write;
    int               max_delay_ms;
    logger_t         *writer_logger;   /* this thread's own operational
                                           messages - never the CSV file */
} db_writer_args_t;

/*
 * metrics_db_bulk_insert()
 *
 * Stage 3 (closure item 5, 2026-08-09) - the real insert, replacing
 * the Stage 2 stub now that OCI_METRICS exists (Create_Metrics_Table.txt).
 *
 * Deliberately simple, per Terry's own direction (2026-08-09): this is
 * internal, trusted data - metrics_record_t's own fields are already
 * guaranteed correct by the struct itself, not raw external input that
 * needs validating. Genuinely NOT execute_insert_batch() - that would
 * unavoidably trigger a full audit_trail_insert() per field for every
 * metrics row, and would run Level 1/2 validation logic that doesn't
 * apply here at all.
 *
 * Oracle dialect extraction, item 3b (2026-10-04): no vendor calls left
 * in this file.
 *   - The INSERT text is built once, through the driver's dialect:
 *     bind_placeholder() for every position and value_expr("TIMESTAMP")
 *     around :19/:20. For Oracle the result is byte-identical to the
 *     old hand-written METRICS_INSERT_SQL (proven by the 3b unit test).
 *   - Each row runs through driver->dml_execute() - one call per row,
 *     same as the old prepare-once/execute-per-row loop. Every value
 *     is bound as a string (dml_execute()'s contract); numbers are
 *     formatted with %d/%lld and Oracle converts them, so the stored
 *     values are unchanged.
 *   - The one commit per batch goes through tx_commit_with_retry(),
 *     like every other commit. Before 3b it was a bare OCITransCommit
 *     whose result was never checked, so a failed commit silently lost
 *     the batch while the log still said "Inserted N". Now a failed
 *     commit is logged, rolled back, and reported as 0 persisted.
 *
 * NULL/unset string fields fall back to "-", matching the exact same
 * placeholder convention already used throughout this project (CSV
 * output, dispatcher error envelopes) - no new convention introduced.
 */
#define METRICS_BIND_COUNT   40
#define METRICS_POS_START_TS 19    /* START_TIME_TS - TIMESTAMP column */
#define METRICS_POS_END_TS   20    /* END_TIME_TS   - TIMESTAMP column */

#define METRICS_INSERT_HEAD \
    "INSERT INTO OCI_METRICS (" \
    "CONSUMER_NAME, SESSION_ID, TRANSACTION_ID, TRANSACTION_NAME, AUDIT_ID, " \
    "CLIENT_IP, HOST_NAME, SERVER_IP, SERVER_PORT, PROCESS_ID, THREAD_ID, " \
    "DATASOURCE_NAME, CONNECTION_ID, POOL_ID, OPERATION, OBJECT_NAME, " \
    "SQL_HASH, CACHE_KEY_HASH, START_TIME_TS, END_TIME_TS, CACHE_LOOKUP_US, " \
    "LEVEL1_PARSE_US, LEVEL2_PARSE_US, SQL_PARSE_US, EXECUTION_US, TOTAL_US, " \
    "ROWS_AFFECTED, OUTPUT_XML_BYTES, CLOB_BYTES, LOB_BYTES, BYTES_PROCESSED, " \
    "CACHE_HIT, STATUS_CODE, ERROR_CODE, ERROR_TEXT, CONNECTION_WAIT_US, " \
    "CONNECTION_CREATE_US, CONNECTION_ACQUIRE_US, LDAP_BIND_US, CRYPT_VERIFY_US" \
    ") VALUES ("

/* Built once, on the DB writer thread's first batch - only that one
 * thread ever touches it (same lifetime the old prepared statement had). */
static char g_metrics_insert_sql[2048] = {0};

static int build_metrics_insert_sql(oci_context_t *ctx)
{
    if (g_metrics_insert_sql[0]) return 0;

    const db_driver_t  *driver = db_driver_get(ctx);
    const db_dialect_t *dl     = driver ? driver->dialect : NULL;
    if (!dl || !dl->bind_placeholder || !dl->value_expr) return -1;

    char   buf[sizeof(g_metrics_insert_sql)];
    size_t used = (size_t)snprintf(buf, sizeof(buf), "%s", METRICS_INSERT_HEAD);
    if (used >= sizeof(buf)) return -1;

    for (int pos = 1; pos <= METRICS_BIND_COUNT; pos++)
    {
        char ph[32], expr[128];
        if (dl->bind_placeholder(pos, ph, sizeof(ph)) != 0) return -1;

        const char *piece = ph;
        if (pos == METRICS_POS_START_TS || pos == METRICS_POS_END_TS)
        {
            if (dl->value_expr("TIMESTAMP", ph, expr, sizeof(expr)) != 0) return -1;
            piece = expr;
        }

        int n = snprintf(buf + used, sizeof(buf) - used, "%s%s",
                         pos > 1 ? ", " : "", piece);
        if (n < 0 || (size_t)n >= sizeof(buf) - used) return -1;
        used += (size_t)n;
    }

    int n = snprintf(buf + used, sizeof(buf) - used, ")");
    if (n < 0 || (size_t)n >= sizeof(buf) - used) return -1;

    memcpy(g_metrics_insert_sql, buf, used + 2);
    return 0;
}

/* Exposed for the 3b unit test only (not in metrics_writer.h). */
const char *metrics_insert_sql_for_test(oci_context_t *ctx)
{
    return build_metrics_insert_sql(ctx) == 0 ? g_metrics_insert_sql : NULL;
}

static void metrics_db_bulk_insert(oci_context_t *ctx,
                                    metrics_record_t **batch,
                                    int batch_count)
{
    if (batch_count <= 0) { return; }

    const db_driver_t *driver = db_driver_get(ctx);

    if (!driver || !driver->dml_execute || !driver->rollback ||
        build_metrics_insert_sql(ctx) != 0)
    {
        logger_write(ctx->metrics_writer_logger, LOG_ERROR, __func__, 0,
                     "Could not build the metrics insert (driver or "
                     "dialect unavailable) - dropping this batch of %d "
                     "record(s)", batch_count);
        for (int i = 0; i < batch_count; i++) metrics_record_deep_free(batch[i]);
        return;
    }

    int inserted = 0;

    for (int i = 0; i < batch_count; i++)
    {
        metrics_record_t *m = batch[i];

        /* "-" fallback for anything unset, matching the same
         * placeholder convention already used everywhere else in this
         * project - no new convention, no extra validation, per
         * Terry's own "keep it simple, trust the data" direction.      */
        const char *consumer_name    = m->consumer_name[0]    ? m->consumer_name    : "-";
        const char *session_id       = m->session_id[0]       ? m->session_id       : "-";
        const char *transaction_id   = m->transaction_id[0]   ? m->transaction_id   : "-";
        const char *transaction_name = m->transaction_name[0] ? m->transaction_name : "-";
        const char *audit_id         = m->audit_id[0]         ? m->audit_id         : "-";
        const char *client_ip        = m->client_ip[0]        ? m->client_ip        : "-";
        const char *host_name        = m->host_name[0]        ? m->host_name        : "-";
        const char *server_ip        = m->server_ip[0]        ? m->server_ip        : "-";
        const char *datasource_name  = m->datasource_name[0]  ? m->datasource_name  : "-";
        const char *operation        = m->operation[0]        ? m->operation        : "-";
        const char *object_name      = m->object_name[0]      ? m->object_name      : "-";
        const char *error_code       = m->error_code[0]       ? m->error_code       : "-";
        const char *error_text       = m->error_text[0]       ? m->error_text       : "-";
        char start_time[48], end_time[48];
        metrics_format_timestamp_us(m->start_time_us, start_time, sizeof(start_time));
        metrics_format_timestamp_us(m->end_time_us,   end_time,   sizeof(end_time));

        /* Numbers as text - the same values the old code bound as
         * SQLT_INT (int and 8-byte long long), same casts.             */
        char num[METRICS_BIND_COUNT + 1][32];
#define NUM_I(pos, v)  snprintf(num[pos], sizeof(num[pos]), "%d",   (int)(v))
#define NUM_L(pos, v)  snprintf(num[pos], sizeof(num[pos]), "%lld", (long long)(v))
        NUM_I(9,  m->server_port);
        NUM_I(10, m->process_id);
        NUM_I(11, m->thread_id);
        NUM_I(13, m->connection_id);
        NUM_I(14, m->pool_id);
        NUM_L(17, m->sql_hash);
        NUM_L(18, m->cache_key_hash);
        NUM_L(21, m->cache_lookup_us);
        NUM_L(22, m->level1_parse_us);
        NUM_L(23, m->level2_parse_us);
        NUM_L(24, m->sql_parse_us);
        NUM_L(25, m->execution_us);
        NUM_L(26, m->total_us);
        NUM_L(27, m->rows_affected);
        NUM_L(28, m->output_xml_bytes);
        NUM_L(29, m->clob_bytes);
        NUM_L(30, m->lob_bytes);
        NUM_L(31, m->bytes_processed);
        NUM_I(32, m->cache_hit);
        NUM_I(33, m->status_code);
        NUM_L(36, m->connection_wait_us);
        NUM_L(37, m->connection_create_us);
        NUM_L(38, m->connection_acquire_us);
        NUM_L(39, m->ldap_bind_us);
        NUM_L(40, m->crypt_verify_us);
#undef NUM_I
#undef NUM_L

        const char *values[METRICS_BIND_COUNT] = {
            consumer_name, session_id, transaction_id, transaction_name,  /*  1- 4 */
            audit_id, client_ip, host_name, server_ip,                    /*  5- 8 */
            num[9], num[10], num[11], datasource_name,                    /*  9-12 */
            num[13], num[14], operation, object_name,                     /* 13-16 */
            num[17], num[18], start_time, end_time,                       /* 17-20 */
            num[21], num[22], num[23], num[24],                           /* 21-24 */
            num[25], num[26], num[27], num[28],                           /* 25-28 */
            num[29], num[30], num[31], num[32],                           /* 29-32 */
            num[33], error_code, error_text, num[36],                     /* 33-36 */
            num[37], num[38], num[39], num[40]                            /* 37-40 */
        };

        db_dml_request_t req;
        memset(&req, 0, sizeof(req));
        req.sql         = g_metrics_insert_sql;
        req.bind_count  = METRICS_BIND_COUNT;
        req.bind_values = values;

        int rows = 0;
        if (driver->dml_execute(ctx, ctx->metrics_writer_logger, &req,
                                &rows, NULL, 0) != 0)
        {
            logger_write(ctx->metrics_writer_logger, LOG_ERROR, __func__, 0,
                         "Insert failed for metrics row %d in this batch - "
                         "skipping this one row, continuing with the rest", i);
            continue;
        }

        inserted++;
    }

    /* One commit per batch, as before - now checked, and retried like
     * every other commit (item 3b). */
    if (tx_commit_with_retry(ctx, ctx->metrics_writer_logger,
                             ctx->ini ? ctx->ini->tx_max_retries    : 0,
                             ctx->ini ? ctx->ini->tx_retry_delay_ms : 0,
                             NULL) != 0)
    {
        logger_write(ctx->metrics_writer_logger, LOG_ERROR, __func__, 0,
                     "Commit failed - rolling back; 0 of %d metrics "
                     "record(s) persisted this batch (%d had been inserted)",
                     batch_count, inserted);
        driver->rollback(ctx, ctx->metrics_writer_logger);
    }
    else
    {
        logger_write(ctx->metrics_writer_logger, LOG_INFO, __func__, 0,
                     "Inserted %d of %d metrics record(s) this batch",
                     inserted, batch_count);
    }

    for (int i = 0; i < batch_count; i++)
        metrics_record_deep_free(batch[i]);
}

static void *db_writer_thread_main(void *arg)
{
    db_writer_args_t args = *(db_writer_args_t *)arg;
    free(arg);

    oci_context_t *metrics_base_ctx = args.metrics_base_ctx;
    generic_queue_t *q              = args.q;
    logger_t *writer_logger        = args.writer_logger;

    /* Same pattern as every other dedicated thread in this project -
     * borrow an independent pooled session at startup, hold it for
     * this thread's whole lifetime. Proven here in Stage 2 even though
     * the actual insert is stubbed until Stage 3 - so Stage 3 only
     * needs to add the SQL itself, not the connection plumbing too.
     * As of the 13 Aug 2026 closure item, metrics_base_ctx is the
     * metrics DB's own independent pool, not the business one - see
     * metrics_writer.h's own note on metrics_writer_start().          */
    oci_context_t thread_ctx;
    memset(&thread_ctx, 0, sizeof(thread_ctx));

    /* Stage 6 (2026-10-09): through the driver - same pool underneath. */
    const db_driver_t *pool_driver = db_driver_get(metrics_base_ctx);

    if (pool_driver->get_session(metrics_base_ctx, &thread_ctx) != 0)
    {
        logger_write(writer_logger, LOG_ERROR, __func__, 0,
                     "Metrics DB writer thread: get_session "
                     "failed - this thread cannot start, DB metrics "
                     "will never be persisted until the process "
                     "restarts (file metrics, if enabled, are "
                     "unaffected)");
        return NULL;
    }

    copy_shared_ctx_fields(&thread_ctx, metrics_base_ctx);
    thread_ctx.active_tx = NULL;

    /* Item 3b fix (2026-10-04): metrics_base_ctx is the metrics pool's
     * own context, which Bootstrap memsets and fills only with ini and
     * connectionpool_logger - so copy_shared_ctx_fields() left
     * thread_ctx.metrics_writer_logger NULL. Every line
     * metrics_db_bulk_insert() wrote ("Inserted N of M", row failures)
     * went to a NULL logger ("Logger is NULL. Going to Cleanup." on the
     * console) and never reached a log file - a pre-existing gap 3b
     * made visible, because dml_execute() and the commit helper log on
     * the same logger. The thread's own dedicated writer_logger
     * (metrics_writer_app.log) is the right destination.            */
    thread_ctx.metrics_writer_logger = writer_logger;

    logger_write(writer_logger, LOG_INFO, __func__, 0,
                 "Metrics DB writer thread started - session borrowed, "
                 "per_write=%d max_delay_ms=%d",
                 args.per_write, args.max_delay_ms);

    metrics_record_t **batch = malloc((size_t)args.per_write * sizeof(metrics_record_t *));
    if (!batch)
    {
        logger_write(writer_logger, LOG_ERROR, __func__, 0,
                     "Metrics DB writer thread: malloc failed for batch "
                     "array (per_write=%d) - this thread cannot start",
                     args.per_write);
        pool_driver->release_session(metrics_base_ctx, &thread_ctx);
        return NULL;
    }

    int total_flushed = 0;
    int total_batches = 0;

    for (;;)
    {
        /* Wait indefinitely for the first record of a new batch - no
         * reason to burn a timeout budget while the queue is genuinely
         * empty and nothing is pending to flush.                      */
        void *first = generic_queue_dequeue_blocking(q);
        if (!first) break;   /* shutdown, queue empty - nothing left at all */

        batch[0] = (metrics_record_t *)first;
        int batch_count = 1;

        struct timespec batch_start;
        clock_gettime(CLOCK_MONOTONIC, &batch_start);

        int shutting_down = 0;

        while (batch_count < args.per_write)
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec  - batch_start.tv_sec)  * 1000L +
                               (now.tv_nsec - batch_start.tv_nsec) / 1000000L;
            long remaining_ms = args.max_delay_ms - elapsed_ms;

            if (remaining_ms <= 0) break;   /* deadline reached - flush what we have */

            int timed_out = 0;
            void *item = generic_queue_dequeue_timed(q, (int)remaining_ms, &timed_out);

            if (item)
            {
                batch[batch_count++] = (metrics_record_t *)item;
            }
            else if (timed_out)
            {
                break;   /* deadline reached - flush what we have */
            }
            else
            {
                /* Shutdown signalled mid-batch - flush what's
                 * accumulated so far, then exit the outer loop too
                 * (the next generic_queue_dequeue_blocking() call
                 * would return NULL immediately anyway, since the
                 * queue is now shutting down and empty - this flag
                 * just avoids one redundant call).                    */
                shutting_down = 1;
                break;
            }
        }

        logger_write(writer_logger, LOG_INFO, __func__, 0,
                     "Metrics DB writer: flushing batch of %d record(s)",
                     batch_count);
        metrics_db_bulk_insert(&thread_ctx, batch, batch_count);
        total_flushed += batch_count;
        total_batches++;

        if (shutting_down) break;
    }

    free(batch);

    logger_write(writer_logger, LOG_INFO, __func__, 0,
                 "Metrics DB writer thread exiting after %d batch(es), "
                 "%d record(s) total - releasing session",
                 total_batches, total_flushed);

    pool_driver->release_session(metrics_base_ctx, &thread_ctx);

    return NULL;
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */
metrics_writer_t *metrics_writer_start(oci_context_t *metrics_base_ctx,
                                        app_config_t  *config,
                                        logger_t      *file_logger,
                                        logger_t      *writer_logger)
{
    metrics_writer_t *w = malloc(sizeof(metrics_writer_t));
    if (!w) return NULL;

    w->file_queue          = NULL;
    w->db_queue             = NULL;
    w->file_thread_started = 0;
    w->db_thread_started    = 0;

    if (config->metrics_file_enabled)
    {
        w->file_queue = generic_queue_create(METRICS_QUEUE_DEPTH, metrics_record_deep_free);
        if (!w->file_queue)
        {
            logger_write(writer_logger, LOG_ERROR, __func__, 0,
                         "metrics_writer_start: generic_queue_create "
                         "failed for file queue - file metrics disabled "
                         "for this run");
        }
        else
        {
            file_writer_args_t *args = malloc(sizeof(file_writer_args_t));
            if (args)
            {
                args->q             = w->file_queue;
                args->file_logger   = file_logger;
                args->writer_logger = writer_logger;
                if (pthread_create(&w->file_thread, NULL, file_writer_thread_main, args) == 0)
                    w->file_thread_started = 1;
                else
                {
                    logger_write(writer_logger, LOG_ERROR, __func__, 0,
                                 "metrics_writer_start: pthread_create "
                                 "failed for file writer thread");
                    free(args);
                    generic_queue_destroy(w->file_queue);
                    w->file_queue = NULL;
                }
            }
        }
    }

    if (config->metrics_db_enabled)
    {
        w->db_queue = generic_queue_create(METRICS_QUEUE_DEPTH, metrics_record_deep_free);
        if (!w->db_queue)
        {
            logger_write(writer_logger, LOG_ERROR, __func__, 0,
                         "metrics_writer_start: generic_queue_create "
                         "failed for DB queue - DB metrics disabled for "
                         "this run");
        }
        else
        {
            db_writer_args_t *args = malloc(sizeof(db_writer_args_t));
            if (args)
            {
                args->metrics_base_ctx = metrics_base_ctx;
                args->q             = w->db_queue;
                args->per_write     = config->metrics_per_write > 0 ? config->metrics_per_write : 100;
                args->max_delay_ms  = config->metrics_max_insert_delay_ms > 0 ? config->metrics_max_insert_delay_ms : 5000;
                args->writer_logger = writer_logger;
                if (pthread_create(&w->db_thread, NULL, db_writer_thread_main, args) == 0)
                    w->db_thread_started = 1;
                else
                {
                    logger_write(writer_logger, LOG_ERROR, __func__, 0,
                                 "metrics_writer_start: pthread_create "
                                 "failed for DB writer thread");
                    free(args);
                    generic_queue_destroy(w->db_queue);
                    w->db_queue = NULL;
                }
            }
        }
    }

    return w;
}

void metrics_finalise_and_enqueue(metrics_writer_t *writer,
                                   logger_t          *metrics_logger,
                                   metrics_record_t *m)
{
    if (!m) return;

    metrics_finalise(m);   /* existing, unchanged - cheap, synchronous,
                               no I/O - computes total_us/bytes_processed */

    if (!writer)
    {
        /* No writer running (e.g. metrics_writer_start() itself
         * failed) - still need to release m's own heap-string fields,
         * matching the exact ownership contract this function
         * documents (they transfer here regardless of outcome).       */
        free(m->input_file_name);
        free(m->input_request);
        free(m->output_response);
        return;
    }

    if (writer->file_queue)
    {
        metrics_record_t *copy = metrics_record_deep_copy(m);
        if (!copy || generic_queue_enqueue(writer->file_queue, copy) != 0)
        {
            if (copy) metrics_record_deep_free(copy);
            logger_write(metrics_logger, LOG_WARN, __func__, 0,
                         "metrics file queue full (or copy failed) - "
                         "dropped one metrics record");
        }
    }

    if (writer->db_queue)
    {
        metrics_record_t *copy = metrics_record_deep_copy(m);
        if (!copy || generic_queue_enqueue(writer->db_queue, copy) != 0)
        {
            if (copy) metrics_record_deep_free(copy);
            logger_write(metrics_logger, LOG_WARN, __func__, 0,
                         "metrics DB queue full (or copy failed) - "
                         "dropped one metrics record");
        }
    }

    /* The caller's own m is done with, per this function's own
     * ownership contract - free its heap-string fields (both
     * destinations, if enabled, already have their own independent
     * copies by this point).                                          */
    free(m->input_file_name);
    free(m->input_request);
    free(m->output_response);
}

void metrics_writer_stop_and_join(metrics_writer_t *writer)
{
    if (!writer) return;

    if (writer->file_thread_started)
    {
        generic_queue_shutdown(writer->file_queue);
        pthread_join(writer->file_thread, NULL);
    }
    if (writer->db_thread_started)
    {
        generic_queue_shutdown(writer->db_queue);
        pthread_join(writer->db_thread, NULL);
    }

    if (writer->file_queue) generic_queue_destroy(writer->file_queue);
    if (writer->db_queue)   generic_queue_destroy(writer->db_queue);

    free(writer);
}
