/* ======================================================================
 * file_consumer_runner.c
 *
 * See file_consumer_runner.h for the full design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "file_consumer_runner.h"
#include "file_consumer.h"
#include "logger.h"

struct file_consumer_runner {
    pthread_t thread;
};

typedef struct {
    oci_context_t   *ctx;
    app_config_t    *config;
    queue_manager_t *qm;
} runner_thread_args_t;

/* Shared stop flag. A single File Consumer thread per process is the
 * only supported configuration (unlike worker.c's N-threads-one-per-
 * queue design) - one static flag is sufficient and keeps this module
 * simple. Declared volatile since it's set from one thread (main, via
 * file_consumer_runner_stop_and_join()) and read from another (the
 * runner thread itself) without a lock - safe here because it's a
 * single word, monotonic one-way transition (0 -> 1, never back), and
 * the runner thread re-checks it at least once per second regardless,
 * so there's no correctness-critical timing dependency on when exactly
 * the write becomes visible.                                          */
static volatile int g_stop_requested = 0;

static void *runner_thread_main(void *arg)
{
    runner_thread_args_t args = *(runner_thread_args_t *)arg;
    free(arg);

    oci_context_t *ctx    = args.ctx;
    app_config_t  *config = args.config;
    queue_manager_t *qm   = args.qm;

    int poll_interval = config->dispatcher_poll_interval_seconds > 0
                         ? config->dispatcher_poll_interval_seconds : 5;
    int lifetime = config->dispatcher_lifetime_seconds;   /* 0 = forever */

    time_t start_time = time(NULL);

    logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                 "File Consumer thread started - poll_interval=%ds "
                 "lifetime=%s", poll_interval,
                 lifetime > 0 ? "bounded" : "forever (until stopped)");
    if (lifetime > 0)
        logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                     "File Consumer thread will stop itself after %ds",
                     lifetime);

    int pass = 0;

    while (!g_stop_requested)
    {
        pass++;
        logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                     "File Consumer scan pass %d", pass);

        int rc = file_consumer_scan_once(ctx, config, qm);

        if (rc < 0)
            logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                         "File Consumer scan pass %d failed - see log "
                         "above", pass);
        else
            logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                         "File Consumer scan pass %d complete - %d "
                         "file(s) enqueued", pass, rc);

        if (lifetime > 0 && (time(NULL) - start_time) >= lifetime)
        {
            logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                         "File Consumer thread: lifetime of %ds elapsed - "
                         "stopping", lifetime);
            break;
        }

        /* Sleep in 1-second increments so a stop request lands quickly
         * rather than waiting out the full poll interval.              */
        for (int waited = 0; waited < poll_interval && !g_stop_requested; waited++)
            sleep(1);
    }

    logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                 "File Consumer thread exiting after %d scan pass(es)",
                 pass);

    return NULL;
}

file_consumer_runner_t *file_consumer_runner_start(oci_context_t   *ctx,
                                                     app_config_t    *config,
                                                     queue_manager_t *qm)
{
    file_consumer_runner_t *runner = malloc(sizeof(file_consumer_runner_t));
    if (!runner) return NULL;

    runner_thread_args_t *args = malloc(sizeof(runner_thread_args_t));
    if (!args) { free(runner); return NULL; }

    args->ctx    = ctx;
    args->config = config;
    args->qm     = qm;

    g_stop_requested = 0;

    if (pthread_create(&runner->thread, NULL, runner_thread_main, args) != 0)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "file_consumer_runner_start: pthread_create failed");
        free(args);
        free(runner);
        return NULL;
    }

    return runner;
}

void file_consumer_runner_stop_and_join(file_consumer_runner_t *runner)
{
    if (!runner) return;

    g_stop_requested = 1;
    pthread_join(runner->thread, NULL);

    free(runner);
}

void file_consumer_runner_join(file_consumer_runner_t *runner)
{
    if (!runner) return;

    /* Deliberately does NOT set g_stop_requested - this just waits for
     * the thread to finish on its own (lifetime elapsing, or an
     * external kill), unlike stop_and_join() above which actively asks
     * it to stop. Calling stop_and_join() here instead was the actual
     * bug behind the "exits after 0 scan passes" report on 2026-08-05 -
     * it told the thread to stop within microseconds of starting it. */
    pthread_join(runner->thread, NULL);

    free(runner);
}
