/* ======================================================================
 * http_consumer.c
 *
 * See http_consumer.h for the full Stage 0 design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <string.h>

#include "http_consumer.h"
#include "logger.h"

/* Stage 0 has no envelope size limit wired to config yet (that belongs
 * with the max_rows-style config plumbing later stages will add
 * alongside real parsing). 10 MB is just a sane guard against a
 * runaway/malicious body filling memory while this is still a bare
 * listener - revisit once config.ini has a real
 * http_consumer_max_body_bytes field.                                   */
#define STAGE0_MAX_BODY_BYTES (10 * 1024 * 1024)

/* Per-connection accumulation state.
 *
 * MHD calls the access handler multiple times per request: once with
 * upload_data_size == 0 to announce headers, then repeatedly with
 * chunks of the body (upload_data_size > 0 each time), then a final
 * call with upload_data_size == 0 again once the body is fully
 * received. con_cls is MHD's per-connection slot for carrying state
 * across those calls - NULL on the very first call, whatever we set it
 * to on every call after that. Without this, a POST body arriving in
 * more than one TCP read gets silently truncated to whichever chunk
 * happened to be logged.                                                */
typedef struct {
    char   *body;
    size_t  body_len;
    size_t  body_capacity;
    int     is_post;
} connection_state_t;

static enum MHD_Result send_static_response(struct MHD_Connection *connection,
                                             unsigned int status_code,
                                             const char *body)
{
    struct MHD_Response *response =
        MHD_create_response_from_buffer(strlen(body), (void *)body,
                                         MHD_RESPMEM_MUST_COPY);
    if (!response)
        return MHD_NO;

    MHD_add_response_header(response, "Content-Type", "text/plain");
    enum MHD_Result ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

enum MHD_Result http_consumer_handle_request(void *cls,
                                              struct MHD_Connection *connection,
                                              const char *url,
                                              const char *method,
                                              const char *version,
                                              const char *upload_data,
                                              size_t *upload_data_size,
                                              void **con_cls)
{
    (void)url;
    (void)version;

    http_consumer_ctx_t *hctx = (http_consumer_ctx_t *)cls;
    oci_context_t *ctx = hctx->ctx;

    /* First call for this connection: allocate accumulation state and
     * return immediately without consuming any body data yet - the
     * standard MHD pattern (mirrors every libmicrohttpd POST example).
     * Non-POST methods are rejected here too, before any body is read. */
    if (*con_cls == NULL)
    {
        connection_state_t *state = calloc(1, sizeof(connection_state_t));
        if (!state)
        {
            logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                         "HTTP Consumer: calloc failed for connection state - "
                         "cannot service this connection");
            return MHD_NO;
        }

        state->is_post = (strcmp(method, "POST") == 0);
        *con_cls = state;

        if (!state->is_post)
        {
            logger_write(ctx->http_consumer_logger, LOG_WARN, __func__, 0,
                         "HTTP Consumer: rejected %s %s - only POST is "
                         "accepted", method, url);
        }

        return MHD_YES;
    }

    connection_state_t *state = (connection_state_t *)*con_cls;

    if (!state->is_post)
        return send_static_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED,
                                     "Method Not Allowed - use POST\n");

    /* Body chunk arriving - accumulate it. */
    if (*upload_data_size != 0)
    {
        if (state->body_len + *upload_data_size > STAGE0_MAX_BODY_BYTES)
        {
            logger_write(ctx->http_consumer_logger, LOG_WARN, __func__, 0,
                         "HTTP Consumer: body exceeds %d byte Stage 0 guard - "
                         "rejecting", STAGE0_MAX_BODY_BYTES);
            *upload_data_size = 0;
            return send_static_response(connection, MHD_HTTP_PAYLOAD_TOO_LARGE,
                                         "Payload Too Large\n");
        }

        size_t needed = state->body_len + *upload_data_size + 1;
        if (needed > state->body_capacity)
        {
            size_t new_capacity = needed * 2;
            char *grown = realloc(state->body, new_capacity);
            if (!grown)
            {
                logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                             "HTTP Consumer: realloc failed accumulating body "
                             "(%zu bytes so far)", state->body_len);
                *upload_data_size = 0;
                return send_static_response(connection,
                                             MHD_HTTP_INTERNAL_SERVER_ERROR,
                                             "Internal Server Error\n");
            }
            state->body = grown;
            state->body_capacity = new_capacity;
        }

        memcpy(state->body + state->body_len, upload_data, *upload_data_size);
        state->body_len += *upload_data_size;
        state->body[state->body_len] = '\0';

        *upload_data_size = 0;   /* tells MHD we consumed it all */
        return MHD_YES;
    }

    /* upload_data_size == 0 and we've already seen at least the header
     * announcement call - body (if any) is fully received. Stage 0 just
     * logs it and returns a static acknowledgement - no parsing, no
     * dispatch. */
    logger_write(ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                 "HTTP Consumer: POST %s - received %zu byte body: %.500s%s",
                 url, state->body_len,
                 state->body ? state->body : "(empty)",
                 (state->body_len > 500) ? "...(truncated in log)" : "");

    return send_static_response(connection, MHD_HTTP_OK,
                                 "Data Manager HTTP Consumer: Stage 0 - "
                                 "listener alive\n");
}

void http_consumer_request_completed(void *cls,
                                      struct MHD_Connection *connection,
                                      void **con_cls,
                                      enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)connection;
    (void)toe;

    connection_state_t *state = (connection_state_t *)*con_cls;
    if (!state)
        return;

    free(state->body);
    free(state);
    *con_cls = NULL;
}
