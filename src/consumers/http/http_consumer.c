/* ======================================================================
 * http_consumer.c
 *
 * See http_consumer.h for the full Stage 2/Stage 4 design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <string.h>

#include "http_consumer.h"
#include "logger.h"
#include "OCI_Session_Manager.h"

/* No config-driven envelope size limit yet - 10 MB is a sane guard
 * against a runaway/malicious body filling memory. Revisit once
 * config.ini has a real http_consumer_max_body_bytes field.            */
#define HTTP_CONSUMER_MAX_BODY_BYTES (10 * 1024 * 1024)

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
                                             const char *content_type,
                                             const char *body)
{
    struct MHD_Response *response =
        MHD_create_response_from_buffer(strlen(body), (void *)body,
                                         MHD_RESPMEM_MUST_COPY);
    if (!response)
        return MHD_NO;

    MHD_add_response_header(response, "Content-Type", content_type);
    enum MHD_Result ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

/* Sends resp->response_body as-is, with a Content-Type matching
 * resp->is_json - always HTTP 200 (see http_consumer.h's own note on
 * why: Data Manager's own PASS/ERROR status lives entirely inside the
 * payload, HTTP status stays purely transport-level). Does not free
 * resp - caller still owns that via response_object_free(), same as
 * before this call.                                                    */
static enum MHD_Result send_dispatch_response(struct MHD_Connection *connection,
                                               response_object_t *resp)
{
    const char *content_type = resp->is_json ? "application/json"
                                              : "application/xml";

    struct MHD_Response *response =
        MHD_create_response_from_buffer(strlen(resp->response_body),
                                         (void *)resp->response_body,
                                         MHD_RESPMEM_MUST_COPY);
    if (!response)
        return MHD_NO;

    MHD_add_response_header(response, "Content-Type", content_type);
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
}

/* Narrow-purpose only - pulls a single flat <tag>value</tag> out of a
 * raw XML string. Not a general parser; used here purely for
 * session_id on an END_SESSION request, which session_request_t
 * doesn't carry (parse_session_request() is CREATE_SESSION-shaped -
 * see OCI_Session_Manager.h). Mirrors the same flat-tag-search
 * approach parse_session_request()'s own internal extract_tag()
 * clearly already uses, just not exported for reuse here. */
static int extract_simple_tag(const char *xml, const char *tag,
                               char *out, size_t out_size)
{
    char open_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    const char *start = strstr(xml, open_tag);
    if (!start) return 0;
    start += strlen(open_tag);

    char close_tag[64];
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    const char *end = strstr(start, close_tag);
    if (!end || end <= start) return 0;

    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

/* Session lifecycle requests (CREATE_SESSION/END_SESSION) use a
 * completely different envelope - <Session_Request> with a plain
 * <operation>CREATE_SESSION</operation> text tag - not the
 * <request>/<transaction>/<operation type="..."> shape every CRUD
 * operation uses. Detected by http_consumer_handle_request() before
 * the normal process_xml_file() dispatch and routed here instead.
 *
 * Level 2 explicitly rejects both operation types if they ever reach
 * the normal CRUD pipeline (LEVEL2_ERR_NOT_IMPLEMENTED, by design -
 * see OCI_Level2_Parser.h's own doc comment), so process_xml_file()
 * could never have serviced these anyway - this isn't bypassing
 * validation that exists, session_create()/session_end() ARE the
 * validators for these two operation types.
 *
 * Always HTTP 200 - same reasoning as send_dispatch_response(): the
 * real status lives in the returned <session>...</session> body.      */
static enum MHD_Result handle_session_request(oci_context_t *base_ctx,
                                               struct MHD_Connection *connection,
                                               const char *payload)
{
    session_request_t sess_req;
    if (parse_session_request(base_ctx, payload, &sess_req) != SESSION_OK)
    {
        logger_write(base_ctx->http_consumer_logger, LOG_WARN, __func__, 0,
                     "HTTP Consumer: Session_Request parse failed - "
                     "<operation> missing or malformed");
        return send_static_response(connection, MHD_HTTP_OK, "application/xml",
            "<session><status>ERROR</status>"
            "<error_code>SESSION_PARSE_FAILED</error_code>"
            "<error_text>Session_Request could not be parsed - "
            "&lt;operation&gt; is mandatory.</error_text></session>\n");
    }

    oci_context_t thread_ctx;
    memset(&thread_ctx, 0, sizeof(thread_ctx));
    if (OCI_Pool_get_session(base_ctx, &thread_ctx) != 0)
    {
        logger_write(base_ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "HTTP Consumer: OCI_Pool_get_session failed servicing "
                     "a %s request", sess_req.operation);
        return send_static_response(connection, MHD_HTTP_SERVICE_UNAVAILABLE,
                                     "text/plain",
                                     "No database session currently available\n");
    }
    copy_shared_ctx_fields(&thread_ctx, base_ctx);
    thread_ctx.active_tx = NULL;

    char *result_xml = NULL;
    enum MHD_Result ret;

    if (strcasecmp(sess_req.operation, "CREATE_SESSION") == 0)
    {
        int rc = session_create(&thread_ctx, &sess_req, &result_xml);
        logger_write(base_ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                     "HTTP Consumer: CREATE_SESSION %s",
                     (rc == SESSION_OK) ? "PASS" : "FAILED");
        ret = send_static_response(connection, MHD_HTTP_OK, "application/xml",
            result_xml ? result_xml :
            "<session><status>ERROR</status>"
            "<error_code>SESSION_CREATE_FAILED</error_code></session>\n");
    }
    else if (strcasecmp(sess_req.operation, "END_SESSION") == 0)
    {
        char session_id[64] = "";
        extract_simple_tag(payload, "session_id", session_id, sizeof(session_id));

        if (!session_id[0])
        {
            ret = send_static_response(connection, MHD_HTTP_OK, "application/xml",
                "<session><status>ERROR</status>"
                "<error_code>SESSION_ID_MISSING</error_code>"
                "<error_text>END_SESSION requires "
                "&lt;session_id&gt;.</error_text></session>\n");
        }
        else
        {
            int rc = session_end(&thread_ctx, session_id,
                                  SESSION_STATUS_LOGGED_OUT,
                                  "CLIENT_REQUESTED", &result_xml);
            logger_write(base_ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                         "HTTP Consumer: END_SESSION %s (session_id=%s)",
                         (rc == SESSION_OK) ? "PASS" : "FAILED", session_id);
            ret = send_static_response(connection, MHD_HTTP_OK, "application/xml",
                result_xml ? result_xml :
                "<session><status>ERROR</status>"
                "<error_code>SESSION_END_FAILED</error_code></session>\n");
        }
    }
    else
    {
        logger_write(base_ctx->http_consumer_logger, LOG_WARN, __func__, 0,
                     "HTTP Consumer: unrecognised session operation '%s'",
                     sess_req.operation);
        ret = send_static_response(connection, MHD_HTTP_OK, "application/xml",
            "<session><status>ERROR</status>"
            "<error_code>UNKNOWN_SESSION_OPERATION</error_code></session>\n");
    }

    free(result_xml);
    OCI_Pool_release_session(base_ctx, &thread_ctx);
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
                                     "text/plain", "Method Not Allowed - use POST\n");

    /* Body chunk arriving - accumulate it. */
    if (*upload_data_size != 0)
    {
        if (state->body_len + *upload_data_size > HTTP_CONSUMER_MAX_BODY_BYTES)
        {
            logger_write(ctx->http_consumer_logger, LOG_WARN, __func__, 0,
                         "HTTP Consumer: body exceeds %d byte guard - "
                         "rejecting", HTTP_CONSUMER_MAX_BODY_BYTES);
            *upload_data_size = 0;
            return send_static_response(connection, MHD_HTTP_PAYLOAD_TOO_LARGE,
                                         "text/plain", "Payload Too Large\n");
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
                                             "text/plain", "Internal Server Error\n");
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
     * announcement call - body (if any) is fully received. */
    const char *payload = state->body ? state->body : "";

    /* Session lifecycle requests get routed separately, before the
     * normal CRUD dispatch - see handle_session_request()'s own doc
     * comment for why. Sniffed on the root element, not the usual
     * <operation type="..."> attribute every CRUD request carries. */
    if (strstr(payload, "<Session_Request") != NULL)
    {
        logger_write(ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                     "HTTP Consumer: POST %s - Session_Request detected, "
                     "routing to session handler", url);
        return handle_session_request(ctx, connection, payload);
    }

    /* Stage 4 CRUD dispatch: build a RequestObject and hand it to the
     * worker pool, which routes it to the right queue (T0 for writes,
     * T1..T(n-1) round-robin for everything else) and blocks this
     * thread until a dedicated worker thread finishes it. No session
     * borrow here at all anymore - each worker owns its own session
     * for its whole lifetime, borrowed once at startup (see
     * http_worker_pool.c). */
    logger_write(ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                 "HTTP Consumer: POST %s - dispatching %zu byte body via "
                 "worker pool", url, state->body_len);

    char *payload_copy = malloc(state->body_len + 1);
    if (!payload_copy)
    {
        logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "HTTP Consumer: malloc failed copying request body "
                     "for the worker pool");
        return send_static_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                     "text/plain", "Internal Server Error\n");
    }
    memcpy(payload_copy, payload, state->body_len);
    payload_copy[state->body_len] = '\0';

    /* request_object_create() takes ownership of payload_copy - do not
     * touch or free it after this call succeeds. filename/paths are
     * File-Consumer-shaped traceability fields this consumer doesn't
     * use - "-" placeholders, matching request_object_t's own documented
     * fallback for "genuinely unavailable". session_id_override stays
     * unset here too - same as Stage 2/3, HTTP consumer trusts whatever
     * session_id is actually in the payload; Level 3 validates it. */
    request_object_t *req = request_object_create(payload_copy, (long)state->body_len,
                                                    "http_request", "-", "-", "-", NULL);
    if (!req)
    {
        logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "HTTP Consumer: request_object_create failed");
        free(payload_copy);
        return send_static_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                     "text/plain", "Internal Server Error\n");
    }

    /* Stage 5 (2026-08-23) - execute_async fire-and-forget path. Sniffed
     * on the payload BEFORE building/consuming req via the normal
     * blocking dispatch, same lightweight content-check technique as
     * write-detection (http_request_is_write()). Level 2 has already
     * confirmed, upstream, that execute_async=1 only ever appears on a
     * SELECT with no sibling writes - nothing further to validate here,
     * this is purely a routing decision between the two dispatch modes. */
    if (http_request_is_async_select(req->payload ? req->payload : ""))
    {
        /* Bug fix (2026-08-24), found via real testing - this sniff
         * alone can't tell whether Level 2 will actually ACCEPT the
         * request (empty/non-HTTPS async_call_back_url, or execute_
         * async=1 alongside a sibling write, both get rejected by
         * Level 2 - see OCI_Level2_Parser.c). Committing to the fire-
         * and-forget path before knowing that meant an invalid request
         * still got told "202 Accepted - batches will be delivered",
         * then got correctly rejected on the worker thread a moment
         * later with nowhere for that rejection to go - a false
         * success told to the client, silently, every time. This gate
         * runs Level 1/Level 2 synchronously first (cheap - pure
         * parsing/validation, no DB round trip) so an invalid request
         * gets its real error envelope now, on this thread, and never
         * reaches the fire-and-forget path at all. See dispatcher.h's
         * own doc comment on validate_async_select_request() for the
         * full rationale. */
        response_object_t validation_resp;
        response_object_init(&validation_resp);

        if (validate_async_select_request(ctx, req->payload ? req->payload : "",
                                           (long)state->body_len, &validation_resp) != 0)
        {
            logger_write(ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                         "HTTP Consumer: POST %s - execute_async=1 request "
                         "rejected by Level 1/Level 2 pre-check, never "
                         "reaches the async dispatch path (audit_id=%s)",
                         url, validation_resp.audit_id);

            enum MHD_Result ret = send_dispatch_response(connection, &validation_resp);
            response_object_free(&validation_resp);
            request_object_free(req);
            return ret;
        }
        response_object_free(&validation_resp);

        logger_write(ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                     "HTTP Consumer: POST %s - execute_async=1, enqueueing "
                     "and returning 202 immediately - batches will be "
                     "delivered to the request's own async_call_back_url",
                     url);

        int async_rc = http_worker_pool_dispatch_async(hctx->pool, req);
        if (async_rc != 0)
        {
            /* QUEUE_FULL - req already freed inside dispatch_async().
             * No completion signal exists in this mode to report a
             * Data-Manager-level QUEUE_FULL envelope through the normal
             * way, so this is the one case in the whole file where an
             * enqueue failure surfaces as a real HTTP status rather
             * than a 200 with an ERROR payload - there is no request-
             * scoped response object left to put that payload in. */
            logger_write(ctx->http_consumer_logger, LOG_WARN, __func__, 0,
                         "HTTP Consumer: POST %s - QUEUE_FULL on the "
                         "async path, all relevant queue(s) at depth",
                         url);
            return send_static_response(connection, MHD_HTTP_SERVICE_UNAVAILABLE,
                                         "text/plain",
                                         "Every relevant queue is currently "
                                         "at capacity - try again shortly.\n");
        }

        return send_static_response(connection, MHD_HTTP_ACCEPTED, "application/xml",
            "<output_xml><status>ACCEPTED</status>"
            "<message>Request accepted - batches will be delivered to "
            "the provided callback URL.</message></output_xml>\n");
    }

    response_object_t resp;
    int rc = http_worker_pool_dispatch(hctx->pool, req, &resp);

    if (rc != 0)
    {
        /* QUEUE_FULL - req was already freed inside dispatch(). Always
         * HTTP 200 per the same reasoning as everything else in this
         * file - QUEUE_FULL is a Data Manager-level outcome, not an
         * HTTP transport failure, so it belongs in the payload, not
         * the status code. */
        logger_write(ctx->http_consumer_logger, LOG_WARN, __func__, 0,
                     "HTTP Consumer: POST %s - QUEUE_FULL, all relevant "
                     "queue(s) at depth", url);
        return send_static_response(connection, MHD_HTTP_OK, "application/xml",
            "<output_xml><execution_envelope><status>ERROR</status>"
            "<error_code>QUEUE_FULL</error_code>"
            "<error_text>Every relevant queue is currently at capacity - "
            "try again shortly.</error_text></execution_envelope>"
            "</output_xml>\n");
    }

    logger_write(ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                 "HTTP Consumer: POST %s - %s (audit_id=%s, operation=%s)",
                 url, (resp.status == RESPONSE_STATUS_PASS) ? "PASS" : "ERROR",
                 resp.audit_id, resp.operation);

    enum MHD_Result ret = send_dispatch_response(connection, &resp);

    response_object_free(&resp);

    return ret;
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
