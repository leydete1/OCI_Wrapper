
#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <strings.h>
#include "logger.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  Trace context (session_id / transaction_id) - 2026-08-06            */
/*  See logger.h's own doc comment for the full design rationale.       */
/* ------------------------------------------------------------------ */
static __thread char g_trace_sid[128]  = "";
static __thread char g_trace_txid[64]  = "";

/* Thread ID / worker ID display fix (2026-08-07) - the thread_id
 * PARAMETER logger_write() already takes has been hardcoded to a
 * literal 0 at nearly every one of its ~500 call sites across the
 * whole codebase for as long as this project has existed - only
 * worker.c's own few direct log lines ("Worker[%d]: ...") ever passed
 * a real value. Everything downstream of worker.c (dispatcher.c, every
 * CRUD execute module) has always shown [T0] regardless of which
 * actual worker thread was running it - confirmed as the direct cause
 * of a false lead during the 2026-08-06/07 ORA-03114 investigation
 * (every failure appeared to be "outside the worker threads" purely
 * because of this mislabeling, not because it actually was).
 *
 * Same fix shape as the sid/txid trace context above, for the exact
 * same reason: a __thread variable set once by worker.c at thread
 * start avoids touching any of those ~500 call sites or changing
 * logger_write()'s signature at all. The thread_id PARAMETER is left
 * in place (removing it would mean touching every call site for no
 * behavioural gain) but is no longer what actually determines the
 * [T%d] tag - see the two fprintf() calls below. Threads that never
 * call logger_set_worker_id() (main, File Consumer, Session Manager -
 * each already has its own dedicated, unshared log file, so [T0]
 * there was never actually ambiguous the way it was in the *shared*
 * per-CRUD-operation logs workers all write to) simply keep the
 * default of 0.                                                       */
static __thread int g_worker_id = 0;

void logger_set_worker_id(int worker_id)
{
    g_worker_id = worker_id;
}

/* Metrics refactor (closure item 5), Stage 2 (2026-08-09) - lets
 * metrics.c stamp the exact same clean worker number (0-4, whatever
 * the calling thread already set via logger_set_worker_id() above)
 * onto every metrics record, instead of building its own separate
 * thread-identification mechanism for something already solved here.
 * See metrics.c's own doc comment on the bug this replaces - the
 * previous metrics_record_t.thread_id was populated from raw
 * pthread_self(), an opaque value with no relationship to the [T%d]
 * tag the same thread's own log lines show.                          */
int logger_get_worker_id(void)
{
    return g_worker_id;
}

/* Global switch, applies to every logger uniformly - deliberately NOT
 * __thread, since it's set once at startup (before any worker thread
 * exists) from config.ini's log_include_trace_context and never
 * written again - safe to read from any thread without a lock.       */
static int g_log_include_trace_context = 1;   /* default ON per Terry's
                                                   own call - leave it on
                                                   for debugging        */

void logger_set_include_trace_context(int enabled)
{
    g_log_include_trace_context = enabled;
}

void logger_set_sid(const char *sid)
{
    if (!sid) { g_trace_sid[0] = '\0'; return; }
    strncpy(g_trace_sid, sid, sizeof(g_trace_sid) - 1);
    g_trace_sid[sizeof(g_trace_sid) - 1] = '\0';
}

void logger_clear_sid(void)
{
    g_trace_sid[0] = '\0';
}

void logger_set_txid(const char *txid)
{
    if (!txid) { g_trace_txid[0] = '\0'; return; }
    strncpy(g_trace_txid, txid, sizeof(g_trace_txid) - 1);
    g_trace_txid[sizeof(g_trace_txid) - 1] = '\0';
}

