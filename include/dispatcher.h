#ifndef DISPATCHER_H
#define DISPATCHER_H

/* ======================================================================
 * dispatcher.h
 *
 * Stage 1 extraction (File_Consumer_proposal v1.2) - process_xml_file()
 * and its five dispatch_*_new() handlers, pulled out of
 * Test_XML_Runner.c so the File Consumer (and later HTTP/MQ consumers)
 * can call the same dispatch logic without depending on the test
 * runner's main() or its startup self-test code.
 *
 * Stage 3 update: process_xml_file() now takes a response_object_t*
 * out-param. Previously the real XML/JSON result body was built,
 * logged, and immediately freed inside each dispatch_*_new() - Stage 3
 * needs that content to survive the call so the Response Manager can
 * actually write it to Output_* / Error_*. See response_object.h for the
 * full shape and an honest note on error-path content availability.
 *
 * Stage 4 update: process_xml_file() no longer touches the filesystem
 * itself - it now takes an already-read payload/payload_length instead
 * of a filepath, per the Payload Ownership addendum (File Consumer
 * alone reads files; the Dispatcher/queue/Worker layer works only with
 * the payload it's handed). read_file() itself is unchanged and still
 * exported (File Consumer calls it directly now to build each
 * RequestObject, and dispatch_create_session() in Test_XML_Runner.c
 * still needs it too).
 *
 * Logs to ctx->dispatcher_logger (its own dedicated log file,
 * dispatcher_log_file_name in config.ini) rather than borrowing
 * connectionpool_logger as it did at first - added post-Stage-2 so
 * dispatch activity is easy to isolate for review.
 * ====================================================================== */

#include "OCI_Connection.h"    /* oci_context_t */
#include "response_object.h"   /* response_object_t */

/*
 * process_xml_file()
 *
 * Takes an already-read payload (NUL-terminated, payload_length bytes
 * not counting the terminator - same convention read_file() already
 * used), detects new-format vs legacy flat-XML, and dispatches to the
 * correct handler (level1/level2 pipeline for new-format
 * SELECT/INSERT/UPDATE/DELETE/EXECUTE_PROCEDURE; legacy dispatch_select()
 * for old-format SELECT; INSERT/UPDATE/DELETE/EXECUTE_PROCEDURE in
 * legacy flat-XML format are no longer supported and log an error).
 * filename is metadata only (used in logging and in the response
 * envelope), not looked up on disk - this function never opens a file.
 *
 * resp must point to a response_object_t the caller has already run
 * through response_object_init() (a stack local is fine). On return,
 * resp->response_body is always non-NULL - a real result body on PASS,
 * a synthesized error envelope on ERROR - and it's caller-owned: call
 * response_object_free(resp) once you're done with it (after the
 * Response Manager has written it out, typically).
 *
 * Returns 0 on success, -1 on failure - same as before Stage 3; the
 * return code and resp->status always agree, the return code just
 * saves a caller that doesn't care about the response body from having
 * to inspect the struct.
 *
 * session_id_override (Session Manager proposal, Stage 1, 2026-08-06):
 * if non-NULL and non-empty, overwrites the parsed request's own
 * session_id field right after Level 1 parsing succeeds - this is what
 * lets File Consumer's real, cache-registered session become the
 * session an eventual Stage 3 validation check actually validates,
 * rather than whatever placeholder ("-", almost always) the raw
 * payload itself happened to carry. Pass NULL for callers that don't
 * participate in this yet (e.g. the legacy Test_XML_Runner fixture
 * harness) - the parsed request's own session_id is left untouched in
 * that case, identical to this function's behaviour before this stage.
 *
 * Exported (was static in Test_XML_Runner.c) so both the existing
 * test-runner main() and worker.c (via the queue) can call it directly.
 */
int process_xml_file(oci_context_t      *ctx,
                      const char         *payload,
                      long                payload_length,
                      const char         *filename,
                      const char         *session_id_override,
                      response_object_t  *resp);

/*
 * build_error_envelope()
 *
 * Synthesizes resp->response_body as a generic error envelope (XML or
 * JSON per is_json), setting resp->status = RESPONSE_STATUS_ERROR and
 * the audit_id/operation/error_code/error_text fields. Exported
 * (originally private to dispatcher.c) specifically so file_consumer.c
 * can build a QUEUE_FULL response (Stage 4) without duplicating the
 * XML/JSON escaping logic - see the Queue-Full Behavior addendum,
 * File_Consumer_proposal v1.2.
 */
void build_error_envelope(response_object_t *resp,
                           const char         *audit_id,
                           const char         *operation,
                           const char         *error_code,
                           const char         *error_text,
                           int                 is_json);

/*
 * read_file()
 *
 * Reads the full contents of path into a heap-allocated, NUL-terminated
 * buffer (caller must free()). Returns NULL on error (missing file,
 * zero-length, or larger than MAX_XML_FILE_SIZE).
 *
 * Exported (was static in Test_XML_Runner.c) because
 * dispatch_create_session() in Test_XML_Runner.c's main() flow also
 * calls it for session-request files - that call site stays in
 * Test_XML_Runner.c since session bootstrap isn't part of the file
 * dispatch pipeline, so this declaration is what keeps it working
 * post-extraction instead of needing a duplicate copy.
 */
char *read_file(const char *path, long *out_len);

#endif /* DISPATCHER_H */
