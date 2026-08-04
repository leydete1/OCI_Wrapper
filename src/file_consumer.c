/* ======================================================================
 * file_consumer.c
 *
 * Stage 2 (File_Consumer_proposal v1.2).
 *
 * Deliberately minimal for this stage: no threads, no queues, no
 * Dispatcher round-robin - just prove the scan -> validate -> move ->
 * dispatch loop is correct on its own before Stage 4 wraps it in
 * concurrency. Per the plan's own reasoning (see
 * File_Consumer_Implementation_Plan.md), every stage gets proven
 * single-threaded first.
 *
 * What "validate" means at this stage: a lightweight stat()-based
 * check (regular file, non-zero size) before the move to Processing.
 * Deeper read failures (permissions, file vanished, truncated writes
 * between the stat() and the actual read) are still caught - just one
 * layer down, inside process_xml_file()'s own read_file() call, which
 * already logs a clear FAIL and returns -1. This stage doesn't
 * duplicate that check; it just needs to notice the -1 and log it as a
 * File Consumer-level outcome too.
 *
 * What's explicitly NOT here yet: moving the file on to Output_* or
 * Error_* based on the dispatch result. Per the Payload Ownership and
 * Queue-Full Behavior addenda (both merged into the proposal, v1.2),
 * a failed request needs an actual ResponseObject with status=ERROR
 * written to Error_* - not just the original input file relocated
 * there unexplained. That machinery is ResponseObject + Response
 * Manager, which is Stage 3. Until then, dispatched files - pass or
 * fail - are left sitting in Processing_*, and the outcome is only
 * visible in the log. Terry: worth knowing before running this against
 * a real inbox, since Processing_* will just accumulate until Stage 3
 * lands.
 * ====================================================================== */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "file_consumer.h"
#include "dispatcher.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  process_directory                                                   */
/*  One input directory's worth of work: scan, validate, move to        */
/*  Processing, dispatch. Returns count dispatched, or -1 if input_dir  */
/*  itself couldn't be opened at all.                                   */
/* ------------------------------------------------------------------ */
static int process_directory(oci_context_t *ctx,
                              const char    *input_dir,
                              const char    *processing_dir,
                              const char    *format_label)
{
    DIR *dir = opendir(input_dir);
    if (!dir)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "File Consumer: cannot open %s input directory '%s' "
                     "(%s)", format_label, input_dir, strerror(errno));
        return -1;
    }

    int processed = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        const char *name = entry->d_name;

        /* Skip ".", "..", and hidden/editor-temp files (".foo.xml~"
         * style) rather than trying to dispatch them and logging a
         * confusing failure. */
        if (name[0] == '.') continue;

        char input_path[1024];
        int n = snprintf(input_path, sizeof(input_path), "%s/%s",
                          input_dir, name);
        if (n < 0 || (size_t)n >= sizeof(input_path))
        {
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer: path too long, skipping '%s/%s'",
                         input_dir, name);
            continue;
        }

        struct stat st;
        if (stat(input_path, &st) != 0)
        {
            logger_write(ctx->file_consumer_logger, LOG_WARN, __func__, 0,
                         "File Consumer: stat() failed for '%s' (%s) - "
                         "skipping, will retry next pass", input_path,
                         strerror(errno));
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;   /* subdirectories etc. - silently skip, not an error */

        if (st.st_size == 0)
        {
            logger_write(ctx->file_consumer_logger, LOG_WARN, __func__, 0,
                         "File Consumer: skipping zero-length file '%s'",
                         input_path);
            continue;
        }

        char processing_path[1024];
        n = snprintf(processing_path, sizeof(processing_path), "%s/%s",
                     processing_dir, name);
        if (n < 0 || (size_t)n >= sizeof(processing_path))
        {
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer: processing path too long, "
                         "skipping '%s'", name);
            continue;
        }

        if (rename(input_path, processing_path) != 0)
        {
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer: failed to move '%s' to '%s' (%s) "
                         "- left in place, will retry next pass",
                         input_path, processing_path, strerror(errno));
            continue;
        }

        logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                     "File Consumer: dispatching %s file '%s'",
                     format_label, name);

        int rc = process_xml_file(ctx, processing_path, name);

        if (rc == 0)
        {
            logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                         "File Consumer: PASS '%s' (still in %s pending "
                         "Stage 3's Output/Error file writer)",
                         name, processing_dir);
        }
        else
        {
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer: FAIL '%s' (rc=%d, still in %s "
                         "pending Stage 3's Output/Error file writer)",
                         name, rc, processing_dir);
        }

        processed++;
    }

    closedir(dir);
    return processed;
}

/* ------------------------------------------------------------------ */
/*  file_consumer_run_once                                              */
/* ------------------------------------------------------------------ */
int file_consumer_run_once(oci_context_t *ctx, app_config_t *config)
{
    int xml_result  = process_directory(ctx,
                                         config->file_consumer_input_xml_dir,
                                         config->file_consumer_processing_xml_dir,
                                         "XML");
    int json_result = process_directory(ctx,
                                         config->file_consumer_input_json_dir,
                                         config->file_consumer_processing_json_dir,
                                         "JSON");

    if (xml_result < 0 && json_result < 0)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "File Consumer: both input directories failed to "
                     "open - check file_consumer.*_dir in consumer_file.ini");
        return -1;
    }

    int total = (xml_result  > 0 ? xml_result  : 0)
              + (json_result > 0 ? json_result : 0);

    return total;
}
