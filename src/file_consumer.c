/* ======================================================================
 * file_consumer.c
 *
 * File Consumer: scan/validate/read loop. Reads each file's payload
 * itself (Payload Ownership addendum), builds a RequestObject, and
 * round-robin enqueues it via queue_manager.
 *
 * Stage 5 update: File Consumer no longer owns the queue_manager or
 * drains it - main() now creates the queue_manager once and starts a
 * long-running worker thread pool (worker.c) against it up front,
 * keeping both alive across many scan passes. file_consumer_scan_once()
 * just does its one job (scan and enqueue) against whatever
 * queue_manager it's handed; the worker pool drains concurrently in
 * the background. See file_consumer.h for the full reasoning on why
 * this split happened (thread startup cost).
 *
 * Queue-Full handling (Queue-Full Behavior addendum, v1.2): if every
 * queue is full, the file never moves to Processing_* at all - it's
 * rejected immediately with a QUEUE_FULL error response written
 * straight from Input_* to Error_*, matching the addendum's HTTP-503-
 * style "no blocking, no retry" behaviour. See queue_manager.h for the
 * exact round-robin/overflow policy.
 *
 * What "validate" means at this stage: a lightweight stat()-based
 * check (regular file, non-zero size), same as before. A read failure
 * after that point (permissions, file vanished, truncated write
 * between the stat() and the actual read) now gets its own
 * FILE_READ_FAILED error response, written straight to Error_* the
 * same way a QUEUE_FULL rejection is - both are File-Consumer-level
 * problems that never make it as far as the queue.
 * ====================================================================== */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "file_consumer.h"
#include "dispatcher.h"
#include "response_object.h"
#include "response_manager.h"
#include "request_object.h"
#include "queue_manager.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  reject_immediately()                                               */
/*  Shared by both rejection paths (read failure, queue full): builds  */
/*  an error response and routes it straight from wherever the file    */
/*  currently sits (still in Input_*, never moved) to error_dir,       */
/*  bypassing Processing_* entirely.                                   */
/* ------------------------------------------------------------------ */
static void reject_immediately(oci_context_t *ctx,
                                const char    *name,
                                const char    *input_path,
                                const char    *output_dir,
                                const char    *error_dir,
                                const char    *error_code,
                                const char    *error_text,
                                int            is_json)
{
    response_object_t resp;
    response_object_init(&resp);
    /* is_json comes from the caller, derived from which directory this
     * file was found in (Input_XML vs Input_JSON) - not from sniffing
     * content, since in the read-failure case there's no content to
     * sniff anyway. Bug fixed 2026-08-04: this used to hardcode 0
     * (XML) regardless of source, so JSON-sourced rejections landed in
     * Error_JSON as ".response.xml" - directory was already correct,
     * only the response's own format/extension was wrong. Terry
     * caught it by spotting XML-named files in Error_JSON.             */
    build_error_envelope(&resp, "-", "-", error_code, error_text, is_json);

    if (response_manager_write(ctx, &resp, name, input_path, output_dir, error_dir) != 0)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "File Consumer: Response Manager reported a problem "
                     "writing/moving '%s' during immediate rejection (%s)",
                     name, error_code);
    }

    response_object_free(&resp);
}

/* ------------------------------------------------------------------ */
/*  process_directory                                                   */
/*  One input directory's worth of work: scan, validate, read, enqueue  */
/*  (round-robin via qm) or reject immediately on read failure/queue    */
/*  full. Returns count enqueued, or -1 if input_dir itself couldn't   */
/*  be opened at all.                                                   */
/* ------------------------------------------------------------------ */
/* Contention Manager proposal (2026-08-08) - see ini_reader.h's own
 * comment on contention_manager_mode for the full design. Not
 * separately configurable, deliberately simple.                      */
#define CONTENTION_MANAGER_WRITER_QUEUE_INDEX 0

