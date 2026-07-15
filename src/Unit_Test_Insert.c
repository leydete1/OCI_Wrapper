/*
 * Test_Insert_Execute.c
 *
 * Unit test for Stage 3 - execute_insert_batch()
 * ------------------------------------------------
 * Tests all six insert paths in sequence:
 *
 *   Round 1 - Scalar only         (array bind path)
 *   Round 2 - Single BLOB         (file path)
 *   Round 3 - CLOB inline text    (inline string)
 *   Round 4 - CLOB file://        (file path prefix)
 *   Round 5 - Mixed bulk          (CLOB guard -> execute_count=1)
 *   Round 6 - Validation failure  (Stage 1 catches before OCI)
 *
 * Usage:
 *   ./Test_Insert_Execute <config.ini>
 *
 * Compile alongside project objects:
 *   gcc -o Test_Insert_Execute \
 *       Test_Insert_Execute.c \
 *       OCI_Insert_Execute_Module.c \
 *       OCI_Insert_Template_Module.c \
 *       OCI_Insert_Validate_Module.c \
 *       OCI_Table_Metadata_Module.c \
 *       OCI_Connection.c logger.c ini_reader.c \
 *       XML_Helper.c string_utils.c ctx_header.c \
 *       -I. -loci -lpthread -lm
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OCI_Connection.h"
#include "OCI_Insert_Execute_Module.h"
#include "OCI_Insert_Template_Module.h"
#include "OCI_Insert_Validate_Module.h"
#include "logger.h"
#include "ini_reader.h"
#include "ctx_helper.h"

/* ------------------------------------------------------------------ */
/*  Helper: run one test round                                          */
/*  Logs PASS/FAIL clearly so each round is easy to spot in the log.  */
/* ------------------------------------------------------------------ */
static int run_test(oci_context_t    *ctx,
                    const char       *round_name,
                    const char       *template_xml)
{
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "================================================");
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "START: %s", round_name);
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "================================================");

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    int rc = execute_insert_batch(ctx, template_xml, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->logger, LOG_INFO, __func__, 0,
                     "PASS: %s", round_name);
        if (cfg.xml && cfg.xml->OUTPUT_XML)
            logger_write(ctx->logger, LOG_INFO, __func__, 0,
                         "Result XML:\n%s", cfg.xml->OUTPUT_XML);
    }
    else
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "FAIL: %s (rc=%d)", round_name, rc);
    }

    /* Cleanup cfg */
    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "END: %s", round_name);
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "================================================\n");

    return rc;
}

/* ------------------------------------------------------------------ */
/*  Helper: run a test that is EXPECTED to fail (e.g. Round 6)         */
/* ------------------------------------------------------------------ */
static int run_test_expect_fail(oci_context_t *ctx,
                                 const char    *round_name,
                                 const char    *template_xml)
{
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "================================================");
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "START (expect failure): %s", round_name);
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "================================================");

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    int rc = execute_insert_batch(ctx, template_xml, &cfg);

    if (rc != 0)
    {
        logger_write(ctx->logger, LOG_INFO, __func__, 0,
                     "PASS: %s correctly rejected (rc=%d)",
                     round_name, rc);
        rc = 0;   /* test passed - failure was expected */
    }
    else
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "FAIL: %s should have been rejected but passed",
                     round_name);
        rc = -1;
    }

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "END: %s", round_name);
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "================================================\n");

    return rc;
}

/* ==================================================================
 *  main
 * ================================================================== */
