/* ======================================================================
 * http_consumer_runner.c
 *
 * See http_consumer_runner.h for the full design rationale.
 * ====================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>

#include "http_consumer_runner.h"
#include "http_consumer.h"
#include "logger.h"

struct http_consumer_runner {
    struct MHD_Daemon    *daemon;
    http_consumer_ctx_t  *hctx;      /* cls handed to MHD - must outlive
                                         the daemon, so it's owned here */
    char                  *tls_cert; /* MHD_OPTION_HTTPS_MEM_CERT needs
                                         these to stay alive for the
                                         daemon's whole lifetime, not
                                         just the start() call          */
    char                  *tls_key;
};

/* Reads an entire file into a malloc'd, NUL-terminated buffer -
 * MHD_OPTION_HTTPS_MEM_CERT/_KEY both want a NUL-terminated PEM string,
 * not a length-prefixed buffer. Returns NULL on any failure (file
 * missing, unreadable, empty, allocation failure) - caller treats NULL
 * as "TLS material unavailable", never as "use plaintext instead".      */
static char *read_pem_file(const char *path)
{
    if (!path || !path[0])
        return NULL;

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (read != (size_t)size)
    {
        free(buf);
        return NULL;
    }

    buf[size] = '\0';
    return buf;
}

http_consumer_runner_t *http_consumer_runner_start(oci_context_t *ctx,
                                                     app_config_t  *config)
{
    http_consumer_runner_t *runner = calloc(1, sizeof(http_consumer_runner_t));
    if (!runner)
    {
        logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_consumer_runner_start: calloc failed");
        return NULL;
    }

    /* --- TLS material: mandatory, no fallback --- */
    runner->tls_cert = read_pem_file(config->http_consumer_tls_cert_file);
    if (!runner->tls_cert)
    {
        logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_consumer_runner_start: could not read TLS cert "
                     "'%s' - refusing to start (TLS is mandatory, there is "
                     "no plaintext fallback)",
                     config->http_consumer_tls_cert_file);
        free(runner);
        return NULL;
    }

    runner->tls_key = read_pem_file(config->http_consumer_tls_key_file);
    if (!runner->tls_key)
    {
        logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_consumer_runner_start: could not read TLS key "
                     "'%s' - refusing to start (TLS is mandatory, there is "
                     "no plaintext fallback)",
                     config->http_consumer_tls_key_file);
        free(runner->tls_cert);
        free(runner);
        return NULL;
    }

    runner->hctx = malloc(sizeof(http_consumer_ctx_t));
    if (!runner->hctx)
    {
        logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_consumer_runner_start: malloc failed for "
                     "http_consumer_ctx_t");
        free(runner->tls_cert);
        free(runner->tls_key);
        free(runner);
        return NULL;
    }
    runner->hctx->ctx    = ctx;
    runner->hctx->config = config;

    int port = config->http_consumer_port > 0 ? config->http_consumer_port : 8443;
    int pool_size = config->http_consumer_thread_pool_size > 0
                    ? config->http_consumer_thread_pool_size : 8;

    /* MHD_USE_INTERNAL_POLLING_THREAD + MHD_OPTION_THREAD_POOL_SIZE:
     * libmicrohttpd owns and runs its own worker threads - nothing for
     * this module to loop or sleep on, unlike file_consumer_runner.c's
     * self-managed pthread. MHD_USE_TLS is not optional - there is
     * deliberately no code path in this file that starts a daemon
     * without it.                                                       */
    runner->daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_TLS | MHD_USE_ERROR_LOG,
        (uint16_t)port,
        NULL, NULL,                                  /* no per-connection accept filter */
        &http_consumer_handle_request, runner->hctx,
        MHD_OPTION_NOTIFY_COMPLETED, &http_consumer_request_completed, NULL,
        MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)pool_size,
        MHD_OPTION_HTTPS_MEM_CERT, runner->tls_cert,
        MHD_OPTION_HTTPS_MEM_KEY, runner->tls_key,
        MHD_OPTION_END);

    if (!runner->daemon)
    {
        logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_consumer_runner_start: MHD_start_daemon failed "
                     "(port=%d, pool_size=%d) - port may already be in use, "
                     "or the TLS material may be malformed", port, pool_size);
        free(runner->tls_cert);
        free(runner->tls_key);
        free(runner->hctx);
        free(runner);
        return NULL;
    }

    logger_write(ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                 "HTTP Consumer: TLS listener started on %s:%d, "
                 "thread_pool_size=%d (Stage 0 - bare listener, no "
                 "request pipeline wired up yet)",
                 config->http_consumer_bind_address[0]
                     ? config->http_consumer_bind_address : "0.0.0.0",
                 port, pool_size);

    return runner;
}

void http_consumer_runner_stop(http_consumer_runner_t *runner)
{
    if (!runner) return;

    if (runner->daemon)
        MHD_stop_daemon(runner->daemon);   /* blocks until fully drained */

    free(runner->hctx);
    free(runner->tls_cert);
    free(runner->tls_key);
    free(runner);
}
