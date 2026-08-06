#ifndef FILE_CONSUMER_H
#define FILE_CONSUMER_H

/* ======================================================================
 * file_consumer.h
 *
 * Single-threaded File Consumer: scans file_consumer_input_xml_dir /
 * file_consumer_input_json_dir, reads each file's payload itself
 * (Payload Ownership addendum), and round-robin enqueues a
 * RequestObject (request_object.h) via queue_manager. Queue-Full
 * rejections and read failures both skip Processing_* entirely and go
 * straight from Input_* to Error_*, per the Queue-Full Behavior
 * addendum.
 *
 * Stage 5 update: File Consumer no longer owns the queue_manager or
 * drains it - it just scans and enqueues into a queue_manager that's
 * now created once, up front, and shared with a long-running worker
 * thread pool (worker.h) that main() owns and keeps alive across many
 * scan passes. This is the split that made long-running workers
 * possible: Stage 4's file_consumer_run_once() created its own
 * queue_manager, enqueued, drained it synchronously via a single
 * worker call, then destroyed it - one-shot-per-pass, which is exactly
 * the "recreate threads every pass" cost Terry flagged as too
 * expensive (2026-08-06). file_consumer_scan_once() below is the
 * scan-and-enqueue half only.
 *
 * Logs to ctx->file_consumer_logger (its own dedicated log file,
 * file_consumer_log_file_name in config.ini) rather than borrowing
 * connectionpool_logger as it did at first.
 * ====================================================================== */

#include "OCI_Connection.h"   /* oci_context_t */
#include "ini_reader.h"       /* app_config_t  */
#include "queue_manager.h"

/*
 * file_consumer_scan_once()
 *
 * One pass over both input directories: scan, read each file's
 * payload, round-robin enqueue into qm (created and owned by the
 * caller - see worker.h's worker_pool_start(), which needs the same
 * queue_manager_t so workers can drain what gets enqueued here).
 * Rejects immediately (skipping Processing_* entirely, straight to
 * Error_*) on read failure or if every queue is full. Does not drain
 * anything itself and does not loop or sleep - looping across passes
 * is main()'s job, actual draining is the worker pool's job.
 *
 * session_id (Session Manager proposal, Stage 1, 2026-08-06): File
 * Consumer's own real, currently-held session for this run - stamped
 * onto every RequestObject built this pass, so it can override
 * whatever the payload itself carries once dispatcher.c gets it (see
 * request_object.h). Pass NULL/"" if File Consumer doesn't currently
 * hold a valid session (e.g. session_create() itself just failed) -
 * requests built this pass simply won't carry an override, identical
 * to this function's behaviour before this stage.
 *
 * Returns the total number of files enqueued (>= 0, files skipped for
 * being zero-length or non-regular don't count, and immediately
 * rejected files don't count either since they never reach a queue),
 * or -1 if BOTH input directories failed to open (a config-level
 * problem - wrong path in consumer_file.ini, permissions, etc.). If
 * only one of the two directories fails to open, that failure is
 * logged and the other directory is still processed normally.
 */
int file_consumer_scan_once(oci_context_t *ctx, app_config_t *config,
                             queue_manager_t *qm, const char *session_id);

#endif /* FILE_CONSUMER_H */
