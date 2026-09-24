#ifndef HTTP_CONSUMER_RUNNER_H
#define HTTP_CONSUMER_RUNNER_H

/* ======================================================================
 * http_consumer_runner.h
 *
 * Owns the libmicrohttpd daemon's lifecycle, same relationship
 * file_consumer_runner.h has to file_consumer.h.
 *
 * TLS is mandatory - see http_consumer_runner_start()'s own doc
 * comment, unchanged from Stage 0.
 *
 * Lifetime decoupling (2026-08-16): previously HTTP consumer only
 * stayed alive because it happened to be started inside the same
 * main() block as File Consumer, and only kept the process alive at
 * all via File Consumer's own file_consumer_runner_join() blocking
 * forever. That coupling is gone. HTTP consumer now owns its own
 * lifetime, read from consumer_http.ini's http_dispatcher.
 * lifetime_seconds (config->http_dispatcher_lifetime_seconds) - 0
 * means forever (external kill only, same convention as File
 * Consumer's own dispatcher.lifetime_seconds), > 0 means it stops
 * itself automatically after that many seconds.
 *
 * This is implemented as a small dedicated "lifetime watchdog" thread,
 * started alongside the MHD daemon:
 *   - lifetime_seconds > 0: sleeps that long, then stops the daemon
 *     itself and returns.
 *   - lifetime_seconds == 0: blocks indefinitely and never returns
 *     under normal operation - external process kill only, exactly
 *     mirroring File Consumer's own forever-case.
 *
 * http_consumer_runner_join() blocks on that thread - same role in
 * main() that file_consumer_runner_join() plays for File Consumer:
 * call it where you want main() to block until this consumer's
 * lifetime ends (which, for lifetime_seconds==0, is "block forever,
 * same as File Consumer's own equivalent setting").
 *
 * http_consumer_runner_stop() is idempotent with respect to the
 * underlying MHD_stop_daemon() call - safe to call after
 * http_consumer_runner_join() returns even though the watchdog thread
 * may have already stopped the daemon itself in the lifetime_seconds>0
 * case. It always still frees the runner's own resources, regardless
 * of who actually triggered the MHD-level stop.
 *
 * Worker pool ownership (Stage 4, 2026-08-20): http_consumer_runner_
 * start() also creates the http_worker_pool_t (http_worker_pool.h) and
 * hands it to the request handler via http_consumer_ctx_t.pool -
 * created BEFORE MHD_start_daemon() so no connection can ever be
 * accepted before the pool exists to service it. http_consumer_runner_
 * stop() stops the pool AFTER MHD_stop_daemon() has already returned -
 * by that point every in-flight request has already received its
 * response via the completion signal, so this is pure idle-worker
 * cleanup, not a race with anything still in flight.
 * ====================================================================== */

#include "OCI_Connection.h"   /* oci_context_t */
#include "ini_reader.h"       /* app_config_t  */

typedef struct http_consumer_runner http_consumer_runner_t;   /* opaque */

/*
 * http_consumer_runner_start()
 *
 * Loads TLS cert/key from config->http_consumer_tls_cert_file /
 * _tls_key_file (mandatory, no fallback - see .c for details), starts
 * an MHD daemon bound to config->http_consumer_bind_address:
 * http_consumer_port with config->http_consumer_thread_pool_size
 * internal worker threads, and starts the lifetime watchdog thread
 * per config->http_dispatcher_lifetime_seconds (loaded from
 * consumer_http.ini via load_http_consumer_ini() - see ini_reader.h).
 *
 * ctx and config must both outlive the returned runner, and
 * ctx->http_consumer_logger must already be initialised.
 *
 * Returns NULL - with the reason logged via ctx->http_consumer_logger
 * at LOG_ERROR - if TLS material can't be read, allocation fails, or
 * MHD_start_daemon() itself fails.
 */
http_consumer_runner_t *http_consumer_runner_start(oci_context_t *ctx,
                                                     app_config_t  *config);

/*
 * http_consumer_runner_join()
 *
 * Blocks until the lifetime watchdog thread exits - i.e. until
 * http_dispatcher.lifetime_seconds elapses (if > 0), or forever if it's
 * 0 (external kill only). Call this from main() exactly where
 * file_consumer_runner_join() would be called for File Consumer - it
 * plays the identical "keep main() alive for as long as this consumer
 * should run" role. Safe to call with NULL (returns immediately).
 */
void http_consumer_runner_join(http_consumer_runner_t *runner);

/*
 * http_consumer_runner_stop()
 *
 * Stops the MHD daemon if it isn't already stopped (idempotent - safe
 * to call even after the lifetime watchdog already self-stopped it),
 * blocking until all in-flight connections drain, then always frees
 * the runner and its held TLS cert/key buffers regardless. Safe to
 * call with NULL.
 */
void http_consumer_runner_stop(http_consumer_runner_t *runner);

#endif /* HTTP_CONSUMER_RUNNER_H */