/*
 * payload_requires_single_writer_queue()
 *
 * Lightweight raw-payload peek - NOT full Level 1 parsing,
 * deliberately, since Level 1 doesn't happen until a worker dequeues
 * this later (see file_consumer.h's own Payload Ownership note).
 * Looks for any INSERT/UPDATE/DELETE operation-type marker anywhere
 * in the raw text, in either XML (type="INSERT") or JSON
 * ("type": "INSERT", with or without the space after the colon)
 * shape. A single match anywhere is enough - a request with N
 * operations where even one is a write routes the WHOLE request to
 * the writer queue, matching the existing "every operation in one
 * file shares one connection/transaction" design (see dispatcher.c's
 * own operation loop) - splitting one file's operations across two
 * different queues/connections would be a bigger change than this
 * feature takes on. SELECT and EXECUTE_PROCEDURE are NOT treated as
 * writes here - EXECUTE_PROCEDURE deliberately left alone, since Data
 * Manager has no visibility into what a procedure's own body actually
 * does.
 */
static int payload_requires_single_writer_queue(const char *payload)
{
    if (!payload) return 0;

    static const char *write_markers[] = {
        "type=\"INSERT\"", "type=\"UPDATE\"", "type=\"DELETE\"",     /* XML */
        "\"type\": \"INSERT\"", "\"type\":\"INSERT\"",                /* JSON */
        "\"type\": \"UPDATE\"", "\"type\":\"UPDATE\"",
        "\"type\": \"DELETE\"", "\"type\":\"DELETE\"",
    };

    for (size_t i = 0; i < sizeof(write_markers) / sizeof(write_markers[0]); i++)
        if (strstr(payload, write_markers[i])) return 1;

    return 0;
}

static int process_directory(oci_context_t   *ctx,
                              queue_manager_t *qm,
                              app_config_t    *config,
                              const char      *input_dir,
                              const char      *processing_dir,
                              const char      *output_dir,
                              const char      *error_dir,
                              const char      *format_label,
                              const char      *session_id)
{
    DIR *dir = opendir(input_dir);
    if (!dir)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "File Consumer: cannot open %s input directory '%s' "
                     "(%s)", format_label, input_dir, strerror(errno));
        return -1;
    }

    int enqueued = 0;
    int is_json = (strcasecmp(format_label, "JSON") == 0);
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

        /* ---- Payload Ownership: File Consumer alone reads the file.
         * Everything downstream (queue, worker, dispatcher) works only
         * with the payload from here on.                               */
        long  payload_length = 0;
        char *payload = read_file(input_path, &payload_length);

        if (!payload)
        {
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer: failed to read '%s' - rejecting "
                         "straight to Error_* (never reaches Processing_*)",
                         input_path);
            reject_immediately(ctx, name, input_path, output_dir, error_dir,
                                "FILE_READ_FAILED",
                                "File Consumer failed to read this file",
                                is_json);
            continue;
        }

        char processing_path[1024];
        n = snprintf(processing_path, sizeof(processing_path), "%s/%s",
                     processing_dir, name);
        if (n < 0 || (size_t)n >= sizeof(processing_path))
        {
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer: processing path too long, "
                         "rejecting '%s'", name);
            reject_immediately(ctx, name, input_path, output_dir, error_dir,
                                "PATH_TOO_LONG",
                                "Processing_* destination path too long",
                                is_json);
            free(payload);
            continue;
        }

        request_object_t *req = request_object_create(payload, payload_length,
                                                        name, processing_path,
                                                        output_dir, error_dir,
                                                        session_id);
        if (!req)
        {
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer: request_object_create() failed "
                         "(out of memory?) for '%s' - rejecting", name);
            reject_immediately(ctx, name, input_path, output_dir, error_dir,
                                "REQUEST_OBJECT_ALLOC_FAILED",
                                "Failed to allocate RequestObject",
                                is_json);
            free(payload);
            continue;
        }

        /* Contention Manager proposal (2026-08-08): when enabled,
         * writes get pinned to one dedicated queue (kept off the
         * normal round-robin rotation) so all INSERT/UPDATE/DELETE
         * traffic stays on one connection - directly targeting the
         * cross-worker row-lock contention this project hit
         * repeatedly under concurrent load. Off by default -
         * unchanged plain round-robin behaviour, same as before this
         * feature existed.                                            */
        int enqueue_rc;
        if (strcasecmp(config->contention_manager_mode, "single_write_queue") == 0 &&
            payload_requires_single_writer_queue(payload))
        {
            enqueue_rc = queue_manager_enqueue_to(qm, req, CONTENTION_MANAGER_WRITER_QUEUE_INDEX);
        }
        else
        {
            enqueue_rc = queue_manager_enqueue_excluding(qm, req,
                strcasecmp(config->contention_manager_mode, "single_write_queue") == 0
                    ? CONTENTION_MANAGER_WRITER_QUEUE_INDEX : -1);
        }

        if (enqueue_rc != 0)
        {
            /* Every queue full - Queue-Full Behavior addendum: reject
             * immediately, skip Processing_* entirely, straight to
             * Error_*. req is still ours to free on this path (see
             * queue_manager_enqueue()'s own contract).                  */
            logger_write(ctx->file_consumer_logger, LOG_WARN, __func__, 0,
                         "File Consumer: all queues full - rejecting '%s' "
                         "straight to Error_* (never reaches Processing_*)",
                         name);
            reject_immediately(ctx, name, input_path, output_dir, error_dir,
                                "QUEUE_FULL",
                                "All dispatcher queues are at capacity - "
                                "rejected without blocking or retry",
                                is_json);
            request_object_free(req);
            continue;
        }

        /* Enqueue succeeded - now actually move the file into
         * Processing_*. If this move fails (rare - permissions/race
         * mid-run), the item is already queued and will still be
         * dispatched and get a real response written out; only the
         * original file's physical move will be missing, which
         * response_manager_write()'s own move step will log clearly
         * when the worker gets to it. Not rolled back here - queue_manager
         * has no "unqueue a specific item" operation, and building one
         * for this rare a case isn't worth the complexity at this stage. */
        if (rename(input_path, processing_path) != 0)
        {
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer: enqueued '%s' but failed to move "
                         "it to '%s' (%s) - already queued, will still be "
                         "dispatched; the Response Manager's own move will "
                         "fail too when it gets to it, logged there",
                         name, processing_path, strerror(errno));
        }

        logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                     "File Consumer: enqueued %s file '%s' (%ld bytes)",
                     format_label, name, payload_length);

        enqueued++;
    }

    closedir(dir);
    return enqueued;
}

