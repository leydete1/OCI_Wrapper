/*
 * metrics.h
 *
 * Independent Metrics Module
 * ---------------------------
 * Captures per-request execution metrics and writes them as a single
 * CSV row to the metrics log file (metrics_Data_Manager.log).
 *
 * Usage pattern
 * -------------
 *   metrics_record_t m;
 *   metrics_init(&m);                          // zero + defaults
 *
 *   m.start_time_us = metrics_now_us();        // at start of request
 *   // ... do work ...
 *   m.end_time_us   = metrics_now_us();        // after work completes
 *
 *   metrics_set_context(&m, ctx);              // fill from oci_context
 *   strncpy(m.operation,    "SELECT", 15);
 *   strncpy(m.object_name,  "MY_TABLE", 127);
 *   m.rows_affected  = abs_rownum;
 *   m.status_code    = rc;
 *
 *   metrics_finalise(&m);                      // compute derived fields
 *   metrics_write(ctx->metrics_logger, &m);    // append CSV row
 *
 * The CSV header is written once the first time metrics_write() is
 * called on a fresh log file (detected by file position == 0).
 *
 * Connection pool fields (connection_wait_us, connection_create_us,
 * connection_acquire_us) are populated with "-" until the connection
 * pool module is wired in.
 */

#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  metrics_record_t                                                    */
/* ------------------------------------------------------------------ */
typedef struct metrics_record_s
{
    /* ---- Session Context ---- */
    char        session_id        [64];
    char        transaction_id    [64];
    char        transaction_name  [128];  /* business label e.g. "Save Booking" */
    char        audit_id          [64];

    /* ---- Consumer Identity (closure item 5, Stage 2, 2026-08-09) ----
     * This consumer instance's own declared identity (config.ini's
     * consumer_name, e.g. "FILE_CONSUMER_01") - stamped in
     * metrics_set_context(). Distinct from datasource_name below,
     * which identifies the DATABASE, not which consumer produced this
     * row. Essential once HTTP consumer exists alongside File Consumer
     * and a dashboard needs to tell their traffic apart.               */
    char        consumer_name     [64];

    /* ---- Client Information ---- */
    char        client_ip       [64];

    /* ---- Server Information ---- */
    char        host_name       [128];
    char        server_ip       [64];
    uint16_t    server_port;
    uint32_t    process_id;
    uint32_t    thread_id;

    /* ---- Database Environment ---- */
    char        datasource_name [128];
    uint32_t    connection_id;
    uint32_t    pool_id;

    /* ---- Request Information ---- */
    char        operation       [16];   /* SELECT INSERT UPDATE DELETE  */
    char        object_name     [128];  /* table / view name            */
    uint64_t    sql_hash;
    uint64_t    cache_key_hash;

    /* ---- Timing Metrics (microseconds) ---- */
    uint64_t    start_time_us;
    uint64_t    end_time_us;
    uint64_t    cache_lookup_us;

    /* Parse-stage timings. In the overwhelming majority of requests
     * these are negligible next to total_us - but on the rare request
     * where parsing itself is slow (a huge request body, a pathological
     * SQL string, etc.), total_us alone can't tell you which stage was
     * responsible. These three make that visible directly.
     *   level1_parse_us - envelope parsing (level1_parse())
     *   level2_parse_us - per-operation validation (level2_validate())
     *   sql_parse_us    - SQL dependency extraction
     *                     (extract_sql_dependencies())
     * 0 if that stage wasn't measured for this request (e.g. requests
     * that don't go through the new-format pipeline at all).           */
    uint64_t    level1_parse_us;
    uint64_t    level2_parse_us;
    uint64_t    sql_parse_us;

    uint64_t    execution_us;
    uint64_t    total_us;

    /* ---- Workload Metrics ---- */
    uint64_t    rows_affected;
    uint64_t    output_xml_bytes;    /* byte length of OUTPUT_XML document  */
    uint64_t    clob_bytes;          /* total bytes of CLOB data processed  */
    uint64_t    lob_bytes;           /* total bytes of BLOB data processed  */
    uint64_t    bytes_processed;     /* output_xml_bytes + clob_bytes
                                        + lob_bytes                         */

    /* ---- Cache Metrics ---- */
    int         cache_hit;              /* 1 = hit, 0 = miss            */

    /* ---- Execution Result ---- */
    int         status_code;            /* 0 = success                  */
    char        error_code  [32];       /* ORA-xxxxx or empty           */
    char        error_text  [256];

    /* ---- Connection Metrics ---- */
    uint64_t    connection_wait_us;     /* time waiting for pool slot   */
    uint64_t    connection_create_us;   /* time to open new slot        */
    uint64_t    connection_acquire_us;  /* total acquire time           */

    /* ---- Optional display fields (controlled by config.ini flags) ----
     * All three are heap-allocated strings - caller sets via strdup()
     * when the corresponding ini flag is 1.  metrics_write() frees them
     * after writing so the caller never needs to free manually.
     * Leave NULL (from metrics_init memset) when flag is 0.
     *
     * input_request / output_response hold the request/response body in
     * whatever format it actually was - XML or JSON - not necessarily
     * XML despite the "xml" name these used to carry; renamed to match
     * what they actually store now that both formats exist.            */
    char       *input_file_name;   /* source XML filename               */
    char       *input_request;     /* input request escaped for CSV     */
    char       *output_response;   /* output response escaped for CSV   */

} metrics_record_t;

