/*
 * metrics.c
 *
 * Independent Metrics Module - Implementation
 * --------------------------------------------
 * See metrics.h for the full design description and usage pattern.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>       /* gethostname(), getpid()  */
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>      /* pthread_self()            */

#include "OCI_Connection.h"
#include "metrics.h"
#include "logger.h"   /* logger_get_worker_id() - closure item 5, Stage 2 */

/* ------------------------------------------------------------------ */
/*  Stage 5 (File_Consumer_proposal v1.2) - metrics.c thread safety.
 *
 *  metrics_write() previously had an unguarded ftell()-then-fprintf()
 *  sequence: two threads could both see pos==0 and both write the CSV
 *  header (or interleave a header write with a row write from another
 *  thread), and nothing serialised the row-write fprintf() itself
 *  against a concurrent header-write from another caller. Flagged as a
 *  hard prerequisite for Stage 5 back when the File Consumer project
 *  started - now that worker threads genuinely run concurrently, this
 *  fixes it before it can bite.
 *
 *  Same approach logger.c already uses for its own log_mutex: one
 *  static mutex guarding the entire write (header-check included) as
 *  a single critical section per call, rather than trying to protect
 *  the ftell() and fprintf() as separate finer-grained pieces - the
 *  header-check-then-maybe-write and the row-write need to be atomic
 *  together, not just individually safe.
 * ------------------------------------------------------------------ */
static pthread_mutex_t metrics_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  Internal: CSV-safe string                                           */
/*  If the value contains a comma or double-quote, wrap in quotes and  */
/*  escape internal double-quotes.  dest must be at least dest_max.   */
/* ------------------------------------------------------------------ */
static void csv_field(char *dest, size_t dest_max, const char *src)
{
    if (!src || src[0] == '\0')
    {
        strncpy(dest, "-", dest_max - 1);
        dest[dest_max - 1] = '\0';
        return;
    }

    /* Check if quoting is needed */
    int needs_quote = 0;
    for (const char *p = src; *p; p++)
    {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
        {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote)
    {
        strncpy(dest, src, dest_max - 1);
        dest[dest_max - 1] = '\0';
        return;
    }

    /* Wrap in double-quotes, escaping internal double-quotes as "" */
    size_t wi = 0;
    if (wi < dest_max - 1) dest[wi++] = '"';
    for (const char *p = src; *p && wi < dest_max - 3; p++)
    {
        if (*p == '"')
        {
            dest[wi++] = '"';
            dest[wi++] = '"';
        }
        else
        {
            dest[wi++] = *p;
        }
    }
    if (wi < dest_max - 1) dest[wi++] = '"';
    dest[wi] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Internal: XML escape for CSV                                        */
/*  Escapes XML special characters AND commas/newlines so the result   */
/*  is safe to embed as a single unquoted CSV field.                   */
/*  Returns a heap-allocated string - caller must free().              */
/*  Returns strdup("-") on NULL/empty input or allocation failure.     */
/* ------------------------------------------------------------------ */
char *xml_escape_for_csv(const char *xml)
{
    if (!xml || xml[0] == '\0')
        return strdup("-");

    /* Count extra bytes needed for escape sequences */
    size_t extra = 0;
    for (const char *p = xml; *p; p++)
    {
        switch (*p)
        {
            case '&':  extra += 4; break;   /* &amp;  (5 chars - 1) */
            case '<':  extra += 3; break;   /* &lt;   (4 chars - 1) */
            case '>':  extra += 3; break;   /* &gt;   (4 chars - 1) */
            case '"':  extra += 5; break;   /* &quot; (6 chars - 1) */
            case '\'': extra += 5; break;   /* &apos; (6 chars - 1) */
            case ',':  extra += 4; break;   /* &#44;  (5 chars - 1) */
            case '\n': extra += 4; break;   /* &#10;  (5 chars - 1) */
            case '\r': extra += 4; break;   /* &#13;  (5 chars - 1) */
            default:   break;
        }
    }

    size_t len = strlen(xml) + extra + 1;
    char  *out = malloc(len);
    if (!out) return strdup("-");

    char *q = out;
    for (const char *p = xml; *p; p++)
    {
        switch (*p)
        {
            case '&':  memcpy(q, "&amp;",  5); q += 5; break;
            case '<':  memcpy(q, "&lt;",   4); q += 4; break;
            case '>':  memcpy(q, "&gt;",   4); q += 4; break;
            case '"':  memcpy(q, "&quot;", 6); q += 6; break;
            case '\'': memcpy(q, "&apos;", 6); q += 6; break;
            case ',':  memcpy(q, "&#44;",  5); q += 5; break;
            case '\n': memcpy(q, "&#10;",  5); q += 5; break;
            case '\r': memcpy(q, "&#13;",  5); q += 5; break;
            default:   *q++ = *p;              break;
        }
    }
    *q = '\0';
    return out;
}

/* ================================================================== */
/*  metrics_now_us                                                      */
/* ================================================================== */
uint64_t metrics_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL +
           (uint64_t)(ts.tv_nsec / 1000);
}

/* ================================================================== */
/*  metrics_init                                                        */
/* ================================================================== */
void metrics_init(metrics_record_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));

    /* Set all string fields to not-available placeholder */
    strncpy(m->session_id,       "-", sizeof(m->session_id)       - 1);
    strncpy(m->transaction_id,   "-", sizeof(m->transaction_id)   - 1);
    strncpy(m->transaction_name, "-", sizeof(m->transaction_name) - 1);
    strncpy(m->audit_id,         "-", sizeof(m->audit_id)         - 1);
    strncpy(m->consumer_name,    "-", sizeof(m->consumer_name)    - 1);
    strncpy(m->client_ip,        "-", sizeof(m->client_ip)        - 1);
    strncpy(m->host_name,        "-", sizeof(m->host_name)        - 1);
    strncpy(m->server_ip,        "-", sizeof(m->server_ip)        - 1);
    strncpy(m->datasource_name,  "-", sizeof(m->datasource_name)  - 1);
    strncpy(m->operation,        "-", sizeof(m->operation)        - 1);
    strncpy(m->object_name,      "-", sizeof(m->object_name)      - 1);
    strncpy(m->error_code,       "-", sizeof(m->error_code)       - 1);
    strncpy(m->error_text,       "-", sizeof(m->error_text)       - 1);

    /* Numeric fields default to 0                                     */
    /* Optional display pointers start NULL - set by caller if needed  */
    m->input_file_name = NULL;
    m->input_request   = NULL;
    m->output_response = NULL;
}

