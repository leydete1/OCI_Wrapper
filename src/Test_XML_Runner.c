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
#include "OCI_Level2_Parser.h"
#include "OCI_Request_Response_Types.h"
#include "OCI_Execute_Query_Batch_Module.h"

static int looks_like_new_request_format(const char *xml, size_t len);
static int dispatch_select_new(oci_context_t *ctx, const char *filename,
                                input_c_request_t *request, input_c_operation_t *op);


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



/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define MAX_XML_FILE_SIZE  (4 * 1024 * 1024)   /* 4 MB per file        */
#define MAX_OPERATION_LEN  32


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



/* ------------------------------------------------------------------ */
/*  Helper: read an entire file into a heap buffer.                    */
/*  Caller must free() the returned pointer.                           */
/*  Returns NULL on error.                                             */
/* ------------------------------------------------------------------ */
static char *read_file(const char *path, long *out_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0 || sz > MAX_XML_FILE_SIZE)
    {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, fp);
    buf[n] = '\0';
    fclose(fp);

    if (out_len) *out_len = (long)n;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Helper: extract text between <tag> and </tag>.                     */
/*  Returns 1 on success, 0 if not found.                              */
/* ------------------------------------------------------------------ */
static int extract_tag(const char *src, const char *tag,
                        char *dest, size_t dest_max)
{
    char open[64], close[64];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    const char *s = strstr(src, open);
    if (!s) return 0;
    s += strlen(open);

    const char *e = strstr(s, close);
    if (!e) return 0;

    size_t len = (size_t)(e - s);
    if (len >= dest_max) len = dest_max - 1;
    memcpy(dest, s, len);
    dest[len] = '\0';

    /* Trim whitespace */
    char *p = dest;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != dest) memmove(dest, p, strlen(p) + 1);
    int l = (int)strlen(dest);
    while (l > 0 && isspace((unsigned char)dest[l-1]))
    { dest[l-1] = '\0'; l--; }

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Helper: uppercase in-place                                          */
/* ------------------------------------------------------------------ */
static void upper(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* ================================================================== */
/*  dispatch_insert                                                     */
/* ================================================================== */
static int dispatch_insert(oci_context_t *ctx,
                            const char    *filename,
                            const char    *xml)
{
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Dispatching INSERT: %s", filename);

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;   /* points to caller's string, no alloc needed */

    int rc = execute_insert_batch(ctx, xml, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "PASS [INSERT]: %s", filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
    }
    else
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "FAIL [INSERT]: %s (rc=%d)", filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    return rc;
}

/* ================================================================== */
/*  dispatch_select                                                     */
/* ================================================================== */
static int dispatch_select(oci_context_t *ctx,
                            const char    *filename,
                            const char    *xml)
{
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Dispatching SELECT: %s", filename);


    /*Comment out these lines to remove tester for the parser*/
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                  "***********Calling Test_sql_dependency_extractor ***********************");
    /*TL 21-June comment out this test as invalid sql has an issue with returing 0 when a previous phase of test fail*/
    /*test_sql_dependency_extractor(ctx);*/


    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                   "***********Finished Test_sql_dependency_extractor ***********************");

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                  "****** Calling get_table_metadata OCI_FIELD_TEST ******");

    table_metadata_alltabs_t *tm = get_table_metadata(ctx,
                                                "DATA_MANAGER",
                                                "OCI_FIELD_TEST");
     if (tm)
     {
         logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                      "get_table_metadata OK: owner='%s' table='%s' "
                      "status='%s' num_rows=%.0f compression='%s' "
                      "partitioned='%s' last_analyzed='%s'",
                      tm->owner,
                      tm->table_name,
                      tm->status,
                      tm->num_rows,
                      tm->compression,
                      tm->partitioned,
                      tm->last_analyzed);

         free_table_metadata(tm);
         tm = NULL;
     }
     else
     {
         logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                      "get_table_metadata FAILED for OCI_FIELD_TEST "
                      "- check Metadata_Data_Manager.log");
     }

     logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                  "****** Finished get_table_metadata ******");








    char sql_buf[8192] = {0};
    if (!extract_tag(xml, "sql", sql_buf, sizeof(sql_buf)))
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT]: %s - no <sql> element found",
                     filename);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.SQL                   = sql_buf;
    cfg.max_rows              = ctx->ini->query_max_record_count;
    cfg.fetch_array_size      = ctx->ini->query_fetch_batch_size;
    cfg.log_execution_results = 1;
    cfg.include_column_names  = 1;
    cfg.input_file_name = (char *)filename;   /* points to caller's string, no alloc needed */

    int rc = execute_query_batch(ctx, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "PASS [SELECT]: %s", filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
    }
    else
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT]: %s (rc=%d)", filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    return rc;
}


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


