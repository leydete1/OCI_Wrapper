/* ======================================================================
 * worker.c
 *
 * See worker.h for the full design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "worker.h"
#include "dispatcher.h"
#include "response_object.h"
#include "response_manager.h"
#include "OCI_Connection_Pool.h"
#include "logger.h"

struct worker_pool {
    pthread_t *threads;
    int        thread_count;
};

typedef struct {
    oci_context_t   *base_ctx;
    queue_manager_t *qm;
    int              queue_index;
    int              worker_id;
} worker_thread_args_t;

/* ------------------------------------------------------------------ */
/*  copy_shared_ctx_fields()                                            */
/*                                                                      */
/*  OCI_Pool_get_session() only populates the OCI connection handles    */
/*  (envhp/errhp/svchp etc.) and pool bookkeeping - every logger        */
/*  pointer, the ini pointer, and the caches remain NULL after that     */
/*  call and must be copied in explicitly. This mirrors the exact       */
/*  field list Test_XML_Runner.c's main() already uses for its own      */
/*  single worker_ctx in pool mode - kept as its own function here      */
/*  specifically so N threads can each get their own copy without       */
/*  duplicating this list N times inline. If a new logger is ever       */
/*  added to oci_context_t, it needs to be added BOTH here AND to       */
/*  main()'s own copy block - the two aren't unified into one shared    */
/*  helper today, which is a real duplication risk worth knowing about. */
/* ------------------------------------------------------------------ */
static void copy_shared_ctx_fields(oci_context_t *dst, oci_context_t *src)
{
    dst->logger                = src->logger;
    dst->select_logger         = src->select_logger;
    dst->cache_logger          = src->cache_logger;
    dst->Metadata_logger       = src->Metadata_logger;
    dst->connection_logger     = src->connection_logger;
    dst->connectionpool_logger = src->connectionpool_logger;
    dst->insert_logger         = src->insert_logger;
    dst->update_logger         = src->update_logger;
    dst->delete_logger         = src->delete_logger;
    dst->dml_logger            = src->dml_logger;
    dst->ddl_logger            = src->ddl_logger;
    dst->procedure_logger      = src->procedure_logger;
    dst->ini                   = src->ini;
    dst->resultset_cache       = src->resultset_cache;
    dst->error_logger          = src->error_logger;
    dst->metrics_logger        = src->metrics_logger;
    dst->transaction_logger    = src->transaction_logger;
    dst->security_logger       = src->security_logger;
    dst->crypt_logger          = src->crypt_logger;
    dst->audit_logger          = src->audit_logger;
    dst->session_logger        = src->session_logger;
    dst->sql_parser_logger     = src->sql_parser_logger;
    dst->file_consumer_logger  = src->file_consumer_logger;
    dst->dispatcher_logger     = src->dispatcher_logger;
    dst->worker_logger         = src->worker_logger;
    dst->metadata_cache        = src->metadata_cache;
    dst->session_cache         = src->session_cache;
}

