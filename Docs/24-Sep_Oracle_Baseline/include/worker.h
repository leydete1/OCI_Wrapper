#ifndef WORKER_H
#define WORKER_H

/* ======================================================================
 * worker.h
 *
 * Stage 5 (File_Consumer_proposal v1.2) - N long-running worker
 * threads, one per queue, each pinning its own DB session from the
 * connection pool for its entire lifetime (Session Model decision -
 * no session affinity between requests, but the session itself is
 * borrowed once per thread, not once per request, since each request
 * already commits/rolls back atomically on its own - see
 * OCI_Insert_Execute_Module.c's own OCITransCommit-when-no-managed-tx
 * behaviour, confirmed 2026-08-06, which is exactly what makes
 * per-thread session pinning safe without any extra tx wrapping here).
 *
 * Threads are created once via worker_pool_start() and live until
 * worker_pool_shutdown_and_join() is called - long-running, not
 * spun-up-per-scan-pass, because thread startup is too expensive to
 * repeat on every File Consumer pass (Terry's call, 2026-08-06). Each
 * thread blocks on its own queue's condition variable
 * (queue_manager_dequeue_blocking()) when there's nothing to do,
 * rather than exiting - this is the change from Stage 4's
 * "worker_run() drains once and returns" model.
 *
 * Logs to ctx->worker_logger - a single shared logger across however
 * many threads. logger.c's log_mutex already serialises all
 * logger_write() calls globally regardless of which logger_t instance
 * is used, so this is safe under real concurrency without any extra
 * locking here - see the design discussion from 2026-08-04. Each
 * thread now passes its own worker_id as logger_write()'s thread_id
 * parameter (previously always 0 throughout the whole codebase, since
 * nothing was actually threaded yet), so worker_Data_Manager.log is
 * filterable per-thread via e.g. grep '\[T3\]'.
 * ====================================================================== */

#include "OCI_Connection.h"     /* oci_context_t */
#include "queue_manager.h"
#include "generic_queue.h"

typedef struct worker_pool worker_pool_t;   /* opaque */

/*
 * worker_pool_start()
 *
 * Creates worker_count threads (normally config->dispatcher_queue_count
 * - one thread per queue, so every queue has exactly one dedicated
 * consumer). base_ctx must already be fully set up (connected,
 * loggers initialised) - each thread borrows its own session from the
 * pool via OCI_Pool_get_session(base_ctx, ...) and copies the same
 * shared logger/ini/cache pointers base_ctx already has, mirroring
 * main()'s own single-worker-ctx setup for pool mode.
 *
 * touch_q (Session Manager proposal, Stage 2, 2026-08-06; now built on
 * generic_queue.h rather than the original session_touch_queue.h - see
 * session_manager_runner.h's own note on that swap, 2026-08-09): each
 * worker enqueues a fire-and-forget touch message onto this queue
 * right after building a ResponseObject for a request that carried a
 * real session_id. Pass NULL to disable this (no touch messages
 * enqueued at all) if the Session Manager thread isn't running.
 *
 * base_ctx, qm, and touch_q must all outlive the returned pool - the
 * caller is responsible for keeping them alive until after
 * worker_pool_shutdown_and_join() returns.
 *
 * Returns NULL on allocation failure or if any thread fails to start
 * (in which case any threads that did start are asked to shut down
 * and joined before returning NULL - no partial pool is left running).
 */
worker_pool_t *worker_pool_start(oci_context_t   *base_ctx,
                                  queue_manager_t *qm,
                                  generic_queue_t *touch_q,
                                  int              worker_count);

/*
 * worker_pool_shutdown_and_join()
 *
 * Calls queue_manager_shutdown(qm) to wake every blocked thread, waits
 * for each to finish whatever it's currently processing and drain the
 * rest of its queue, joins all threads, then frees the pool. Each
 * thread releases its pooled session (OCI_Pool_release_session())
 * before it exits, so by the time this returns every borrowed session
 * is back in the pool.
 *
 * Safe to call with NULL.
 */
void worker_pool_shutdown_and_join(worker_pool_t *pool, queue_manager_t *qm);

#endif /* WORKER_H */
