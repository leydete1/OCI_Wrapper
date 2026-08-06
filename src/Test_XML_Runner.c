/*
 * Test_XML_Runner.c
 *
 * Self-discovering XML Test Runner
 * ----------------------------------
 * Reads every .xml file in ctx->ini->xml_input_dir, extracts the
 * <operation> element, and dispatches to the correct module:
 *
 *   INSERT             -> execute_insert_batch()
 *   SELECT             -> execute_query_batch()
 *   UPDATE             -> execute_update_batch()
 *   DELETE             -> execute_delete_batch()
 *   EXECUTE_PROCEDURE  -> execute_procedure()
 *
 * Adding a new test is as simple as dropping an XML file into the
 * input directory.  No code changes needed.
 *
 * Connection mode
 * ---------------
 * An optional last argument controls whether a direct OCI connection
 * or a connection pool is used:
 *
 *   --pool  or  1   ->  OCI_Connect_pool  / OCI_Disconnect_pool
 *   --direct or 0   ->  OCI_Connect       / OCI_Disconnect  (default)
 *
 * The setting can also be driven entirely from config.ini by setting
 *   use_connection_pool = 1
 * The command-line argument overrides the ini value when supplied.
 *
 * Usage:
 *   ./Test_XML_Runner <config.ini>           (direct, ini controls pool)
 *   ./Test_XML_Runner <config.ini> --pool    (force pool)
 *   ./Test_XML_Runner <config.ini> --direct  (force direct)
 *   ./Test_XML_Runner <config.ini> 1         (force pool)
 *   ./Test_XML_Runner <config.ini> 0         (force direct)
 *
 * Compile:
 *   gcc -o Test_XML_Runner \
 *       Test_XML_Runner.c \
 *       OCI_Insert_Execute_Module.c \
 *       OCI_Update_Execute_Module.c \
 *       OCI_Delete_Execute_Module.c \
 *       OCI_Execute_Procedure_Module.c \
 *       OCI_Insert_Template_Module.c \
 *       OCI_Insert_Validate_Module.c \
 *       OCI_Table_Metadata_Module.c \
 *       OCI_Execute_Query_Batch_Module.c \
 *       OCI_Execute_Query_Module.c \
 *       OCI_Connection.c \
 *       OCI_Connection_Pool.c \
 *       oci_cache.c resultset_cache.c \
 *       logger.c ini_reader.c \
 *       XML_Helper.c string_utils.c ctx_header.c \
 *       -I. -loci -lpthread -lm
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>    /* readdir()-adjacent declarations; sleep() itself
                          moved to file_consumer_runner.c along with the
                          scan loop it used to pace */
#include <time.h>      /* nanosleep() - drain-wait visibility poll below.
                          usleep() was tried first but needs feature-test
                          macros (_XOPEN_SOURCE/_DEFAULT_SOURCE) that
                          weren't reliably in effect for this translation
                          unit - nanosleep() is more portably declared
                          under the _POSIX_C_SOURCE this file already
                          expects, so switched rather than chase macros. */
#include <ctype.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "OCI_Insert_Execute_Module.h"
#include "OCI_Update_Execute_Module.h"
#include "OCI_Delete_Execute_Module.h"
#include "OCI_Execute_Procedure_Module.h"
#include "OCI_Insert_Template_Module.h"
#include "OCI_Insert_Validate_Module.h"
#include "logger.h"
#include "ini_reader.h"
#include "ctx_helper.h"
#include "resultset_cache.h"
#include "metadata_cache.h"
#include "sql_dependency_extractor.h"
#include "metrics.h"
#include "OCI_Transaction_Manager.h"
#include <sys/resource.h>
#include <stdio.h>
#include "session_cache.h"
#include "OCI_Session_Manager.h"
#include "OCI_Level1_Parser.h"
#include "OCI_Unit_Test_Module.h"
#include "OCI_Level2_Parser.h"
#include "OCI_Request_Response_Types.h"
#include "OCI_Execute_Query_Batch_Module.h"
#include "dispatcher.h"
#include "response_object.h"
#include "file_consumer.h"
#include "file_consumer_runner.h"
#include "queue_manager.h"
#include "session_touch_queue.h"
#include "session_manager_runner.h"
#include "worker.h"

/* looks_like_new_request_format()'s own forward declaration removed
 * 2026-08-01 - now level1_looks_like_new_format(), declared in
 * OCI_Level1_Parser.h (already included above).
 *
 * 2026-08-03 (Stage 1 extraction, File_Consumer_proposal v1.2):
 * process_xml_file(), dispatch_select(), dispatch_select_new(),
 * dispatch_insert_new(), dispatch_update_new(), dispatch_delete_new(),
 * dispatch_procedure_new(), read_file(), extract_tag(), and upper()
 * all moved to dispatcher.c/dispatcher.h - see that file's own header
 * comment. process_xml_file() and read_file() are exported from there
 * and used below unchanged; the rest were purely internal to the
 * dispatch chain and moved with no remaining call sites here.         */


typedef struct {
    const char *description;
    const char *sql;
    int         expect_pass;   /* 1 = should succeed, 0 = should fail */
} dep_test_case_t;

void print_limits(void);
/* ------------------------------------------------------------------ */
/*  Test cases                                                          */
/* ------------------------------------------------------------------ */
static dep_test_case_t test_cases[] = {

    /* ---- PASS cases ---- */
    {
        "Simple single table with alias",
        "SELECT t.id, t.description, t.file_name "
        "FROM oci_test_table t "
        "WHERE t.id = 1",
        1
    },
    {
        "Owner-qualified table",
        "SELECT t.id, t.name "
        "FROM data_manager.my_table t",
        1
    },
    {
        "Two tables comma join",
        "SELECT a.id, a.name, b.amount "
        "FROM orders a, order_lines b "
        "WHERE a.id = b.order_id",
        1
    },
    {
        "INNER JOIN with aliases",
        "SELECT e.emp_id, e.last_name, d.dept_name "
        "FROM employees e "
        "INNER JOIN departments d ON e.dept_id = d.dept_id",
        1
    },
    {
        "LEFT JOIN three tables",
        "SELECT e.emp_id, e.last_name, d.dept_name, l.location_name "
        "FROM employees e "
        "LEFT JOIN departments d ON e.dept_id = d.dept_id "
        "LEFT JOIN locations l ON d.location_id = l.location_id",
        1
    },
    {
        "Field with AS alias",
        "SELECT t.product_code AS code, t.unit_price AS price "
        "FROM products t",
        1
    },
    {
        "Field with space alias (no AS)",
        "SELECT t.product_code code, t.unit_price price "
        "FROM products t",
        1
    },
    {
        "Owner-qualified table with alias and field alias",
        "SELECT o.order_id AS id, o.total_amount AS total "
        "FROM sales.orders o "
        "WHERE o.status = 'OPEN'",
        1
    },
    {
        "No alias on table - field uses table name directly",
        "SELECT employees.emp_id, employees.last_name "
        "FROM employees",
        1
    },
    {
        "OCI_FIELD_TEST all column types",
        "SELECT t.number_col, t.varchar2_col, t.date_col, "
        "t.timestamp_col, t.clob_col, t.blob_col "
        "FROM oci_field_test t",
        1
    },

    /* ---- FAIL cases ---- */
    {
        "FAIL: unqualified field (no table prefix)",
        "SELECT id, name FROM my_table t",
        0
    },
    {
        "FAIL: SELECT *",
        "SELECT * FROM my_table t",
        0
    },
    {
        "FAIL: UNION",
        "SELECT t.id FROM table_a t UNION SELECT t.id FROM table_b t",
        0
    },
    {
        "FAIL: function in SELECT",
        "SELECT NVL(t.amount, 0), t.name FROM my_table t",
        0
    },
    {
        "FAIL: field references table not in FROM",
        "SELECT a.id, b.name FROM table_a a",
        0
    },
    {
        "FAIL: subquery in FROM",
        "SELECT t.id FROM (SELECT id FROM inner_table) t",
        0
    },
    {
        "FAIL: no FROM clause",
        "SELECT t.col1 WHERE t.col1 = 1",
        0
    },

    /* sentinel */
    { NULL, NULL, 0 }
};



/* MAX_XML_FILE_SIZE / MAX_OPERATION_LEN moved to dispatcher.c (Stage 1
 * extraction) - both were only used by code that moved with them.     */

/* File Consumer's scan interval and lifetime are now real config
 * (dispatcher.poll_interval_seconds / dispatcher.lifetime_seconds in
 * consumer_file.ini - see file_consumer_runner.c), not hardcoded test
 * knobs - Terry's proposal, 2026-08-05 "Findings and lessons" doc,
 * section 1b. This one constant is purely about shutdown-wait
 * visibility (see the consumer_type=FILE branch below) - not a
 * behavioural timeout, worker_pool_shutdown_and_join() always fully
 * drains regardless of how long that takes.                           */