/* ------------------------------------------------------------------ */
/*  CSV column header (matches field order in metrics_write exactly)   */
/* ------------------------------------------------------------------ */
#define METRICS_CSV_HEADER \
    "session_id,"          \
    "transaction_id,"      \
    "transaction_name,"    \
    "audit_id,"            \
    "consumer_name,"       \
    "client_ip,"           \
    "host_name,"           \
    "server_ip,"           \
    "server_port,"         \
    "process_id,"          \
    "thread_id,"           \
    "datasource_name,"     \
    "connection_id,"       \
    "pool_id,"             \
    "operation,"           \
    "object_name,"         \
    "sql_hash,"            \
    "cache_key_hash,"      \
    "start_time_us,"       \
    "end_time_us,"         \
    "cache_lookup_us,"     \
    "level1_parse_us,"     \
    "level2_parse_us,"     \
    "sql_parse_us,"        \
    "execution_us,"        \
    "total_us,"            \
    "rows_affected,"       \
    "output_xml_bytes,"    \
    "clob_bytes,"          \
    "lob_bytes,"           \
    "bytes_processed,"     \
    "cache_hit,"           \
    "status_code,"         \
    "error_code,"          \
    "error_text,"          \
    "connection_wait_us,"  \
    "connection_create_us,"\
    "connection_acquire_us,"\
    "input_file_name,"     \
    "input_request,"       \
    "output_response"

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

/*
 * metrics_now_us()
 * Return the current wall-clock time in microseconds (CLOCK_REALTIME).
 * Use for start_time_us and end_time_us.
 */
uint64_t metrics_now_us(void);

/*
 * metrics_init()
 * Zero the record and set all string fields to "-" (not-available).
 * Call this before populating any fields.
 */
void metrics_init(metrics_record_t *m);

/*
 * metrics_set_context()
 * Populate server / connection fields from oci_context_t.
 * Fills: host_name, datasource_name, process_id, connection_id,
 *        pool_id (pool_slot_index), server_port (1521 default).
 * Requires OCI_Connection.h to be included before this header in
 * any .c file that calls this function.
 */
struct oci_context_t;   /* forward declaration - avoids circular include */
void metrics_set_context(metrics_record_t   *m,
                          struct oci_context_t *ctx);

/*
 * metrics_finalise()
 * Compute derived timing fields:
 *   total_us    = end_time_us - start_time_us  (if both set)
 *   execution_us is left to the caller to set independently
 *                 (it is the OCI execute+fetch time, a subset of total)
 */
void metrics_finalise(metrics_record_t *m);

/*
 * metrics_write()
 * Append one CSV row to the metrics logger.
 * Writes the header line automatically if the log file is empty.
 * Safe to call with a NULL logger (no-op).
 * String fields containing commas or double-quotes are quoted.
 * After writing, frees m->input_file_name, m->input_request, m->output_response
 * if set - caller must not free these after metrics_write() returns.
 */
/*
 * metrics_format_timestamp_us()
 *
 * Formats a raw microsecond-epoch value (start_time_us/end_time_us's
 * own actual type - despite appearances in a CSV row, these are
 * uint64_t microsecond timestamps, not pre-formatted strings) as
 * "YYYY-MM-DD HH24:MI:SS.FFFFFF". Extracted from metrics_write()'s own
 * previously-inline, twice-duplicated (once for start, once for end)
 * conversion logic (closure item 5, Stage 3, 2026-08-09) - now shared
 * with metrics_db_bulk_insert() (metrics_writer.c), which needs the
 * exact same conversion to build a string Oracle's own TO_TIMESTAMP
 * can parse. buf_size should be at least 48 bytes; us == 0 writes "-".
 */
void metrics_format_timestamp_us(uint64_t us, char *buf, size_t buf_size);

void metrics_write(logger_t         *metrics_logger,
                   metrics_record_t *m);

/*
 * xml_escape_for_csv()
 * Escapes an XML string so it is safe to embed as a single CSV field.
 * Replaces: & < > " ' , \n \r with XML entity / numeric references.
 * Returns a heap-allocated string - caller must free() unless the
 * string is assigned to m->input_request or m->output_response, in which
 * case metrics_write() frees it automatically.
 * Returns strdup("-") on NULL/empty input or allocation failure.
 */
char *xml_escape_for_csv(const char *xml);
char *flatten_for_csv(const char *src);
char *flatten_for_csv2(const char *src);
char *flatten_for_csv3(const char *src);
#ifdef __cplusplus
}
#endif

#endif /* METRICS_H */
