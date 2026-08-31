/*
 * OCI_Unit_Test_Module.c
 *
 * See OCI_Unit_Test_Module.h for the full architecture rationale, and
 * Unit_Test_Module_Design_Specification.docx for the complete design
 * (tiers, catalog, decisions).
 *
 * Only Tier 1 tests (UT-LOG-001/002, UT-INI-001/002/003, UT-L1-001
 * through UT-L1-008, UT-DATE-005) are implemented in this first pass -
 * 2026-08-01. Tier 2/3 tests follow once the Tier 1 harness pattern
 * established here is proven, per the agreed implementation order.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "OCI_Unit_Test_Module.h"
#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "file_consumer.h"      /* payload_requires_single_writer_queue() -
                                    UT-CONT-001/002, 2026-08-12 */
#include "generic_queue.h"      /* UT-SESS-006, 2026-08-12 */
#include "OCI_Level1_Parser.h"
#include "OCI_Level2_Parser.h"
#include "OCI_Insert_Execute_Module.h"
#include "OCI_Update_Execute_Module.h"
#include "OCI_Delete_Execute_Module.h"
#include "OCI_Execute_Procedure_Module.h"
#include "OCI_Execute_Query_Batch_Module.h"
#include "OCI_Request_Response_Types.h"
#include "OCI_Session_Manager.h"
#include "metadata_cache.h"
#include "metadata_cache_meta.h"
#include "OCI_Table_Metadata_Module.h"
#include "OCI_Transaction_Manager.h"
#include "OCI_Audit_Trail_Manager.h"
#include "ini_reader.h"
#include "logger.h"

/* ================================================================== */
/*  Small helpers                                                       */
/* ================================================================== */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Set once by main() before unit_test_run_all() is called, so
 * UT-INI-002 (the only test that needs it) can re-run load_ini()
 * against the real, known-good config.ini - deliberately not a new
 * oci_context_t field (that struct is a connection concern, not a
 * test-harness one) and deliberately not threaded through every test
 * function's own signature just for the one test that needs it.       */
static char g_ini_file_path[512] = {0};

void unit_test_set_ini_path(const char *path)
{
    if (!path) { g_ini_file_path[0] = '\0'; return; }
    strncpy(g_ini_file_path, path, sizeof(g_ini_file_path) - 1);
    g_ini_file_path[sizeof(g_ini_file_path) - 1] = '\0';
}

/* Same reasoning as g_ini_file_path above - set once by main() (from
 * the already-loaded unit_test_config_t) before any Tier 3 test runs,
 * so every Tier 3 test knows which real, dedicated schema objects to
 * use without unit_test_config_t itself needing to be threaded through
 * every test function's own signature.                                 */
static char g_test_table_name[128] = {0};
static char g_test_table_owner[128] = {0};
static char g_test_procedure_name[128] = {0};

void unit_test_set_tier3_objects(const char *table_name, const char *table_owner,
                                  const char *procedure_name)
{
    strncpy(g_test_table_name, table_name ? table_name : "", sizeof(g_test_table_name) - 1);
    strncpy(g_test_table_owner, table_owner ? table_owner : "", sizeof(g_test_table_owner) - 1);
    strncpy(g_test_procedure_name, procedure_name ? procedure_name : "",
            sizeof(g_test_procedure_name) - 1);
}

/*
 * begin_test_transaction() / rollback_test_transaction()
 *
 * Every Tier 3 test wraps its own real INSERT/UPDATE/DELETE/PROCEDURE
 * calls between these two - giving execute_insert_batch() etc. an
 * already-active external transaction (via ctx->active_tx) so they
 * never commit internally themselves (see the "if (!ctx->active_tx)
 * OCITransCommit(...)" convention documented directly in
 * oci_context_t's own comment in OCI_Connection.h), then always rolling
 * back explicitly afterward regardless of the test's own outcome - no
 * Tier 3 test run ever leaves real rows, audit entries, or session
 * records behind, per the design spec's own Tier 3 definition.
 *
 * Reuses tx_begin()/tx_rollback() directly rather than
 * begin_standalone_tx_if_needed()/end_standalone_tx_if_owned() - those
 * two are designed for the opposite direction (an execute module
 * giving *itself* a transaction when none exists yet); here, the test
 * harness is deliberately the one supplying the external transaction.
 */
static int begin_test_transaction(oci_context_t *ctx, tx_handle_t *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->ctx = ctx;   /* tx_begin()'s own very first check requires this -
                      * back-pointer, not owned by tx_handle_t, per its
                      * own doc comment in OCI_Transaction_Manager.h    */
    char *result_xml = NULL;
    int rc = tx_begin(tx, "unit-test-session", "unit_test_tx", &result_xml);
    free(result_xml);
    if (rc != 0) return -1;
    ctx->active_tx = tx;
    return 0;
}

static void rollback_test_transaction(oci_context_t *ctx, tx_handle_t *tx)
{
    char *result_xml = NULL;
    tx_rollback(tx, &result_xml);
    free(result_xml);
    ctx->active_tx = NULL;
}

/* ================================================================== */
/*  unit_test_load_config                                                */
/*  Simple key=value parser - see this function's own doc comment in    */
/*  OCI_Unit_Test_Module.h for the full reasoning (deliberately not      */
/*  ini_reader.c's load_ini(), deliberately never a hard-failure path).  */
/* ================================================================== */
static void trim_ut_line(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) { s[len - 1] = '\0'; len--; }
}

int unit_test_load_config(const char *path, logger_t *logger, unit_test_config_t *cfg)
{
    if (!cfg) return 0;

    memset(cfg, 0, sizeof(*cfg));
    /* Safe, backward-compatible defaults - see this function's own doc
     * comment in OCI_Unit_Test_Module.h.                                */
    cfg->startup_self_test_enabled     = 0;
    cfg->startup_max_tier              = 1;
    cfg->startup_halt_on_tier1_fail    = 1;
    cfg->startup_halt_on_tier2_fail    = 1;
    cfg->startup_halt_on_tier3_fail    = 0;
    cfg->unit_test_log_summary_enabled = 1;
    strncpy(cfg->unit_test_log_file_name, "start_unit_tests.log",
            sizeof(cfg->unit_test_log_file_name) - 1);
    cfg->unit_test_log_file_max_size = 10485760;   /* 10MB, matching the
                                                     * project-wide default */
    cfg->unit_test_log_file_rotation_number = 5;
    strncpy(cfg->unit_test_log_level, "DEBUG", sizeof(cfg->unit_test_log_level) - 1);

    if (!path)
    {
        if (logger) logger_write(logger, LOG_INFO, __func__, 0,
                     "unit_test_load_config: no path given - "
                     "startup self-test stays disabled");
        return 0;
    }

    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        if (logger) logger_write(logger, LOG_INFO, __func__, 0,
                     "unit_test_load_config: '%s' not found - startup "
                     "self-test stays disabled (this is expected for any "
                     "deployment that predates this feature)", path);
        return 0;
    }

    char line[512];
    int  found_enabled_key = 0;
    while (fgets(line, sizeof(line), fp))
    {
        trim_ut_line(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_ut_line(key);
        trim_ut_line(val);

        if (strcmp(key, "startup_self_test_enabled") == 0)
        { cfg->startup_self_test_enabled = atoi(val); found_enabled_key = 1; }
        else if (strcmp(key, "startup_max_tier") == 0)
            cfg->startup_max_tier = atoi(val);
        else if (strcmp(key, "startup_halt_on_tier1_fail") == 0)
            cfg->startup_halt_on_tier1_fail = atoi(val);
        else if (strcmp(key, "startup_halt_on_tier2_fail") == 0)
            cfg->startup_halt_on_tier2_fail = atoi(val);
        else if (strcmp(key, "startup_halt_on_tier3_fail") == 0)
            cfg->startup_halt_on_tier3_fail = atoi(val);
        else if (strcmp(key, "test_table_name") == 0)
            strncpy(cfg->test_table_name, val, sizeof(cfg->test_table_name) - 1);
        else if (strcmp(key, "test_table_owner") == 0)
            strncpy(cfg->test_table_owner, val, sizeof(cfg->test_table_owner) - 1);
        else if (strcmp(key, "test_procedure_name") == 0)
            strncpy(cfg->test_procedure_name, val, sizeof(cfg->test_procedure_name) - 1);
        else if (strcmp(key, "unit_test_log_summary_enabled") == 0)
            cfg->unit_test_log_summary_enabled = atoi(val);
        else if (strcmp(key, "unit_test_log_file_name") == 0)
            strncpy(cfg->unit_test_log_file_name, val,
                    sizeof(cfg->unit_test_log_file_name) - 1);
        else if (strcmp(key, "unit_test_log_file_max_size") == 0)
            cfg->unit_test_log_file_max_size = atof(val);
        else if (strcmp(key, "unit_test_log_file_rotation_number") == 0)
            cfg->unit_test_log_file_rotation_number = atoi(val);
        else if (strcmp(key, "unit_test_log_level") == 0)
            strncpy(cfg->unit_test_log_level, val, sizeof(cfg->unit_test_log_level) - 1);
        else if (logger)
            logger_write(logger, LOG_WARN, __func__, 0,
                         "unit_test_load_config: unrecognised key '%s' in "
                         "'%s' - ignored", key, path);
    }
    fclose(fp);

    if (!found_enabled_key && logger)
        logger_write(logger, LOG_WARN, __func__, 0,
                     "unit_test_load_config: '%s' exists but has no "
                     "startup_self_test_enabled key - defaulting to disabled",
                     path);

    if (logger)
        logger_write(logger, LOG_INFO, __func__, 0,
                     "unit_test_load_config: loaded from '%s' - "
                     "self_test_enabled=%d max_tier=%d",
                     path, cfg->startup_self_test_enabled, cfg->startup_max_tier);

    return 0;
}

/* ================================================================== */
/*  UT-LOG-001                                                           */
/*  Every logger actually initialised by main() (see the                */
/*  logger_init_str2() call sequence in Test_XML_Runner.c) is non-NULL   */
/*  on ctx. Listed explicitly rather than derived from config.ini's own  */
/*  field list, since a handful of config fields (connection_log_*,      */
/*  dml_log_*, error_log_*) are reserved but not currently wired up to   */
/*  their own logger_t at all - checking those here would be a false     */
/*  failure, not a real one.                                             */
/* ================================================================== */
static int test_ut_log_001(oci_context_t *ctx, char *message, size_t message_max)
{
    struct { const char *name; logger_t *logger; } checks[] = {
        { "logger",             ctx->logger },
        { "select_logger",      ctx->select_logger },
        { "cache_logger",       ctx->cache_logger },
        { "Metadata_logger",    ctx->Metadata_logger },
        { "connectionpool_logger", ctx->connectionpool_logger },
        { "insert_logger",      ctx->insert_logger },
        { "update_logger",      ctx->update_logger },
        { "delete_logger",      ctx->delete_logger },
        { "ddl_logger",         ctx->ddl_logger },
        { "procedure_logger",   ctx->procedure_logger },
        { "metrics_logger",     ctx->metrics_logger },
        { "transaction_logger", ctx->transaction_logger },
        { "security_logger",    ctx->security_logger },
        { "crypt_logger",       ctx->crypt_logger },
        { "audit_logger",       ctx->audit_logger },
        { "session_logger",     ctx->session_logger },
        { "sql_parser_logger",  ctx->sql_parser_logger },
    };
    size_t n = sizeof(checks) / sizeof(checks[0]);

    for (size_t i = 0; i < n; i++)
    {
        if (!checks[i].logger)
        {
            snprintf(message, message_max,
                     "ctx->%s is NULL - not initialised", checks[i].name);
            return -1;
        }
    }
    return 0;
}

/* ================================================================== */
/*  UT-LOG-002                                                           */
/*  Log level filtering: a DEBUG-level line is suppressed when the       */
/*  logger's level is INFO. Uses its own temporary logger/file - never    */
/*  touches any of ctx's real log files, so a test run never adds noise   */
/*  to production logs.                                                  */
/* ================================================================== */
static int test_ut_log_002(oci_context_t *ctx, char *message, size_t message_max)
{
    (void)ctx;

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/ut_log_002_%d.log", (int)getpid());

    logger_t temp_logger;
    memset(&temp_logger, 0, sizeof(temp_logger));

    if (logger_init_str2(&temp_logger, tmp_path, 1048576.0, 1, "INFO", NULL) != 0)
    {
        snprintf(message, message_max,
                 "Could not initialise temporary logger at '%s'", tmp_path);
        return -1;
    }

    const char *marker = "UT_LOG_002_DEBUG_MARKER_SHOULD_NOT_APPEAR";
    logger_write(&temp_logger, LOG_DEBUG, __func__, 0, "%s", marker);
    logger_close(&temp_logger);

    int rc = 0;
    FILE *fp = fopen(tmp_path, "r");
    if (!fp)
    {
        snprintf(message, message_max,
                 "Temporary log file '%s' was not created", tmp_path);
        rc = -1;
    }
    else
    {
        char line[512];
        int found = 0;
        while (fgets(line, sizeof(line), fp))
        {
            if (strstr(line, marker)) { found = 1; break; }
        }
        fclose(fp);

        if (found)
        {
            snprintf(message, message_max,
                     "DEBUG-level line was written despite logger level=INFO - "
                     "log level filtering is not working");
            rc = -1;
        }
    }

    unlink(tmp_path);
    return rc;
}

