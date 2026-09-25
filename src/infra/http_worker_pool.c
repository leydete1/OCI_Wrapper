/* ======================================================================
 * http_worker_pool.c
 *
 * See http_worker_pool.h for the full Stage 4 design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http_worker_pool.h"
#include "queue_manager.h"
#include "Connection_Pool.h"
#include "ctx_utils.h"
#include "dispatcher.h"
#include "logger.h"

/* Stack-local to the MHD handler thread that's waiting - never
 * malloc'd, never outlives the dispatch() call that created it. The
 * worker thread only ever touches it between dispatch()'s enqueue and
 * its own pthread_cond_wait() returning, so its lifetime is always
 * safely bounded by the waiting thread's own stack frame. */
typedef struct {
    pthread_mutex_t    mutex;
    pthread_cond_t     cond;
    int                done;
    response_object_t  resp;
} http_completion_t;

typedef struct {
    http_worker_pool_t *pool;
    int                  queue_index;
} worker_thread_args_t;

struct http_worker_pool {
    queue_manager_t       *qm;
    oci_context_t         *base_ctx;
    int                     queue_count;
    pthread_t              *threads;
    worker_thread_args_t   *thread_args;

    /* Bug fix (2026-08-21) - queue_manager's own round-robin cursor is
     * explicitly documented (queue_manager.h) as NOT thread-safe for
     * concurrent enqueuers: "Only File Consumer's single scanning
     * thread is expected to call queue_manager_enqueue()". File
     * Consumer only ever has one thread enqueueing; HTTP consumer has
     * up to http_consumer_thread_pool_size MHD handler threads calling
     * http_worker_pool_dispatch() - and therefore queue_manager_
     * enqueue_excluding()/enqueue_to() - CONCURRENTLY. That's a data
     * race on queue_manager's internal state that queue_manager itself
     * was never built to survive. This mutex serializes the enqueue
     * call itself (only that call, not the wait-for-completion that
     * follows it) across every HTTP handler thread, restoring the
     * single-producer assumption queue_manager actually relies on. */
    pthread_mutex_t         enqueue_mutex;
};

/* Manual case-insensitive substring search - avoids depending on
 * strcasestr(), a GNU extension not guaranteed available without
 * _GNU_SOURCE (same reasoning as the tester's own contains_ci()). */
static int contains_ci(const char *haystack, const char *needle)
{
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++)
        if (strncasecmp(haystack + i, needle, nlen) == 0)
            return 1;
    return 0;
}

int http_request_is_write(const char *payload)
{
    return (contains_ci(payload, "\"INSERT\"")      || contains_ci(payload, "type=\"INSERT\"")  ||
            contains_ci(payload, "\"UPDATE\"")      || contains_ci(payload, "type=\"UPDATE\"")  ||
            contains_ci(payload, "\"DELETE\"")      || contains_ci(payload, "type=\"DELETE\""));
}

