#define _POSIX_C_SOURCE 200809L

#include "ini_reader.h"
#include "OCI_Connection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stddef.h>

typedef struct {
    const char *name;
    cfg_type_t  type;
    size_t      offset;
    const char *default_str;
    int         default_int;
} ctx_config_map_t;

/* ======================================================================
 * Configuration map
 * Every field in app_config_t that should be populated from config.ini
 * has one entry here.  Order does not matter.
 * ====================================================================== */
static ctx_config_map_t ctx_map[] = {

    /* ----------------------------------------------------------------
     * LOB / XML sharing flags
     * ---------------------------------------------------------------- */
    { "xml_share_BLOB_output_dir", CFG_BOOL,
        offsetof(app_config_t, xml_share_BLOB_output_dir), NULL, 0 },
    { "xml_share_CLOB_output_dir", CFG_BOOL,
        offsetof(app_config_t, xml_share_CLOB_output_dir), NULL, 0 },
    { "xml_share_BLOB_host_path",  CFG_BOOL,
        offsetof(app_config_t, xml_share_BLOB_host_path),  NULL, 0 },
    { "xml_share_CLOB_URL_path",   CFG_BOOL,
        offsetof(app_config_t, xml_share_CLOB_URL_path),   NULL, 0 },
    { "xml_share_BLOB_URL_path",   CFG_BOOL,
        offsetof(app_config_t, xml_share_BLOB_URL_path),   NULL, 0 },

    /* ----------------------------------------------------------------
     * Paths
     * ---------------------------------------------------------------- */
    { "BLOB_host_path",  CFG_STRING, offsetof(app_config_t, BLOB_host_path),  "//localhost/tmp", 0 },
    { "CLOB_URL_path",   CFG_STRING, offsetof(app_config_t, CLOB_URL_path),   "https://localhost:8080/OCI_Wrapper/", 0 },
    { "BLOB_URL_path",   CFG_STRING, offsetof(app_config_t, BLOB_URL_path),   "https://localhost:8080/OCI_Wrapper/", 0 },
    { "BLOB_output_dir", CFG_STRING, offsetof(app_config_t, BLOB_output_dir), "/tmp", 0 },
    { "CLOB_output_dir", CFG_STRING, offsetof(app_config_t, CLOB_output_dir), "/tmp", 0 },

    /* ----------------------------------------------------------------
     * Database credentials
     * ---------------------------------------------------------------- */
    { "username", CFG_STRING, offsetof(app_config_t, username), "Data_Manager",          0 },
    { "password", CFG_STRING, offsetof(app_config_t, password), "",                      0 },
    { "dbname",   CFG_STRING, offsetof(app_config_t, dbname),   "localhost:1521/freepdb1",0 },
    { "use_wallet",        CFG_BOOL,   offsetof(app_config_t, use_wallet),        NULL, 0   },
    { "wallet_location",   CFG_STRING, offsetof(app_config_t, wallet_location),   "/opt/data_manager/wallet", 0 },

    /* ----------------------------------------------------------------
     * Logging main log
     * ---------------------------------------------------------------- */
    { "log_file_name",          CFG_STRING, offsetof(app_config_t, log_file_name),          "app.log", 0 },
    { "log_file_max_size",      CFG_INT,    offsetof(app_config_t, log_file_max_size),       NULL, 10485760 },
    { "log_file_rotation_number", CFG_INT,  offsetof(app_config_t, log_file_rotation_number),NULL, 5 },
    { "log_level",              CFG_STRING, offsetof(app_config_t, log_level),               "DEBUG", 0 },
    { "TEST_SQL_FILE_NAME",     CFG_STRING, offsetof(app_config_t, TEST_SQL_FILE_NAME),      "test.sql", 0 },

	/* ----------------------------------------------------------------
     * Logging select log
     * ---------------------------------------------------------------- */
    { "select_log_file_name",          CFG_STRING, offsetof(app_config_t, select_log_file_name),          "select_app.log", 0 },
    { "select_log_file_max_size",      CFG_INT,    offsetof(app_config_t, select_log_file_max_size),       NULL, 10485760 },
    { "select_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, select_log_file_rotation_number),NULL, 5 },
    { "select_log_level",              CFG_STRING, offsetof(app_config_t, select_log_level),               "DEBUG", 0 },


	/* ----------------------------------------------------------------
     * Logging cache log
     * ---------------------------------------------------------------- */
    { "cache_log_file_name",          CFG_STRING, offsetof(app_config_t, cache_log_file_name),          "cache_app.log", 0 },
    { "cache_log_file_max_size",      CFG_INT,    offsetof(app_config_t, cache_log_file_max_size),       NULL, 10485760 },
    { "cache_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, cache_log_file_rotation_number),NULL, 5 },
    { "cache_log_level",              CFG_STRING, offsetof(app_config_t, cache_log_level),               "DEBUG", 0 },


	/* ----------------------------------------------------------------
     * Logging Metadata log
     * ---------------------------------------------------------------- */
    { "Metadata_log_file_name",          CFG_STRING, offsetof(app_config_t, Metadata_log_file_name),          "Metadata_app.log", 0 },
    { "Metadata_log_file_max_size",      CFG_INT,    offsetof(app_config_t, Metadata_log_file_max_size),       NULL, 10485760 },
    { "Metadata_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, Metadata_log_file_rotation_number),NULL, 5 },
    { "Metadata_log_level",              CFG_STRING, offsetof(app_config_t, Metadata_log_level),               "DEBUG", 0 },




	/* ----------------------------------------------------------------
     * Logging connection log
     * ---------------------------------------------------------------- */
    { "connection_log_file_name",          CFG_STRING, offsetof(app_config_t, connection_log_file_name),          "connection_app.log", 0 },
    { "connection_log_file_max_size",      CFG_INT,    offsetof(app_config_t, connection_log_file_max_size),       NULL, 10485760 },
    { "connection_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, connection_log_file_rotation_number),NULL, 5 },
    { "connection_log_level",              CFG_STRING, offsetof(app_config_t, connection_log_level),               "DEBUG", 0 },


	/* ----------------------------------------------------------------
     * Logging connectionpool log
     * ---------------------------------------------------------------- */
    { "connectionpool_log_file_name",          CFG_STRING, offsetof(app_config_t, connectionpool_log_file_name),          "connectionpool_app.log", 0 },
    { "connectionpool_log_file_max_size",      CFG_INT,    offsetof(app_config_t, connectionpool_log_file_max_size),       NULL, 10485760 },
    { "connectionpool_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, connectionpool_log_file_rotation_number),NULL, 5 },
    { "connectionpool_log_level",              CFG_STRING, offsetof(app_config_t, connectionpool_log_level),               "DEBUG", 0 },




	/* ----------------------------------------------------------------
     * Logging update log
     * ---------------------------------------------------------------- */
    { "update_log_file_name",          CFG_STRING, offsetof(app_config_t, update_log_file_name),          "update_app.log", 0 },
    { "update_log_file_max_size",      CFG_INT,    offsetof(app_config_t, update_log_file_max_size),       NULL, 10485760 },
    { "update_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, update_log_file_rotation_number),NULL, 5 },
    { "update_log_level",              CFG_STRING, offsetof(app_config_t, update_log_level),               "DEBUG", 0 },


	/* ----------------------------------------------------------------
     * Logging insert log
     * ---------------------------------------------------------------- */
    { "insert_log_file_name",          CFG_STRING, offsetof(app_config_t, insert_log_file_name),          "insert_app.log", 0 },
    { "insert_log_file_max_size",      CFG_INT,    offsetof(app_config_t, insert_log_file_max_size),       NULL, 10485760 },
    { "insert_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, insert_log_file_rotation_number),NULL, 5 },
    { "insert_log_level",              CFG_STRING, offsetof(app_config_t, insert_log_level),               "DEBUG", 0 },




	/* ----------------------------------------------------------------
     * Logging delete log
     * ---------------------------------------------------------------- */
    { "delete_log_file_name",          CFG_STRING, offsetof(app_config_t, delete_log_file_name),          "delete_app.log", 0 },
    { "delete_log_file_max_size",      CFG_INT,    offsetof(app_config_t, delete_log_file_max_size),       NULL, 10485760 },
    { "delete_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, delete_log_file_rotation_number),NULL, 5 },
    { "delete_log_level",              CFG_STRING, offsetof(app_config_t, delete_log_level),               "DEBUG", 0 },



	/* ----------------------------------------------------------------
     * Logging dml log
     * ---------------------------------------------------------------- */
    { "dml_log_file_name",          CFG_STRING, offsetof(app_config_t, dml_log_file_name),          "dml_app.log", 0 },
    { "dml_log_file_max_size",      CFG_INT,    offsetof(app_config_t, dml_log_file_max_size),       NULL, 10485760 },
    { "dml_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, dml_log_file_rotation_number),NULL, 5 },
    { "dml_log_level",              CFG_STRING, offsetof(app_config_t, dml_log_level),               "DEBUG", 0 },



	/* ----------------------------------------------------------------
     * Logging ddl log
     * ---------------------------------------------------------------- */
    { "ddl_log_file_name",          CFG_STRING, offsetof(app_config_t, ddl_log_file_name),          "ddl_app.log", 0 },
    { "ddl_log_file_max_size",      CFG_INT,    offsetof(app_config_t, ddl_log_file_max_size),       NULL, 10485760 },
    { "ddl_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, ddl_log_file_rotation_number),NULL, 5 },
    { "ddl_log_level",              CFG_STRING, offsetof(app_config_t, ddl_log_level),               "DEBUG", 0 },


	/* ----------------------------------------------------------------
     * Logging procedure log
     * ---------------------------------------------------------------- */
    { "procedure_log_file_name",          CFG_STRING, offsetof(app_config_t, procedure_log_file_name),          "procedure_app.log", 0 },
    { "procedure_log_file_max_size",      CFG_INT,    offsetof(app_config_t, procedure_log_file_max_size),       NULL, 10485760 },
    { "procedure_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, procedure_log_file_rotation_number),NULL, 5 },
    { "procedure_log_level",              CFG_STRING, offsetof(app_config_t, procedure_log_level),               "DEBUG", 0 },




	/*Additional logs  error , metrics, tx , sec . crypt, audit , session added 31-May-2026*/
	/* ----------------------------------------------------------------
     * Logging error log
     * ---------------------------------------------------------------- */
    { "error_log_file_name",          CFG_STRING, offsetof(app_config_t, error_log_file_name),          "error_app.log", 0 },
    { "error_log_file_max_size",      CFG_INT,    offsetof(app_config_t, error_log_file_max_size),       NULL, 10485760 },
    { "error_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, error_log_file_rotation_number),NULL, 5 },
    { "error_log_level",              CFG_STRING, offsetof(app_config_t, error_log_level),               "DEBUG", 0 },



	/* ----------------------------------------------------------------
     * Logging metrics log
     * ---------------------------------------------------------------- */
    { "metrics_log_file_name",          CFG_STRING, offsetof(app_config_t, metrics_log_file_name),          "metrics_app.log", 0 },
    { "metrics_log_file_max_size",      CFG_INT,    offsetof(app_config_t, metrics_log_file_max_size),       NULL, 10485760 },
    { "metrics_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, metrics_log_file_rotation_number),NULL, 5 },
    { "metrics_log_level",              CFG_STRING, offsetof(app_config_t, metrics_log_level),               "DEBUG", 0 },



	/* ----------------------------------------------------------------
     * Logging Transaction log
     * ---------------------------------------------------------------- */
    { "tx_log_file_name",          CFG_STRING, offsetof(app_config_t, transaction_log_file_name),          "transaction_app.log", 0 },
    { "tx_log_file_max_size",      CFG_INT,    offsetof(app_config_t, transaction_log_file_max_size),       NULL, 10485760 },
    { "tx_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, transaction_log_file_rotation_number),NULL, 5 },
    { "tx_log_level",              CFG_STRING, offsetof(app_config_t, transaction_log_level),               "DEBUG", 0 },



	/* ----------------------------------------------------------------
     * Logging Security log
     * ---------------------------------------------------------------- */
    { "sec_log_file_name",          CFG_STRING, offsetof(app_config_t, security_log_file_name),          "security_app.log", 0 },
    { "sec_log_file_max_size",      CFG_INT,    offsetof(app_config_t, security_log_file_max_size),       NULL, 10485760 },
    { "sec_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, security_log_file_rotation_number),NULL, 5 },
    { "sec_log_level",              CFG_STRING, offsetof(app_config_t, security_log_level),               "DEBUG", 0 },



	/* ----------------------------------------------------------------
     * Logging crypt log
     * ---------------------------------------------------------------- */
    { "crypt_log_file_name",          CFG_STRING, offsetof(app_config_t, crypt_log_file_name),          "crypt_app.log", 0 },
    { "crypt_log_file_max_size",      CFG_INT,    offsetof(app_config_t, crypt_log_file_max_size),       NULL, 10485760 },
    { "crypt_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, crypt_log_file_rotation_number),NULL, 5 },
    { "crypt_log_level",              CFG_STRING, offsetof(app_config_t, crypt_log_level),               "DEBUG", 0 },


	/* ----------------------------------------------------------------
     * Logging audit log
     * ---------------------------------------------------------------- */
    { "audit_log_file_name",          CFG_STRING, offsetof(app_config_t, audit_log_file_name),          "audit_app.log", 0 },
    { "audit_log_file_max_size",      CFG_INT,    offsetof(app_config_t, audit_log_file_max_size),       NULL, 10485760 },
    { "audit_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, audit_log_file_rotation_number),NULL, 5 },
    { "audit_log_level",              CFG_STRING, offsetof(app_config_t, audit_log_level),               "DEBUG", 0 },


	/* ----------------------------------------------------------------
     * Logging session log
     * ---------------------------------------------------------------- */
    { "session_log_file_name",          CFG_STRING, offsetof(app_config_t, session_log_file_name),          "security_app.log", 0 },
    { "session_log_file_max_size",      CFG_INT,    offsetof(app_config_t, session_log_file_max_size),       NULL, 10485760 },
    { "session_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, session_log_file_rotation_number),NULL, 5 },
    { "session_log_level",              CFG_STRING, offsetof(app_config_t, session_log_level),               "DEBUG", 0 },



	/* ----------------------------------------------------------------
     * Logging sql_parser log
     * ---------------------------------------------------------------- */
    { "sql_parser_log_file_name",          CFG_STRING, offsetof(app_config_t, sql_parser_log_file_name),          "sql_parser_app.log", 0 },
    { "sql_parser_log_file_max_size",      CFG_INT,    offsetof(app_config_t, sql_parser_log_file_max_size),       NULL, 10485760 },
    { "sql_parser_log_file_rotation_number", CFG_INT,  offsetof(app_config_t, sql_parser_log_file_rotation_number),NULL, 5 },
    { "sql_parser_log_level",              CFG_STRING, offsetof(app_config_t, sql_parser_log_level),               "DEBUG", 0 },

#	/*End Additional logs  error , metrics, tx , sec . crypt, audit , session added 31-May-2026*/


	/* ----------------------------------------------------------------
     * XML directories
     * ---------------------------------------------------------------- */
    { "xml_input_dir",  CFG_STRING, offsetof(app_config_t, xml_input_dir),  "./input_xml",  0 },
    { "xml_output_dir", CFG_STRING, offsetof(app_config_t, xml_output_dir), "./output_xml", 0 },
    { "xml_error_dir",  CFG_STRING, offsetof(app_config_t, xml_error_dir),  "./error_xml",  0 },

    /* ----------------------------------------------------------------
     * LOB limits
     * ---------------------------------------------------------------- */
    { "max_BLOBS_per_record",   CFG_INT, offsetof(app_config_t, max_BLOBS_per_record),   NULL, 5 },
    { "max_CLOBS_per_record",   CFG_INT, offsetof(app_config_t, max_CLOBS_per_record),   NULL, 5 },
    { "chunk_read_size",        CFG_INT, offsetof(app_config_t, chunk_read_size),         NULL, 65536 },
    { "query_max_record_count", CFG_INT, offsetof(app_config_t, query_max_record_count),  NULL, 500 },

    /* BLOB filename / MIME columns */
    { "BLOB_default_file_name_col",   CFG_STRING, offsetof(app_config_t, BLOB_default_file_name_col),   "FILE_NAME", 0 },
    { "BLOB_default_file_name_col_1", CFG_STRING, offsetof(app_config_t, BLOB_default_file_name_col_1), "FILE_NAME", 0 },
    { "BLOB_default_file_name_col_2", CFG_STRING, offsetof(app_config_t, BLOB_default_file_name_col_2), "FILE_NAME", 0 },
    { "BLOB_default_file_name_col_3", CFG_STRING, offsetof(app_config_t, BLOB_default_file_name_col_3), "FILE_NAME", 0 },
    { "BLOB_default_file_name_col_4", CFG_STRING, offsetof(app_config_t, BLOB_default_file_name_col_4), "FILE_NAME", 0 },
    { "BLOB_default_file_name_col_5", CFG_STRING, offsetof(app_config_t, BLOB_default_file_name_col_5), "FILE_NAME", 0 },
    { "BLOB_default_MIME_col",        CFG_STRING, offsetof(app_config_t, BLOB_default_MIME_col),        ".jpg", 0 },
    { "BLOB_default_MIME_TYPE_col_1", CFG_STRING, offsetof(app_config_t, BLOB_default_MIME_TYPE_col_1), ".jpg", 0 },
    { "BLOB_default_MIME_TYPE_col_2", CFG_STRING, offsetof(app_config_t, BLOB_default_MIME_TYPE_col_2), ".jpg", 0 },
    { "BLOB_default_MIME_TYPE_col_3", CFG_STRING, offsetof(app_config_t, BLOB_default_MIME_TYPE_col_3), ".jpg", 0 },
    { "BLOB_default_MIME_TYPE_col_4", CFG_STRING, offsetof(app_config_t, BLOB_default_MIME_TYPE_col_4), ".jpg", 0 },
    { "BLOB_default_MIME_TYPE_col_5", CFG_STRING, offsetof(app_config_t, BLOB_default_MIME_TYPE_col_5), ".jpg", 0 },
    { "BLOB_default_file_name",       CFG_STRING, offsetof(app_config_t, BLOB_default_file_name),       "blob_output", 0 },
    { "BLOB_default_MIME_TYPE",       CFG_STRING, offsetof(app_config_t, BLOB_default_MIME_TYPE),       ".bin", 0 },
    { "BLOB_append_file_timestamp",   CFG_INT,    offsetof(app_config_t, BLOB_append_file_timestamp),   NULL, 1 },
    { "query_fetch_batch_size",       CFG_INT,    offsetof(app_config_t, query_fetch_batch_size),        NULL, 1 },
    { "clob_default_extension",       CFG_STRING, offsetof(app_config_t, clob_default_extension),       ".txt", 0 },

    /* ----------------------------------------------------------------
     * Insert template defaults
     * ---------------------------------------------------------------- */
    { "insert_table_defaults",        CFG_INT,    offsetof(app_config_t, insert_table_defaults),         NULL, 0 },
    { "insert_default_number",        CFG_STRING, offsetof(app_config_t, insert_default_number),         "0",   0 },
    { "insert_default_float",         CFG_STRING, offsetof(app_config_t, insert_default_float),          "0.0", 0 },
    { "insert_default_binary_float",  CFG_STRING, offsetof(app_config_t, insert_default_binary_float),   "0.0", 0 },
    { "insert_default_binary_double", CFG_STRING, offsetof(app_config_t, insert_default_binary_double),  "0.0", 0 },
    { "insert_default_char",          CFG_STRING, offsetof(app_config_t, insert_default_char),           " ",   0 },
    { "insert_default_varchar2",      CFG_STRING, offsetof(app_config_t, insert_default_varchar2),       " ",   0 },
    { "insert_default_nchar",         CFG_STRING, offsetof(app_config_t, insert_default_nchar),          " ",   0 },
    { "insert_default_nvarchar2",     CFG_STRING, offsetof(app_config_t, insert_default_nvarchar2),      " ",   0 },
    { "insert_default_date",          CFG_STRING, offsetof(app_config_t, insert_default_date),           "2000-01-01", 0 },
    { "insert_default_timestamp",     CFG_STRING, offsetof(app_config_t, insert_default_timestamp),      "2000-01-01 00:00:00", 0 },
    { "insert_default_interval_ym",   CFG_STRING, offsetof(app_config_t, insert_default_interval_ym),    "0-0", 0 },
    { "insert_default_interval_ds",   CFG_STRING, offsetof(app_config_t, insert_default_interval_ds),    "0 00:00:00", 0 },
    { "insert_default_raw",           CFG_STRING, offsetof(app_config_t, insert_default_raw),            "00", 0 },
    { "insert_default_clob",          CFG_STRING, offsetof(app_config_t, insert_default_clob),           " ",  0 },
    { "insert_default_nclob",         CFG_STRING, offsetof(app_config_t, insert_default_nclob),          " ",  0 },
    { "insert_default_blob",          CFG_STRING, offsetof(app_config_t, insert_default_blob),           "",   0 },
    { "insert_default_rowid",         CFG_STRING, offsetof(app_config_t, insert_default_rowid),          "",   0 },
    { "insert_default_urowid",        CFG_STRING, offsetof(app_config_t, insert_default_urowid),         "",   0 },

    /* ----------------------------------------------------------------
     * Bulk insert control
     * ---------------------------------------------------------------- */
    { "max_bulk_inserts", CFG_INT, offsetof(app_config_t, max_bulk_inserts), NULL, 500 },

    /* ================================================================
     * Connection pool parameters
     * ================================================================ */

    /* Master switch */
    { "use_connection_pool", CFG_BOOL, offsetof(app_config_t, use_connection_pool), NULL, 0 },

    /* Pool sizing */
    { "pool_min_size",  CFG_INT, offsetof(app_config_t, pool_min_size),  NULL, 1  },
    { "pool_max_size",  CFG_INT, offsetof(app_config_t, pool_max_size),  NULL, 10 },
    { "pool_increment", CFG_INT, offsetof(app_config_t, pool_increment), NULL, 1  },

    /* Timeouts */
    { "pool_connection_timeout",          CFG_INT, offsetof(app_config_t, pool_connection_timeout),          NULL, 30   },
    { "session_idle_timeout",             CFG_INT, offsetof(app_config_t, session_idle_timeout),             NULL, 300  },
    { "max_time_to_establish",            CFG_INT, offsetof(app_config_t, max_time_to_establish),            NULL, 15   },
    { "network_read_write_timeout",       CFG_INT, offsetof(app_config_t, network_read_write_timeout),       NULL, 60   },
    { "query_execution_timeout",          CFG_INT, offsetof(app_config_t, query_execution_timeout),          NULL, 0    },
    { "authentication_handshake_timeout", CFG_INT, offsetof(app_config_t, authentication_handshake_timeout), NULL, 10   },
    { "login_auth_timeout",               CFG_INT, offsetof(app_config_t, login_auth_timeout),               NULL, 10   },
    { "session_max_lifetime",             CFG_INT, offsetof(app_config_t, session_max_lifetime),             NULL, 3600 },
    { "heartbeat_keepalive_interval",     CFG_INT, offsetof(app_config_t, heartbeat_keepalive_interval),     NULL, 60   },

    /* Retry */
    { "retries_on_connection_failure", CFG_INT, offsetof(app_config_t, retries_on_connection_failure), NULL, 3 },

    /* Session behaviour */
    { "connection_validation_on_borrow", CFG_BOOL, offsetof(app_config_t, connection_validation_on_borrow), NULL, 1 },
    { "rollback_on_return_to_pool",      CFG_BOOL, offsetof(app_config_t, rollback_on_return_to_pool),      NULL, 1 },
    { "autocommit_mode",                 CFG_BOOL, offsetof(app_config_t, autocommit_mode),                 NULL, 0 },

    /* NLS strings */
    { "nls_date_format",     CFG_STRING, offsetof(app_config_t, nls_date_format),     "YYYY-MM-DD HH24:MI:SS", 0 },
    { "nls_language",        CFG_STRING, offsetof(app_config_t, nls_language),        "AMERICAN",              0 },
    { "nls_territory",       CFG_STRING, offsetof(app_config_t, nls_territory),       "AMERICA",               0 },
    { "nls_characterset",    CFG_STRING, offsetof(app_config_t, nls_characterset),    "AL32UTF8",              0 },
    { "nls_session_timezone",CFG_STRING, offsetof(app_config_t, nls_session_timezone),"UTC",                   0 },

	/* Result Set Cache */
	    { "resultset_cache_enabled",          CFG_BOOL,   offsetof(app_config_t, resultset_cache_enabled),          NULL, 0      },
	    { "resultset_cache_ttl_seconds",      CFG_INT,    offsetof(app_config_t, resultset_cache_ttl_seconds),      NULL, 300    },
	    { "resultset_cache_max_entries",      CFG_INT,    offsetof(app_config_t, resultset_cache_max_entries),      NULL, 1000   },
	    { "resultset_cache_max_memory_mb",    CFG_INT,    offsetof(app_config_t, resultset_cache_max_memory_mb),    NULL, 256    },
	    { "resultset_cache_bucket_count",     CFG_INT,    offsetof(app_config_t, resultset_cache_bucket_count),     NULL, 2048   },
	    { "resultset_cache_hash_algorithm",   CFG_STRING, offsetof(app_config_t, resultset_cache_hash_algorithm),   "fnv1a", 0   },


           /* Transaction Manager */
           { "tx_timeout_seconds",  CFG_INT,  offsetof(app_config_t, tx_timeout_seconds),  NULL, 300 },
           { "tx_max_retries",      CFG_INT,  offsetof(app_config_t, tx_max_retries),      NULL, 3   },
           { "tx_retry_delay_ms",   CFG_INT,  offsetof(app_config_t, tx_retry_delay_ms),   NULL, 500 },
           { "tx_log_begin",        CFG_BOOL, offsetof(app_config_t, tx_log_begin),        NULL, 1   },
           { "tx_log_commit",       CFG_BOOL, offsetof(app_config_t, tx_log_commit),       NULL, 1   },
           { "tx_log_rollback",     CFG_BOOL, offsetof(app_config_t, tx_log_rollback),     NULL, 1   },
           { "tx_log_timeout",      CFG_BOOL, offsetof(app_config_t, tx_log_timeout),      NULL, 1   },

	    /* Metadata Cache */
	    { "metadata_cache_enabled",           CFG_BOOL,   offsetof(app_config_t, metadata_cache_enabled),           NULL, 0      },
	    { "metadata_cache_ttl_seconds",       CFG_INT,    offsetof(app_config_t, metadata_cache_ttl_seconds),       NULL, 3600   },
	    { "metadata_cache_max_entries",       CFG_INT,    offsetof(app_config_t, metadata_cache_max_entries),       NULL, 500    },
	    { "metadata_cache_max_memory_mb",     CFG_INT,    offsetof(app_config_t, metadata_cache_max_memory_mb),     NULL, 64     },
	    { "metadata_cache_bucket_count",      CFG_INT,    offsetof(app_config_t, metadata_cache_bucket_count),      NULL, 512    },
	    { "metadata_cache_hash_algorithm",    CFG_STRING, offsetof(app_config_t, metadata_cache_hash_algorithm),    "fnv1a", 0   },

	    /* Statement Cache */
	    { "statement_cache_enabled",          CFG_BOOL,   offsetof(app_config_t, statement_cache_enabled),          NULL, 0      },
	    { "statement_cache_ttl_seconds",      CFG_INT,    offsetof(app_config_t, statement_cache_ttl_seconds),      NULL, 1800   },
	    { "statement_cache_max_entries",      CFG_INT,    offsetof(app_config_t, statement_cache_max_entries),      NULL, 200    },
	    { "statement_cache_max_memory_mb",    CFG_INT,    offsetof(app_config_t, statement_cache_max_memory_mb),    NULL, 32     },
	    { "statement_cache_bucket_count",     CFG_INT,    offsetof(app_config_t, statement_cache_bucket_count),     NULL, 256    },
	    { "statement_cache_hash_algorithm",   CFG_STRING, offsetof(app_config_t, statement_cache_hash_algorithm),   "fnv1a", 0   },


	    /* Metrics */
	    { "metrics_display_input_file_name",      CFG_INT,    offsetof(app_config_t, metrics_display_input_file_name),      NULL, 0  },
	    { "metrics_display_input_request",      CFG_INT,    offsetof(app_config_t, metrics_display_input_request),      NULL, 0  },
	    { "metrics_display_output_response",      CFG_INT,    offsetof(app_config_t, metrics_display_output_response),      NULL, 0  },


		/*Session Cache*/
	    { "session_cache_enabled",          CFG_BOOL,   offsetof(app_config_t, session_cache_enabled),          NULL, 1      },
		{ "session_cache_ttl_seconds",      CFG_INT,    offsetof(app_config_t, session_cache_ttl_seconds),      NULL, 1800   },
        { "session_cache_hash_algorithm",   CFG_STRING, offsetof(app_config_t, session_cache_hash_algorithm),   "fnv1a", 0   },
	    { "session_cache_max_entries",      CFG_INT,    offsetof(app_config_t, session_cache_max_entries),      NULL, 5000   },
	    { "session_cache_max_memory_mb",    CFG_INT,    offsetof(app_config_t, session_cache_max_memory_mb),    NULL, 128    },
	    { "session_cache_bucket_count",     CFG_INT,    offsetof(app_config_t, session_cache_bucket_count),     NULL, 4096   },

	    { "session_default_ttl_seconds",    CFG_INT,    offsetof(app_config_t, session_default_ttl_seconds),    NULL, 1800   },
	    { "session_log_create",             CFG_BOOL,   offsetof(app_config_t, session_log_create),             NULL, 1      },
	    { "session_log_end",                CFG_BOOL,   offsetof(app_config_t, session_log_end),                NULL, 1      },
	    { "session_log_reconcile",          CFG_BOOL,   offsetof(app_config_t, session_log_reconcile),          NULL, 1      },

};

static size_t ctx_map_count = sizeof(ctx_map) / sizeof(ctx_map[0]);

/* ------------------------------------------------------------------ */
/*  Config validation helpers                                           */
/* ------------------------------------------------------------------ */

/*
 * ini_has_key()
 * Case-insensitive check for whether name exists in the parsed ini file,
 * independent of its value. Used to distinguish "key present but happens
 * to match its default value" from "key genuinely absent" -
 * ini_get_str()/int()/bool() alone can't tell those apart, since they
 * silently return the caller-supplied default for either case.
 */
static int ini_has_key(const ini_file_t *ini, const char *name)
{
    for (size_t i = 0; i < ini->count; i++)
        if (strcasecmp(ini->entries[i].name, name) == 0)
            return 1;
    return 0;
}

/*
 * ctx_map_has_name()
 * Case-insensitive check for whether name has a matching entry anywhere
 * in ctx_map[]. Used to flag config.ini keys the loader doesn't
 * recognise at all - almost always a typo or a stale/renamed setting.
 */
static int ctx_map_has_name(const char *name)
{
    for (size_t i = 0; i < ctx_map_count; i++)
        if (strcasecmp(ctx_map[i].name, name) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Utility                                                             */
/* ------------------------------------------------------------------ */
static char *trim(char *str)
{
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/* ------------------------------------------------------------------ */
/*  INI accessors                                                       */
/* ------------------------------------------------------------------ */
const char *ini_get_str(const ini_file_t *ini, const char *name,
                         const char *default_val)
{
    for (size_t i = 0; i < ini->count; i++)
        if (strcasecmp(ini->entries[i].name, name) == 0)
            return ini->entries[i].value;
    return default_val;
}

int ini_get_int(const ini_file_t *ini, const char *name, int default_val)
{
    const char *val = ini_get_str(ini, name, NULL);
    if (!val) return default_val;
    char *endptr;
    long num = strtol(val, &endptr, 10);
    if (*endptr != '\0') return default_val;
    return (int)num;
}

int ini_get_bool(const ini_file_t *ini, const char *name, int default_val)
{
    const char *val = ini_get_str(ini, name, NULL);
    if (!val) return default_val;
    if (strcasecmp(val, "1") == 0 || strcasecmp(val, "true")  == 0) return 1;
    if (strcasecmp(val, "0") == 0 || strcasecmp(val, "false") == 0) return 0;
    return default_val;
}

void ini_dump(const ini_file_t *ini)
{
    printf("INI File Contents (%zu entries):\n", ini->count);
    for (size_t i = 0; i < ini->count; i++)
        printf("  %s = %s\n", ini->entries[i].name, ini->entries[i].value);
}

void ini_free(ini_file_t *ini)
{
    for (size_t i = 0; i < ini->count; i++) {
        free(ini->entries[i].name);
        free(ini->entries[i].value);
    }
    free(ini->entries);
    ini->entries = NULL;
    ini->count   = 0;
}

/* ================================================================== */
/*  load_ini                                                            */
/* ================================================================== */
int load_ini(const char *filename, app_config_t *config, oci_context_t *ctx)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;

    /* ---- Memory-safety guard tracking ----
     * Records every CFG_STRING field whose maxlen never gets set by a
     * real "else if (!strcmp(m->name, ...)) maxlen=sizeof(config->...)"
     * branch below - exactly the bug class that caused the
     * 2026-07-24 stack-buffer-overflow (three *_cache_hash_algorithm
     * fields silently fell through to an old unsafe 256-byte default,
     * then strncpy() zero-padded up to that length regardless of the
     * value's real length, blowing straight through their real,
     * smaller destination fields). This turns any FUTURE instance of
     * the same bug class - e.g. a new CFG_STRING field added to the
     * ctx_map below without its own maxlen branch - into a loud,
     * fail-closed startup error instead of a silent, possibly
     * unnoticed-for-months memory corruption.                          */
    char unguarded_names[32][128];
    int  unguarded_count = 0;

    ini_file_t ini;
    size_t capacity = 16;
    ini.entries = malloc(capacity * sizeof(ini_entry_t));
    ini.count   = 0;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (*p == '#' || (p[0] == '/' && p[1] == '/')) continue;
        if (*p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        char *name  = trim(p);
        char *value = trim(eq + 1);

        /* Strip inline comments:  value = something   # comment
         * Walk the value looking for a bare # or //.
         * We do NOT strip # inside a quoted string, but since none of
         * our values are quoted this simple scan is safe.            */
        {
            char *cp = value;
            while (*cp)
            {
                if (*cp == '#')
                { *cp = '\0'; break; }
                if (*cp == '/' && *(cp + 1) == '/')
                { *cp = '\0'; break; }
                cp++;
            }
            /* Re-trim trailing whitespace left before the comment    */
            int vlen = (int)strlen(value);
            while (vlen > 0 && isspace((unsigned char)value[vlen - 1]))
            { value[vlen - 1] = '\0'; vlen--; }
        }

        if (ini.count >= capacity) {
            capacity *= 2;
            ini.entries = realloc(ini.entries,
                                  capacity * sizeof(ini_entry_t));
        }
        ini.entries[ini.count].name  = strdup(name);
        ini.entries[ini.count].value = strdup(value);
        ini.count++;
    }
    fclose(fp);

    /* Track which ctx_map[] entries were actually found in config.ini,
     * as opposed to silently falling back to their default value.      */
    int *loaded = calloc(ctx_map_count, sizeof(int));
    if (!loaded)
    {
        ini_free(&ini);
        return -1;
    }

    /* ---------- Populate config struct from map ---------- */
    for (size_t i = 0; i < ctx_map_count; i++) {
        ctx_config_map_t *m = &ctx_map[i];
        char *field_ptr = (char *)config + m->offset;

        loaded[i] = ini_has_key(&ini, m->name);

        switch (m->type) {

            case CFG_BOOL: {
                int val = ini_get_bool(&ini, m->name, m->default_int);
                *((int *)field_ptr) = val;
                break;
            }

            case CFG_INT: {
                int val = ini_get_int(&ini, m->name, m->default_int);
                *((int *)field_ptr) = val;
                break;
            }

            case CFG_STRING: {
                const char *val = ini_get_str(&ini, m->name, m->default_str);
                if (val) {
                    /* ---- Determine correct field size ----
                     * 0 is a sentinel, not a real default: no
                     * sizeof(config->anything) is ever 0 for a
                     * char[N] field, so maxlen still being 0 after the
                     * whole chain below reliably means no branch
                     * matched this field's name - see the memory-
                     * safety guard check right after this chain.       */
                    size_t maxlen = 0;

                    /* Logging */
                    if      (!strcmp(m->name,"log_file_name"))              maxlen=sizeof(config->log_file_name);
                    else if (!strcmp(m->name,"TEST_SQL_FILE_NAME"))         maxlen=sizeof(config->TEST_SQL_FILE_NAME);
                    else if (!strcmp(m->name,"log_level"))                  maxlen=sizeof(config->log_level);
                    else if      (!strcmp(m->name,"select_log_file_name"))              maxlen=sizeof(config->select_log_file_name);
					else if (!strcmp(m->name,"select_log_level"))                  maxlen=sizeof(config->select_log_level);
					else if      (!strcmp(m->name,"cache_log_file_name"))              maxlen=sizeof(config->cache_log_file_name);
				     else if (!strcmp(m->name,"cache_log_level"))                  maxlen=sizeof(config->cache_log_level);
				     else if      (!strcmp(m->name,"Metadata_log_file_name"))              maxlen=sizeof(config->Metadata_log_file_name);
					else if (!strcmp(m->name,"Metadata_log_level"))                  maxlen=sizeof(config->Metadata_log_level);
				     else if      (!strcmp(m->name,"connection_log_file_name"))              maxlen=sizeof(config->connection_log_file_name);
					else if (!strcmp(m->name,"connection_log_level"))                  maxlen=sizeof(config->connection_log_level);
				     else if      (!strcmp(m->name,"connectionpool_log_file_name"))              maxlen=sizeof(config->connectionpool_log_file_name);
					else if (!strcmp(m->name,"connectionpool_log_level"))                  maxlen=sizeof(config->connectionpool_log_level);
				     else if      (!strcmp(m->name,"update_log_file_name"))              maxlen=sizeof(config->update_log_file_name);
					else if (!strcmp(m->name,"update_log_level"))                  maxlen=sizeof(config->update_log_level);
				     else if      (!strcmp(m->name,"insert_log_file_name"))              maxlen=sizeof(config->insert_log_file_name);
					else if (!strcmp(m->name,"insert_log_level"))                  maxlen=sizeof(config->insert_log_level);
				     else if      (!strcmp(m->name,"delete_log_file_name"))              maxlen=sizeof(config->delete_log_file_name);
				     else if (!strcmp(m->name,"delete_log_level"))                  maxlen=sizeof(config->delete_log_level);
				     else if      (!strcmp(m->name,"dml_log_file_name"))              maxlen=sizeof(config->dml_log_file_name);
					else if (!strcmp(m->name,"dml_log_level"))                  maxlen=sizeof(config->dml_log_level);
				     else if      (!strcmp(m->name,"procedure_log_file_name"))              maxlen=sizeof(config->procedure_log_file_name);
					else if (!strcmp(m->name,"procedure_log_level"))                  maxlen=sizeof(config->procedure_log_level);
				     else if      (!strcmp(m->name,"ddl_log_file_name"))              maxlen=sizeof(config->ddl_log_file_name);
					else if (!strcmp(m->name,"ddl_log_level"))                  maxlen=sizeof(config->ddl_log_level);


                    /*Additional log gaurds 07-MJun-2026*/
					else if      (!strcmp(m->name,"connection_log_file_name"))              maxlen=sizeof(config->connection_log_file_name);
					else if (!strcmp(m->name,"connection_log_level"))                  maxlen=sizeof(config->connection_log_level);
					else if      (!strcmp(m->name,"connectionpool_log_file_name"))              maxlen=sizeof(config->connectionpool_log_file_name);
					else if (!strcmp(m->name,"connectionpool_log_level"))                  maxlen=sizeof(config->connectionpool_log_level);
					else if (!strcmp(m->name,"resultset_cache_hash_algorithm"))
					         maxlen=sizeof(config->resultset_cache_hash_algorithm);
					else if (!strcmp(m->name,"metadata_cache_hash_algorithm"))
					         maxlen=sizeof(config->metadata_cache_hash_algorithm);
					else if (!strcmp(m->name,"statement_cache_hash_algorithm"))
					         maxlen=sizeof(config->statement_cache_hash_algorithm);
					else if (!strcmp(m->name,"session_cache_hash_algorithm"))
					         maxlen=sizeof(config->session_cache_hash_algorithm);



                    /*Additional logs  error , metrics, tx , sec . crypt, audit , session added 31-May-2026*/
				     else if      (!strcmp(m->name,"error_log_file_name"))              maxlen=sizeof(config->error_log_file_name);
					else if (!strcmp(m->name,"error_log_level"))                  maxlen=sizeof(config->error_log_level);
				     else if      (!strcmp(m->name,"metrics_log_file_name"))              maxlen=sizeof(config->metrics_log_file_name);
					else if (!strcmp(m->name,"metrics_log_level"))                  maxlen=sizeof(config->metrics_log_level);
				     else if      (!strcmp(m->name,"tx_log_file_name"))              maxlen=sizeof(config->transaction_log_file_name);
					else if (!strcmp(m->name,"tx_log_level"))                  maxlen=sizeof(config->transaction_log_level);
					else if      (!strcmp(m->name,"sec_log_file_name"))              maxlen=sizeof(config->security_log_file_name);
					else if (!strcmp(m->name,"sec_log_level"))                  maxlen=sizeof(config->security_log_level);
					else if      (!strcmp(m->name,"crypt_log_file_name"))              maxlen=sizeof(config->crypt_log_file_name);
					else if (!strcmp(m->name,"crypt_log_level"))                  maxlen=sizeof(config->crypt_log_level);
					else if      (!strcmp(m->name,"audit_log_file_name"))              maxlen=sizeof(config->audit_log_file_name);
					else if (!strcmp(m->name,"audit_log_level"))                  maxlen=sizeof(config->audit_log_level);
					else if      (!strcmp(m->name,"session_log_file_name"))              maxlen=sizeof(config->session_log_file_name);
					else if (!strcmp(m->name,"session_log_level"))                  maxlen=sizeof(config->session_log_level);
					else if      (!strcmp(m->name,"sql_parser_log_file_name"))              maxlen=sizeof(config->sql_parser_log_file_name);
					else if (!strcmp(m->name,"sql_parser_log_level"))                  maxlen=sizeof(config->sql_parser_log_level);



                    /*End Additional logs  error , metrics, tx , sec . crypt, audit , session added 31-May-2026*/

                    /* Credentials / db */
                    else if (!strcmp(m->name,"username"))                   maxlen=sizeof(config->username);
                    else if (!strcmp(m->name,"password"))                   maxlen=sizeof(config->password);
                    else if (!strcmp(m->name,"dbname"))                     maxlen=sizeof(config->dbname);
                    else if (!strcmp(m->name,"wallet_location"))            maxlen=sizeof(config->wallet_location);

                    /* XML dirs */
                    else if (!strcmp(m->name,"xml_input_dir"))              maxlen=sizeof(config->xml_input_dir);
                    else if (!strcmp(m->name,"xml_output_dir"))             maxlen=sizeof(config->xml_output_dir);
                    else if (!strcmp(m->name,"xml_error_dir"))              maxlen=sizeof(config->xml_error_dir);

                    /* LOB paths */
                    else if (!strcmp(m->name,"BLOB_host_path"))             maxlen=sizeof(config->BLOB_host_path);
                    else if (!strcmp(m->name,"CLOB_URL_path"))              maxlen=sizeof(config->CLOB_URL_path);
                    else if (!strcmp(m->name,"BLOB_URL_path"))              maxlen=sizeof(config->BLOB_URL_path);
                    else if (!strcmp(m->name,"BLOB_output_dir"))            maxlen=sizeof(config->BLOB_output_dir);
                    else if (!strcmp(m->name,"CLOB_output_dir"))            maxlen=sizeof(config->CLOB_output_dir);

                    /* BLOB filename / MIME columns */
                    else if (!strcmp(m->name,"BLOB_default_file_name_col"))   maxlen=sizeof(config->BLOB_default_file_name_col);
                    else if (!strcmp(m->name,"BLOB_default_file_name_col_1")) maxlen=sizeof(config->BLOB_default_file_name_col_1);
                    else if (!strcmp(m->name,"BLOB_default_file_name_col_2")) maxlen=sizeof(config->BLOB_default_file_name_col_2);
                    else if (!strcmp(m->name,"BLOB_default_file_name_col_3")) maxlen=sizeof(config->BLOB_default_file_name_col_3);
                    else if (!strcmp(m->name,"BLOB_default_file_name_col_4")) maxlen=sizeof(config->BLOB_default_file_name_col_4);
                    else if (!strcmp(m->name,"BLOB_default_file_name_col_5")) maxlen=sizeof(config->BLOB_default_file_name_col_5);
                    else if (!strcmp(m->name,"BLOB_default_MIME_col"))        maxlen=sizeof(config->BLOB_default_MIME_col);
                    else if (!strcmp(m->name,"BLOB_default_MIME_TYPE_col_1")) maxlen=sizeof(config->BLOB_default_MIME_TYPE_col_1);
                    else if (!strcmp(m->name,"BLOB_default_MIME_TYPE_col_2")) maxlen=sizeof(config->BLOB_default_MIME_TYPE_col_2);
                    else if (!strcmp(m->name,"BLOB_default_MIME_TYPE_col_3")) maxlen=sizeof(config->BLOB_default_MIME_TYPE_col_3);
                    else if (!strcmp(m->name,"BLOB_default_MIME_TYPE_col_4")) maxlen=sizeof(config->BLOB_default_MIME_TYPE_col_4);
                    else if (!strcmp(m->name,"BLOB_default_MIME_TYPE_col_5")) maxlen=sizeof(config->BLOB_default_MIME_TYPE_col_5);
                    else if (!strcmp(m->name,"BLOB_default_MIME_TYPE"))       maxlen=sizeof(config->BLOB_default_MIME_TYPE);
                    else if (!strcmp(m->name,"BLOB_default_file_name"))       maxlen=sizeof(config->BLOB_default_file_name);
                    else if (!strcmp(m->name,"clob_default_extension"))       maxlen=sizeof(config->clob_default_extension);

                    /* Insert template defaults */
                    else if (!strcmp(m->name,"insert_default_number"))        maxlen=sizeof(config->insert_default_number);
                    else if (!strcmp(m->name,"insert_default_float"))         maxlen=sizeof(config->insert_default_float);
                    else if (!strcmp(m->name,"insert_default_binary_float"))  maxlen=sizeof(config->insert_default_binary_float);
                    else if (!strcmp(m->name,"insert_default_binary_double")) maxlen=sizeof(config->insert_default_binary_double);
                    else if (!strcmp(m->name,"insert_default_char"))          maxlen=sizeof(config->insert_default_char);
                    else if (!strcmp(m->name,"insert_default_varchar2"))      maxlen=sizeof(config->insert_default_varchar2);
                    else if (!strcmp(m->name,"insert_default_nchar"))         maxlen=sizeof(config->insert_default_nchar);
                    else if (!strcmp(m->name,"insert_default_nvarchar2"))     maxlen=sizeof(config->insert_default_nvarchar2);
                    else if (!strcmp(m->name,"insert_default_date"))          maxlen=sizeof(config->insert_default_date);
                    else if (!strcmp(m->name,"insert_default_timestamp"))     maxlen=sizeof(config->insert_default_timestamp);
                    else if (!strcmp(m->name,"insert_default_interval_ym"))   maxlen=sizeof(config->insert_default_interval_ym);
                    else if (!strcmp(m->name,"insert_default_interval_ds"))   maxlen=sizeof(config->insert_default_interval_ds);
                    else if (!strcmp(m->name,"insert_default_raw"))           maxlen=sizeof(config->insert_default_raw);
                    else if (!strcmp(m->name,"insert_default_clob"))          maxlen=sizeof(config->insert_default_clob);
                    else if (!strcmp(m->name,"insert_default_nclob"))         maxlen=sizeof(config->insert_default_nclob);
                    else if (!strcmp(m->name,"insert_default_blob"))          maxlen=sizeof(config->insert_default_blob);
                    else if (!strcmp(m->name,"insert_default_rowid"))         maxlen=sizeof(config->insert_default_rowid);
                    else if (!strcmp(m->name,"insert_default_urowid"))        maxlen=sizeof(config->insert_default_urowid);

                    /* Pool NLS strings */
                    else if (!strcmp(m->name,"nls_date_format"))              maxlen=sizeof(config->nls_date_format);
                    else if (!strcmp(m->name,"nls_language"))                 maxlen=sizeof(config->nls_language);
                    else if (!strcmp(m->name,"nls_territory"))                maxlen=sizeof(config->nls_territory);
                    else if (!strcmp(m->name,"nls_characterset"))             maxlen=sizeof(config->nls_characterset);
                    else if (!strcmp(m->name,"nls_session_timezone"))         maxlen=sizeof(config->nls_session_timezone);

                    if (maxlen == 0)
                    {
                        /* No branch above matched - the real destination
                         * size for this field is unknown, so there is
                         * no safe number of bytes to write. Record it
                         * and skip - reported, and load_ini() FAILS,
                         * once every field has been checked. Leaving
                         * config's field at its calloc'd zero (empty
                         * string) is safe; guessing a byte count is
                         * not.                                         */
                        if (unguarded_count < 32)
                            strncpy(unguarded_names[unguarded_count], m->name,
                                    sizeof(unguarded_names[0]) - 1);
                        unguarded_count++;
                    }
                    else
                    {
                        strncpy(field_ptr, val, maxlen - 1);
                        field_ptr[maxlen - 1] = '\0';
                    }
                }
                break;
            }
        }
    }

    /* ---------- Report any expected settings never found ---------- */
    int missing_count = 0;
    for (size_t i = 0; i < ctx_map_count; i++)
        if (!loaded[i]) missing_count++;

    if (missing_count > 0)
    {
        printf("\n");
        printf("================================================================\n");
        printf("CONFIG VALIDATION FAILED: %d expected setting(s) missing from %s\n",
               missing_count, filename);
        printf("================================================================\n");

        for (size_t i = 0; i < ctx_map_count; i++)
        {
            if (loaded[i]) continue;

            ctx_config_map_t *m = &ctx_map[i];
            if (m->type == CFG_STRING)
                printf("  MISSING: %-40s (would default to: \"%s\")\n",
                       m->name, m->default_str ? m->default_str : "");
            else
                printf("  MISSING: %-40s (would default to: %d)\n",
                       m->name, m->default_int);
        }

        printf("================================================================\n");
        printf("Refusing to start with %d missing configuration key(s).\n",
               missing_count);
        printf("Add the key(s) above to %s and try again.\n", filename);
        printf("================================================================\n\n");

        free(loaded);
        ini_free(&ini);
        return -1;
    }

    free(loaded);

    /* ---------- Warn about config.ini keys the loader doesn't recognise ---------- */
    int unknown_count = 0;
    for (size_t i = 0; i < ini.count; i++)
        if (!ctx_map_has_name(ini.entries[i].name))
            unknown_count++;

    if (unknown_count > 0)
    {
        printf("\n");
        printf("----------------------------------------------------------------\n");
        printf("CONFIG WARNING: %d key(s) in %s are not recognised by "
               "load_ini() - check for typos or stale/renamed settings:\n",
               unknown_count, filename);
        for (size_t i = 0; i < ini.count; i++)
            if (!ctx_map_has_name(ini.entries[i].name))
                printf("  UNKNOWN: %s = %s\n",
                       ini.entries[i].name, ini.entries[i].value);
        printf("----------------------------------------------------------------\n\n");
    }

    /* ---------- Memory-safety guard: fail closed on any unguarded field ----------
     * Unlike the unrecognised-keys warning above (informational only -
     * an unrecognised key just gets ignored, nothing unsafe happens),
     * this one is a hard failure: an unguarded field means load_ini()
     * was about to trust an unverified byte count for a real struct
     * field, exactly the bug class that caused the 2026-07-24 stack-
     * buffer-overflow. Nip it in the bud here rather than let the
     * caller run with a config it doesn't know is unsafe.              */
    if (unguarded_count > 0)
    {
        printf("\n");
        printf("================================================================\n");
        printf("CONFIG FAILURE: %d CFG_STRING field(s) in load_ini()'s ctx_map "
               "have no maxlen guard branch - their real destination size in "
               "app_config_t is unverified, so the safe byte count to write is "
               "unknown. This is exactly the bug class that caused the "
               "2026-07-24 stack-buffer-overflow (missing maxlen branches for "
               "*_cache_hash_algorithm silently defaulted to an unsafe 256-byte "
               "fallback). Add a\n"
               "  else if (!strcmp(m->name,\"<name>\")) maxlen=sizeof(config-><field>);\n"
               "branch for each field below before this config can be trusted:\n",
               unguarded_count);
        for (int i = 0; i < unguarded_count && i < 32; i++)
            printf("  UNGUARDED: %s\n", unguarded_names[i]);
        if (unguarded_count > 32)
            printf("  ...and %d more (only the first 32 are listed)\n",
                   unguarded_count - 32);
        printf("================================================================\n\n");

        ini_free(&ini);
        return -1;
    }

    /* Dump loaded values */
    ini_dump(&ini);

    /* Store pointer in context */
    ctx->ini = config;

    ini_free(&ini);
    return 0;
}