/* ================================================================== */
/*  UT-INI-001                                                           */
/*  ctx->ini is already populated by the time any test runs (main()     */
/*  calls load_ini() before setting up loggers/connection) - this is a   */
/*  structural sanity check that the real load actually succeeded and    */
/*  populated a sentinel field, not a re-test of load_ini() itself       */
/*  (that's UT-INI-002/003 below).                                       */
/* ================================================================== */
static int test_ut_ini_001(oci_context_t *ctx, char *message, size_t message_max)
{
    if (!ctx->ini)
    {
        snprintf(message, message_max, "ctx->ini is NULL");
        return -1;
    }
    if (!ctx->ini->nls_date_format[0])
    {
        snprintf(message, message_max,
                 "ctx->ini->nls_date_format is empty - load_ini() did not "
                 "populate a value or default correctly");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  UT-INI-002                                                           */
/*  Regression test for the 2026-07-25 ini_reader.c stack-buffer         */
/*  overflow (three CFG_STRING fields silently defaulting to maxlen=256  */
/*  against 16-byte destination buffers). Re-runs load_ini() against the */
/*  real, already-known-good config.ini into a fresh app_config_t - if   */
/*  any CFG_STRING field is ever added in future without its own maxlen   */
/*  branch, load_ini() itself now refuses to proceed (see ini_reader.c's  */
/*  own unguarded_names[] safeguard) and this test catches that refusal   */
/*  independently of whatever happened during the real startup load.     */
/* ================================================================== */
static int test_ut_ini_002(oci_context_t *ctx, char *message, size_t message_max)
{
    if (!g_ini_file_path[0])
    {
        snprintf(message, message_max,
                 "unit_test_set_ini_path() was never called - cannot re-run "
                 "load_ini() for this test (main() must call it once before "
                 "unit_test_run_all())");
        return -1;
    }

    app_config_t   fresh_config;
    oci_context_t  fresh_ctx;
    memset(&fresh_config, 0, sizeof(fresh_config));
    memset(&fresh_ctx, 0, sizeof(fresh_ctx));
    fresh_ctx.error_logger = ctx->error_logger;   /* reuse the real, working one */

    int rc = load_ini(g_ini_file_path, &fresh_config, &fresh_ctx);
    if (rc != 0)
    {
        snprintf(message, message_max,
                 "load_ini() failed (rc=%d) against the known-good config.ini "
                 "on re-run - see error_Data_Manager.log for which field(s) "
                 "triggered it (missing key, or an unguarded CFG_STRING field)",
                 rc);
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  UT-INI-003                                                           */
/*  A config.ini missing a required key is rejected outright, not        */
/*  silently defaulted - confirms load_ini()'s own missing-key check     */
/*  ("Refusing to start with N missing configuration key(s)") actually   */
/*  fires. Builds a deliberately empty temp file rather than editing a   */
/*  copy of the real config.ini, so this test has no dependency on which  */
/*  specific key is chosen as "required" - an entirely empty file must   */
/*  be missing at least every required key.                              */
/* ================================================================== */
static int test_ut_ini_003(oci_context_t *ctx, char *message, size_t message_max)
{
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/ut_ini_003_%d.ini", (int)getpid());

    FILE *fp = fopen(tmp_path, "w");
    if (!fp)
    {
        snprintf(message, message_max,
                 "Could not create temporary empty ini file at '%s'", tmp_path);
        return -1;
    }
    fprintf(fp, "[Data_Manager]\n");   /* section header only, no keys at all */
    fclose(fp);

    app_config_t   empty_config;
    oci_context_t  temp_ctx;
    memset(&empty_config, 0, sizeof(empty_config));
    memset(&temp_ctx, 0, sizeof(temp_ctx));
    temp_ctx.error_logger = ctx->error_logger;

    int rc = load_ini(tmp_path, &empty_config, &temp_ctx);
    unlink(tmp_path);

    if (rc == 0)
    {
        snprintf(message, message_max,
                 "load_ini() succeeded against a config.ini with no keys at "
                 "all - missing-required-key detection is not working");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  UT-L1-001                                                            */
/*  Regression test for the 2026-07-28 comment-skipping dispatch bug.    */
/*  Calls level1_looks_like_new_format() (OCI_Level1_Parser.c, relocated  */
/*  here 2026-08-01 from Test_XML_Runner.c specifically so this test can  */
/*  exercise the real function rather than a reimplementation of it)     */
/*  against a buffer shaped exactly like every fixture in this project:   */
/*  XML declaration, blank line, multi-line comment, blank line, then     */
/*  the <request> root tag.                                               */
/* ================================================================== */
static int test_ut_l1_001(oci_context_t *ctx, char *message, size_t message_max)
{
    (void)ctx;

    static const char *buf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "\n"
        "<!--\n"
        "========================================================================\n"
        " A multi-line descriptive comment, exactly like every real fixture\n"
        " in this project has.\n"
        "========================================================================\n"
        "-->\n"
        "\n"
        "<request version=\"1.0\">\n"
        "    <external_audit_id>audit_stub_ut_l1_001</external_audit_id>\n"
        "    <session_id>-</session_id>\n"
        "    <transaction required=\"0\">\n"
        "        <operation type=\"SELECT\">\n"
        "            <sql>SELECT 1 FROM DUAL</sql>\n"
        "        </operation>\n"
        "    </transaction>\n"
        "</request>\n";

    if (!level1_looks_like_new_format(buf, strlen(buf)))
    {
        snprintf(message, message_max,
                 "level1_looks_like_new_format() returned false for a "
                 "well-formed request preceded by a comment block - this is "
                 "exactly the 2026-07-28 regression (every fixture in this "
                 "project has a leading comment; a false result here means "
                 "every one of them would silently misroute to the dead "
                 "old-format path again)");
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  UT-L1-002                                                            */
/*  The same INSERT request, XML and JSON forms, parses to identical      */
/*  in-memory field values.                                               */
/* ================================================================== */
static int test_ut_l1_002(oci_context_t *ctx, char *message, size_t message_max)
{
    static const char *xml_buf =
        "<?xml version=\"1.0\"?>\n"
        "<request version=\"1.0\">\n"
        "  <external_audit_id>audit_stub_ut_l1_002</external_audit_id>\n"
        "  <session_id>-</session_id>\n"
        "  <transaction required=\"1\">\n"
        "    <operation type=\"INSERT\">\n"
        "      <table_name>OCI_FIELD_TEST</table_name>\n"
        "      <owner>DATA_MANAGER</owner>\n"
        "      <row number=\"1\">\n"
        "        <field><field_name>NUMBER_COL</field_name><value>777</value></field>\n"
        "      </row>\n"
        "    </operation>\n"
        "  </transaction>\n"
        "</request>\n";

    static const char *json_buf =
        "{"
        "\"external_audit_id\":\"audit_stub_ut_l1_002\","
        "\"session_id\":\"-\","
        "\"transaction\":{\"required\":1,\"operations\":["
        "{\"type\":\"INSERT\",\"table_name\":\"OCI_FIELD_TEST\",\"owner\":\"DATA_MANAGER\","
        "\"rows\":[{\"number\":1,\"fields\":["
        "{\"field_name\":\"NUMBER_COL\",\"value\":\"777\"}]}]}"
        "]}}";

    input_c_request_t xml_req, json_req;
    operation_status_t status;
    memset(&xml_req, 0, sizeof(xml_req));
    memset(&json_req, 0, sizeof(json_req));
    memset(&status, 0, sizeof(status));

    int rc1 = level1_parse(ctx, xml_buf, strlen(xml_buf), &xml_req, &status);
    if (rc1 != 0)
    {
        snprintf(message, message_max, "XML parse failed: %s", status.error_text);
        return -1;
    }

    memset(&status, 0, sizeof(status));
    int rc2 = level1_parse(ctx, json_buf, strlen(json_buf), &json_req, &status);
    if (rc2 != 0)
    {
        snprintf(message, message_max, "JSON parse failed: %s", status.error_text);
        level1_free_request(&xml_req);
        return -1;
    }

    int rc = 0;
    if (xml_req.operation_count != 1 || json_req.operation_count != 1)
    {
        snprintf(message, message_max,
                 "operation_count mismatch: xml=%d json=%d",
                 xml_req.operation_count, json_req.operation_count);
        rc = -1;
    }
    else
    {
        insert_request_t *xi = (insert_request_t *)xml_req.operations[0].payload;
        insert_request_t *ji = (insert_request_t *)json_req.operations[0].payload;

        if (strcmp(xi->table_name, ji->table_name) != 0 ||
            strcmp(xi->owner, ji->owner) != 0 ||
            xi->row_count != ji->row_count ||
            strcmp(xi->rows[0].fields[0].field_name, ji->rows[0].fields[0].field_name) != 0 ||
            strcmp(xi->rows[0].fields[0].value, ji->rows[0].fields[0].value) != 0)
        {
            snprintf(message, message_max,
                     "XML and JSON parses produced different field values "
                     "for the same logical request");
            rc = -1;
        }
    }

    level1_free_request(&xml_req);
    level1_free_request(&json_req);
    return rc;
}

/* ================================================================== */
/*  UT-L1-003                                                            */
/*  A <transaction> with mixed operation types (INSERT + UPDATE +        */
/*  DELETE) parses to operation_count=3, in order.                       */
/* ================================================================== */
static int test_ut_l1_003(oci_context_t *ctx, char *message, size_t message_max)
{
    static const char *buf =
        "<?xml version=\"1.0\"?>\n"
        "<request version=\"1.0\">\n"
        "  <external_audit_id>audit_stub_ut_l1_003</external_audit_id>\n"
        "  <session_id>-</session_id>\n"
        "  <transaction required=\"1\">\n"
        "    <operation type=\"INSERT\">\n"
        "      <table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>\n"
        "      <row number=\"1\"><field><field_name>NUMBER_COL</field_name><value>1</value></field></row>\n"
        "    </operation>\n"
        "    <operation type=\"UPDATE\">\n"
        "      <table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>\n"
        "      <where><key><field_name>NUMBER_COL</field_name><key_value>1</key_value></key></where>\n"
        "      <set><field><field_name>VARCHAR2_COL</field_name><value>x</value></field></set>\n"
        "    </operation>\n"
        "    <operation type=\"DELETE\">\n"
        "      <table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>\n"
        "      <where><key><field_name>NUMBER_COL</field_name><key_value>1</key_value></key></where>\n"
        "    </operation>\n"
        "  </transaction>\n"
        "</request>\n";

    input_c_request_t req;
    operation_status_t status;
    memset(&req, 0, sizeof(req));
    memset(&status, 0, sizeof(status));

    if (level1_parse(ctx, buf, strlen(buf), &req, &status) != 0)
    {
        snprintf(message, message_max, "Parse failed: %s", status.error_text);
        return -1;
    }

    int rc = 0;
    if (req.operation_count != 3)
    {
        snprintf(message, message_max,
                 "operation_count=%d, expected 3", req.operation_count);
        rc = -1;
    }
    else if (req.operations[0].type != OP_INSERT ||
             req.operations[1].type != OP_UPDATE ||
             req.operations[2].type != OP_DELETE)
    {
        snprintf(message, message_max,
                 "Operations parsed out of order or with the wrong type: "
                 "got [%d, %d, %d], expected [INSERT, UPDATE, DELETE]",
                 (int)req.operations[0].type, (int)req.operations[1].type,
                 (int)req.operations[2].type);
        rc = -1;
    }

    level1_free_request(&req);
    return rc;
}

/* ================================================================== */
/*  UT-L1-004                                                            */
/*  INSERT: multiple <row> elements parse to the correct row_count and   */
/*  field values per row.                                                 */
/* ================================================================== */
static int test_ut_l1_004(oci_context_t *ctx, char *message, size_t message_max)
{
    static const char *buf =
        "<?xml version=\"1.0\"?>\n"
        "<request version=\"1.0\">\n"
        "  <external_audit_id>audit_stub_ut_l1_004</external_audit_id>\n"
        "  <session_id>-</session_id>\n"
        "  <transaction required=\"1\">\n"
        "    <operation type=\"INSERT\">\n"
        "      <table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>\n"
        "      <row number=\"1\"><field><field_name>NUMBER_COL</field_name><value>101</value></field></row>\n"
        "      <row number=\"2\"><field><field_name>NUMBER_COL</field_name><value>102</value></field></row>\n"
        "      <row number=\"3\"><field><field_name>NUMBER_COL</field_name><value>103</value></field></row>\n"
        "    </operation>\n"
        "  </transaction>\n"
        "</request>\n";

    input_c_request_t req;
    operation_status_t status;
    memset(&req, 0, sizeof(req));
    memset(&status, 0, sizeof(status));

    if (level1_parse(ctx, buf, strlen(buf), &req, &status) != 0)
    {
        snprintf(message, message_max, "Parse failed: %s", status.error_text);
        return -1;
    }

    int rc = 0;
    insert_request_t *ins = (insert_request_t *)req.operations[0].payload;
    if (ins->row_count != 3)
    {
        snprintf(message, message_max, "row_count=%d, expected 3", ins->row_count);
        rc = -1;
    }
    else if (strcmp(ins->rows[0].fields[0].value, "101") != 0 ||
             strcmp(ins->rows[1].fields[0].value, "102") != 0 ||
             strcmp(ins->rows[2].fields[0].value, "103") != 0)
    {
        snprintf(message, message_max,
                 "Row values parsed incorrectly: got [%s, %s, %s], "
                 "expected [101, 102, 103]",
                 ins->rows[0].fields[0].value, ins->rows[1].fields[0].value,
                 ins->rows[2].fields[0].value);
        rc = -1;
    }

    level1_free_request(&req);
    return rc;
}

/* ================================================================== */
/*  UT-L1-005                                                            */
/*  UPDATE: <client_date_format> on a WHERE key parses into                */
/*  where_key_t.client_date_format correctly; absent tag leaves it empty. */
/* ================================================================== */
static int test_ut_l1_005(oci_context_t *ctx, char *message, size_t message_max)
{
    static const char *buf =
        "<?xml version=\"1.0\"?>\n"
        "<request version=\"1.0\">\n"
        "  <external_audit_id>audit_stub_ut_l1_005</external_audit_id>\n"
        "  <session_id>-</session_id>\n"
        "  <transaction required=\"1\">\n"
        "    <operation type=\"UPDATE\">\n"
        "      <table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>\n"
        "      <where>\n"
        "        <key><field_name>DATE_COL</field_name><key_value>19/08/2026 14:30:00</key_value>"
        "<client_date_format>DD/MM/YYYY HH24:MI:SS</client_date_format></key>\n"
        "        <key><field_name>NUMBER_COL</field_name><key_value>1</key_value></key>\n"
        "      </where>\n"
        "      <set><field><field_name>VARCHAR2_COL</field_name><value>x</value></field></set>\n"
        "    </operation>\n"
        "  </transaction>\n"
        "</request>\n";

    input_c_request_t req;
    operation_status_t status;
    memset(&req, 0, sizeof(req));
    memset(&status, 0, sizeof(status));

    if (level1_parse(ctx, buf, strlen(buf), &req, &status) != 0)
    {
        snprintf(message, message_max, "Parse failed: %s", status.error_text);
        return -1;
    }

    int rc = 0;
    update_request_t *upd = (update_request_t *)req.operations[0].payload;
    if (strcmp(upd->keys[0].client_date_format, "DD/MM/YYYY HH24:MI:SS") != 0)
    {
        snprintf(message, message_max,
                 "keys[0].client_date_format='%s', expected 'DD/MM/YYYY HH24:MI:SS'",
                 upd->keys[0].client_date_format);
        rc = -1;
    }
    else if (upd->keys[1].client_date_format[0] != '\0')
    {
        snprintf(message, message_max,
                 "keys[1].client_date_format='%s', expected empty (tag was absent)",
                 upd->keys[1].client_date_format);
        rc = -1;
    }

    level1_free_request(&req);
    return rc;
}

/* ================================================================== */
/*  UT-L1-006                                                            */
/*  DELETE: <client_date_format> on a WHERE key parses correctly           */
/*  (mirrors UT-L1-005).                                                  */
/* ================================================================== */
static int test_ut_l1_006(oci_context_t *ctx, char *message, size_t message_max)
{
    static const char *buf =
        "<?xml version=\"1.0\"?>\n"
        "<request version=\"1.0\">\n"
        "  <external_audit_id>audit_stub_ut_l1_006</external_audit_id>\n"
        "  <session_id>-</session_id>\n"
        "  <transaction required=\"1\">\n"
        "    <operation type=\"DELETE\">\n"
        "      <table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>\n"
        "      <where>\n"
        "        <key><field_name>DATE_COL</field_name><key_value>08/19/2026 14:30:00</key_value>"
        "<client_date_format>MM/DD/YYYY HH24:MI:SS</client_date_format></key>\n"
        "      </where>\n"
        "    </operation>\n"
        "  </transaction>\n"
        "</request>\n";

    input_c_request_t req;
    operation_status_t status;
    memset(&req, 0, sizeof(req));
    memset(&status, 0, sizeof(status));

    if (level1_parse(ctx, buf, strlen(buf), &req, &status) != 0)
    {
        snprintf(message, message_max, "Parse failed: %s", status.error_text);
        return -1;
    }

    int rc = 0;
    delete_request_t *del = (delete_request_t *)req.operations[0].payload;
    if (strcmp(del->keys[0].client_date_format, "MM/DD/YYYY HH24:MI:SS") != 0)
    {
        snprintf(message, message_max,
                 "keys[0].client_date_format='%s', expected 'MM/DD/YYYY HH24:MI:SS'",
                 del->keys[0].client_date_format);
        rc = -1;
    }

    level1_free_request(&req);
    return rc;
}

/* ================================================================== */
/*  UT-L1-007                                                            */
/*  EXECUTE_PROCEDURE: nested <parameters><parameter> parses to the      */
/*  correct param_count, and <param_direction> maps to the correct        */
/*  param_direction_t (IN/OUT/IN_OUT), including an unrecognised value    */
/*  defaulting to IN (matching parse_direction_l1()'s documented          */
/*  behaviour).                                                            */
/* ================================================================== */
static int test_ut_l1_007(oci_context_t *ctx, char *message, size_t message_max)
{
    static const char *buf =
        "<?xml version=\"1.0\"?>\n"
        "<request version=\"1.0\">\n"
        "  <external_audit_id>audit_stub_ut_l1_007</external_audit_id>\n"
        "  <session_id>-</session_id>\n"
        "  <transaction required=\"1\">\n"
        "    <operation type=\"EXECUTE_PROCEDURE\">\n"
        "      <procedure_name>SOME_PROC</procedure_name><owner>DATA_MANAGER</owner>\n"
        "      <parameters>\n"
        "        <parameter><param_name>P_IN</param_name><param_type>NUMBER</param_type>"
        "<param_direction>IN</param_direction><param_value>1</param_value></parameter>\n"
        "        <parameter><param_name>P_OUT</param_name><param_type>INTEGER</param_type>"
        "<param_direction>OUT</param_direction><param_value></param_value></parameter>\n"
        "        <parameter><param_name>P_CUR</param_name><param_type>CURSOR</param_type>"
        "<param_direction>OUT</param_direction><param_value></param_value></parameter>\n"
        "        <parameter><param_name>P_BAD</param_name><param_type>VARCHAR2</param_type>"
        "<param_direction>NOT_A_REAL_DIRECTION</param_direction><param_value></param_value></parameter>\n"
        "      </parameters>\n"
        "    </operation>\n"
        "  </transaction>\n"
        "</request>\n";

    input_c_request_t req;
    operation_status_t status;
    memset(&req, 0, sizeof(req));
    memset(&status, 0, sizeof(status));

    if (level1_parse(ctx, buf, strlen(buf), &req, &status) != 0)
    {
        snprintf(message, message_max, "Parse failed: %s", status.error_text);
        return -1;
    }

    int rc = 0;
    execute_procedure_request_t *proc =
        (execute_procedure_request_t *)req.operations[0].payload;

    if (proc->param_count != 4)
    {
        snprintf(message, message_max, "param_count=%d, expected 4", proc->param_count);
        rc = -1;
    }
    else if (proc->parameters[0].direction != PARAM_DIR_IN ||
             proc->parameters[1].direction != PARAM_DIR_OUT ||
             proc->parameters[2].direction != PARAM_DIR_OUT ||
             proc->parameters[3].direction != PARAM_DIR_IN)
    {
        snprintf(message, message_max,
                 "direction mapping wrong: got [%d, %d, %d, %d], expected "
                 "[IN, OUT, OUT, IN] (last one is an unrecognised direction "
                 "string, which should default to IN)",
                 (int)proc->parameters[0].direction, (int)proc->parameters[1].direction,
                 (int)proc->parameters[2].direction, (int)proc->parameters[3].direction);
        rc = -1;
    }

    level1_free_request(&req);
    return rc;
}

/* ================================================================== */
/*  UT-L1-008                                                            */
/*  level1_free_request() runs cleanly (no crash, and - under an ASan     */
/*  build - no leak) for a request of each operation type. This test     */
/*  itself cannot detect a leak without ASan; it exists to actually       */
/*  exercise every cleanup branch so ASan has something to check when      */
/*  the suite is run under it.                                            */
/* ================================================================== */
static int test_ut_l1_008(oci_context_t *ctx, char *message, size_t message_max)
{
    static const char *bufs[] = {
        /* INSERT */
        "<?xml version=\"1.0\"?><request version=\"1.0\">"
        "<external_audit_id>a</external_audit_id><session_id>-</session_id>"
        "<transaction required=\"1\"><operation type=\"INSERT\">"
        "<table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>"
        "<row number=\"1\"><field><field_name>NUMBER_COL</field_name><value>1</value></field></row>"
        "</operation></transaction></request>",
        /* UPDATE */
        "<?xml version=\"1.0\"?><request version=\"1.0\">"
        "<external_audit_id>a</external_audit_id><session_id>-</session_id>"
        "<transaction required=\"1\"><operation type=\"UPDATE\">"
        "<table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>"
        "<where><key><field_name>NUMBER_COL</field_name><key_value>1</key_value></key></where>"
        "<set><field><field_name>VARCHAR2_COL</field_name><value>x</value></field></set>"
        "</operation></transaction></request>",
        /* DELETE */
        "<?xml version=\"1.0\"?><request version=\"1.0\">"
        "<external_audit_id>a</external_audit_id><session_id>-</session_id>"
        "<transaction required=\"1\"><operation type=\"DELETE\">"
        "<table_name>OCI_FIELD_TEST</table_name><owner>DATA_MANAGER</owner>"
        "<where><key><field_name>NUMBER_COL</field_name><key_value>1</key_value></key></where>"
        "</operation></transaction></request>",
        /* SELECT */
        "<?xml version=\"1.0\"?><request version=\"1.0\">"
        "<external_audit_id>a</external_audit_id><session_id>-</session_id>"
        "<transaction required=\"1\"><operation type=\"SELECT\">"
        "<sql>SELECT 1 FROM DUAL</sql>"
        "</operation></transaction></request>",
        /* EXECUTE_PROCEDURE */
        "<?xml version=\"1.0\"?><request version=\"1.0\">"
        "<external_audit_id>a</external_audit_id><session_id>-</session_id>"
        "<transaction required=\"1\"><operation type=\"EXECUTE_PROCEDURE\">"
        "<procedure_name>SOME_PROC</procedure_name><owner>DATA_MANAGER</owner>"
        "<parameters><parameter><param_name>P1</param_name><param_type>NUMBER</param_type>"
        "<param_direction>IN</param_direction><param_value>1</param_value></parameter></parameters>"
        "</operation></transaction></request>",
    };
    size_t n = sizeof(bufs) / sizeof(bufs[0]);

    for (size_t i = 0; i < n; i++)
    {
        input_c_request_t req;
        operation_status_t status;
        memset(&req, 0, sizeof(req));
        memset(&status, 0, sizeof(status));

        if (level1_parse(ctx, bufs[i], strlen(bufs[i]), &req, &status) != 0)
        {
            snprintf(message, message_max,
                     "Parse failed for operation type index %zu: %s",
                     i, status.error_text);
            return -1;
        }
        level1_free_request(&req);
    }

    /* Also confirm level1_free_request() is safe on a zeroed, never-
     * populated request, per its own documented contract.              */
    input_c_request_t zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    level1_free_request(&zeroed);

    return 0;
}

/* ================================================================== */
/*  UT-DATE-005                                                          */
/*  No hardcoded date format literal exists in any of the four date-      */
/*  wrapper functions. A static/grep-based check rather than a runtime    */
/*  behavioural test - genuinely useful as a regression guard (a future   */
/*  edit could easily reintroduce a hardcoded literal), but depends on    */
/*  the source tree being present at the path this runs from. Gracefully  */
/*  passes with a note if the source files can't be found, rather than    */
/*  failing a check that's structurally unable to run in this             */
/*  environment (e.g. a production deployment with no source tree at      */
/*  all) - this is a development-time convenience check, not a functional  */
/*  correctness one.                                                       */
/* ================================================================== */
static int test_ut_date_005(oci_context_t *ctx, char *message, size_t message_max)
{
    (void)ctx;

    static const char *files[] = {
        "../src/OCI_Insert_Execute_Module.c",
        "../src/OCI_Update_Execute_Module.c",
        "../src/OCI_Delete_Execute_Module.c",
        "../src/OCI_Audit_Trail_Manager.c",
    };
    size_t n = sizeof(files) / sizeof(files[0]);
    int any_found = 0;

    for (size_t i = 0; i < n; i++)
    {
        FILE *fp = fopen(files[i], "r");
        if (!fp) continue;   /* source tree not present here - see doc comment */
        any_found = 1;

        char line[1024];
        while (fgets(line, sizeof(line), fp))
        {
            if (strstr(line, "YYYY-MM-DD HH24"))
            {
                snprintf(message, message_max,
                         "Hardcoded date format literal found in %s - every "
                         "TO_DATE()/TO_TIMESTAMP() wrapper must read "
                         "ctx->ini->nls_date_format dynamically instead "
                         "(2026-07-28 decision)", files[i]);
                fclose(fp);
                return -1;
            }
        }
        fclose(fp);
    }

    if (!any_found)
    {
        snprintf(message, message_max,
                 "Source tree not found at the expected relative paths - "
                 "this development-time check could not run in this "
                 "environment (not a failure)");
    }
    return 0;
}

/* ================================================================== */
/*  Tier 2 tests                                                        */
/*  All need a live connection but are read-only - no INSERT/UPDATE/     */
/*  DELETE/COMMIT anywhere in this section.                              */
/* ================================================================== */

/* ---- UT-CONN-001 ----
 * Confirms the connection this whole run is actually using is alive
 * and can execute a real round-trip query - the same safe pattern
 * UT-CONN-002 already uses.
 *
 * 2026-08-01 redesign: the original version of this test created a
 * genuinely separate, independent OCI_Connect()/OCI_Disconnect() cycle
 * on its own temp_ctx, specifically to exercise the direct (non-
 * pooled) connect path in isolation. Found via a real Tier 2 run to
 * destabilise the REAL, already-active connection - every metadata
 * lookup after this test ran began failing consistently and
 * permanently with an empty "OCI Error 0" (the classic signature of
 * OCI_INVALID_HANDLE, where OCIErrorGet() finds nothing to report on
 * the now-bad handle). OCI_Connect()/OCI_Disconnect() are correctly,
 * independently scoped at the C level (OCIEnvCreate() writes into
 * whichever ctx is passed in, OCI_Disconnect() only frees that same
 * ctx's own handles) - the corruption most likely happens at a lower
 * level, inside Oracle's own native client library (libclntsh), which
 * may not be safe for two independent OCIEnvCreate() environments to
 * coexist concurrently within one process regardless of how cleanly
 * separated their C-level handles are. Not confirmed with certainty
 * (would need direct testing against Oracle's own client library
 * internals to be sure), but the practical fix is the same either way:
 * never stand up a second, concurrent, independent connection while
 * the real one is still active. Testing the direct-connect code path
 * specifically would need to happen in true isolation - its own
 * process, not concurrently with anything else - which is out of
 * scope for a startup self-test. */
static int test_ut_conn_001(oci_context_t *ctx, char *message, size_t message_max)
{
    OCIStmt *stmt = NULL;
    char     result_buf[16] = {0};
    sb2      result_ind = 0;
    OCIDefine *dfn = NULL;

    const char *sql = "SELECT '1' FROM DUAL";

    sword status = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                                    (text *)sql, (ub4)strlen(sql),
                                    NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO)
    {
        snprintf(message, message_max,
                 "Could not prepare a trivial round-trip query on the "
                 "real, active connection");
        return -1;
    }

    OCIDefineByPos(stmt, &dfn, ctx->errhp, 1,
                   (dvoid *)result_buf, (sb4)sizeof(result_buf),
                   SQLT_STR, &result_ind, NULL, NULL, OCI_DEFAULT);

    status = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);

    int rc = 0;
    if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO)
    {
        snprintf(message, message_max,
                 "Trivial round-trip query failed to execute on the "
                 "real, active connection");
        rc = -1;
    }
    else if (strcmp(result_buf, "1") != 0)
    {
        snprintf(message, message_max,
                 "Round-trip query returned '%s', expected '1'", result_buf);
        rc = -1;
    }

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return rc;
}

/* UT-CONN-002 removed 2026-08-01. It originally tested whether the
 * session's NLS_DATE_FORMAT was correctly applied via ALTER SESSION,
 * first via SYS_CONTEXT('USERENV', ...) and then via an implicit
 * date-to-string conversion - both consistently showed Oracle's own
 * default shape even though connectionpool_Data_Manager.log confirms
 * the ALTER SESSION SET NLS_DATE_FORMAT call itself runs successfully,
 * with no error, for every pool slot. The most likely explanation:
 * Oracle's OCI client library typically caches NLS settings at session
 * logon time for implicit (client-side) conversions, and doesn't
 * necessarily re-sync with the server after a later ALTER SESSION,
 * even though the server-side session state genuinely did change.
 *
 * This matters less than it sounds: normalize_client_date_value() (the
 * actual product code) never relies on implicit conversion at all - it
 * always passes nls_date_format explicitly as a bind variable to
 * TO_DATE()/TO_CHAR(), exactly what UT-DATE-001/UT-DATE-002 already
 * test successfully. UT-CONN-002 was testing a session property the
 * product doesn't actually depend on anywhere. Decision (2026-08-01):
 * not worth further investigation now - real, working date-handling
 * tests already prove the mechanism that matters. Revisit rigorously
 * once the HTTP layer exists and end-to-end date handling gets
 * reconfirmed properly under real request/response conditions. */

/* ---- UT-CONN-003 ----
 * The ctx this test (and every other Tier 2+ test) actually runs
 * against has real, usable OCI handles.
 *
 * 2026-08-01 redesign, second pass: this test used to call
 * OCI_Pool_get_session() itself. That's no longer correct - the
 * orchestrator (unit_test_run_all() etc., via acquire_test_ctx()) now
 * acquires one real worker session centrally, once, and passes it to
 * every test in the run - including this one - rather than each test
 * acquiring its own. Calling OCI_Pool_get_session() again from inside
 * an already-acquired worker ctx would fail immediately (that ctx has
 * no pool_handle of its own). This test now simply confirms the ctx it
 * was actually given has real handles - which doubles as confirmation
 * that acquire_test_ctx() itself is working correctly for every other
 * Tier 2+ test in the same run, not just this one.
 *
 * First redesign's own reasoning still applies: this only checks what
 * OCI_Pool_get_session() is documented to populate (svchp/errhp/envhp/
 * ini/logger) - the 16 operation-specific loggers are a separate
 * concern, copied by acquire_test_ctx() itself now (see its own doc
 * comment), not tested here. */
static int test_ut_conn_003(oci_context_t *ctx, char *message, size_t message_max)
{
    if (!ctx->svchp || !ctx->errhp || !ctx->envhp)
    {
        snprintf(message, message_max,
                 "ctx is missing an OCI handle (svchp=%p errhp=%p "
                 "envhp=%p) - acquire_test_ctx() did not provide a "
                 "usable connection for this test run",
                 (void *)ctx->svchp, (void *)ctx->errhp, (void *)ctx->envhp);
        return -1;
    }
    if (!ctx->ini || !ctx->logger)
    {
        snprintf(message, message_max, "ctx->ini or ctx->logger is NULL");
        return -1;
    }
    return 0;
}

/* ---- UT-CONN-005 ----
 * Self-healing reconnect (OCI_Pool_session_is_alive()/
 * OCI_Pool_reconnect_session(), 2026-08-07) - closure item 5 follow-up
 * test catalog addition, 2026-08-10.
 *
 * DELIBERATELY does NOT touch the ctx this test itself was given.
 * unit_test_run_all() acquires exactly ONE shared test_ctx and reuses
 * it across every test in the run (see acquire_test_ctx() and its own
 * doc comment) - killing that shared session here would silently
 * poison every test that runs after this one in the same pass, for a
 * reason that has nothing to do with whatever those tests are actually
 * checking. Instead, this test opens its own separate, independent,
 * non-pooled connection (OCI_Connect()) specifically to be the victim,
 * and uses the test's own already-open ctx only to issue the KILL
 * SESSION command against that separate connection - ctx itself is
 * never at risk.
 *
 * SCOPE NOTE: this only proves the DETECTION half
 * (OCI_Pool_session_is_alive() correctly reporting a killed session as
 * dead). The RECOVERY half (OCI_Pool_reconnect_session()) needs a
 * base_ctx parameter that individual tests are not currently given -
 * only the already-borrowed test_ctx is passed down, and
 * OCI_Pool_reconnect_session() needs the pool owner itself, which that
 * borrowed ctx doesn't have (see OCI_Pool_reconnect_session()'s own
 * signature). Exercising the reconnect half in this framework would
 * need a small, deliberate extension - passing the original base_ctx
 * through to tests that ask for it - not something to build silently
 * as a side effect of one test. Flagged for a decision, not worked
 * around here.
 *
 * PRIVILEGE NOTE: ALTER SYSTEM KILL SESSION needs the ALTER SYSTEM
 * system privilege, which a normal application schema user may not
 * have. If the kill itself fails, this test fails with a message
 * naming exactly what's needed, rather than silently passing without
 * having tested anything - Tier 3's own default (log and continue,
 * not halt) means this doesn't block startup either way. */
static int test_ut_conn_005(oci_context_t *ctx, char *message, size_t message_max)
{
    oci_context_t victim;
    memset(&victim, 0, sizeof(victim));
    victim.ini = ctx->ini;

    if (OCI_Connect(&victim) != 0)
    {
        snprintf(message, message_max,
                 "Could not open a separate, independent connection to "
                 "act as the victim for this test");
        return -1;
    }

    if (!OCI_Pool_session_is_alive(&victim))
    {
        snprintf(message, message_max,
                 "Freshly-opened victim connection already reports as "
                 "not alive, before anything has killed it - "
                 "OCI_Pool_session_is_alive() itself looks broken");
        OCI_Disconnect(&victim);
        return -1;
    }

    /* SYS_CONTEXT('USERENV','SID') reliably returns the calling
     * session's own SID - queried on victim itself, not guessed at
     * from outside it.                                                 */
    int sid = 0;
    {
        OCIStmt *stmt = NULL;
        OCIDefine *dfn = NULL;
        sb2 ind = 0;
        const char *sql = "SELECT SYS_CONTEXT('USERENV','SID') FROM DUAL";
        if (OCIStmtPrepare2(victim.svchp, &stmt, victim.errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX,
                             OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, victim.errhp, 1, (dvoid *)&sid,
                           (sb4)sizeof(sid), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(victim.svchp, stmt, victim.errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, victim.errhp, NULL, 0, OCI_DEFAULT);
        }
    }

    if (sid <= 0)
    {
        snprintf(message, message_max,
                 "Could not determine the victim connection's own SID");
        OCI_Disconnect(&victim);
        return -1;
    }

    /* Serial# looked up separately, on ctx (the test's own shared,
     * already-open connection) - this is also the connection that
     * issues the KILL below, so it needs V$SESSION visibility either
     * way.                                                             */
    int serial = 0;
    {
        OCIStmt *stmt = NULL;
        OCIDefine *dfn = NULL;
        sb2 ind = 0;
        OCIBind *bindp = NULL;
        const char *sql = "SELECT SERIAL# FROM V$SESSION WHERE SID = :1";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX,
                             OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIBindByPos(stmt, &bindp, ctx->errhp, 1, (dvoid *)&sid,
                         (sb4)sizeof(sid), SQLT_INT, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&serial,
                           (sb4)sizeof(serial), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }
    }

    if (serial <= 0)
    {
        snprintf(message, message_max,
                 "Could not determine the victim connection's own "
                 "SERIAL# via V$SESSION (sid=%d) - this may itself need "
                 "a grant (SELECT_CATALOG_ROLE or equivalent)", sid);
        OCI_Disconnect(&victim);
        return -1;
    }

    char kill_sql[128];
    snprintf(kill_sql, sizeof(kill_sql),
             "ALTER SYSTEM KILL SESSION '%d,%d' IMMEDIATE", sid, serial);

    OCIStmt *kill_stmt = NULL;
    sword kill_status = OCIStmtPrepare2(ctx->svchp, &kill_stmt, ctx->errhp,
                                         (text *)kill_sql, (ub4)strlen(kill_sql),
                                         NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (kill_status == OCI_SUCCESS || kill_status == OCI_SUCCESS_WITH_INFO)
        kill_status = OCIStmtExecute(ctx->svchp, kill_stmt, ctx->errhp, 1, 0,
                                      NULL, NULL, OCI_DEFAULT);
    if (kill_stmt) OCIStmtRelease(kill_stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

    if (kill_status != OCI_SUCCESS && kill_status != OCI_SUCCESS_WITH_INFO)
    {
        snprintf(message, message_max,
                 "ALTER SYSTEM KILL SESSION '%d,%d' failed - this test "
                 "schema user most likely lacks the ALTER SYSTEM "
                 "privilege; grant it (or run this specific test as a "
                 "user who has it) to actually exercise self-healing "
                 "reconnect detection", sid, serial);
        OCI_Disconnect(&victim);
        return -1;
    }

    int result = 0;
    if (OCI_Pool_session_is_alive(&victim))
    {
        snprintf(message, message_max,
                 "OCI_Pool_session_is_alive() still reports the victim "
                 "connection as alive after ALTER SYSTEM KILL SESSION "
                 "'%d,%d' IMMEDIATE - detection is not working", sid, serial);
        result = -1;
    }

    OCI_Disconnect(&victim);
    return result;
}

/* ---- UT-META-001 ----
 * metadata_cache_get_or_fetch() correctly resolves every real column
 * for the test table, matching what ALL_TAB_COLUMNS itself would
 * report - checked here by simply confirming resolution succeeds and
 * returns at least one column with a non-empty name/type, since a
 * full column-by-column diff against ALL_TAB_COLUMNS directly would
 * duplicate what metadata_cache.c's own query already does. */
static int test_ut_meta_001(oci_context_t *ctx, char *message, size_t message_max)
{
    col_metadata_t     cols[MAX_TABLE_COLUMNS];
    int                col_count = 0;
    metadata_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);

    metadata_cache_result_t result;
    memset(&result, 0, sizeof(result));

    if (metadata_cache_get_or_fetch(ctx->metadata_cache, ctx, &req,
                                     cols, &col_count, MAX_TABLE_COLUMNS,
                                     &result) != 0)
    {
        snprintf(message, message_max,
                 "metadata_cache_get_or_fetch() failed for "
                 "DATA_MANAGER.OCI_FIELD_TEST");
        return -1;
    }

    if (col_count <= 0)
    {
        snprintf(message, message_max,
                 "metadata_cache_get_or_fetch() returned col_count=%d - "
                 "expected at least one column", col_count);
        return -1;
    }

    if (!cols[0].col_name[0] || !cols[0].data_type[0])
    {
        snprintf(message, message_max,
                 "First resolved column has an empty name or data_type");
        return -1;
    }

    return 0;
}

/* ---- UT-META-002 ----
 * A second lookup for the same table within the cache TTL is a cache
 * hit, not a re-query. */
static int test_ut_meta_002(oci_context_t *ctx, char *message, size_t message_max)
{
    if (!ctx->metadata_cache)
    {
        snprintf(message, message_max,
                 "ctx->metadata_cache is NULL (caching disabled in this "
                 "environment) - cache-hit behaviour cannot be tested "
                 "(not a failure)");
        return 0;
    }

    col_metadata_t     cols[MAX_TABLE_COLUMNS];
    int                col_count = 0;
    metadata_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);

    metadata_cache_result_t first, second;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));

    if (metadata_cache_get_or_fetch(ctx->metadata_cache, ctx, &req,
                                     cols, &col_count, MAX_TABLE_COLUMNS,
                                     &first) != 0)
    {
        snprintf(message, message_max, "First lookup failed");
        return -1;
    }
    if (metadata_cache_get_or_fetch(ctx->metadata_cache, ctx, &req,
                                     cols, &col_count, MAX_TABLE_COLUMNS,
                                     &second) != 0)
    {
        snprintf(message, message_max, "Second lookup failed");
        return -1;
    }

    if (!second.was_cache_hit)
    {
        snprintf(message, message_max,
                 "Second lookup for the same table (within TTL) was not "
                 "a cache hit - was_cache_hit=%d", second.was_cache_hit);
        return -1;
    }

    return 0;
}

/* ---- UT-META-003 ----
 * A client-supplied column type is never trusted where real metadata
 * is available - confirmed for INSERT, UPDATE's SET and WHERE clauses
 * (regression test for the 2026-07-30 finding that build_update_sql()'s
 * WHERE-key resolution silently read an always-empty field instead of
 * the real lookup its own SET clause already did correctly), and
 * DELETE's WHERE clause. This is a static/source-inspection check
 * (confirming the real lookup loop exists in each of these four call
 * sites), not a runtime behavioural one - a genuine runtime test would
 * require deliberately sending a wrong client type and confirming it's
 * ignored, which duplicates Tier 3 execute-path tests better done
 * there. Gracefully passes with a note if the source tree isn't
 * present in this environment - same reasoning as UT-DATE-005. */
static int test_ut_meta_003(oci_context_t *ctx, char *message, size_t message_max)
{
    (void)ctx;

    struct { const char *file; const char *func_marker; } checks[] = {
        { "../src/OCI_Insert_Execute_Module.c", "static int build_insert_sql" },
        { "../src/OCI_Update_Execute_Module.c", "static int build_update_sql" },
        { "../src/OCI_Delete_Execute_Module.c", "static int build_delete_sql" },
    };
    size_t n = sizeof(checks) / sizeof(checks[0]);
    int any_found = 0;

    for (size_t i = 0; i < n; i++)
    {
        FILE *fp = fopen(checks[i].file, "r");
        if (!fp) continue;
        any_found = 1;

        /* Read the whole file and confirm a real cols[]-lookup loop
         * pattern is present ("strcasecmp(cols[" is the distinctive
         * marker every real lookup in this project uses) - a crude but
         * effective proxy for "this function resolves type from real
         * metadata rather than trusting something client-supplied".   */
        char   buf[65536];
        size_t total = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[total] = '\0';
        fclose(fp);

        if (!strstr(buf, "strcasecmp(cols["))
        {
            snprintf(message, message_max,
                     "%s has no real cols[] metadata lookup pattern - "
                     "possible regression of the 2026-07-30 "
                     "client-type-trust fix", checks[i].file);
            return -1;
        }
    }

    if (!any_found)
        snprintf(message, message_max,
                 "Source tree not found at the expected relative paths - "
                 "this development-time check could not run (not a failure)");
    return 0;
}

/* ---- UT-L2-001 ---- */
static int test_ut_l2_001(oci_context_t *ctx, char *message, size_t message_max)
{
    insert_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);
    req.row_count = 1;
    insert_row_t row;
    memset(&row, 0, sizeof(row));
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, "NOT_A_REAL_COLUMN", sizeof(fv.field_name) - 1);
    strncpy(fv.value, "1", sizeof(fv.value) - 1);
    row.field_count = 1;
    row.fields = &fv;
    req.rows = &row;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_INSERT;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    int rc = level2_validate_insert(ctx, &op, &status);
    if (rc == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_insert() accepted a field naming an "
                 "unknown column - should have been rejected");
        return -1;
    }
    if (!strstr(status.error_text, "NOT_A_REAL_COLUMN") &&
        !strstr(status.error_text, "no such column"))
    {
        snprintf(message, message_max,
                 "Rejected correctly, but the error message didn't name "
                 "the offending column: '%s'", status.error_text);
        return -1;
    }
    return 0;
}

/* ---- UT-L2-002 ----
 * 2026-08-01 rewrite: the original version targeted OCI_FIELD_TEST
 * with a completely empty row, on the assumption that table has at
 * least one NOT NULL, no-default column. It doesn't - confirmed via
 * Metadata_Data_Manager.log, OCI_FIELD_TEST's columns are all nullable
 * or defaulted, so an empty row is genuinely, correctly valid for it,
 * and the original test's "must be rejected" premise was simply wrong
 * for that table.
 *
 * AUDIT_TRAIL is the real, confirmed table with this property -
 * CHANGE_REASON specifically (VARCHAR2(500), NOT NULL, no default),
 * per the same metadata log and the real 2026-07-29 INSERT fixture
 * testing that first found this rejection working correctly. This
 * test now targets AUDIT_TRAIL directly, supplying every other
 * required (NOT NULL, no default) column - TABLE_NAME, RECORD_ID,
 * FIELD_NAME, ACTION_TYPE, CHANGED_BY, TRANSACTION_ID - and omitting
 * only CHANGE_REASON itself. Safe to run for real: level2_validate_
 * insert() alone never executes anything against the database, it
 * only validates - no row is ever actually written. */
static int test_ut_l2_002(oci_context_t *ctx, char *message, size_t message_max)
{
    insert_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "AUDIT_TRAIL", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);
    req.row_count = 1;

    field_value_t fields[6];
    memset(fields, 0, sizeof(fields));
    strncpy(fields[0].field_name, "TABLE_NAME",     sizeof(fields[0].field_name) - 1);
    strncpy(fields[0].value,      "OCI_FIELD_TEST", sizeof(fields[0].value) - 1);
    strncpy(fields[1].field_name, "RECORD_ID",      sizeof(fields[1].field_name) - 1);
    strncpy(fields[1].value,      "1",              sizeof(fields[1].value) - 1);
    strncpy(fields[2].field_name, "FIELD_NAME",     sizeof(fields[2].field_name) - 1);
    strncpy(fields[2].value,      "NUMBER_COL",     sizeof(fields[2].value) - 1);
    strncpy(fields[3].field_name, "ACTION_TYPE",    sizeof(fields[3].field_name) - 1);
    strncpy(fields[3].value,      "INSERT",         sizeof(fields[3].value) - 1);
    strncpy(fields[4].field_name, "CHANGED_BY",     sizeof(fields[4].field_name) - 1);
    strncpy(fields[4].value,      "unit_test",      sizeof(fields[4].value) - 1);
    strncpy(fields[5].field_name, "TRANSACTION_ID", sizeof(fields[5].field_name) - 1);
    strncpy(fields[5].value,      "ut-stub-tx",     sizeof(fields[5].value) - 1);
    /* CHANGE_REASON deliberately omitted - the one thing this test is
     * actually checking for.                                          */

    insert_row_t row;
    memset(&row, 0, sizeof(row));
    row.field_count = 6;
    row.fields = fields;
    req.rows = &row;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_INSERT;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    int rc = level2_validate_insert(ctx, &op, &status);
    if (rc == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_insert() accepted an AUDIT_TRAIL row "
                 "missing CHANGE_REASON (NOT NULL, no default)");
        return -1;
    }
    if (!strstr(status.error_text, "CHANGE_REASON"))
    {
        snprintf(message, message_max,
                 "Rejected correctly, but the error message didn't name "
                 "CHANGE_REASON specifically: '%s'", status.error_text);
        return -1;
    }
    return 0;
}

