#ifndef HTTP_CONSUMER_RUNNER_H
#define HTTP_CONSUMER_RUNNER_H

/* ======================================================================
 * http_consumer_runner.h
 *
 * Owns the libmicrohttpd daemon's lifecycle, same relationship
 * file_consumer_runner.h has to file_consumer.h: this module starts/
 * stops the listener, http_consumer.c owns what happens per request.
 * main() (Data_Manager_Bootstrap.c) only orchestrates - create, start,
 * eventually stop - never touches MHD directly.
 *
 * Unlike file_consumer_runner.h, this module does NOT manage its own
 * pthread - libmicrohttpd's internal thread pool (MHD_USE_INTERNAL_
 * POLLING_THREAD + MHD_OPTION_THREAD_POOL_SIZE) does that. There is
 * nothing here to "join" the way file_consumer_runner_join() waits on
 * a self-managed thread; http_consumer_runner_stop() is synchronous -
 * MHD_stop_daemon() itself blocks until every in-flight connection
 * drains and all of MHD's internal threads have exited, which is the
 * natural equivalent.
 *
 * TLS is mandatory, no exceptions (Terry, 2026-08-14):
 * http_consumer_runner_start() will refuse to start - returning NULL,
 * with a clear log entry explaining why - if the configured cert/key
 * files cannot be read. There is no MHD_USE_TLS-less code path in this
 * module at all, so a future change can't accidentally introduce a
 * plaintext listener by omission.
 * ====================================================================== */

#include "OCI_Connection.h"   /* oci_context_t */
#include "ini_reader.h"       /* app_config_t  */

typedef struct http_consumer_runner http_consumer_runner_t;   /* opaque */

/*
 * http_consumer_runner_start()
 *
 * Loads the TLS cert/key named by config->http_consumer_tls_cert_file /
 * http_consumer_tls_key_file, then starts an MHD daemon bound to
 * config->http_consumer_bind_address:http_consumer_port with
 * config->http_consumer_thread_pool_size internal worker threads.
 *
 * ctx must already be fully set up (connected, loggers initialised,
 * including the new ctx->http_consumer_logger - see http_consumer.h's
 * wiring checklist). ctx and config must both outlive the returned
 * runner.
 *
 * Returns NULL, with the reason logged via ctx->http_consumer_logger
 * at LOG_ERROR, if:
 *   - either TLS file can't be read,
 *   - allocation fails, or
 *   - MHD_start_daemon() itself fails (e.g. port already in use).
 *
 * There is no fallback to a non-TLS listener under any of these
 * failure conditions - a Stage 0 HTTP consumer that can't start with
 * TLS simply does not start.
 */
http_consumer_runner_t *http_consumer_runner_start(oci_context_t *ctx,
                                                     app_config_t  *config);

/*
 * http_consumer_runner_stop()
 *
 * Stops the MHD daemon (MHD_stop_daemon() - blocks until all in-flight
 * connections finish and MHD's internal threads exit), frees the
 * runner and its held TLS cert/key buffers. Safe to call with NULL.
 */
void http_consumer_runner_stop(http_consumer_runner_t *runner);

#endif /* HTTP_CONSUMER_RUNNER_H */