/* ================================================================== */
/*  resolve_server_ip                                                   */
/*  Resolves and caches this machine's own IP address once per process.*/
/*  getaddrinfo() is a real (if usually fast) lookup - not worth        */
/*  repeating on every single metrics row. Tries exactly once; on any  */
/*  failure the cached value stays "-" and every row just gets that,   */
/*  rather than retrying and risking a slow/unreliable resolver on the */
/*  hot path of every DB operation.                                     */
/* ================================================================== */
static char cached_server_ip[64]  = "-";
static int  server_ip_resolved    = 0;

static void resolve_server_ip(const char *hostname)
{
    if (server_ip_resolved) return;
    server_ip_resolved = 1;   /* only ever try once, success or fail   */

    if (!hostname || !hostname[0]) return;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || !res)
        return;   /* leave cached_server_ip as "-"                     */

    void *addr = NULL;
    if (res->ai_family == AF_INET)
        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    else if (res->ai_family == AF_INET6)
        addr = &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;

    if (addr)
        inet_ntop(res->ai_family, addr, cached_server_ip,
                  sizeof(cached_server_ip));

    freeaddrinfo(res);
}

/* ================================================================== */
/*  metrics_set_context                                                 */
/* ================================================================== */
void metrics_set_context(metrics_record_t *m,
                          struct oci_context_t *ctx)
{
    if (!m || !ctx) return;

    /* Host name + this server's own IP */
    char hname[128] = {0};
    if (gethostname(hname, sizeof(hname) - 1) == 0 && hname[0])
    {
        strncpy(m->host_name, hname, sizeof(m->host_name) - 1);
        resolve_server_ip(hname);
    }
    strncpy(m->server_ip, cached_server_ip, sizeof(m->server_ip) - 1);

    /* Datasource from ini */
    if (ctx->ini && ctx->ini->dbname[0])
        strncpy(m->datasource_name, ctx->ini->dbname,
                sizeof(m->datasource_name) - 1);

    /* Metrics refactor (closure item 5), Stage 2 (2026-08-09) - this
     * consumer instance's own declared identity, so a dashboard can
     * tell which consumer produced this row.                          */
    if (ctx->ini && ctx->ini->consumer_name[0])
        strncpy(m->consumer_name, ctx->ini->consumer_name,
                sizeof(m->consumer_name) - 1);

    /* Process and thread IDs */
    m->process_id = (uint32_t)getpid();