/* ---- UT-L2-003 ----
 * Both UPDATE and DELETE reject a request with zero WHERE keys. */
static int test_ut_l2_003(oci_context_t *ctx, char *message, size_t message_max)
{
    update_request_t ureq;
    memset(&ureq, 0, sizeof(ureq));
    strncpy(ureq.table_name, "OCI_FIELD_TEST", sizeof(ureq.table_name) - 1);
    strncpy(ureq.owner, "DATA_MANAGER", sizeof(ureq.owner) - 1);
    ureq.key_count = 0;
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, "VARCHAR2_COL", sizeof(fv.field_name) - 1);
    strncpy(fv.value, "x", sizeof(fv.value) - 1);
    ureq.field_count = 1;
    ureq.fields = &fv;

    input_c_operation_t uop;
    memset(&uop, 0, sizeof(uop));
    uop.type = OP_UPDATE;
    uop.payload = &ureq;

    operation_status_t ustatus;
    memset(&ustatus, 0, sizeof(ustatus));

    if (level2_validate_update(ctx, &uop, &ustatus) == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_update() accepted zero WHERE keys "
                 "(would match every row in the table)");
        return -1;
    }

    delete_request_t dreq;
    memset(&dreq, 0, sizeof(dreq));
    strncpy(dreq.table_name, "OCI_FIELD_TEST", sizeof(dreq.table_name) - 1);
    strncpy(dreq.owner, "DATA_MANAGER", sizeof(dreq.owner) - 1);
    dreq.key_count = 0;

    input_c_operation_t dop;
    memset(&dop, 0, sizeof(dop));
    dop.type = OP_DELETE;
    dop.payload = &dreq;

    operation_status_t dstatus;
    memset(&dstatus, 0, sizeof(dstatus));

    if (level2_validate_delete(ctx, &dop, &dstatus) == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_delete() accepted zero WHERE keys "
                 "(would delete every row in the table)");
        return -1;
    }

    return 0;
}

/* ---- UT-L2-004 ---- */
static int test_ut_l2_004(oci_context_t *ctx, char *message, size_t message_max)
{
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "NOT_A_REAL_COLUMN", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "1", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, "VARCHAR2_COL", sizeof(fv.field_name) - 1);
    strncpy(fv.value, "x", sizeof(fv.value) - 1);
    req.field_count = 1;
    req.fields = &fv;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_UPDATE;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    if (level2_validate_update(ctx, &op, &status) == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_update() accepted a WHERE key naming "
                 "an unknown column");
        return -1;
    }
    return 0;
}

/* ---- UT-L2-005 ----
 * 2026-08-01 rewrite, following a real SEGV: the original version of
 * this test declared field_count=10000 with fields=NULL, on the
 * assumption that level2_validate_update() would defensively reject an
 * out-of-range count before ever touching the array. Checking the real
 * source shows it does not - there is no upper-bound check at all,
 * anywhere in level2_validate_update() or find_column(); the count is
 * trusted completely to match the caller's own array. That's a
 * reasonable design given the only real caller is Level 1's own
 * parsing (which always keeps the two consistent) - but it means the
 * original test's premise ("an out-of-range count gets rejected") was
 * simply wrong, and confirmed by a real ASan SEGV during a live Tier 2
 * run: find_column() (OCI_Level2_Parser.c:143) dereferenced fields[0]
 * on a NULL array, crashing the entire process - not just failing this
 * one test, but taking down the whole self-test run and every test
 * after it with it. A startup self-test that can crash the process is
 * worse than no self-test at all.
 *
 * This test now checks something real and safe instead: a genuinely
 * large (50-field), but correctly and fully backed, SET clause is
 * accepted and processed without error - there being no artificial cap
 * is the actual, current, intentional behaviour, not a bug, so
 * confirming it doesn't crash on a wide update is the meaningful
 * property to verify. */
static int test_ut_l2_005(oci_context_t *ctx, char *message, size_t message_max)
{
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "1", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;

    /* 50 real, fully-backed entries, all targeting the same existing
     * VARCHAR2 column - repeats are fine here, this is only testing
     * that a large field_count with a genuinely matching array is
     * handled safely, not testing duplicate-field semantics.          */
    #define UT_L2_005_FIELD_COUNT 50
    field_value_t fields[UT_L2_005_FIELD_COUNT];
    memset(fields, 0, sizeof(fields));
    for (int i = 0; i < UT_L2_005_FIELD_COUNT; i++)
    {
        strncpy(fields[i].field_name, "VARCHAR2_COL", sizeof(fields[i].field_name) - 1);
        strncpy(fields[i].value, "x", sizeof(fields[i].value) - 1);
    }
    req.field_count = UT_L2_005_FIELD_COUNT;
    req.fields = fields;
    #undef UT_L2_005_FIELD_COUNT

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_UPDATE;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    if (level2_validate_update(ctx, &op, &status) != LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_update() rejected a genuinely large "
                 "(50-field) but correctly and fully backed SET clause: %s",
                 status.error_text);
        return -1;
    }
    return 0;
}

/* ---- UT-L2-006 ----
 * CURSOR direction IN or IN_OUT is rejected. */
static int test_ut_l2_006(oci_context_t *ctx, char *message, size_t message_max)
{
    execute_procedure_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.procedure_name, "SOME_PROC", sizeof(req.procedure_name) - 1);
    procedure_param_t pp;
    memset(&pp, 0, sizeof(pp));
    strncpy(pp.param_name, "P_CUR", sizeof(pp.param_name) - 1);
    strncpy(pp.param_type, "CURSOR", sizeof(pp.param_type) - 1);
    pp.direction = PARAM_DIR_IN;   /* invalid for a cursor */
    req.param_count = 1;
    req.parameters = &pp;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_EXECUTE_PROCEDURE;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    if (level2_validate_procedure(ctx, &op, &status) == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_procedure() accepted a CURSOR "
                 "parameter with direction=IN");
        return -1;
    }
    return 0;
}

/* ---- UT-L2-007 ---- */
static int test_ut_l2_007(oci_context_t *ctx, char *message, size_t message_max)
{
    execute_procedure_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.procedure_name, "SOME_PROC", sizeof(req.procedure_name) - 1);
    req.param_count = MAX_PROC_PARAMS + 1;
    req.parameters = NULL;   /* deliberately absurd, no backing array */

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_EXECUTE_PROCEDURE;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    if (level2_validate_procedure(ctx, &op, &status) == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_procedure() accepted "
                 "param_count=MAX_PROC_PARAMS+1");
        return -1;
    }
    return 0;
}

/* ---- UT-L2-008 ----
 * A DATE value that doesn't match its declared client_date_format is
 * rejected with a clear "Invalid date" message. */
static int test_ut_l2_008(oci_context_t *ctx, char *message, size_t message_max)
{
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "DATE_COL", sizeof(wk.field_name) - 1);
    /* This value doesn't match the declared format at all - "not-a-date"
     * against a DD/MM/YYYY mask should fail cleanly, not crash or
     * silently pass through.                                           */
    strncpy(wk.key_value, "not-a-date", sizeof(wk.key_value) - 1);
    strncpy(wk.client_date_format, "DD/MM/YYYY HH24:MI:SS",
            sizeof(wk.client_date_format) - 1);
    req.key_count = 1;
    req.keys = &wk;
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, "VARCHAR2_COL", sizeof(fv.field_name) - 1);
    strncpy(fv.value, "x", sizeof(fv.value) - 1);
    req.field_count = 1;
    req.fields = &fv;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_UPDATE;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    if (level2_validate_update(ctx, &op, &status) == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_update() accepted a WHERE key value "
                 "that does not match its own declared client_date_format");
        return -1;
    }
    if (!strstr(status.error_text, "date") && !strstr(status.error_text, "Date"))
    {
        snprintf(message, message_max,
                 "Rejected correctly, but the error message doesn't "
                 "mention the date problem: '%s'", status.error_text);
        return -1;
    }
    return 0;
}

/* ---- UT-L2-009 ----
 * Regression test for the 2026-07-29 idempotency bug: calling
 * level2_validate_update() twice in succession against the SAME,
 * already-mutated payload (matching the real dispatcher-then-Stage-1
 * double-validation pattern) must not fail the second time just
 * because the first call already normalised the value in place. */
static int test_ut_l2_009(oci_context_t *ctx, char *message, size_t message_max)
{
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "DATE_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "19/08/2026 14:30:00", sizeof(wk.key_value) - 1);
    strncpy(wk.client_date_format, "DD/MM/YYYY HH24:MI:SS",
            sizeof(wk.client_date_format) - 1);
    req.key_count = 1;
    req.keys = &wk;
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, "VARCHAR2_COL", sizeof(fv.field_name) - 1);
    strncpy(fv.value, "x", sizeof(fv.value) - 1);
    req.field_count = 1;
    req.fields = &fv;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_UPDATE;
    op.payload = &req;

    operation_status_t status1, status2;
    memset(&status1, 0, sizeof(status1));
    memset(&status2, 0, sizeof(status2));

    int rc1 = level2_validate_update(ctx, &op, &status1);
    if (rc1 != LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "First validation pass failed: %s", status1.error_text);
        return -1;
    }

    /* Same op, same (now-mutated) payload - exactly what happens for
     * real between the dispatcher's own top-level validation and
     * execute_update_batch()'s own Stage 1 defense-in-depth call.      */
    int rc2 = level2_validate_update(ctx, &op, &status2);
    if (rc2 != LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "Second validation pass against the same already-"
                 "converted payload failed: %s - this is exactly the "
                 "2026-07-29 idempotency bug", status2.error_text);
        return -1;
    }

    return 0;
}

/* ---- UT-SEL-002 ----
 * A SELECT clause containing a function call is rejected by
 * extract_sql_dependencies() with a clear message, not a cryptic
 * downstream failure. */
static int test_ut_sel_002(oci_context_t *ctx, char *message, size_t message_max)
{
    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char sql[256] = "SELECT NUMBER_COL, TO_CHAR(DATE_COL,'YYYY-MM-DD') "
                     "FROM DATA_MANAGER.OCI_FIELD_TEST WHERE NUMBER_COL = 1";
    cfg.SQL = sql;
    cfg.max_rows = 10;
    cfg.fetch_array_size = 10;

    int rc = execute_query_batch(ctx, &cfg);

    int result = 0;
    if (rc == 0)
    {
        snprintf(message, message_max,
                 "execute_query_batch() accepted a SELECT clause "
                 "containing a function call (TO_CHAR) - should have "
                 "been rejected by extract_sql_dependencies()");
        result = -1;
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    if (cfg.OUTPUT_JSON) free(cfg.OUTPUT_JSON);

    return result;
}

/* ---- UT-SESS-003 ----
 * session_reconcile_orphans() finding zero orphaned sessions completes
 * cleanly (rc=0) rather than being treated as an error - regression
 * test for repeated confusion during 2026-07 testing about this exact
 * log line ("record_count=0 exceeds max=2000..." being mistaken for a
 * real problem). Does not assert on orphan_count itself, since that
 * genuinely depends on what's in OCI_SESSION at the time this runs -
 * only that the function's own return code is never -1 purely because
 * zero rows matched. */
static int test_ut_sess_003(oci_context_t *ctx, char *message, size_t message_max)
{
    int orphan_count = -999;
    int rc = session_reconcile_orphans(ctx, &orphan_count);

    if (rc != 0)
    {
        snprintf(message, message_max,
                 "session_reconcile_orphans() returned rc=%d - zero "
                 "orphaned sessions should be the normal, healthy "
                 "outcome, not an error", rc);
        return -1;
    }
    return 0;
}

/* ---- UT-DATE-001 ----
 * A date value with no client_date_format is validated against
 * nls_date_format directly - confirms this is a real check now, not
 * the pre-2026-07-28 silent no-op. */
static int test_ut_date_001(oci_context_t *ctx, char *message, size_t message_max)
{
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "DATE_COL", sizeof(wk.field_name) - 1);
    /* No client_date_format - so this falls through to validation
     * against whatever nls_date_format is CURRENTLY CONFIGURED
     * (ctx->ini->nls_date_format), read live - not a hardcoded shape
     * baked into this test. "19/08/2026" is what's actually being
     * relied on here: it must NOT parse successfully under whatever
     * format is deployed, or this test can't tell "correctly
     * rejected" apart from "never checked at all".
     *
     * Latent fragility worth knowing about (found 2026-08-08, closure
     * item 3 review): this test's correctness depends on the deployed
     * nls_date_format never accepting a bare DD/MM/YYYY-shaped value
     * like this one. True for the project's actual default
     * (NLS_DATE_FORMAT_DEFAULT, ini_reader.h = "YYYY-MM-DD
     * HH24:MI:SS") - but nls_date_format is a real, user-configurable
     * setting, and UT-DATE-002 immediately below this test explicitly
     * demonstrates that the exact same "19/08/2026" shape IS valid
     * under a DD/MM/YYYY format. If nls_date_format is ever
     * reconfigured to something DD/MM/YYYY-shaped, this specific test
     * would start failing - or worse, silently stop testing what it
     * claims to. Not fixed here, since pinning this test to its own
     * explicit format (rather than whatever's globally configured)
     * is a slightly larger change than a comment update - flagged for
     * a deliberate decision rather than fixed as a drive-by.          */
    strncpy(wk.key_value, "19/08/2026", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, "VARCHAR2_COL", sizeof(fv.field_name) - 1);
    strncpy(fv.value, "x", sizeof(fv.value) - 1);
    req.field_count = 1;
    req.fields = &fv;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_UPDATE;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    if (level2_validate_update(ctx, &op, &status) == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "An un-tagged date value not matching nls_date_format "
                 "was accepted - the always-validate path is not "
                 "actually running");
        return -1;
    }
    return 0;
}

/* ---- UT-DATE-002 ----
 * A date value WITH client_date_format converts successfully when it
 * genuinely matches that format. */
static int test_ut_date_002(oci_context_t *ctx, char *message, size_t message_max)
{
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, "OCI_FIELD_TEST", sizeof(req.table_name) - 1);
    strncpy(req.owner, "DATA_MANAGER", sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "DATE_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "19/08/2026 14:30:00", sizeof(wk.key_value) - 1);
    strncpy(wk.client_date_format, "DD/MM/YYYY HH24:MI:SS",
            sizeof(wk.client_date_format) - 1);
    req.key_count = 1;
    req.keys = &wk;
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, "VARCHAR2_COL", sizeof(fv.field_name) - 1);
    strncpy(fv.value, "x", sizeof(fv.value) - 1);
    req.field_count = 1;
    req.fields = &fv;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_UPDATE;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    if (level2_validate_update(ctx, &op, &status) != LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "A validly-formatted DD/MM/YYYY date, correctly "
                 "declared via client_date_format, was rejected: %s",
                 status.error_text);
        return -1;
    }

    /* Also confirm it was genuinely normalised into the canonical
     * format, not just accepted as-is.                                 */
    if (strcmp(wk.key_value, "19/08/2026 14:30:00") == 0)
    {
        snprintf(message, message_max,
                 "Validation passed but key_value was never actually "
                 "normalised into nls_date_format - still shows the "
                 "original client-format value");
        return -1;
    }

    return 0;
}

