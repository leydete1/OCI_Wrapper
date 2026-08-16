#ifndef HTTP_CONSUMER_H
#define HTTP_CONSUMER_H

/* ======================================================================
 * http_consumer.h
 *
 * Stage 2 (2026-08-16) - real dispatch. Every fully-received POST body
 * now goes through process_xml_file() (dispatcher.c) - the same
 * reusable entrypoint File Consumer's worker.c calls - and the real
 * response body (PASS or ERROR envelope, XML or JSON per what
 * process_xml_file() itself detected) is written back as the HTTP
 * response.
 *
 * Session lifecycle (same day, 2026-08-16): CREATE_SESSION/END_SESSION
 * were never reachable through process_xml_file() at all - Level 2
 * explicitly rejects both (LEVEL2_ERR_NOT_IMPLEMENTED), and every
 * existing caller of session_create() (Data_Manager_Bootstrap.c's own
 * harness, file_consumer_runner.c's startup) calls it directly,
 * bypassing the whole request pipeline, since neither of them serves
 * an actual external client that needs its own session. HTTP consumer
 * is the first consumer type that genuinely does - see
 * OCI_Session_Manager.h's own note: "once the HTTP input module
 * replaces the XML test-file input, it will read the session_id from
 * the request and call session_validate() before dispatching to any
 * other module." This is that module. A <Session_Request> envelope
 * (distinct shape from the normal <request>/<operation type="...">
 * CRUD envelope) is sniffed before the normal dispatch path and routed
 * to session_create()/session_end() directly - see
 * handle_session_request() in the .c file.
 *
 * HTTP status codes stay purely transport-level (Terry, 2026-08-16):
 * every request that reaches process_xml_file() or the session handler
 * gets HTTP 200 regardless of whether Data Manager's own result was
 * PASS or ERROR - that result lives entirely inside the response
 * payload, which the caller opens to find out. Non-200 is reserved for
 * genuine HTTP/transport failures that never reach either handler at
 * all: 405 for non-POST, 413 for an oversized body, 503 if no session
 * is available to borrow. The app knows nothing about HTTP, by design -
 * it stays that way.
 *
 * Session handling on the CRUD path: borrowed per-request, not per-
 * thread. Every other long-lived-thread consumer in this codebase
 * (File Consumer, Session Manager, worker.c) borrows one session per
 * thread and reuses it - this handler can't do that cleanly because
 * MHD's internal thread pool is opaque to us (no "this pool thread
 * just started" hook to borrow against). Per-request borrow/release is
 * simpler and definitely correct; revisit only if the concurrency
 * stress-testing stage shows it's actually a bottleneck.
 *
 * TLS is non-negotiable (Terry, 2026-08-14): http_consumer_runner_start()
 * refuses to start the daemon at all if the cert/key can't be loaded.
 * There is no plaintext fallback path anywhere in this module.
 * ====================================================================== */

#include <microhttpd.h>

#include "OCI_Connection.h"      /* oci_context_t */
#include "ini_reader.h"          /* app_config_t  */
#include "OCI_Connection_Pool.h" /* OCI_Pool_get_session/_release_session */
#include "ctx_utils.h"           /* copy_shared_ctx_fields */
#include "dispatcher.h"          /* process_xml_file */
#include "response_object.h"     /* response_object_t */

/*
 * http_consumer_handle_request()
 *
 * MHD_AccessHandlerCallback. Passed to MHD_start_daemon() as the
 * request handler; cls is the http_consumer_ctx_t* set up by
 * http_consumer_runner_start() (see http_consumer_runner.h).
 *
 * Stage 2 behaviour:
 *   - Non-POST methods get 405 Method Not Allowed, logged at WARN.
 *   - POST body is accumulated across MHD's incremental calls (see
 *     http_consumer.c's own doc comment on why this needs con_cls).
 *   - Once fully received: borrows a session, calls process_xml_file()
 *     (same entrypoint File Consumer's worker.c uses), writes
 *     resp.response_body back as the HTTP response with a matching
 *     Content-Type (resp.is_json decides XML vs JSON), releases the
 *     session. Always HTTP 200 - see this header's own note above on
 *     why.
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