    /* Fix (closure item 5, Stage 2, 2026-08-09): this used to be
     * (uint32_t)(uintptr_t)pthread_self() - the raw, opaque OS thread
     * handle. That's technically "a thread identifier", but it has no
     * relationship to the [T%d] tag the same thread's own log lines
     * show - a metrics row and its own log lines couldn't be
     * correlated by thread at all. logger_get_worker_id() returns the
     * exact same clean 0-4 worker number already used everywhere else
     * for this thread, matching what [T%d] actually shows.            */
    m->thread_id  = (uint32_t)logger_get_worker_id();

    /* Pool slot index as connection_id / pool_id */
    if (ctx->pool_slot_index >= 0)
    {
        m->connection_id = (uint32_t)ctx->pool_slot_index;
        m->pool_id       = (uint32_t)ctx->pool_slot_index;
    }

    /* Default Oracle port */
    m->server_port = 1521;

    /* Parse-stage timing, if this request went through the pipeline
     * that measures it (see oci_context_t's level1_parse_us /
     * level2_parse_us for what sets these and when they're 0).         */
    m->level1_parse_us = ctx->level1_parse_us;
    m->level2_parse_us = ctx->level2_parse_us;

    /* Session context - populated once session_create() has attached a
     * session to this ctx (mirrors ctx->active_tx for transaction_id).
     * Left at "-" (set in metrics_init) if no session is attached.     */
    if (ctx->active_session_id[0])
        strncpy(m->session_id, ctx->active_session_id,
                sizeof(m->session_id) - 1);

    /* client_ip: real value once a session is attached and carried its
     * own client_ip; stubbed to 127.0.0.1 otherwise, pending the HTTP
     * input module which will supply this per-request directly.       */
    if (ctx->active_client_ip[0])
        strncpy(m->client_ip, ctx->active_client_ip,
                sizeof(m->client_ip) - 1);
    else
        strncpy(m->client_ip, "127.0.0.1", sizeof(m->client_ip) - 1);

    /* audit_id: stubbed as "-" (set in metrics_init) until the HTTP
     * input module provides and validates it per-request.             */
}

/* ================================================================== */
/*  metrics_finalise                                                    */
/* ================================================================== */
void metrics_finalise(metrics_record_t *m)
{
    if (!m) return;

    if (m->end_time_us > m->start_time_us)
        m->total_us = m->end_time_us - m->start_time_us;
    else
        m->total_us = 0;

    /* Compute bytes_processed as sum of the three components          */
    m->bytes_processed = m->output_xml_bytes
                       + m->clob_bytes
                       + m->lob_bytes;
}

/* ================================================================== */
/*  metrics_write                                                       */
/* ================================================================== */
/*
 * metrics_format_timestamp_us()
 *
 * Formats a raw microsecond-epoch value as "YYYY-MM-DD HH24:MI:SS.FFFFFF".
 * Extracted (closure item 5, Stage 3, 2026-08-09) from what used to be
 * inline, duplicated logic inside metrics_write() below (once for
 * start_time_us, once for end_time_us) - now shared with
 * metrics_db_bulk_insert() (metrics_writer.c), which needs the exact
 * same conversion to build a string Oracle's own TO_TIMESTAMP can
 * parse. buf_size should be at least 48 bytes; us == 0 writes "-".
 */
void metrics_format_timestamp_us(uint64_t us, char *buf, size_t buf_size)
{
    strncpy(buf, "-", buf_size - 1);
    buf[buf_size - 1] = '\0';

    if (us == 0) return;

    time_t sec  = (time_t)(us / 1000000ULL);
    uint32_t usec = (uint32_t)(us % 1000000ULL);
    struct tm *tm_info = gmtime(&sec);
    if (tm_info)
        snprintf(buf, buf_size,
                 "%04d-%02d-%02d %02d:%02d:%02d.%06u",
                 tm_info->tm_year + 1900,
                 tm_info->tm_mon  + 1,
                 tm_info->tm_mday,
                 tm_info->tm_hour,
                 tm_info->tm_min,
                 tm_info->tm_sec,
                 usec);
}

