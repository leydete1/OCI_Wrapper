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
#include "OCI_Connection_Pool.h"
#include "ctx_utils.h"
#include "logger.h"

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
 * metrics row (confirmed by reading its own source - the audit call is
 * baked into execute_insert_batch() itself, not a separate step), and
 * would run Level 1/2 validation logic that doesn't apply here at all.
 *
 * Also deliberately NOT a true multi-row array-bind (one OCIStmtExecute
 * with iters=batch_count) - that needs the whole batch transposed into
 * 37 parallel C arrays plus indicator/length arrays per column, real
 * complexity for genuinely marginal gain at this volume. Instead: one
 * statement, prepared once and reused for the thread's whole lifetime,
 * a plain loop that rebinds fresh values and executes once per row,
 * one commit at the end of the whole batch - that one commit-per-batch
 * (instead of per-row) is the actual performance win anyway.
 *
 * NULL/unset string fields fall back to "-", matching the exact same
 * placeholder convention already used throughout this project (CSV
 * output, dispatcher error envelopes) - no new convention introduced.
 */
static OCIStmt *g_metrics_insert_stmt = NULL;   /* prepared once, reused
                                                    for this thread's
                                                    whole lifetime       */

#define METRICS_INSERT_SQL \
    "INSERT INTO OCI_METRICS (" \
    "CONSUMER_NAME, SESSION_ID, TRANSACTION_ID, TRANSACTION_NAME, AUDIT_ID, " \
    "CLIENT_IP, HOST_NAME, SERVER_IP, SERVER_PORT, PROCESS_ID, THREAD_ID, " \
    "DATASOURCE_NAME, CONNECTION_ID, POOL_ID, OPERATION, OBJECT_NAME, " \
    "SQL_HASH, CACHE_KEY_HASH, START_TIME_TS, END_TIME_TS, CACHE_LOOKUP_US, " \
    "LEVEL1_PARSE_US, LEVEL2_PARSE_US, SQL_PARSE_US, EXECUTION_US, TOTAL_US, " \
    "ROWS_AFFECTED, OUTPUT_XML_BYTES, CLOB_BYTES, LOB_BYTES, BYTES_PROCESSED, " \
    "CACHE_HIT, STATUS_CODE, ERROR_CODE, ERROR_TEXT, CONNECTION_WAIT_US, " \
    "CONNECTION_CREATE_US, CONNECTION_ACQUIRE_US" \
    ") VALUES (" \
    ":1, :2, :3, :4, :5, :6, :7, :8, :9, :10, :11, :12, :13, :14, :15, :16, " \
    ":17, :18, TO_TIMESTAMP(:19,'YYYY-MM-DD HH24:MI:SS.FF6'), " \
    "TO_TIMESTAMP(:20,'YYYY-MM-DD HH24:MI:SS.FF6'), :21, :22, :23, :24, :25, " \
    ":26, :27, :28, :29, :30, :31, :32, :33, :34, :35, :36, :37, :38)"

/* Simple bind helper - matches SQLT_STR string binding already used
 * throughout this codebase's own insert modules (see
 * OCI_Insert_Execute_Module.c's own OCIBindByPos usage) - just without
 * that module's own array-batch bookkeeping, since this binds exactly
 * one row per call.                                                    */
#define BIND_STR(pos, buf) \
    OCIBindByPos(g_metrics_insert_stmt, &bindp, ctx->errhp, (pos), \
                 (dvoid *)(buf), (sb4)(strlen(buf) + 1), SQLT_STR, \
                 NULL, NULL, NULL, 0, NULL, OCI_DEFAULT)
#define BIND_NUM(pos, val) \
    OCIBindByPos(g_metrics_insert_stmt, &bindp, ctx->errhp, (pos), \
                 (dvoid *)&(val), (sb4)sizeof(val), SQLT_INT, \
                 NULL, NULL, NULL, 0, NULL, OCI_DEFAULT)