void logger_clear_txid(void)
{
    g_trace_txid[0] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Global last error slot - written on every LOG_ERROR call           */
/* ------------------------------------------------------------------ */
logger_last_error_t logger_last_error = { "-", "-" };

void logger_clear_last_error(void)
{
    strncpy(logger_last_error.error_code, "-",
            sizeof(logger_last_error.error_code) - 1);
    strncpy(logger_last_error.error_text, "-",
            sizeof(logger_last_error.error_text) - 1);
}

/* ================================================================== */
/*  rotate_log_if_needed                                                */
/*  Called under log_mutex - safe to access logger fields directly.   */
/* ================================================================== */
static void rotate_log_if_needed(logger_t *logger)
{
    if (!logger->file) return;

    struct stat st;
    if (stat(logger->filename, &st) == 0 &&
        st.st_size >= (off_t)logger->max_size)
    {
        fclose(logger->file);

        /* Rotate existing files: .N -> .N+1, file -> .1 */
        for (int i = logger->rotation_count - 1; i >= 0; i--)
        {
            char oldname[512], newname[512];
            if (i == 0)
                snprintf(oldname, sizeof(oldname), "%s", logger->filename);
            else
                snprintf(oldname, sizeof(oldname), "%s.%d",
                         logger->filename, i);

            snprintf(newname, sizeof(newname), "%s.%d",
                     logger->filename, i + 1);

            if (access(oldname, F_OK) == 0)
                rename(oldname, newname);
        }

        logger->file = fopen(logger->filename, "a");
    }
}

/* ================================================================== */
/*  logger_parse_level                                                  */
/*  Translate a level string to its LOG_* numeric constant.            */
/*  This is the single authoritative translation point in the project. */
/*  All callers - logger_init_str, load_ini helpers, etc - delegate    */
/*  here so the mapping never needs to be duplicated anywhere.         */
/* ================================================================== */
int logger_parse_level(const char *level_str)
{
    if (!level_str || level_str[0] == '\0')
        return LOG_DEBUG;

    if (strcasecmp(level_str, "ERROR") == 0) return LOG_ERROR;
    if (strcasecmp(level_str, "WARN")  == 0) return LOG_WARN;
    if (strcasecmp(level_str, "INFO")  == 0) return LOG_INFO;
    if (strcasecmp(level_str, "DEBUG") == 0) return LOG_DEBUG;

    /* Numeric strings passed by mistake - accept them gracefully */
    if (level_str[0] == '3') return LOG_ERROR;
    if (level_str[0] == '2') return LOG_WARN;
    if (level_str[0] == '1') return LOG_INFO;
    if (level_str[0] == '0') return LOG_DEBUG;

    return LOG_DEBUG;   /* safe fallback for any unrecognised value    */
}

/* ================================================================== */
/*  logger_init                                                         */
/*  Existing function - signature unchanged, all callers unaffected.  */
/* ================================================================== */
int logger_init(logger_t   *logger,
                const char *filename,
                double      max_size,
                int         rotation_count,
                int         level)
{
    logger->filename       = strdup(filename);
    logger->max_size       = max_size;
    logger->rotation_count = rotation_count;
    logger->level          = level;

    logger->file = fopen(filename, "a");
    if (!logger->file) return -1;
    return 0;
}



int logger_init2(logger_t   *logger,
                const char *filename,
                double      max_size,
                int         rotation_count,
                int         level,
				logger_t   *error_logger)
{
    logger->filename       = strdup(filename);
    logger->max_size       = max_size;
    logger->rotation_count = rotation_count;
    logger->level          = level;
    logger->error_logger   = error_logger;

    logger->file = fopen(filename, "a");
    if (!logger->file) return -1;
    return 0;
}
/* ================================================================== */
/*  logger_init_str                                                     */
/*  Preferred entry point when reading from config.ini.                */
/*  Translates level_str via logger_parse_level() then delegates to    */
/*  logger_init().  The string-to-numeric translation now lives here   */
/*  permanently - no caller ever needs to do it themselves.            */
/* ================================================================== */
int logger_init_str(logger_t   *logger,
                    const char *filename,
                    double      max_size,
                    int         rotation_count,
                    const char *level_str)
{
    int level = logger_parse_level(level_str);
    //This gaurd is needed to set the last_error init at startup.  If not set logger_write can silently die* /
    logger_clear_last_error();
    return logger_init(logger, filename, max_size, rotation_count, level);
}


int logger_init_str2(logger_t   *logger,
                    const char *filename,
                    double      max_size,
                    int         rotation_count,
                    const char *level_str,
					logger_t   *error_logger)
{
    int level = logger_parse_level(level_str);
    //This gaurd is needed to set the last_error init at startup.  If not set logger_write can silently die
    logger_clear_last_error();
  return logger_init2(logger, filename, max_size, rotation_count, level, error_logger);
}

/* ================================================================== */
/*  logger_write                                                        */
/*  Unchanged from original.                                           */
/* ================================================================== */
void logger_write(logger_t   *logger,
                  int         level,
                  const char *func,
                  int         thread_id,
                  const char *fmt, ...)
{
	/* thread_id kept in the signature to avoid touching ~500 call sites
	 * for no behavioural gain (see g_worker_id's own comment above,
	 * 2026-08-07) - no longer what determines the [T%d] tag, so
	 * explicitly unused here.                                          */
	(void)thread_id;

	int mutex_locked = 0;

	//Gaurds for NULL logger values,  report and ignore,  dont silently die.
	if (logger == NULL) {
	    	printf("Logger is NULL.  Going to Cleanup.\n");
	    	goto Cleanup;
	}

	if (logger->file == NULL) {
	    	printf("Logger has NULL FILE*\n. Going to Cleanup.\n");
	    	goto Cleanup;
	}

	if (func == NULL) {
	    	printf("func == NULL\n. Going to Cleanup.\n");
	    	goto Cleanup;
	}


	if (fmt == NULL) {
	    	printf("fmt == NULL\n. Going to Cleanup.\n");
	    	goto Cleanup;
	}

    if (level < logger->level) return;

    pthread_mutex_lock(&log_mutex);
	mutex_locked = 1;

    rotate_log_if_needed(logger);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_info;
    char msg[4096];


    localtime_r(&ts.tv_sec, &tm_info);

    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_info);
    /*fprintf(logger->file, "%s.%06ld ", buffer, ts.tv_nsec / 1000);*/

    const char *level_str = "INFO";
    if      (level == LOG_DEBUG) level_str = "DEBUG";
    else if (level == LOG_INFO)  level_str = "INFO";
    else if (level == LOG_WARN)  level_str = "WARN";
    else if (level == LOG_ERROR) level_str = "ERROR";

    /*fprintf(logger->file, "[%s] [T%d] [%s] ", level_str, thread_id, func);*/

    va_list args;
    va_start(args, fmt);



    vsnprintf(msg, sizeof(msg),fmt , args);
	//Gaurds for NULL msg values,  report and ignore,  dont silently die.
    if (fmt == NULL)
    {
        printf("logger_write: fmt is NULL\n");
        goto Cleanup;
    }


   va_end(args);

    /* ---- Append trace context (sid/txid), if enabled and set ----
     * Both fprintf() calls below (main logger + mirrored error_logger)
     * read from this same msg buffer, so appending here covers both
     * with no duplicated logic. snprintf with the remaining buffer
     * size is always safe - never overflows even if msg is already
     * close to full from the caller's own format string. Point-in-time
     * capture only, per Terry's own framing - no correctness path
     * depends on this ever being present.                             */
    if (g_log_include_trace_context &&
        (g_trace_sid[0] != '\0' || g_trace_txid[0] != '\0'))
    {
        size_t used = strlen(msg);
        if (used < sizeof(msg) - 1)
        {
            if (g_trace_sid[0] != '\0' && g_trace_txid[0] != '\0')
                snprintf(msg + used, sizeof(msg) - used,
                         " sid=%s txid=%s", g_trace_sid, g_trace_txid);
            else if (g_trace_sid[0] != '\0')
                snprintf(msg + used, sizeof(msg) - used,
                         " sid=%s", g_trace_sid);
            else
                snprintf(msg + used, sizeof(msg) - used,
                         " txid=%s", g_trace_txid);
        }
    }

    fprintf(logger->file,
            "%s.%06ld [%s] [T%d] [%s] %s\n",
            buffer,
            ts.tv_nsec / 1000,
            level_str,
            g_worker_id,
            func,
            msg);
    fflush(logger->file);

    /**Set the last error */
    if (level == LOG_ERROR)
    {
        strncpy(logger_last_error.error_code, "Error",
                sizeof(logger_last_error.error_code) - 1);
        logger_last_error.error_code[sizeof(logger_last_error.error_code) - 1] = '\0';

        strncpy(logger_last_error.error_text, msg,
                sizeof(logger_last_error.error_text) - 1);
        logger_last_error.error_text[sizeof(logger_last_error.error_text) - 1] = '\0';
    }


    /*This guard is needed incase the error_logger is null*/
    if (level == LOG_ERROR && logger->error_logger )
    {

		fprintf(logger->error_logger->file,
					"%s.%06ld [%s] [T%d] [%s] %s\n",
					buffer,
					ts.tv_nsec / 1000,
					level_str,
					g_worker_id,
					func,
					msg);

			fflush(logger->error_logger->file);
    }


Cleanup:


	if ( mutex_locked )
		pthread_mutex_unlock(&log_mutex);

}

/* ================================================================== */
/*  logger_close                                                        */
/*  Unchanged from original.                                           */
/* ================================================================== */
void logger_close(logger_t *logger)
{
    if (logger->file)
    {
        fclose(logger->file);
        logger->file = NULL;
    }
    free(logger->filename);
}
