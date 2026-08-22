/* ======================================================================
 * async_callback_client.c
 *
 * See async_callback_client.h for the full Stage 5 design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Same safety net as http_consumer_tester.c - bypasses curl.h's
 * GCC-specific compile-time type-checking macros, which have shown a
 * history of failing to expand cleanly in this environment regardless
 * of root cause (libcurl4-openssl-dev confirmed properly installed).
 * Harmless either way; only removes extra compile-time argument-type
 * checking on curl_easy_setopt(), not any actual functionality. */
#define CURL_DISABLE_TYPECHECK 1
#include <curl/curl.h>

#include "async_callback_client.h"
#include "logger.h"

/* Generous but bounded - long enough that a momentarily-busy callback
 * endpoint isn't punished unfairly, short enough that one unresponsive
 * endpoint can't stall a fetch loop that still has more batches to
 * send. Not currently config-driven; revisit if a real deployment
 * needs this tuned per-callback-endpoint. */
#define ASYNC_CALLBACK_TIMEOUT_SECONDS 10L

/* Discards the response body - the caller (a batch delivery, not a
 * request/response exchange) doesn't do anything with whatever the
 * callback endpoint sends back, only whether the POST itself
 * succeeded. libcurl requires a write callback to be set regardless;
 * this one just reports every byte as consumed without storing it.  */
static size_t discard_response_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    (void)contents;
    (void)userp;
    return size * nmemb;
}

int async_callback_post(oci_context_t *ctx,
                         const char    *url,
                         const char    *body,
                         int            is_json)
{
    if (!ctx || !url || !url[0] || !body)
    {
        logger_write(ctx ? ctx->select_logger : NULL, LOG_WARN, __func__, 0,
                     "async_callback_post: missing url or body - "
                     "not attempting delivery");
        return -1;
    }

    /* Defensive re-check - see this module's own header doc comment on
     * why this stays here even though Level 2 already enforces it
     * upstream. TLS is non-negotiable for this module, full stop.     */
    if (strncasecmp(url, "https://", 8) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "async_callback_post: refusing non-HTTPS callback "
                     "URL ('%s') - this should have been rejected at "
                     "Level 2 validation already", url);
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "async_callback_post: curl_easy_init failed");
        return -1;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
        is_json ? "Content-Type: application/json"
                : "Content-Type: application/xml");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, ASYNC_CALLBACK_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response_cb);

    /* Deliberately left at libcurl's default (verify ON) - unlike
     * http_consumer_tester.c's own use of -k against OUR self-signed
     * local test cert, this is a genuine outbound call to a THIRD
     * PARTY's own HTTPS endpoint. Certificate verification stays on;
     * there is no equivalent local-self-signed-cert justification for
     * disabling it here. */

    CURLcode rc = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "async_callback_post: delivery failed to '%s' - %s "
                     "(best-effort - continuing to the next batch, if "
                     "any)", url, curl_easy_strerror(rc));
        return -1;
    }

    if (http_status < 200 || http_status >= 300)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "async_callback_post: '%s' responded HTTP %ld - "
                     "(best-effort - continuing to the next batch, if "
                     "any)", url, http_status);
        return -1;
    }

    return 0;
}
