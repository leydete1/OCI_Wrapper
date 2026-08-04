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
 * Reads filepath, detects new-format vs legacy flat-XML, and dispatches
 * to the correct handler (level1/level2 pipeline for new-format
 * SELECT/INSERT/UPDATE/DELETE/EXECUTE_PROCEDURE; legacy dispatch_select()
 * for old-format SELECT; INSERT/UPDATE/DELETE/EXECUTE_PROCEDURE in
 * legacy flat-XML format are no longer supported and log an error).
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
 * Exported (was static in Test_XML_Runner.c) so both the existing
 * test-runner main() and the File Consumer can call it directly.
 */
int process_xml_file(oci_context_t      *ctx,
                      const char         *filepath,
                      const char         *filename,
                      response_object_t  *resp);

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
