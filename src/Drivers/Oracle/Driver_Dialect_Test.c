/*
 * Driver_Dialect_Test.c
 *
 * Oracle dialect extraction, Stage 2 (2026-10-02). Checks the Oracle
 * driver's db_dialect_t - the SQL fragments core now asks the driver
 * for instead of writing them itself (see db_driver.h, DIALECT).
 *
 * The main Stage 2 proof is that every SQL statement core logs is
 * unchanged; Driver_Insert/Update/Delete/Procedure/Select_Test and the
 * full HTTP consumer run cover that. This harness adds two things they
 * cannot: the exact text of every hook, including the ones only a
 * rare path reaches (intervals, the row-limit wrapper, the session
 * expiry expression), and proof that what the hooks produce is SQL
 * Oracle actually accepts and evaluates correctly.
 *
 * Tests:
 *   Test 1 - the driver has a dialect and every hook is set.
 *   Test 2 - exact text of every hook for every type the four removed
 *            wrapper functions handled (byte-identical to pre-Stage-2).
 *   Test 3 - every hook that writes into a buffer returns -1, without
 *            overrunning it, when the buffer is too small.
 *   Test 4 - live: value_expr() for DATE and TIMESTAMP, applied to an
 *            ISO literal and read back with TO_CHAR, returns the same
 *            value - the fixed ISO mask really parses ISO.
 *   Test 5 - live: count_rows_sql() over a table with a BLOB column
 *            (the reason for its SELECT 1) equals a direct COUNT(*),
 *            and row_limit_sql(..., 3) returns exactly 3 rows.
 *   Test 6 - live: add_seconds_expr(now_expr(), 60) is later than
 *            now_expr() - the session-expiry arithmetic evaluates.
 *   Test 7 - live (item 2b, 2026-10-03): db_select_int(), the helper
 *            execute_query_batch() now runs its count query through.
 *            The count over the BLOB-column SELECT equals Test 5's
 *            direct COUNT(*), an empty count gives 0, and five bad
 *            queries (missing table, no row, two rows, two columns,
 *            text) are each rejected with -1. The rejected ones log
 *            their reason (and, for the missing table, ORA-00942) in
 *            select_Data_Manager.log - expected, not a fault.
 *   Test 8 - live (Stage 5, 2026-10-08): select_open() with bind
 *            values. A bound filter gives the same count as the same
 *            filter written as a literal; a NULL bind matches nothing;
 *            a value containing a quote and "--" comes back exactly as
 *            sent (it was bound, not pasted into the SQL); two binds
 *            in one statement; and a bind value Oracle cannot convert
 *            (ORA-01722 at execute) now fails select_open() itself
 *            instead of the first fetch. The ORA-01722 line in
 *            select_Data_Manager.log is expected, not a fault.
 *   Test 9 - live (Stage 5): text_lobs_inline. A 3,000-character
 *            CLOB comes back in the field exactly; a NULL CLOB comes
 *            back as ""; a 4,200-character CLOB does not fit the
 *            4,096-byte field and fails the fetch (logged in
 *            select_Data_Manager.log - expected) instead of being cut
 *            short. No CLOB file is written by this test.
 *
 * No DB writes anywhere in this file.
 *
 * Build: added to Build.sh like every other harness (same FILES list).
 * Run:   ./Driver_Dialect_Test (same LD_LIBRARY_PATH/LSAN_OPTIONS as
 *        Run_Manually.sh).
 *
 * Expected output on success:
 *   Test 1 (dialect present, 9 hooks)        ... OK
 *   Test 2 (exact hook text, n cases)        ... OK
 *   Test 3 (small buffers rejected)          ... OK (n cases)
 *   Test 4 (ISO date/timestamp round trip)   ... OK (2 of 2)
 *   Test 5 (count_rows_sql / row_limit_sql)  ... OK (count=6 direct=6, limit 3 rows)
 *   Test 6 (now_expr + add_seconds_expr)     ... OK
 *   Test 7 (db_select_int, 7 cases)          ... OK (count=6 direct=6)
 *   Test 8 (select_open with binds, 5 cases) ... OK (bound=6 literal=6)
 *   Test 9 (CLOB returned inline, 3 cases)   ... OK (3000 chars exact)
 *   PASS
 */

#define _POSIX_C_SOURCE 200809L