/* ------------------------------------------------------------------ */
/*  worker_thread_main()                                                */
/*  Entry point for each worker thread. Borrows its session, loops     */
/*  draining its own queue until shutdown, releases its session.       */
/* ------------------------------------------------------------------ */
static void *worker_thread_main(void *arg)
{
    worker_thread_args_t args = *(worker_thread_args_t *)arg;
    free(arg);   /* handed off - this thread's own copy on the stack now */

    oci_context_t thread_ctx;
    memset(&thread_ctx, 0, sizeof(thread_ctx));

    if (OCI_Pool_get_session(args.base_ctx, &thread_ctx) != 0)
    {
        /* Can't use thread_ctx's own logger - the borrow that would
         * have populated it failed. Log via base_ctx instead so this
         * failure is at least visible somewhere.                      */
        logger_write(args.base_ctx->worker_logger, LOG_ERROR, __func__,
                     args.worker_id,
                     "Worker[%d]: OCI_Pool_get_session failed - this "
                     "worker thread cannot start, queue[%d] will never "
                     "be drained until the process restarts",
                     args.worker_id, args.queue_index);
        return NULL;
    }

    copy_shared_ctx_fields(&thread_ctx, args.base_ctx);
    thread_ctx.active_tx = NULL;   /* no managed transaction - each
                                       request self-commits via
                                       OCITransCommit, per the Session
                                       Model decision.                  */

    logger_write(thread_ctx.worker_logger, LOG_INFO, __func__, args.worker_id,
                 "Worker[%d]: started, session borrowed, draining queue[%d]",
                 args.worker_id, args.queue_index);

    int processed = 0;
    request_object_t *req;

    while ((req = queue_manager_dequeue_blocking(args.qm, args.queue_index)) != NULL)
    {
        logger_write(thread_ctx.worker_logger, LOG_INFO, __func__, args.worker_id,
                     "Worker[%d]: dequeued '%s' (%ld bytes)",
                     args.worker_id, req->filename, req->payload_length);

        response_object_t resp;
        response_object_init(&resp);

        int rc = process_xml_file(&thread_ctx, req->payload, req->payload_length,
                                   req->filename, &resp);

        if (rc == 0)
            logger_write(thread_ctx.worker_logger, LOG_INFO, __func__, args.worker_id,
                         "Worker[%d]: PASS '%s'", args.worker_id, req->filename);
        else
            logger_write(thread_ctx.worker_logger, LOG_ERROR, __func__, args.worker_id,
                         "Worker[%d]: FAIL '%s' (rc=%d, error_code=%s)",
                         args.worker_id, req->filename, rc, resp.error_code);

        if (response_manager_write(&thread_ctx, &resp, req->filename, req->processing_path,
                                    req->output_dir, req->error_dir) != 0)
        {
            logger_write(thread_ctx.worker_logger, LOG_ERROR, __func__, args.worker_id,
                         "Worker[%d]: Response Manager reported a problem "
                         "writing/moving '%s' - see log above for detail",
                         args.worker_id, req->filename);
        }

        response_object_free(&resp);
        request_object_free(req);

        processed++;
    }

    logger_write(thread_ctx.worker_logger, LOG_INFO, __func__, args.worker_id,
                 "Worker[%d]: shutdown signalled, queue[%d] drained - "
                 "processed %d item(s) total this run, releasing session",
                 args.worker_id, args.queue_index, processed);

    OCI_Pool_release_session(args.base_ctx, &thread_ctx);

    return NULL;
}

/* ------------------------------------------------------------------ */
worker_pool_t *worker_pool_start(oci_context_t   *base_ctx,
                                  queue_manager_t *qm,
                                  int              worker_count)
{
    if (worker_count <= 0) return NULL;

    worker_pool_t *pool = malloc(sizeof(worker_pool_t));
    if (!pool) return NULL;

    pool->threads = calloc((size_t)worker_count, sizeof(pthread_t));
    if (!pool->threads) { free(pool); return NULL; }
    pool->thread_count = 0;   /* bumped as each thread actually starts */

    for (int i = 0; i < worker_count; i++)
    {
        worker_thread_args_t *args = malloc(sizeof(worker_thread_args_t));
        if (!args) break;

        args->base_ctx    = base_ctx;
        args->qm           = qm;
        args->queue_index = i;
        args->worker_id    = i;

        if (pthread_create(&pool->threads[i], NULL, worker_thread_main, args) != 0)
        {
            free(args);
            logger_write(base_ctx->worker_logger, LOG_ERROR, __func__, 0,
                         "worker_pool_start: pthread_create failed for "
                         "worker[%d] - starting %d of %d requested workers "
                         "failed, unwinding the whole pool", i, worker_count, worker_count);
            break;
        }
        pool->thread_count++;
    }

    if (pool->thread_count != worker_count)
    {
        /* Partial start failure - don't leave a half-sized pool
         * silently running. Shut down and join whatever did start,
         * then report total failure.                                  */
        worker_pool_shutdown_and_join(pool, qm);
        return NULL;
    }

    return pool;
}

void worker_pool_shutdown_and_join(worker_pool_t *pool, queue_manager_t *qm)
{
    if (!pool) return;

    queue_manager_shutdown(qm);

    for (int i = 0; i < pool->thread_count; i++)
        pthread_join(pool->threads[i], NULL);

    free(pool->threads);
    free(pool);
}
