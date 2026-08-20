#ifndef HTTP_WORKER_POOL_H
#define HTTP_WORKER_POOL_H

/* ======================================================================
 * http_worker_pool.h
 *
 * Stage 4 (2026-08-20) - routes HTTP consumer's CRUD requests through
 * the same queue_manager/Contention Manager pattern File Consumer
 * already relies on, instead of Stage 2/3's direct synchronous
 * dispatch. Design agreed with Terry:
 *
 *   - HTTP consumer gets its OWN queue_manager_t instance (own queues,
 *     own dedicated worker threads) - entirely separate from File
 *     Consumer's, consistent with consumer_type currently gating the
 *     two as mutually exclusive.
 *   - Queue 0 ("T0") is the dedicated single writer queue - every
 *     INSERT/UPDATE/DELETE (and any mixed transaction containing one)
 *     goes there, one connection, exactly like File Consumer's own
 *     contention_manager.mode=single_write_queue.
 *   - Queues 1..N-1 ("T1"..) round-robin everything else (SELECT/
 *     EXECUTE_PROCEDURE) - confirmed explicitly: these also go through
 *     the queue, not a separate synchronous fast-path. One consistent
 *     code path for every CRUD request.
 *   - CREATE_SESSION/END_SESSION are NOT routed through this pool -
 *     they stay on http_consumer.c's existing direct/synchronous path
 *     (handle_session_request()), unchanged from Stage 2/3. Session
 *     lifecycle doesn't touch contention-prone tables, so there's
 *     nothing here for it to gain.
 *
 * The missing piece Stage 2/3 didn't need: File Consumer's own workers
 * are fire-and-forget (response gets written to a file later, nobody's
 * waiting). An HTTP client IS waiting on its connection. See
 * request_object.h's new completion_ctx field - each dispatch call
 * attaches a stack-local http_completion_t (owned by the calling MHD
 * handler thread, which blocks on it) to the request before enqueueing;
 * the worker thread that eventually processes it signals that same
 * completion object when done.
 *
 * Nice side effect of this stage, not the primary goal: these are
 * genuinely long-lived worker threads (one per queue, created once,
 * living for the pool's whole lifetime) - unlike the MHD-managed
 * thread pool, we DO get a real "this thread just started" moment, so
 * each worker borrows ONE session at startup and reuses it for every
 * request it ever processes, same as every other long-lived-thread
 * consumer in this codebase. The per-request borrow/release Stage 2
 * settled for (documented as a deliberate, revisitable simplification)
 * goes away entirely for anything that reaches this pool.
 *
 * Concurrent-enqueue fix (2026-08-21): queue_manager's own round-robin
 * cursor is explicitly documented as safe only for a single enqueueing
 * thread (File Consumer's own single scanning thread). HTTP consumer's
 * MHD handler threads call http_worker_pool_dispatch() - and therefore
 * queue_manager's enqueue functions - concurrently, which queue_manager
 * itself was never built to survive. http_worker_pool_dispatch() now
 * serializes the enqueue call itself under its own mutex, restoring
 * that single-producer assumption without touching queue_manager.c at
 * all. Found via real evidence, not by review: a Stage 4 test run's
 * metrics showed write traffic split across three different
 * connections instead of one, with read/write traffic mixed on the
 * same connections that should have been strictly separated.
 * ====================================================================== */

#include <pthread.h>

#include "OCI_Connection.h"
#include "ini_reader.h"
#include "request_object.h"
#include "response_object.h"

typedef struct http_worker_pool http_worker_pool_t;   /* opaque */

/*
 * http_worker_pool_start()
 *
 * Creates the queue_manager_t (sized from config->http_dispatcher_
 * queue_count / http_dispatcher_queue_depth - see ini_reader.h/
 * consumer_http.ini) and starts one dedicated worker thread per queue.
 * base_ctx must already be fully connected/pooled and outlive the
 * returned pool; each worker borrows its own session from
 * base_ctx's pool at startup.
 *
 * Returns NULL on any allocation/queue_manager_create() failure -
 * logged via base_ctx->http_consumer_logger at LOG_ERROR.
 */
http_worker_pool_t *http_worker_pool_start(oci_context_t *base_ctx,
                                            app_config_t  *config);

/*
 * http_worker_pool_dispatch()
 *
 * Routes req to the correct queue (write detection via
 * http_request_is_write() below) and BLOCKS the calling thread until a
 * worker finishes processing it, then copies the result into
 * *out_resp (caller must response_object_free() it - see
 * response_object.h).
 *
 * req is ALWAYS consumed by this call, regardless of outcome - freed
 * internally on a QUEUE_FULL failure, freed by the worker thread after
 * successful processing. The caller must never touch or free req again
 * after this call, on any code path.
 *
 * Returns 0 on success. Returns -1 if the target queue was full (the
 * caller should build a QUEUE_FULL-style error response of its own -
 * out_resp is left uninitialised in this case, do not
 * response_object_free() it).
 */
int http_worker_pool_dispatch(http_worker_pool_t *pool,
                               request_object_t   *req,
                               response_object_t  *out_resp);

/*
 * http_request_is_write()
 *
 * Cheap content sniff (not real parsing) for whether payload contains
 * an INSERT/UPDATE/DELETE operation, XML or JSON. A mixed transaction
 * containing a write alongside reads still returns true here,
 * deliberately - see this header's own design note above: the whole
 * request routes to the writer queue if ANY part of it is a write.
 */
int http_request_is_write(const char *payload);

/*
 * http_worker_pool_stop()
 *
 * queue_manager_shutdown() (wakes every worker, lets each drain
 * whatever's already in its queue), pthread_join()s every worker
 * thread, releases each worker's borrowed session, then
 * queue_manager_destroy()s the queue_manager_t and frees the pool.
 * Safe to call with NULL. Call this AFTER MHD_stop_daemon() has
 * already returned (http_consumer_runner_stop() does this ordering) -
 * by that point every in-flight HTTP connection has already received
 * its response via the completion signal, so this is pure idle-thread
 * cleanup, not a race with anything still in flight.
 */
void http_worker_pool_stop(http_worker_pool_t *pool);

#endif /* HTTP_WORKER_POOL_H */
