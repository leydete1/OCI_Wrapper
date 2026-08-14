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
#include "OCI_Session_Manager.h"
#include "OCI_Connection_Pool.h"
#include "ctx_utils.h"
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

/* Session Manager proposal, Stage 1 (2026-08-06): File Consumer's own
 * current session for this run - one shared session_id stamped onto
 * every RequestObject regardless of which worker eventually processes
 * it (confirmed design, not per-worker). Only the File Consumer thread
 * itself ever reads or writes this, so - unlike g_stop_requested above -
 * no volatile/lock/thread-local is needed here at all.                */
static char g_current_session_id[SESSION_UUID_LEN] = "";

/* Extract <session_id>...</session_id> from session_create()'s own
 * result_xml. Same established pattern already used elsewhere in this
 * project (OCI_Unit_Test_Module.c) rather than adding a new out-param
 * to session_create() itself for one caller. Returns 1 on success, 0
 * if the tag isn't found or doesn't fit in out.                       */
static int extract_session_id_from_xml(const char *xml, char *out, size_t out_size)
{
    if (!xml) return 0;
    const char *tag_start = strstr(xml, "<session_id>");
    if (!tag_start) return 0;
    tag_start += strlen("<session_id>");
    const char *tag_end = strstr(tag_start, "</session_id>");
    if (!tag_end) return 0;
    size_t len = (size_t)(tag_end - tag_start);
    if (len >= out_size) return 0;
    memcpy(out, tag_start, len);
    out[len] = '\0';
    return 1;
}

/* Ensure g_current_session_id holds a valid session before this scan
 * pass, creating a fresh one if it's empty or has genuinely expired -
 * this is what gives the "10am batch, then a long gap, then a fresh
 * session at 20:00" behaviour from the proposal. The common case (an
 * already-valid session) is a single cache-only session_validate()
 * call - cheap, no DB hit.
 *
 * On any failure to create/extract a fresh session, leaves
 * g_current_session_id empty rather than stale - file_consumer_scan_once()
 * treats an empty session_id the same as before this stage existed
 * (no override applied), so a session-creation hiccup degrades to
 * "old behaviour for this pass" rather than silently reusing a session
 * that might not actually be valid.                                    */
static void ensure_valid_session(oci_context_t *ctx)
{
    if (g_current_session_id[0] &&
        session_validate(ctx, g_current_session_id, NULL) == SESSION_OK)
        return;   /* still good - nothing to do */

    if (g_current_session_id[0])
        logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                     "File Consumer: session '%s' no longer valid - "
                     "creating a fresh one", g_current_session_id);

    session_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.operation, "CREATE_SESSION", sizeof(req.operation) - 1);
    strncpy(req.application_name, "File Consumer", sizeof(req.application_name) - 1);
    req.requested_ttl_seconds = 0;   /* use session_default_ttl_seconds */

    char *result_xml = NULL;
    int rc = session_create(ctx, &req, &result_xml);

    if (rc != SESSION_OK || !result_xml)
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "File Consumer: session_create() failed (rc=%d) - "
                     "this pass's requests will not carry a session_id "
                     "override", rc);
        g_current_session_id[0] = '\0';
        free(result_xml);
        return;
    }

    if (!extract_session_id_from_xml(result_xml, g_current_session_id,
                                      sizeof(g_current_session_id)))
    {
        logger_write(ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "File Consumer: session_create() succeeded but its "
                     "own result XML didn't contain a <session_id> - "
                     "this pass's requests will not carry a session_id "
                     "override");
        g_current_session_id[0] = '\0';
    }
    else
    {
        logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                     "File Consumer: created session '%s'",
                     g_current_session_id);
    }

    free(result_xml);
}

static void *runner_thread_main(void *arg)
{
    runner_thread_args_t args = *(runner_thread_args_t *)arg;
    free(arg);

    oci_context_t *base_ctx = args.ctx;
    app_config_t  *config   = args.config;
    queue_manager_t *qm     = args.qm;

    /* Bug fix (2026-08-06): this thread never borrowed its own DB
     * session before - it never needed one prior to the Session
     * Manager proposal's Stage 1 (this thread was pure filesystem +
     * logging until ensure_valid_session() started calling
     * session_create()/session_validate()). Using base_ctx directly for
     * those calls was the actual bug behind every File-Consumer-
     * triggered session_create() failing with SESSION_ERR_DB_FAILURE:
     * base_ctx's own bootstrap session is explicitly released back to
     * the pool in main() (OCI_Pool_release_session()) BEFORE this
     * thread is even started, so by the time this thread tried to use
     * it, there was no valid OCI session behind it at all. Same fix
     * shape as every worker thread already uses (worker.c) - borrow an
     * independent session here, at thread start, hold it for this
     * thread's whole lifetime, release it when the thread stops.       */
    oci_context_t thread_ctx;
    memset(&thread_ctx, 0, sizeof(thread_ctx));

    if (OCI_Pool_get_session(base_ctx, &thread_ctx) != 0)
    {
        logger_write(base_ctx->file_consumer_logger, LOG_ERROR, __func__, 0,
                     "File Consumer thread: OCI_Pool_get_session failed - "
                     "this thread cannot start, no files will ever be "
                     "processed until the process restarts");
        return NULL;
    }

    copy_shared_ctx_fields(&thread_ctx, base_ctx);
    thread_ctx.active_tx = NULL;   /* no managed transaction - matches
                                       every worker thread's own session,
                                       each request self-commits          */

    oci_context_t *ctx = &thread_ctx;   /* everything below uses this,
                                            not base_ctx, from here on   */

    int poll_interval = config->dispatcher_poll_interval_seconds > 0
                         ? config->dispatcher_poll_interval_seconds : 5;
    int lifetime = config->dispatcher_lifetime_seconds;   /* 0 = forever */

    time_t start_time = time(NULL);

    logger_write(ctx->file_consumer_logger, LOG_INFO, __func__, 0,
                 "File Consumer thread started - session borrowed, "
                 "poll_interval=%ds lifetime=%s", poll_interval,
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

        ensure_valid_session(ctx);

        int rc = file_consumer_scan_once(ctx, config, qm, g_current_session_id);

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
                 "File Consumer thread exiting after %d scan pass(es) - "
                 "releasing session", pass);

    OCI_Pool_release_session(base_ctx, &thread_ctx);

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