#define FILE_CONSUMER_DRAIN_POLL_WARN_SECONDS       30

/* Session Manager proposal, Stage 2 (2026-08-06) - own thread, own
 * queue. No config key for the queue depth yet (Stage 2 keeps it
 * simple) - 1000 is comfortable headroom for tiny, fast-draining touch
 * messages; not expected to ever fill up under normal load given how
 * cheap each touch is compared to a full CRUD dispatch.               */
#define SESSION_TOUCH_QUEUE_DEPTH                   1000


/* ================================================================== */
/*  test_sql_dependency_extractor                                       */
/*  Run all test cases and log results to ctx->select_logger.          */
/*  Returns number of failures (0 = all passed).                       */
/* ================================================================== */
int test_sql_dependency_extractor(oci_context_t *ctx)
{
    int total   = 0;
    int passed  = 0;
    int failed  = 0;

    logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                 "========================================");
    logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                 "sql_dependency_extractor test suite");
    logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                 "========================================");

    for (int i = 0; test_cases[i].description != NULL; i++)
    {
        const dep_test_case_t *tc = &test_cases[i];
        total++;

        logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                     "------------------------------------------------");
        logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                     "Test %d: %s", total, tc->description);
        logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                     "SQL: %s", tc->sql);
        logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                     "Expected: %s",
                     tc->expect_pass ? "PASS" : "FAIL");

        OCI_DEPENDENCY_LIST deps;
        memset(&deps, 0, sizeof(deps));

        int rc = extract_sql_dependencies(tc->sql, &deps, ctx);

        int test_ok;
        if (tc->expect_pass)
            test_ok = (rc == 0);
        else
            test_ok = (rc != 0);

        if (test_ok)
        {
            passed++;
            logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                         "Result: PASS");

            /* Dump the dependency list for passing cases */
            if (rc == 0)
                sql_dep_dump(&deps, ctx);
        }
        else
        {
            failed++;
            logger_write(ctx->sql_parser_logger, LOG_ERROR, __func__, 0,
                         "Result: FAIL (rc=%d expected_pass=%d)",
                         rc, tc->expect_pass);
        }
    }

    logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                 "================================================");
    logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                 "Test suite complete: total=%d passed=%d failed=%d",
                 total, passed, failed);
    logger_write(ctx->sql_parser_logger, LOG_INFO, __func__, 0,
                 "================================================");

    return failed;
}

/* ================================================================== */
/*  show_metadata_integration_pattern                                   */
/*  Demonstrates how extract_sql_dependencies feeds get_request_metadata
 *  This is the intended downstream usage once the select module is
 *  updated to call the extractor.
 * ================================================================== */
void show_metadata_integration_pattern(oci_context_t *ctx)
{
    /*
     * Example SQL as it would arrive from input_xml.
     * Fields are TABLE_ALIAS.COLUMN_NAME.
     * Tables are listed with aliases in FROM.
     */
    const char *sql =
        "SELECT e.emp_id, e.last_name, e.salary, "
        "       d.dept_name, d.budget "
        "FROM employees e "
        "INNER JOIN departments d ON e.dept_id = d.dept_id "
        "WHERE e.status = 'ACTIVE' "
        "ORDER BY e.last_name";

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "=== Metadata integration pattern demo ===");
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "SQL: %s", sql);

    OCI_DEPENDENCY_LIST deps;
    memset(&deps, 0, sizeof(deps));

    if (extract_sql_dependencies(sql, &deps, ctx) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Extraction failed - cannot proceed to metadata");
        return;
    }

    /*
     * This is how get_multi_request_metadata will be called once the
     * select module is updated.  For now we just log what would be called.
     *
     * For each unique table in the dependency list, call
     * get_request_metadata to retrieve column metadata.
     * The metadata cache will serve repeated calls from cache.
     *
     *   for (int i = 0; i < deps.object_count; i++)
     *   {
     *       metadata_request_t req;
     *       memset(&req, 0, sizeof(req));
     *       strncpy(req.owner,      deps.objects[i].owner,
     *               sizeof(req.owner) - 1);
     *       strncpy(req.table_name, deps.objects[i].object_name,
     *               sizeof(req.table_name) - 1);
     *
     *       col_metadata_t cols[MAX_TABLE_COLUMNS];
     *       int col_count = 0;
     *
     *       // This call will be served from metadata_cache on
     *       // subsequent executions of the same query.
     *       metadata_cache_get_or_fetch(ctx->metadata_cache,
     *                                    ctx, &req,
     *                                    cols, &col_count,
     *                                    MAX_TABLE_COLUMNS);
     *   }
     */

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Would call get_request_metadata for %d object(s):",
                 deps.object_count);

    for (int i = 0; i < deps.object_count; i++)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "  [%d] owner='%s' table='%s' alias='%s'",
                     i + 1,
                     deps.objects[i].owner[0]
                         ? deps.objects[i].owner : "(resolve from cache)",
                     deps.objects[i].object_name,
                     deps.objects[i].alias[0]
                         ? deps.objects[i].alias : "(none)");
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Would cross-reference %d field(s) to resolved metadata:",
                 deps.field_count);

    for (int i = 0; i < deps.field_count; i++)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "  [%d] pos=%d '%s.%s'%s",
                     i + 1,
                     deps.fields[i].field_pos,
                     deps.fields[i].table_ref,
                     deps.fields[i].field_name,
                     deps.fields[i].field_alias[0]
                         ? " AS " : "",
                     deps.fields[i].field_alias[0]
                         ? deps.fields[i].field_alias : "");
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "=== end pattern demo ===");
}



/* read_file(), extract_tag(), upper(), and dispatch_select() moved to
 * dispatcher.c (Stage 1 extraction, File_Consumer_proposal v1.2). See
 * dispatcher.h for read_file()'s exported declaration - the other
 * three had no remaining call sites in this file after the move.     */


/* ================================================================== */
/*  dispatch_select                                                     */
/* ================================================================== */

    static int dispatch_create_session(oci_context_t *ctx,
                                        const char    *filepath,
                                        char          *out_session_id,
                                        size_t         out_session_id_size)
    {
        long  len = 0;
        char *xml = read_file(filepath, &len);
        if (!xml)
        {
            logger_write(ctx->session_logger, LOG_ERROR, __func__, 0,
                         "Failed to read session request file: %s", filepath);
            return -1;
        }

        session_request_t req;
        int rc = parse_session_request(ctx, xml, &req);
        free(xml);
        if (rc != SESSION_OK)
        {
            logger_write(ctx->session_logger, LOG_ERROR, __func__, 0,
                         "Failed to parse session request: %s", filepath);
            return -1;
        }

        char *result_xml = NULL;
        rc = session_create(ctx, &req, &result_xml);
        if (rc != SESSION_OK)
        {
            logger_write(ctx->session_logger, LOG_ERROR, __func__, 0,
                         "session_create FAILED (rc=%d)", rc);
            if (result_xml) free(result_xml);
            return -1;
        }

        logger_write(ctx->session_logger, LOG_INFO, __func__, 0,
                     "session_create OK:\n%s",
                     result_xml ? result_xml : "-");

        /* Pull session_id back out of the result XML for the caller  */
        if (result_xml && out_session_id)
        {
            const char *s = strstr(result_xml, "<session_id>");
            if (s)
            {
                s += strlen("<session_id>");
                const char *e = strstr(s, "</session_id>");
                if (e)
                {
                    size_t l = (size_t)(e - s);
                    if (l >= out_session_id_size) l = out_session_id_size - 1;
                    memcpy(out_session_id, s, l);
                    out_session_id[l] = '\0';
                }
            }
        }

        free(result_xml);
        return 0;
    }


/* dispatch_update (legacy, flat-XML <operation>UPDATE</operation>
 * format) removed - same reasoning as dispatch_insert's removal:
 * execute_update_batch()'s signature changed from a raw XML string to
 * update_request_t* as part of this refactor, so the old flat format
 * is retired for UPDATE too. See dispatch_update_new() below for the
 * new-pipeline replacement. */

/* dispatch_delete (legacy, flat-XML <operation>DELETE</operation>
 * format) removed - same reasoning as dispatch_insert/dispatch_update's
 * removal: execute_delete_batch()'s signature changed from a raw XML
 * string to delete_request_t* as part of this refactor, so the old
 * flat format is retired for DELETE too. See dispatch_delete_new()
 * below for the new-pipeline replacement. */

