#ifndef RESPONSE_MANAGER_H
#define RESPONSE_MANAGER_H

/* ======================================================================
 * response_manager.h
 *
 * Stage 3 (File_Consumer_proposal v1.2) - takes a populated
 * response_object_t (from process_xml_file(), dispatcher.c) and
 * writes it to disk: Output_* on PASS, Error_* on ERROR. Also moves
 * the original input file (currently sitting in Processing_*)
 * alongside the response it just wrote, so Processing_* is transient
 * rather than an ever-growing pile - per Terry's call on where the
 * original file ends up (2026-08-04).
 *
 * Response filename convention: <original_filename>.response.xml (or
 * .json) - keeps the original input's name recognisable next to its
 * response rather than reusing the exact same filename, which would
 * either collide with the moved-original or force choosing between
 * request/response content under one name.
 * ====================================================================== */

#include "OCI_Connection.h"    /* oci_context_t */
#include "response_object.h"   /* response_object_t */

/*
 * response_manager_write()
 *
 * ctx              - for logging (writes to ctx->file_consumer_logger,
 *                     since this is called from file_consumer.c and
 *                     dispatcher_logger is dispatch-specific)
 * resp             - populated response_object_t from process_xml_file().
 *                     Not modified or freed here - caller still owns it
 *                     and is still responsible for response_object_free().
 * original_filename - bare filename (no directory), e.g. "Foo.xml"
 * processing_path   - full current path of the original input file
 *                     (sitting in Processing_*)
 * output_dir       - Output_XML or Output_JSON, matching resp->is_json
 * error_dir        - Error_XML or Error_JSON, matching resp->is_json
 *
 * On PASS: writes resp->response_body to
 *   <output_dir>/<original_filename>.response.(xml|json)
 * and moves the original from processing_path to
 *   <output_dir>/<original_filename>
 *
 * On ERROR: same, but under error_dir instead of output_dir.
 *
 * Returns 0 if both the response write and the original-file move
 * succeeded, -1 otherwise (logged either way - a -1 here is a
 * Response-Manager-level problem, e.g. a full disk or bad permissions
 * on the output directory, distinct from resp->status which reflects
 * the dispatch outcome itself).
 */
int response_manager_write(oci_context_t      *ctx,
                            response_object_t  *resp,
                            const char         *original_filename,
                            const char         *processing_path,
                            const char         *output_dir,
                            const char         *error_dir);

#endif /* RESPONSE_MANAGER_H */
