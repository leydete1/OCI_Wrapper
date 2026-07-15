
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

    fprintf(logger->file,
            "%s.%06ld [%s] [T%d] [%s] %s\n",
            buffer,
            ts.tv_nsec / 1000,
            level_str,
            thread_id,
            func,
            msg);
    fflush(logger->file);

    /*This guard is needed incase the error_logger is null*/
    if (level == LOG_ERROR && logger->error_logger)
    {
        fprintf(logger->error_logger->file,
                "%s.%06ld [%s] [T%d] [%s] %s\n",
                buffer,
                ts.tv_nsec / 1000,
                level_str,
                thread_id,
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
