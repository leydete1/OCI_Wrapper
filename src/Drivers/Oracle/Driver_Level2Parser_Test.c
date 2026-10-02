/*
 * Driver_Level2Parser_Test.c
 *
 * Follow-up proposal item 2 (2026-09-20) - standalone-harness pass for
 * OCI_Level2_Parser.c, third module after OCI_Table_Metadata_Module.c
 * (Driver_Metadata_Test.c, bug found and fixed) and
 * OCI_Transaction_Manager.c (Driver_Transaction_Test.c, clean pass).
 *
 * Scope note (see OCI_Level2_Parser.h's own header comment):
 * level2_validate_select() is deliberately connection-free - pure
 * syntax/structure analysis via extract_sql_dependencies(), already
 * covered by sql_dependency_extractor's own tests, out of scope here.
 * level2_validate_insert()/update()/delete() DO touch the database, via
 * a single shared static helper, normalize_client_date_value() - all 7
 * raw OCI calls in this module live there (a self-contained
 * SELECT TO_CHAR(TO_DATE(:1,:2),:3) FROM DUAL round trip used to
 * validate/canonicalise a client-supplied date string). This harness
 * exercises that helper through its real, public entry point -
 * level2_validate_insert() - rather than calling it directly (it's
 * static), which also happens to be exactly how it's actually invoked
 * in production (OCI_Insert_Template_Module.c/dispatcher.c never call
 * normalize_client_date_value() itself).
 *
 * No DB writes happen anywhere in this file. level2_validate_insert()
 * is validation-only - it resolves metadata via ctx->metadata_cache and
 * runs the date round-trip SELECT, but never executes the INSERT
 * itself (that's execute_insert_batch()'s job, a different module).
 * No baseline row-count helpers, no cleanup step needed - unlike
 * Driver_Transaction_Test.c, this table is never actually written to.
 *
 * Tests:
 *
 *   Test 1  - row_count=0: LEVEL2_ERR_ROW_COUNT_EXCEEDED. Pure struct
 *             check, no connection touched - confirms the
 *             connection-free checks still run before anything OCI-
 *             related, per Check 1's documented ordering.
 *   Test 2  - two rows with different column sets: LEVEL2_ERR_FIELD_
 *             INVALID (Check 1b). Also connection-free, also checked
 *             before the metadata_cache lookup, per the header's own
 *             ordering.
 *   Test 3  - unknown column name ('BOGUS_COL'): LEVEL2_ERR_FIELD_
 *             INVALID, "no such column". Proves the metadata_cache
 *             round trip itself works (a real column set has to come
 *             back correctly for "BOGUS_COL isn't in it" to be the
 *             actual reason this fails, not a metadata lookup error).
 *   Test 4  - NOT NULL column (NUMBER_COL) omitted entirely:
 *             LEVEL2_ERR_FIELD_INVALID, Check 4's "not supplied"
 *             message.
 *   Test 5  - the real target of this harness: DATE_COL set with an
 *             explicit client_date_format ("DD/MM/YYYY") on a
 *             genuinely valid date string ("25/12/2026") - must
 *             normalize cleanly to the canonical nls_date_format
 *             (config.ini) and return LEVEL2_OK. This is
 *             normalize_client_date_value()'s OCI round trip actually
 *             succeeding end to end.
 *   Test 6  - the failure-path mirror of Test 5: client_date_format
 *             "DD/MM/YYYY" with a value that isn't a valid date at all
 *             ("not-a-date") - Oracle must reject the TO_DATE() call
 *             (ORA-01858/ORA-01861), and that must surface as
 *             LEVEL2_ERR_FIELD_INVALID with the OCI error detail in
 *             error_detail->error_text, not a crash or a silently
 *             accepted bad value.
 *   Test 7  - DATE_COL set with NO client_date_format, value already
 *             in the canonical nls_date_format - exercises the
 *             source_fmt == canonical_fmt branch (source_fmt defaults
 *             to canonical_fmt when client_date_format is empty, per
 *             normalize_client_date_value()'s own doc comment) -
 *             LEVEL2_OK.
 *
 * Oracle dialect extraction, Stage 1 (2026-09-30). Level 2 no longer
 * asks Oracle to convert dates - normalize_client_date_value() now
 * calls date_normalize() (plain C). Tests 5-7 above still run through
 * level2_validate_insert() and now exercise the C path. Tests 8-10 are
 * the Stage 1 proof:
 *
 *   Test 8  - db_type_class() against REAL metadata: every column of
 *             UNIT_TEST_FIELD_TEST and OCI_FIELD_TEST (which has the
 *             TIMESTAMP WITH TIME ZONE / WITH LOCAL TIME ZONE columns,
 *             proposal 0.9) must get the same date/timestamp decision
 *             as the old strcmp(type,"DATE") / strncmp(type,"TIMESTAMP",9)
 *             rule.
 *   Test 9  - DIFFERENTIAL: the old Oracle path (a verbatim copy of the
 *             pre-Stage-1 round trip, old_oracle_normalize() below,
 *             SELECT TO_CHAR(TO_DATE(:1,:2),:3) FROM DUAL) and the new
 *             date_normalize() run over the same corpus - valid values,
 *             bad day/month, 29-Feb leap and non-leap (Julian and
 *             Gregorian), the 1582 gap, every mask element, TIMESTAMP
 *             fractions of 0-9 digits, and the leniency cases (short
 *             fields, separator variation, omitted separators, omitted
 *             trailing time, leading/trailing blanks). Every case must
 *             give the same accept/reject decision, and accepted cases
 *             the same output string, byte for byte. Mismatches are
 *             listed one per line ("MISMATCH ...") in this program's
 *             output. Cases marked "unsupported" use a mask element
 *             Stage 1 deliberately does not support (DY, FX): these
 *             pass if the new path rejects the MASK, whatever Oracle
 *             did, and are counted separately.
 *   Test 10 - rejection text (proposal decision 0.4): for a rejected
 *             value, level2_validate_insert()'s message must start with
 *             exactly the same text as the old Oracle path's message up
 *             to the " (" before the reason, and must not contain
 *             "ORA-".
 *
 * Uses UNIT_TEST_FIELD_TEST (NUMBER_COL NOT NULL/PK, VARCHAR2_COL,
 * DATE_COL, CLOB_COL - all three non-key columns nullable, no
 * defaults - see Driver_Metadata_Test.c's Test 1 output) so Tests 5-7
 * only ever need to set NUMBER_COL + DATE_COL, nothing else.
 *
 * Build (same convention as Driver_Metadata_Test.c/Driver_Transaction_
 * Test.c - this module pulls in metadata_cache.c/metadata_cache_meta.c
 * and OCI_Insert_Validate_Module.c as real dependencies, not stubs,
 * since Test 3/4/5/6/7 all genuinely exercise them):
 *
 *   gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
 *       -I/usr/include/cjson -I/usr/include/libxml2 \
 *       -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include -I. \
 *       -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
 *       -o Driver_Level2Parser_Test \
 *       Driver_Level2Parser_Test.c db_driver.c driver_oracle.c \
 *       OCI_Connection.c OCI_Connection_Pool.c oci_cache.c string_utils.c \
 *       ini_reader.c logger.c metrics.c ctx_utils.c \
 *       OCI_Level2_Parser.c OCI_Table_Metadata_Module.c \
 *       OCI_Insert_Validate_Module.c metadata_cache.c metadata_cache_meta.c \
 *       sql_dependency_extractor.c \
 *       -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
 *       -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson \
 *       -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
 *
 * Run (same LD_LIBRARY_PATH/LSAN_OPTIONS pattern as Run_Manually.sh):
 *   ./Driver_Level2Parser_Test
 *
 * Expected output on success:
 *   Test 1 (row_count=0)                        ... OK (LEVEL2_ERR_ROW_COUNT_EXCEEDED)
 *   Test 2 (row column set mismatch)            ... OK (LEVEL2_ERR_FIELD_INVALID)
 *   Test 3 (unknown column)                     ... OK (LEVEL2_ERR_FIELD_INVALID)
 *   Test 4 (NOT NULL column omitted)            ... OK (LEVEL2_ERR_FIELD_INVALID)
 *   Test 5 (date normalize, valid + format)     ... OK (LEVEL2_OK)
 *   Test 6 (date normalize, invalid value)      ... OK (LEVEL2_ERR_FIELD_INVALID)
 *   Test 7 (date normalize, no client format)   ... OK (LEVEL2_OK)
 *   Test 8 (type class vs old rule, real metadata) ... OK (n columns, ...)
 *   Test 9 (differential old Oracle vs new C, n cases) ... OK (...)
 *   Test 10 (rejection text prefix, no ORA suffix) ... OK
 *   PASS
 *
 * A non-zero exit code means at least one test failed - check
 * config.insert_log_file_name (Level 2's INSERT checks all log there,
 * matching every existing Insert-path log call per the header) and
 * config.Metadata_log_file_name/config.cache_log_file_name for the
 * metadata_cache_get_or_fetch() round trip specifically.
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"
#define TEST_TABLE "UNIT_TEST_FIELD_TEST"
#define TEST_OWNER "DATA_MANAGER"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "Connection.h"
#include "Connection_Pool.h"
#include "Level2_Parser.h"
#include "Insert_Execute_Module.h"
#include "Request_Response_Types.h"
#include "Table_Metadata_Module.h"   /* get_request_metadata() - Test 8 */
#include "metadata_cache.h"
#include "date_normalize.h"          /* Stage 1 - Tests 8-10           */
#include "db_type_class.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *main_logger,
                     logger_t *connection_logger,
                     logger_t *connectionpool_logger,
                     logger_t *insert_logger,
                     logger_t *metadata_logger,
                     logger_t *cache_logger)
{
    memset(ctx,    0, sizeof(*ctx));
    memset(config, 0, sizeof(*config));
    ctx->ini             = config;
    ctx->pool_slot_index = -1;

    if (load_ini(CONFIG_INI, config, ctx) != 0)
    {
        fprintf(stderr, "Failed to load ini file: %s\n", CONFIG_INI);
        return -1;
    }

    if (logger_init_str(error_logger, config->error_log_file_name,
                         config->error_log_file_max_size,
                         config->error_log_file_rotation_number,
                         config->error_log_level) != 0)
    { fprintf(stderr, "Failed to init error_logger\n"); return -1; }
    ctx->error_logger = error_logger;

    if (logger_init_str2(main_logger, config->log_file_name,
                          config->log_file_max_size,
                          config->log_file_rotation_number,
                          config->log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init main logger\n"); return -1; }
    ctx->logger = main_logger;

    if (logger_init_str2(connection_logger, config->connection_log_file_name,
                          config->connection_log_file_max_size,
                          config->connection_log_file_rotation_number,
                          config->connection_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init connection_logger\n"); return -1; }
    ctx->connection_logger = connection_logger;

    if (logger_init_str2(connectionpool_logger, config->connectionpool_log_file_name,
                          config->connectionpool_log_file_max_size,
                          config->connectionpool_log_file_rotation_number,
                          config->connectionpool_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init connectionpool_logger\n"); return -1; }
    ctx->connectionpool_logger = connectionpool_logger;

    if (logger_init_str2(insert_logger, config->insert_log_file_name,
                          config->insert_log_file_max_size,
                          config->insert_log_file_rotation_number,
                          config->insert_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init insert_logger\n"); return -1; }
    ctx->insert_logger = insert_logger;

    if (logger_init_str2(metadata_logger, config->Metadata_log_file_name,
                          config->Metadata_log_file_max_size,
                          config->Metadata_log_file_rotation_number,
                          config->Metadata_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init Metadata_logger\n"); return -1; }
    ctx->Metadata_logger = metadata_logger;

    if (logger_init_str2(cache_logger, config->cache_log_file_name,
                          config->cache_log_file_max_size,
                          config->cache_log_file_rotation_number,
                          config->cache_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init cache_logger\n"); return -1; }
    ctx->cache_logger = cache_logger;

    return 0;
}

/* One field_value_t, zero-initialised, with the given name/value and
 * optional client_date_format ("" for none). */
static field_value_t make_field(const char *name, const char *value,
                                 const char *date_fmt)
{
    field_value_t fv;
    memset(&fv, 0, sizeof(fv));
    strncpy(fv.field_name, name, sizeof(fv.field_name) - 1);
    strncpy(fv.value,      value, sizeof(fv.value) - 1);
    if (date_fmt)
        strncpy(fv.client_date_format, date_fmt, sizeof(fv.client_date_format) - 1);
    return fv;
}

/* ================================================================== */
/*  Stage 1 - the OLD Oracle date path, kept here only as the          */
/*  reference for Test 9/10. Verbatim logic of the pre-Stage-1         */
/*  normalize_client_date_value() (Level2_Parser.c, 29 Sep baseline):  */
/*  same SQL, same binds, same canonical format built from             */
/*  nls_date_format, same error text. It no longer exists anywhere     */
/*  else in the code base.                                             */
/* ================================================================== */
static int old_oracle_normalize(oci_context_t *ctx,
                                const char *value_in,
                                const char *client_date_format,
                                int         is_timestamp,
                                char       *out, size_t out_size,
                                char       *err_msg, size_t err_msg_max)
{
    char value[1024];
    snprintf(value, sizeof(value), "%s", value_in);
    out[0] = '\0';
    err_msg[0] = '\0';

    char canonical_fmt[80];
    if (is_timestamp)
        snprintf(canonical_fmt, sizeof(canonical_fmt), "%s.FF6",
                 ctx->ini->nls_date_format);
    else
        snprintf(canonical_fmt, sizeof(canonical_fmt), "%s",
                 ctx->ini->nls_date_format);

    const char *source_fmt = (client_date_format && client_date_format[0])
                              ? client_date_format
                              : canonical_fmt;

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT TO_CHAR(%s(:1,:2),:3) FROM DUAL",
             is_timestamp ? "TO_TIMESTAMP" : "TO_DATE");

    OCIStmt *stmt = NULL;
    OCIBind *bnd1 = NULL, *bnd2 = NULL, *bnd3 = NULL;
    OCIDefine *dfn = NULL;
    char     result_buf[128] = {0};
    sb2      result_ind = 0;
    int      rc = 0;

    sword status = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                                    (text *)sql, (ub4)strlen(sql),
                                    NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO)
    {
        snprintf(err_msg, err_msg_max,
                 "Internal error preparing date normalization query");
        return -2;   /* harness-level failure, not a date decision */
    }

    OCIBindByPos(stmt, &bnd1, ctx->errhp, 1,
                 (dvoid *)value, (sb4)strlen(value) + 1,
                 SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
    OCIBindByPos(stmt, &bnd2, ctx->errhp, 2,
                 (dvoid *)source_fmt, (sb4)strlen(source_fmt) + 1,
                 SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);
    OCIBindByPos(stmt, &bnd3, ctx->errhp, 3,
                 (dvoid *)canonical_fmt, (sb4)strlen(canonical_fmt) + 1,
                 SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT);

    OCIDefineByPos(stmt, &dfn, ctx->errhp, 1,
                   (dvoid *)result_buf, (sb4)sizeof(result_buf),
                   SQLT_STR, &result_ind, NULL, NULL, OCI_DEFAULT);

    status = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp, 1, 0,
                             NULL, NULL, OCI_DEFAULT);

    if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO)
    {
        text errbuf[512];
        sb4  errcode = 0;
        OCIErrorGet(ctx->errhp, 1, NULL, &errcode, errbuf, sizeof(errbuf),
                    OCI_HTYPE_ERROR);
        size_t el = strlen((char *)errbuf);
        while (el > 0 && (errbuf[el - 1] == '\n' || errbuf[el - 1] == '\r'))
            errbuf[--el] = '\0';
        snprintf(err_msg, err_msg_max,
                 "Invalid date: value='%.80s' does not match "
                 "%s='%s' (ORA-%05d: %.200s)",
                 value,
                 (client_date_format && client_date_format[0])
                     ? "client_date_format" : "nls_date_format",
                 source_fmt, errcode, (char *)errbuf);
        rc = -1;
    }
    else
    {
        snprintf(out, out_size, "%s", result_buf);
    }

    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return rc;
}

/* Test 9 corpus. mask "" = no client_date_format (canonical path).
 * unsupported = 1: a mask element Stage 1 deliberately rejects. */
typedef struct {
    const char *value;
    const char *mask;
    int         is_timestamp;
    int         unsupported;
} date_case_t;

static const date_case_t DATE_CORPUS[] = {
    /* ---- DD/MM/YYYY: basics, short fields, separators ---- */
    { "25/12/2026",            "DD/MM/YYYY", 0, 0 },
    { "5/1/2026",              "DD/MM/YYYY", 0, 0 },
    { "05-01-2026",            "DD/MM/YYYY", 0, 0 },
    { "05 01 2026",            "DD/MM/YYYY", 0, 0 },
    { "05.01.2026",            "DD/MM/YYYY", 0, 0 },
    { "25//12//2026",          "DD/MM/YYYY", 0, 0 },
    { "25 /12/2026",           "DD/MM/YYYY", 0, 0 },   /* 2 chars, 1-char sep */
    { "25/12 2026",            "DD/MM/YYYY", 0, 0 },
    { "25 / 12 / 2026",        "DD/MM/YYYY", 0, 0 },   /* spaces around sep  */
    { "25/-12/2026",           "DD/MM/YYYY", 0, 0 },   /* 2 punct, 1-char sep */
    { "05012026",              "DD/MM/YYYY", 0, 0 },
    { "5012026",               "DD/MM/YYYY", 0, 0 },
    { " 25/12/2026",           "DD/MM/YYYY", 0, 0 },
    { "25/12/2026 ",           "DD/MM/YYYY", 0, 0 },
    { "25/12/2026 10:30",      "DD/MM/YYYY", 0, 0 },
    { "25/12",                 "DD/MM/YYYY", 0, 0 },
    { "25",                    "DD/MM/YYYY", 0, 0 },
    { "25-12-2026T",           "DD/MM/YYYY", 0, 0 },
    { "not-a-date",            "DD/MM/YYYY", 0, 0 },
    /* ---- ranges ---- */
    { "31/12/2026",            "DD/MM/YYYY", 0, 0 },
    { "31/04/2026",            "DD/MM/YYYY", 0, 0 },
    { "32/01/2026",            "DD/MM/YYYY", 0, 0 },
    { "00/01/2026",            "DD/MM/YYYY", 0, 0 },
    { "15/13/2026",            "DD/MM/YYYY", 0, 0 },
    { "15/00/2026",            "DD/MM/YYYY", 0, 0 },
    { "25/12/26",              "DD/MM/YYYY", 0, 0 },
    { "25/12/0000",            "DD/MM/YYYY", 0, 0 },
    { "25/12/0001",            "DD/MM/YYYY", 0, 0 },
    { "31/12/9999",            "DD/MM/YYYY", 0, 0 },
    { "25/12/99999",           "DD/MM/YYYY", 0, 0 },
    /* ---- leap years, Julian/Gregorian, the 1582 gap ---- */
    { "29/02/2024",            "DD/MM/YYYY", 0, 0 },
    { "29/02/2025",            "DD/MM/YYYY", 0, 0 },
    { "29/02/2000",            "DD/MM/YYYY", 0, 0 },
    { "29/02/1900",            "DD/MM/YYYY", 0, 0 },
    { "29/02/1500",            "DD/MM/YYYY", 0, 0 },
    { "04/10/1582",            "DD/MM/YYYY", 0, 0 },
    { "10/10/1582",            "DD/MM/YYYY", 0, 0 },
    { "15/10/1582",            "DD/MM/YYYY", 0, 0 },
    { "05/10/1582",            "DD/MM/YYYY", 0, 0 },   /* gap: first day  */
    { "14/10/1582",            "DD/MM/YYYY", 0, 0 },   /* gap: last day   */
    /* ---- month names, MM alternative ---- */
    { "25/DEC/2026",           "DD/MM/YYYY", 0, 0 },
    { "25/December/2026",      "DD/MM/YYYY", 0, 0 },
    { "15-JUN-2026",           "DD-MON-YYYY", 0, 0 },
    { "15-jun-2026",           "DD-MON-YYYY", 0, 0 },
    { "15-June-2026",          "DD-MON-YYYY", 0, 0 },
    { "15-JUX-2026",           "DD-MON-YYYY", 0, 0 },
    { "15-06-2026",            "DD-MON-YYYY", 0, 0 },
    { "15 September 2026",     "DD MONTH YYYY", 0, 0 },
    /* ---- US order, and the E2E pair (Request Definitions, Date Handling) ---- */
    { "12/25/2026",            "MM/DD/YYYY", 0, 0 },
    { "25/12/2026",            "MM/DD/YYYY", 0, 0 },
    { "08/19/2026 14:30:00",   "MM/DD/YYYY HH24:MI:SS", 0, 0 },
    { "19/08/2026 14:30:00",   "DD/MM/YYYY HH24:MI:SS", 0, 0 },
    { "19/08/2026 14:30",      "DD/MM/YYYY HH24:MI:SS", 0, 0 },
    { "19/08/2026",            "DD/MM/YYYY HH24:MI:SS", 0, 0 },
    { "19/08/2026 7:5:3",      "DD/MM/YYYY HH24:MI:SS", 0, 0 },
    { "19/08/2026 24:00:00",   "DD/MM/YYYY HH24:MI:SS", 0, 0 },
    { "19/08/2026 23:60:00",   "DD/MM/YYYY HH24:MI:SS", 0, 0 },
    { "19/08/2026 23:59:60",   "DD/MM/YYYY HH24:MI:SS", 0, 0 },
    /* ---- no client format: validated against nls_date_format ---- */
    { "2026-06-15 00:00:00",   "", 0, 0 },
    { "2026-06-15",            "", 0, 0 },
    { "2026-06-15 10",         "", 0, 0 },
    { "2026-06-15 10:30",      "", 0, 0 },
    { "2026-6-5 1:2:3",        "", 0, 0 },
    { "2026/06/15 10:00:00",   "", 0, 0 },
    { "20260615",              "", 0, 0 },
    { "20260615103000",        "", 0, 0 },
    { "2026-06-15T10:00:00",   "", 0, 0 },
    { "2026-06-15  10:00:00",  "", 0, 0 },   /* two spaces, 1-char sep */
    { "2026-06-15 10:00:00.5", "", 0, 0 },
    { "19/08/2026",            "", 0, 0 },   /* UT-DATE-001 */
    /* ---- two-digit years ---- */
    { "25/12/26",              "DD/MM/YY", 0, 0 },
    { "25/12/99",              "DD/MM/YY", 0, 0 },
    { "25/12/2026",            "DD/MM/YY", 0, 0 },
    { "25/12/99",              "DD/MM/RR", 0, 0 },
    { "25/12/49",              "DD/MM/RR", 0, 0 },
    { "25/12/50",              "DD/MM/RR", 0, 0 },
    { "25/12/2026",            "DD/MM/RR", 0, 0 },
    { "25/12/99",              "DD/MM/RRRR", 0, 0 },
    { "25/12/1999",            "DD/MM/RRRR", 0, 0 },
    /* ---- 12-hour clock ---- */
    { "25/12/2026 01:30 PM",   "DD/MM/YYYY HH:MI AM", 0, 0 },
    { "25/12/2026 01:30 AM",   "DD/MM/YYYY HH:MI PM", 0, 0 },
    { "25/12/2026 12:15 AM",   "DD/MM/YYYY HH:MI AM", 0, 0 },
    { "25/12/2026 12:15 PM",   "DD/MM/YYYY HH:MI AM", 0, 0 },
    { "25/12/2026 12:15 p.m.", "DD/MM/YYYY HH:MI A.M.", 0, 0 },
    { "25/12/2026 13:15 PM",   "DD/MM/YYYY HH:MI AM", 0, 0 },
    { "25/12/2026 00:15 AM",   "DD/MM/YYYY HH:MI AM", 0, 0 },
    { "25/12/2026 01:30",      "DD/MM/YYYY HH:MI AM", 0, 0 },
    { "25/12/2026 12:15",      "DD/MM/YYYY HH12:MI", 0, 0 },
    { "25/12/2026 07:15",      "DD/MM/YYYY HH12:MI", 0, 0 },
    { "25/12/2026 12:15 PM",   "DD/MM/YYYY HH12:MI AM", 0, 0 },
    { "15/06/2026 12:00",      "DD/MM/YYYY HH:MI", 0, 0 },
    /* ---- partial masks: Oracle's defaults for missing fields ---- */
    { "10:30",                 "HH24:MI", 0, 0 },
    { "2026-06",               "YYYY-MM", 0, 0 },
    { "06/2026",               "MM/YYYY", 0, 0 },
    { "2026",                  "YYYY", 0, 0 },
    /* ---- literals, no separators ---- */
    { "2026-06-15T10:00:00",   "YYYY-MM-DD\"T\"HH24:MI:SS", 0, 0 },
    { "25122026",              "DDMMYYYY", 0, 0 },
    { "2512026",               "DDMMYYYY", 0, 0 },
    /* ---- bad masks: both paths must reject ---- */
    { "25/12/2026",            "DD/MM/YYYYY", 0, 0 },
    { "25/12/2026 10:30 AM",   "DD/MM/YYYY HH24:MI AM", 0, 0 },
    { "25/12/2026",            "DD/QQ/YYYY", 0, 0 },
    { "25/12/2026",            "DD/MM/YYYY/DD", 0, 0 },
    { "25/12/2026 10:00:00.5", "DD/MM/YYYY HH24:MI:SS.FF", 0, 0 },
    /* ---- elements Stage 1 does not support (counted separately) ---- */
    { "FRI 25/12/2026",        "DY DD/MM/YYYY", 0, 1 },
    { "25/12/2026",            "FXDD/MM/YYYY", 0, 1 },
    /* ---- TIMESTAMP ---- */
    { "2026-06-15 10:00:00",           "", 1, 0 },
    { "2026-06-15",                    "", 1, 0 },
    { "2026-06-15 10:00:00.",          "", 1, 0 },
    { "2026-06-15 10:00:00.5",         "", 1, 0 },
    { "2026-06-15 10:00:00.123456",    "", 1, 0 },
    { "2026-06-15 10:00:00.000000",    "", 1, 0 },
    { "2026-06-15 10:00:00.1234567",   "", 1, 0 },
    { "2026-06-15 10:00:00.123456789", "", 1, 0 },
    { "25/12/2026 10:11:12.123",       "DD/MM/YYYY HH24:MI:SS.FF3", 1, 0 },
    { "25/12/2026 10:11:12.1",         "DD/MM/YYYY HH24:MI:SS.FF3", 1, 0 },
    { "25/12/2026 10:11:12.1234",      "DD/MM/YYYY HH24:MI:SS.FF3", 1, 0 },
    { "25/12/2026 10:11:12.123456789", "DD/MM/YYYY HH24:MI:SS.FF", 1, 0 },
    { "25/12/2026 10:11:12.9999999",   "DD/MM/YYYY HH24:MI:SS.FF", 1, 0 },
    { "25/12/2026 10:11:12.000000001", "DD/MM/YYYY HH24:MI:SS.FF9", 1, 0 },
    { "25/12/2026",                    "DD/MM/YYYY", 1, 0 },
    { "25/12/2026 11:59:59.99 PM",     "DD/MM/YYYY HH:MI:SS.FF2 PM", 1, 0 },
    { "29/02/2025 10:00:00.5",         "DD/MM/YYYY HH24:MI:SS.FF", 1, 0 },
};

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx_base;
    app_config_t  config;
    logger_t err_log, main_log, conn_log, poolconn_log,
             insert_log, meta_log, cache_log;

    if (init_ctx(&ctx_base, &config, &err_log, &main_log, &conn_log,
                 &poolconn_log, &insert_log, &meta_log, &cache_log) != 0)
        return 1;

    const db_driver_t *driver = db_driver_get(&ctx_base);
    if (!driver || !driver->connect || !driver->disconnect ||
        !driver->get_session || !driver->release_session)
    {
        fprintf(stderr, "db_driver_get() returned an incomplete driver\n");
        return 1;
    }

    if (driver->connect(&ctx_base) != 0)
    {
        fprintf(stderr, "connect() failed - see %s\n", config.connection_log_file_name);
        return 1;
    }

    oci_context_t worker;
    if (driver->get_session(&ctx_base, &worker) != 0)
    {
        fprintf(stderr, "get_session() failed\n");
        driver->disconnect(&ctx_base);
        return 1;
    }
    worker.insert_logger         = &insert_log;
    worker.Metadata_logger       = &meta_log;
    worker.cache_logger          = &cache_log;
    worker.connection_logger     = &conn_log;
    worker.connectionpool_logger = &poolconn_log;
    worker.error_logger          = &err_log;
    worker.logger                = &main_log;
    worker.ini                   = &config;

    cache_t *mcache = metadata_cache_init(&config, &cache_log);
    if (!mcache)
    {
        fprintf(stderr, "metadata_cache_init() failed\n");
        driver->release_session(&ctx_base, &worker);
        driver->disconnect(&ctx_base);
        return 1;
    }
    worker.metadata_cache = mcache;

    /* ---- Test 1: row_count=0 ---- */
    printf("Test 1 (row_count=0) ... ");
    {
        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 0;
        req.rows = NULL;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_ROW_COUNT_EXCEEDED)
        {
            printf("FAILED - expected LEVEL2_ERR_ROW_COUNT_EXCEEDED, got %d\n", rc);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_ROW_COUNT_EXCEEDED)\n");
    }

    /* ---- Test 2: two rows, different column sets ---- */
    printf("Test 2 (row column set mismatch) ... ");
    {
        field_value_t row0_fields[1] = { make_field("NUMBER_COL", "1", "") };
        field_value_t row1_fields[2] = { make_field("NUMBER_COL", "2", ""),
                                          make_field("VARCHAR2_COL", "x", "") };
        insert_row_t rows[2] = {
            { .field_count = 1, .fields = row0_fields },
            { .field_count = 2, .fields = row1_fields },
        };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 2;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_FIELD_INVALID)
        {
            printf("FAILED - expected LEVEL2_ERR_FIELD_INVALID, got %d\n", rc);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_FIELD_INVALID)\n");
    }

    /* ---- Test 3: unknown column ---- */
    printf("Test 3 (unknown column) ... ");
    {
        field_value_t fields[2] = { make_field("NUMBER_COL", "3", ""),
                                     make_field("BOGUS_COL", "x", "") };
        insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_FIELD_INVALID || !strstr(status.error_text, "no such column"))
        {
            printf("FAILED - rc=%d error_text='%s' (expected LEVEL2_ERR_FIELD_INVALID, "
                   "'no such column')\n", rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_FIELD_INVALID)\n");
    }

    /* ---- Test 4: NOT NULL column omitted ---- */
    printf("Test 4 (NOT NULL column omitted) ... ");
    {
        field_value_t fields[1] = { make_field("VARCHAR2_COL", "x", "") };
        insert_row_t rows[1] = { { .field_count = 1, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_FIELD_INVALID)
        {
            printf("FAILED - expected LEVEL2_ERR_FIELD_INVALID, got %d (error_text='%s')\n",
                   rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_FIELD_INVALID)\n");
    }

    /* ---- Test 5: date normalize, valid value + explicit format ---- */
    printf("Test 5 (date normalize, valid + format) ... ");
    {
        field_value_t fields[2] = {
            make_field("NUMBER_COL", "5", ""),
            make_field("DATE_COL",   "25/12/2026", "DD/MM/YYYY"),
        };
        insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_OK)
        {
            printf("FAILED - expected LEVEL2_OK, got %d (error_text='%s')\n",
                   rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_OK, normalized value='%s')\n", fields[1].value);
    }

    /* ---- Test 6: date normalize, invalid value ---- */
    printf("Test 6 (date normalize, invalid value) ... ");
    {
        field_value_t fields[2] = {
            make_field("NUMBER_COL", "6", ""),
            make_field("DATE_COL",   "not-a-date", "DD/MM/YYYY"),
        };
        insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_ERR_FIELD_INVALID || !strstr(status.error_text, "Invalid date"))
        {
            printf("FAILED - rc=%d error_text='%s' (expected LEVEL2_ERR_FIELD_INVALID, "
                   "'Invalid date')\n", rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_ERR_FIELD_INVALID)\n");
    }

    /* ---- Test 7: date normalize, no client_date_format ---- */
    printf("Test 7 (date normalize, no client format) ... ");
    {
        /* Already in config.ini's nls_date_format - see source_fmt
         * defaulting to canonical_fmt in normalize_client_date_value()
         * when client_date_format is empty. */
        field_value_t fields[2] = {
            make_field("NUMBER_COL", "7", ""),
            make_field("DATE_COL",   "2026-06-15 00:00:00", ""),
        };
        insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

        insert_request_t req;
        memset(&req, 0, sizeof(req));
        strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
        strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
        req.row_count = 1;
        req.rows = rows;

        input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
        operation_status_t  status; memset(&status, 0, sizeof(status));

        int rc = level2_validate_insert(&worker, &op, &status);
        if (rc != LEVEL2_OK)
        {
            printf("FAILED - expected LEVEL2_OK, got %d (error_text='%s')\n",
                   rc, status.error_text);
            failed = 1;
        }
        else
            printf("OK (LEVEL2_OK)\n");
    }

    /* ---- Test 8: db_type_class() vs the old rule, on real metadata ---- */
    printf("Test 8 (type class vs old rule, real metadata) ... ");
    {
        const char *tables[2] = { TEST_TABLE, "OCI_FIELD_TEST" };
        int checked = 0, date_cols = 0, ts_cols = 0, bad = 0, meta_fail = 0;
        char tz_seen[256] = "";

        for (int t = 0; t < 2; t++)
        {
            col_metadata_t     cols[MAX_TABLE_COLUMNS];
            int                col_count = 0;
            metadata_request_t mreq;
            memset(&mreq, 0, sizeof(mreq));
            strncpy(mreq.table_name, tables[t], sizeof(mreq.table_name) - 1);
            strncpy(mreq.owner, TEST_OWNER, sizeof(mreq.owner) - 1);

            if (get_request_metadata(&worker, &mreq, cols, &col_count,
                                     MAX_TABLE_COLUMNS) != 0)
            {
                meta_fail++;
                continue;
            }

            for (int c = 0; c < col_count; c++)
            {
                const char *ty = cols[c].data_type;
                int old_ts   = (strncmp(ty, "TIMESTAMP", 9) == 0);
                int old_date = (strcmp(ty, "DATE") == 0);
                db_type_class_t cls = db_type_class(ty);
                int new_ts   = (cls == DB_TYPE_CLASS_TIMESTAMP);
                int new_date = (cls == DB_TYPE_CLASS_DATE);

                checked++;
                date_cols += new_date;
                ts_cols   += new_ts;
                if (old_ts != new_ts || old_date != new_date)
                {
                    printf("\n  MISMATCH %s.%s type='%s' old(date=%d ts=%d) "
                           "new=%s", tables[t], cols[c].col_name, ty,
                           old_date, old_ts, db_type_class_name(cls));
                    bad++;
                }
                if (strstr(ty, "TIME ZONE") &&
                    strlen(tz_seen) + strlen(ty) + 3 < sizeof(tz_seen))
                {
                    if (tz_seen[0]) strcat(tz_seen, "; ");
                    strcat(tz_seen, ty);
                }
            }
        }

        if (meta_fail || bad || checked == 0 || !tz_seen[0])
        {
            printf("%sFAILED - checked=%d mismatches=%d metadata_failures=%d "
                   "tz_columns='%s'\n", bad ? "\n" : "", checked, bad,
                   meta_fail, tz_seen);
            failed = 1;
        }
        else
            printf("OK (%d columns, %d DATE, %d TIMESTAMP incl. %s)\n",
                   checked, date_cols, ts_cols, tz_seen);
    }

    /* ---- Test 9: differential - old Oracle path vs new C path ---- */
    {
        int n = (int)(sizeof(DATE_CORPUS) / sizeof(DATE_CORPUS[0]));
        int same_accept = 0, same_reject = 0, unsupported_ok = 0;
        int mismatches = 0, harness_errors = 0;

        printf("Test 9 (differential old Oracle vs new C, %d cases) ... ", n);

        for (int i = 0; i < n; i++)
        {
            const date_case_t *dc = &DATE_CORPUS[i];
            char old_out[128], old_err[512];
            char new_out[DATE_NORMALIZE_OUT_MAX] = "", new_why[256] = "";

            int orc = old_oracle_normalize(&worker, dc->value, dc->mask,
                                           dc->is_timestamp,
                                           old_out, sizeof(old_out),
                                           old_err, sizeof(old_err));
            int nrc = date_normalize(dc->value, dc->mask, dc->is_timestamp,
                                     new_out, sizeof(new_out),
                                     new_why, sizeof(new_why));

            if (orc == -2) { harness_errors++; continue; }

            if (dc->unsupported)
            {
                if (nrc == DATE_NORM_BAD_MASK)
                    unsupported_ok++;
                else
                {
                    printf("\n  MISMATCH [%d] value='%s' mask='%s' ts=%d: "
                           "expected unsupported-mask rejection, new rc=%d",
                           i, dc->value, dc->mask, dc->is_timestamp, nrc);
                    mismatches++;
                }
                continue;
            }

            int old_ok = (orc == 0), new_ok = (nrc == DATE_NORM_OK);

            if (old_ok && new_ok && strcmp(old_out, new_out) == 0)
                same_accept++;
            else if (!old_ok && !new_ok)
                same_reject++;
            else
            {
                printf("\n  MISMATCH [%d] value='%s' mask='%s' ts=%d\n"
                       "      old: %s\n"
                       "      new: %s",
                       i, dc->value, dc->mask[0] ? dc->mask : "(none)",
                       dc->is_timestamp,
                       old_ok ? old_out : old_err,
                       new_ok ? new_out : new_why);
                mismatches++;
            }
        }

        if (mismatches || harness_errors)
        {
            printf("\nTest 9 FAILED - %d mismatch(es), %d harness error(s); "
                   "%d accepted identically, %d rejected by both\n",
                   mismatches, harness_errors, same_accept, same_reject);
            failed = 1;
        }
        else
            printf("OK (%d accepted with identical output, %d rejected by "
                   "both, %d unsupported mask(s) rejected by new path)\n",
                   same_accept, same_reject, unsupported_ok);
    }

    /* ---- Test 10: rejection text - same prefix, no ORA suffix ---- */
    printf("Test 10 (rejection text prefix, no ORA suffix) ... ");
    {
        struct { const char *value; const char *mask; } rej[3] = {
            { "31/02/2026", "DD/MM/YYYY" },
            { "15/13/2026", "DD/MM/YYYY" },
            { "19/08/2026", ""           },
        };
        int bad = 0;

        for (int i = 0; i < 3; i++)
        {
            char old_out[128], old_err[512];
            old_oracle_normalize(&worker, rej[i].value, rej[i].mask, 0,
                                 old_out, sizeof(old_out),
                                 old_err, sizeof(old_err));

            field_value_t fields[2] = {
                make_field("NUMBER_COL", "10", ""),
                make_field("DATE_COL",   rej[i].value, rej[i].mask),
            };
            insert_row_t rows[1] = { { .field_count = 2, .fields = fields } };

            insert_request_t req;
            memset(&req, 0, sizeof(req));
            strncpy(req.table_name, TEST_TABLE, sizeof(req.table_name) - 1);
            strncpy(req.owner,      TEST_OWNER, sizeof(req.owner) - 1);
            req.row_count = 1;
            req.rows = rows;

            input_c_operation_t op = { .type = OP_INSERT, .payload = &req };
            operation_status_t  status; memset(&status, 0, sizeof(status));
            int rc = level2_validate_insert(&worker, &op, &status);

            /* New text is "Row 1, field 'DATE_COL': Invalid date: ...".
             * Compare from "Invalid date" up to " (" on both sides. */
            const char *nt = strstr(status.error_text, "Invalid date");
            const char *op_ = strstr(old_err, "Invalid date");
            const char *nt_end = nt ? strstr(nt, " (") : NULL;
            const char *ot_end = op_ ? strstr(op_, " (") : NULL;

            int ok = (rc == LEVEL2_ERR_FIELD_INVALID) && nt && op_ &&
                     nt_end && ot_end &&
                     (nt_end - nt) == (ot_end - op_) &&
                     strncmp(nt, op_, (size_t)(nt_end - nt)) == 0 &&
                     !strstr(status.error_text, "ORA-");
            if (!ok)
            {
                printf("\n  MISMATCH value='%s' mask='%s'\n      old: %s\n"
                       "      new: %s", rej[i].value, rej[i].mask,
                       old_err, status.error_text);
                bad++;
            }
        }

        if (bad)
        {
            printf("\nTest 10 FAILED - %d of 3 messages differ\n", bad);
            failed = 1;
        }
        else
            printf("OK (3 of 3 prefixes identical, no ORA- text)\n");
    }

    printf("%s\n", failed ? "FAIL" : "PASS");

    metadata_cache_destroy(mcache);
    driver->release_session(&ctx_base, &worker);
    driver->disconnect(&ctx_base);

    logger_close(&err_log);
    logger_close(&main_log);
    logger_close(&conn_log);
    logger_close(&poolconn_log);
    logger_close(&insert_log);
    logger_close(&meta_log);
    logger_close(&cache_log);

    return failed ? 1 : 0;
}