/* dispatch_procedure (legacy, flat-XML <Procedure_Template> format)
 * removed - same reasoning as dispatch_insert/update/delete's removal:
 * execute_procedure()'s signature changed from a raw XML string to
 * execute_procedure_request_t* as part of this refactor, so the old
 * flat format is retired for EXECUTE_PROCEDURE too. See
 * dispatch_procedure_new() below for the new-pipeline replacement. */

/* process_xml_file() moved to dispatcher.c (Stage 1 extraction,
 * File_Consumer_proposal v1.2) - exported via dispatcher.h and called
 * unchanged below. See dispatcher.c's own header comment.             */
/* ================================================================== */
/*  parse_pool_arg                                                      */
/*  Returns 1 = use pool, 0 = use direct, -1 = not recognised         */
/* ================================================================== */
static int parse_pool_arg(const char *arg)
{
    if (!arg) return -1;
    if (strcmp(arg, "--pool")   == 0 || strcmp(arg, "1") == 0) return 1;
    if (strcmp(arg, "--direct") == 0 || strcmp(arg, "0") == 0) return 0;
    return -1;
}

/* ================================================================== */
/*  initialise_loggers                                                  */
/*                                                                      */
/*  Opens every per-module log file and wires the resulting logger_t   */
/*  pointers into ctx.  Called once from main() before any OCI work.  */
/*                                                                      */
/*  Returns 0 on success, -1 if any logger fails to open.             */
/*                                                                      */
/*  IMPORTANT - Connection pool / worker context note                  */
/*  -------------------------------------------------------            */
/*  When the connection pool is in use, each XML file is processed     */
/*  through a worker_ctx created with memset() zero.                   */
/*  OCI_Pool_get_session() copies only the OCI handles (envhp, errhp, */
/*  svchp etc.) into worker_ctx - it does NOT copy logger pointers     */
/*  or any other application-level fields.  Those fields remain NULL   */
/*  after the memset, causing all module logging to be silently lost.  */
/*                                                                      */
/*  The fix is in the pool dispatch block in main() - immediately      */
/*  after OCI_Pool_get_session() succeeds, every logger pointer and    */
/*  ctx.ini / ctx.resultset_cache must be explicitly copied from the   */
/*  parent ctx into worker_ctx.  Any new logger added here must also   */
/*  be added to that copy block or it will not write to its log file   */
/*  when running in pool mode.                                         */
/* ================================================================== */
static int initialise_loggers(oci_context_t *ctx,
                               logger_t      *logger,
                               logger_t      *select_logger,
                               logger_t      *cache_logger,
                               logger_t      *Metadata_logger,
                               logger_t      *connection_logger,
                               logger_t      *connectionpool_logger,
                               logger_t      *insert_logger,
                               logger_t      *update_logger,
                               logger_t      *delete_logger,
							  logger_t      *dml_logger,
							  logger_t      *ddl_logger,
							   logger_t      *procedure_logger,
							   logger_t		 *error_logger,
							   logger_t		 *metrics_logger,
							   logger_t		 *transaction_logger,
							   logger_t		 *security_logger,
							   logger_t		 *crypt_logger,
							   logger_t		 *audit_logger,
							   logger_t      *session_logger,
							   logger_t      *sql_parser_logger,
							   logger_t      *file_consumer_logger,
							   logger_t      *dispatcher_logger,
							   logger_t      *worker_logger)
{

    /* ---- error logger MUST BE initialized first---- */
    printf("Initialize error logger name =%sx\n",
           ctx->ini->error_log_file_name);
    if (logger_init_str(error_logger,
                        ctx->ini->error_log_file_name,
                        ctx->ini->error_log_file_max_size,
                        ctx->ini->error_log_file_rotation_number,
                        ctx->ini->error_log_level) != 0)
    {
        printf("Failed to initialise error logger: %s\n",
               ctx->ini->error_log_file_name);
        return -1;
    }
    ctx->error_logger = error_logger;
    printf("Initialize error logger name =%s complete successful.\n",
           ctx->ini->error_log_file_name);


    /* ---- Main logger ---- */
    printf("Initialize Main logger name =%sx\n",
           ctx->ini->log_file_name);
    if (logger_init_str2(logger,
                        ctx->ini->log_file_name,
                        ctx->ini->log_file_max_size,
                        ctx->ini->log_file_rotation_number,
                        ctx->ini->log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise main logger: %s\n",
               ctx->ini->log_file_name);
        return -1;
    }
    /* 2026-08-01 fix: this used to read
     * "ctx->connectionpool_logger = logger;" - a copy-paste slip that
     * meant ctx->logger itself was NEVER actually assigned, ever. It
     * stayed invisible because ctx->connectionpool_logger gets its own,
     * correct, separate assignment further down (from its own
     * dedicated connectionpool_logger variable), which simply
     * overwrote this mistake before anything could observe it being
     * wrong - but ctx->logger had no such second chance. Found via
     * UT-LOG-001, the very first time anything actually checked
     * ctx->logger directly rather than always using the standalone
     * local `logger` variable (which was never affected by this bug -
     * only the ctx-> field was).                                      */
    ctx->logger = logger;
    printf("Initialize Main logger name =%s complete successful.\n",
           ctx->ini->log_file_name);

    /* ---- Select logger ---- */
    printf("Initialize Select logger name =%sx\n",
           ctx->ini->select_log_file_name);
    if (logger_init_str2(select_logger,
                        ctx->ini->select_log_file_name,
                        ctx->ini->select_log_file_max_size,
                        ctx->ini->select_log_file_rotation_number,
                        ctx->ini->select_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise select logger: %s\n",
               ctx->ini->select_log_file_name);
        return -1;
    }
    ctx->select_logger = select_logger;
    printf("Initialize select logger name =%s complete successful.\n",
           ctx->ini->select_log_file_name);

    /* ---- File Consumer logger (File_Consumer_proposal v1.2, Stage 2) ---- */
    printf("Initialize file_consumer logger name =%sx\n",
           ctx->ini->file_consumer_log_file_name);
    if (logger_init_str2(file_consumer_logger,
                        ctx->ini->file_consumer_log_file_name,
                        ctx->ini->file_consumer_log_file_max_size,
                        ctx->ini->file_consumer_log_file_rotation_number,
                        ctx->ini->file_consumer_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise file_consumer logger: %s\n",
               ctx->ini->file_consumer_log_file_name);
        return -1;
    }
    ctx->file_consumer_logger = file_consumer_logger;
    printf("Initialize file_consumer logger name =%s complete successful.\n",
           ctx->ini->file_consumer_log_file_name);

    /* ---- Dispatcher logger (File_Consumer_proposal v1.2, Stage 1) ---- */
    printf("Initialize dispatcher logger name =%sx\n",
           ctx->ini->dispatcher_log_file_name);
    if (logger_init_str2(dispatcher_logger,
                        ctx->ini->dispatcher_log_file_name,
                        ctx->ini->dispatcher_log_file_max_size,
                        ctx->ini->dispatcher_log_file_rotation_number,
                        ctx->ini->dispatcher_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise dispatcher logger: %s\n",
               ctx->ini->dispatcher_log_file_name);
        return -1;
    }
    ctx->dispatcher_logger = dispatcher_logger;
    printf("Initialize dispatcher logger name =%s complete successful.\n",
           ctx->ini->dispatcher_log_file_name);

    /* ---- Worker logger (File_Consumer_proposal v1.2, Stage 4) ---- */
    printf("Initialize worker logger name =%sx\n",
           ctx->ini->worker_log_file_name);
    if (logger_init_str2(worker_logger,
                        ctx->ini->worker_log_file_name,
                        ctx->ini->worker_log_file_max_size,
                        ctx->ini->worker_log_file_rotation_number,
                        ctx->ini->worker_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise worker logger: %s\n",
               ctx->ini->worker_log_file_name);
        return -1;
    }
    ctx->worker_logger = worker_logger;
    printf("Initialize worker logger name =%s complete successful.\n",
           ctx->ini->worker_log_file_name);

    /* ---- Cache logger ---- */
    printf("Initialize cache logger name =%sx\n",
           ctx->ini->cache_log_file_name);
    if (logger_init_str2(cache_logger,
                        ctx->ini->cache_log_file_name,
                        ctx->ini->cache_log_file_max_size,
                        ctx->ini->cache_log_file_rotation_number,
                        ctx->ini->cache_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise cache logger: %s\n",
               ctx->ini->cache_log_file_name);
        return -1;
    }
    ctx->cache_logger = cache_logger;
    printf("Initialize cache logger name =%s complete successful.\n",
           ctx->ini->cache_log_file_name);

    /* ---- Metadata logger ---- */
    printf("Initialize Metadata logger name =%sx\n",
           ctx->ini->Metadata_log_file_name);
    if (logger_init_str2(Metadata_logger,
                        ctx->ini->Metadata_log_file_name,
                        ctx->ini->Metadata_log_file_max_size,
                        ctx->ini->Metadata_log_file_rotation_number,
                        ctx->ini->Metadata_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise Metadata logger: %s\n",
               ctx->ini->Metadata_log_file_name);
        return -1;
    }
    ctx->Metadata_logger = Metadata_logger;
    printf("Initialize Metadata logger name =%s complete successful.\n",
           ctx->ini->Metadata_log_file_name);


    /* ---- Connection pool logger ---- */
    printf("Initialize connectionpool logger name =%sx\n",
           ctx->ini->connectionpool_log_file_name);
    if (logger_init_str2(connectionpool_logger,
                        ctx->ini->connectionpool_log_file_name,
                        ctx->ini->connectionpool_log_file_max_size,
                        ctx->ini->connectionpool_log_file_rotation_number,
                        ctx->ini->connectionpool_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise connectionpool logger: %s\n",
               ctx->ini->connectionpool_log_file_name);
        return -1;
    }
    ctx->connectionpool_logger = connectionpool_logger;
    printf("Initialize connectionpool logger name =%s complete successful.\n",
           ctx->ini->connectionpool_log_file_name);

    /* ---- Insert logger ---- */
    printf("Initialize insert logger name =%sx\n",
           ctx->ini->insert_log_file_name);
    if (logger_init_str2(insert_logger,
                        ctx->ini->insert_log_file_name,
                        ctx->ini->insert_log_file_max_size,
                        ctx->ini->insert_log_file_rotation_number,
                        ctx->ini->insert_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise insert logger: %s\n",
               ctx->ini->insert_log_file_name);
        return -1;
    }
    ctx->insert_logger = insert_logger;
    printf("Initialize insert logger name =%s complete successful.\n",
           ctx->ini->insert_log_file_name);

    /* ---- Update logger ---- */
    printf("Initialize update logger name =%sx\n",
           ctx->ini->update_log_file_name);
    if (logger_init_str2(update_logger,
                        ctx->ini->update_log_file_name,
                        ctx->ini->update_log_file_max_size,
                        ctx->ini->update_log_file_rotation_number,
                        ctx->ini->update_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise update logger: %s\n",
               ctx->ini->update_log_file_name);
        return -1;
    }
    ctx->update_logger = update_logger;
    printf("Initialize update logger name =%s complete successful.\n",
           ctx->ini->update_log_file_name);


    /* ---- Delete logger ---- */
    printf("Initialize delete logger name =%sx\n",
           ctx->ini->delete_log_file_name);
    if (logger_init_str2(delete_logger,
                        ctx->ini->delete_log_file_name,
                        ctx->ini->delete_log_file_max_size,
                        ctx->ini->delete_log_file_rotation_number,
                        ctx->ini->delete_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise delete logger: %s\n",
               ctx->ini->delete_log_file_name);
        return -1;
    }
    ctx->delete_logger = delete_logger;
    printf("Initialize delete logger name =%s complete successful.\n",
           ctx->ini->delete_log_file_name);

    /* ---- DDL logger ---- */
    printf("Initialize ddl logger name =%sx\n",
           ctx->ini->ddl_log_file_name);
    if (logger_init_str2(ddl_logger,
                        ctx->ini->ddl_log_file_name,
                        ctx->ini->ddl_log_file_max_size,
                        ctx->ini->ddl_log_file_rotation_number,
                        ctx->ini->ddl_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise ddl logger: %s\n",
               ctx->ini->ddl_log_file_name);
        return -1;
    }
    ctx->ddl_logger = ddl_logger;
    printf("Initialize ddl logger name =%s complete successful.\n",
           ctx->ini->ddl_log_file_name);

    /* ---- Procedure logger ---- */
    printf("Initialize procedure logger name =%sx\n",
           ctx->ini->procedure_log_file_name);
    if (logger_init_str2(procedure_logger,
                        ctx->ini->procedure_log_file_name,
                        ctx->ini->procedure_log_file_max_size,
                        ctx->ini->procedure_log_file_rotation_number,
                        ctx->ini->procedure_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise procedure logger: %s\n",
               ctx->ini->procedure_log_file_name);
        return -1;
    }
    ctx->procedure_logger = procedure_logger;
    printf("Initialize procedure logger name =%s complete successful.\n",
           ctx->ini->procedure_log_file_name);





    /* ---- metrics logger ---- */
    printf("Initialize metrics logger name =%sx\n",
           ctx->ini->metrics_log_file_name);
    if (logger_init_str2(metrics_logger,
                        ctx->ini->metrics_log_file_name,
                        ctx->ini->metrics_log_file_max_size,
                        ctx->ini->metrics_log_file_rotation_number,
                        ctx->ini->metrics_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise metrics logger: %s\n",
               ctx->ini->metrics_log_file_name);
        return -1;
    }
    ctx->metrics_logger = metrics_logger;
    printf("Initialize metrics logger name =%s complete successful.\n",
           ctx->ini->metrics_log_file_name);



    /* ---- transaction logger ---- */
    printf("Initialize transaction logger name =%sx\n",
           ctx->ini->transaction_log_file_name);
    if (logger_init_str2(transaction_logger,
                        ctx->ini->transaction_log_file_name,
                        ctx->ini->transaction_log_file_max_size,
                        ctx->ini->transaction_log_file_rotation_number,
                        ctx->ini->transaction_log_level,
						ctx->error_logger) != 0)
    {
        printf("Failed to initialise transaction logger: %s\n",
               ctx->ini->transaction_log_file_name);
        return -1;
    }
    ctx->transaction_logger = transaction_logger;
    printf("Initialize transaction logger name =%s complete successful.\n",
           ctx->ini->transaction_log_file_name);


    /* ---- security logger ---- */
     printf("Initialize security logger name =%sx\n",
            ctx->ini->security_log_file_name);
     if (logger_init_str2(security_logger,
                         ctx->ini->security_log_file_name,
                         ctx->ini->security_log_file_max_size,
                         ctx->ini->security_log_file_rotation_number,
                         ctx->ini->security_log_level,
						 ctx->error_logger) != 0)
     {
         printf("Failed to initialise security logger: %s\n",
                ctx->ini->security_log_file_name);
         return -1;
     }
     ctx->security_logger = security_logger;
     printf("Initialize security logger name =%s complete successful.\n",
            ctx->ini->security_log_file_name);





     /* ---- crypt logger ---- */
      printf("Initialize crypt logger name =%sx\n",
             ctx->ini->crypt_log_file_name);
      if (logger_init_str2(crypt_logger,
                          ctx->ini->crypt_log_file_name,
                          ctx->ini->crypt_log_file_max_size,
                          ctx->ini->crypt_log_file_rotation_number,
                          ctx->ini->crypt_log_level,
						  ctx->error_logger) != 0)
      {
          printf("Failed to initialise crypt logger: %s\n",
                 ctx->ini->crypt_log_file_name);
          return -1;
      }
      ctx->crypt_logger = crypt_logger;
      printf("Initialize crypt logger name =%s complete successful.\n",
             ctx->ini->crypt_log_file_name);





      /* ---- audit logger ---- */
       printf("Initialize audit logger name =%sx\n",
              ctx->ini->audit_log_file_name);
       if (logger_init_str2(audit_logger,
                           ctx->ini->audit_log_file_name,
                           ctx->ini->audit_log_file_max_size,
                           ctx->ini->audit_log_file_rotation_number,
                           ctx->ini->audit_log_level,
						   ctx->error_logger) != 0)
       {
           printf("Failed to initialise audit logger: %s\n",
                  ctx->ini->audit_log_file_name);
           return -1;
       }
       ctx->audit_logger = audit_logger;
       printf("Initialize audit logger name =%s complete successful.\n",
              ctx->ini->audit_log_file_name);




       /* ---- session logger ---- */
         printf("Initialize session logger name =%sx\n",
                ctx->ini->session_log_file_name);
         if (logger_init_str2(session_logger,
                             ctx->ini->session_log_file_name,
                             ctx->ini->session_log_file_max_size,
                             ctx->ini->session_log_file_rotation_number,
                             ctx->ini->session_log_level,
							 ctx->error_logger) != 0)
         {
             printf("Failed to initialise session logger: %s\n",
                    ctx->ini->session_log_file_name);
             return -1;
         }
         ctx->session_logger = session_logger;
         printf("Initialize session logger name =%s complete successful.\n",
                ctx->ini->session_log_file_name);



         /* ---- sql_parser logger ---- */
            if (logger_init_str2(sql_parser_logger,
                               ctx->ini->sql_parser_log_file_name,
                               ctx->ini->sql_parser_log_file_max_size,
                               ctx->ini->sql_parser_log_file_rotation_number,
                               ctx->ini->sql_parser_log_level,
							   ctx->error_logger) != 0)
           {
               printf("Failed to initialise session logger: %s\n",
                      ctx->ini->sql_parser_log_file_name);
               return -1;
           }
           ctx->sql_parser_logger = sql_parser_logger;
           printf("Initialize session logger name =%s ctx->ini->sql_parser_log_level=%s complete successful.\n",
                  ctx->ini->sql_parser_log_file_name, ctx->ini->sql_parser_log_level);




    return 0;
}

