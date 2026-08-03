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
 * Behavior is unchanged from the version in Test_XML_Runner.c - this is
 * purely a file split, verified line-for-line against the original
 * before being wired in. See File_Consumer_Implementation_Plan.md,
 * Stage 1.
 * ====================================================================== */

#include "OCI_Connection.h"   /* oci_context_t */

/*
 * process_xml_file()
 *
 * Reads filepath, detects new-format vs legacy flat-XML, and dispatches
 * to the correct handler (level1/level2 pipeline for new-format
 * SELECT/INSERT/UPDATE/DELETE/EXECUTE_PROCEDURE; legacy dispatch_select()
 * for old-format SELECT; INSERT/UPDATE/DELETE/EXECUTE_PROCEDURE in
 * legacy flat-XML format are no longer supported and log an error).
 *
 * Returns 0 on success, -1 on failure. Exported (was static in
 * Test_XML_Runner.c) so both the existing test-runner main() and the
 * new File Consumer (Stage 2) can call it directly.
 */
int process_xml_file(oci_context_t *ctx,
                      const char    *filepath,
                      const char    *filename);

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
