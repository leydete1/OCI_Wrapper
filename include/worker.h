#ifndef WORKER_H
#define WORKER_H

/* ======================================================================
 * worker.h
 *
 * Stage 4 (File_Consumer_proposal v1.2) - drains queue_manager's
 * queues and runs each RequestObject through the same dispatch +
 * Response Manager pipeline file_consumer.c used to call directly
 * before the queue layer existed.
 *
 * Still single-threaded and synchronous for this stage - worker_run()
 * is called once, drains everything currently queued, and returns.
 * Stage 5 wraps N of these in pthread_create(), one per queue, with
 * each thread pinning its own DB session from the pool (Session Model
 * decision, File_Consumer_proposal v1.2) - nothing here changes for
 * that, worker_run() just starts getting called from a thread instead
 * of directly from file_consumer_run_once().
 *
 * Logs to ctx->worker_logger - a single shared logger across whatever
 * ends up calling this (one thread today, N threads from Stage 5 on).
 * logger.c's log_mutex already serialises all logger_write() calls
 * globally regardless of which logger_t instance is used, so a shared
 * logger is safe under real concurrency without any extra locking here
 * - see the design discussion from 2026-08-04. The thread_id parameter
 * every logger_write() call already takes (currently always 0
 * throughout the whole codebase) is what Stage 5 will start setting to
 * each worker's real index, making a shared worker_Data_Manager.log
 * filterable per-worker via e.g. grep '\[T3\]' - not needed yet with
 * one synchronous caller, but the plumbing is ready for it.
 * ====================================================================== */

#include "OCI_Connection.h"     /* oci_context_t */
#include "queue_manager.h"

/*
 * worker_run()
 *
 * Drains qm completely: for each RequestObject, calls
 * process_xml_file() with its payload, builds/gets the response, and
 * calls response_manager_write() using the output_dir/error_dir
 * carried on the RequestObject itself (set by whoever enqueued it -
 * file_consumer.c, so far). Frees each RequestObject once done with
 * it.
 *
 * Returns the number of items drained (>= 0).
 */
int worker_run(oci_context_t *ctx, queue_manager_t *qm);

#endif /* WORKER_H */
