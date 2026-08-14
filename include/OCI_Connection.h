/*
 * OCI_Connection.h
 *
 * Core OCI context, handle types, and direct (non-pooled) connect API.
 *
 * Changes from original
 * ---------------------
 *   - Added pool_handle     : opaque pointer to oci_pool_handle_t.
 *                             NULL when use_connection_pool = 0.
 *   - Added pool_slot_index : slot index written by OCI_Pool_get_session
 *                             so OCI_Pool_release_session knows which
 *                             slot to free.  -1 when not a pooled ctx.
 *
 * All existing code (OCI_Connect / OCI_Disconnect and every execute
 * module) compiles unchanged - the two new fields are simply ignored
 * when the pool is not in use.
 */

#ifndef OCI_CONNECTION_H
#define OCI_CONNECTION_H

#include <stdint.h>
#include "logger.h"
#include "ini_reader.h"
#include <oci.h>
#include "oci_cache.h"

/* Metrics refactor (closure item 5), Stage 2 (2026-08-09) - forward
 * declaration only, deliberately not #include "metrics_writer.h" here:
 * that header itself includes THIS one for oci_context_t, so a direct
 * include here would be circular. metrics_writer_t is only ever used
 * as a pointer below - every module that actually calls into it
 * (dispatcher.c, the CRUD execute modules) already includes
 * metrics_writer.h directly for the real declaration.                 */
typedef struct metrics_writer metrics_writer_t;

typedef struct oci_context_t {
    OCIEnv     *envhp;
    OCIError   *errhp;
    OCIServer  *srvhp;
    OCISvcCtx  *svchp;
    OCISession *authp;

    logger_t     *logger;
    logger_t     *select_logger;
    logger_t     *cache_logger;
    logger_t     *Metadata_logger;
    logger_t     *connection_logger;
    logger_t     *connectionpool_logger;
    logger_t     *insert_logger;
    logger_t     *update_logger;
    logger_t     *delete_logger;
    logger_t     *dml_logger;
    logger_t     *ddl_logger;
    logger_t     *procedure_logger;
   logger_t		 *error_logger;
   logger_t		 *metrics_logger;
   metrics_writer_t *metrics_writer;   /* closure item 5, Stage 2 - NULL
                                           if neither destination is
                                           enabled; metrics_finalise_
                                           and_enqueue() handles NULL
                                           safely either way            */
   logger_t         *metrics_writer_logger;   /* Stage 2 follow-up
                                                   (2026-08-09) - the
                                                   writer threads' own
                                                   operational logging,
                                                   deliberately separate
                                                   from metrics_logger
                                                   (the actual CSV data
                                                   file) - see
                                                   ini_reader.h's own
                                                   doc comment          */
   logger_t		 *transaction_logger;
   logger_t		 *security_logger;
   logger_t		 *crypt_logger;
   logger_t		 *audit_logger;
   logger_t      *session_logger;
   logger_t      *sql_parser_logger;
   logger_t      *file_consumer_logger;  /* File_Consumer_proposal v1.2,
                                             Stage 2 - file_consumer.c   */
   logger_t      *http_consumer_logger;  /* http_Consumer_proposal v1.2,
                                             Stage 2 - http_consumer.c   */
   logger_t      *dispatcher_logger;     /* File_Consumer_proposal v1.2,
                                             Stage 1 - dispatcher.c   */
   logger_t      *worker_logger;         /* File_Consumer_proposal v1.2,
                                             Stage 4 - worker.c. Single
                                             shared logger across however
                                             many callers eventually use
                                             it - logger.c's global
                                             log_mutex already makes this
                                             safe under real concurrency,
                                             per the 2026-08-04 design
                                             discussion.                  */
    app_config_t *ini;
    char         *NLS_DATE_FORMAT;       /* e.g. "YYYY-MM-DD HH24:MI:SS"  */
    char         *log_file_name;
    int           log_file_max_size;
    int           log_file_rotation_number;
    int           TEST_SQL_FILE_NAME;

    /* ----------------------------------------------------------------
     * Connection pool support (NULL / -1 when pool not in use)
     * ---------------------------------------------------------------- */
    void *pool_handle;     /* opaque oci_pool_handle_t* - avoids circular
                              include between OCI_Connection.h and
                              OCI_Connection_Pool.h                      */
    int   pool_slot_index; /* slot index assigned by OCI_Pool_get_session;
                              -1 for direct (non-pooled) connections      */
                              
    cache_t  *resultset_cache;   // NULL when cache is disabled
    cache_t  *metadata_cache;    // NULL when disabled
    cache_t  *session_cache;     // NULL when disabled          <-- ADD THIS

    uint64_t    start_time_us;
    uint64_t    end_time_us;
    uint64_t    connection_wait_us;
    uint64_t    connection_create_us;
    uint64_t    connection_acquire_us;
    /* ----------------------------------------------------------------
     * Transaction Manager support
     * Set by tx_begin(), cleared by tx_commit() / tx_rollback() /
     * tx_abort().  NULL means no transaction is active on this context.
     * Execute modules check this before issuing OCITransCommit:
     *   if (!ctx->active_tx) OCITransCommit(...);
     * The struct forward declaration avoids a circular include between
     * OCI_Connection.h and OCI_Transaction_Manager.h.
     * ---------------------------------------------------------------- */
    struct tx_handle_s *active_tx;   /* NULL = no active transaction   */

    /* ----------------------------------------------------------------
     * Session Manager support
     * Set by session_create(), cleared by session_end() (only if the
     * session being ended matches the currently active one).  Empty
     * string ("") means no session is attached to this context.
     * Plain strings rather than a handle pointer (unlike active_tx) -
     * the session module owns the full lifecycle via session_cache;
     * ctx only needs to remember which session a given call is on
     * behalf of, purely for metrics tagging.  metrics.c reads these
     * two fields directly with no dependency on OCI_Session_Manager.h.
     * ---------------------------------------------------------------- */
    char active_session_id[64];      /* "" = no session attached        */
    char active_client_ip[64];       /* "" = no client IP captured yet  */

    /* Same purpose as active_session_id above (metrics tagging), for
     * the same reason - closure item 5 follow-up (2026-08-10). Stamped
     * fresh by dispatcher.c for every single request, right after
     * session validation - a worker's own ctx is reused across many
     * requests on the same thread, and each one can carry a different
     * external_audit_id. "" = no audit id known for whatever's
     * currently executing on this ctx. (dispatcher.c's own per-request
     * stamp now also refreshes active_session_id above on a worker's
     * ctx - independent of, and more frequent than, session_create()/
     * session_end()'s own updates to that same field on File
     * Consumer's separate ctx, which track a different thing: which
     * session is currently attached to that specific ctx.)             */
    char active_audit_id[64];

    char *INPUT_XML;            /* Input XML for passing commands  */
    char *OUTPUT_XML;           /* Output XML for passing results  */

    /* ---- Parse-stage timing, bridged through to metrics ----
     * Set by whichever dispatcher calls level1_parse() / level2_
     * validate() (currently Test_XML_Runner.c) immediately around each
     * call. metrics_set_context() reads these into the metrics record
     * further downstream, once execution reaches an execute module -
     * same bridging pattern already used for active_session_id /
     * active_client_ip above. 0 if that stage wasn't measured for this
     * request (e.g. old-format dispatch path, which doesn't call
     * level1_parse()/level2_validate() at all).                        */
    uint64_t level1_parse_us;
    uint64_t level2_parse_us;

} oci_context_t;

