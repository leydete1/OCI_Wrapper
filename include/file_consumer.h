#ifndef FILE_CONSUMER_H
#define FILE_CONSUMER_H

/* ======================================================================
 * file_consumer.h
 *
 * Stage 2 (File_Consumer_proposal v1.2) - single-threaded File Consumer.
 * Scans file_consumer_input_xml_dir / file_consumer_input_json_dir.
 *
 * Stage 3 update: process_xml_file() hands back a populated
 * response_object_t, and response_manager_write() (response_manager.h)
 * writes it to Output_* / Error_* and moves the original file there
 * alongside it.
 *
 * Stage 4 update: File Consumer now reads each file's payload itself
 * and round-robin enqueues a RequestObject (request_object.h) via
 * queue_manager, rather than calling process_xml_file() directly - a
 * single synchronous worker (worker.h) drains everything once the scan
 * is done. Queue-Full rejections and read failures both skip
 * Processing_* entirely and go straight from Input_* to Error_*, per
 * the Queue-Full Behavior addendum. Still no real threads - that's
 * Stage 5.
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
