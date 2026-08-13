 #ifndef INI_READER_H
#define INI_READER_H

#include <stddef.h>

/* Forward declaration */
typedef struct oci_context_t oci_context_t;

typedef enum
{
    CFG_INT,
    CFG_BOOL,
    CFG_STRING
} cfg_type_t;

typedef struct {
    char *name;
    char *value;
} ini_entry_t;

typedef struct {
    ini_entry_t *entries;
    size_t count;
} ini_file_t;

/* Date format hygiene (2026-08-08 closure item 3) - single source of
 * truth for the default NLS date format string, referenced by
 * ini_reader.c's own ctx_map default and by OCI_Connection_Pool.c's
 * COPY_NLS fallback, so the two can never silently drift apart the
 * way ad-hoc literal copies of this string had started to. */
#define NLS_DATE_FORMAT_DEFAULT "YYYY-MM-DD HH24:MI:SS"

typedef struct
{
    /* ----------------------------------------------------------------
     * Logging
     * ---------------------------------------------------------------- */

	/*Main log*/
	char log_file_name[256];
    int  log_file_max_size;
    int  log_file_rotation_number;
    char log_level[20];
    int  log_level_num;

	/* Global sid/txid trace-context toggle (2026-08-06) - a single
	 * switch applying to every logger uniformly, not per-logger. See
	 * logger.h's own doc comment on logger_set_include_trace_context()
	 * for the full design. Default on (1) per Terry's own call - leave
	 * it on for debugging. */
	int  log_include_trace_context;

	/*Cache log file*/
	char cache_log_file_name[256];
    int  cache_log_file_max_size;
    int  cache_log_file_rotation_number;
    char cache_log_level[20];
    int  cache_log_level_num;

	/*Select log file*/
	char select_log_file_name[256];
    int  select_log_file_max_size;
    int  select_log_file_rotation_number;
    char select_log_level[20];
    int  select_log_level_num;

	/* File Consumer log file (File_Consumer_proposal v1.2) -
	 * file_consumer.c's own dedicated log, separate from the shared
	 * connectionpool_logger it was borrowing before. */
	char file_consumer_log_file_name[256];
    int  file_consumer_log_file_max_size;
    int  file_consumer_log_file_rotation_number;
    char file_consumer_log_level[20];

    /* Metrics refactor (closure item 5), Stage 2 follow-up (2026-08-09) -
     * a dedicated logger for the Metrics Writer threads' OWN
     * operational/lifecycle messages ("thread started", "flushing
     * batch of N"), kept genuinely separate from metrics_logger (the
     * actual CSV data file) - the same one-logger-per-subsystem
     * pattern already used for File Consumer and Session Manager. Real
     * bug this fixes: these threads' own diagnostic messages were
     * previously written through the SAME logger as the CSV data
     * itself, corrupting the CSV's row structure with interleaved log
     * lines wherever one landed between data rows.                    */
    char metrics_writer_log_file_name[256];
    int  metrics_writer_log_file_max_size;
    int  metrics_writer_log_file_rotation_number;
    char metrics_writer_log_level[20];

	/* Dispatcher log file (File_Consumer_proposal v1.2) -
	 * dispatcher.c's own dedicated log, same reasoning as above. */
	char dispatcher_log_file_name[256];
    int  dispatcher_log_file_max_size;
    int  dispatcher_log_file_rotation_number;
    char dispatcher_log_level[20];

	/* Worker log file (File_Consumer_proposal v1.2) - worker.c's own
	 * dedicated log. Single shared log across however many workers
	 * eventually write to it - see OCI_Connection.h's worker_logger
	 * field for the thread-safety reasoning. */
	char worker_log_file_name[256];
    int  worker_log_file_max_size;
    int  worker_log_file_rotation_number;
    char worker_log_level[20];


	/*Metadata log file*/
	char Metadata_log_file_name[256];
    int  Metadata_log_file_max_size;
    int  Metadata_log_file_rotation_number;
    char Metadata_log_level[20];
    int  Metadata_log_level_num;



	/*connection log file*/
	char connection_log_file_name[256];
    int  connection_log_file_max_size;
    int  connection_log_file_rotation_number;
    char connection_log_level[20];
    int  connection_log_level_num;


	/*connectionpool log file*/
	char connectionpool_log_file_name[256];
    int  connectionpool_log_file_max_size;
    int  connectionpool_log_file_rotation_number;
    char connectionpool_log_level[20];
    int  connectionpool_log_level_num;
    
    
   	/*update log file*/
	char update_log_file_name[256];
    int  update_log_file_max_size;
    int  update_log_file_rotation_number;
    char update_log_level[20];
    int  update_log_level_num;
    
   
      	/*insert log file*/
	char insert_log_file_name[256];
    int  insert_log_file_max_size;
    int  insert_log_file_rotation_number;
    char insert_log_level[20];
    int  insert_log_level_num;


     	/*delete log file*/
	char delete_log_file_name[256];
    int  delete_log_file_max_size;
    int  delete_log_file_rotation_number;
    char delete_log_level[20];
    int  delete_log_level_num;


     	/*dml log file*/
	char dml_log_file_name[256];
    int  dml_log_file_max_size;
    int  dml_log_file_rotation_number;
    char dml_log_level[20];
    int  dml_log_level_num;


   	/*ddl log file*/
	char ddl_log_file_name[256];
    int  ddl_log_file_max_size;
    int  ddl_log_file_rotation_number;
    char ddl_log_level[20];
    int  ddl_log_level_num;

     	/*procedure log file*/
	char procedure_log_file_name[256];
    int  procedure_log_file_max_size;
    int  procedure_log_file_rotation_number;
    char procedure_log_level[20];
    int  procedure_log_level_num;

  /*Additional logs  error , metrics, transaction , security . crypt, audit , session added 31-May-2026*/

    	/*error log file*/
	char error_log_file_name[256];
    int  error_log_file_max_size;
    int  error_log_file_rotation_number;
    char error_log_level[20];
    int  error_log_level_num;

    	/*metrics log file*/
	char metrics_log_file_name[256];
    int  metrics_log_file_max_size;
    int  metrics_log_file_rotation_number;
    char metrics_log_level[20];
    int  metrics_log_level_num;

  	/*transaction log file*/
	char transaction_log_file_name[256];
    int  transaction_log_file_max_size;
    int  transaction_log_file_rotation_number;
    char transaction_log_level[20];
    int  transaction_log_level_num;


  	/*security log file*/
	char security_log_file_name[256];
    int  security_log_file_max_size;
    int  security_log_file_rotation_number;
    char security_log_level[20];
    int  security_log_level_num;

	/*crypt log file*/
	char crypt_log_file_name[256];
    int  crypt_log_file_max_size;
    int  crypt_log_file_rotation_number;
    char crypt_log_level[20];
    int  crypt_log_level_num;


 	/*audit log file*/
	char audit_log_file_name[256];
    int  audit_log_file_max_size;
    int  audit_log_file_rotation_number;
    char audit_log_level[20];
    int  audit_log_level_num;
    
    	/*session log file*/
	char session_log_file_name[256];
    int  session_log_file_max_size;
    int  session_log_file_rotation_number;
    char session_log_level[20];
    int  session_log_level_num;


   	/*sql_parser log file*/
	char sql_parser_log_file_name[256];
    int  sql_parser_log_file_max_size;
    int  sql_parser_log_file_rotation_number;
    char sql_parser_log_level[20];
    int  sql_parser_log_level_num;

  /*End Additional logs  error , metrics, tx , sec . crypt, audit , session added 31-May-2026*/

    /* ----------------------------------------------------------------
     * Database credentials / connection string
     * ---------------------------------------------------------------- */
    char TEST_SQL_FILE_NAME[256];
    char dbname          [128];
    char username        [64];   /* retained for logging / NLS only    */

    /* ----------------------------------------------------------------
     * Oracle Wallet - replaces plaintext password in config.ini.
     * Set use_wallet = 1 and wallet_location = path to wallet dir.
     * OCI will use OCI_CRED_EXT and read credentials from the wallet
     * transparently.  username and password are NOT required when
     * use_wallet = 1.  TNS_ADMIN is set programmatically from
     * wallet_location before the first OCI call.
     *
     * use_wallet = 0 (default): legacy OCI_CRED_RDBMS mode.
     *   Requires password field below.  Development / fallback only.
     *   Not recommended for production or GxP environments.
     *
     * use_wallet = 1 (recommended): OCI_CRED_EXT wallet mode.
     *   password field is ignored.  wallet_location must point to a
     *   directory containing cwallet.sso, ewallet.p12, sqlnet.ora.
     * ---------------------------------------------------------------- */
    int  use_wallet;              /* 0=legacy RDBMS creds  1=wallet    */
    char wallet_location [256];   /* path to Oracle Wallet directory   */
    char password        [64];    /* legacy only - ignored if use_wallet=1
                                     leave blank in production config  */

    /* ----------------------------------------------------------------
     * Metrics database - independent connection pool (response to
     * closure proposal, 13 Aug 2026). Mirrors the block above exactly -
     * the metrics DB writer thread used to borrow its session from the
     * SAME pool as the business connection above, meaning DB metrics
     * silently went wherever dbname happened to point. These give the
     * metrics destination its own identity, entirely independent of
     * the business database. Only read/used when metrics_db_enabled=1
     * (see the Metrics parameters block further down).
     * ---------------------------------------------------------------- */
    char metrics_dbname          [128];
    char metrics_username        [64];   /* logging / NLS only, as above */
    int  metrics_use_wallet;
    char metrics_wallet_location [256];
    char metrics_password        [64];   /* legacy only - ignored if
                                             metrics_use_wallet=1        */

    /* ----------------------------------------------------------------
     * XML I/O directories
     * ---------------------------------------------------------------- */
    char xml_input_dir [256];
    char xml_output_dir[256];
    char xml_error_dir [256];

    /* ----------------------------------------------------------------
     * Threading (direct connection mode)
     * ---------------------------------------------------------------- */
    int  max_threads;
    int  min_threads;
    int  query_timeout;
    int  connection_timeout;
    int  heartbeat_on;
    int  heartbeat_timeout;
    int  max_num_timeouts;

    /* ----------------------------------------------------------------
     * LOB / BLOB / CLOB output
     * ---------------------------------------------------------------- */
    int  xml_share_BLOB_output_dir;
    int  xml_share_CLOB_output_dir;
    int  xml_share_BLOB_host_path;
    int  xml_share_CLOB_URL_path;
    int  xml_share_BLOB_URL_path;
    char BLOB_host_path[256];
    char CLOB_URL_path [256];
    char BLOB_URL_path [256];
    char BLOB_output_dir[256];
    char CLOB_output_dir[256];
    int  max_BLOBS_per_record;
    int  max_CLOBS_per_record;
    unsigned long chunk_read_size;
    int  query_max_record_count;

    char BLOB_default_file_name_col  [50];
    char BLOB_default_file_name_col_1[50];
    char BLOB_default_file_name_col_2[50];
    char BLOB_default_file_name_col_3[50];
    char BLOB_default_file_name_col_4[50];
    char BLOB_default_file_name_col_5[50];
    char BLOB_default_MIME_col       [10];
    char BLOB_default_MIME_TYPE_col_1[10];
    char BLOB_default_MIME_TYPE_col_2[10];
    char BLOB_default_MIME_TYPE_col_3[10];
    char BLOB_default_MIME_TYPE_col_4[10];
    char BLOB_default_MIME_TYPE_col_5[10];
    char BLOB_default_MIME_TYPE      [10];
    char BLOB_default_file_name      [10];
    int  BLOB_append_file_timestamp;
    int  query_fetch_batch_size;
    char clob_default_extension[16];

    /* ----------------------------------------------------------------
     * Insert template defaults
     * ---------------------------------------------------------------- */
    int  insert_table_defaults;

    char insert_default_number      [64];
    char insert_default_float       [64];
    char insert_default_binary_float [64];
    char insert_default_binary_double[64];
    char insert_default_char        [64];
    char insert_default_varchar2    [64];
    char insert_default_nchar       [64];
    char insert_default_nvarchar2   [64];
    char insert_default_date        [32];
    char insert_default_timestamp   [64];
    char insert_default_interval_ym [32];
    char insert_default_interval_ds [64];
    char insert_default_raw         [64];
    char insert_default_clob        [64];
    char insert_default_nclob       [64];
    char insert_default_blob        [256];
    char insert_default_rowid       [32];
    char insert_default_urowid      [32];

    /* ----------------------------------------------------------------
     * Bulk insert control
     * ---------------------------------------------------------------- */
    int  max_bulk_inserts;

    /* ================================================================
     * Connection pool parameters
     *
     * use_connection_pool = 1  activates OCI_Connect_pool() instead
     * of OCI_Connect().  All other pool fields are only read when the
     * pool is active.  Every field has a safe default in ini_reader.c
     * so adding these to an existing ini file is optional.
     * ================================================================ */

    /* Master switch */
    int  use_connection_pool;         /* 0=direct connect  1=pool       */

    /* Pool sizing */
    int  pool_min_size;               /* sessions opened at startup      */
    int  pool_max_size;               /* maximum concurrent sessions      */
    int  pool_increment;              /* sessions added when pool grows   */

    /* Timeout settings - all in seconds */
    int  pool_connection_timeout;     /* wait for a free slot             */
    int  session_idle_timeout;        /* recycle idle session after N s   */
    int  max_time_to_establish;       /* OCIServerAttach limit            */
    int  network_read_write_timeout;  /* socket read/write timeout        */
    int  query_execution_timeout;     /* max query run time (0=unlimited) */
    int  authentication_handshake_timeout; /* OCISessionBegin limit       */
    int  login_auth_timeout;          /* alias: auth handshake limit      */
    int  session_max_lifetime;        /* recycle session after N s        */
    int  heartbeat_keepalive_interval;/* background ping interval         */

    /* Retry */
    int  retries_on_connection_failure; /* open/reopen attempts           */

    /* Session behaviour */
    int  connection_validation_on_borrow; /* OCIPing before handing out   */
    int  rollback_on_return_to_pool;      /* rollback when returned       */
    int  autocommit_mode;                 /* ALTER SESSION SET AUTOCOMMIT */

    /* NLS / character set */
    char nls_date_format    [64];   /* default NLS_DATE_FORMAT_DEFAULT   */
    char nls_language       [64];   /* default "AMERICAN"                */
    char nls_territory      [64];   /* default "AMERICA"                 */
    char nls_characterset   [64];   /* default "AL32UTF8"                */
    char nls_session_timezone[64];  /* default "UTC"                     */
    
    /* ================================================================
     * Result Set Cache parameters
     * ================================================================ */
    int  resultset_cache_enabled;              /* 0=off  1=on            */
    int  resultset_cache_ttl_seconds;          /* entry lifetime          */
    int  resultset_cache_max_entries;          /* max cached queries      */
    int  resultset_cache_max_memory_mb;        /* memory cap in MB        */
    int  resultset_cache_bucket_count;         /* hash table width        */
    char resultset_cache_hash_algorithm[16];   /* fnv1a|djb2|murmur3     */

    /* ================================================================
     * Metadata Cache parameters
     * ================================================================ */
    int  metadata_cache_enabled;
    int  metadata_cache_ttl_seconds;
    int  metadata_cache_max_entries;
    int  metadata_cache_max_memory_mb;
    int  metadata_cache_bucket_count;
    char metadata_cache_hash_algorithm[16];

    /* ================================================================
     * Statement Cache parameters
     * ================================================================ */
    int  statement_cache_enabled;
    int  statement_cache_ttl_seconds;
    int  statement_cache_max_entries;
    int  statement_cache_max_memory_mb;
    int  statement_cache_bucket_count;
    char statement_cache_hash_algorithm[16];





   /* ================================================================
     * Transaction Manager parameters
     * ================================================================ */
    int  tx_timeout_seconds;
     int  tx_max_retries;    
     int  tx_retry_delay_ms;     
     int  tx_log_begin;          
     int  tx_log_commit;         
     int  tx_log_rollback;       
     int  tx_log_timeout;  
     
   /* ================================================================
     * Metrics parameters
     * ================================================================ */
      int  metrics_display_input_file_name;
      int  metrics_display_input_request;
      int  metrics_display_output_response;

    /* Metrics refactor (closure item 5), Stage 2 (2026-08-09) - see
     * metrics_writer.h for the full design. finalize_metrics() itself
     * is now always fire-and-forget/non-blocking regardless of these
     * settings; these two independently control which destination(s)
     * actually persist anything. Both may be on, either alone, or
     * neither (metrics silently dropped, matching the same "drop
     * rather than block" philosophy already used for session touches -
     * a metrics gap is not something worth blocking real request
     * traffic to avoid).                                              */
    int  metrics_file_enabled;
    int  metrics_db_enabled;

    /* DB destination only - file writes stay one-row-at-a-time (cheap,
     * local I/O, no round-trip cost worth batching for). The DB writer
     * flushes whichever comes first: metrics_per_write records
     * accumulated, or metrics_max_insert_delay_ms elapsed since the
     * oldest record still unflushed - so a quiet period never leaves
     * metrics sitting unwritten indefinitely just because the batch
     * never filled up.                                                */
    int  metrics_per_write;
    int  metrics_max_insert_delay_ms;

    /* Metrics DB pool - independent connection pool (response to
     * closure proposal, 13 Aug 2026). Sizing is deliberately separate
     * from, and normally much smaller than, the business pool above -
     * only one thread (the metrics DB writer) ever borrows from this
     * pool, so pool_min_size=1/pool_max_size=2 is a sensible default
     * rather than reusing the business pool's 10/50. Every field here
     * mirrors its business-pool counterpart 1:1 (see the Connection
     * pool parameters block above) so the two pools can be tuned
     * independently, and so a future second metrics thread would not
     * need any config changes at all.                                  */
    int  metrics_pool_min_size;
    int  metrics_pool_max_size;
    int  metrics_pool_increment;

    int  metrics_pool_connection_timeout;
    int  metrics_session_idle_timeout;
    int  metrics_max_time_to_establish;
    int  metrics_network_read_write_timeout;
    int  metrics_query_execution_timeout;
    int  metrics_authentication_handshake_timeout;
    int  metrics_login_auth_timeout;
    int  metrics_session_max_lifetime;
    int  metrics_heartbeat_keepalive_interval;

    int  metrics_retries_on_connection_failure;

    int  metrics_connection_validation_on_borrow;
    int  metrics_rollback_on_return_to_pool;
    int  metrics_autocommit_mode;

    /* Startup failure behaviour (response to closure proposal, 13 Aug
     * 2026) - there are genuine reasons the metrics database
     * specifically might be unreachable while the business database is
     * fine, and metrics is never allowed to take production down with
     * it (see metrics_writer.h's own stated design purpose). Default 0:
     * a failed metrics pool connect at startup is logged and DB
     * metrics are disabled for that run; file metrics (if enabled) are
     * unaffected, and the business connection is untouched either way.
     * Set to 1 to make a failed metrics pool connect fatal, same as a
     * failed business pool connect today.                              */
    int  metrics_db_fail_force_shutdown;


    /* ================================================================
     * Session Cache parameters
     * ================================================================ */
    int  session_cache_enabled;
    int  session_cache_ttl_seconds;
    int  session_cache_max_entries;
    int  session_cache_max_memory_mb;
    int  session_cache_bucket_count;
    char session_cache_hash_algorithm[16];

    /* ================================================================
     * Session Manager parameters
     * ================================================================ */
    int  session_default_ttl_seconds;
    int  session_log_create;
    int  session_log_end;
    int  session_log_reconcile;

    /* Session Manager proposal, Stage 3 (2026-08-08) - kill switch for
     * dispatcher.c's hard session validation (see dispatcher.c's own
     * comment at the check itself). Deliberately required, fail-closed
     * like every other key in this project - no silent default.
     * Production runs with this on (1); UAT is expected to run with it
     * off (0), since UAT traffic often uses ad-hoc/fixture data
     * without a proper session handshake. Also doubles as a disaster-
     * recovery lever in production - flip to 0 and restart if session
     * validation itself is ever the thing blocking otherwise-
     * legitimate traffic. Gates only the rejection behaviour - session
     * creation/tracking (Stages 1-2) stay on regardless of this
     * setting.                                                        */
    int  session_validation_enabled;

    /* ================================================================
     * File Consumer / Dispatcher parameters (File_Consumer_proposal v1.2)
     * consumer_type selects which consumer ini gets loaded as a second
     * load_ini() pass in main() - see patch_main_c_snippet.txt.
     * ================================================================ */
    char consumer_type[32];              /* FILE | HTTP | MQ */
    char consumer_ini_path[256];         /* full path to the consumer_type-specific
                                             ini file, e.g. .../Props/consumer_file.ini -
                                             read here so it's available before
                                             load_consumer_ini() is called            */

    int  dispatcher_queue_count;         /* == worker thread count      */
    int  dispatcher_queue_depth;         /* items/queue before QUEUE_FULL */
    char dispatcher_algorithm[32];       /* round_robin (Defined) | least_busy (Pending) */

    /* Contention Manager proposal (2026-08-08) - off (default) uses
     * plain round-robin across every queue, unchanged from before this
     * feature existed. single_write_queue routes every request
     * containing at least one INSERT/UPDATE/DELETE operation to one
     * fixed, dedicated queue (queue index 0 - not separately
     * configurable, deliberately simple), keeping all write traffic on
     * one connection; SELECT and EXECUTE_PROCEDURE continue round-
     * robin across the remaining queues. EXECUTE_PROCEDURE is
     * deliberately NOT treated as a write for this purpose - Data
     * Manager has no visibility into what a procedure's own body
     * actually does, and isolating it would need a different, larger
     * design (see the design discussion this was decided against).   */
    char contention_manager_mode[32];    /* off | single_write_queue */

    /* Metrics refactor (closure item 5), Stage 2 (2026-08-09) - this
     * consumer instance's own declared identity, e.g. "FILE_CONSUMER_01".
     * Stamped onto every metrics record (metrics_set_context()) so a
     * dashboard querying the metrics table can distinguish which
     * consumer produced a given row - essential once HTTP consumer
     * exists alongside File Consumer, and also lets two instances of
     * the SAME consumer type carry distinct identities if ever run
     * side by side. Required, like every other key in this project -
     * no silent "unknown consumer" default.                           */
    char consumer_name[64];

    /* File Consumer thread lifecycle (Terry's proposal, 2026-08-05
     * "Findings and lessons" doc, section 1b) - replaces the hardcoded
     * FILE_CONSUMER_TEST_PASSES/INTERVAL test-only C constants with
     * real config, now that File Consumer runs as its own dedicated
     * thread (file_consumer_runner.c) rather than inline on main().  */
    int  dispatcher_poll_interval_seconds; /* seconds between scan passes */
    int  dispatcher_lifetime_seconds;      /* 0 = run forever, else stop
                                               after this many seconds  */

    char file_consumer_input_xml_dir     [256];
    char file_consumer_processing_xml_dir[256];
    char file_consumer_output_xml_dir    [256];
    char file_consumer_error_xml_dir     [256];

    char file_consumer_input_json_dir     [256];
    char file_consumer_processing_json_dir[256];
    char file_consumer_output_json_dir    [256];
    char file_consumer_error_json_dir     [256];

} app_config_t;


/* INI file functions */
void        ini_free     (ini_file_t *ini);
const char *ini_get_str  (const ini_file_t *ini, const char *name, const char *default_val);
int         ini_get_int  (const ini_file_t *ini, const char *name, int default_val);
double      ini_get_double(const ini_file_t *ini, const char *name, double default_val);
int         ini_get_bool (const ini_file_t *ini, const char *name, int default_val);
void        ini_dump     (const ini_file_t *ini);
int         load_ini     (const char *filename, app_config_t *config, oci_context_t *ctx);
int         load_consumer_ini(const char *filename, app_config_t *config);
int         populate_ctx_from_ini(app_config_t *ini);

#endif /* INI_READER_H */
