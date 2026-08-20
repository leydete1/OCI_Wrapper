#include <stdlib.h>
#include <string.h>

#include "request_object.h"

request_object_t *request_object_create(char       *payload,
                                          long        payload_length,
                                          const char *filename,
                                          const char *processing_path,
                                          const char *output_dir,
                                          const char *error_dir,
                                          const char *session_id)
{
    request_object_t *req = malloc(sizeof(request_object_t));
    if (!req) return NULL;

    req->payload        = payload;   /* ownership transferred to req */
    req->payload_length  = payload_length;

    strncpy(req->filename, filename ? filename : "-", sizeof(req->filename) - 1);
    req->filename[sizeof(req->filename) - 1] = '\0';

    strncpy(req->processing_path, processing_path ? processing_path : "-",
            sizeof(req->processing_path) - 1);
    req->processing_path[sizeof(req->processing_path) - 1] = '\0';

    strncpy(req->output_dir, output_dir ? output_dir : "-", sizeof(req->output_dir) - 1);
    req->output_dir[sizeof(req->output_dir) - 1] = '\0';

    strncpy(req->error_dir, error_dir ? error_dir : "-", sizeof(req->error_dir) - 1);
    req->error_dir[sizeof(req->error_dir) - 1] = '\0';

    strncpy(req->session_id, (session_id && session_id[0]) ? session_id : "",
            sizeof(req->session_id) - 1);
    req->session_id[sizeof(req->session_id) - 1] = '\0';

    req->completion_ctx = NULL;   /* HTTP Consumer Stage 4 - see request_object.h.
                                      Always NULL here; set explicitly via
                                      request_object_set_completion() only by
                                      the caller that actually needs it.       */

    return req;
}

void request_object_set_completion(request_object_t *req, void *completion_ctx)
{
    if (!req) return;
    req->completion_ctx = completion_ctx;
}

void request_object_free(request_object_t *req)
{
    if (!req) return;
    if (req->payload) free(req->payload);
    free(req);
}