void metrics_write(logger_t         *metrics_logger,
                   metrics_record_t *m)
{
    if (!metrics_logger || !m) return;

    pthread_mutex_lock(&metrics_mutex);

    /* ---- Write header if the log file is empty ---- */
    if (metrics_logger->file)
    {
        long pos = ftell(metrics_logger->file);
        if (pos == 0)
        {
            fprintf(metrics_logger->file,
                    "%s\n", METRICS_CSV_HEADER);
            fflush(metrics_logger->file);
        }
    }

    /* ---- Build CSV row ---- */
    /*
     * Each string field passes through csv_field() to handle commas
     * and quotes safely.  Numeric fields are formatted directly.
     * Connection metric fields not yet wired are written as "-".
     */

    /* Individual escape buffers - one per string field.
     * A single shared buffer cannot be used here because fprintf()
     * evaluates all arguments before writing and the evaluation order
     * is unspecified in C, so every WF() call would overwrite the
     * same buffer and only the last value evaluated would survive.    */
    char f_session_id       [512];
    char f_transaction_id   [512];
    char f_transaction_name [512];
    char f_audit_id         [512];
    char f_consumer_name    [512];
    char f_client_ip        [512];
    char f_host_name        [512];
    char f_server_ip        [512];
    char f_datasource       [512];
    char f_operation        [512];
    char f_object_name      [512];
    char f_error_code       [512];
    char f_error_text       [512];

    csv_field(f_session_id,       sizeof(f_session_id),       m->session_id);
    csv_field(f_transaction_id,   sizeof(f_transaction_id),   m->transaction_id);
    csv_field(f_transaction_name, sizeof(f_transaction_name), m->transaction_name);
    csv_field(f_audit_id,         sizeof(f_audit_id),         m->audit_id);
    csv_field(f_consumer_name,    sizeof(f_consumer_name),    m->consumer_name);
    csv_field(f_client_ip,        sizeof(f_client_ip),        m->client_ip);
    csv_field(f_host_name,        sizeof(f_host_name),        m->host_name);
    csv_field(f_server_ip,        sizeof(f_server_ip),        m->server_ip);
    csv_field(f_datasource,       sizeof(f_datasource),       m->datasource_name);
    csv_field(f_operation,        sizeof(f_operation),        m->operation);
    csv_field(f_object_name,      sizeof(f_object_name),      m->object_name);
    csv_field(f_error_code,       sizeof(f_error_code),       m->error_code);
    csv_field(f_error_text,       sizeof(f_error_text),       m->error_text);

    /* Format start_time_us as a human-readable timestamp + microseconds
     * so the CSV is readable without a separate converter. Extracted
     * (closure item 5, Stage 3, 2026-08-09) into metrics_format_
     * timestamp_us(), now shared with metrics_db_bulk_insert()
     * (metrics_writer.c) instead of duplicated a third time.           */
    char start_ts[48];
    metrics_format_timestamp_us(m->start_time_us, start_ts, sizeof(start_ts));

    char end_ts[48];
    metrics_format_timestamp_us(m->end_time_us, end_ts, sizeof(end_ts));

    if (metrics_logger->file)
    {
        fprintf(metrics_logger->file,
            "%s,"       /* session_id           */
            "%s,"       /* transaction_id       */
            "%s,"       /* transaction_name     */
            "%s,"       /* audit_id             */
            "%s,"       /* consumer_name        */
            "%s,"       /* client_ip            */
            "%s,"       /* host_name            */
            "%s,"       /* server_ip            */
            "%u,"       /* server_port          */
            "%u,"       /* process_id           */
            "%u,"       /* thread_id            */
            "%s,"       /* datasource_name      */
            "%u,"       /* connection_id        */
            "%u,"       /* pool_id              */
            "%s,"       /* operation            */
            "%s,"       /* object_name          */
            "%llu,"     /* sql_hash             */
            "%llu,"     /* cache_key_hash       */
            "%s,"       /* start_time_us        */
            "%s,"       /* end_time_us          */
            "%llu,"     /* cache_lookup_us      */
            "%llu,"     /* level1_parse_us      */
            "%llu,"     /* level2_parse_us      */
            "%llu,"     /* sql_parse_us         */
            "%llu,"     /* execution_us         */
            "%llu,"     /* total_us             */
            "%llu,"     /* rows_affected        */
            "%llu,"     /* output_xml_bytes     */
            "%llu,"     /* clob_bytes           */
            "%llu,"     /* lob_bytes            */
            "%llu,"     /* bytes_processed      */
            "%d,"       /* cache_hit            */
            "%d,"       /* status_code          */
            "%s,"       /* error_code           */
            "%s,"       /* error_text   */
            "%llu,"     /* connection_wait_us    */
            "%llu,"     /* connection_create_us  */
            "%llu,"     /* connection_acquire_us */
            "%s,"       /* input_file_name       */
            "%s,"       /* input_request         */
            "%s"        /* output_response       */
            "\n",
            f_session_id,
            f_transaction_id,
            f_transaction_name,
            f_audit_id,
            f_consumer_name,
            f_client_ip,
            f_host_name,
            f_server_ip,
            (unsigned)m->server_port,
            (unsigned)m->process_id,
            (unsigned)m->thread_id,
            f_datasource,
            (unsigned)m->connection_id,
            (unsigned)m->pool_id,
            f_operation,
            f_object_name,
            (unsigned long long)m->sql_hash,
            (unsigned long long)m->cache_key_hash,
            start_ts,
            end_ts,
            (unsigned long long)m->cache_lookup_us,
            (unsigned long long)m->level1_parse_us,
            (unsigned long long)m->level2_parse_us,
            (unsigned long long)m->sql_parse_us,
            (unsigned long long)m->execution_us,
            (unsigned long long)m->total_us,
            (unsigned long long)m->rows_affected,
            (unsigned long long)m->output_xml_bytes,
            (unsigned long long)m->clob_bytes,
            (unsigned long long)m->lob_bytes,
            (unsigned long long)m->bytes_processed,
            m->cache_hit,
            m->status_code,
            f_error_code,
            f_error_text,
	            (unsigned long long)m->connection_wait_us,
            (unsigned long long)m->connection_create_us,
            (unsigned long long)m->connection_acquire_us,
            m->input_file_name ? m->input_file_name : "-",
            m->input_request   ? m->input_request   : "-",
            m->output_response ? m->output_response : "-"
        );

        fflush(metrics_logger->file);
    }

    /* ---- Free optional heap fields ---- */
    free(m->input_file_name);  m->input_file_name = NULL;
    free(m->input_request);    m->input_request   = NULL;
    free(m->output_response);  m->output_response  = NULL;

    pthread_mutex_unlock(&metrics_mutex);
}