int main_bak1(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s <config.ini>\n", argv[0]);
        return -1;
    }

    /* ---- Initialise context - same pattern as Test3.c ---- */
    app_config_t  config;
    oci_context_t ctx;
    logger_t      logger;

    memset(&ctx,    0, sizeof(ctx));
    memset(&config, 0, sizeof(config));
    ctx.ini = &config;

    if (load_ini(argv[1], &config, &ctx) != 0)
    {
        printf("Failed to load ini file: %s\n", argv[1]);
        return -1;
    }

    if (logger_init(&logger,
                    ctx.ini->log_file_name,
                    ctx.ini->log_file_max_size,
                    ctx.ini->log_file_rotation_number,
                    ctx.ini->log_level_num) != 0)
    {
        printf("Failed to initialise logger\n");
        return -1;
    }
    ctx.logger          = &logger;
    ctx.NLS_DATE_FORMAT = "YYYY-MM-DD HH24:MI:SS";

    logger_dump_ctx(&ctx);

    if (OCI_Connect(&ctx) != 0)
    {
        logger_write(&logger, LOG_ERROR, __func__, 0,
                     "Database connection failed");
        logger_close(&logger);
        return -1;
    }

    logger_write(&logger, LOG_INFO, __func__, 0,
                 "========================================");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "execute_insert_batch Unit Test Suite");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "========================================\n");

    int total  = 0;
    int passed = 0;

    /* ================================================================
     *  Round 1 - Scalar only
     *  Tests the array bind path with no LOB columns.
     *  Two rows in one batch - exercises bulk insert.
     * ================================================================ */
    const char *round1_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Insert_Template>\n"
        "  <operation>INSERT</operation>\n"
        "  <table_name>OCI_FIELD_TEST</table_name>\n"
        "  <owner>DATA_MANAGER</owner>\n"
        "  <row number=\"1\">\n"
        "    <field>\n"
        "      <field_number>1</field_number>\n"
        "      <field_name>NUMBER_COL</field_name>\n"
        "      <field_type>NUMBER</field_type>\n"
        "      <field_length>22</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>42</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>2</field_number>\n"
        "      <field_name>VARCHAR2_COL</field_name>\n"
        "      <field_type>VARCHAR2</field_type>\n"
        "      <field_length>100</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>Round 1 scalar test row 1</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>3</field_number>\n"
        "      <field_name>DATE_COL</field_name>\n"
        "      <field_type>DATE</field_type>\n"
        "      <field_length>7</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>2026-05-20</insert_value>\n"
        "    </field>\n"
        "  </row>\n"
        "  <row number=\"2\">\n"
        "    <field>\n"
        "      <field_number>1</field_number>\n"
        "      <field_name>NUMBER_COL</field_name>\n"
        "      <field_type>NUMBER</field_type>\n"
        "      <field_length>22</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>99</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>2</field_number>\n"
        "      <field_name>VARCHAR2_COL</field_name>\n"
        "      <field_type>VARCHAR2</field_type>\n"
        "      <field_length>100</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>Round 1 scalar test row 2</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>3</field_number>\n"
        "      <field_name>DATE_COL</field_name>\n"
        "      <field_type>DATE</field_type>\n"
        "      <field_length>7</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>2026-05-20</insert_value>\n"
        "    </field>\n"
        "  </row>\n"
        "  <column_count>3</column_count>\n"
        "</Insert_Template>\n";

    total++;
    if (run_test(&ctx, "Round 1 - Scalar only (2 rows bulk)", round1_xml) == 0)
        passed++;

    /* ================================================================
     *  Round 2 - Single BLOB from file
     *  insert_value = full path to a real file on disk.
     *  Update the path below to a file that exists on your system.
     * ================================================================ */
    const char *round2_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Insert_Template>\n"
        "  <operation>INSERT</operation>\n"
        "  <table_name>OCI_FIELD_TEST</table_name>\n"
        "  <owner>DATA_MANAGER</owner>\n"
        "  <row number=\"1\">\n"
        "    <field>\n"
        "      <field_number>1</field_number>\n"
        "      <field_name>NUMBER_COL</field_name>\n"
        "      <field_type>NUMBER</field_type>\n"
        "      <field_length>22</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>200</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>2</field_number>\n"
        "      <field_name>VARCHAR2_COL</field_name>\n"
        "      <field_type>VARCHAR2</field_type>\n"
        "      <field_length>100</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>Round 2 BLOB test</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>3</field_number>\n"
        "      <field_name>BLOB_COL</field_name>\n"
        "      <field_type>BLOB</field_type>\n"
        "      <field_length>4000</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>/home/leyden100/eclipse-workspace/OCI_Wrapper/test_data/test.bin</insert_value>\n"
        "    </field>\n"
        "  </row>\n"
        "  <column_count>3</column_count>\n"
        "</Insert_Template>\n";

    total++;
    if (run_test(&ctx, "Round 2 - Single BLOB from file", round2_xml) == 0)
        passed++;

    /* ================================================================
     *  Round 3 - CLOB inline text
     *  insert_value = plain text string, no file:// prefix.
     * ================================================================ */
    const char *round3_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Insert_Template>\n"
        "  <operation>INSERT</operation>\n"
        "  <table_name>OCI_FIELD_TEST</table_name>\n"
        "  <owner>DATA_MANAGER</owner>\n"
        "  <row number=\"1\">\n"
        "    <field>\n"
        "      <field_number>1</field_number>\n"
        "      <field_name>NUMBER_COL</field_name>\n"
        "      <field_type>NUMBER</field_type>\n"
        "      <field_length>22</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>300</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>2</field_number>\n"
        "      <field_name>CLOB_COL</field_name>\n"
        "      <field_type>CLOB</field_type>\n"
        "      <field_length>4000</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>This is inline CLOB text for Round 3. "
                            "The quick brown fox jumps over the lazy dog. "
                            "Testing chunked CLOB write path.</insert_value>\n"
        "    </field>\n"
        "  </row>\n"
        "  <column_count>2</column_count>\n"
        "</Insert_Template>\n";

    total++;
    if (run_test(&ctx, "Round 3 - CLOB inline text", round3_xml) == 0)
        passed++;

    /* ================================================================
     *  Round 4 - CLOB from file:// path
     *  insert_value starts with "file://" -> read from disk.
     *  Update the path below to a text file that exists on your system.
     * ================================================================ */
    const char *round4_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Insert_Template>\n"
        "  <operation>INSERT</operation>\n"
        "  <table_name>OCI_FIELD_TEST</table_name>\n"
        "  <owner>DATA_MANAGER</owner>\n"
        "  <row number=\"1\">\n"
        "    <field>\n"
        "      <field_number>1</field_number>\n"
        "      <field_name>NUMBER_COL</field_name>\n"
        "      <field_type>NUMBER</field_type>\n"
        "      <field_length>22</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>400</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>2</field_number>\n"
        "      <field_name>CLOB_COL</field_name>\n"
        "      <field_type>CLOB</field_type>\n"
        "      <field_length>4000</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>file:///home/leyden100/eclipse-workspace/OCI_Wrapper/test_data/test_clob.txt</insert_value>\n"
        "    </field>\n"
        "  </row>\n"
        "  <column_count>2</column_count>\n"
        "</Insert_Template>\n";

    total++;
    if (run_test(&ctx, "Round 4 - CLOB from file:// path", round4_xml) == 0)
        passed++;

    /* ================================================================
     *  Round 5 - Mixed bulk: scalar + CLOB
     *  CLOB present -> CLOB guard fires -> execute_count=1
     *  Watch for "forcing execute_count=1" in log.
     * ================================================================ */
    const char *round5_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Insert_Template>\n"
        "  <operation>INSERT</operation>\n"
        "  <table_name>OCI_FIELD_TEST</table_name>\n"
        "  <owner>DATA_MANAGER</owner>\n"
        "  <row number=\"1\">\n"
        "    <field>\n"
        "      <field_number>1</field_number>\n"
        "      <field_name>NUMBER_COL</field_name>\n"
        "      <field_type>NUMBER</field_type>\n"
        "      <field_length>22</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>501</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>2</field_number>\n"
        "      <field_name>VARCHAR2_COL</field_name>\n"
        "      <field_type>VARCHAR2</field_type>\n"
        "      <field_length>100</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>Round 5 mixed row 1</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>3</field_number>\n"
        "      <field_name>CLOB_COL</field_name>\n"
        "      <field_type>CLOB</field_type>\n"
        "      <field_length>4000</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>CLOB content row 1 - mixed bulk test</insert_value>\n"
        "    </field>\n"
        "  </row>\n"
        "  <row number=\"2\">\n"
        "    <field>\n"
        "      <field_number>1</field_number>\n"
        "      <field_name>NUMBER_COL</field_name>\n"
        "      <field_type>NUMBER</field_type>\n"
        "      <field_length>22</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>502</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>2</field_number>\n"
        "      <field_name>VARCHAR2_COL</field_name>\n"
        "      <field_type>VARCHAR2</field_type>\n"
        "      <field_length>100</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>Round 5 mixed row 2</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>3</field_number>\n"
        "      <field_name>CLOB_COL</field_name>\n"
        "      <field_type>CLOB</field_type>\n"
        "      <field_length>4000</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>CLOB content row 2 - mixed bulk test</insert_value>\n"
        "    </field>\n"
        "  </row>\n"
        "  <column_count>3</column_count>\n"
        "</Insert_Template>\n";

    total++;
    if (run_test(&ctx, "Round 5 - Mixed bulk scalar+CLOB (CLOB guard)", round5_xml) == 0)
        passed++;

    /* ================================================================
     *  Round 6 - Validation failure
     *  VARCHAR2_COL value exceeds field_length=100.
     *  Stage 1 must catch this BEFORE any OCI call.
     *  Watch log for "Stage 1 validation failed" - no OCI calls after.
     * ================================================================ */
    const char *round6_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Insert_Template>\n"
        "  <operation>INSERT</operation>\n"
        "  <table_name>OCI_FIELD_TEST</table_name>\n"
        "  <owner>DATA_MANAGER</owner>\n"
        "  <row number=\"1\">\n"
        "    <field>\n"
        "      <field_number>1</field_number>\n"
        "      <field_name>NUMBER_COL</field_name>\n"
        "      <field_type>NUMBER</field_type>\n"
        "      <field_length>22</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>Y</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>999</insert_value>\n"
        "    </field>\n"
        "    <field>\n"
        "      <field_number>2</field_number>\n"
        "      <field_name>VARCHAR2_COL</field_name>\n"
        "      <field_type>VARCHAR2</field_type>\n"
        "      <field_length>100</field_length>\n"
        "      <field_precision>-1</field_precision>\n"
        "      <field_scale>-1</field_scale>\n"
        "      <field_nullable>N</field_nullable>\n"
        "      <field_default></field_default>\n"
        "      <insert_value>THIS VALUE IS DELIBERATELY TOO LONG - "
                            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                            "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
                            "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"
                            "</insert_value>\n"
        "    </field>\n"
        "  </row>\n"
        "  <column_count>2</column_count>\n"
        "</Insert_Template>\n";

    total++;
    if (run_test_expect_fail(&ctx,
                              "Round 6 - Validation failure (value too long)",
                              round6_xml) == 0)
        passed++;

    /* ================================================================
     *  Summary
     * ================================================================ */
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "========================================");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "Test Suite Complete: %d/%d passed", passed, total);
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "========================================");

    OCI_Disconnect(&ctx);
    logger_close(&logger);

    return (passed == total) ? 0 : -1;
}
