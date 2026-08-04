/* ======================================================================
 * worker.c
 *
 * See worker.h for the full design rationale.
 * ====================================================================== */

#include "worker.h"
#include "dispatcher.h"
#include "response_object.h"
#include "response_manager.h"
#include "logger.h"

int worker_run(oci_context_t *ctx, queue_manager_t *qm)
{
    int drained = 0;
    request_object_t *req;

    while ((req = queue_manager_dequeue_any(qm)) != NULL)
    {
        logger_write(ctx->worker_logger, LOG_INFO, __func__, 0,
                     "Worker: dequeued '%s' (%ld bytes)",
                     req->filename, req->payload_length);

        response_object_t resp;
        response_object_init(&resp);

        int rc = process_xml_file(ctx, req->payload, req->payload_length,
                                   req->filename, &resp);

        if (rc == 0)
            logger_write(ctx->worker_logger, LOG_INFO, __func__, 0,
                         "Worker: PASS '%s'", req->filename);
        else
            logger_write(ctx->worker_logger, LOG_ERROR, __func__, 0,
                         "Worker: FAIL '%s' (rc=%d, error_code=%s)",
                         req->filename, rc, resp.error_code);

        if (response_manager_write(ctx, &resp, req->filename, req->processing_path,
                                    req->output_dir, req->error_dir) != 0)
        {
            logger_write(ctx->worker_logger, LOG_ERROR, __func__, 0,
                         "Worker: Response Manager reported a problem "
                         "writing/moving '%s' - see log above for detail",
                         req->filename);
        }

        response_object_free(&resp);
        request_object_free(req);

        drained++;
    }

    logger_write(ctx->worker_logger, LOG_INFO, __func__, 0,
                 "Worker: drained %d item(s), queues now empty", drained);

    return drained;
}