/*
 * flatten_for_csv()
 * Replace all newlines and carriage returns with a space,
 * collapse multiple spaces to one.
 * Returns a heap-allocated string - caller must free().
 * Returns strdup("-") on NULL/empty input.
 */

char *flatten_for_csv(const char *src)
{
    if (!src || src[0] == '\0')
        return strdup("-");

    size_t len = strlen(src);
    char  *out = malloc(len + 1);
    if (!out) return strdup("-");

    char   *q         = out;
    int     last_space = 0;

    for (const char *p = src; *p; p++)
    {
        if (*p == '\n' || *p == '\r' || *p == '\t')
        {
            if (!last_space) { *q++ = ' '; last_space = 1; }
        }
        else if (*p == ' ')
        {
            if (!last_space) { *q++ = ' '; last_space = 1; }
        }
        else
        {
            *q++ = *p;
            last_space = 0;
        }
    }
    *q = '\0';
    return out;
}



/*
 * flatten_for_csv2()
 * Wraps the string in double quotes and escapes any embedded double
 * quotes as "".  Newlines, carriage returns, commas and all other
 * characters are left as-is since they are safe inside a quoted field.
 * Returns a heap-allocated string - caller must free().
 * Returns strdup("-") on NULL/empty input or allocation failure.
 */
char *flatten_for_csv2(const char *src)
{
    if (!src || src[0] == '\0')
        return strdup("-");

    size_t len = strlen(src);

    /* Worst case every char is a quote so doubles in size,
       plus 2 for surrounding quotes and 1 for null terminator */
    char *out = malloc((len * 2) + 3);
    if (!out) return strdup("-");

    char *q = out;

    *q++ = '"';        /* opening quote */

    for (const char *p = src; *p; p++)
    {
        if (*p == '"')
        {
            *q++ = '"';    /* escape embedded quote as "" */
            *q++ = '"';
        }
        else
        {
            *q++ = *p;
        }
    }

    *q++ = '"';        /* closing quote */
    *q   = '\0';

    return out;
}






/*
 * flatten_for_csv3()
 * Wraps in double quotes, escapes embedded double quotes as "",
 * and replaces newlines/tabs with a single space so the field
 * stays on one line in the CSV.
 */
char *flatten_for_csv3(const char *src)
{
    if (!src || src[0] == '\0')
        return strdup("-");

    size_t len = strlen(src);

    /* Worst case every char is a quote, plus 2 quotes + null */
    char *out = malloc((len * 2) + 3);
    if (!out) return strdup("-");

    char *q = out;

    *q++ = '"';    /* opening quote */

    for (const char *p = src; *p; p++)
    {
        if (*p == '"')
        {
            *q++ = '"';
            *q++ = '"';    /* escape as "" */
        }
        else if (*p == '\n' || *p == '\r' || *p == '\t')
        {
            *q++ = ' ';    /* replace with space */
        }
        else
        {
            *q++ = *p;
        }
    }

    *q++ = '"';    /* closing quote */
    *q   = '\0';

    return out;
}