#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Connection.h"
#include "Connection_Pool.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"
#include "Resultset_Builder.h"

static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *connection_logger,
                     logger_t *connectionpool_logger, logger_t *select_logger,
                     logger_t *metadata_logger)
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

    if (logger_init_str2(select_logger, config->select_log_file_name,
                          config->select_log_file_max_size,
                          config->select_log_file_rotation_number,
                          config->select_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init select_logger\n"); return -1; }
    ctx->select_logger = select_logger;

    if (logger_init_str2(metadata_logger, config->Metadata_log_file_name,
                          config->Metadata_log_file_max_size,
                          config->Metadata_log_file_rotation_number,
                          config->Metadata_log_level, ctx->error_logger) != 0)
    { fprintf(stderr, "Failed to init Metadata_logger\n"); return -1; }
    ctx->Metadata_logger = metadata_logger;

    return 0;
}

/* Runs sql through select_open/select_fetch_batch and returns how many
 * rows came back; the first row's first field is copied to first_value
 * (if non-NULL). -1 on any driver failure; *open_failed (if non-NULL)
 * says whether it was select_open() that failed. Stage 5: optional
 * bind values and text_lobs_inline. */
static int run_query_ex(const db_driver_t *driver, oci_context_t *ctx,
                        const char *sql, const char **binds, int bind_count,
                        int text_lobs_inline, int *open_failed,
                        char *first_value, size_t first_max)
{
    db_select_request_t req;
    memset(&req, 0, sizeof(req));
    req.sql              = sql;
    req.fetch_array_size = 5;
    req.bind_count       = bind_count;
    req.bind_values      = binds;
    req.text_lobs_inline = text_lobs_inline;

    if (open_failed) *open_failed = 0;

    db_select_cursor_t *cursor  = NULL;
    db_column_meta_t   *columns = NULL;
    int col_count = 0, batch_size = 0;

    if (first_value && first_max) first_value[0] = '\0';

    if (driver->select_open(ctx, &req, &cursor, &columns,
                            &col_count, &batch_size) != 0)
    {
        if (open_failed) *open_failed = 1;
        return -1;
    }

    int total = 0, rc = 0;
    for (;;)
    {
        resultset_t *rs = NULL;
        int rows = 0;
        if (driver->select_fetch_batch(cursor, &rs, &rows, NULL) != 0)
        {
            rc = -1;
            break;
        }
        if (rows == 0) break;

        if (total == 0 && first_value && first_max &&
            rs->records[0].field_count > 0)
            snprintf(first_value, first_max, "%s", rs->records[0].fields[0].value);

        total += rows;
        resultset_free(rs);
    }

    driver->select_close(cursor);
    free(columns);
    return rc == 0 ? total : -1;
}

static int run_query(const db_driver_t *driver, oci_context_t *ctx,
                     const char *sql, char *first_value, size_t first_max)
{
    return run_query_ex(driver, ctx, sql, NULL, 0, 0, NULL,
                        first_value, first_max);
}

typedef struct {
    const char *what;
    const char *dtype;
    const char *operand;
    const char *expected;
} value_case_t;

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    oci_context_t ctx; app_config_t config;
    logger_t err_l, conn_l, connpool_l, select_l, meta_l;
    if (init_ctx(&ctx, &config, &err_l, &conn_l, &connpool_l,
                 &select_l, &meta_l) != 0)
    {
        printf("INIT FAILED\n");
        return 1;
    }

    const db_driver_t *driver = db_driver_get(&ctx);
    if (!driver)
    {
        printf("FAILED - db_driver_get() returned NULL\n");
        return 1;
    }
    const db_dialect_t *dl = driver->dialect;

    /* ---- Test 1: dialect present, every hook set ---- */
    printf("Test 1 (dialect present, 9 hooks)        ... ");
    if (!dl || !dl->bind_placeholder || !dl->value_expr ||
        !dl->lob_placeholder || !dl->row_locator_clauses ||
        !dl->row_limit_sql || !dl->count_rows_sql || !dl->now_expr ||
        !dl->add_seconds_expr || !dl->procedure_call_sql)
    {
        printf("FAILED - dialect or a hook is NULL\n");
        printf("FAIL\n");
        return 1;
    }
    printf("OK\n");

    /* ---- Test 2: exact text ---- */
    {
        int cases = 0, bad = 0;
        char out[512], mid[128], tail[128];

        /* value_expr - every type the four removed wrappers handled,
         * plus the plain types that must pass through untouched.      */
        static const value_case_t VC[] = {
            { "DATE",      "DATE",                        ":3",
              "TO_DATE(:3,'YYYY-MM-DD HH24:MI:SS')" },
            { "TIMESTAMP", "TIMESTAMP(6)",                ":4",
              "TO_TIMESTAMP(:4,'YYYY-MM-DD HH24:MI:SS.FF6')" },
            { "TS TZ",     "TIMESTAMP(6) WITH TIME ZONE", ":5",
              "TO_TIMESTAMP(:5,'YYYY-MM-DD HH24:MI:SS.FF6')" },
            { "TS LTZ",    "TIMESTAMP(6) WITH LOCAL TIME ZONE", ":6",
              "TO_TIMESTAMP(:6,'YYYY-MM-DD HH24:MI:SS.FF6')" },
            { "INT YM",    "INTERVAL YEAR(2) TO MONTH",   ":7",
              "TO_YMINTERVAL(:7)" },
            { "INT DS",    "INTERVAL DAY(2) TO SECOND(6)", ":8",
              "TO_DSINTERVAL(:8)" },
            { "VARCHAR2",  "VARCHAR2",                    ":9",   ":9" },
            { "NUMBER",    "NUMBER",                      ":10",  ":10" },
            { "RAW",       "RAW",                         ":11",  ":11" },
            { "literal",   "DATE",                        "'2026-08-19 14:30:00'",
              "TO_DATE('2026-08-19 14:30:00','YYYY-MM-DD HH24:MI:SS')" },
            { "NULL type", NULL,                          ":12",  ":12" },
        };
        for (size_t i = 0; i < sizeof(VC) / sizeof(VC[0]); i++)
        {
            cases++;
            if (dl->value_expr(VC[i].dtype, VC[i].operand, out, sizeof(out)) != 0 ||
                strcmp(out, VC[i].expected) != 0)
            {
                printf("\n  MISMATCH value_expr %s: got '%s' expected '%s'",
                       VC[i].what, out, VC[i].expected);
                bad++;
            }
        }

        /* bind_placeholder */
        cases++;
        if (dl->bind_placeholder(17, out, sizeof(out)) != 0 || strcmp(out, ":17") != 0)
        { printf("\n  MISMATCH bind_placeholder: '%s'", out); bad++; }

        /* lob_placeholder */
        struct { const char *t; const char *e; } LC[] = {
            { "BLOB", "EMPTY_BLOB()" }, { "CLOB", "EMPTY_CLOB()" },
            { "NCLOB", "EMPTY_CLOB()" }, { "BFILE", NULL },
            { "VARCHAR2", NULL }, { "DATE", NULL },
        };
        for (size_t i = 0; i < sizeof(LC) / sizeof(LC[0]); i++)
        {
            cases++;
            const char *got = dl->lob_placeholder(LC[i].t);
            if ((got == NULL) != (LC[i].e == NULL) ||
                (got && strcmp(got, LC[i].e) != 0))
            {
                printf("\n  MISMATCH lob_placeholder %s: got '%s'",
                       LC[i].t, got ? got : "(NULL)");
                bad++;
            }
        }

        /* row_locator_clauses - INSERT and UPDATE */
        db_row_locator_ctx_t loc = { "OCI_FIELD_TEST", "DATA_MANAGER", NULL, 0 };
        db_dml_kind_t kinds[2] = { DB_DML_INSERT, DB_DML_UPDATE };
        for (int k = 0; k < 2; k++)
        {
            cases++;
            if (dl->row_locator_clauses(kinds[k], &loc, 6, mid, sizeof(mid),
                                        tail, sizeof(tail)) != 0 ||
                mid[0] != '\0' || strcmp(tail, "RETURNING ROWID INTO :6") != 0)
            {
                printf("\n  MISMATCH row_locator_clauses kind=%d: mid='%s' tail='%s'",
                       (int)kinds[k], mid, tail);
                bad++;
            }
        }

        /* row_limit_sql / count_rows_sql */
        cases++;
        if (dl->row_limit_sql("SELECT * FROM T", 5, out, sizeof(out)) != 0 ||
            strcmp(out, "SELECT * FROM (SELECT * FROM T) WHERE ROWNUM <= 5") != 0)
        { printf("\n  MISMATCH row_limit_sql: '%s'", out); bad++; }

        cases++;
        if (dl->count_rows_sql("SELECT * FROM T", out, sizeof(out)) != 0 ||
            strcmp(out, "SELECT COUNT(*) FROM (SELECT 1 FROM (SELECT * FROM T))") != 0)
        { printf("\n  MISMATCH count_rows_sql: '%s'", out); bad++; }

        /* now_expr / add_seconds_expr - together they must rebuild the
         * session-expiry condition exactly as Session_Manager.c had it */
        cases++;
        if (strcmp(dl->now_expr(), "SYSTIMESTAMP") != 0)
        { printf("\n  MISMATCH now_expr: '%s'", dl->now_expr()); bad++; }

        cases++;
        if (dl->add_seconds_expr("CREATED_TS", "TTL_SECONDS", out, sizeof(out)) != 0 ||
            strcmp(out, "CREATED_TS + NUMTODSINTERVAL(TTL_SECONDS, 'SECOND')") != 0)
        { printf("\n  MISMATCH add_seconds_expr: '%s'", out); bad++; }

        /* procedure_call_sql - none, one and several parameters */
        const char *p3[3] = { "P_IN", "P_OUT", "P_CUR" };
        cases++;
        if (dl->procedure_call_sql("UNIT_TEST_NOPARAM", NULL, 0, out, sizeof(out)) != 0 ||
            strcmp(out, "BEGIN UNIT_TEST_NOPARAM; END;") != 0)
        { printf("\n  MISMATCH procedure_call_sql (0): '%s'", out); bad++; }

        cases++;
        if (dl->procedure_call_sql("DATA_MANAGER.P", p3, 1, out, sizeof(out)) != 0 ||
            strcmp(out, "BEGIN DATA_MANAGER.P(:P_IN); END;") != 0)
        { printf("\n  MISMATCH procedure_call_sql (1): '%s'", out); bad++; }

        cases++;
        if (dl->procedure_call_sql("P", p3, 3, out, sizeof(out)) != 0 ||
            strcmp(out, "BEGIN P(:P_IN, :P_OUT, :P_CUR); END;") != 0)
        { printf("\n  MISMATCH procedure_call_sql (3): '%s'", out); bad++; }

        printf("Test 2 (exact hook text, %d cases)        ... ", cases);
        if (bad) { printf("FAILED - %d mismatch(es)\n", bad); failed = 1; }
        else       printf("OK\n");
    }

    /* ---- Test 3: buffers too small ---- */
    {
        int cases = 0, bad = 0;
        char tiny[6];
        char big[64];
        const char *p1[1] = { "P_IN" };
        db_row_locator_ctx_t loc = { "T", "", NULL, 0 };

        /* sentinel after tiny[] to catch any overrun */
        struct { char buf[6]; char guard[8]; } g;
        memset(&g, 0, sizeof(g));
        memcpy(g.guard, "GUARD!!", 8);

#define EXPECT_FAIL(expr) do { cases++; if ((expr) != -1) { \
            printf("\n  NOT REJECTED: %s", #expr); bad++; } } while (0)

        EXPECT_FAIL(dl->bind_placeholder(123456, g.buf, sizeof(g.buf)));
        EXPECT_FAIL(dl->value_expr("DATE", ":1", g.buf, sizeof(g.buf)));
        EXPECT_FAIL(dl->row_locator_clauses(DB_DML_INSERT, &loc, 1,
                                            big, sizeof(big), g.buf, sizeof(g.buf)));
        EXPECT_FAIL(dl->row_limit_sql("SELECT 1 FROM DUAL", 3, g.buf, sizeof(g.buf)));
        EXPECT_FAIL(dl->count_rows_sql("SELECT 1 FROM DUAL", g.buf, sizeof(g.buf)));
        EXPECT_FAIL(dl->add_seconds_expr("A", "B", g.buf, sizeof(g.buf)));
        EXPECT_FAIL(dl->procedure_call_sql("PROC", p1, 1, g.buf, sizeof(g.buf)));
        EXPECT_FAIL(dl->procedure_call_sql("PROCEDURE", NULL, 0, g.buf, sizeof(g.buf)));
        EXPECT_FAIL(dl->bind_placeholder(0, tiny, sizeof(tiny)));   /* bad position */
#undef EXPECT_FAIL

        cases++;
        if (memcmp(g.guard, "GUARD!!", 8) != 0)
        { printf("\n  BUFFER OVERRUN - guard bytes changed"); bad++; }

        printf("Test 3 (small buffers rejected)          ... ");
        if (bad) { printf("FAILED - %d problem(s)\n", bad); failed = 1; }
        else       printf("OK (%d cases)\n", cases);
    }

    /* ---- Live tests need a session ---- */
    if (!driver->connect || driver->connect(&ctx) != 0)
    {
        printf("FAILED - connect(): see %s\n", config.connectionpool_log_file_name);
        printf("FAIL\n");
        return 1;
    }

    oci_context_t worker;
    memset(&worker, 0, sizeof(worker));
    if (driver->get_session(&ctx, &worker) != 0)
    {
        printf("FAILED - get_session()\n");
        driver->disconnect(&ctx);
        printf("FAIL\n");
        return 1;
    }
    worker.error_logger    = ctx.error_logger;
    worker.select_logger   = ctx.select_logger;
    worker.Metadata_logger = ctx.Metadata_logger;
    worker.ini             = &config;

    /* ---- Test 4: ISO round trip through value_expr ---- */
    {
        struct { const char *dtype; const char *iso; const char *back_fmt; } RT[2] = {
            { "DATE",         "2026-08-19 14:30:05",        "YYYY-MM-DD HH24:MI:SS" },
            { "TIMESTAMP(6)", "2026-08-19 14:30:05.123456", "YYYY-MM-DD HH24:MI:SS.FF6" },
        };
        int ok = 0;
        printf("Test 4 (ISO date/timestamp round trip)   ... ");
        for (int i = 0; i < 2; i++)
        {
            char lit[64], expr[256], sql[512], got[128];
            snprintf(lit, sizeof(lit), "'%s'", RT[i].iso);
            if (dl->value_expr(RT[i].dtype, lit, expr, sizeof(expr)) != 0)
            { printf("\n  value_expr failed for %s", RT[i].dtype); continue; }
            snprintf(sql, sizeof(sql), "SELECT TO_CHAR(%s,'%s') FROM DUAL",
                     expr, RT[i].back_fmt);
            int rows = run_query(driver, &worker, sql, got, sizeof(got));
            if (rows == 1 && strcmp(got, RT[i].iso) == 0)
                ok++;
            else
                printf("\n  %s: rows=%d got='%s' expected='%s' sql=%s",
                       RT[i].dtype, rows, got, RT[i].iso, sql);
        }
        if (ok == 2) printf("OK (2 of 2)\n");
        else { printf("\nTest 4 FAILED - %d of 2\n", ok); failed = 1; }
    }

    /* ---- Test 5: count_rows_sql and row_limit_sql, live ---- */
    {
        /* count over a SELECT that includes the BLOB column (the case
         * SELECT 1 exists for); the row limit over scalar columns only,
         * so fetching does not write BLOB files to BLOB_output_dir. */
        const char *base   = "SELECT ID, DESCRIPTION, PHOTO FROM OCI_LOB_TEST";
        const char *scalar = "SELECT ID, DESCRIPTION FROM OCI_LOB_TEST";
        const char *direct = "SELECT COUNT(*) FROM OCI_LOB_TEST";
        char sql[512], count_val[32] = "", direct_val[32] = "";

        printf("Test 5 (count_rows_sql / row_limit_sql)  ... ");

        int r1 = -1, r2 = -1, limited = -1;
        if (dl->count_rows_sql(base, sql, sizeof(sql)) == 0)
            r1 = run_query(driver, &worker, sql, count_val, sizeof(count_val));
        r2 = run_query(driver, &worker, direct, direct_val, sizeof(direct_val));
        if (dl->row_limit_sql(scalar, 3, sql, sizeof(sql)) == 0)
            limited = run_query(driver, &worker, sql, NULL, 0);

        int direct_n = atoi(direct_val);
        if (r1 == 1 && r2 == 1 && strcmp(count_val, direct_val) == 0 &&
            limited == (direct_n < 3 ? direct_n : 3))
            printf("OK (count=%s direct=%s, limit %d rows)\n",
                   count_val, direct_val, limited);
        else
        {
            printf("FAILED - count rows=%d val='%s', direct rows=%d val='%s', "
                   "limited rows=%d\n", r1, count_val, r2, direct_val, limited);
            failed = 1;
        }
    }

    /* ---- Test 6: now_expr + add_seconds_expr, live ---- */
    {
        char later[160], sql[512], got[16] = "";
        printf("Test 6 (now_expr + add_seconds_expr)     ... ");
        if (dl->add_seconds_expr(dl->now_expr(), "60", later, sizeof(later)) == 0)
        {
            snprintf(sql, sizeof(sql),
                     "SELECT CASE WHEN %s > %s THEN 'Y' ELSE 'N' END FROM DUAL",
                     later, dl->now_expr());
            int rows = run_query(driver, &worker, sql, got, sizeof(got));
            if (rows == 1 && strcmp(got, "Y") == 0)
                printf("OK\n");
            else
            {
                printf("FAILED - rows=%d got='%s' sql=%s\n", rows, got, sql);
                failed = 1;
            }
        }
        else
        {
            printf("FAILED - add_seconds_expr\n");
            failed = 1;
        }
    }

    /* ---- Test 7: db_select_int (item 2b), live ---- */
    {
        char count_sql[512], direct_val[32] = "";
        long long counted = -1, empty = -1;

        printf("Test 7 (db_select_int, 7 cases)          ... ");

        int ok = 0;
        if (dl->count_rows_sql("SELECT ID, DESCRIPTION, PHOTO FROM OCI_LOB_TEST",
                               count_sql, sizeof(count_sql)) == 0 &&
            db_select_int(&worker, count_sql, 0, &counted) == 0 &&
            run_query(driver, &worker, "SELECT COUNT(*) FROM OCI_LOB_TEST",
                      direct_val, sizeof(direct_val)) == 1 &&
            counted == atoll(direct_val))
            ok++;
        else
            printf("\n  count: counted=%lld direct='%s'", counted, direct_val);

        if (dl->count_rows_sql("SELECT ID FROM OCI_LOB_TEST WHERE 1 = 0",
                               count_sql, sizeof(count_sql)) == 0 &&
            db_select_int(&worker, count_sql, 0, &empty) == 0 && empty == 0)
            ok++;
        else
            printf("\n  empty count: got %lld", empty);

        static const struct { const char *what; const char *sql; } BAD[] = {
            { "missing table", "SELECT COUNT(*) FROM DIALECT_TEST_NO_SUCH_TABLE" },
            { "no row",        "SELECT 1 FROM DUAL WHERE 1 = 0" },
            { "two rows",      "SELECT 1 FROM DUAL UNION ALL SELECT 2 FROM DUAL" },
            { "two columns",   "SELECT 1, 2 FROM DUAL" },
            { "text",          "SELECT 'abc' FROM DUAL" },
        };
        for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++)
        {
            long long v = -777;
            if (db_select_int(&worker, BAD[i].sql, 0, &v) == -1 && v == -777)
                ok++;
            else
                printf("\n  %s: not rejected (v=%lld)", BAD[i].what, v);
        }

        if (ok == 7)
            printf("OK (count=%lld direct=%s)\n", counted, direct_val);
        else
        {
            printf("\nTest 7 FAILED - %d of 7\n", ok);
            failed = 1;
        }
    }

    /* ---- Test 8: select_open with binds (Stage 5), live ---- */
    {
        char ph1[16], ph2[16], sql[256];
        char bound[32] = "", literal[32] = "", got[256] = "";
        int  ok = 0, open_failed = 0;

        printf("Test 8 (select_open with binds, 5 cases) ... ");

        dl->bind_placeholder(1, ph1, sizeof(ph1));
        dl->bind_placeholder(2, ph2, sizeof(ph2));

        /* 1. bound filter == literal filter */
        const char *zero[1] = { "0" };
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM OCI_LOB_TEST WHERE ID >= %s", ph1);
        if (run_query_ex(driver, &worker, sql, zero, 1, 0, NULL, bound, sizeof(bound)) == 1 &&
            run_query(driver, &worker, "SELECT COUNT(*) FROM OCI_LOB_TEST WHERE ID >= 0",
                      literal, sizeof(literal)) == 1 &&
            strcmp(bound, literal) == 0 && atoi(bound) > 0)
            ok++;
        else
            printf("\n  bound='%s' literal='%s'", bound, literal);

        /* 2. NULL bind matches nothing */
        const char *null_bind[1] = { NULL };
        char nulls[32] = "";
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM OCI_LOB_TEST WHERE ID = %s", ph1);
        if (run_query_ex(driver, &worker, sql, null_bind, 1, 0, NULL, nulls, sizeof(nulls)) == 1 &&
            strcmp(nulls, "0") == 0)
            ok++;
        else
            printf("\n  NULL bind count='%s'", nulls);

        /* 3. quote and comment characters come back verbatim */
        const char *tricky[1] = { "O'Brien' OR '1'='1 --" };
        snprintf(sql, sizeof(sql), "SELECT %s FROM DUAL", ph1);
        if (run_query_ex(driver, &worker, sql, tricky, 1, 0, NULL, got, sizeof(got)) == 1 &&
            strcmp(got, tricky[0]) == 0)
            ok++;
        else
            printf("\n  tricky value came back as '%s'", got);

        /* 4. two binds */
        const char *two[2] = { "alpha", "beta" };
        snprintf(sql, sizeof(sql), "SELECT %s || '-' || %s FROM DUAL", ph1, ph2);
        if (run_query_ex(driver, &worker, sql, two, 2, 0, NULL, got, sizeof(got)) == 1 &&
            strcmp(got, "alpha-beta") == 0)
            ok++;
        else
            printf("\n  two binds gave '%s'", got);

        /* 5. execute failure (ORA-01722) now fails select_open() */
        const char *bad[1] = { "abc" };
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM DUAL WHERE 1 = TO_NUMBER(%s)", ph1);
        if (run_query_ex(driver, &worker, sql, bad, 1, 0, &open_failed, NULL, 0) == -1 &&
            open_failed)
            ok++;
        else
            printf("\n  ORA-01722 case: open_failed=%d", open_failed);

        if (ok == 5)
            printf("OK (bound=%s literal=%s)\n", bound, literal);
        else
        {
            printf("\nTest 8 FAILED - %d of 5\n", ok);
            failed = 1;
        }
    }

    /* ---- Test 9: text_lobs_inline (Stage 5), live ---- */
    {
        static char text[3001], got[4096];
        char ph1[16], sql[256];
        int  ok = 0;

        printf("Test 9 (CLOB returned inline, 3 cases)   ... ");

        memset(text, 0, sizeof(text));
        for (int i = 0; i < 3000; i++) text[i] = (char)('a' + i % 26);
        dl->bind_placeholder(1, ph1, sizeof(ph1));

        /* 1. 3,000 characters back exactly */
        const char *b[1] = { text };
        snprintf(sql, sizeof(sql), "SELECT TO_CLOB(%s) FROM DUAL", ph1);
        if (run_query_ex(driver, &worker, sql, b, 1, 1, NULL, got, sizeof(got)) == 1 &&
            strcmp(got, text) == 0)
            ok++;
        else
            printf("\n  3000-char CLOB came back as %zu chars", strlen(got));

        /* 2. NULL CLOB -> "" */
        strcpy(got, "not-empty");
        if (run_query_ex(driver, &worker, "SELECT TO_CLOB(NULL) FROM DUAL",
                         NULL, 0, 1, NULL, got, sizeof(got)) == 1 && got[0] == '\0')
            ok++;
        else
            printf("\n  NULL CLOB came back as '%s'", got);

        /* 3. 4,200 characters do not fit: the fetch fails */
        if (run_query_ex(driver, &worker,
                         "SELECT TO_CLOB(RPAD('x', 4000, 'x')) || "
                         "TO_CLOB(RPAD('y', 200, 'y')) FROM DUAL",
                         NULL, 0, 1, NULL, got, sizeof(got)) == -1)
            ok++;
        else
            printf("\n  4200-char CLOB was not rejected");

        if (ok == 3)
            printf("OK (3000 chars exact)\n");
        else
        {
            printf("\nTest 9 FAILED - %d of 3\n", ok);
            failed = 1;
        }
    }

    driver->release_session(&ctx, &worker);
    driver->disconnect(&ctx);

    printf("%s\n", failed ? "FAIL" : "PASS");

    logger_close(&err_l);
    logger_close(&conn_l);
    logger_close(&connpool_l);
    logger_close(&select_l);
    logger_close(&meta_l);

    return failed ? 1 : 0;
}
