#ifndef FILE_CONSUMER_H
#define FILE_CONSUMER_H

/* ======================================================================
 * file_consumer.h
 *
 * Stage 2 (File_Consumer_proposal v1.2) - single-threaded File Consumer.
 * Scans file_consumer_input_xml_dir / file_consumer_input_json_dir,
 * moves each file it finds into the matching Processing_* folder, then
 * calls process_xml_file() (dispatcher.h) directly - no queue, no
 * worker threads, no Dispatcher round-robin yet. Those come in Stages
 * 4-5 once this stage is proven correct on its own.
 *
 * Stage 3 update: process_xml_file() now hands back a populated
 * response_object_t instead of just a pass/fail int, and
 * response_manager_write() (response_manager.h) writes that response
 * to Output_* / Error_* and moves the original file there alongside it
 * - Processing_* is transient now rather than an ever-growing pile
 * (Terry's call, 2026-08-04).
 *
 * Logs to ctx->file_consumer_logger (its own dedicated log file,
 * file_consumer_log_file_name in config.ini) rather than borrowing
 * connectionpool_logger as it did at first.
 * ====================================================================== */

#include "OCI_Connection.h"   /* oci_context_t */
#include "ini_reader.h"       /* app_config_t  */

/*
 * file_consumer_run_once()
 *
 * One pass over both input directories: scan, move each file found to
 * Processing, dispatch it, log the result, move on. Does not loop or
 * sleep - that's a decision for whatever calls this (continuous
 * polling loop vs. single invocation) and is intentionally left to
 * main() rather than baked in here.
 *
 * Returns the total number of files dispatched (>= 0, files skipped
 * for being zero-length or non-regular don't count), or -1 if BOTH
 * input directories failed to open (a config-level problem - wrong
 * path in consumer_file.ini, permissions, etc.). If only one of the
 * two directories fails to open, that failure is logged and the other
 * directory is still processed normally.
 */
int file_consumer_run_once(oci_context_t *ctx, app_config_t *config);

#endif /* FILE_CONSUMER_H */