/* ================================================================== */
/*  dispatch_update                                                     */
/* ================================================================== */
static int dispatch_update(oci_context_t *ctx,
                            const char    *filename,
                            const char    *xml)
{
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Dispatching UPDATE: %s", filename);

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;   /* points to caller's string, no alloc needed */

    int rc = execute_update_batch(ctx, xml, &cfg);

    if (rc == 0)
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "PASS [UPDATE]: %s", filename);
    else
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "FAIL [UPDATE]: %s (rc=%d)", filename, rc);

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    return rc;
}

/* ================================================================== */
/*  dispatch_delete                                                     */
/* ================================================================== */
static int dispatch_delete(oci_context_t *ctx,
                            const char    *filename,
                            const char    *xml)
{
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Dispatching DELETE: %s", filename);

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;   /* points to caller's string, no alloc needed */

    int rc = execute_delete_batch(ctx, xml, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "PASS [DELETE]: %s", filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
    }
    else
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "FAIL [DELETE]: %s (rc=%d)", filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    return rc;
}

/* ================================================================== */
/*  dispatch_procedure                                                  */
/* ================================================================== */
static int dispatch_procedure(oci_context_t *ctx,
                               const char    *filename,
                               const char    *xml)
{
    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "Dispatching EXECUTE_PROCEDURE: %s", filename);

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)filename;   /* points to caller's string, no alloc needed */

    int rc = execute_procedure(ctx, xml, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "PASS [EXECUTE_PROCEDURE]: %s", filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
    }
    else
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "FAIL [EXECUTE_PROCEDURE]: %s (rc=%d)",
                     filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    return rc;
}