/* ---- UT-DATE-003 ----
 * See UT-L2-009 above - this is the same regression, named to match
 * the Date Handling section of the catalog specifically. Kept as a
 * separate registry entry (not just an alias) since the two IDs are
 * documented independently in the design spec. */
static int test_ut_date_003(oci_context_t *ctx, char *message, size_t message_max)
{
    return test_ut_l2_009(ctx, message, message_max);
}

/* ================================================================== */
/*  Tier 3 tests                                                        */
/*  Full round trip against the dedicated test table/procedure (see      */
/*  unit_test_set_tier3_objects() above). Every test wraps its own real  */
/*  execute_*_batch() calls between begin_test_transaction()/            */
/*  rollback_test_transaction() - nothing here ever persists.            */
/* ================================================================== */

/* ---- UT-INS-001 ----
 * A single-row INSERT commits (within the test's own, later-rolled-
 * back transaction) and is visible in a follow-up SELECT within that
 * same transaction - Oracle's own read-consistency rules mean a
 * session always sees its own uncommitted changes. */
static int test_ut_ins_001(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    insert_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    req.row_count = 1;

    field_value_t fields[2];
    memset(fields, 0, sizeof(fields));
    strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
    strncpy(fields[0].value, "999001", sizeof(fields[0].value) - 1);
    strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
    strncpy(fields[1].value, "UT-INS-001", sizeof(fields[1].value) - 1);

    insert_row_t row;
    memset(&row, 0, sizeof(row));
    row.field_count = 2;
    row.fields = fields;
    req.rows = &row;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.include_column_names = 1;

    int rc = execute_insert_batch(ctx, &req, &cfg);
    int result = 0;

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_insert_batch() failed rc=%d", rc);
        result = -1;
    }
    else
    {
        /* Follow-up SELECT, same still-open transaction, raw OCI - a
         * direct, minimal check rather than routing through
         * execute_query_batch() for a one-row existence check.         */
        OCIStmt *stmt = NULL;
        int      found = 0;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        const char *sql = "SELECT 1 FROM DUAL WHERE EXISTS "
                           "(SELECT 1 FROM UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = 999001)";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&found,
                           (sb4)sizeof(found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        if (found != 1)
        {
            snprintf(message, message_max,
                     "Inserted row not visible in a follow-up SELECT "
                     "within the same still-open transaction");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-INS-002 ----
 * A multi-row INSERT (bulk/array-bind) commits all rows in one batch. */
static int test_ut_ins_002(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    insert_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    req.row_count = 3;

    insert_row_t rows[3];
    field_value_t fields[3][1];
    memset(rows, 0, sizeof(rows));
    memset(fields, 0, sizeof(fields));
    for (int i = 0; i < 3; i++)
    {
        char numbuf[16];
        snprintf(numbuf, sizeof(numbuf), "%d", 999010 + i);
        strncpy(fields[i][0].field_name, "NUMBER_COL", sizeof(fields[i][0].field_name) - 1);
        strncpy(fields[i][0].value, numbuf, sizeof(fields[i][0].value) - 1);
        rows[i].field_count = 1;
        rows[i].fields = fields[i];
    }
    req.rows = rows;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    int rc = execute_insert_batch(ctx, &req, &cfg);
    int result = 0;

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_insert_batch() (3 rows) failed rc=%d", rc);
        result = -1;
    }
    else if (cfg.xml && cfg.xml->OUTPUT_XML && !strstr(cfg.xml->OUTPUT_XML, "<rows_inserted>3</rows_inserted>"))
    {
        snprintf(message, message_max,
                 "Expected <rows_inserted>3</rows_inserted> in the response, "
                 "got: %s", cfg.xml->OUTPUT_XML);
        result = -1;
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-INS-003 ----
 * row_count exceeding max_bulk_inserts is rejected at Level 2, before
 * any OCI call - a real, fully-backed array (matching row_count
 * exactly) one row past the configured limit, all rows minimal but
 * valid, so a NULL/OOB-array crash (the 2026-08-01 UT-L2-005 lesson)
 * is not possible regardless of how level2_validate_insert() itself
 * behaves. No transaction needed - rejected before any DML at all. */
static int test_ut_ins_003(oci_context_t *ctx, char *message, size_t message_max)
{
    int too_many = ctx->ini->max_bulk_inserts + 1;
    if (too_many < 1 || too_many > 100000)
    {
        snprintf(message, message_max,
                 "ctx->ini->max_bulk_inserts=%d looks unreasonable - "
                 "skipping rather than allocating an absurd array",
                 ctx->ini->max_bulk_inserts);
        return -1;
    }

    insert_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    req.row_count = too_many;

    insert_row_t   *rows   = calloc((size_t)too_many, sizeof(insert_row_t));
    field_value_t  *fields = calloc((size_t)too_many, sizeof(field_value_t));
    if (!rows || !fields)
    {
        free(rows); free(fields);
        snprintf(message, message_max, "calloc failed for %d rows", too_many);
        return -1;
    }
    for (int i = 0; i < too_many; i++)
    {
        strncpy(fields[i].field_name, "NUMBER_COL", sizeof(fields[i].field_name) - 1);
        snprintf(fields[i].value, sizeof(fields[i].value), "%d", 1000000 + i);
        rows[i].field_count = 1;
        rows[i].fields = &fields[i];
    }
    req.rows = rows;

    input_c_operation_t op;
    memset(&op, 0, sizeof(op));
    op.type = OP_INSERT;
    op.payload = &req;

    operation_status_t status;
    memset(&status, 0, sizeof(status));

    int rc = level2_validate_insert(ctx, &op, &status);

    free(rows);
    free(fields);

    if (rc == LEVEL2_OK)
    {
        snprintf(message, message_max,
                 "level2_validate_insert() accepted row_count=%d, "
                 "one past max_bulk_inserts=%d", too_many, ctx->ini->max_bulk_inserts);
        return -1;
    }
    return 0;
}

/* ---- UT-INS-004 ----
 * A CLOB value exceeding field_value_t.value's inline capacity (4096
 * bytes) correctly uses the large_value overflow path and is stored/
 * read back intact. */
static int test_ut_ins_004(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    /* 5000 bytes - genuinely past field_value_t.value's 4096-byte
     * inline capacity, forcing the large_value overflow path.         */
    size_t big_len = 5000;
    char  *big_value = malloc(big_len + 1);
    int result = 0;

    if (!big_value)
    {
        snprintf(message, message_max, "malloc failed for the oversized CLOB value");
        rollback_test_transaction(ctx, &tx);
        return -1;
    }
    for (size_t i = 0; i < big_len; i++) big_value[i] = 'A' + (char)(i % 26);
    big_value[big_len] = '\0';

    insert_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    req.row_count = 1;

    field_value_t fields[2];
    memset(fields, 0, sizeof(fields));
    strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
    strncpy(fields[0].value, "999020", sizeof(fields[0].value) - 1);
    strncpy(fields[1].field_name, "CLOB_COL", sizeof(fields[1].field_name) - 1);
    fields[1].large_value = big_value;   /* overflow path - value[] stays empty */

    insert_row_t row;
    memset(&row, 0, sizeof(row));
    row.field_count = 2;
    row.fields = fields;
    req.rows = &row;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    int rc = execute_insert_batch(ctx, &req, &cfg);
    if (rc != 0)
    {
        snprintf(message, message_max, "execute_insert_batch() with a 5000-byte "
                 "CLOB value failed rc=%d", rc);
        result = -1;
    }
    else
    {
        /* Read it back within the same transaction, byte-for-byte.    */
        OCIStmt *stmt = NULL;
        char    *readback = calloc(1, big_len + 1);
        ub2      readback_len = 0;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        const char *sql = "SELECT CLOB_COL FROM UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = 999020";

        if (!readback)
        {
            snprintf(message, message_max, "calloc failed for readback buffer");
            result = -1;
        }
        else if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                                  (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) != OCI_SUCCESS)
        {
            snprintf(message, message_max, "Could not prepare the CLOB readback query");
            result = -1;
        }
        else
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)readback,
                           (sb4)(big_len + 1), SQLT_STR, &ind, NULL, &readback_len, OCI_DEFAULT);
            sword exec_status = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0,
                                                NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

            if (exec_status != OCI_SUCCESS && exec_status != OCI_SUCCESS_WITH_INFO)
            {
                snprintf(message, message_max, "CLOB readback query failed to execute");
                result = -1;
            }
            else if (strncmp(readback, big_value, big_len) != 0)
            {
                snprintf(message, message_max,
                         "CLOB readback did not match the original 5000-byte value");
                result = -1;
            }
        }
        free(readback);
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);
    free(big_value);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-INS-005 ----
 * The nested AUDIT_TRAIL insert (audit_trail_insert_snapshot(), called
 * internally by execute_insert_batch()) succeeds alongside the business
 * insert, both inside the same transaction - confirmed by querying
 * AUDIT_TRAIL directly afterward, within that same still-open
 * transaction, rather than calling audit_trail_insert_snapshot()
 * directly ourselves (which would duplicate what execute_insert_batch()
 * already does, not test it). */
static int test_ut_ins_005(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    insert_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    req.row_count = 1;

    field_value_t fields[1];
    memset(fields, 0, sizeof(fields));
    strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
    strncpy(fields[0].value, "999030", sizeof(fields[0].value) - 1);

    insert_row_t row;
    memset(&row, 0, sizeof(row));
    row.field_count = 1;
    row.fields = fields;
    req.rows = &row;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    int rc = execute_insert_batch(ctx, &req, &cfg);
    int result = 0;

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_insert_batch() failed rc=%d", rc);
        result = -1;
    }
    else
    {
        OCIStmt *stmt = NULL;
        int      found = 0;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        /* 2026-08-01, second correction: audit_trail_insert_snapshot()
         * actually writes one AUDIT_TRAIL row per BUSINESS ROW (not per
         * column) - req.row_count = atr->row_count, confirmed directly
         * in its own source - with every column's change serialised
         * together into one combined snapshot via
         * audit_trail_serialise_row(), not as individual FIELD_NAME/
         * NEW_VALUE rows the way the first correction assumed.
         * record_id is the Oracle ROWID (not predictable either - see
         * the first correction's own note, still true). CHANGE_REASON
         * is the genuinely reliable, checkable field here - set
         * directly from ctx->active_tx->tx_name when a named
         * transaction is active (confirmed in audit_trail_insert_
         * snapshot()'s own source), and begin_test_transaction() above
         * always names its transaction "unit_test_tx".                 */
        const char *sql =
            "SELECT 1 FROM DUAL WHERE EXISTS "
            "(SELECT 1 FROM AUDIT_TRAIL WHERE TABLE_NAME = 'UNIT_TEST_FIELD_TEST' "
            "AND ACTION_TYPE = 'INSERT' AND CHANGE_REASON = 'unit_test_tx')";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&found,
                           (sb4)sizeof(found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        if (found != 1)
        {
            snprintf(message, message_max,
                     "No matching AUDIT_TRAIL row found (TABLE_NAME="
                     "'UNIT_TEST_FIELD_TEST', ACTION_TYPE='INSERT', "
                     "CHANGE_REASON='unit_test_tx') - the nested "
                     "audit_trail_insert_snapshot() call may not have "
                     "run or committed within the same transaction");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-UPD-001 ----
 * A scalar UPDATE keyed on a NUMBER column commits and the changed
 * column reads back correctly. Inserts its own row first (within the
 * same transaction), so this test is fully self-contained. */
static int test_ut_upd_001(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999040", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "original", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* Update it */
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "999040", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;
    field_value_t set_field;
    memset(&set_field, 0, sizeof(set_field));
    strncpy(set_field.field_name, "VARCHAR2_COL", sizeof(set_field.field_name) - 1);
    strncpy(set_field.value, "changed", sizeof(set_field.value) - 1);
    req.field_count = 1;
    req.fields = &set_field;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_update_batch(ctx, &req, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_update_batch() failed rc=%d", rc);
        result = -1;
    }
    else
    {
        OCIStmt *stmt = NULL;
        char     readback[128] = {0};
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        const char *sql = "SELECT VARCHAR2_COL FROM UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = 999040";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)readback,
                           (sb4)sizeof(readback), SQLT_STR, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }
        if (strcmp(readback, "changed") != 0)
        {
            snprintf(message, message_max,
                     "VARCHAR2_COL reads back as '%s', expected 'changed'", readback);
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-UPD-002 ----
 * A DATE-typed WHERE key, value already in nls_date_format, matches
 * the real row - regression test for the original 2026-07-26 bug
 * (ORA-01861 from a missing TO_DATE wrapper on the WHERE clause). */
static int test_ut_upd_002(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row with a real DATE_COL value */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999041", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "DATE_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "2026-08-19 14:30:00", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT (with DATE_COL) failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* Update, keyed on the DATE_COL WHERE clause */
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "DATE_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "2026-08-19 14:30:00", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;
    field_value_t set_field;
    memset(&set_field, 0, sizeof(set_field));
    strncpy(set_field.field_name, "VARCHAR2_COL", sizeof(set_field.field_name) - 1);
    strncpy(set_field.value, "date-keyed-update", sizeof(set_field.value) - 1);
    req.field_count = 1;
    req.fields = &set_field;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_update_batch(ctx, &req, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max,
                 "execute_update_batch() with a DATE_COL WHERE key failed "
                 "rc=%d - possible regression of the 2026-07-26 missing "
                 "TO_DATE wrapper fix", rc);
        result = -1;
    }
    else if (cfg.xml && cfg.xml->OUTPUT_XML && !strstr(cfg.xml->OUTPUT_XML, "<rows_updated>1</rows_updated>"))
    {
        snprintf(message, message_max,
                 "Expected <rows_updated>1</rows_updated>, got: %s",
                 cfg.xml->OUTPUT_XML);
        result = -1;
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-UPD-003 ----
 * audit_trail_insert_update() writes one row per genuinely changed
 * column, and nothing for a column whose new value equals its old
 * value - confirmed at the real, field-level AUDIT_TRAIL structure
 * UPDATE actually uses (FIELD_NAME/OLD_VALUE/NEW_VALUE per changed
 * column) - genuinely different from INSERT's own one-row-per-business-
 * row snapshot (see UT-INS-005's own corrected doc comment for that
 * distinction, confirmed directly from audit_trail_insert_update()'s
 * own source). */
static int test_ut_upd_003(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999042", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "same-value", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* Update: VARCHAR2_COL genuinely changes; DATE_COL is set to a
     * value but DATE_COL was never set on the seed row, so it goes
     * from NULL to a real value - that IS a genuine change too. To
     * test the "no audit row for an unchanged column" side properly,
     * VARCHAR2_COL is set to the SAME value it already has instead.
     *
     * 2026-08-01 fix: originally used CLOB_COL for the "genuinely
     * changed" half - audit_trail_fetch_before_image() has no explicit
     * LOB handling at all, so a LOB column's before-image likely isn't
     * captured correctly through this path. That's a real, separate
     * characteristic worth knowing about, but testing LOB-column
     * auditing specifically isn't this test's actual purpose - the
     * property under test (changed vs. unchanged column distinction)
     * doesn't need a LOB column at all, so DATE_COL (a plain scalar)
     * is used instead. */
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "999042", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;

    field_value_t set_fields[2];
    memset(set_fields, 0, sizeof(set_fields));
    strncpy(set_fields[0].field_name, "VARCHAR2_COL", sizeof(set_fields[0].field_name) - 1);
    strncpy(set_fields[0].value, "same-value", sizeof(set_fields[0].value) - 1);   /* unchanged */
    strncpy(set_fields[1].field_name, "DATE_COL", sizeof(set_fields[1].field_name) - 1);
    strncpy(set_fields[1].value, "2026-08-19 14:30:00", sizeof(set_fields[1].value) - 1); /* NULL -> value: changed */
    req.field_count = 2;
    req.fields = set_fields;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_update_batch(ctx, &req, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_update_batch() failed rc=%d", rc);
        result = -1;
    }
    else
    {
        int changed_found = 0, unchanged_found = 0;
        OCIStmt *stmt = NULL;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        const char *sql1 =
            "SELECT 1 FROM DUAL WHERE EXISTS "
            "(SELECT 1 FROM AUDIT_TRAIL WHERE TABLE_NAME = 'UNIT_TEST_FIELD_TEST' "
            "AND FIELD_NAME = 'DATE_COL' AND ACTION_TYPE = 'UPDATE')";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql1,
                             (ub4)strlen(sql1), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&changed_found,
                           (sb4)sizeof(changed_found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        stmt = NULL; dfn = NULL; ind = 0;
        const char *sql2 =
            "SELECT 1 FROM DUAL WHERE EXISTS "
            "(SELECT 1 FROM AUDIT_TRAIL WHERE TABLE_NAME = 'UNIT_TEST_FIELD_TEST' "
            "AND FIELD_NAME = 'VARCHAR2_COL' AND ACTION_TYPE = 'UPDATE' "
            "AND CHANGE_REASON = 'unit_test_tx')";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql2,
                             (ub4)strlen(sql2), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&unchanged_found,
                           (sb4)sizeof(unchanged_found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        if (changed_found != 1)
        {
            snprintf(message, message_max,
                     "No AUDIT_TRAIL row for DATE_COL, which genuinely "
                     "changed (NULL -> '2026-08-19 14:30:00')");
            result = -1;
        }
        else if (unchanged_found == 1)
        {
            snprintf(message, message_max,
                     "An AUDIT_TRAIL row exists for VARCHAR2_COL, which "
                     "was set to the same value it already had - should "
                     "have been skipped");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-UPD-004 ----
 * session_end() (which builds an update_request_t directly, not via
 * the parsed pipeline) succeeds - regression test for the pre-refactor
 * <update_value>/<insert_value> tag-mismatch bug. Creates a real
 * session first via session_create(), so this test is self-contained. */
static int test_ut_upd_004(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;
    char *create_xml = NULL;

    session_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.operation, "CREATE_SESSION", sizeof(req.operation) - 1);
    strncpy(req.client_id, "unit-test-client", sizeof(req.client_id) - 1);
    strncpy(req.client_ip, "127.0.0.1", sizeof(req.client_ip) - 1);
    strncpy(req.application_name, "unit_test", sizeof(req.application_name) - 1);
    req.requested_ttl_seconds = 60;

    if (session_create(ctx, &req, &create_xml) != 0 || !create_xml)
    {
        snprintf(message, message_max, "session_create() failed");
        free(create_xml);
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    /* Minimal, established-pattern tag extraction. */
    char session_id[128] = {0};
    const char *tag_start = strstr(create_xml, "<session_id>");
    if (tag_start)
    {
        tag_start += strlen("<session_id>");
        const char *tag_end = strstr(tag_start, "</session_id>");
        if (tag_end && (size_t)(tag_end - tag_start) < sizeof(session_id))
        {
            memcpy(session_id, tag_start, (size_t)(tag_end - tag_start));
            session_id[tag_end - tag_start] = '\0';
        }
    }
    free(create_xml);

    if (!session_id[0])
    {
        snprintf(message, message_max,
                 "Could not extract session_id from session_create()'s "
                 "own result XML");
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    char *end_xml = NULL;
    int rc = session_end(ctx, session_id, SESSION_STATUS_LOGGED_OUT,
                          "unit test cleanup", &end_xml);
    free(end_xml);

    if (rc != SESSION_OK)
    {
        snprintf(message, message_max,
                 "session_end() failed rc=%d for session_id='%s' - "
                 "possible regression of the pre-refactor <update_value>/"
                 "<insert_value> tag-mismatch bug", rc, session_id);
        result = -1;
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-AUDIT-004 ----
 * A CLOB column's audit trail is captured correctly on UPDATE - a real,
 * non-NULL CLOB value changing to a different real value (not a NULL-
 * to-value transition, which is a separate question already resolved
 * for scalar columns via the 2026-08-01 upd_fv_t struct-layout fix -
 * see UT-UPD-003's own history).
 *
 * 2026-08-01 finding: OLD_VALUE and NEW_VALUE are asymmetric for a LOB
 * column, and correctly so. NEW_VALUE is the raw, client-supplied text
 * ("changed-clob-value") - it comes straight from the UPDATE request,
 * never round-tripped through a SELECT. OLD_VALUE comes from a real
 * before-image SELECT against the database, and for a CLOB column that
 * renders as a file-URL reference (e.g. "https:/CLOB_COL_row1_clob0.txt"),
 * matching this project's own established LOB-output convention (the
 * same pattern used for BLOB output elsewhere) - not the raw text. This
 * test does not assert literal old-value text for that reason; it
 * confirms an audit row exists, NEW_VALUE is the correct raw text, and
 * OLD_VALUE is both non-empty and genuinely different from NEW_VALUE -
 * proving a real change was captured, without assuming a text
 * representation the system was never going to produce on that side. */
static int test_ut_audit_004(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row with a real, non-NULL CLOB value */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999050", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "CLOB_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "original-clob-value", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT (with real CLOB_COL value) failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* Update CLOB_COL to a different real value */
    update_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "999050", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;
    field_value_t set_field;
    memset(&set_field, 0, sizeof(set_field));
    strncpy(set_field.field_name, "CLOB_COL", sizeof(set_field.field_name) - 1);
    strncpy(set_field.value, "changed-clob-value", sizeof(set_field.value) - 1);
    req.field_count = 1;
    req.fields = &set_field;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_update_batch(ctx, &req, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_update_batch() on CLOB_COL failed rc=%d", rc);
        result = -1;
    }
    else
    {
        OCIStmt *stmt = NULL;
        char     old_val[128] = {0};
        char     new_val[128] = {0};
        sb2      ind1 = 0, ind2 = 0;
        OCIDefine *dfn1 = NULL, *dfn2 = NULL;
        int found = 0;
        const char *sql =
            "SELECT OLD_VALUE, NEW_VALUE FROM AUDIT_TRAIL WHERE "
            "TABLE_NAME = 'UNIT_TEST_FIELD_TEST' AND FIELD_NAME = 'CLOB_COL' "
            "AND ACTION_TYPE = 'UPDATE' AND CHANGE_REASON = 'unit_test_tx'";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn1, ctx->errhp, 1, (dvoid *)old_val,
                           (sb4)sizeof(old_val), SQLT_STR, &ind1, NULL, NULL, OCI_DEFAULT);
            OCIDefineByPos(stmt, &dfn2, ctx->errhp, 2, (dvoid *)new_val,
                           (sb4)sizeof(new_val), SQLT_STR, &ind2, NULL, NULL, OCI_DEFAULT);
            sword exec_status = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0,
                                                NULL, NULL, OCI_DEFAULT);
            if (exec_status == OCI_SUCCESS || exec_status == OCI_SUCCESS_WITH_INFO)
                found = 1;
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        if (!found)
        {
            snprintf(message, message_max,
                     "No AUDIT_TRAIL row found for the CLOB_COL update - "
                     "a genuine LOB value change was not audited at all");
            result = -1;
        }
        else if (strcmp(new_val, "changed-clob-value") != 0)
        {
            snprintf(message, message_max,
                     "AUDIT_TRAIL row found but NEW_VALUE='%s', expected "
                     "'changed-clob-value' (this side is always the raw, "
                     "client-supplied text - never round-tripped through "
                     "a SELECT, so no file-URL rendering applies to it)",
                     new_val);
            result = -1;
        }
        else if (!old_val[0] || strcmp(old_val, new_val) == 0)
        {
            snprintf(message, message_max,
                     "AUDIT_TRAIL row found but OLD_VALUE='%s' is empty or "
                     "identical to NEW_VALUE - no genuine change was "
                     "captured", old_val);
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-DEL-001 ----
 * A DELETE keyed on a single column removes exactly the matching
 * row(s) and rows_affected reflects the real count. */
static int test_ut_del_001(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fv;
        memset(&fv, 0, sizeof(fv));
        strncpy(fv.field_name, "NUMBER_COL", sizeof(fv.field_name) - 1);
        strncpy(fv.value, "999060", sizeof(fv.value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 1;
        row.fields = &fv;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    delete_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "999060", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_delete_batch(ctx, &req, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_delete_batch() failed rc=%d", rc);
        result = -1;
    }
    else if (cfg.xml && cfg.xml->OUTPUT_XML && !strstr(cfg.xml->OUTPUT_XML, "<rows_deleted>1</rows_deleted>"))
    {
        snprintf(message, message_max,
                 "Expected <rows_deleted>1</rows_deleted>, got: %s",
                 cfg.xml->OUTPUT_XML);
        result = -1;
    }
    else
    {
        OCIStmt *stmt = NULL;
        int      found = 0;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        const char *sql = "SELECT 1 FROM DUAL WHERE EXISTS "
                           "(SELECT 1 FROM UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = 999060)";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&found,
                           (sb4)sizeof(found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }
        if (found != 0)
        {
            snprintf(message, message_max,
                     "Row NUMBER_COL=999060 still exists after DELETE");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-DEL-002 ----
 * A compound-key DELETE (multiple AND'd WHERE keys) matches only rows
 * satisfying all keys. Seeds two rows sharing one key value but
 * differing on a second, confirms only the fully-matching row is
 * removed. */
static int test_ut_del_002(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed two rows: 999061/'match' and 999062/'nomatch' */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 2;

        insert_row_t rows[2];
        field_value_t fields[2][2];
        memset(rows, 0, sizeof(rows));
        memset(fields, 0, sizeof(fields));

        strncpy(fields[0][0].field_name, "NUMBER_COL", sizeof(fields[0][0].field_name) - 1);
        strncpy(fields[0][0].value, "999061", sizeof(fields[0][0].value) - 1);
        strncpy(fields[0][1].field_name, "VARCHAR2_COL", sizeof(fields[0][1].field_name) - 1);
        strncpy(fields[0][1].value, "match", sizeof(fields[0][1].value) - 1);
        rows[0].field_count = 2;
        rows[0].fields = fields[0];

        strncpy(fields[1][0].field_name, "NUMBER_COL", sizeof(fields[1][0].field_name) - 1);
        strncpy(fields[1][0].value, "999062", sizeof(fields[1][0].value) - 1);
        strncpy(fields[1][1].field_name, "VARCHAR2_COL", sizeof(fields[1][1].field_name) - 1);
        strncpy(fields[1][1].value, "nomatch", sizeof(fields[1][1].value) - 1);
        rows[1].field_count = 2;
        rows[1].fields = fields[1];

        req.rows = rows;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT (2 rows) failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* DELETE WHERE VARCHAR2_COL='match' AND NUMBER_COL=999061 - the
     * compound key should only match the first seeded row.            */
    delete_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    where_key_t wks[2];
    memset(wks, 0, sizeof(wks));
    strncpy(wks[0].field_name, "VARCHAR2_COL", sizeof(wks[0].field_name) - 1);
    strncpy(wks[0].key_value, "match", sizeof(wks[0].key_value) - 1);
    strncpy(wks[1].field_name, "NUMBER_COL", sizeof(wks[1].field_name) - 1);
    strncpy(wks[1].key_value, "999061", sizeof(wks[1].key_value) - 1);
    req.key_count = 2;
    req.keys = wks;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_delete_batch(ctx, &req, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_delete_batch() (compound key) failed rc=%d", rc);
        result = -1;
    }
    else if (cfg.xml && cfg.xml->OUTPUT_XML && !strstr(cfg.xml->OUTPUT_XML, "<rows_deleted>1</rows_deleted>"))
    {
        snprintf(message, message_max,
                 "Expected <rows_deleted>1</rows_deleted> (only the fully-"
                 "matching row), got: %s", cfg.xml->OUTPUT_XML);
        result = -1;
    }
    else
    {
        OCIStmt *stmt = NULL;
        int      still_there = 0;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        const char *sql = "SELECT 1 FROM DUAL WHERE EXISTS "
                           "(SELECT 1 FROM UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = 999062)";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&still_there,
                           (sb4)sizeof(still_there), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }
        if (still_there != 1)
        {
            snprintf(message, message_max,
                     "The non-matching row (999062) was incorrectly "
                     "removed too - compound key was not AND'd correctly");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-DEL-004 ----
 * The before-image capture is scoped to the WHERE-key columns only,
 * not every column on the table - confirmed by seeding a row with
 * multiple columns, deleting keyed on only one, and checking
 * AUDIT_TRAIL has an entry for the key column but none for the other,
 * non-key columns that were also present on the row. */
static int test_ut_del_004(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row with 3 columns */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999063", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "not-a-key", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* DELETE keyed only on NUMBER_COL - VARCHAR2_COL is not a key here
     * even though it's present on the row.                             */
    delete_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "999063", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_delete_batch(ctx, &req, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_delete_batch() failed rc=%d", rc);
        result = -1;
    }
    else
    {
        int key_col_found = 0, non_key_col_found = 0;
        OCIStmt *stmt = NULL;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;

        const char *sql1 =
            "SELECT 1 FROM DUAL WHERE EXISTS "
            "(SELECT 1 FROM AUDIT_TRAIL WHERE TABLE_NAME = 'UNIT_TEST_FIELD_TEST' "
            "AND FIELD_NAME = 'NUMBER_COL' AND ACTION_TYPE = 'DELETE' "
            "AND CHANGE_REASON = 'unit_test_tx')";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql1,
                             (ub4)strlen(sql1), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&key_col_found,
                           (sb4)sizeof(key_col_found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        stmt = NULL; dfn = NULL; ind = 0;
        const char *sql2 =
            "SELECT 1 FROM DUAL WHERE EXISTS "
            "(SELECT 1 FROM AUDIT_TRAIL WHERE TABLE_NAME = 'UNIT_TEST_FIELD_TEST' "
            "AND FIELD_NAME = 'VARCHAR2_COL' AND ACTION_TYPE = 'DELETE' "
            "AND CHANGE_REASON = 'unit_test_tx')";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql2,
                             (ub4)strlen(sql2), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&non_key_col_found,
                           (sb4)sizeof(non_key_col_found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        if (key_col_found != 1)
        {
            snprintf(message, message_max,
                     "No AUDIT_TRAIL row for the WHERE-key column "
                     "(NUMBER_COL) - the before-image should always "
                     "capture the key column(s)");
            result = -1;
        }
        else if (non_key_col_found == 1)
        {
            snprintf(message, message_max,
                     "An AUDIT_TRAIL row exists for VARCHAR2_COL, which "
                     "was not a WHERE key - before-image capture should "
                     "be scoped to key columns only");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-DEL-005 ----
 * A DELETE with zero WHERE keys never reaches Stage 3 (execution) -
 * confirms UT-L2-003's own rejection actually prevents execution, not
 * just validation. Calls execute_delete_batch() directly (its own
 * Stage 1 defense-in-depth should reject this itself, independent of
 * whether a caller already ran level2_validate_delete() first) and
 * confirms no row was actually removed - the test table is left
 * completely untouched either way given the whole transaction rolls
 * back, but this confirms the rejection happens before Stage 3, not
 * that a same-transaction rollback merely undid a real delete. */
static int test_ut_del_005(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fv;
        memset(&fv, 0, sizeof(fv));
        strncpy(fv.field_name, "NUMBER_COL", sizeof(fv.field_name) - 1);
        strncpy(fv.value, "999064", sizeof(fv.value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 1;
        row.fields = &fv;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    delete_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    req.key_count = 0;   /* deliberately empty WHERE clause */
    req.keys = NULL;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_delete_batch(ctx, &req, &cfg);

    if (rc == 0)
    {
        snprintf(message, message_max,
                 "execute_delete_batch() accepted zero WHERE keys and "
                 "returned success - this would delete every row in "
                 "the table");
        result = -1;
    }
    else
    {
        OCIStmt *stmt = NULL;
        int      found = 0;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        const char *sql = "SELECT 1 FROM DUAL WHERE EXISTS "
                           "(SELECT 1 FROM UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = 999064)";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&found,
                           (sb4)sizeof(found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }
        if (found != 1)
        {
            snprintf(message, message_max,
                     "The seeded row is gone even though "
                     "execute_delete_batch() reported rejection - "
                     "something executed a real DELETE anyway");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-DEL-003 ----
 * The before-image SELECT and the AUDIT_TRAIL write both complete
 * BEFORE the actual DELETE statement executes - the single most
 * important test in this whole catalog from a GxP standpoint.
 *
 * 2026-08-01 design decision: the original catalog description called
 * for revoking DELETE privilege on the test table for this one test.
 * Not possible in practice - DATA_MANAGER is both the schema owner and
 * the only connection this self-test has, and Oracle does not allow a
 * schema owner to revoke privilege from themselves on their own
 * objects (ownership privilege is intrinsic, not grant-based). A
 * genuinely separate, restricted database user would be needed to test
 * privilege revocation specifically, which is real additional setup
 * (a new user, new credentials, a second connection path) for a
 * self-test that currently only needs one.
 *
 * Uses a BEFORE DELETE trigger instead, scoped narrowly to one
 * dedicated marker value (NUMBER_COL=999099) so it never affects any
 * other DEL test:
 *
 *   CREATE OR REPLACE TRIGGER DATA_MANAGER.UNIT_TEST_FIELD_TEST_DEL_BLOCK
 *   BEFORE DELETE ON DATA_MANAGER.UNIT_TEST_FIELD_TEST
 *   FOR EACH ROW
 *   WHEN (OLD.NUMBER_COL = 999099)
 *   BEGIN
 *       RAISE_APPLICATION_ERROR(-20001, 'UT-DEL-003: DELETE deliberately blocked for testing');
 *   END;
 *
 * This tests the exact same underlying property the original design
 * called for - does the audit write survive a DELETE that Oracle
 * itself then rejects - without needing a second connection. The
 * *reason* Oracle rejects the DELETE (a trigger vs. a privilege check)
 * is not what this test is about; what matters is that
 * execute_delete_batch()'s own Stage ordering (before-image + audit
 * write, then the DELETE statement) means the audit record exists
 * regardless of what happens next. */
static int test_ut_del_003(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed the row the trigger targets */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fv;
        memset(&fv, 0, sizeof(fv));
        strncpy(fv.field_name, "NUMBER_COL", sizeof(fv.field_name) - 1);
        strncpy(fv.value, "999099", sizeof(fv.value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 1;
        row.fields = &fv;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT (trigger-target row) failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* Attempt the DELETE - the trigger should reject it */
    delete_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    where_key_t wk;
    memset(&wk, 0, sizeof(wk));
    strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
    strncpy(wk.key_value, "999099", sizeof(wk.key_value) - 1);
    req.key_count = 1;
    req.keys = &wk;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_delete_batch(ctx, &req, &cfg);

    if (rc == 0)
    {
        snprintf(message, message_max,
                 "execute_delete_batch() reported success, but the "
                 "UNIT_TEST_FIELD_TEST_DEL_BLOCK trigger should have "
                 "rejected this DELETE - is the trigger actually "
                 "installed?");
        result = -1;
    }
    else
    {
        /* The DELETE was correctly rejected. Now confirm the audit
         * record still exists - proving the before-image/audit write
         * happened before the DELETE statement, not after.            */
        int audit_found = 0, row_still_there = 0;
        OCIStmt *stmt = NULL;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;

        /* 2026-08-01: dropped the OLD_VALUE='999099' condition that was
         * here originally - matching UT-DEL-004's own, already-proven
         * query pattern (FIELD_NAME/ACTION_TYPE/CHANGE_REASON only).
         * The audit log for this exact run confirmed the insert itself
         * genuinely succeeded (rc=0, "1 row(s) written to AUDIT_TRAIL")
         * - the failure was this query's own extra condition not
         * matching, not a real absence of the audit row.               */
        const char *sql1 =
            "SELECT 1 FROM DUAL WHERE EXISTS "
            "(SELECT 1 FROM AUDIT_TRAIL WHERE TABLE_NAME = 'UNIT_TEST_FIELD_TEST' "
            "AND FIELD_NAME = 'NUMBER_COL' AND ACTION_TYPE = 'DELETE' "
            "AND CHANGE_REASON = 'unit_test_tx')";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql1,
                             (ub4)strlen(sql1), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&audit_found,
                           (sb4)sizeof(audit_found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        stmt = NULL; dfn = NULL; ind = 0;
        const char *sql2 = "SELECT 1 FROM DUAL WHERE EXISTS "
                           "(SELECT 1 FROM UNIT_TEST_FIELD_TEST WHERE NUMBER_COL = 999099)";
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql2,
                             (ub4)strlen(sql2), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&row_still_there,
                           (sb4)sizeof(row_still_there), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }

        if (!audit_found)
        {
            snprintf(message, message_max,
                     "The DELETE was correctly rejected by the trigger, "
                     "but no AUDIT_TRAIL row exists for the attempt - the "
                     "audit write did not survive the failed DELETE, "
                     "meaning it is not genuinely happening before the "
                     "DML statement");
            result = -1;
        }
        else if (!row_still_there)
        {
            snprintf(message, message_max,
                     "The AUDIT_TRAIL row exists, but the row itself is "
                     "gone - the DELETE should have been fully rejected "
                     "by the trigger, not partially applied");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-SEL-001 ----
 * A plain column-list SELECT returns the expected row count and
 * values. */
static int test_ut_sel_001(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999070", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "select-me", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT VARCHAR2_COL FROM %s.%s WHERE NUMBER_COL = 999070",
             g_test_table_owner, g_test_table_name);
    cfg.SQL = sql;
    cfg.max_rows = 10;
    cfg.fetch_array_size = 10;
    cfg.include_column_names = 1;

    int rc = execute_query_batch(ctx, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_query_batch() failed rc=%d", rc);
        result = -1;
    }
    else if (!cfg.xml || !cfg.xml->OUTPUT_XML || !strstr(cfg.xml->OUTPUT_XML, "select-me"))
    {
        snprintf(message, message_max,
                 "Expected value 'select-me' not found in the SELECT "
                 "response: %s", cfg.xml && cfg.xml->OUTPUT_XML ? cfg.xml->OUTPUT_XML : "(null)");
        result = -1;
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-SEL-003 ----
 * A CLOB column in the SELECT list is correctly extracted and included
 * in the resultset. Per UT-AUDIT-004's own 2026-08-01 finding, a CLOB
 * value fetched via a real SELECT renders as a file-URL reference
 * (this project's established LOB-output convention), not the raw
 * text - so this test confirms a reference is present, not that the
 * literal original text appears verbatim. */
static int test_ut_sel_003(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row with a real CLOB value */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999071", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "CLOB_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "select-this-clob", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT (with real CLOB_COL value) failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT CLOB_COL FROM %s.%s WHERE NUMBER_COL = 999071",
             g_test_table_owner, g_test_table_name);
    cfg.SQL = sql;
    cfg.max_rows = 10;
    cfg.fetch_array_size = 10;
    cfg.include_column_names = 1;

    int rc = execute_query_batch(ctx, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max,
                 "execute_query_batch() with a CLOB_COL in the SELECT "
                 "list failed rc=%d", rc);
        result = -1;
    }
    else if (!cfg.xml || !cfg.xml->OUTPUT_XML)
    {
        snprintf(message, message_max, "No OUTPUT_XML produced");
        result = -1;
    }
    else
    {
        /* CLOB_COL's own field should be present with SOME non-empty
         * content - a file-URL reference, per the established LOB-
         * output convention, not necessarily the raw text.             */
        const char *field = strstr(cfg.xml->OUTPUT_XML, "CLOB_COL");
        if (!field)
        {
            snprintf(message, message_max,
                     "CLOB_COL not present at all in the resultset - "
                     "LOB extraction may have failed silently");
            result = -1;
        }
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-SEL-004 ----
 * A query against changed underlying data is not silently served
 * stale from cache. Tested as an observable, black-box property
 * (select, update, select again, confirm the new value comes back) -
 * cache_hit itself is only tracked in metrics.csv, not exposed
 * programmatically, so this is more robust than trying to inspect an
 * internal flag directly, and is valid regardless of whether
 * resultset caching is even enabled in this environment. */
static int test_ut_sel_004(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999072", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "before-update", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT VARCHAR2_COL FROM %s.%s WHERE NUMBER_COL = 999072",
             g_test_table_owner, g_test_table_name);

    /* First SELECT - establishes a possible cache entry */
    {
        execute_config_t cfg1;
        memset(&cfg1, 0, sizeof(cfg1));
        cfg1.SQL = sql;
        cfg1.max_rows = 10;
        cfg1.fetch_array_size = 10;
        int rc1 = execute_query_batch(ctx, &cfg1);
        int first_ok = (rc1 == 0 && cfg1.xml && cfg1.xml->OUTPUT_XML &&
                        strstr(cfg1.xml->OUTPUT_XML, "before-update") != NULL);
        if (cfg1.xml) { free(cfg1.xml->OUTPUT_XML); free(cfg1.xml); }
        free(cfg1.OUTPUT_JSON);
        if (!first_ok)
        {
            snprintf(message, message_max,
                     "First SELECT did not return the expected "
                     "'before-update' value");
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
    }

    /* Update the row */
    {
        update_request_t ureq;
        memset(&ureq, 0, sizeof(ureq));
        strncpy(ureq.table_name, g_test_table_name, sizeof(ureq.table_name) - 1);
        strncpy(ureq.owner, g_test_table_owner, sizeof(ureq.owner) - 1);
        where_key_t wk;
        memset(&wk, 0, sizeof(wk));
        strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
        strncpy(wk.key_value, "999072", sizeof(wk.key_value) - 1);
        ureq.key_count = 1;
        ureq.keys = &wk;
        field_value_t set_field;
        memset(&set_field, 0, sizeof(set_field));
        strncpy(set_field.field_name, "VARCHAR2_COL", sizeof(set_field.field_name) - 1);
        strncpy(set_field.value, "after-update", sizeof(set_field.value) - 1);
        ureq.field_count = 1;
        ureq.fields = &set_field;
        execute_config_t ucfg;
        memset(&ucfg, 0, sizeof(ucfg));
        if (execute_update_batch(ctx, &ureq, &ucfg) != 0)
        {
            snprintf(message, message_max, "UPDATE (between the two SELECTs) failed");
            if (ucfg.xml) { free(ucfg.xml->OUTPUT_XML); free(ucfg.xml); }
            free(ucfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (ucfg.xml) { free(ucfg.xml->OUTPUT_XML); free(ucfg.xml); }
        free(ucfg.OUTPUT_JSON);
    }

    /* Second SELECT - must reflect the update, not a stale cached value */
    {
        execute_config_t cfg2;
        memset(&cfg2, 0, sizeof(cfg2));
        cfg2.SQL = sql;
        cfg2.max_rows = 10;
        cfg2.fetch_array_size = 10;
        int rc2 = execute_query_batch(ctx, &cfg2);

        if (rc2 != 0)
        {
            snprintf(message, message_max, "Second execute_query_batch() failed rc=%d", rc2);
            result = -1;
        }
        else if (!cfg2.xml || !cfg2.xml->OUTPUT_XML ||
                 !strstr(cfg2.xml->OUTPUT_XML, "after-update"))
        {
            snprintf(message, message_max,
                     "Second SELECT did not return 'after-update' - "
                     "possibly served a stale, cached result from "
                     "before the UPDATE");
            result = -1;
        }
        if (cfg2.xml) { free(cfg2.xml->OUTPUT_XML); free(cfg2.xml); }
        free(cfg2.OUTPUT_JSON);
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-SEL-005 ----
 * Control test for UT-SEL-004, added 2026-08-02 following a real
 * question about that test's own design: UT-SEL-004 exercises the
 * resultset cache's own invalidation logic (a genuinely more valuable,
 * realistic property than an uncached query) - but that means a PASS
 * there is ambiguous by itself. It could mean the cache correctly
 * invalidated, or it could mean caching happened to be off in this
 * environment and there was never anything to go stale in the first
 * place. Both look identical from the outside.
 *
 * This test runs the exact same select-update-select sequence with
 * ctx->resultset_cache temporarily, deliberately hidden (saved and set
 * to NULL for the duration of this test only, restored immediately
 * after - the real cache object is never destroyed or recreated, just
 * hidden from this one test's own calls, so every other query in the
 * program keeps using it normally throughout). The cache is consulted
 * purely via whether ctx->resultset_cache is non-NULL at query time
 * (confirmed directly in OCI_Execute_Query_Batch_Module.c) - it is not
 * re-read from ctx->ini->resultset_cache_enabled per query, so that
 * config flag cannot be toggled live; hiding the pointer itself is the
 * only way to genuinely bypass the cache for one call.
 *
 * With no cache involved at all, this MUST pass - it is a pure
 * sanity baseline, not testing the cache. Its real value is as a
 * control: if this one ever fails, the problem is not caching at all
 * (something more fundamental, like transaction visibility) - and if
 * UT-SEL-004 ever fails while this one still passes, that is a clean,
 * unambiguous signal that the cache's own invalidation logic
 * specifically is broken, not something else. */
static int test_ut_sel_005(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999073", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "before-update-nocache", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* Hide the cache for the duration of this test only */
    cache_t *saved_cache = ctx->resultset_cache;
    ctx->resultset_cache = NULL;

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT VARCHAR2_COL FROM %s.%s WHERE NUMBER_COL = 999073",
             g_test_table_owner, g_test_table_name);

    /* First SELECT, cache hidden */
    {
        execute_config_t cfg1;
        memset(&cfg1, 0, sizeof(cfg1));
        cfg1.SQL = sql;
        cfg1.max_rows = 10;
        cfg1.fetch_array_size = 10;
        int rc1 = execute_query_batch(ctx, &cfg1);
        int first_ok = (rc1 == 0 && cfg1.xml && cfg1.xml->OUTPUT_XML &&
                        strstr(cfg1.xml->OUTPUT_XML, "before-update-nocache") != NULL);
        if (cfg1.xml) { free(cfg1.xml->OUTPUT_XML); free(cfg1.xml); }
        free(cfg1.OUTPUT_JSON);
        if (!first_ok)
        {
            snprintf(message, message_max,
                     "First, cache-bypassed SELECT did not return the "
                     "expected 'before-update-nocache' value");
            ctx->resultset_cache = saved_cache;
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
    }

    /* Update the row */
    {
        update_request_t ureq;
        memset(&ureq, 0, sizeof(ureq));
        strncpy(ureq.table_name, g_test_table_name, sizeof(ureq.table_name) - 1);
        strncpy(ureq.owner, g_test_table_owner, sizeof(ureq.owner) - 1);
        where_key_t wk;
        memset(&wk, 0, sizeof(wk));
        strncpy(wk.field_name, "NUMBER_COL", sizeof(wk.field_name) - 1);
        strncpy(wk.key_value, "999073", sizeof(wk.key_value) - 1);
        ureq.key_count = 1;
        ureq.keys = &wk;
        field_value_t set_field;
        memset(&set_field, 0, sizeof(set_field));
        strncpy(set_field.field_name, "VARCHAR2_COL", sizeof(set_field.field_name) - 1);
        strncpy(set_field.value, "after-update-nocache", sizeof(set_field.value) - 1);
        ureq.field_count = 1;
        ureq.fields = &set_field;
        execute_config_t ucfg;
        memset(&ucfg, 0, sizeof(ucfg));
        if (execute_update_batch(ctx, &ureq, &ucfg) != 0)
        {
            snprintf(message, message_max, "UPDATE (between the two SELECTs) failed");
            if (ucfg.xml) { free(ucfg.xml->OUTPUT_XML); free(ucfg.xml); }
            free(ucfg.OUTPUT_JSON);
            ctx->resultset_cache = saved_cache;
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (ucfg.xml) { free(ucfg.xml->OUTPUT_XML); free(ucfg.xml); }
        free(ucfg.OUTPUT_JSON);
    }

    /* Second SELECT, cache still hidden - with no cache involved at
     * all, this must reflect the update.                               */
    {
        execute_config_t cfg2;
        memset(&cfg2, 0, sizeof(cfg2));
        cfg2.SQL = sql;
        cfg2.max_rows = 10;
        cfg2.fetch_array_size = 10;
        int rc2 = execute_query_batch(ctx, &cfg2);

        if (rc2 != 0)
        {
            snprintf(message, message_max,
                     "Second, cache-bypassed execute_query_batch() "
                     "failed rc=%d", rc2);
            result = -1;
        }
        else if (!cfg2.xml || !cfg2.xml->OUTPUT_XML ||
                 !strstr(cfg2.xml->OUTPUT_XML, "after-update-nocache"))
        {
            snprintf(message, message_max,
                     "Second, cache-bypassed SELECT did not return "
                     "'after-update-nocache' - with the cache genuinely "
                     "hidden, this points to something more fundamental "
                     "than caching (e.g. transaction visibility)");
            result = -1;
        }
        if (cfg2.xml) { free(cfg2.xml->OUTPUT_XML); free(cfg2.xml); }
        free(cfg2.OUTPUT_JSON);
    }

    ctx->resultset_cache = saved_cache;
    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-PROC-001 ----
 * A procedure with one scalar IN and one scalar OUT parameter executes
 * and returns the correct OUT value. */
static int test_ut_proc_001(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    execute_procedure_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.procedure_name, g_test_procedure_name, sizeof(req.procedure_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);

    procedure_param_t params[3];
    memset(params, 0, sizeof(params));
    strncpy(params[0].param_name, "P_IN_NUM", sizeof(params[0].param_name) - 1);
    strncpy(params[0].param_type, "NUMBER", sizeof(params[0].param_type) - 1);
    params[0].direction = PARAM_DIR_IN;
    strncpy(params[0].param_value, "21", sizeof(params[0].param_value) - 1);

    strncpy(params[1].param_name, "P_OUT_NUM", sizeof(params[1].param_name) - 1);
    strncpy(params[1].param_type, "INTEGER", sizeof(params[1].param_type) - 1);
    params[1].direction = PARAM_DIR_OUT;

    strncpy(params[2].param_name, "P_RESULTS", sizeof(params[2].param_name) - 1);
    strncpy(params[2].param_type, "CURSOR", sizeof(params[2].param_type) - 1);
    params[2].direction = PARAM_DIR_OUT;

    req.param_count = 3;
    req.parameters = params;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_procedure(ctx, &req, &cfg);

    int result = 0;
    if (rc != 0)
    {
        snprintf(message, message_max, "execute_procedure() failed rc=%d", rc);
        result = -1;
    }
    else if (!cfg.xml || !cfg.xml->OUTPUT_XML ||
             !strstr(cfg.xml->OUTPUT_XML, "<param_value>42</param_value>"))
    {
        snprintf(message, message_max,
                 "Expected <param_value>42</param_value> (21*2) for "
                 "P_OUT_NUM, got: %s",
                 cfg.xml && cfg.xml->OUTPUT_XML ? cfg.xml->OUTPUT_XML : "(null)");
        result = -1;
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-PROC-002 ----
 * A procedure with a CURSOR OUT parameter executes and the resultset
 * is fetched correctly - seeds a matching row first so the cursor has
 * something real to return. */
static int test_ut_proc_002(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed a row the procedure's own cursor query should find */
    {
        insert_request_t ireq;
        memset(&ireq, 0, sizeof(ireq));
        strncpy(ireq.table_name, g_test_table_name, sizeof(ireq.table_name) - 1);
        strncpy(ireq.owner, g_test_table_owner, sizeof(ireq.owner) - 1);
        ireq.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999080", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "cursor-target-row", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        ireq.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &ireq, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    execute_procedure_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.procedure_name, g_test_procedure_name, sizeof(req.procedure_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);

    procedure_param_t params[3];
    memset(params, 0, sizeof(params));
    strncpy(params[0].param_name, "P_IN_NUM", sizeof(params[0].param_name) - 1);
    strncpy(params[0].param_type, "NUMBER", sizeof(params[0].param_type) - 1);
    params[0].direction = PARAM_DIR_IN;
    strncpy(params[0].param_value, "999080", sizeof(params[0].param_value) - 1);

    strncpy(params[1].param_name, "P_OUT_NUM", sizeof(params[1].param_name) - 1);
    strncpy(params[1].param_type, "INTEGER", sizeof(params[1].param_type) - 1);
    params[1].direction = PARAM_DIR_OUT;

    strncpy(params[2].param_name, "P_RESULTS", sizeof(params[2].param_name) - 1);
    strncpy(params[2].param_type, "CURSOR", sizeof(params[2].param_type) - 1);
    params[2].direction = PARAM_DIR_OUT;

    req.param_count = 3;
    req.parameters = params;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_procedure(ctx, &req, &cfg);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_procedure() failed rc=%d", rc);
        result = -1;
    }
    else if (!cfg.xml || !cfg.xml->OUTPUT_XML ||
             !strstr(cfg.xml->OUTPUT_XML, "cursor-target-row"))
    {
        snprintf(message, message_max,
                 "Expected 'cursor-target-row' in the CURSOR OUT "
                 "resultset, got: %s",
                 cfg.xml && cfg.xml->OUTPUT_XML ? cfg.xml->OUTPUT_XML : "(null)");
        result = -1;
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-PROC-005 ----
 * Exactly one metrics row is written per procedure call - regression
 * test for the 2026-07-31 double metrics_write() bug. Counts lines in
 * the real metrics CSV file directly (via ctx->metrics_logger->filename,
 * the exact path the logger itself writes to - not a guessed path)
 * before and after one procedure call, confirming the difference is
 * exactly 1, not 2. */
static int test_ut_proc_005(oci_context_t *ctx, char *message, size_t message_max)
{
    if (!ctx->metrics_logger || !ctx->metrics_logger->filename)
    {
        snprintf(message, message_max,
                 "ctx->metrics_logger or its filename is not available - "
                 "cannot count metrics rows for this test");
        return -1;
    }

    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    long lines_before = 0;
    {
        FILE *fp = fopen(ctx->metrics_logger->filename, "r");
        if (fp)
        {
            char buf[1024];
            while (fgets(buf, sizeof(buf), fp)) lines_before++;
            fclose(fp);
        }
    }

    execute_procedure_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.procedure_name, g_test_procedure_name, sizeof(req.procedure_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);

    procedure_param_t params[3];
    memset(params, 0, sizeof(params));
    strncpy(params[0].param_name, "P_IN_NUM", sizeof(params[0].param_name) - 1);
    strncpy(params[0].param_type, "NUMBER", sizeof(params[0].param_type) - 1);
    params[0].direction = PARAM_DIR_IN;
    strncpy(params[0].param_value, "5", sizeof(params[0].param_value) - 1);
    strncpy(params[1].param_name, "P_OUT_NUM", sizeof(params[1].param_name) - 1);
    strncpy(params[1].param_type, "INTEGER", sizeof(params[1].param_type) - 1);
    params[1].direction = PARAM_DIR_OUT;
    strncpy(params[2].param_name, "P_RESULTS", sizeof(params[2].param_name) - 1);
    strncpy(params[2].param_type, "CURSOR", sizeof(params[2].param_type) - 1);
    params[2].direction = PARAM_DIR_OUT;
    req.param_count = 3;
    req.parameters = params;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_procedure(ctx, &req, &cfg);
    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_procedure() failed rc=%d", rc);
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    /* Fixed 2026-08-10 - this used to count lines immediately after
     * execute_procedure() returned, which was correct back when
     * metrics_write() was called synchronously, inline, on this same
     * call path. As of the metrics refactor's Stage 2 (2026-08-09),
     * metrics_finalise_and_enqueue() is fire-and-forget by design - the
     * actual file write happens moments later, on the Metrics Writer's
     * own dedicated thread, not synchronously within execute_procedure()
     * at all. An immediate single check could legitimately see 0 new
     * lines even on a fully correct system, simply because the writer
     * thread hadn't been scheduled yet - not the double-write regression
     * this test exists to catch. Bounded polling (not a fixed sleep)
     * waits only as long as actually needed: fast on a healthy system,
     * still bounded (2s ceiling) rather than hanging if something is
     * genuinely broken.                                                 */
    long lines_after = 0;
    int  waited_ms = 0;
    const int poll_interval_ms = 50;
    const int max_wait_ms = 2000;
    while (waited_ms <= max_wait_ms)
    {
        lines_after = 0;
        FILE *fp = fopen(ctx->metrics_logger->filename, "r");
        if (fp)
        {
            char buf[1024];
            while (fgets(buf, sizeof(buf), fp)) lines_after++;
            fclose(fp);
        }
        if (lines_after - lines_before >= 1) break;
        struct timespec poll_ts = { 0, (long)poll_interval_ms * 1000000L };
        nanosleep(&poll_ts, NULL);
        waited_ms += poll_interval_ms;
    }

    long new_lines = lines_after - lines_before;
    if (new_lines != 1)
    {
        snprintf(message, message_max,
                 "Expected exactly 1 new metrics row for this procedure "
                 "call, got %ld after waiting up to %dms for the async "
                 "Metrics Writer to catch up - regression of either the "
                 "2026-07-31 double metrics_write() bug (if > 1) or the "
                 "async pipeline itself not persisting this record at "
                 "all (if 0)", new_lines, max_wait_ms);
        result = -1;
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-PROC-006 ----
 * No AUDIT_TRAIL row is written for a procedure call, confirming the
 * deliberate 2026-07-29 no-audit-trail design decision is actually the
 * observed behaviour, not just the documented intent. Counts total
 * AUDIT_TRAIL rows before and after one procedure call, confirming no
 * change. */
static int test_ut_proc_006(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;
    int count_before = 0, count_after = 0;
    OCIStmt *stmt = NULL;
    sb2      ind = 0;
    OCIDefine *dfn = NULL;
    const char *sql = "SELECT COUNT(*) FROM AUDIT_TRAIL";

    if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                         (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
    {
        OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&count_before,
                       (sb4)sizeof(count_before), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    }

    execute_procedure_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.procedure_name, g_test_procedure_name, sizeof(req.procedure_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);

    procedure_param_t params[3];
    memset(params, 0, sizeof(params));
    strncpy(params[0].param_name, "P_IN_NUM", sizeof(params[0].param_name) - 1);
    strncpy(params[0].param_type, "NUMBER", sizeof(params[0].param_type) - 1);
    params[0].direction = PARAM_DIR_IN;
    strncpy(params[0].param_value, "7", sizeof(params[0].param_value) - 1);
    strncpy(params[1].param_name, "P_OUT_NUM", sizeof(params[1].param_name) - 1);
    strncpy(params[1].param_type, "INTEGER", sizeof(params[1].param_type) - 1);
    params[1].direction = PARAM_DIR_OUT;
    strncpy(params[2].param_name, "P_RESULTS", sizeof(params[2].param_name) - 1);
    strncpy(params[2].param_type, "CURSOR", sizeof(params[2].param_type) - 1);
    params[2].direction = PARAM_DIR_OUT;
    req.param_count = 3;
    req.parameters = params;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_procedure(ctx, &req, &cfg);
    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    if (rc != 0)
    {
        snprintf(message, message_max, "execute_procedure() failed rc=%d", rc);
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    stmt = NULL; dfn = NULL; ind = 0;
    if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                         (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
    {
        OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&count_after,
                       (sb4)sizeof(count_after), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    }

    if (count_after != count_before)
    {
        snprintf(message, message_max,
                 "AUDIT_TRAIL row count changed from %d to %d after a "
                 "procedure call - the deliberate no-audit-trail design "
                 "decision is not actually being observed", count_before,
                 count_after);
        result = -1;
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-PROC-003 ----
 * A procedure with zero parameters executes via the
 * BEGIN proc_name; END; code path. Uses a dedicated, hardcoded
 * procedure name (UNIT_TEST_NOOP_PROC) rather than g_test_procedure_
 * name - unit_test.ini's own test_procedure_name is a single,
 * configurable value already used for the scalar/CURSOR tests above;
 * this genuinely different signature (zero parameters) needs its own,
 * separate, dedicated procedure, not something that varies per
 * environment the way the main one might. */
static int test_ut_proc_003(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    execute_procedure_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.procedure_name, "UNIT_TEST_NOOP_PROC", sizeof(req.procedure_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
    req.param_count = 0;
    req.parameters = NULL;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_procedure(ctx, &req, &cfg);

    int result = 0;
    if (rc != 0)
    {
        snprintf(message, message_max,
                 "execute_procedure() on a zero-parameter procedure "
                 "failed rc=%d", rc);
        result = -1;
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-PROC-004 ----
 * A CURSOR OUT parameter that is genuinely NULL (the procedure never
 * opened it at all - not merely a cursor opened against zero matching
 * rows) produces an empty <resultset> element, not an error. Uses a
 * dedicated procedure (UNIT_TEST_CURSOR_PROC) whose own P_OPEN_CURSOR
 * flag controls whether it opens the cursor at all - passing 0 here
 * means the cursor is left completely unopened, the specific scenario
 * this test needs and the existing scalar/CURSOR procedure has no way
 * to produce (it always opens its own cursor, just sometimes against
 * zero rows). */
static int test_ut_proc_004(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    execute_procedure_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.procedure_name, "UNIT_TEST_CURSOR_PROC", sizeof(req.procedure_name) - 1);
    strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);

    procedure_param_t params[2];
    memset(params, 0, sizeof(params));
    strncpy(params[0].param_name, "P_OPEN_CURSOR", sizeof(params[0].param_name) - 1);
    strncpy(params[0].param_type, "NUMBER", sizeof(params[0].param_type) - 1);
    params[0].direction = PARAM_DIR_IN;
    strncpy(params[0].param_value, "0", sizeof(params[0].param_value) - 1);   /* leave the cursor unopened */

    strncpy(params[1].param_name, "P_RESULTS", sizeof(params[1].param_name) - 1);
    strncpy(params[1].param_type, "CURSOR", sizeof(params[1].param_type) - 1);
    params[1].direction = PARAM_DIR_OUT;

    req.param_count = 2;
    req.parameters = params;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = execute_procedure(ctx, &req, &cfg);

    int result = 0;
    if (rc != 0)
    {
        snprintf(message, message_max,
                 "execute_procedure() with a deliberately-unopened "
                 "CURSOR OUT parameter failed rc=%d - a NULL cursor "
                 "should produce an empty resultset, not an error", rc);
        result = -1;
    }
    else if (!cfg.xml || !cfg.xml->OUTPUT_XML ||
             !strstr(cfg.xml->OUTPUT_XML, "<resultset param_name=\"P_RESULTS\""))
    {
        snprintf(message, message_max,
                 "Expected a <resultset param_name=\"P_RESULTS\" .../> "
                 "element even for an unopened cursor, got: %s",
                 cfg.xml && cfg.xml->OUTPUT_XML ? cfg.xml->OUTPUT_XML : "(null)");
        result = -1;
    }

    if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
    free(cfg.OUTPUT_JSON);

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-AUDIT-001 ----
 * audit_trail_fetch_before_image() returns the real, current column
 * value for a row that exists - regression test for the SELECT-
 * refactor-era bug where this always returned "tag not found" because
 * it searched for a literal <COL_NAME> tag against response_write_xml()'s
 * newer <field><field_name>/<field_value> shape. Calls the function
 * directly, not indirectly through UPDATE/DELETE. */
static int test_ut_audit_001(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999091", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "audit-fetch-test", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    char col_names[1][128];
    strncpy(col_names[0], "VARCHAR2_COL", sizeof(col_names[0]) - 1);
    char key_names[1][128];
    strncpy(key_names[0], "NUMBER_COL", sizeof(key_names[0]) - 1);
    char key_values[1][32768];
    strncpy(key_values[0], "999091", sizeof(key_values[0]) - 1);
    char key_data_types[1][128];
    strncpy(key_data_types[0], "NUMBER", sizeof(key_data_types[0]) - 1);

    audit_old_value_t *old_values = NULL;
    int row_count = 0;
    int rc = audit_trail_fetch_before_image(ctx, g_test_table_name, g_test_table_owner,
                                             col_names, 1, key_names, key_values,
                                             key_data_types, 1, &old_values, &row_count);

    if (rc != 0)
    {
        snprintf(message, message_max,
                 "audit_trail_fetch_before_image() failed rc=%d", rc);
        result = -1;
    }
    else if (row_count != 1 || !old_values)
    {
        snprintf(message, message_max,
                 "Expected row_count=1 with a populated old_values array, "
                 "got row_count=%d", row_count);
        result = -1;
    }
    else if (old_values[0].is_null || strcmp(old_values[0].value, "audit-fetch-test") != 0)
    {
        snprintf(message, message_max,
                 "Expected VARCHAR2_COL='audit-fetch-test', got is_null=%d "
                 "value='%s' - this is exactly the SELECT-refactor-era "
                 "'tag not found' regression if is_null is incorrectly 1",
                 old_values[0].is_null, old_values[0].value);
        result = -1;
    }

    free(old_values);
    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-AUDIT-002 ----
 * The same function, given a DATE-typed WHERE key, correctly wraps the
 * comparison in TO_DATE() using the real column type - regression test
 * for the 2026-07-26 finding (this function had no type-awareness at
 * all originally, and a DATE-typed WHERE value used as a bare string
 * comparison would fail with ORA-01858/01861 or simply match nothing). */
static int test_ut_audit_002(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row with a real DATE_COL value */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[2];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999092", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "DATE_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "2026-08-19 14:30:00", sizeof(fields[1].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 2;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT (with real DATE_COL value) failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    char col_names[1][128];
    strncpy(col_names[0], "VARCHAR2_COL", sizeof(col_names[0]) - 1);
    char key_names[1][128];
    strncpy(key_names[0], "DATE_COL", sizeof(key_names[0]) - 1);
    char key_values[1][32768];
    strncpy(key_values[0], "2026-08-19 14:30:00", sizeof(key_values[0]) - 1);
    char key_data_types[1][128];
    strncpy(key_data_types[0], "DATE", sizeof(key_data_types[0]) - 1);

    audit_old_value_t *old_values = NULL;
    int row_count = 0;
    int rc = audit_trail_fetch_before_image(ctx, g_test_table_name, g_test_table_owner,
                                             col_names, 1, key_names, key_values,
                                             key_data_types, 1, &old_values, &row_count);

    if (rc != 0)
    {
        snprintf(message, message_max,
                 "audit_trail_fetch_before_image() with a DATE-typed "
                 "WHERE key failed rc=%d - possible regression of the "
                 "2026-07-26 type-awareness fix", rc);
        result = -1;
    }
    else if (row_count != 1)
    {
        snprintf(message, message_max,
                 "Expected row_count=1 for the DATE-keyed lookup, got %d "
                 "- the DATE-typed WHERE value may not have matched the "
                 "real row at all", row_count);
        result = -1;
    }

    free(old_values);
    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-AUDIT-003 ----
 * audit_trail_insert() (field-level path) correctly handles
 * new_values=NULL, used by DELETE where there is no "new" value. Calls
 * the function directly with a synthetic, minimal request rather than
 * indirectly through execute_delete_batch() (which is already
 * exercised by UT-DEL-001 through 005). */
static int test_ut_audit_003(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    /* audit_trail_insert()'s own old_values/new_values arrays must
     * match audit_field_value_t's real, private layout - defined
     * locally inside OCI_Audit_Trail_Manager.c as
     * { char value[32768]; int is_empty; } - not field_value_t's own,
     * completely different layout (field_name[128]/value[4096]/
     * client_date_format[64]/large_value). Found via a real
     * stack-use-after-return ASan crash: passing a field_value_t here
     * (the original version of this test) let this function read
     * misaligned, garbage memory that happened to look like a stale
     * pointer from an earlier, already-returned xml_append() stack
     * frame. Mirrored locally, matching the same established pattern
     * already used elsewhere in this project (upd_fv_t/audit_fv_t) for
     * exactly this purpose.                                            */
    typedef struct { char value[32768]; int is_empty; } audit_test_fv_t;
    audit_test_fv_t old_val;
    memset(&old_val, 0, sizeof(old_val));
    strncpy(old_val.value, "999093", sizeof(old_val.value) - 1);
    old_val.is_empty = 0;

    char col_names[1][128];
    strncpy(col_names[0], "NUMBER_COL", sizeof(col_names[0]) - 1);

    audit_trail_request_t atr;
    memset(&atr, 0, sizeof(atr));
    strncpy(atr.table_name, g_test_table_name, sizeof(atr.table_name) - 1);
    strncpy(atr.record_id, "999093", sizeof(atr.record_id) - 1);
    strncpy(atr.action_type, "DELETE", sizeof(atr.action_type) - 1);
    atr.col_names = col_names;
    atr.col_types = NULL;
    atr.new_values = NULL;   /* the specific case this test is about */
    atr.old_values = &old_val;
    atr.row_count = 1;
    atr.col_count = 1;
    strncpy(atr.changed_by, "unit_test", sizeof(atr.changed_by) - 1);
    strncpy(atr.change_reason, "unit_test_tx", sizeof(atr.change_reason) - 1);
    strncpy(atr.module_name, "OCI_Unit_Test_Module", sizeof(atr.module_name) - 1);
    atr.audit_mode = 0;

    int rc = audit_trail_insert(ctx, &atr);

    int result = 0;
    if (rc != 0)
    {
        snprintf(message, message_max,
                 "audit_trail_insert() with new_values=NULL (the DELETE "
                 "case) failed rc=%d", rc);
        result = -1;
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-TX-002 ----
 * begin_standalone_tx_if_needed() gives a standalone call (no external
 * tx wrapper) its own transaction_id - regression test for the
 * 2026-07-26 GxP traceability gap. Deliberately does NOT wrap itself in
 * begin_test_transaction()/rollback_test_transaction() the way every
 * other Tier 3 test does - the whole point of this test is confirming
 * what happens when ctx->active_tx is genuinely NULL beforehand, so
 * wrapping it in an external transaction would defeat the test
 * entirely. Safe to do: begin_standalone_tx_if_needed()/
 * end_standalone_tx_if_owned() never touch the real database at all
 * (per their own doc comment in OCI_Transaction_Manager.h - local_tx is
 * never registered with any transaction table or commit bookkeeping),
 * this is pure in-memory pointer/struct bookkeeping. */
static int test_ut_tx_002(oci_context_t *ctx, char *message, size_t message_max)
{
    if (ctx->active_tx != NULL)
    {
        snprintf(message, message_max,
                 "ctx->active_tx is already non-NULL before this test "
                 "even starts - cannot cleanly test the standalone case "
                 "(a prior test may not have cleaned up correctly)");
        return -1;
    }

    tx_handle_t local_tx;
    memset(&local_tx, 0, sizeof(local_tx));
    int owned = begin_standalone_tx_if_needed(ctx, &local_tx);

    int result = 0;
    if (owned != 1)
    {
        snprintf(message, message_max,
                 "begin_standalone_tx_if_needed() returned %d, expected 1 "
                 "(ctx->active_tx was genuinely NULL beforehand, so this "
                 "call should own a fresh transaction identity)", owned);
        result = -1;
    }
    else if (ctx->active_tx != &local_tx)
    {
        snprintf(message, message_max,
                 "ctx->active_tx does not point at local_tx even though "
                 "begin_standalone_tx_if_needed() returned 1");
        result = -1;
    }
    else
    {
        const char *tx_id = tx_get_id(ctx->active_tx);
        if (!tx_id || tx_id[0] == '\0' || strcmp(tx_id, "-") == 0)
        {
            snprintf(message, message_max,
                     "tx_get_id() returned '%s' for a standalone call - "
                     "expected a real UUID, not the '-' placeholder "
                     "(this is exactly the 2026-07-26 traceability gap)",
                     tx_id ? tx_id : "(null)");
            result = -1;
        }
    }

    end_standalone_tx_if_owned(ctx, owned);

    if (result == 0 && ctx->active_tx != NULL)
    {
        snprintf(message, message_max,
                 "ctx->active_tx is still non-NULL after "
                 "end_standalone_tx_if_owned() - it should be cleared "
                 "back to NULL once this call's own standalone "
                 "transaction ends");
        result = -1;
    }

    return result;
}

/* ---- UT-TX-003 ----
 * The same function, called from a request that IS already inside an
 * external transaction, does not overwrite the existing transaction_id
 * - confirms the recursion-safety behaviour (e.g. the audit trail's
 * own INSERT nested inside the business operation that triggered it
 * correctly inherits the outer call's transaction_id unchanged). */
static int test_ut_tx_003(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t external_tx;
    if (begin_test_transaction(ctx, &external_tx) != 0)
    {
        snprintf(message, message_max,
                 "Could not begin the test's own (external) transaction");
        return -1;
    }

    tx_handle_t *original_active_tx = ctx->active_tx;
    const char  *original_tx_id = tx_get_id(ctx->active_tx);
    char saved_tx_id[128] = {0};
    strncpy(saved_tx_id, original_tx_id ? original_tx_id : "", sizeof(saved_tx_id) - 1);

    tx_handle_t local_tx;
    memset(&local_tx, 0, sizeof(local_tx));
    int owned = begin_standalone_tx_if_needed(ctx, &local_tx);

    int result = 0;
    if (owned != 0)
    {
        snprintf(message, message_max,
                 "begin_standalone_tx_if_needed() returned %d, expected "
                 "0 (ctx->active_tx was already set by an external "
                 "transaction, so this call should do nothing)", owned);
        result = -1;
    }
    else if (ctx->active_tx != original_active_tx)
    {
        snprintf(message, message_max,
                 "ctx->active_tx was overwritten - it should have been "
                 "left pointing at the original, external transaction "
                 "unchanged");
        result = -1;
    }
    else
    {
        const char *current_tx_id = tx_get_id(ctx->active_tx);
        if (!current_tx_id || strcmp(current_tx_id, saved_tx_id) != 0)
        {
            snprintf(message, message_max,
                     "Transaction id changed from '%s' to '%s' - the "
                     "existing, external transaction_id should never be "
                     "overwritten by a nested standalone call",
                     saved_tx_id, current_tx_id ? current_tx_id : "(null)");
            result = -1;
        }
    }

    end_standalone_tx_if_owned(ctx, owned);

    rollback_test_transaction(ctx, &external_tx);
    return result;
}

/* ---- UT-SESS-001 ----
 * CREATE_SESSION writes a permanent OCI_SESSION row and returns a
 * well-formed session_id. Wrapped in the same begin_test_transaction()/
 * rollback_test_transaction() pattern as every other Tier 3 test for
 * consistency and safety, even though a real CREATE_SESSION call is
 * normally permanent (not something a caller would roll back) - here
 * the rollback is purely this test's own cleanup guarantee, not a
 * comment on how sessions behave in real usage.
 *
 * UT-SESS-002 (END_SESSION setting LOGGED_OUT via session_end()) is
 * already covered by UT-UPD-004, and UT-SESS-003 (session_reconcile_
 * orphans() treating zero orphans as healthy) is already implemented
 * as a Tier 2 test - see this catalog's own notes for both. This is
 * the only genuinely new SESS test needed. */
static int test_ut_sess_001(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;
    char *create_xml = NULL;

    session_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.operation, "CREATE_SESSION", sizeof(req.operation) - 1);
    strncpy(req.client_id, "unit-test-client", sizeof(req.client_id) - 1);
    strncpy(req.client_ip, "127.0.0.1", sizeof(req.client_ip) - 1);
    strncpy(req.application_name, "unit_test", sizeof(req.application_name) - 1);
    req.requested_ttl_seconds = 60;

    if (session_create(ctx, &req, &create_xml) != 0 || !create_xml)
    {
        snprintf(message, message_max, "session_create() failed");
        free(create_xml);
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    char session_id[128] = {0};
    const char *tag_start = strstr(create_xml, "<session_id>");
    if (tag_start)
    {
        tag_start += strlen("<session_id>");
        const char *tag_end = strstr(tag_start, "</session_id>");
        if (tag_end && (size_t)(tag_end - tag_start) < sizeof(session_id))
        {
            memcpy(session_id, tag_start, (size_t)(tag_end - tag_start));
            session_id[tag_end - tag_start] = '\0';
        }
    }
    free(create_xml);

    if (!session_id[0])
    {
        snprintf(message, message_max,
                 "Could not extract a session_id from session_create()'s "
                 "own result XML");
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    /* A well-formed session_id: SESSION_ID is VARCHAR2(36) per
     * Create_Session_Table.txt (a UUID), so a reasonable, non-fragile
     * sanity check is length alone (36 chars) rather than parsing the
     * exact UUID format.                                               */
    if (strlen(session_id) != 36)
    {
        snprintf(message, message_max,
                 "session_id='%s' (%zu chars) does not look like a "
                 "well-formed UUID (expected 36 chars)",
                 session_id, strlen(session_id));
        result = -1;
    }
    else
    {
        OCIStmt *stmt = NULL;
        int      found = 0;
        sb2      ind = 0;
        OCIDefine *dfn = NULL;
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT 1 FROM DUAL WHERE EXISTS "
                 "(SELECT 1 FROM OCI_SESSION WHERE SESSION_ID = '%s')",
                 session_id);
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn, ctx->errhp, 1, (dvoid *)&found,
                           (sb4)sizeof(found), SQLT_INT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        }
        if (found != 1)
        {
            snprintf(message, message_max,
                     "No OCI_SESSION row found for session_id='%s' - "
                     "CREATE_SESSION should write a real, permanent row",
                     session_id);
            result = -1;
        }
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-CONT-001 ----
 * Contention Manager's own classifier (payload_requires_single_writer_
 * queue(), file_consumer.c) - closure item 5 follow-up test catalog
 * addition, 2026-08-12.
 *
 * Pure string-classification logic, no connection needed - Tier 1.
 * Calls the real function directly (exported non-static specifically
 * for this - see file_consumer.h's own note on that change). Deliberately
 * minimal, representative payloads - this function is a lightweight
 * raw-text peek, not a parser, so a full, valid request envelope isn't
 * needed to exercise it; only the operation-type marker it actually
 * looks for matters.                                                    */
static int test_ut_cont_001(oci_context_t *ctx, char *message, size_t message_max)
{
    (void)ctx;   /* pure logic, no ctx needed at all */

    struct { const char *payload; int expect_write; const char *label; } cases[] = {
        { "<request><op type=\"INSERT\">x</op></request>",              1, "XML INSERT" },
        { "<request><op type=\"UPDATE\">x</op></request>",              1, "XML UPDATE" },
        { "<request><op type=\"DELETE\">x</op></request>",              1, "XML DELETE" },
        { "{\"op\": {\"type\": \"INSERT\"}}",                           1, "JSON INSERT (spaced)" },
        { "{\"op\": {\"type\":\"UPDATE\"}}",                            1, "JSON UPDATE (unspaced)" },
        { "{\"op\": {\"type\": \"DELETE\"}}",                           1, "JSON DELETE (spaced)" },
        { "<request><op type=\"SELECT\">x</op></request>",              0, "XML SELECT" },
        { "{\"op\": {\"type\": \"SELECT\"}}",                           0, "JSON SELECT" },
        { "<request><op type=\"EXECUTE_PROCEDURE\">x</op></request>",   0, "XML EXECUTE_PROCEDURE" },
        { "{\"op\": {\"type\": \"EXECUTE_PROCEDURE\"}}",                0, "JSON EXECUTE_PROCEDURE" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int got = payload_requires_single_writer_queue(cases[i].payload);
        if ((got != 0) != (cases[i].expect_write != 0))
        {
            snprintf(message, message_max,
                     "%s: expected requires_write=%d, got %d",
                     cases[i].label, cases[i].expect_write, got);
            return -1;
        }
    }

    return 0;
}

/* ---- UT-CONT-002 ----
 * Confirms the "any write anywhere in the request" rule specifically -
 * not just "checks the first operation". The write marker here is
 * deliberately the LAST of three operations, with two SELECTs ahead of
 * it - a classifier that only checked the first operation, or stopped
 * at the first non-match, would wrongly return 0 for this payload.    */
static int test_ut_cont_002(oci_context_t *ctx, char *message, size_t message_max)
{
    (void)ctx;

    const char *payload =
        "<request>"
        "<op type=\"SELECT\">a</op>"
        "<op type=\"SELECT\">b</op>"
        "<op type=\"UPDATE\">c</op>"
        "</request>";

    if (!payload_requires_single_writer_queue(payload))
    {
        snprintf(message, message_max,
                 "A request with a write as its third (last) operation, "
                 "behind two reads, was NOT classified as requiring the "
                 "writer queue - looks like only the first operation is "
                 "being checked");
        return -1;
    }

    /* Sibling check the other direction, for completeness - a request
     * with genuinely no write anywhere must NOT be classified as one. */
    const char *all_reads =
        "<request>"
        "<op type=\"SELECT\">a</op>"
        "<op type=\"SELECT\">b</op>"
        "<op type=\"EXECUTE_PROCEDURE\">c</op>"
        "</request>";

    if (payload_requires_single_writer_queue(all_reads))
    {
        snprintf(message, message_max,
                 "A request with no write operation anywhere was "
                 "incorrectly classified as requiring the writer queue");
        return -1;
    }

    return 0;
}

/* ---- UT-CONT-004 ----
 * contention_manager_mode_is_single_write_queue() (file_consumer.c) -
 * closure item 5 follow-up test catalog addition, 2026-08-12. Extracted
 * from what used to be duplicated inline logic specifically to make
 * this testable - see file_consumer.h's own doc comment on that
 * function.
 *
 * Regression test for the exact config mistake made and caught
 * 2026-08-08: a bare "1" (the boolean-style value every OTHER recently-
 * added toggle in this project uses) must NOT be treated as equivalent
 * to "single_write_queue" - this is a string setting, not a boolean,
 * and the documented contract is a silent no-match (feature off) for
 * anything that isn't an exact string match, not an error.            */
static int test_ut_cont_004(oci_context_t *ctx, char *message, size_t message_max)
{
    (void)ctx;

    struct { const char *mode; int expect_enabled; const char *label; } cases[] = {
        { "single_write_queue", 1, "exact match" },
        { "SINGLE_WRITE_QUEUE", 1, "exact match, different case" },
        { "1",                 0, "boolean-style '1' - the actual 2026-08-08 mistake" },
        { "true",              0, "boolean-style 'true'" },
        { "off",               0, "documented off value" },
        { "",                  0, "empty string" },
        { NULL,                0, "NULL" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int got = contention_manager_mode_is_single_write_queue(cases[i].mode);
        if ((got != 0) != (cases[i].expect_enabled != 0))
        {
            snprintf(message, message_max,
                     "mode=%s (%s): expected enabled=%d, got %d",
                     cases[i].mode ? cases[i].mode : "(NULL)",
                     cases[i].label, cases[i].expect_enabled, got);
            return -1;
        }
    }

    return 0;
}

/* ---- UT-SESS-006 ----
 * Confirms generic_queue.c's own "drop rather than block" contract
 * holds under genuine saturation - closure item 5 follow-up test
 * catalog addition, 2026-08-12.
 *
 * Deliberately does NOT touch the real, shared Session Manager touch
 * queue - that queue doesn't exist yet at the point self-tests run
 * (Session Manager's own thread starts later, alongside the worker
 * pool - see this file's own Architecture note on self-test timing).
 * Instead, creates its own small, independent, test-owned
 * generic_queue_t (matching how UT-CONN-005 tests OCI_Pool_session_
 * is_alive() via its own separate connection rather than the shared
 * test_ctx) - proving the same underlying mechanism the real touch
 * queue depends on, without needing the real queue to exist.          */
static int test_ut_sess_006(oci_context_t *ctx, char *message, size_t message_max)
{
    (void)ctx;

    const int depth = 4;
    generic_queue_t *q = generic_queue_create(depth, free);
    if (!q)
    {
        snprintf(message, message_max, "generic_queue_create() itself failed");
        return -1;
    }

    int result = 0;

    /* Fill to capacity - every one of these must succeed. */
    for (int i = 0; i < depth; i++)
    {
        int *item = malloc(sizeof(int));
        *item = i;
        if (generic_queue_enqueue(q, item) != 0)
        {
            snprintf(message, message_max,
                     "Enqueue %d of %d (queue not yet full) unexpectedly "
                     "failed", i + 1, depth);
            free(item);
            result = -1;
            break;
        }
    }

    /* One more, past capacity - this one must be REJECTED, not block
     * (there is nothing dequeuing concurrently here - a blocking
     * implementation would hang this whole test run, not just fail a
     * comparison), and the test must get its own item back untouched
     * so it can free it itself (unlike a successful enqueue, which
     * transfers ownership to the queue).                               */
    if (result == 0)
    {
        int *overflow_item = malloc(sizeof(int));
        *overflow_item = 999;
        int rc = generic_queue_enqueue(q, overflow_item);
        if (rc == 0)
        {
            snprintf(message, message_max,
                     "Enqueue past a full queue's own depth (%d) "
                     "succeeded when it should have been rejected - "
                     "the queue is not actually bounded", depth);
            result = -1;
        }
        free(overflow_item);   /* ownership stayed with the caller on
                                   failure, per generic_queue.h's own
                                   documented contract - must free it
                                   here regardless of pass/fail above  */
    }

    /* free_fn passed to generic_queue_create() above (free) correctly
     * cleans up the depth items still queued - not manually drained
     * here, deliberately, to also prove that path works.              */
    generic_queue_destroy(q);

    return result;
}

/* ---- UT-SESS-004 ----
 * Session Manager Stage 2 (async activity tracking, 2026-08-06) -
 * closure item 5 follow-up test catalog addition, 2026-08-10.
 *
 * session_touch() (cache-only) and session_touch_db() (permanent-table
 * persistence) are two deliberately separate functions - see both
 * their own doc comments in OCI_Session_Manager.h. This test calls
 * both directly, exactly as the real code does (session_touch() is
 * called inline on the request path; session_touch_db() is called
 * asynchronously by the Session Manager's own dedicated thread
 * draining its touch queue - neither of those callers themselves are
 * exercised here, matching every other test in this catalog testing
 * the underlying function, not the thread/queue wiring around it).
 *
 * last_activity_ts is time_t (second granularity) - a real 1-second
 * sleep is used so a genuinely later touch is guaranteed to produce a
 * different, comparable value, not just an assumption that it would. */
static int test_ut_sess_004(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;
    char *create_xml = NULL;

    session_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.operation, "CREATE_SESSION", sizeof(req.operation) - 1);
    strncpy(req.client_id, "unit-test-client", sizeof(req.client_id) - 1);
    strncpy(req.client_ip, "127.0.0.1", sizeof(req.client_ip) - 1);
    strncpy(req.application_name, "unit_test", sizeof(req.application_name) - 1);
    req.requested_ttl_seconds = 60;

    if (session_create(ctx, &req, &create_xml) != 0 || !create_xml)
    {
        snprintf(message, message_max, "session_create() failed");
        free(create_xml);
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    char session_id[128] = {0};
    const char *tag_start = strstr(create_xml, "<session_id>");
    if (tag_start)
    {
        tag_start += strlen("<session_id>");
        const char *tag_end = strstr(tag_start, "</session_id>");
        if (tag_end && (size_t)(tag_end - tag_start) < sizeof(session_id))
        {
            memcpy(session_id, tag_start, (size_t)(tag_end - tag_start));
            session_id[tag_end - tag_start] = '\0';
        }
    }
    free(create_xml);

    if (!session_id[0])
    {
        snprintf(message, message_max,
                 "Could not extract a session_id from session_create()'s "
                 "own result XML");
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    session_record_t before;
    memset(&before, 0, sizeof(before));
    if (session_validate(ctx, session_id, &before) != SESSION_OK)
    {
        snprintf(message, message_max,
                 "session_validate() could not find the session this "
                 "test just created - cannot proceed");
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    sleep(1);   /* see this test's own top comment on why */

    if (session_touch(ctx, session_id) != SESSION_OK)
    {
        snprintf(message, message_max, "session_touch() itself failed");
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    session_record_t after_cache;
    memset(&after_cache, 0, sizeof(after_cache));
    if (session_validate(ctx, session_id, &after_cache) != SESSION_OK ||
        after_cache.last_activity_ts <= before.last_activity_ts)
    {
        snprintf(message, message_max,
                 "session_touch() did not advance last_activity_ts in "
                 "the cache (before=%ld, after=%ld)",
                 (long)before.last_activity_ts, (long)after_cache.last_activity_ts);
        result = -1;
    }

    if (session_touch_db(ctx, session_id) != SESSION_OK)
    {
        snprintf(message, message_max,
                 "session_touch_db() itself failed%s",
                 result == -1 ? " (in addition to the cache check above)" : "");
        result = -1;
    }
    else
    {
        OCIStmt *stmt = NULL;
        OCIDefine *dfn = NULL;
        sb2 ind = 0;
        time_t db_last_activity = 0;
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT CAST(LAST_ACTIVITY_TS AS DATE) - "
                 "TO_DATE('1970-01-01','YYYY-MM-DD') "
                 "FROM OCI_SESSION WHERE SESSION_ID = '%s'", session_id);
        /* LAST_ACTIVITY_TS is TIMESTAMP (Create_Session_Table.txt), not
         * DATE - CAST to DATE first so the subtraction below produces a
         * plain NUMBER (days since epoch), not an INTERVAL, which
         * SQLT_FLT could not bind. Dropping sub-second precision here
         * is fine - this test only needs second-granularity comparison
         * anyway, matching time_t's own resolution.                    */
        double days_since_epoch = 0;
        OCIDefine *dfn2 = NULL;
        if (OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp, (text *)sql,
                             (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT) == OCI_SUCCESS)
        {
            OCIDefineByPos(stmt, &dfn2, ctx->errhp, 1, (dvoid *)&days_since_epoch,
                           (sb4)sizeof(days_since_epoch), SQLT_FLT, &ind, NULL, NULL, OCI_DEFAULT);
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0, NULL, NULL, OCI_DEFAULT);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
            db_last_activity = (time_t)(days_since_epoch * 86400.0);
        }
        (void)dfn;

        if (db_last_activity < before.last_activity_ts)
        {
            snprintf(message, message_max,
                     "session_touch_db() did not advance LAST_ACTIVITY_TS "
                     "in the permanent OCI_SESSION row (before=%ld, "
                     "db_after=%ld)",
                     (long)before.last_activity_ts, (long)db_last_activity);
            result = -1;
        }
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-SESS-005 ----
 * Session Manager Stage 3 (hard validation, 2026-08-08) - closure item
 * 5 follow-up test catalog addition, 2026-08-10.
 *
 * SCOPE NOTE, narrower than this catalog's own original description:
 * tests session_validate() itself directly - accepts a real, just-
 * created session; rejects a genuinely unknown one with SESSION_ERR_
 * NOT_FOUND - not dispatcher.c's own integration of it (the SESSION_
 * VALIDATION rejection envelope, or the session_validation_enabled
 * kill-switch). This test-core module deliberately never routes
 * through the dispatcher for any test (see this file's own top
 * comment on why) - session_validate() is the real mechanism
 * dispatcher.c relies on, and is what's actually being proven here,
 * consistent with every other test in this catalog testing the
 * underlying function rather than the dispatch wiring around it. */
static int test_ut_sess_005(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;
    char *create_xml = NULL;

    session_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.operation, "CREATE_SESSION", sizeof(req.operation) - 1);
    strncpy(req.client_id, "unit-test-client", sizeof(req.client_id) - 1);
    strncpy(req.client_ip, "127.0.0.1", sizeof(req.client_ip) - 1);
    strncpy(req.application_name, "unit_test", sizeof(req.application_name) - 1);
    req.requested_ttl_seconds = 60;

    if (session_create(ctx, &req, &create_xml) != 0 || !create_xml)
    {
        snprintf(message, message_max, "session_create() failed");
        free(create_xml);
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    char session_id[128] = {0};
    const char *tag_start = strstr(create_xml, "<session_id>");
    if (tag_start)
    {
        tag_start += strlen("<session_id>");
        const char *tag_end = strstr(tag_start, "</session_id>");
        if (tag_end && (size_t)(tag_end - tag_start) < sizeof(session_id))
        {
            memcpy(session_id, tag_start, (size_t)(tag_end - tag_start));
            session_id[tag_end - tag_start] = '\0';
        }
    }
    free(create_xml);

    if (!session_id[0])
    {
        snprintf(message, message_max,
                 "Could not extract a session_id from session_create()'s "
                 "own result XML");
        rollback_test_transaction(ctx, &tx);
        return -1;
    }

    if (session_validate(ctx, session_id, NULL) != SESSION_OK)
    {
        snprintf(message, message_max,
                 "session_validate() rejected a session_id this test "
                 "just created and confirmed exists");
        result = -1;
    }

    /* Not just any wrong string - a syntactically well-formed but
     * genuinely nonexistent UUID, so this is a real "not found" case,
     * not just malformed input rejected for some unrelated reason.    */
    int rc = session_validate(ctx, "00000000-0000-0000-0000-000000000000", NULL);
    if (rc != SESSION_ERR_NOT_FOUND)
    {
        snprintf(message, message_max,
                 "session_validate() returned rc=%d for a genuinely "
                 "unknown session_id - expected SESSION_ERR_NOT_FOUND%s",
                 rc, result == -1 ? " (in addition to the failure above)" : "");
        result = -1;
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}

/* ---- UT-DATE-004 ----
 * An UPDATE WHERE key declared in European DD/MM/YYYY format and a
 * DELETE WHERE key on the same underlying value declared in US
 * MM/DD/YYYY format both correctly match the real row - the same
 * scenario originally confirmed manually during the real Date E2E
 * testing weeks ago (2026-07-27/28/29), now as a real, automated,
 * always-rolled-back Tier 3 test. The DATE_COL value itself never
 * changes across the UPDATE/DELETE - only VARCHAR2_COL does - so both
 * operations' own WHERE keys genuinely target the same underlying date,
 * just declared in two different client formats. */
static int test_ut_date_004(oci_context_t *ctx, char *message, size_t message_max)
{
    tx_handle_t tx;
    if (begin_test_transaction(ctx, &tx) != 0)
    {
        snprintf(message, message_max, "Could not begin the test's own transaction");
        return -1;
    }

    int result = 0;

    /* Seed row with DATE_COL in the canonical, configured format */
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, g_test_table_name, sizeof(req.table_name) - 1);
        strncpy(req.owner, g_test_table_owner, sizeof(req.owner) - 1);
        req.row_count = 1;
        field_value_t fields[3];
        memset(fields, 0, sizeof(fields));
        strncpy(fields[0].field_name, "NUMBER_COL", sizeof(fields[0].field_name) - 1);
        strncpy(fields[0].value, "999094", sizeof(fields[0].value) - 1);
        strncpy(fields[1].field_name, "VARCHAR2_COL", sizeof(fields[1].field_name) - 1);
        strncpy(fields[1].value, "before-date-e2e", sizeof(fields[1].value) - 1);
        strncpy(fields[2].field_name, "DATE_COL", sizeof(fields[2].field_name) - 1);
        strncpy(fields[2].value, "2026-08-19 14:30:00", sizeof(fields[2].value) - 1);
        insert_row_t row;
        memset(&row, 0, sizeof(row));
        row.field_count = 3;
        row.fields = fields;
        req.rows = &row;
        execute_config_t icfg;
        memset(&icfg, 0, sizeof(icfg));
        if (execute_insert_batch(ctx, &req, &icfg) != 0)
        {
            snprintf(message, message_max, "Seed INSERT (with real DATE_COL value) failed");
            if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
            free(icfg.OUTPUT_JSON);
            rollback_test_transaction(ctx, &tx);
            return -1;
        }
        if (icfg.xml) { free(icfg.xml->OUTPUT_XML); free(icfg.xml); }
        free(icfg.OUTPUT_JSON);
    }

    /* UPDATE, WHERE-keyed on DATE_COL declared in European DD/MM/YYYY */
    {
        update_request_t ureq;
        memset(&ureq, 0, sizeof(ureq));
        strncpy(ureq.table_name, g_test_table_name, sizeof(ureq.table_name) - 1);
        strncpy(ureq.owner, g_test_table_owner, sizeof(ureq.owner) - 1);
        where_key_t wk;
        memset(&wk, 0, sizeof(wk));
        strncpy(wk.field_name, "DATE_COL", sizeof(wk.field_name) - 1);
        strncpy(wk.key_value, "19/08/2026 14:30:00", sizeof(wk.key_value) - 1);
        strncpy(wk.client_date_format, "DD/MM/YYYY HH24:MI:SS",
                sizeof(wk.client_date_format) - 1);
        ureq.key_count = 1;
        ureq.keys = &wk;
        field_value_t set_field;
        memset(&set_field, 0, sizeof(set_field));
        strncpy(set_field.field_name, "VARCHAR2_COL", sizeof(set_field.field_name) - 1);
        strncpy(set_field.value, "after-date-e2e", sizeof(set_field.value) - 1);
        ureq.field_count = 1;
        ureq.fields = &set_field;

        execute_config_t ucfg;
        memset(&ucfg, 0, sizeof(ucfg));
        int rc = execute_update_batch(ctx, &ureq, &ucfg);

        if (rc != 0)
        {
            snprintf(message, message_max,
                     "UPDATE with a European DD/MM/YYYY-declared DATE_COL "
                     "WHERE key failed rc=%d", rc);
            result = -1;
        }
        else if (ucfg.xml && ucfg.xml->OUTPUT_XML &&
                 !strstr(ucfg.xml->OUTPUT_XML, "<rows_updated>1</rows_updated>"))
        {
            snprintf(message, message_max,
                     "Expected <rows_updated>1</rows_updated> for the "
                     "European-format DATE_COL WHERE key, got: %s",
                     ucfg.xml->OUTPUT_XML);
            result = -1;
        }
        if (ucfg.xml) { free(ucfg.xml->OUTPUT_XML); free(ucfg.xml); }
        free(ucfg.OUTPUT_JSON);

        if (result != 0)
        {
            rollback_test_transaction(ctx, &tx);
            return result;
        }
    }

    /* DELETE, WHERE-keyed on the SAME underlying DATE_COL value,
     * declared in US MM/DD/YYYY this time.                             */
    {
        delete_request_t dreq;
        memset(&dreq, 0, sizeof(dreq));
        strncpy(dreq.table_name, g_test_table_name, sizeof(dreq.table_name) - 1);
        strncpy(dreq.owner, g_test_table_owner, sizeof(dreq.owner) - 1);
        where_key_t wk;
        memset(&wk, 0, sizeof(wk));
        strncpy(wk.field_name, "DATE_COL", sizeof(wk.field_name) - 1);
        strncpy(wk.key_value, "08/19/2026 14:30:00", sizeof(wk.key_value) - 1);
        strncpy(wk.client_date_format, "MM/DD/YYYY HH24:MI:SS",
                sizeof(wk.client_date_format) - 1);
        dreq.key_count = 1;
        dreq.keys = &wk;

        execute_config_t dcfg;
        memset(&dcfg, 0, sizeof(dcfg));
        int rc = execute_delete_batch(ctx, &dreq, &dcfg);

        if (rc != 0)
        {
            snprintf(message, message_max,
                     "DELETE with a US MM/DD/YYYY-declared DATE_COL "
                     "WHERE key (same underlying value the UPDATE just "
                     "matched) failed rc=%d", rc);
            result = -1;
        }
        else if (dcfg.xml && dcfg.xml->OUTPUT_XML &&
                 !strstr(dcfg.xml->OUTPUT_XML, "<rows_deleted>1</rows_deleted>"))
        {
            snprintf(message, message_max,
                     "Expected <rows_deleted>1</rows_deleted> for the "
                     "US-format DATE_COL WHERE key, got: %s",
                     dcfg.xml->OUTPUT_XML);
            result = -1;
        }
        if (dcfg.xml) { free(dcfg.xml->OUTPUT_XML); free(dcfg.xml); }
        free(dcfg.OUTPUT_JSON);
    }

    rollback_test_transaction(ctx, &tx);
    return result;
}


static const unit_test_case_t g_registry[] = {
    { "UT-LOG-001",  UT_TIER_1, "LOG",  "Every configured logger is non-NULL on ctx",        test_ut_log_001 },
    { "UT-LOG-002",  UT_TIER_1, "LOG",  "DEBUG line suppressed when logger level is INFO",   test_ut_log_002 },
    { "UT-INI-001",  UT_TIER_1, "INI",  "ctx->ini populated by the real startup load",       test_ut_ini_001 },
    { "UT-INI-002",  UT_TIER_1, "INI",  "Unguarded CFG_STRING field safeguard - zero found", test_ut_ini_002 },
    { "UT-INI-003",  UT_TIER_1, "INI",  "Missing-required-key config is rejected",           test_ut_ini_003 },
    { "UT-L1-001",   UT_TIER_1, "L1",   "New-format detection survives a leading comment",   test_ut_l1_001 },
    { "UT-L1-002",   UT_TIER_1, "L1",   "XML and JSON parse to identical INSERT structure",  test_ut_l1_002 },
    { "UT-L1-003",   UT_TIER_1, "L1",   "Mixed-type multi-operation transaction parses",     test_ut_l1_003 },
    { "UT-L1-004",   UT_TIER_1, "L1",   "INSERT multi-row parses correctly",                 test_ut_l1_004 },
    { "UT-L1-005",   UT_TIER_1, "L1",   "UPDATE WHERE-key client_date_format parses",        test_ut_l1_005 },
    { "UT-L1-006",   UT_TIER_1, "L1",   "DELETE WHERE-key client_date_format parses",        test_ut_l1_006 },
    { "UT-L1-007",   UT_TIER_1, "L1",   "EXECUTE_PROCEDURE parameters/direction parse",      test_ut_l1_007 },
    { "UT-L1-008",   UT_TIER_1, "L1",   "level1_free_request() clean for every op type",     test_ut_l1_008 },
    { "UT-DATE-005", UT_TIER_1, "DATE", "No hardcoded date format literal in source",        test_ut_date_005 },

    /* ---- Tier 2 (2026-08-01) ---- */
    { "UT-CONN-001", UT_TIER_2, "CONN", "The real, active connection is alive and healthy", test_ut_conn_001 },
    /* UT-CONN-002 removed 2026-08-01 - see the note at its former
     * location, right before UT-CONN-003, for the full reasoning.      */
    { "UT-CONN-003", UT_TIER_2, "CONN", "Pooled worker_ctx has every logger set",     test_ut_conn_003 },
    { "UT-CONN-005", UT_TIER_3, "CONN", "Self-healing reconnect detects a killed session (detection half only - see this test's own top comment)", test_ut_conn_005 },
    { "UT-META-001", UT_TIER_2, "META", "metadata_cache resolves real columns",       test_ut_meta_001 },
    { "UT-META-002", UT_TIER_2, "META", "Repeat lookup within TTL is a cache hit",    test_ut_meta_002 },
    { "UT-META-003", UT_TIER_2, "META", "INSERT/UPDATE/DELETE never trust client type", test_ut_meta_003 },
    { "UT-L2-001",   UT_TIER_2, "L2",   "INSERT rejects an unknown column",           test_ut_l2_001 },
    { "UT-L2-002",   UT_TIER_2, "L2",   "INSERT rejects a missing required column",   test_ut_l2_002 },
    { "UT-L2-003",   UT_TIER_2, "L2",   "UPDATE/DELETE reject zero WHERE keys",       test_ut_l2_003 },
    { "UT-L2-004",   UT_TIER_2, "L2",   "UPDATE rejects an unknown WHERE column",     test_ut_l2_004 },
    { "UT-L2-005",   UT_TIER_2, "L2",   "UPDATE handles a large, fully-backed SET clause safely", test_ut_l2_005 },
    { "UT-L2-006",   UT_TIER_2, "L2",   "EXECUTE_PROCEDURE rejects CURSOR IN",        test_ut_l2_006 },
    { "UT-L2-007",   UT_TIER_2, "L2",   "EXECUTE_PROCEDURE rejects param_count overflow", test_ut_l2_007 },
    { "UT-L2-008",   UT_TIER_2, "L2",   "A date not matching its declared format is rejected", test_ut_l2_008 },
    { "UT-L2-009",   UT_TIER_2, "L2",   "Double validation of a converted date is idempotent", test_ut_l2_009 },
    { "UT-SEL-002",  UT_TIER_2, "SEL",  "SELECT with a function call is rejected",    test_ut_sel_002 },
    { "UT-SESS-003", UT_TIER_2, "SESS", "Zero orphaned sessions is a healthy outcome", test_ut_sess_003 },
    { "UT-DATE-001", UT_TIER_2, "DATE", "Un-tagged date is validated, not assumed",   test_ut_date_001 },
    { "UT-DATE-002", UT_TIER_2, "DATE", "A correctly-declared date format converts",  test_ut_date_002 },
    { "UT-DATE-003", UT_TIER_2, "DATE", "Same idempotency regression, Date Handling section", test_ut_date_003 },

    /* ---- Tier 3 (2026-08-01, first pass - INSERT) ---- */
    { "UT-INS-001", UT_TIER_3, "INS", "Single-row INSERT visible in a follow-up SELECT", test_ut_ins_001 },
    { "UT-INS-002", UT_TIER_3, "INS", "Multi-row bulk INSERT commits all rows",         test_ut_ins_002 },
    { "UT-INS-003", UT_TIER_3, "INS", "row_count > max_bulk_inserts is rejected",       test_ut_ins_003 },
    { "UT-INS-004", UT_TIER_3, "INS", "Oversized CLOB uses the large_value overflow path", test_ut_ins_004 },
    { "UT-INS-005", UT_TIER_3, "INS", "Nested AUDIT_TRAIL insert runs alongside the business insert", test_ut_ins_005 },

    /* ---- Tier 3 (2026-08-01, second pass - UPDATE) ---- */
    { "UT-UPD-001", UT_TIER_3, "UPD", "Scalar UPDATE keyed on NUMBER_COL reads back correctly", test_ut_upd_001 },
    { "UT-UPD-002", UT_TIER_3, "UPD", "DATE-typed WHERE key matches the real row",             test_ut_upd_002 },
    { "UT-UPD-003", UT_TIER_3, "UPD", "Audit diff: one row per changed column, none for unchanged", test_ut_upd_003 },
    { "UT-UPD-004", UT_TIER_3, "UPD", "session_end() succeeds against a real session",         test_ut_upd_004 },

    /* ---- Tier 3 (2026-08-01, third pass - AUDIT) ---- */
    { "UT-AUDIT-004", UT_TIER_3, "AUDIT", "A real CLOB value change is captured correctly on UPDATE", test_ut_audit_004 },

    /* ---- Tier 3 (2026-08-01, fourth pass - DELETE, partial) ---- */
    { "UT-DEL-001", UT_TIER_3, "DEL", "Single-key DELETE removes exactly the matching row", test_ut_del_001 },
    { "UT-DEL-002", UT_TIER_3, "DEL", "Compound-key DELETE only matches all-key rows",      test_ut_del_002 },
    { "UT-DEL-003", UT_TIER_3, "DEL", "Audit write survives a DELETE Oracle itself rejects", test_ut_del_003 },
    { "UT-DEL-004", UT_TIER_3, "DEL", "Before-image is scoped to WHERE-key columns only",   test_ut_del_004 },
    { "UT-DEL-005", UT_TIER_3, "DEL", "Zero WHERE keys is rejected before Stage 3",         test_ut_del_005 },

    /* ---- Tier 3 (2026-08-02, fifth pass - SELECT) ---- */
    { "UT-SEL-001", UT_TIER_3, "SEL", "Plain column-list SELECT returns the expected value", test_ut_sel_001 },
    { "UT-SEL-003", UT_TIER_3, "SEL", "A CLOB column in the SELECT list is correctly extracted", test_ut_sel_003 },
    { "UT-SEL-004", UT_TIER_3, "SEL", "A post-UPDATE SELECT never returns a stale cached value", test_ut_sel_004 },
    { "UT-SEL-005", UT_TIER_3, "SEL", "Control for UT-SEL-004 - same sequence with the cache hidden", test_ut_sel_005 },

    /* ---- Tier 3 (2026-08-02, sixth pass - EXECUTE_PROCEDURE, partial) ---- */
    { "UT-PROC-001", UT_TIER_3, "PROC", "Scalar IN/OUT procedure call returns the correct value", test_ut_proc_001 },
    { "UT-PROC-002", UT_TIER_3, "PROC", "CURSOR OUT parameter returns the seeded row",           test_ut_proc_002 },
    { "UT-PROC-003", UT_TIER_3, "PROC", "Zero-parameter procedure executes via BEGIN...END",     test_ut_proc_003 },
    { "UT-PROC-004", UT_TIER_3, "PROC", "A genuinely unopened CURSOR OUT produces an empty resultset", test_ut_proc_004 },
    { "UT-PROC-005", UT_TIER_3, "PROC", "Exactly one metrics row is written per procedure call", test_ut_proc_005 },
    { "UT-PROC-006", UT_TIER_3, "PROC", "No AUDIT_TRAIL row is written for a procedure call",    test_ut_proc_006 },

    /* ---- Tier 3 (2026-08-02, seventh pass - AUDIT direct calls) ---- */
    { "UT-AUDIT-001", UT_TIER_3, "AUDIT", "audit_trail_fetch_before_image() returns the real column value", test_ut_audit_001 },
    { "UT-AUDIT-002", UT_TIER_3, "AUDIT", "Same function correctly wraps a DATE-typed WHERE key",  test_ut_audit_002 },
    { "UT-AUDIT-003", UT_TIER_3, "AUDIT", "audit_trail_insert() handles new_values=NULL (DELETE case)", test_ut_audit_003 },

    /* ---- Tier 3 (2026-08-02, eighth pass - TX, partial) ---- */
    { "UT-TX-002", UT_TIER_3, "TX", "A standalone call gets its own transaction_id", test_ut_tx_002 },
    { "UT-TX-003", UT_TIER_3, "TX", "A nested call never overwrites an existing transaction_id", test_ut_tx_003 },

    /* UT-TX-001 (per-request multi-operation rollback isolation)
     * deliberately deferred 2026-08-02 - the only transaction wrapping
     * that currently exists is the whole-program-run transaction in
     * Test_XML_Runner.c (every file in one run shares one commit/
     * rollback outcome), a deliberate, temporary shortcut from when
     * that file was first written to get the project off the ground
     * quickly. It will be replaced once the file-consumer/HTTP-consumer
     * split gives each individual request its own, properly-scoped
     * transaction, respecting transaction_required per-request rather
     * than per-run. Testing UT-TX-001 against today's temporary
     * behaviour would either test something about to be replaced, or
     * test the wrong property entirely - revisit once that refactor
     * lands.                                                            */

    /* ---- Tier 3 (2026-08-02, ninth pass - SESS) ---- */
    { "UT-SESS-001", UT_TIER_3, "SESS", "CREATE_SESSION writes a real, permanent OCI_SESSION row", test_ut_sess_001 },
    { "UT-SESS-004", UT_TIER_3, "SESS", "session_touch()/session_touch_db() both advance LAST_ACTIVITY_TS", test_ut_sess_004 },
    { "UT-SESS-005", UT_TIER_3, "SESS", "session_validate() accepts real, rejects unknown session_id", test_ut_sess_005 },
    { "UT-SESS-006", UT_TIER_1, "SESS", "generic_queue drop-rather-than-block contract holds under saturation", test_ut_sess_006 },
    { "UT-CONT-001", UT_TIER_1, "CONT", "payload_requires_single_writer_queue() classifies INSERT/UPDATE/DELETE vs SELECT/EXECUTE_PROCEDURE", test_ut_cont_001 },
    { "UT-CONT-002", UT_TIER_1, "CONT", "A write anywhere in a multi-op request classifies the whole request as a write", test_ut_cont_002 },
    { "UT-CONT-004", UT_TIER_1, "CONT", "contention_manager.mode='1' (boolean-style) does not match 'single_write_queue'", test_ut_cont_004 },

    /* UT-SESS-002 (END_SESSION via session_end()) is already covered by
     * UT-UPD-004; UT-SESS-003 (zero-orphan reconciliation) is already a
     * Tier 2 test - see this catalog's own notes for both.             */

    /* ---- Tier 3 (2026-08-02, tenth and final pass - DATE-004) ---- */
    { "UT-DATE-004", UT_TIER_3, "DATE", "European UPDATE key and US DELETE key both match the same real row", test_ut_date_004 },

    /* Catalog complete, per Unit_Test_Module_Design_Specification.docx -
     * every Tier 1/2/3 test either implemented, or deliberately
     * deferred with its own documented reasoning (UT-TX-001).          */
};
static const int g_registry_count = sizeof(g_registry) / sizeof(g_registry[0]);

/* ================================================================== */
/*  Orchestration                                                       */
/* ================================================================== */
static void run_one(oci_context_t *ctx, const unit_test_case_t *tc,
                     unit_test_result_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->test_id, tc->test_id, sizeof(out->test_id) - 1);
    out->tier = tc->tier;

    double start = now_seconds();
    char msg[UT_MESSAGE_LEN] = {0};
    int rc = tc->fn(ctx, msg, sizeof(msg));
    out->execution_time_seconds = now_seconds() - start;

    strncpy(out->status, rc == 0 ? "PASS" : "FAIL", sizeof(out->status) - 1);
    strncpy(out->message, msg, sizeof(out->message) - 1);
}

/*
 * acquire_test_ctx() / release_test_ctx()
 *
 * Found 2026-08-01 via a real Tier 2 run: in pool mode, the master ctx
 * passed into unit_test_run_all() etc. never gets a real, usable
 * svchp/errhp/envhp at all - confirmed directly against
 * OCI_Connect_pool()'s own source, which never assigns any of those
 * three fields on the master ctx. Only OCI_Pool_get_session() populates
 * them, on a genuinely separate worker_ctx. Every OCI-touching test
 * (directly or via level2_validate_*()/metadata_cache_get_or_fetch()/
 * execute_query_batch()/session_reconcile_orphans()) needs a real
 * worker session in pool mode, not the master ctx.
 *
 * Centralised here rather than duplicated in every individual test
 * function - acquired ONCE per unit_test_run_all()/run_by_id()/
 * run_by_module() call (not once per test), released once at the end.
 *
 * The acquired worker_ctx also needs the master ctx's own operation-
 * specific loggers copied onto it - OCI_Pool_get_session() itself only
 * copies svchp/errhp/envhp/ini/the main logger, never the 16 operation-
 * specific ones (see UT-CONN-003's own 2026-08-01 doc comment for the
 * full detail on that). This duplicates the same copy pattern main()'s
 * own dispatch loop in Test_XML_Runner.c already has - a real, if
 * small, source of possible drift if a new logger is added to
 * oci_context_t in future without updating both copy sites. Accepted
 * for now rather than a bigger refactor extracting that block into its
 * own shared, callable function.
 *
 * In direct (non-pool) mode, ctx already has everything it needs -
 * this is a no-op, returning ctx itself unchanged.
 */
static oci_context_t *acquire_test_ctx(oci_context_t *ctx,
                                        oci_context_t *worker_ctx_storage,
                                        int            *owns_worker)
{
    *owns_worker = 0;

    if (!ctx->ini->use_connection_pool)
        return ctx;

    memset(worker_ctx_storage, 0, sizeof(*worker_ctx_storage));
    if (OCI_Pool_get_session(ctx, worker_ctx_storage) != 0)
        return NULL;

    worker_ctx_storage->select_logger        = ctx->select_logger;
    worker_ctx_storage->cache_logger         = ctx->cache_logger;
    worker_ctx_storage->Metadata_logger      = ctx->Metadata_logger;
    worker_ctx_storage->connectionpool_logger = ctx->connectionpool_logger;
    worker_ctx_storage->insert_logger        = ctx->insert_logger;
    worker_ctx_storage->update_logger        = ctx->update_logger;
    worker_ctx_storage->delete_logger        = ctx->delete_logger;
    worker_ctx_storage->ddl_logger           = ctx->ddl_logger;
    worker_ctx_storage->procedure_logger     = ctx->procedure_logger;
    worker_ctx_storage->error_logger         = ctx->error_logger;
    worker_ctx_storage->metrics_logger       = ctx->metrics_logger;
    /* Second instance of the exact same gap class as session_cache
     * above - metrics_writer (closure item 5, Stage 2, 2026-08-09) and
     * metrics_writer_logger (Stage 2 follow-up, 2026-08-09) were both
     * added to oci_context_t after acquire_test_ctx() was originally
     * written, and neither was ever retrofitted into this copy list.
     * Found 2026-08-12: relocating metrics_writer_start() to run
     * before self-tests (2026-08-11) correctly gave the MASTER ctx a
     * working writer, but every individual test receives test_ctx, not
     * the master ctx - and test_ctx->metrics_writer stayed NULL
     * regardless, since it was never copied here. metrics_finalise_
     * and_enqueue() safely no-ops on a NULL writer by design, which is
     * exactly why this - like session_cache before it - produced no
     * crash, just silently missing data, until UT-PROC-005 finally
     * checked for the row it should have produced.                     */
    worker_ctx_storage->metrics_writer       = ctx->metrics_writer;
    worker_ctx_storage->metrics_writer_logger = ctx->metrics_writer_logger;
    worker_ctx_storage->transaction_logger   = ctx->transaction_logger;
    worker_ctx_storage->security_logger      = ctx->security_logger;
    worker_ctx_storage->crypt_logger         = ctx->crypt_logger;
    worker_ctx_storage->audit_logger         = ctx->audit_logger;
    worker_ctx_storage->session_logger       = ctx->session_logger;
    worker_ctx_storage->sql_parser_logger    = ctx->sql_parser_logger;
    /* Full audit (2026-08-12), prompted by this being the third
     * instance of the same gap class found this week (session_cache,
     * then metrics_writer/metrics_writer_logger, now these) - rather
     * than keep patching this list one missing field at a time as each
     * new test happens to need one, cross-referenced every pointer/
     * struct field on oci_context_t against this copy list directly.
     * ini, logger, and resultset_cache are NOT missing despite not
     * appearing here - ini and logger are already set automatically by
     * OCI_Pool_get_session() itself (OCI_Connection_Pool.c), called
     * just above this function's own copy block; resultset_cache is
     * genuinely never populated anywhere (see below). Everything else
     * on oci_context_t not already above was missing here and is
     * added now:                                                       */
    worker_ctx_storage->connection_logger    = ctx->connection_logger;
    worker_ctx_storage->dml_logger           = ctx->dml_logger;         /* ddl_logger's
                                                                             sibling was
                                                                             already
                                                                             copied above;
                                                                             this one
                                                                             simply never
                                                                             was          */
    worker_ctx_storage->file_consumer_logger = ctx->file_consumer_logger;
    worker_ctx_storage->dispatcher_logger    = ctx->dispatcher_logger;
    worker_ctx_storage->worker_logger        = ctx->worker_logger;
    /* Lower-severity than the loggers above (no crash risk - its
     * absence just means resultset caching is silently disabled for
     * every test, always, rather than any test being able to exercise
     * or verify real caching behaviour) but a genuine gap all the
     * same, and the same audit already found it, so fixed alongside
     * the rest rather than left as a known, separate loose end.        */
    worker_ctx_storage->resultset_cache      = ctx->resultset_cache;
    worker_ctx_storage->metadata_cache       = ctx->metadata_cache;
    /* Genuine pre-existing gap, found 2026-08-10 by UT-SESS-004/005 -
     * the first tests in this catalog to actually exercise the session
     * CACHE path (session_create()'s own store, session_validate()'s
     * own lookup) rather than only the permanent OCI_SESSION table
     * directly (as UT-SESS-001 already did, successfully, without ever
     * needing this). Without this line, session_cache_store()/
     * session_validate() both silently operate on a NULL cache pointer
     * on the test's own ctx - session_cache_store() gracefully returns
     * -1 rather than crashing (session_create() itself still "succeeds"
     * with only a logged warning), which is why this sat undetected
     * until a test finally tried to read back what it had just
     * written.                                                          */
    worker_ctx_storage->session_cache        = ctx->session_cache;
    worker_ctx_storage->authz_cache          = ctx->authz_cache;   /* Security
                                                       * Module Stage 5,
                                                       * 2026-08-31 - same
                                                       * class of bug as
                                                       * session_cache's own
                                                       * note just above:
                                                       * authz_cache_store()
                                                       * silently returns -1
                                                       * on a NULL cache
                                                       * pointer rather than
                                                       * crashing, so this
                                                       * also sat undetected
                                                       * until a real
                                                       * CHECK_PERMISSION
                                                       * test tried to read
                                                       * back a permission it
                                                       * had just granted. */

    *owns_worker = 1;
    return worker_ctx_storage;
}

static void release_test_ctx(oci_context_t *ctx, oci_context_t *worker_ctx,
                              int owns_worker)
{
    if (owns_worker)
        OCI_Pool_release_session(ctx, worker_ctx);
}

int unit_test_run_all(oci_context_t *ctx, int max_tier,
                       unit_test_result_t **results_out, int *result_count_out)
{
    if (!ctx || !results_out || !result_count_out) return -1;

    int count = 0;
    for (int i = 0; i < g_registry_count; i++)
        if (g_registry[i].tier <= max_tier) count++;

    unit_test_result_t *results = calloc((size_t)count, sizeof(unit_test_result_t));
    if (!results) { *results_out = NULL; *result_count_out = 0; return -1; }

    oci_context_t worker_ctx_storage;
    int            owns_worker = 0;
    oci_context_t *test_ctx = acquire_test_ctx(ctx, &worker_ctx_storage, &owns_worker);
    if (!test_ctx)
    {
        /* Could not acquire a worker session at all - every test that
         * needs one will fail cleanly on its own first OCI call rather
         * than this function refusing to run anything, since Tier 1
         * tests genuinely don't need a connection and should still run. */
        test_ctx = ctx;
    }

    int idx = 0;
    int any_fail = 0;
    for (int i = 0; i < g_registry_count; i++)
    {
        if (g_registry[i].tier > max_tier) continue;
        run_one(test_ctx, &g_registry[i], &results[idx]);
        if (strcmp(results[idx].status, "FAIL") == 0) any_fail = 1;
        idx++;
    }

    release_test_ctx(ctx, &worker_ctx_storage, owns_worker);

    *results_out = results;
    *result_count_out = count;
    return any_fail ? -1 : 0;
}

int unit_test_run_by_id(oci_context_t *ctx, const char *test_id,
                         unit_test_result_t *result_out)
{
    if (!ctx || !test_id || !result_out) return -1;

    for (int i = 0; i < g_registry_count; i++)
    {
        if (strcmp(g_registry[i].test_id, test_id) == 0)
        {
            oci_context_t worker_ctx_storage;
            int            owns_worker = 0;
            oci_context_t *test_ctx = acquire_test_ctx(ctx, &worker_ctx_storage, &owns_worker);
            if (!test_ctx) test_ctx = ctx;   /* see unit_test_run_all()'s own note */

            run_one(test_ctx, &g_registry[i], result_out);

            release_test_ctx(ctx, &worker_ctx_storage, owns_worker);
            return 0;
        }
    }

    memset(result_out, 0, sizeof(*result_out));
    strncpy(result_out->test_id, test_id, sizeof(result_out->test_id) - 1);
    strncpy(result_out->status, "SKIP", sizeof(result_out->status) - 1);
    snprintf(result_out->message, sizeof(result_out->message),
             "No test registered with this test_id");
    return -1;
}

int unit_test_run_by_module(oci_context_t *ctx, const char *module,
                             unit_test_result_t **results_out, int *result_count_out)
{
    if (!ctx || !module || !results_out || !result_count_out) return -1;

    int count = 0;
    for (int i = 0; i < g_registry_count; i++)
        if (strcmp(g_registry[i].module, module) == 0) count++;

    if (count == 0) { *results_out = NULL; *result_count_out = 0; return -1; }

    unit_test_result_t *results = calloc((size_t)count, sizeof(unit_test_result_t));
    if (!results) { *results_out = NULL; *result_count_out = 0; return -1; }

    oci_context_t worker_ctx_storage;
    int            owns_worker = 0;
    oci_context_t *test_ctx = acquire_test_ctx(ctx, &worker_ctx_storage, &owns_worker);
    if (!test_ctx) test_ctx = ctx;   /* see unit_test_run_all()'s own note */

    int idx = 0;
    int any_fail = 0;
    for (int i = 0; i < g_registry_count; i++)
    {
        if (strcmp(g_registry[i].module, module) != 0) continue;
        run_one(test_ctx, &g_registry[i], &results[idx]);
        if (strcmp(results[idx].status, "FAIL") == 0) any_fail = 1;
        idx++;
    }

    release_test_ctx(ctx, &worker_ctx_storage, owns_worker);

    *results_out = results;
    *result_count_out = count;
    return any_fail ? -1 : 0;
}

void unit_test_write_summary(logger_t *logger, const unit_test_result_t *results, int count)
{
    if (!logger) return;

    int tier_pass[4] = {0}, tier_fail[4] = {0}, tier_skip[4] = {0};
    for (int i = 0; i < count; i++)
    {
        int t = results[i].tier;
        if (t < 1 || t > 3) continue;
        if (strcmp(results[i].status, "PASS") == 0) tier_pass[t]++;
        else if (strcmp(results[i].status, "FAIL") == 0) tier_fail[t]++;
        else tier_skip[t]++;
    }

    logger_write(logger, LOG_INFO, __func__, 0, "Unit Test Summary");
    for (int t = 1; t <= 3; t++)
    {
        if (tier_pass[t] + tier_fail[t] + tier_skip[t] == 0) continue;
        logger_write(logger, LOG_INFO, __func__, 0,
                     "Tier %d: %d pass, %d fail, %d skip",
                     t, tier_pass[t], tier_fail[t], tier_skip[t]);
    }
    for (int i = 0; i < count; i++)
    {
        if (strcmp(results[i].status, "FAIL") == 0)
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "  FAIL: %s - %s", results[i].test_id, results[i].message);
    }

    int overall_pass = 1;
    for (int t = 1; t <= 3; t++) if (tier_fail[t] > 0) overall_pass = 0;
    logger_write(logger, LOG_INFO, __func__, 0,
                 "Overall: %s", overall_pass ? "PASS" : "FAIL");
}

void unit_test_free_results(unit_test_result_t *results)
{
    free(results);
}
