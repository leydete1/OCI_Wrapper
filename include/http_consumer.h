#ifndef HTTP_CONSUMER_H
#define HTTP_CONSUMER_H

/* ======================================================================
 * http_consumer.h
 *
 * Stage 4 (2026-08-20) - CRUD dispatch now routes through
 * http_worker_pool_dispatch() (http_worker_pool.h) instead of calling
 * process_xml_file() directly on the MHD handler thread. Every INSERT/
 * UPDATE/DELETE (and any mixed transaction containing one) goes to
 * queue 0, the dedicated single-writer queue - one connection, exactly
 * like File Consumer's contention_manager.mode=single_write_queue.
 * SELECT/EXECUTE_PROCEDURE round-robin across the remaining queues -
 * NOT a separate synchronous fast-path, the same queued path as
 * writes, per Terry's explicit decision. The handler thread blocks on
 * the dispatch call until a worker signals completion, then writes
 * whatever it got back - PASS or ERROR, doesn't matter, same contract
 * process_xml_file() always had.
 *
 * Session lifecycle (CREATE_SESSION/END_SESSION) is UNCHANGED from
 * Stage 2/3 - handle_session_request() still borrows its own session
 * per-request and calls session_create()/session_end() directly,
 * bypassing the worker pool entirely. Nothing about session lifecycle
 * touches contention-prone tables, so there's nothing to gain by
 * queueing it.
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
#include "request_object.h"      /* request_object_t - Stage 4 */
#include "http_worker_pool.h"    /* http_worker_pool_t, dispatch - Stage 4 */

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
 * The cls closure handed to every MHD callback.
 *
 * pool added Stage 4 (2026-08-20) - the CRUD dispatch path routes
 * through it now (http_worker_pool_dispatch()) instead of calling
 * process_xml_file() directly on the MHD handler thread. Owned/started
 * by http_consumer_runner_start(), stopped by http_consumer_runner_
 * stop() - see http_worker_pool.h for the full design. Session
 * requests (CREATE_SESSION/END_SESSION) do NOT use pool - see
 * handle_session_request() in the .c file, unchanged from Stage 2/3.
 */
typedef struct {
    oci_context_t       *ctx;
    app_config_t         *config;
    http_worker_pool_t  *pool;
} http_consumer_ctx_t;

#endif /* HTTP_CONSUMER_H */
