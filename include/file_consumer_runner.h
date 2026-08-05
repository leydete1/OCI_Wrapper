#ifndef FILE_CONSUMER_RUNNER_H
#define FILE_CONSUMER_RUNNER_H

/* ======================================================================
 * file_consumer_runner.h
 *
 * Gives File Consumer its own dedicated thread, mirroring worker.h's
 * pool pattern - main() should create, not implement, both. Before
 * this, main() ran the scan-sleep-scan loop directly on the main
 * thread (a real inconsistency Terry flagged, 2026-08-05 "Findings and
 * lessons" doc, section 1a): the worker pool got proper dedicated
 * threads, but File Consumer itself didn't. Fixed here.
 *
 * Also replaces the old hardcoded FILE_CONSUMER_TEST_PASSES/INTERVAL
 * test-only C constants with real config
 * (dispatcher.poll_interval_seconds / dispatcher.lifetime_seconds in
 * consumer_file.ini, section 1b of the same doc) - this thread now
 * scans on a real configurable interval and stops on a real
 * configurable lifetime (0 = run forever) rather than a fixed test
 * pass count.
 *
 * This module is deliberately generic - it's the intended template for
 * the future HTTP consumer to follow the same pattern (its own
 * dedicated thread, its own lifecycle functions, main() only
 * orchestrating).
 * ====================================================================== */

#include "OCI_Connection.h"   /* oci_context_t */
#include "ini_reader.h"       /* app_config_t  */
#include "queue_manager.h"

typedef struct file_consumer_runner file_consumer_runner_t;   /* opaque */

/*
 * file_consumer_runner_start()
 *
 * Starts File Consumer's dedicated thread. ctx must already be fully
 * set up (connected, loggers initialised) - the thread logs via
 * ctx->file_consumer_logger like file_consumer_scan_once() always has.
 * qm must already exist (same queue_manager_t the worker pool is
 * draining) and outlive the returned runner.
 *
 * The thread loops: call file_consumer_scan_once(), then sleep for
 * config->dispatcher_poll_interval_seconds (checked in 1-second
 * increments so a stop request lands within at most a second, not
 * delayed by a long sleep). Stops on its own once
 * config->dispatcher_lifetime_seconds have elapsed since start (0 =
 * never stops on its own - runs until
 * file_consumer_runner_stop_and_join() is called, i.e. a real
 * long-running service).
 *
 * Returns NULL on allocation or thread-start failure.
 */
file_consumer_runner_t *file_consumer_runner_start(oci_context_t   *ctx,
                                                     app_config_t    *config,
                                                     queue_manager_t *qm);

/*
 * file_consumer_runner_stop_and_join()
 *
 * Requests the thread stop (checked within at most 1 second, per the
 * sleep-in-increments design above) and blocks until it exits. Safe to
 * call even if the thread already stopped on its own (lifetime
 * elapsed) - it will just join immediately. Safe to call with NULL.
 *
 * Only call this when something has actually decided the thread
 * should stop (e.g. a future SIGINT/SIGTERM handler) - see
 * file_consumer_runner_join() below for the "just wait for it to
 * finish on its own" case, which is what most callers want today.
 */
void file_consumer_runner_stop_and_join(file_consumer_runner_t *runner);

/*
 * file_consumer_runner_join()
 *
 * Blocks until the thread exits on its own - i.e. once
 * dispatcher.lifetime_seconds have elapsed (0 means it never will on
 * its own, so this blocks forever, which is the correct "run until
 * externally killed" behaviour for a real long-running service - no
 * signal handling is wired up yet, so today that means an actual
 * process kill, not a graceful in-process stop). Does NOT request a
 * stop - this is the "just wait for it to finish" counterpart to
 * stop_and_join() above, and is what main() should call in the
 * ordinary case where nothing has decided to stop the run early.
 * Safe to call with NULL.
 */
void file_consumer_runner_join(file_consumer_runner_t *runner);

#endif /* FILE_CONSUMER_RUNNER_H */