typedef struct {
    char *INPUT_XML;            /* Input XML for passing commands  */
    char *OUTPUT_XML;           /* Output XML for passing results  */
} XML_envelope;

typedef struct {
    int   col_idx;
    int   row_idx;
    char *value;                /* record field data               */
} ORA_record;


typedef struct {
    int log_connect_stats;
    int log_execute_stats;
    int log_memory_stats;
    int log_execution_results;

    int max_rows;
    int max_memory_bytes;
    int fetch_array_size;
    int auto_commit;
    int include_column_names;
    int query_timeout;

    char         *ReturnFormat; /* XML, JSON, CSV - checked (case-
                                  * insensitive) against "JSON" to
                                  * decide which of xml->OUTPUT_XML /
                                  * OUTPUT_JSON below gets populated  */
    char         *SQL;          /* Input SQL                       */
    XML_envelope *xml;
    char         *OUTPUT_JSON;  /* Output JSON for passing results,
                                  * set only when ReturnFormat is
                                  * "JSON". NULL otherwise.          */
    char *input_file_name;   /* source XML filename - set by dispatcher */
    
} execute_config_t;


typedef struct
{
    int            file_name_set;
    char          *file_name;
    char          *column_name;
    int            column_index;
    char          *BLOB_Default_file_name_col;
    char          *BLOB_Default_MIME_TYPE_col;
    int            file_name_source;
    int            Add_extract_time_stamp;
    char          *mime_type;
    char          *output_file_url;
    char          *output_file_destination;
    ub4            blob_size;
    char          *start_extract_timestamp;
    char          *end_extract_timestamp;
    int            fetch_chunk_size;
    int            number_of_chunks;
    OCILobLocator *lob_loc;
    unsigned long  bytes_read;
    int            is_null;
    unsigned char *BLOB_buff;
    unsigned char *blob_data;
} lob_item_t;


int  OCI_Connect   (oci_context_t *ctx);
void OCI_Disconnect(oci_context_t *ctx);


#endif /* OCI_CONNECTION_H */
