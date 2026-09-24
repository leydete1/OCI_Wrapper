#ifndef ASYNC_CALLBACK_CLIENT_H
#define ASYNC_CALLBACK_CLIENT_H

/* ======================================================================
 * async_callback_client.h
 *
 * Stage 5 (2026-08-22) - execute_async / async_call_back_url.
 *
 * The one genuinely new kind of thing in this codebase: an OUTBOUND HTTP
 * client. Everything else in Data Manager either talks to Oracle or
 * receives inbound HTTP; this is the first module that makes a request
 * TO something, on the caller's behalf, as a side effect of processing
 * a SELECT.
 *
 * Design, per discussion with Terry (2026-08-21):
 *   - Best-effort delivery, no retry. If a batch POST fails (timeout,
 *     connection refused, non-2xx response), log it and move on to the
 *     next batch - do not block the fetch loop, do not retry, do not
 *     fail the whole request. Any network issue is the CLIENT's problem
 *     to notice and retry from their end; a stuck retry loop on our
 *     side would be worse than a dropped batch. "Anything network
 *     related could affect the client from receiving. If they timeout
 *     they can retry."
 *   - TLS-only, no exceptions. Level 2 already rejects a non-https://
 *     async_call_back_url before this module is ever reached, but
 *     async_callback_post() re-checks anyway, defensively - this
 *     module should never be the one place in the codebase that would
 *     silently accept a plaintext callback if some future caller
 *     forgot the upstream check. "No one would implement or tolerate
 *     unencrypted traffic today."
 *   - Every batch is a complete, independently well-formed XML/JSON
 *     document (same envelope shape as a normal SELECT response), not
 *     a fragment that only becomes valid once concatenated with others.
 *     The caller (execute_query_batch()) is responsible for building
 *     that document per batch; this module only POSTs it.
 * ====================================================================== */

#include "OCI_Connection.h"

/*
 * async_callback_post()
 *
 * POSTs body (a complete, well-formed batch document - XML or JSON, per
 * is_json) to url. Single attempt, no retry, generous but bounded
 * timeout (see .c file) so one slow/unresponsive callback endpoint
 * can't stall the fetch loop indefinitely.
 *
 * Returns 0 if the POST completed with a 2xx response. Returns -1 on
 * any failure (bad URL, transport failure, non-2xx response, timeout) -
 * logged via ctx->select_logger at WARN, never escalated to an error
 * the caller needs to handle specially. Per this module's own design
 * note above: a failed batch delivery does not stop the fetch loop and
 * does not fail the request - the caller should simply continue to the
 * next batch.
 *
 * url MUST be https:// - defensively re-checked here even though
 * Level 2 already enforces this upstream (see OCI_Level2_Parser.c).
 * Returns -1 immediately, without attempting any connection, if url
 * is not https://.
 */
int async_callback_post(oci_context_t *ctx,
                         const char    *url,
                         const char    *body,
                         int            is_json);

#endif /* ASYNC_CALLBACK_CLIENT_H */
