#ifndef SESSION_MANAGER_RUNNER_H
#define SESSION_MANAGER_RUNNER_H

/* ======================================================================
 * session_manager_runner.h
 *
 * Session Manager proposal, Stage 2 (2026-08-06). Gives the Session
 * Manager its own dedicated long-running thread, same shape as
 * worker.c and file_consumer_runner.c: borrows its own independent
 * pooled DB connection at startup, holds it for the thread's whole
 * lifetime, drains generic_queue_t at its own pace, releases
 * the session on shutdown.
 *
 * For each session_id dequeued, calls both session_touch() (cache -
 * cheap, matches what a request's own critical path would have done
 * anyway) and session_touch_db() (a real UPDATE against the permanent
 * OCI_SESSION table - see OCI_Session_Manager.h's own doc comment on
 * why that's a separate function, meant specifically to be called off
 * the critical path like this).
 * ====================================================================== */

/* Closure item 5 follow-on (2026-08-09): now built on generic_queue.h
 * rather than the original session_touch_queue.h - the session-touch-
 * specific queue was retired once its needs became a strict subset of
 * the reusable generic module. Ownership contract is identical either
 * way (whoever dequeues owns the item, must free it); the only real
 * difference is that generic_queue_enqueue() never copies its item -
 * the caller (worker.c) now strdup()s the session_id itself before
 * enqueueing, where session_touch_queue_enqueue() used to do that
 * copy internally.                                                    */

#include "OCI_Connection.h"   /* oci_context_t */
#include "generic_queue.h"

typedef struct session_manager_runner session_manager_runner_t;   /* opaque */

/*
 * session_manager_runner_start()
 *
 * base_ctx must already be fully set up (connected, loggers
 * initialised) - this thread borrows its own session from the same
 * pool base_ctx is attached to, exactly like worker.c and
 * file_consumer_runner.c already do. q must already exist and outlive
 * the returned runner.
 *
 * Returns NULL on allocation, thread-start, or session-borrow failure.
 */
session_manager_runner_t *session_manager_runner_start(oci_context_t   *base_ctx,
                                                         generic_queue_t *q);

/*
 * session_manager_runner_stop_and_join()
 *
 * Requests the thread stop (via generic_queue_shutdown()) and
 * blocks until it exits - this still fully drains whatever's left in
 * the queue first, same "shutdown signals, doesn't abandon queued
 * work" guarantee as worker_pool_shutdown_and_join(). Safe to call
 * with NULL.
 */
void session_manager_runner_stop_and_join(session_manager_runner_t *runner,
                                           generic_queue_t          *q);

#endif /* SESSION_MANAGER_RUNNER_H */
