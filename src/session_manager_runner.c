/* ======================================================================
 * session_manager_runner.c
 *
 * See session_manager_runner.h for the full design rationale.
 * ====================================================================== */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "session_manager_runner.h"
#include "OCI_Session_Manager.h"
#include "OCI_Connection_Pool.h"
#include "ctx_utils.h"
#include "logger.h"

struct session_manager_runner {
    pthread_t thread;
};

typedef struct {
    oci_context_t         *base_ctx;
    generic_queue_t *q;
} runner_thread_args_t;

static void *runner_thread_main(void *arg)
{
    runner_thread_args_t args = *(runner_thread_args_t *)arg;
    free(arg);

    oci_context_t *base_ctx = args.base_ctx;
    generic_queue_t *q = args.q;

    /* Same pattern as worker.c/file_consumer_runner.c - borrow an
     * independent pooled session at thread start, hold it for this
     * thread's whole lifetime.                                        */
    oci_context_t thread_ctx;
    memset(&thread_ctx, 0, sizeof(thread_ctx));

    if (OCI_Pool_get_session(base_ctx, &thread_ctx) != 0)
    {
        logger_write(base_ctx->session_logger, LOG_ERROR, __func__, 0,
                     "Session Manager thread: OCI_Pool_get_session failed "
                     "- this thread cannot start, session activity will "
                     "never be persisted to the table until the process "
                     "restarts (session_touch()'s own cache-only refresh "
                     "is unaffected - see OCI_Session_Manager.h)");
        return NULL;
    }

    copy_shared_ctx_fields(&thread_ctx, base_ctx);
    thread_ctx.active_tx = NULL;   /* no managed transaction - each touch
                                       self-commits, same reasoning as
                                       every other worker's own session */

    logger_write(thread_ctx.session_logger, LOG_INFO, __func__, 0,
                 "Session Manager thread started - session borrowed, "
                 "draining touch queue");

    int touched = 0;
    char *session_id;

    while ((session_id = (char *)generic_queue_dequeue_blocking(q)) != NULL)
    {
        int cache_rc = session_touch(&thread_ctx, session_id);
        if (cache_rc != SESSION_OK)
            logger_write(thread_ctx.session_logger, LOG_WARN, __func__, 0,
                         "Session Manager: session_touch() (cache) "
                         "returned %d for session_id=%s", cache_rc, session_id);

        int db_rc = session_touch_db(&thread_ctx, session_id);
        if (db_rc != SESSION_OK)
            logger_write(thread_ctx.session_logger, LOG_WARN, __func__, 0,
                         "Session Manager: session_touch_db() (table) "
                         "returned %d for session_id=%s - table sync "
                         "missed this cycle, cache is still current",
                         db_rc, session_id);

        free(session_id);
        touched++;
    }

    logger_write(thread_ctx.session_logger, LOG_INFO, __func__, 0,
                 "Session Manager thread exiting after %d touch(es) - "
                 "releasing session", touched);

    OCI_Pool_release_session(base_ctx, &thread_ctx);

    return NULL;
}

session_manager_runner_t *session_manager_runner_start(oci_context_t         *base_ctx,
                                                         generic_queue_t *q)
{
    session_manager_runner_t *runner = malloc(sizeof(session_manager_runner_t));
    if (!runner) return NULL;

    runner_thread_args_t *args = malloc(sizeof(runner_thread_args_t));
    if (!args) { free(runner); return NULL; }

    args->base_ctx = base_ctx;
    args->q        = q;

    if (pthread_create(&runner->thread, NULL, runner_thread_main, args) != 0)
    {
        logger_write(base_ctx->session_logger, LOG_ERROR, __func__, 0,
                     "session_manager_runner_start: pthread_create failed");
        free(args);
        free(runner);
        return NULL;
    }

    return runner;
}

void session_manager_runner_stop_and_join(session_manager_runner_t *runner,
                                           generic_queue_t          *q)
{
    if (!runner) return;

    generic_queue_shutdown(q);
    pthread_join(runner->thread, NULL);

    free(runner);
}
