
#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>

#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3

typedef struct logger{
    FILE   *file;
    char   *filename;
    double  max_size;      /* in bytes                                   */
    int     rotation_count;
    int     level;
    struct  logger *error_logger;
} logger_t;


/*
 * logger_init()
 *
 * Initialise a logger with a pre-resolved numeric level.
 * Existing callers are unchanged.
 *
 * Parameters
 *   logger         - logger_t to initialise
 *   filename       - log file path
 *   max_size       - max file size in bytes before rotation
 *   rotation_count - number of rotated files to keep
 *   level          - LOG_DEBUG / LOG_INFO / LOG_WARN / LOG_ERROR
 *
 * Returns  0 on success, -1 if the file cannot be opened.
 */
int logger_init(logger_t   *logger,
                const char *filename,
                double      max_size,
                int         rotation_count,
                int         level);

int logger_init2(logger_t   *logger,
                const char *filename,
                double      max_size,
                int         rotation_count,
                int         level,
				logger_t   *error_logger);


/*
 * logger_init_str()
 *
 * Initialise a logger from a human-readable level string.
 * Translates the string to a numeric level then calls logger_init().
 * This is the preferred entry point when reading from config.ini so
 * that the string-to-numeric translation always lives in one place.
 *
 * level_str accepts (case-insensitive):
 *   "DEBUG"  ->  LOG_DEBUG  (0)
 *   "INFO"   ->  LOG_INFO   (1)
 *   "WARN"   ->  LOG_WARN   (2)
 *   "ERROR"  ->  LOG_ERROR  (3)
 *   NULL or unrecognised -> LOG_DEBUG
 *
 * Returns  0 on success, -1 if the file cannot be opened.
 */
int logger_init_str(logger_t   *logger,
                    const char *filename,
                    double      max_size,
                    int         rotation_count,
                    const char *level_str);


int logger_init_str2(logger_t   *logger,
                    const char *filename,
                    double      max_size,
                    int         rotation_count,
                    const char *level_str,
		    logger_t   *error_logger);

/*
 * logger_parse_level()
 *
 * Translate a log level string to its numeric constant.
 * Exposed so callers that store the numeric value (e.g. app_config_t
 * log_level_num fields) can populate it without duplicating logic.
 *
 * Returns LOG_DEBUG (0) for NULL or unrecognised input.
 */
int logger_parse_level(const char *level_str);

/* Write to log */
void logger_write(logger_t   *logger,
                  int         level,
                  const char *func,
                  int         thread_id,
                  const char *fmt, ...);

/* ------------------------------------------------------------------ */
/*  Trace context (session_id / transaction_id) - 2026-08-06            */
/*                                                                      */
/*  Point-in-time capture only, for tracing/debugging across the        */
/*  ~500 existing logger_write() call sites without changing that       */
/*  function's signature or touching any of them. Storage is __thread   */
/*  (thread-local) - each worker thread has its own private sid/txid,   */
/*  set/cleared as that thread moves through a request, with no         */
/*  locking needed since no other thread can ever see or touch it.      */
/*  logger_write() appends "sid=... txid=..." to the already-formatted  */
/*  message for whichever of these the calling thread currently has     */
/*  set, gated by the single global toggle below.                       */
/* ------------------------------------------------------------------ */

/*
 * logger_set_include_trace_context()
 *
 * Single switch applying to every logger, not per-logger-instance -
 * set once at startup from config.ini's log_include_trace_context
 * (default on), before any worker threads exist. Safe as a plain
 * global (not __thread) because it's write-once-then-read-only for
 * the rest of the process lifetime.
 */
void logger_set_include_trace_context(int enabled);

/*
 * logger_set_sid() / logger_clear_sid()
 * logger_set_txid() / logger_clear_txid()
 *
 * Set/clear the calling thread's own session_id / transaction_id for
 * log-line tracing. NULL is equivalent to clearing. Each is silently
 * truncated to fit its internal buffer if longer than expected - never
 * a fatal condition, this is a debugging aid, not a correctness path.
 */
void logger_set_sid  (const char *sid);
void logger_clear_sid(void);
void logger_set_txid  (const char *txid);
void logger_clear_txid(void);

/* Close logger */
void logger_close(logger_t *logger);

/* ------------------------------------------------------------------ */
/*  Last error slot                                                     */
/*                                                                      */
/*  Populated automatically by logger_write() on every LOG_ERROR call. */
/*  Execute modules read this into metrics.error_code / error_text     */
/*  just before metrics_write().  The last error wins - adequate for   */
/*  the single-connection, single-thread module design.                 */
/*                                                                      */
/*  Usage in execute modules (copy this block before metrics_write):   */
/*                                                                      */
/*    strncpy(metrics.error_code,                                       */
/*            logger_last_error.error_code,                             */
/*            sizeof(metrics.error_code) - 1);                         */
/*    strncpy(metrics.error_text,                                       */
/*            logger_last_error.error_text,                             */
/*            sizeof(metrics.error_text) - 1);                         */
/*    logger_clear_last_error();   // reset for next operation          */
/*                                                                      */
/*  On the success path keep the existing "-" defaults and do NOT       */
/*  copy from logger_last_error - only copy on rc != 0.                */
/* ------------------------------------------------------------------ */
typedef struct {
    char error_code [64];    /* function name where error was logged    */
    char error_text [256];   /* formatted error message                 */
} logger_last_error_t;

extern logger_last_error_t logger_last_error;

/*
 * logger_clear_last_error()
 * Reset the last error slot to "-" defaults.
 * Call at the start of each operation so stale errors do not bleed
 * across requests.
 */
void logger_clear_last_error(void);

#endif /* LOGGER_H */