/* ------------------------------------------------------------------ */
/*  file_consumer_scan_once                                             */
/* ------------------------------------------------------------------ */
int file_consumer_scan_once(oci_context_t *ctx, app_config_t *config,
                             queue_manager_t *qm, const char *session_id)
{
    if (strcasecmp(config->dispatcher_algorithm, "round_robin") != 0)
    {
        logger_write(ctx->file_consumer_logger, LOG_WARN, __func__, 0,
                     "File Consumer: dispatcher_algorithm='%s' is not "
                     "implemented (only round_robin is) - using round_robin "
                     "anyway", config->dispatcher_algorithm);
    }

    int xml_result  = process_directory(ctx, qm, config,
                                         config->file_consumer_input_xml_dir,
                                         config->file_consumer_processing_xml_dir,
                                         config->file_consumer_output_xml_dir,
                                         config->file_consumer_error_xml_dir,
                                         "XML", session_id);
    int json_result = process_directory(ctx, qm, config,
                                         config->file_consumer_input_json_dir,
                                         config->file_consumer_processing_json_dir,
                                         config->file_consumer_output_json_dir,
                                         config->file_consumer_error_json_dir,
                                         "JSON", session_id);

    if (xml_result < 0 && json_result < 0)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "File Consumer: both input directories failed to "
                     "open - check file_consumer.*_dir in consumer_file.ini");
        return -1;
    }

    int total = (xml_result  > 0 ? xml_result  : 0)
              + (json_result > 0 ? json_result : 0);

    int queued = queue_manager_total_count(qm);
    logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                 "File Consumer: scan complete - %d enqueued this pass, "
                 "%d item(s) currently queued across %d queue(s) (worker "
                 "pool drains these concurrently in the background)",
                 total, queued, config->dispatcher_queue_count);

    return total;
}
