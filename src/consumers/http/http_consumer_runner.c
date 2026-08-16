/* ======================================================================
 * http_consumer_runner.c
 *
 * See http_consumer_runner.h for the full design rationale, including
 * the 2026-08-16 lifetime decoupling change.
 * ====================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include <microhttpd.h>

#include "http_consumer_runner.h"
#include "http_consumer.h"
#include "logger.h"

struct http_consumer_runner {
    struct MHD_Daemon    *daemon;
    http_consumer_ctx_t  *hctx;      /* cls handed to MHD - must outlive
                                         the daemon, so it's owned here */
    char                  *tls_cert;
    char                  *tls_key;

    /* Lifetime decoupling (2026-08-16) */
    int             lifetime_seconds;        /* 0 = forever */
    pthread_t       lifetime_thread;
    int             lifetime_thread_started; /* pthread_join() is UB on
                                                 an unstarted pthread_t,
                                                 this guards that       */
    pthread_mutex_t stop_mutex;
    int             already_stopped;         /* idempotency guard -
                                                 watchdog and an
                                                 explicit stop() call
                                                 can both reach the
                                                 MHD_stop_daemon() call */
};

/* Reads an entire file into a malloc'd, NUL-terminated buffer -
 * MHD_OPTION_HTTPS_MEM_CERT/_KEY both want a NUL-terminated PEM string,
 * not a length-prefixed buffer. Returns NULL on any failure - caller
 * treats NULL as "TLS material unavailable", never as "use plaintext
 * instead".                                                             */
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

/* Actually stops the MHD daemon, exactly once, regardless of whether
 * the caller is the lifetime watchdog thread or an explicit
 * http_consumer_runner_stop() call from main() - whichever gets here
 * first does the real work, the other becomes a no-op. Does NOT free
 * the runner itself - that's still http_consumer_runner_stop()'s job,
 * unconditionally, since resources need freeing regardless of who
 * triggered the MHD-level stop.                                        */
static void stop_daemon_once(http_consumer_runner_t *runner, int from_watchdog)
{
    pthread_mutex_lock(&runner->stop_mutex);
    if (runner->already_stopped)
    {
        pthread_mutex_unlock(&runner->stop_mutex);
        return;
    }
    runner->already_stopped = 1;
    pthread_mutex_unlock(&runner->stop_mutex);

    logger_t *http_logger = runner->hctx ? runner->hctx->ctx->http_consumer_logger : NULL;

    if (http_logger)
        logger_write(http_logger, LOG_INFO, __func__, 0,
                     "HTTP Consumer: stopping TLS listener (%s) - "
                     "blocking until all in-flight connections drain",
                     from_watchdog ? "http_dispatcher.lifetime_seconds elapsed"
                                   : "external stop request");

    if (runner->daemon)
        MHD_stop_daemon(runner->daemon);   /* blocks until fully drained */

    if (http_logger)
        logger_write(http_logger, LOG_INFO, __func__, 0,
                     "HTTP Consumer: TLS listener stopped, all "
                     "connections drained");
}

static void *lifetime_watchdog_thread(void *arg)
{
    http_consumer_runner_t *runner = (http_consumer_runner_t *)arg;

    if (runner->lifetime_seconds > 0)
    {
        struct timespec ts = { runner->lifetime_seconds, 0 };
        nanosleep(&ts, NULL);

        logger_t *http_logger = runner->hctx ? runner->hctx->ctx->http_consumer_logger : NULL;
        if (http_logger)
            logger_write(http_logger, LOG_INFO, __func__, 0,
                         "HTTP Consumer: http_dispatcher.lifetime_seconds "
                         "(%d) elapsed - self-stopping",
                         runner->lifetime_seconds);

        stop_daemon_once(runner, 1);
        return NULL;
    }

    /* lifetime_seconds == 0: block indefinitely - same "external kill
     * only" semantics as File Consumer's own dispatcher.lifetime_
     * seconds=0 case (file_consumer_runner_join() never returns either,
     * under that same setting). This thread deliberately never returns
     * under normal operation; http_consumer_runner_join(), which
     * pthread_joins it, blocks for exactly as long.                    */
    for (;;)
    {
        struct timespec ts = { 3600, 0 };
        nanosleep(&ts, NULL);
    }

    return NULL;   /* unreachable - silences -Wreturn-type on some compilers */
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

    runner->daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_TLS | MHD_USE_ERROR_LOG,
        (uint16_t)port,
        NULL, NULL,
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

    /* --- Lifetime watchdog (2026-08-16) --- */
    runner->lifetime_seconds = config->http_dispatcher_lifetime_seconds;
    pthread_mutex_init(&runner->stop_mutex, NULL);
    runner->already_stopped = 0;

    if (pthread_create(&runner->lifetime_thread, NULL,
                        lifetime_watchdog_thread, runner) != 0)
    {
        logger_write(ctx->http_consumer_logger, LOG_ERROR, __func__, 0,
                     "http_consumer_runner_start: pthread_create failed "
                     "for the lifetime watchdog - the daemon IS running, "
                     "but http_consumer_runner_join() will return "
                     "immediately and http_dispatcher.lifetime_seconds "
                     "will not be enforced. Call http_consumer_runner_"
                     "stop() explicitly to shut it down.");
        runner->lifetime_thread_started = 0;
    }
    else
    {
        runner->lifetime_thread_started = 1;
        logger_write(ctx->http_consumer_logger, LOG_INFO, __func__, 0,
                     "HTTP Consumer: lifetime watchdog started "
                     "(http_dispatcher.lifetime_seconds=%d, %s)",
                     runner->lifetime_seconds,
                     runner->lifetime_seconds > 0
                         ? "will self-stop when elapsed"
                         : "runs forever - external stop only");
    }

    return runner;
}

void http_consumer_runner_join(http_consumer_runner_t *runner)
{
    if (!runner) return;

    if (runner->lifetime_thread_started)
        pthread_join(runner->lifetime_thread, NULL);
}

void http_consumer_runner_stop(http_consumer_runner_t *runner)
{
    if (!runner) return;

    stop_daemon_once(runner, 0);

    pthread_mutex_destroy(&runner->stop_mutex);
    free(runner->hctx);
    free(runner->tls_cert);
    free(runner->tls_key);
    free(runner);
}