static void *http_worker_thread_main(void *arg_v)
{
    worker_thread_args_t *arg = (worker_thread_args_t *)arg_v;
    http_worker_pool_t   *pool = arg->pool;
    int                    queue_index = arg->queue_index;

    /* Fix (2026-09-15, found investigating a real ORA-01002 recurrence
     * under full concurrent load) - worker.c's own thread-start function
     * calls logger_set_worker_id() first thing (see its own comment,
     * 2026-08-07 fix); http_worker_pool.c - what every HTTP-consumer
     * deployment actually runs - never did. Every log line from every
     * HTTP worker thread, and everything it calls into (dispatcher.c,
     * every CRUD execute module, driver_oracle.c), has always shown
     * [T0] regardless of which real worker actually ran it - the exact
     * same mislabeling the 2026-08-07 fix's own comment warns caused a
     * false lead during the original ORA-03114 investigation, just
     * never closed off on this specific code path. Set first, before
     * anything else on this thread logs, same placement and reasoning
     * as worker.c. queue_index is already this thread's own natural
     * identifier - used a few lines down for this same thread's own
     * startup log line, nothing new introduced by using it here too. */
    logger_set_worker_id(queue_index);

    oci_context_t thread_ctx;
    memset(&thread_ctx, 0, sizeof(thread_ctx));   /* see http_consumer.c's
        own Stage 2 note on why this is mandatory before OCI_Pool_get_session() -
        it does not zero the struct itself. */

    if (OCI_Pool_get_session(pool->base_ctx, &thread_ctx) != 0)
    {
        logger_write(pool->base_ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "HTTP worker %d: OCI_Pool_get_session failed at startup - "
                     "this worker cannot run, queue %d will never be serviced",
                     queue_index, queue_index);
        return NULL;
    }
    copy_shared_ctx_fields(&thread_ctx, pool->base_ctx);

    logger_write(pool->base_ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                 "HTTP worker %d started - servicing queue %d (%s), "
                 "pool_slot_index=%d",
                 queue_index, queue_index,
                 queue_index == 0 ? "dedicated writer queue" : "round-robin",
                 thread_ctx.pool_slot_index);

    for (;;)
    {
        request_object_t *req = queue_manager_dequeue_blocking(pool->qm, queue_index);
        if (!req) break;   /* queue_manager_shutdown() was called and this
                               queue has fully drained - clean exit signal */

        /* Diagnostic logging (2026-08-21) - see the matching log in
         * http_worker_pool_dispatch(). Confirms which queue actually
         * ended up with a given request, independent of what routing
         * intended - the two logs together show whether a mismatch
         * happens at the routing decision or somewhere in queue_manager
         * itself. Safe to remove once the cause is confirmed and fixed. */
        logger_write(pool->base_ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                     "HTTP worker %d: dequeued item, payload_snippet=\"%.100s\"",
                     queue_index, req->payload ? req->payload : "(empty)");

        thread_ctx.active_tx = NULL;

        response_object_t resp;
        response_object_init(&resp);

        const char *payload = req->payload ? req->payload : "";
        process_xml_file(&thread_ctx, payload, req->payload_length,
                          req->filename,
                          req->session_id[0] ? req->session_id : NULL,
                          &resp);

        http_completion_t *completion = (http_completion_t *)req->completion_ctx;
        if (completion)
        {
            pthread_mutex_lock(&completion->mutex);
            completion->resp = resp;   /* ownership of resp.response_body
                                           transfers here - do not free resp
                                           separately below */
            completion->done = 1;
            pthread_cond_signal(&completion->cond);
            pthread_mutex_unlock(&completion->mutex);
        }
        else
        {
            /* Stage 5 (2026-08-23): this is now an EXPECTED outcome,
             * not just a defensive fallback - http_worker_pool_dispatch_
             * async() deliberately never attaches a completion_ctx for
             * execute_async requests, since nobody's waiting for this
             * response (the real result already went out via the
             * per-batch callback). Still fine as a genuine fallback too,
             * if some future caller ever forgets to attach one by
             * mistake - either way there's nothing to signal and
             * nothing to leak. Downgraded from WARN to INFO now that
             * it's a normal, expected code path rather than an anomaly. */
            logger_write(pool->base_ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                         "HTTP worker %d: dequeued a fire-and-forget "
                         "request (no completion_ctx) - discarding its "
                         "response, as expected",
                         queue_index);
            response_object_free(&resp);
        }

        request_object_free(req);
    }

    OCI_Pool_release_session(pool->base_ctx, &thread_ctx);
    logger_write(pool->base_ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                 "HTTP worker %d stopped", queue_index);
    return NULL;
}