/* ================================================================== */
/*  process_xml_file                                                    */
/*  Read file, extract operation, dispatch to correct handler.         */
/* ================================================================== */
static int process_xml_file(oci_context_t *ctx,
                              const char    *filepath,
                              const char    *filename)
{
    long  len = 0;

    char *xml = read_file(filepath, &len);

    if (!xml)
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "Failed to read file: %s", filepath);
        return -1;
    }
    if (xml) {
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "Read file: %s.  Updating ctx->INPUT_XML", filepath);
        free(ctx->INPUT_XML);                  /* clear any previous value  */
        ctx->INPUT_XML = strdup(xml);          /* heap copy - ctx owns it   */
    }


    /* ---- Try the new format-agnostic pipeline first (SELECT only for
     * now, per 2026-07-19 decision) - the cheap pre-check means
     * level1_parse() is only ever called on files that already look
     * like new-format requests, so its own error logging stays
     * meaningful rather than firing on every old-format file. Old
     * dispatch below is untouched and stays the fallback for
     * everything else, both formats coexisting in xml_input_dir. */
    if (looks_like_new_request_format(xml, (size_t)len))
    {
        input_c_request_t   new_request;
        operation_status_t  level1_error;
        memset(&new_request, 0, sizeof(new_request));
        memset(&level1_error, 0, sizeof(level1_error));

        uint64_t level1_start = metrics_now_us();
        int level1_rc = level1_parse(ctx, xml, (size_t)len,
                                      &new_request, &level1_error);
        ctx->level1_parse_us = metrics_now_us() - level1_start;

        if (level1_rc != LEVEL1_OK)
        {
            logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                         "FAIL [Level1] %s error_code=%s error_text=%s",
                         filename, level1_error.error_code, level1_error.error_text);
            free(xml);
            return -1;
        }

        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "File='%s' matched new request format - audit_id=%s "
                     "operations=%d", filename, new_request.external_audit_id,
                     new_request.operation_count);

        uint64_t level2_start = metrics_now_us();
        int level2_rc = level2_validate(ctx, &new_request);
        ctx->level2_parse_us = metrics_now_us() - level2_start;
        int rc = 0;

        if (level2_rc == LEVEL2_OK)
        {
            for (int i = 0; i < new_request.operation_count; i++)
            {
                input_c_operation_t *op = &new_request.operations[i];

                if (op->type == OP_SELECT)
                {
                    rc = dispatch_select_new(ctx, filename, &new_request, op);
                }
                else
                {
                    /* Every other operation type still runs through the
                     * old dispatch below for now - not applicable here,
                     * since a new-format file's body doesn't match what
                     * the old extract_tag()-based dispatch expects
                     * anyway. Logged clearly rather than silently
                     * skipped, so it's obvious in the log why nothing
                     * happened for this operation.                     */
                    logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                                 "File='%s' operation[%d] type=%d - new "
                                 "pipeline only implements SELECT so far, "
                                 "skipping", filename, i, (int)op->type);
                }
            }
        }
        else
        {
            int failed_op = -1;
            for (int i = 0; i < new_request.operation_count; i++)
                if (new_request.operations[i].validation_status.status_code != 0)
                { failed_op = i; break; }

            if (failed_op >= 0)
                logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                             "FAIL [Level2] %s operation[%d] error_code=%s error_text=%s",
                             filename, failed_op,
                             new_request.operations[failed_op].validation_status.error_code,
                             new_request.operations[failed_op].validation_status.error_text);
            rc = -1;
        }

        level1_free_request(&new_request);
        free(xml);
        return rc;
    }



    char operation[MAX_OPERATION_LEN] = {0};
    if (!extract_tag(xml, "operation", operation, sizeof(operation)))
    {
        logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                     "No <operation> found in %s - skipping", filename);
        free(xml);
        return 0;
    }
    upper(operation);

    logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                 "File='%s' operation='%s' size=%ld bytes",
                 filename, operation, len);

    int rc = 0;

    if      (strcmp(operation, "INSERT") == 0)
        rc = dispatch_insert(ctx, filename, xml);
    else if (strcmp(operation, "SELECT") == 0)
        rc = dispatch_select(ctx, filename, xml);
    else if (strcmp(operation, "UPDATE") == 0)
        rc = dispatch_update(ctx, filename, xml);
    else if (strcmp(operation, "DELETE") == 0)
        rc = dispatch_delete(ctx, filename, xml);
    else if (strcmp(operation, "EXECUTE_PROCEDURE") == 0)
        rc = dispatch_procedure(ctx, filename, xml);
    else
        logger_write(ctx->connectionpool_logger, LOG_WARN, __func__, 0,
                     "Unknown operation '%s' in %s - skipping",
                     operation, filename);

    free(xml);
    return rc;
}

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
							   logger_t      *sql_parser_logger)
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
    ctx->connectionpool_logger = logger;
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
							&sql_parser_logger
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

    /* ---- Scan input_xml directory ---- */
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
         */
        rc = process_xml_file(tx_ctx, filepath, name);
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




/*
 * looks_like_new_request_format()
 *
 * Cheap sniff of just the root tag - does NOT call level1_parse().
 * Only files that pass this go anywhere near the real parser, so
 * level1_parse()'s existing LOG_ERROR-level diagnostics stay meaningful
 * (a real error on a file that already looked like a new-format
 * request) rather than firing on every old-format file just for being
 * old-format, which would otherwise be normal/expected noise in
 * error_Data_Manager.log during this transition period.
 *
 * Deliberately does not touch or duplicate level1_parse()'s own format
 * detection (level1_detect_format()) - that function's job is
 * XML-vs-JSON; this one's job is old-format-vs-new-format, a different
 * question, answered before level1_parse() is ever invoked at all.
 */

