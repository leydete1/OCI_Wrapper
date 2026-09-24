#ifndef HTTP_CONSUMER_H
#define HTTP_CONSUMER_H

/* ======================================================================
 * http_consumer.h
 *
 * Stage 0 - bare listener, no business logic.
 *
 * Per the staged plan (2026-08-14): this stage proves the MHD_daemon
 * start/stop lifecycle, TLS enforcement, and thread pool sizing only.
 * It does NOT parse the request envelope, does NOT call Level 1/Level 2,
 * and does NOT enqueue via queue_manager - that's Stage 2 onward, once
 * this bare skeleton is proven stable. The single handler here logs
 * whatever body arrives via ctx->http_consumer_logger and returns a
 * static 200 OK, exactly like file_consumer_scan_once() logging a scan
 * pass before any dispatcher wiring existed.
 *
 * TLS is non-negotiable (Terry, 2026-08-14): http_consumer_runner_start()
 * refuses to start the daemon at all if the cert/key can't be loaded.
 * There is no plaintext fallback path anywhere in this module.
 *
 * ---------------------------------------------------------------------
 * REQUIRED WIRING - not part of this file, do before building:
 *
 * 1. oci_context_t (OCI_Connection.h) needs a new field, alongside the
 *    existing file_consumer_logger:
 *
 *        logger_t *http_consumer_logger;
 *
 * 2. app_config_t (ini_reader.h) needs a new block, alongside the
 *    existing file_consumer_log_* fields:
 *
 *        int  http_consumer_port;
 *        char http_consumer_bind_address[64];
 *        int  http_consumer_thread_pool_size;
 *        char http_consumer_tls_cert_file[256];
 *        char http_consumer_tls_key_file[256];
 *        char http_consumer_log_file_name[256];
 *        int  http_consumer_log_file_max_size;
 *        int  http_consumer_log_file_rotation_number;
 *        char http_consumer_log_level[20];
 *
 *    ...plus the matching load_ini()/ini_reader.c parsing lines and a
 *    [http_consumer] section in config.ini, e.g.:
 *
 *        [http_consumer]
 *        http_consumer_port=8443
 *        http_consumer_bind_address=0.0.0.0
 *        http_consumer_thread_pool_size=8
 *        http_consumer_tls_cert_file=Props/http_consumer_cert.pem
 *        http_consumer_tls_key_file=Props/http_consumer_key.pem
 *        http_consumer_log_file_name=logs/http_consumer_Data_Manager.log
 *        http_consumer_log_file_max_size=5242880
 *        http_consumer_log_file_rotation_number=5
 *        http_consumer_log_level=INFO
 *
 * 3. Data_Manager_Bootstrap.c's initialise_loggers() needs a new
 *    logger_t http_consumer_logger local + logger_init_str() call,
 *    same pattern as every other per-subsystem logger there.
 *
 * 4. For local/dev testing, a self-signed cert is enough:
 *
 *        openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
 *          -keyout Props/http_consumer_key.pem \
 *          -out    Props/http_consumer_cert.pem \
 *          -subj   "/CN=localhost"
 *
 *    A real certificate is a pre-req for anything beyond local testing -
 *    tracked separately, not a Stage 0 concern.
 * ====================================================================== */

#include <microhttpd.h>

#include "OCI_Connection.h"   /* oci_context_t */
#include "ini_reader.h"       /* app_config_t  */

/*
 * http_consumer_handle_request()
 *
 * MHD_AccessHandlerCallback. Passed to MHD_start_daemon() as the
 * request handler; cls is the http_consumer_ctx_t* set up by
 * http_consumer_runner_start() (see http_consumer_runner.h).
 *
 * Stage 0 behaviour only:
 *   - Non-POST methods get 405 Method Not Allowed, logged at WARN.
 *   - POST body is accumulated across MHD's incremental calls (see
 *     http_consumer.c's own doc comment on why this needs con_cls),
 *     then logged in full at INFO once fully received.
 *   - Every request gets a static 200 OK ("Data Manager HTTP Consumer:
 *     Stage 0 - listener alive") regardless of body content - no
 *     parsing, no dispatch, nothing envelope-aware yet.
 */
enum MHD_Result http_consumer_handle_request(void *cls,
                                              struct MHD_Connection *connection,
                                              const char *url,
                                              const char *method,
                                              const char *version,
                                              const char *upload_data,
                                              size_t *upload_data_size,
                                              void **con_cls);

/*
 * http_consumer_request_completed()
 *
 * MHD_RequestCompletedCallback. Frees the per-connection accumulation
 * buffer allocated on first call to http_consumer_handle_request()
 * above. Passed to MHD_start_daemon() via MHD_OPTION_NOTIFY_COMPLETED.
 * Required - without this, every connection leaks its buffer.
 */
void http_consumer_request_completed(void *cls,
                                      struct MHD_Connection *connection,
                                      void **con_cls,
                                      enum MHD_RequestTerminationCode toe);

/*
 * http_consumer_ctx_t
 *
 * The cls closure handed to every MHD callback. Stage 0 only needs
 * ctx (for the logger) and config (for nothing yet, but every later
 * stage will need it here too - e.g. max body size, execute_async
 * defaults - so it's included now rather than threaded through later).
 */
typedef struct {
    oci_context_t *ctx;
    app_config_t  *config;
} http_consumer_ctx_t;

#endif /* HTTP_CONSUMER_H */
