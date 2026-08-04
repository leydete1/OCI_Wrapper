/* ======================================================================
 * response_manager.c
 *
 * Stage 3 (File_Consumer_proposal v1.2). See response_manager.h for
 * the full contract. Deliberately simple/single-threaded - same
 * "prove it works before adding concurrency" approach as every other
 * stage so far.
 * ====================================================================== */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "response_manager.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  write_response_file()                                              */
/*  Writes body to <dir>/<filename>.response.<ext>. Returns 0 on        */
/*  success, -1 on failure (open/write/close error - all logged here). */
/* ------------------------------------------------------------------ */
static int write_response_file(oci_context_t *ctx,
                                const char    *dir,
                                const char    *original_filename,
                                const char    *body,
                                int            is_json)
{
    char response_path[1200];
    int n = snprintf(response_path, sizeof(response_path), "%s/%s.response.%s",
                      dir, original_filename, is_json ? "json" : "xml");
    if (n < 0 || (size_t)n >= sizeof(response_path))
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "Response Manager: path too long for '%s' in '%s'",
                     original_filename, dir);
        return -1;
    }

    FILE *fp = fopen(response_path, "w");
    if (!fp)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "Response Manager: failed to open '%s' for writing (%s)",
                     response_path, strerror(errno));
        return -1;
    }

    size_t len = strlen(body);
    size_t written = fwrite(body, 1, len, fp);
    int close_rc = fclose(fp);

    if (written != len)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "Response Manager: short write to '%s' (%zu of %zu bytes)",
                     response_path, written, len);
        return -1;
    }
    if (close_rc != 0)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "Response Manager: fclose() failed for '%s' (%s)",
                     response_path, strerror(errno));
        return -1;
    }

    logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                 "Response Manager: wrote response '%s' (%zu bytes)",
                 response_path, len);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  move_original_file()                                               */
/*  Moves processing_path to <dir>/<original_filename>.                */
/* ------------------------------------------------------------------ */
static int move_original_file(oci_context_t *ctx,
                               const char    *processing_path,
                               const char    *dir,
                               const char    *original_filename)
{
    char dest_path[1200];
    int n = snprintf(dest_path, sizeof(dest_path), "%s/%s", dir, original_filename);
    if (n < 0 || (size_t)n >= sizeof(dest_path))
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "Response Manager: destination path too long for '%s' in '%s'",
                     original_filename, dir);
        return -1;
    }

    if (rename(processing_path, dest_path) != 0)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "Response Manager: failed to move '%s' to '%s' (%s) - "
                     "original file left in Processing_*",
                     processing_path, dest_path, strerror(errno));
        return -1;
    }

    logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                 "Response Manager: moved original '%s' to '%s'",
                 processing_path, dest_path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  response_manager_write                                              */
/* ------------------------------------------------------------------ */
int response_manager_write(oci_context_t      *ctx,
                            response_object_t  *resp,
                            const char         *original_filename,
                            const char         *processing_path,
                            const char         *output_dir,
                            const char         *error_dir)
{
    const char *dest_dir = (resp->status == RESPONSE_STATUS_PASS)
                            ? output_dir : error_dir;

    if (!resp->response_body)
    {
        /* Shouldn't happen - process_xml_file()'s contract guarantees
         * response_body is always non-NULL on return - but don't write
         * a NULL through fwrite() if something upstream ever slips.   */
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "Response Manager: resp->response_body is NULL for '%s' - "
                     "this violates process_xml_file()'s contract, nothing written",
                     original_filename);
        return -1;
    }

    int write_rc = write_response_file(ctx, dest_dir, original_filename,
                                        resp->response_body, resp->is_json);

    /* Move the original even if the response write failed - an
     * accumulating Processing_* directory is worse than a moved
     * original with a missing response file, and the write failure is
     * already logged above with enough detail to investigate.          */
    int move_rc = move_original_file(ctx, processing_path, dest_dir, original_filename);

    return (write_rc == 0 && move_rc == 0) ? 0 : -1;
}