static int looks_like_new_request_format(const char *xml, size_t len)
{
    if (!xml) return 0;

    input_format_t fmt = level1_detect_format(xml, len);

    if (fmt == INPUT_FORMAT_JSON)
        return 1;   /* old-format files are never JSON - safe to assume new format */

    if (fmt == INPUT_FORMAT_XML)
    {
        /* Skip an optional XML declaration before checking the root tag -
         * e.g. <?xml version="1.0" encoding="UTF-8"?> - every Request_*.xml
         * fixture has one, and the original version of this check didn't
         * account for it, so every new-format XML file was incorrectly
         * treated as old-format.                                         */
        const char *p = xml;
        while (*p && isspace((unsigned char)*p)) p++;

        if (strncmp(p, "<?xml", 5) == 0)
        {
            const char *decl_end = strstr(p, "?>");
            if (decl_end)
            {
                p = decl_end + 2;
                while (*p && isspace((unsigned char)*p)) p++;
            }
        }

        return (strncmp(p, "<request", 8) == 0);
    }

    return 0;
}




/* ================================================================== */
/*  dispatch_select_new                                                 */
/*  SELECT via the new format-agnostic pipeline (Level 1 already ran,   */
/*  Level 2 already validated req->sql - this is purely the thin        */
/*  adapter from select_request_t to execute_config_t, then the same    */
/*  execute_query_batch() call the old dispatch_select() already uses,  */
/*  unchanged).                                                          */
/* ================================================================== */
static int dispatch_select_new(oci_context_t       *ctx,
                                const char          *filename,
                                input_c_request_t   *request,
                                input_c_operation_t *op)
{
    select_request_t *req = (select_request_t *)op->payload;

    if (!req)
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT/new]: %s - no select_request_t payload",
                     filename);
        return -1;
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* req->sql is already a writable char[4096] buffer, not a pointer
     * to a literal - safe to point cfg.SQL directly at it. See
     * OCI_Session_Manager.c's session_reconcile_orphans() design note
     * for why that distinction matters (trim_sql_inplace() modifies
     * cfg->SQL in place).                                              */
    cfg.SQL = req->sql;

    /* 0 means "use the config default" per select_request_t's own doc
     * comment - Level 1 deliberately left these as 0 rather than
     * guessing, same as the old dispatch_select() already does.        */
    cfg.max_rows         = (req->max_rows > 0)
                            ? req->max_rows
                            : ctx->ini->query_max_record_count;
    cfg.fetch_array_size = (req->fetch_batch_size > 0)
                            ? req->fetch_batch_size
                            : ctx->ini->query_fetch_batch_size;

    cfg.log_execution_results = 1;
    cfg.include_column_names  = req->include_column_names;
    cfg.input_file_name        = (char *)filename;

    /* This is the actual fix for JSON requests never getting a JSON
     * response (and, downstream of that, metrics always showing XML
     * in output_response): ReturnFormat existed on execute_config_t
     * but nothing ever set it, so every format-aware check added later
     * (cache hit serving, response_writer_cache_store(), the metrics
     * output_response fix) always evaluated as "not JSON" regardless
     * of what was actually requested.                                  */
    cfg.ReturnFormat = (request->source_format == INPUT_FORMAT_JSON)
                        ? "JSON" : "XML";

    int rc = execute_query_batch(ctx, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                     "PASS [SELECT/new] audit_id=%s: %s",
                     request->external_audit_id, filename);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
        if (cfg.OUTPUT_JSON)
            logger_write(ctx->connectionpool_logger, LOG_INFO, __func__, 0,
                         "Result JSON:\n%s", cfg.OUTPUT_JSON);
    }
    else
    {
        logger_write(ctx->connectionpool_logger, LOG_ERROR, __func__, 0,
                     "FAIL [SELECT/new] audit_id=%s: %s (rc=%d)",
                     request->external_audit_id, filename, rc);
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }

    /* Now that ReturnFormat is actually set above, cfg.OUTPUT_JSON can
     * really be populated by execute_query_batch() - free it here or
     * this becomes a genuine leak on every JSON request.               */
    if (cfg.OUTPUT_JSON) free(cfg.OUTPUT_JSON);

    return rc;
}