static void metrics_db_bulk_insert(oci_context_t *ctx,
                                    metrics_record_t **batch,
                                    int batch_count)
{
    if (batch_count <= 0) { return; }

    if (!g_metrics_insert_stmt)
    {
        if (OCIStmtPrepare2(ctx->svchp, &g_metrics_insert_stmt, ctx->errhp,
                             (text *)METRICS_INSERT_SQL,
                             (ub4)strlen(METRICS_INSERT_SQL),
                             NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) != OCI_SUCCESS)
        {
            logger_write(ctx->metrics_writer_logger, LOG_ERROR, __func__, 0,
                         "OCIStmtPrepare2 failed for metrics insert - "
                         "dropping this batch of %d record(s)", batch_count);
            for (int i = 0; i < batch_count; i++) metrics_record_deep_free(batch[i]);
            return;
        }
    }

    int inserted = 0;

    for (int i = 0; i < batch_count; i++)
    {
        metrics_record_t *m = batch[i];
        OCIBind *bindp = NULL;

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

        int server_port = (int)m->server_port, process_id = (int)m->process_id,
            thread_id = (int)m->thread_id, connection_id = (int)m->connection_id,
            pool_id = (int)m->pool_id, cache_hit = m->cache_hit,
            status_code = m->status_code;
        long long sql_hash = (long long)m->sql_hash,
                  cache_key_hash = (long long)m->cache_key_hash,
                  cache_lookup_us = (long long)m->cache_lookup_us,
                  level1_parse_us = (long long)m->level1_parse_us,
                  level2_parse_us = (long long)m->level2_parse_us,
                  sql_parse_us = (long long)m->sql_parse_us,
                  execution_us = (long long)m->execution_us,
                  total_us = (long long)m->total_us,
                  rows_affected = (long long)m->rows_affected,
                  output_xml_bytes = (long long)m->output_xml_bytes,
                  clob_bytes = (long long)m->clob_bytes,
                  lob_bytes = (long long)m->lob_bytes,
                  bytes_processed = (long long)m->bytes_processed,
                  connection_wait_us = (long long)m->connection_wait_us,
                  connection_create_us = (long long)m->connection_create_us,
                  connection_acquire_us = (long long)m->connection_acquire_us;

        sword rc = OCI_SUCCESS;
        rc |= BIND_STR(1,  consumer_name);
        rc |= BIND_STR(2,  session_id);
        rc |= BIND_STR(3,  transaction_id);
        rc |= BIND_STR(4,  transaction_name);
        rc |= BIND_STR(5,  audit_id);
        rc |= BIND_STR(6,  client_ip);
        rc |= BIND_STR(7,  host_name);
        rc |= BIND_STR(8,  server_ip);
        rc |= BIND_NUM(9,  server_port);
        rc |= BIND_NUM(10, process_id);
        rc |= BIND_NUM(11, thread_id);
        rc |= BIND_STR(12, datasource_name);
        rc |= BIND_NUM(13, connection_id);
        rc |= BIND_NUM(14, pool_id);
        rc |= BIND_STR(15, operation);
        rc |= BIND_STR(16, object_name);
        rc |= BIND_NUM(17, sql_hash);
        rc |= BIND_NUM(18, cache_key_hash);
        rc |= BIND_STR(19, start_time);
        rc |= BIND_STR(20, end_time);
        rc |= BIND_NUM(21, cache_lookup_us);
        rc |= BIND_NUM(22, level1_parse_us);
        rc |= BIND_NUM(23, level2_parse_us);
        rc |= BIND_NUM(24, sql_parse_us);
        rc |= BIND_NUM(25, execution_us);
        rc |= BIND_NUM(26, total_us);
        rc |= BIND_NUM(27, rows_affected);
        rc |= BIND_NUM(28, output_xml_bytes);
        rc |= BIND_NUM(29, clob_bytes);
        rc |= BIND_NUM(30, lob_bytes);
        rc |= BIND_NUM(31, bytes_processed);
        rc |= BIND_NUM(32, cache_hit);
        rc |= BIND_NUM(33, status_code);
        rc |= BIND_STR(34, error_code);
        rc |= BIND_STR(35, error_text);
        rc |= BIND_NUM(36, connection_wait_us);
        rc |= BIND_NUM(37, connection_create_us);

        if (rc == OCI_SUCCESS)
            rc = OCIBindByPos(g_metrics_insert_stmt, &bindp, ctx->errhp, 38,
                               (dvoid *)&connection_acquire_us,
                               (sb4)sizeof(connection_acquire_us), SQLT_INT,
                               NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
            /* Position 38, the 38th and final column - matches
             * CONNECTION_ACQUIRE_US as the last column in both the SQL's
             * own column list and its VALUES clause. A genuine off-by-
             * one existed here during development (SQL's VALUES clause
             * only went up to :37, one short of the 38-column list) -
             * caught by scripted position-count verification before
             * delivery, not by manual counting, which had already
             * missed it once.                                          */

        if (rc != OCI_SUCCESS)
        {
            logger_write(ctx->metrics_writer_logger, LOG_ERROR, __func__, 0,
                         "OCIBindByPos failed for metrics row %d in this "
                         "batch - skipping this one row, continuing with "
                         "the rest", i);
            continue;
        }

        rc = OCIStmtExecute(ctx->svchp, g_metrics_insert_stmt, ctx->errhp,
                             1, 0, NULL, NULL, OCI_DEFAULT);
        if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO)
        {
            logger_write(ctx->metrics_writer_logger, LOG_ERROR, __func__, 0,
                         "OCIStmtExecute failed for metrics row %d in "
                         "this batch - skipping this one row, continuing "
                         "with the rest", i);
            continue;
        }

        inserted++;
    }

    OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT);

    logger_write(ctx->metrics_writer_logger, LOG_INFO, __func__, 0,
                 "Inserted %d of %d metrics record(s) this batch",
                 inserted, batch_count);

    for (int i = 0; i < batch_count; i++)
        metrics_record_deep_free(batch[i]);
}

#undef BIND_STR
#undef BIND_NUM

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

    if (OCI_Pool_get_session(metrics_base_ctx, &thread_ctx) != 0)
    {
        logger_write(writer_logger, LOG_ERROR, __func__, 0,
                     "Metrics DB writer thread: OCI_Pool_get_session "
                     "failed - this thread cannot start, DB metrics "
                     "will never be persisted until the process "
                     "restarts (file metrics, if enabled, are "
                     "unaffected)");
        return NULL;
    }

    copy_shared_ctx_fields(&thread_ctx, metrics_base_ctx);
    thread_ctx.active_tx = NULL;

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
        OCI_Pool_release_session(metrics_base_ctx, &thread_ctx);
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

    OCI_Pool_release_session(metrics_base_ctx, &thread_ctx);

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