http_worker_pool_t *http_worker_pool_start(oci_context_t *base_ctx,
                                            app_config_t  *config)
{
    int queue_count = config->http_dispatcher_queue_count > 0
                       ? config->http_dispatcher_queue_count : 5;
    int queue_depth  = config->http_dispatcher_queue_depth > 0
                       ? config->http_dispatcher_queue_depth : 50;

    http_worker_pool_t *pool = calloc(1, sizeof(http_worker_pool_t));
    if (!pool)
    {
        logger_write(base_ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_worker_pool_start: calloc failed");
        return NULL;
    }

    pool->qm = queue_manager_create(queue_count, queue_depth);
    if (!pool->qm)
    {
        logger_write(base_ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_worker_pool_start: queue_manager_create failed "
                     "(queue_count=%d, queue_depth=%d)", queue_count, queue_depth);
        free(pool);
        return NULL;
    }

    pool->base_ctx    = base_ctx;
    pool->queue_count = queue_count;
    pool->threads      = malloc(sizeof(pthread_t) * queue_count);
    pool->thread_args  = malloc(sizeof(worker_thread_args_t) * queue_count);
    pthread_mutex_init(&pool->enqueue_mutex, NULL);

    if (!pool->threads || !pool->thread_args)
    {
        logger_write(base_ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_worker_pool_start: malloc failed for thread arrays");
        queue_manager_destroy(pool->qm);
        free(pool->threads);
        free(pool->thread_args);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < queue_count; i++)
    {
        pool->thread_args[i].pool        = pool;
        pool->thread_args[i].queue_index = i;

        if (pthread_create(&pool->threads[i], NULL,
                            http_worker_thread_main, &pool->thread_args[i]) != 0)
        {
            logger_write(base_ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                         "http_worker_pool_start: pthread_create failed for "
                         "queue %d - this queue will never be serviced", i);
        }
    }

    logger_write(base_ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                 "HTTP Consumer: worker pool started - %d queues total "
                 "(queue 0 = dedicated writer queue for INSERT/UPDATE/"
                 "DELETE, queues 1-%d round-robin for SELECT/EXECUTE_"
                 "PROCEDURE), depth=%d each",
                 queue_count, queue_count - 1, queue_depth);

    return pool;
}

int http_worker_pool_dispatch(http_worker_pool_t *pool,
                               request_object_t   *req,
                               response_object_t  *out_resp)
{
    http_completion_t completion;
    pthread_mutex_init(&completion.mutex, NULL);
    pthread_cond_init(&completion.cond, NULL);
    completion.done = 0;

    request_object_set_completion(req, &completion);

    /* Diagnostic logging (2026-08-21) - added specifically to nail down
     * why real test data showed writes split across multiple
     * connections instead of funnelling through queue 0 alone. Turns
     * "does this routing decision look right by code inspection" into
     * concrete, per-request evidence in http_consumer_Data_Manager.log.
     * Safe to remove once the actual cause is confirmed and fixed. */
    int is_write = http_request_is_write(req->payload ? req->payload : "");
    logger_write(pool->base_ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                 "HTTP Consumer: routing decision - is_write=%d, "
                 "target=%s, payload_snippet=\"%.100s\"",
                 is_write, is_write ? "queue 0 (writer)" : "round-robin (excluding 0)",
                 req->payload ? req->payload : "(empty)");

    /* Serialized (see enqueue_mutex's own doc comment on the struct) -
     * queue_manager's round-robin cursor is not safe for concurrent
     * callers, and this dispatch function IS called concurrently, from
     * every MHD handler thread. Only the enqueue call itself needs the
     * lock - the wait below happens after we've already released it,
     * so one slow request blocked waiting for its worker never holds up
     * anyone else's enqueue. */
    pthread_mutex_lock(&pool->enqueue_mutex);
    int enqueue_rc = is_write
        ? queue_manager_enqueue_to(pool->qm, req, 0)
        : queue_manager_enqueue_excluding(pool->qm, req, 0);
    pthread_mutex_unlock(&pool->enqueue_mutex);

    if (enqueue_rc != 0)
    {
        /* QUEUE_FULL - caller still owned req per queue_manager's own
           contract; we take care of freeing it here so the caller's own
           contract with US is simpler ("req is always consumed"). */
        request_object_free(req);
        pthread_mutex_destroy(&completion.mutex);
        pthread_cond_destroy(&completion.cond);
        return -1;
    }

    pthread_mutex_lock(&completion.mutex);
    while (!completion.done)
        pthread_cond_wait(&completion.cond, &completion.mutex);
    pthread_mutex_unlock(&completion.mutex);

    *out_resp = completion.resp;   /* ownership transfers to caller */

    pthread_mutex_destroy(&completion.mutex);
    pthread_cond_destroy(&completion.cond);
    return 0;
}

int http_request_is_async_select(const char *payload)
{
    return (contains_ci(payload, "<execute_async>1</execute_async>") ||
            contains_ci(payload, "\"execute_async\":1")              ||
            contains_ci(payload, "\"execute_async\": 1")             ||
            contains_ci(payload, "\"execute_async\":true")           ||
            contains_ci(payload, "\"execute_async\": true"));
}

int http_worker_pool_dispatch_async(http_worker_pool_t *pool,
                                     request_object_t   *req)
{
    /* Deliberately no request_object_set_completion() call - a NULL
     * completion_ctx is exactly what tells the worker thread this is
     * fire-and-forget (see http_worker_thread_main()'s own handling of
     * that case). Always the round-robin path, never queue 0 - Level 2
     * has already confirmed execute_async only ever applies to a SELECT
     * with no sibling writes in its transaction, so there is nothing to
     * write-detect here. */
    pthread_mutex_lock(&pool->enqueue_mutex);
    int enqueue_rc = queue_manager_enqueue_excluding(pool->qm, req, 0);
    pthread_mutex_unlock(&pool->enqueue_mutex);

    if (enqueue_rc != 0)
    {
        request_object_free(req);
        return -1;
    }

    return 0;
}

void http_worker_pool_stop(http_worker_pool_t *pool)
{
    if (!pool) return;

    queue_manager_shutdown(pool->qm);
    for (int i = 0; i < pool->queue_count; i++)
        pthread_join(pool->threads[i], NULL);

    queue_manager_destroy(pool->qm);
    pthread_mutex_destroy(&pool->enqueue_mutex);
    free(pool->threads);
    free(pool->thread_args);
    free(pool);
}