/* ================================================================== */
/*  main                                                                */
/* ================================================================== */
int main(int argc, char *argv[])
{
	/*flush all printf immediatly*/
	setvbuf(stdout, NULL, _IONBF, 0);
	print_limits();

    if (argc < 2)
    {
        printf("Usage: %s <config.ini> [--pool|--direct|1|0]\n",
               argv[0]);
        printf("\n");
        printf("  --pool   or  1  : use OCI connection pool\n");
        printf("  --direct or  0  : use direct OCI connection (default)\n");
        printf("\n");
        printf("  If the last argument is omitted the value of\n");
        printf("  use_connection_pool in config.ini is used.\n");
        return -1;
    }

    /* ---- Declare all logger instances on the stack ---- */
    app_config_t  config;
    oci_context_t ctx;
    logger_t      logger;
    logger_t      select_logger;
    logger_t      cache_logger;
    logger_t      Metadata_logger;
    logger_t      connection_logger;
    logger_t      connectionpool_logger;
    logger_t      insert_logger;
    logger_t      update_logger;
    logger_t      delete_logger;
    logger_t      dml_logger;
    logger_t      ddl_logger;
    logger_t      procedure_logger;
    logger_t	  error_logger;
    logger_t	  metrics_logger;
    logger_t	  transaction_logger;
    logger_t	  security_logger;
    logger_t	  crypt_logger;
    logger_t	  audit_logger;
    logger_t	  session_logger;
    logger_t	  sql_parser_logger;
    logger_t	  file_consumer_logger;
    logger_t	  dispatcher_logger;
    logger_t	  worker_logger;




    memset(&ctx,    0, sizeof(ctx));
    memset(&config, 0, sizeof(config));
    ctx.ini             = &config;
    ctx.pool_slot_index = -1;   /* not a pooled worker context */

    /* ---- Load ini ---- */
    if (load_ini(argv[1], &config, &ctx) != 0)
    {
        printf("Failed to load ini file: %s\n", argv[1]);
        return -1;
    }

    /* Set once, here, before any worker/File-Consumer thread exists -
     * see logger.h's own doc comment on why this is safe as a plain
     * global rather than __thread (write-once-then-read-only for the
     * rest of the process lifetime).                                  */
    logger_set_include_trace_context(config.log_include_trace_context);

    /* ---- Load consumer-type-specific ini (File_Consumer_proposal v1.2).
     * config.consumer_ini_path is already populated by load_ini() above
     * (it's a plain config.ini key) - just pass it straight through.
     * ---- */
    if (load_consumer_ini(config.consumer_ini_path, &config) != 0)
    {
        printf("Failed to load consumer ini file: %s\n", config.consumer_ini_path);
        return -1;
    }

    /* Record the real config.ini path for UT-INI-002's own re-run of
     * load_ini() against this same known-good file - see
     * unit_test_set_ini_path()'s own doc comment in
     * OCI_Unit_Test_Module.h.                                          */
    unit_test_set_ini_path(argv[1]);

    /* ---- Initialise all loggers via helper ---- */
    if (initialise_loggers(&ctx,
                            &logger,
                            &select_logger,
                            &cache_logger,
                            &Metadata_logger,
                            &connection_logger,
                            &connectionpool_logger,
                            &insert_logger,
                            &update_logger,
                            &delete_logger,
                            &dml_logger,
                            &ddl_logger,
                            &procedure_logger,
							&error_logger,
							&metrics_logger,
							&transaction_logger,
							&security_logger,
							&crypt_logger,
							&audit_logger,
							&session_logger,
							&sql_parser_logger,
							&file_consumer_logger,
							&dispatcher_logger,
							&worker_logger
    				) != 0)
    {
        printf("Failed to initialise loggers - exiting\n");
        return -1;
    }

    ctx.NLS_DATE_FORMAT = "YYYY-MM-DD HH24:MI:SS";

    logger_dump_ctx(&ctx);

    /* ---- Initialise resultset cache ---- */
    ctx.resultset_cache = resultset_cache_init(ctx.ini, ctx.cache_logger);
    /* NULL return means disabled - all downstream code handles NULL safely */


    /* ---- Initialise metadata cache ---- */
    ctx.metadata_cache = metadata_cache_init(ctx.ini,
                                                    ctx.Metadata_logger);
     // NULL return means disabled - downstream code handles NULL safely


    /* ---- Initialise session cache ---- */
    ctx.session_cache = session_cache_init(ctx.ini, ctx.session_logger);
    if (ctx.session_cache)
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "session_cache initialised");
    else
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "session_cache disabled or failed to initialise");



    /* ---- Determine connection mode ---- */
    /*
     * Priority:
     *   1. Command-line argument (argv[2]) if supplied and recognised
     *   2. use_connection_pool from config.ini
     */
    int use_pool = ctx.ini->use_connection_pool;

    if (argc >= 3)
    {
        int arg_pool = parse_pool_arg(argv[2]);
        if (arg_pool >= 0)
        {
            use_pool = arg_pool;
            logger_write(&logger, LOG_INFO, __func__, 0,
                         "Connection mode overridden by command-line "
                         "argument '%s': %s",
                         argv[2],
                         use_pool ? "CONNECTION POOL" : "DIRECT");
        }
        else
        {
            logger_write(&logger, LOG_WARN, __func__, 0,
                         "Unrecognised argument '%s' - ignoring, "
                         "using ini value use_connection_pool=%d",
                         argv[2], use_pool);
        }
    }





    logger_write(&logger, LOG_INFO, __func__, 0,
                 "================================================");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "XML Test Runner");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  config.ini       : %s", argv[1]);
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  connection mode  : %s",
                 use_pool ? "CONNECTION POOL" : "DIRECT");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  input directory  : %s", ctx.ini->xml_input_dir);
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "================================================\n");

    /* ---- Connect ---- */
    if (use_pool)
    {
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Calling OCI_Connect_pool");
        if (OCI_Connect_pool(&ctx) != 0)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "OCI_Connect_pool failed");
            logger_close(&logger);
            return -1;
        }
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "OCI_Connect_pool OK");
    }
    else
    {
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Calling OCI_Connect");
        if (OCI_Connect(&ctx) != 0)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "OCI_Connect failed");
            logger_close(&logger);
            return -1;
        }
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "OCI_Connect OK");
    }

    /* ---- Startup self-test ----
     * ctx is fully set up at this point (loggers + connection) - see
     * OCI_Unit_Test_Module.h's own top comment for why every test is
     * called with ctx in this state rather than splitting invocation
     * by tier. unit_test.ini is looked for alongside config.ini (same
     * directory as argv[1]) - if it isn't there, unit_test_load_config()
     * itself returns safe, disabled-by-default settings (2026-08-01
     * backward-compatibility decision), so this whole block is a no-op
     * for any deployment that predates this feature.                   */
    {
        char ut_ini_path[600];
        const char *last_slash = strrchr(argv[1], '/');
        if (last_slash)
        {
            size_t dir_len = (size_t)(last_slash - argv[1]) + 1;
            if (dir_len > sizeof(ut_ini_path) - 32) dir_len = sizeof(ut_ini_path) - 32;
            memcpy(ut_ini_path, argv[1], dir_len);
            snprintf(ut_ini_path + dir_len, sizeof(ut_ini_path) - dir_len, "unit_test.ini");
        }
        else
        {
            snprintf(ut_ini_path, sizeof(ut_ini_path), "unit_test.ini");
        }

        unit_test_config_t ut_cfg;
        unit_test_load_config(ut_ini_path, &logger, &ut_cfg);
        unit_test_set_tier3_objects(ut_cfg.test_table_name, ut_cfg.test_table_owner,
                                     ut_cfg.test_procedure_name);

        if (ut_cfg.startup_self_test_enabled)
        {
            /* Dedicated log file for the self-test's own results - only
             * created when the self-test is actually enabled, matching
             * the same backward-compatibility principle as everything
             * else in this feature (nothing new happens for a
             * deployment that doesn't use it). Uses ctx.error_logger,
             * matching the same convention as every other logger_init_
             * str2() call in this function.                            */
            logger_t unit_test_logger;
            memset(&unit_test_logger, 0, sizeof(unit_test_logger));
            int ut_logger_ok = (logger_init_str2(&unit_test_logger,
                                 ut_cfg.unit_test_log_file_name,
                                 ut_cfg.unit_test_log_file_max_size,
                                 ut_cfg.unit_test_log_file_rotation_number,
                                 ut_cfg.unit_test_log_level,
                                 ctx.error_logger) == 0);
            logger_t *summary_logger = ut_logger_ok ? &unit_test_logger : &logger;

            if (!ut_logger_ok)
                logger_write(&logger, LOG_WARN, __func__, 0,
                             "Could not initialise the dedicated unit test "
                             "logger ('%s') - falling back to the main log",
                             ut_cfg.unit_test_log_file_name);

            logger_write(summary_logger, LOG_INFO, __func__, 0,
                         "Startup self-test enabled (max_tier=%d) - running now",
                         ut_cfg.startup_max_tier);

            unit_test_result_t *ut_results = NULL;
            int ut_result_count = 0;
            unit_test_run_all(&ctx, ut_cfg.startup_max_tier, &ut_results, &ut_result_count);

            if (ut_cfg.unit_test_log_summary_enabled)
                unit_test_write_summary(summary_logger, ut_results, ut_result_count);

            if (ut_logger_ok) logger_close(&unit_test_logger);

            /* Halt decision is per-tier, per unit_test.ini - see this
             * file's own doc comment in OCI_Unit_Test_Module.h. A test
             * above startup_max_tier never even ran, so it can't be part
             * of this decision either way.                             */
            int tier1_failed = 0, tier2_failed = 0, tier3_failed = 0;
            for (int i = 0; i < ut_result_count; i++)
            {
                if (strcmp(ut_results[i].status, "FAIL") != 0) continue;
                if (ut_results[i].tier == 1) tier1_failed = 1;
                else if (ut_results[i].tier == 2) tier2_failed = 1;
                else if (ut_results[i].tier == 3) tier3_failed = 1;
            }

            int should_halt =
                (tier1_failed && ut_cfg.startup_halt_on_tier1_fail) ||
                (tier2_failed && ut_cfg.startup_halt_on_tier2_fail) ||
                (tier3_failed && ut_cfg.startup_halt_on_tier3_fail);

            unit_test_free_results(ut_results);

            if (should_halt)
            {
                logger_write(&logger, LOG_ERROR, __func__, 0,
                             "Startup self-test failure(s) triggered a halt "
                             "per unit_test.ini - exiting before the request "
                             "consumer loop starts. See the Unit Test Summary "
                             "above for detail.");
                logger_close(&logger);
                return -1;
            }
        }
    }


    const char *input_dir = ctx.ini->xml_input_dir;

    DIR *dir = opendir(input_dir);
    if (!dir)
    {
        logger_write(&logger, LOG_ERROR, __func__, 0,
                     "Failed to open input_xml dir: %s", input_dir);
        use_pool ? OCI_Disconnect_pool(&ctx) : OCI_Disconnect(&ctx);
        logger_close(&logger);
        return -1;
    }

    int total   = 0;
    int passed  = 0;
    int skipped = 0;
    int failed_ops = 0;

    struct dirent *entry;

    /* ================================================================
     *  Pin a single physical OCI session for the lifetime of the
     *  transaction.
     *
     *  Oracle transactions are bound to one session.  In pooled mode,
     *  OCI_Pool_get_session() normally hands out a fresh borrowed
     *  session per call - correct for independent, single-call units
     *  of work, but wrong once multiple DML calls must be wrapped in
     *  one logical transaction via tx_begin()/tx_commit().  If each
     *  file in the loop below borrowed its own session, each file's
     *  work would land on a different physical connection; the per-
     *  file commit is correctly skipped (active_tx is set) but that
     *  uncommitted work is then rolled back the moment the session is
     *  returned to the pool, leaving nothing for the later tx_commit()
     *  on the pool's master context to actually commit.
     *
     *  tx_ctx is the single context - master ctx in direct mode, or
     *  one pinned worker_ctx in pooled mode - that owns the physical
     *  session the transaction runs on.  Every file in the loop uses
     *  tx_ctx, not a fresh per-file borrow, while the transaction is
     *  open.  The session is only returned to the pool after the
     *  transaction has been committed or rolled back.
     * ================================================================ */
    oci_context_t  worker_ctx;
    oci_context_t *tx_ctx = &ctx;   /* default: direct mode shares ctx */

    memset(&worker_ctx, 0, sizeof(worker_ctx));


    if (use_pool)
    {
        if (OCI_Pool_get_session(&ctx, &worker_ctx) != 0)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "OCI_Pool_get_session failed - cannot start "
                         "transaction-scoped session");
            closedir(dir);
            OCI_Disconnect_pool(&ctx);
            logger_close(&logger);
            return -1;
        }

        /*
         * ---- Copy all shared pointers into the pinned worker context ----
         *
         * OCI_Pool_get_session() copies only the OCI handles (envhp,
         * errhp, svchp etc.) into worker_ctx.  All logger pointers and
         * application-level fields remain NULL after the memset above
         * and must be explicitly copied here before any module code
         * runs.  This copy now happens ONCE for the whole transaction
         * instead of once per file.
         *
         * Any new logger added to oci_context_t and initialised in
         * initialise_loggers() MUST also be added to this block,
         * otherwise it will silently not write in pool mode.
         */
        worker_ctx.logger                = ctx.logger;
        worker_ctx.select_logger         = ctx.select_logger;
        worker_ctx.cache_logger          = ctx.cache_logger;
        worker_ctx.Metadata_logger       = ctx.Metadata_logger;
        worker_ctx.connection_logger     = ctx.connection_logger;
        worker_ctx.connectionpool_logger = ctx.connectionpool_logger;
        worker_ctx.insert_logger         = ctx.insert_logger;
        worker_ctx.update_logger         = ctx.update_logger;
        worker_ctx.delete_logger         = ctx.delete_logger;
        worker_ctx.dml_logger            = ctx.dml_logger;
        worker_ctx.ddl_logger            = ctx.ddl_logger;
        worker_ctx.procedure_logger      = ctx.procedure_logger;
        worker_ctx.ini                   = ctx.ini;
        worker_ctx.resultset_cache       = ctx.resultset_cache;
        worker_ctx.error_logger          = ctx.error_logger;
        worker_ctx.metrics_logger        = ctx.metrics_logger;
        worker_ctx.transaction_logger    = ctx.transaction_logger;
        worker_ctx.security_logger       = ctx.security_logger;
        worker_ctx.crypt_logger          = ctx.crypt_logger;
        worker_ctx.audit_logger          = ctx.audit_logger;
        worker_ctx.session_logger        = ctx.session_logger;
        worker_ctx.sql_parser_logger     = ctx.sql_parser_logger;
        worker_ctx.file_consumer_logger  = ctx.file_consumer_logger;
        worker_ctx.dispatcher_logger     = ctx.dispatcher_logger;
        worker_ctx.worker_logger         = ctx.worker_logger;
        worker_ctx.metadata_cache        = ctx.metadata_cache;
        worker_ctx.session_cache        = ctx.session_cache;

        tx_ctx = &worker_ctx;
        tx_ctx->active_tx = NULL;

        //TL 09-JUL Startup reconciliation - call once, immediately after OCI_Connect() /
        //  OCI_Connect_pool() succeeds and BEFORE the CREATE_SESSION call in 5d:

        /*Lets reconcile any orphaned sessions*/
		int orphan_count = 0;
		if (session_reconcile_orphans(tx_ctx, &orphan_count) != SESSION_OK){
			logger_write(&logger, LOG_WARN, __func__, 0,
						 "session_reconcile_orphans reported a failure - "
						 "see session_Data_Manager.log");
		} else {
			logger_write(&logger, LOG_INFO, __func__, 0,
						 "session_reconcile_orphans closed %d orphaned "
						 "session(s)", orphan_count);
		}



		/*10-JUL Create a stubbed session*/
        char session_id_from_client[SESSION_UUID_LEN] = "Stubbed 9-JUL";
        if (dispatch_create_session(tx_ctx,
                "/home/leyden100/eclipse-workspace/OCI_Wrapper/OCI_Tester/OCI_Test_Create_Session.xml"   /* This will eventually be replace with xml envelope*/,
                session_id_from_client, sizeof(session_id_from_client)) != 0)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "Failed to establish session - falling back to "
                         "placeholder session id for this run");
            strncpy(session_id_from_client, "-", sizeof(session_id_from_client) - 1);
        }




        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Pinned pool session slot=%d for the duration of "
                     "the transaction", worker_ctx.pool_slot_index);
    }

    /* ================================================================
     * consumer_type=FILE (File_Consumer_proposal v1.2)
     *
     * Stage 5 update: real worker threads. main() now owns the
     * queue_manager and a long-running worker pool - created once,
     * kept alive across every scan pass, shut down only at the end of
     * this run. Each worker thread borrows its own session from the
     * pool independently, so this path requires connection pool mode;
     * direct mode's single shared connection can't support that.
     *
     * Doesn't continue into the fixture-directory test harness below:
     * that harness wraps every file in one shared tx_begin/tx_commit
     * spanning the whole batch, which is a completely different
     * session/transaction model than the File Consumer's "one request,
     * one atomic session, then the session goes back to the pool"
     * design (Payload Ownership addendum / Session Model decision).
     * The two are not meant to run in the same pass.
     * ================================================================ */
    if (strcasecmp(config.consumer_type, "FILE") == 0)
    {
        if (!use_pool)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "consumer_type=FILE requires connection pool mode "
                         "(use_connection_pool=1 in config.ini) - each "
                         "worker thread needs its own independently-"
                         "borrowed session, which direct mode's single "
                         "shared connection can't provide. Refusing to "
                         "start.");
            OCI_Disconnect(&ctx);
            logger_close(&logger);
            return -1;
        }

        /* The session pinned into tx_ctx above was only needed for
         * session_reconcile_orphans()/dispatch_create_session() during
         * startup - the File Consumer path itself never touches the
         * database directly (file_consumer.c only reads files off
         * disk and enqueues RequestObjects; ctx there is used purely
         * for its logger pointers), and every worker thread below
         * borrows its own independent session anyway. Release this one
         * now rather than holding it uselessly for the whole run.      */
        OCI_Pool_release_session(&ctx, tx_ctx);
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Released the startup-only pinned session - each "
                     "worker thread borrows its own below");

        logger_write(&logger, LOG_INFO, __func__, 0,
                     "consumer_type=FILE - starting worker pool (%d "
                     "thread(s), one per queue)", config.dispatcher_queue_count);

        queue_manager_t *qm = queue_manager_create(config.dispatcher_queue_count,
                                                    config.dispatcher_queue_depth);
        if (!qm)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "queue_manager_create() failed - check "
                         "dispatcher_queue_count/dispatcher_queue_depth in "
                         "consumer_file.ini (both must be > 0)");
            OCI_Disconnect_pool(&ctx);
            logger_close(&logger);
            return -1;
        }

        /* Session Manager proposal, Stage 2 (2026-08-06) - own thread,
         * own queue, started alongside the worker pool and File
         * Consumer.                                                    */
        session_touch_queue_t *touch_q = session_touch_queue_create(SESSION_TOUCH_QUEUE_DEPTH);
        if (!touch_q)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "session_touch_queue_create() failed - continuing "
                         "without session activity tracking (workers will "
                         "log a WARN per dropped touch, see worker.c)");
        }

        session_manager_runner_t *sm_runner = NULL;
        if (touch_q)
        {
            sm_runner = session_manager_runner_start(&ctx, touch_q);
            if (!sm_runner)
                logger_write(&logger, LOG_ERROR, __func__, 0,
                             "session_manager_runner_start() failed - "
                             "continuing without session activity "
                             "tracking (touch queue will just fill and "
                             "workers will log dropped touches)");
        }

        worker_pool_t *pool = worker_pool_start(&ctx, qm, touch_q, config.dispatcher_queue_count);
        if (!pool)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "worker_pool_start() failed - see worker log above "
                         "for detail. Refusing to run with no workers.");
            if (sm_runner) session_manager_runner_stop_and_join(sm_runner, touch_q);
            if (touch_q) session_touch_queue_destroy(touch_q);
            queue_manager_destroy(qm);
            OCI_Disconnect_pool(&ctx);
            logger_close(&logger);
            return -1;
        }

        /* File Consumer now gets its own dedicated thread too, rather
         * than main() running its scan-sleep-scan loop directly - see
         * file_consumer_runner.h. This is the fix for the main()
         * inconsistency Terry flagged (worker pool had proper threads,
         * File Consumer didn't) - main()'s job here shrinks down to
         * pure orchestration: start both, wait, shut both down.        */
        file_consumer_runner_t *fc_runner = file_consumer_runner_start(&ctx, &config, qm);
        if (!fc_runner)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "file_consumer_runner_start() failed - shutting "
                         "down the worker pool that already started, "
                         "nothing will ever be enqueued for it to drain");
            worker_pool_shutdown_and_join(pool, qm);
            queue_manager_destroy(qm);
            OCI_Disconnect_pool(&ctx);
            logger_close(&logger);
            return -1;
        }

        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Worker pool and File Consumer thread both started - "
                     "waiting for File Consumer to stop (lifetime=%s)",
                     config.dispatcher_lifetime_seconds > 0
                     ? "bounded, see file_consumer log for the exact value"
                     : "forever - this run will not return on its own");

        /* Blocks until the File Consumer thread stops on its own -
         * i.e. once dispatcher.lifetime_seconds have elapsed. With
         * lifetime_seconds=0 this call - and therefore this whole
         * process - blocks here indefinitely, matching "0 = run
         * forever" as a real long-running service rather than a
         * bounded test pass (no signal handling is wired up yet, so
         * today that means an actual process kill to stop it, not a
         * graceful in-process shutdown). Set
         * dispatcher.lifetime_seconds > 0 in consumer_file.ini for a
         * bounded test run instead.
         *
         * Deliberately file_consumer_runner_join() here, NOT
         * stop_and_join() - stop_and_join() actively requests the
         * thread stop immediately, which is exactly the bug behind the
         * "exits after 0 scan passes" report on 2026-08-05: calling it
         * unconditionally right after start() told the thread to stop
         * before it ever ran a single pass.                           */
        file_consumer_runner_join(fc_runner);

        logger_write(&logger, LOG_INFO, __func__, 0,
                     "File Consumer thread stopped - waiting for the "
                     "worker pool to finish draining whatever's still "
                     "queued before shutdown...");

        /* Poll for visibility/logging only - worker_pool_shutdown_and_join()
         * below still fully drains every queue regardless of how long
         * that takes, this loop just gives an early WARN if it's
         * taking a while so it's visible in the log rather than a
         * silent long wait.                                            */
        int waited_ms = 0;
        while (queue_manager_total_count(qm) > 0 &&
               waited_ms < FILE_CONSUMER_DRAIN_POLL_WARN_SECONDS * 1000)
        {
            struct timespec drain_wait_ts = {0, 200000000L};   /* 200ms */
            nanosleep(&drain_wait_ts, NULL);
            waited_ms += 200;
        }

        int remaining_at_poll_warn = queue_manager_total_count(qm);
        if (remaining_at_poll_warn > 0)
            logger_write(&logger, LOG_WARN, __func__, 0,
                         "Still %d item(s) queued after %d second(s) of "
                         "waiting - this is just a visibility WARN, NOT "
                         "abandoning that work: worker_pool_shutdown_and_join() "
                         "below still blocks until every queued item is "
                         "genuinely processed by its worker, however long "
                         "that takes.",
                         remaining_at_poll_warn, FILE_CONSUMER_DRAIN_POLL_WARN_SECONDS);
        else
            logger_write(&logger, LOG_INFO, __func__, 0,
                         "All queued items drained - shutting down worker "
                         "pool");

        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Calling worker_pool_shutdown_and_join() - this call "
                     "blocks until every worker has fully drained its own "
                     "queue and exited, regardless of how long that takes");

        worker_pool_shutdown_and_join(pool, qm);

        logger_write(&logger, LOG_INFO, __func__, 0,
                     "worker_pool_shutdown_and_join() returned - all workers "
                     "exited, all queues should now be empty");

        /* Session Manager stops AFTER the worker pool, not before or
         * concurrently - workers are the only producers onto touch_q,
         * so stopping them first guarantees no more touches will ever
         * be enqueued, and session_manager_runner_stop_and_join() then
         * fully drains whatever's already queued before the thread
         * exits (same "signal shutdown, don't abandon queued work"
         * guarantee as worker_pool_shutdown_and_join() itself).        */
        if (sm_runner)
        {
            logger_write(&logger, LOG_INFO, __func__, 0,
                         "Stopping Session Manager thread - draining "
                         "remaining touch queue");
            session_manager_runner_stop_and_join(sm_runner, touch_q);
        }
        if (touch_q) session_touch_queue_destroy(touch_q);

        queue_manager_destroy(qm);

        OCI_Disconnect_pool(&ctx);
        logger_close(&logger);
        return 0;
    }

    /*TL June 13 : For a test,  lets encase every test in 1 transaction, using transaction manager*/
    /* tx_init/tx_begin run against tx_ctx so the transaction is bound
     * to the single physical session that will execute every file in
     * the loop below - not the pool's master context.                */
    tx_handle_t tx;
    tx_init(&tx, tx_ctx);
    char *session_id_from_client = "Session_id_from_client_stub";

  char *begin_xml = NULL;
  logger_write(&logger, LOG_INFO, __func__, 0,
               "Begining Transaction");
   int rc = tx_begin(&tx, session_id_from_client, "Save Booking", &begin_xml);
   if (rc == TX_OK) {
	  logger_write(&logger, LOG_INFO, __func__, 0,
	               "Begining Transact succeded / Transaction id = %s", begin_xml);
	  free(begin_xml);
  }else{
	  logger_write(&logger, LOG_ERROR, __func__, 0,
	               "Begining Transact succeded / failed  = %s", begin_xml);
	  free(begin_xml);
  }

    /* tx_begin() sets tx_ctx->active_tx internally.  Mirror it onto the
     * master ctx as well so any code still inspecting ctx.active_tx
     * (e.g. end-of-run logging) observes the active transaction.      */
    ctx.active_tx = tx_ctx->active_tx;

    while ((entry = readdir(dir)) != NULL)
    {
        /* Only process .xml files */
        const char *name = entry->d_name;
        size_t      nlen = strlen(name);

        int is_xml  = (nlen >= 5 && strcasecmp(name + nlen - 4, ".xml")  == 0);
        int is_json = (nlen >= 6 && strcasecmp(name + nlen - 5, ".json") == 0);

        if (!is_xml && !is_json)

        if (nlen < 5 ||
            strcasecmp(name + nlen - 4, ".xml") != 0)
            continue;

        /* Build full path */
        char filepath[1024];
        snprintf(filepath, sizeof(filepath),
                 "%s/%s", input_dir, name);

        /* Skip directories */
        struct stat st;
        if (stat(filepath, &st) != 0 || S_ISDIR(st.st_mode))
            continue;

        total++;

        logger_write(&logger, LOG_INFO, __func__, 0,
                     "------------------------------------------------");
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Processing file %d: %s", total, name);

        /*
         * Every file - pool mode or direct mode - now runs on tx_ctx,
         * the single session pinned for the lifetime of this
         * transaction.  No per-file borrow/release: that is exactly
         * the pattern that silently dropped committed work before.
         *
         * Stage 3 note: process_xml_file() now takes a response_object_t
         * out-param. This harness only ever cared about pass/fail
         * counts, not response content, so the response is built and
         * immediately discarded here rather than wired to the Response
         * Manager - this loop's own tx_begin/tx_commit-per-batch model
         * doesn't match the File Consumer's one-request-one-session
         * design anyway (see the consumer_type=FILE branch above,
         * which is where Response Manager wiring actually lives).
         *
         * Stage 4 note: process_xml_file() no longer reads the file
         * itself (Payload Ownership addendum - only File Consumer does
         * that now, everything else works on payload). This harness
         * isn't the File Consumer, but it still needs to read the file
         * before calling process_xml_file() now that the function no
         * longer does it internally.                                   */
        long  harness_payload_len = 0;
        char *harness_payload = read_file(filepath, &harness_payload_len);
        if (!harness_payload)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "Failed to read file: %s", filepath);
            failed_ops++;
            continue;
        }

        response_object_t harness_resp;
        response_object_init(&harness_resp);
        /* NULL session_id_override - this legacy harness doesn't create
         * or hold a session (Session Manager proposal, Stage 1 - see
         * that plan's own Stage 3 note on this harness's fixtures still
         * carrying "-" and the compatibility decision still pending
         * for whenever validation itself goes live). Behaviour here is
         * unchanged from before this stage existed.                    */
        rc = process_xml_file(tx_ctx, harness_payload, harness_payload_len,
                               name, NULL, &harness_resp);
        response_object_free(&harness_resp);
        free(harness_payload);
        if (rc == 0)
            passed++;
        else
            failed_ops++;

        logger_write(&logger, LOG_INFO, __func__, 0,
                     "------------------------------------------------\n");
    }


  	/*Clean up transaction*/
	  logger_write(&logger, LOG_INFO, __func__, 0,
				   "At cleanup Transaction. passed=%d failed=%d",
				   passed, failed_ops);


	  char *tx_result_xml = NULL;
	  if (failed_ops == 0)
	  {
		  rc = tx_commit(&tx, &tx_result_xml);
		  logger_write(&logger, LOG_INFO, __func__, 0,
					   "TX_COMMIT %s: %s",
					   rc == TX_OK ? "OK" : "FAILED",
					   tx_result_xml ? tx_result_xml : "-");
	  }
	  else
	  {
		  logger_write(&logger, LOG_WARN, __func__, 0,
					   "%d operation(s) failed - rolling back", failed_ops);
		  rc = tx_rollback(&tx, &tx_result_xml);
	      printf("DEBUG: taking ROLLBACK path\n");   /* ADD */
		  logger_write(&logger, LOG_INFO, __func__, 0,
					   "TX_ROLLBACK %s: %s",
					   rc == TX_OK ? "OK" : "FAILED",
					   tx_result_xml ? tx_result_xml : "-");
	  }
	  if (tx_result_xml) free(tx_result_xml);

    closedir(dir);

    /* ---- Release the pinned pool session now that the transaction
     *      has been committed or rolled back ---- */
    if (use_pool)
    {
        OCI_Pool_release_session(&ctx, &worker_ctx);
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Released pinned pool session back to pool");
    }


    /* ---- Summary ---- */
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "================================================");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "Test Runner Complete");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  Connection mode  : %s",
                 use_pool ? "CONNECTION POOL" : "DIRECT");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  Files processed  : %d", total);
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  Passed           : %d", passed);
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  Failed           : %d", total - passed - skipped);
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "================================================");

    /* ---- Disconnect ---- */
    if (use_pool)
    {
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Calling OCI_Disconnect_pool");
        OCI_Disconnect_pool(&ctx);
    }
    else
    {
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Calling OCI_Disconnect");
        OCI_Disconnect(&ctx);
    }
    /*Free input and output xml*/
    if (ctx.INPUT_XML)
      {
          free(ctx.INPUT_XML);
          ctx.INPUT_XML = NULL;
      }

    if (ctx.OUTPUT_XML)
      {
          free(ctx.OUTPUT_XML);
          ctx.OUTPUT_XML = NULL;
      }



    /* ---- Shut down cache if running ---- */
    if (ctx.resultset_cache)
    {
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Calling resultset_cache_destroy");
        resultset_cache_destroy(ctx.resultset_cache);
    }
    else
    {
        logger_write(&logger, LOG_INFO, __func__, 0,
                     "Skip Calling resultset_cache_destroy - not running.");
    }



   if (ctx.metadata_cache)
   {
		logger_write(&logger, LOG_INFO, __func__, 0,
					 "Calling metadata_cache_destroy");
		metadata_cache_destroy(ctx.metadata_cache);
	}
	else
	{
		logger_write(&logger, LOG_INFO, __func__, 0,
					 "Skip metadata_cache_destroy - not running.");
	}


   if (ctx.session_cache)
   {
       logger_write(&logger, LOG_INFO, __func__, 0,
                    "Calling session_cache_destroy");
       session_cache_destroy(ctx.session_cache);
   }
   else
   {
       logger_write(&logger, LOG_INFO, __func__, 0,
                    "Skip session_cache_destroy - not running.");
   }

   logger_close(&logger);



    return (passed == total) ? 0 : -1;
}




void print_limits(void)
{
    struct rlimit rl;

    getrlimit(RLIMIT_STACK, &rl);
    printf("Stack soft = %ld bytes\n", (long)rl.rlim_cur);
    printf("Stack hard = %ld bytes\n", (long)rl.rlim_max);
}




/* looks_like_new_request_format() moved to OCI_Level1_Parser.c/.h as
 * level1_looks_like_new_format() (2026-08-01) - format detection is a
 * Level 1 concern, and keeping it private to this file made it
 * untestable by the new Unit Test module (OCI_Unit_Test_Module.c,
 * UT-L1-001) without duplicating the logic. See that function's own
 * doc comment in OCI_Level1_Parser.h for the full reasoning (unchanged
 * from when it lived here) - this file now just calls it. */


/* dispatch_select_new(), dispatch_insert_new(), dispatch_update_new(),
 * dispatch_delete_new(), and dispatch_procedure_new() moved to
 * dispatcher.c (Stage 1 extraction, File_Consumer_proposal v1.2). No
 * remaining call sites in this file - they were only ever called from
 * inside process_xml_file(), which moved with them. See dispatcher.c.
 */
