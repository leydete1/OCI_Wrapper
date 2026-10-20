/*
 * MSSQL_Driver_Dialect_Test.c   (driver_mssql Stage 3, point T10;
 *                                 Stage 4: U2 row locator, U6 text LOBs)
 *
 * Checks driver_mssql's dialect (mssql_dialect.c) and the SQL Server
 * names in db_type_class.c. No database: every check compares exact
 * text, so this harness needs no ini file (an argument is ignored).
 *
 *   Test 1   bind_placeholder
 *   Test 2   value_expr - one CAST per date/time type, CONVERT for
 *            binary, others unchanged
 *   Test 3   lob_placeholder - 0x for binary LOBs, NULL for text LOBs
 *            (bound in the statement, U6) and everything else
 *   Test 4   now_expr, add_seconds_expr
 *   Test 5   procedure_call_sql
 *   Test 6   count_rows_sql without ORDER BY
 *   Test 7   count_rows_sql with a top-level ORDER BY (5a: OFFSET 0 ROWS)
 *   Test 8   ORDER BY that is not top level - in a string, comment,
 *            [identifier], "identifier", subquery, OVER() - is ignored
 *   Test 9   ORDER BY with OFFSET/FETCH already, or SELECT TOP: wrapped
 *   Test 10  row_limit_sql - TOP wrapper, or OFFSET/FETCH (5a)
 *   Test 11  text after a trailing -- comment starts on a new line
 *   Test 12  every hook returns -1 when the buffer is too small
 *   Test 13  row_locator_clauses: OUTPUT inserted.[KEY] (U2), nothing
 *            without a key, -1 for a composite key
 *   Test 14  db_type_class / db_type_lob_kind / db_type_text_bound for
 *            SQL Server names, the Oracle names unchanged, and lob_kind
 *            agrees with lob_placeholder for every name tried
 *
 * Prints PASS or FAIL as the last line; exit code 0 / 1.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "db_driver.h"
#include "db_type_class.h"
#include "mssql_dialect.h"

static int g_fail;          /* failures in the current test */
static int g_failed_tests;

static void begin(const char *label)
{
    g_fail = 0;
    printf("%-38s... ", label);
}

static void end(const char *ok_note)
{
    if (g_fail) { printf("FAILED\n"); g_failed_tests++; }
    else        printf("OK%s%s%s\n", ok_note ? " (" : "", ok_note ? ok_note : "",
                       ok_note ? ")" : "");
}

static void expect_str(const char *what, int rc, const char *got, const char *want)
{
    if (rc != 0 || strcmp(got, want) != 0)
    {
        if (!g_fail) printf("\n");
        printf("    %s:\n      want rc=0 \"%s\"\n      got  rc=%d \"%s\"\n",
               what, want, rc, got);
        g_fail = 1;
    }
}

static void expect_rc(const char *what, int rc, int want)
{
    if (rc != want)
    {
        if (!g_fail) printf("\n");
        printf("    %s: want rc=%d, got %d\n", what, want, rc);
        g_fail = 1;
    }
}

static void expect_ptr(const char *what, const char *got, const char *want)
{
    int same = (!got && !want) || (got && want && strcmp(got, want) == 0);
    if (!same)
    {
        if (!g_fail) printf("\n");
        printf("    %s: want %s%s%s, got %s%s%s\n", what,
               want ? "\"" : "", want ? want : "NULL", want ? "\"" : "",
               got ? "\"" : "", got ? got : "NULL", got ? "\"" : "");
        g_fail = 1;
    }
}

static void count_is(const db_dialect_t *d, const char *sql, const char *want)
{
    char out[4096];
    int rc = d->count_rows_sql(sql, out, sizeof(out));
    expect_str(sql, rc, out, want);
}

static void limit_is(const db_dialect_t *d, const char *sql, int n, const char *want)
{
    char out[4096];
    int rc = d->row_limit_sql(sql, n, out, sizeof(out));
    expect_str(sql, rc, out, want);
}

static const char *CNT_A = "SELECT COUNT(*) FROM (SELECT 1 AS dm_c FROM (";
static const char *CNT_B = ") AS dm_a) AS dm_b";

/* count_rows_sql text for sql + mid */
static void count_wrap(char *buf, size_t n, const char *sql, const char *mid)
{
    snprintf(buf, n, "%s%s%s%s", CNT_A, sql, mid, CNT_B);
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    const db_dialect_t *d = mssql_dialect_get();
    char out[4096], want[4096];
    int rc;

    /* ---- Test 1 ---- */
    begin("Test 1 (bind_placeholder)");
    rc = d->bind_placeholder(1, out, sizeof(out));  expect_str("pos 1", rc, out, "?");
    rc = d->bind_placeholder(7, out, sizeof(out));  expect_str("pos 7", rc, out, "?");
    expect_rc("pos 0", d->bind_placeholder(0, out, sizeof(out)), -1);
    end("\"?\"");

    /* ---- Test 2 ---- */
    begin("Test 2 (value_expr)");
    {
        static const struct { const char *t, *want; } V[] = {
            { "DATETIME2",      "CAST(? AS datetime2(7))" },
            { "datetime2(6)",   "CAST(? AS datetime2(7))" },
            { "DATETIME",       "CAST(? AS datetime2(7))" },
            { "SMALLDATETIME",  "CAST(? AS datetime2(7))" },
            { "DATE",           "CAST(? AS date)" },
            { "DATETIMEOFFSET", "CAST(? AS datetimeoffset(7))" },
            { "TIME",           "CAST(? AS time(7))" },
            { "NVARCHAR",       "?" },
            { "BIGINT",         "?" },
            { "VARCHAR(MAX)",   "?" },
            { "VARBINARY",      "CONVERT(varbinary(max), ?, 2)" },
            { "BINARY",         "CONVERT(varbinary(max), ?, 2)" },
            { "varbinary(50)",  "CONVERT(varbinary(max), ?, 2)" },
            { "",               "?" },
        };
        for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++)
        {
            rc = d->value_expr(V[i].t, "?", out, sizeof(out));
            expect_str(V[i].t, rc, out, V[i].want);
        }
        rc = d->value_expr("DATE", "'2030-01-02 00:00:00'", out, sizeof(out));
        expect_str("DATE literal", rc, out, "CAST('2030-01-02 00:00:00' AS date)");
        rc = d->value_expr(NULL, "?", out, sizeof(out));
        expect_str("NULL dtype", rc, out, "?");
        expect_rc("NULL operand", d->value_expr("DATE", NULL, out, sizeof(out)), -1);
    }
    end(NULL);

    /* ---- Test 3 ---- */
    begin("Test 3 (lob_placeholder)");
    expect_ptr("VARBINARY(MAX)",   d->lob_placeholder("VARBINARY(MAX)"), "0x");
    expect_ptr("varbinary( max )", d->lob_placeholder("varbinary( max )"), "0x");
    expect_ptr("IMAGE",            d->lob_placeholder("IMAGE"), "0x");
    expect_ptr("VARCHAR(MAX)",     d->lob_placeholder("VARCHAR(MAX)"), NULL);
    expect_ptr("TEXT",             d->lob_placeholder("TEXT"), NULL);
    expect_ptr("NVARCHAR(MAX)",    d->lob_placeholder("NVARCHAR(MAX)"), NULL);
    expect_ptr("NTEXT",            d->lob_placeholder("NTEXT"), NULL);
    expect_ptr("NVARCHAR",         d->lob_placeholder("NVARCHAR"), NULL);
    expect_ptr("VARBINARY",        d->lob_placeholder("VARBINARY"), NULL);
    expect_ptr("XML",              d->lob_placeholder("XML"), NULL);
    expect_ptr("NULL",             d->lob_placeholder(NULL), NULL);
    end("0x for binary; text LOBs bound");

    /* ---- Test 4 ---- */
    begin("Test 4 (now_expr, add_seconds_expr)");
    expect_ptr("now_expr", d->now_expr(), "SYSDATETIME()");
    rc = d->add_seconds_expr("CREATED_TS", "TTL_SECONDS", out, sizeof(out));
    expect_str("add_seconds", rc, out, "DATEADD(second, CAST(TTL_SECONDS AS int), CREATED_TS)");
    expect_rc("NULL base", d->add_seconds_expr(NULL, "1", out, sizeof(out)), -1);
    end(NULL);

    /* ---- Test 5 ---- */
    begin("Test 5 (procedure_call_sql)");
    {
        const char *names[] = { "P_IN_NUM", "P_OUT_NUM", "P_RESULTS" };
        rc = d->procedure_call_sql("DATA_MANAGER.UNIT_TEST_SAMPLE_PROC", names, 3,
                                   out, sizeof(out));
        expect_str("3 params", rc, out, "{CALL DATA_MANAGER.UNIT_TEST_SAMPLE_PROC(?, ?, ?)}");
        rc = d->procedure_call_sql("UNIT_TEST_NOOP_PROC", NULL, 0, out, sizeof(out));
        expect_str("0 params", rc, out, "{CALL UNIT_TEST_NOOP_PROC}");
        const char *bad[] = { "P1", NULL };
        expect_rc("NULL name", d->procedure_call_sql("P", bad, 2, out, sizeof(out)), -1);
        expect_rc("NULL proc", d->procedure_call_sql(NULL, names, 1, out, sizeof(out)), -1);
    }
    end(NULL);

    /* ---- Test 6 ---- */
    begin("Test 6 (count_rows_sql, no ORDER BY)");
    {
        const char *q = "SELECT SESSION_ID, TTL_SECONDS FROM OCI_SESSION WHERE STATUS = 'ACTIVE'";
        count_wrap(want, sizeof(want), q, "");
        count_is(d, q, want);
    }
    end(NULL);

    /* ---- Test 7 ---- */
    begin("Test 7 (count_rows_sql, ORDER BY - 5a)");
    {
        const char *q[] = {
            "SELECT NUMBER_COL FROM OCI_FIELD_TEST ORDER BY NUMBER_COL",
            "select number_col from oci_field_test order by number_col desc",
            "SELECT A FROM T ORDER /* c */ BY A",
            "SELECT A FROM T\nORDER\n  BY A",
            "SELECT A FROM T UNION SELECT B FROM U ORDER BY 1",
            "SELECT N'x' AS A FROM T ORDER BY A",
        };
        for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++)
        {
            count_wrap(want, sizeof(want), q[i], " OFFSET 0 ROWS");
            count_is(d, q[i], want);
        }
    }
    end("\" OFFSET 0 ROWS\" added");

    /* ---- Test 8 ---- */
    begin("Test 8 (ORDER BY not at top level)");
    {
        const char *q[] = {
            "SELECT 'x ORDER BY y' AS A FROM T",
            "SELECT N'it''s ORDER BY' AS A FROM T",
            "SELECT A FROM T -- ORDER BY A\nWHERE 1 = 1",
            "SELECT A FROM T /* ORDER BY A /* nested */ ORDER BY B */",
            "SELECT [ORDER BY] FROM T",
            "SELECT \"ORDER BY\" FROM T",
            "SELECT A FROM (SELECT TOP 5 A FROM T ORDER BY A) AS X",
            "SELECT A, ROW_NUMBER() OVER (ORDER BY A) AS RN FROM T",
            "SELECT [A]]ORDER BY] FROM T",
            "SELECT ORDER_ID, BY_USER FROM T",
        };
        for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++)
        {
            count_wrap(want, sizeof(want), q[i], "");
            count_is(d, q[i], want);
            int ob = -1;
            mssql_sql_shape(q[i], &ob, NULL, NULL, NULL);
            expect_rc(q[i], ob, 0);
        }
    }
    end("10 cases");

    /* ---- Test 9 ---- */
    begin("Test 9 (OFFSET/FETCH or TOP: wrapped)");
    {
        const char *q[] = {
            "SELECT A FROM T ORDER BY A OFFSET 10 ROWS FETCH NEXT 5 ROWS ONLY",
            "SELECT TOP (5) A FROM T ORDER BY A",
            "SELECT TOP 5 A FROM T ORDER BY A",
            "SELECT DISTINCT TOP (5) A FROM T ORDER BY A",
            "select all top 5 A from T order by A",
        };
        for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++)
        {
            count_wrap(want, sizeof(want), q[i], "");
            count_is(d, q[i], want);
            snprintf(want, sizeof(want), "SELECT TOP (3) * FROM (%s) AS dm_t", q[i]);
            limit_is(d, q[i], 3, want);
        }
        /* OFFSET as a column name before any ORDER BY is not OFFSET/FETCH */
        const char *q2 = "SELECT OFFSET FROM T ORDER BY OFFSET";
        count_wrap(want, sizeof(want), q2, " OFFSET 0 ROWS");
        count_is(d, q2, want);
        const char *q4 = "SELECT A, OFFSET FROM T ORDER BY A, OFFSET";
        count_wrap(want, sizeof(want), q4, " OFFSET 0 ROWS");
        count_is(d, q4, want);
        const char *q5 = "SELECT A FROM T ORDER BY A OFFSET @skip ROWS";
        count_wrap(want, sizeof(want), q5, "");
        count_is(d, q5, want);
        /* "TOP" later in the statement is not SELECT TOP */
        const char *q3 = "SELECT A, TOP FROM T ORDER BY A";
        count_wrap(want, sizeof(want), q3, " OFFSET 0 ROWS");
        count_is(d, q3, want);
    }
    end(NULL);

    /* ---- Test 10 ---- */
    begin("Test 10 (row_limit_sql)");
    limit_is(d, "SELECT NUMBER_COL FROM OCI_FIELD_TEST", 2,
             "SELECT TOP (2) * FROM (SELECT NUMBER_COL FROM OCI_FIELD_TEST) AS dm_t");
    limit_is(d, "SELECT NUMBER_COL FROM OCI_FIELD_TEST ORDER BY NUMBER_COL DESC", 2,
             "SELECT NUMBER_COL FROM OCI_FIELD_TEST ORDER BY NUMBER_COL DESC "
             "OFFSET 0 ROWS FETCH NEXT 2 ROWS ONLY");
    limit_is(d, "SELECT A FROM T", 0, "SELECT TOP (0) * FROM (SELECT A FROM T) AS dm_t");
    expect_rc("max_rows -1", d->row_limit_sql("SELECT A FROM T", -1, out, sizeof(out)), -1);
    expect_rc("NULL sql", d->row_limit_sql(NULL, 1, out, sizeof(out)), -1);
    end("TOP wrapper / OFFSET-FETCH");

    /* ---- Test 11 ---- */
    begin("Test 11 (trailing -- comment)");
    {
        const char *q = "SELECT A FROM T ORDER BY A -- newest first";
        snprintf(want, sizeof(want), "%s%s\nOFFSET 0 ROWS%s", CNT_A, q, CNT_B);
        count_is(d, q, want);
        snprintf(want, sizeof(want), "%s\nOFFSET 0 ROWS FETCH NEXT 4 ROWS ONLY", q);
        limit_is(d, q, 4, want);
        const char *q2 = "SELECT A FROM T -- all of them";
        snprintf(want, sizeof(want), "SELECT TOP (4) * FROM (%s\n) AS dm_t", q2);
        limit_is(d, q2, 4, want);
        snprintf(want, sizeof(want), "%s%s\n%s", CNT_A, q2, CNT_B);
        count_is(d, q2, want);
        /* a -- comment followed by a new line is closed */
        const char *q3 = "SELECT A FROM T -- c\n";
        snprintf(want, sizeof(want), "SELECT TOP (4) * FROM (%s) AS dm_t", q3);
        limit_is(d, q3, 4, want);
    }
    end(NULL);

    /* ---- Test 12 ---- */
    begin("Test 12 (buffer too small)");
    {
        char tiny[8];
        expect_rc("bind_placeholder", d->bind_placeholder(1, tiny, 1), -1);
        expect_rc("value_expr", d->value_expr("DATE", "?", tiny, sizeof(tiny)), -1);
        expect_rc("count_rows_sql", d->count_rows_sql("SELECT 1 AS A", tiny, sizeof(tiny)), -1);
        expect_rc("row_limit_sql", d->row_limit_sql("SELECT 1 AS A", 1, tiny, sizeof(tiny)), -1);
        expect_rc("add_seconds_expr", d->add_seconds_expr("A", "B", tiny, sizeof(tiny)), -1);
        const char *names[] = { "P1", "P2" };
        expect_rc("procedure_call_sql", d->procedure_call_sql("PROC", names, 2, tiny, sizeof(tiny)), -1);
        expect_rc("NULL out", d->count_rows_sql("SELECT 1 AS A", NULL, 0), -1);
    }
    end(NULL);

    /* ---- Test 13 ---- */
    begin("Test 13 (row_locator_clauses)");
    {
        char mid[64] = "x", tail[64] = "x";
        const char *k1[] = { "FIELD_TEST_ID" };
        db_row_locator_ctx_t loc = { "OCI_FIELD_TEST", "DATA_MANAGER", k1, 1 };
        rc = d->row_locator_clauses(DB_DML_INSERT, &loc, 3, mid, sizeof(mid), tail, sizeof(tail));
        expect_str("INSERT mid", rc, mid, "OUTPUT inserted.[FIELD_TEST_ID]");
        expect_str("INSERT tail", rc, tail, "");
        rc = d->row_locator_clauses(DB_DML_UPDATE, &loc, 9, mid, sizeof(mid), tail, sizeof(tail));
        expect_str("UPDATE mid", rc, mid, "OUTPUT inserted.[FIELD_TEST_ID]");
        const char *k2[] = { "ODD]NAME" };
        db_row_locator_ctx_t loc2 = { "T", "", k2, 1 };
        rc = d->row_locator_clauses(DB_DML_INSERT, &loc2, 1, mid, sizeof(mid), tail, sizeof(tail));
        expect_str("] doubled", rc, mid, "OUTPUT inserted.[ODD]]NAME]");
        db_row_locator_ctx_t nokey = { "T", "", NULL, 0 };
        strcpy(mid, "x"); strcpy(tail, "x");
        rc = d->row_locator_clauses(DB_DML_INSERT, &nokey, 1, mid, sizeof(mid), tail, sizeof(tail));
        expect_str("no key: mid", rc, mid, "");
        expect_str("no key: tail", rc, tail, "");
        const char *k3[] = { "A", "B" };
        db_row_locator_ctx_t comp = { "T", "", k3, 2 };
        expect_rc("composite", d->row_locator_clauses(DB_DML_INSERT, &comp, 1, mid, sizeof(mid),
                                                      tail, sizeof(tail)), -1);
        char tiny[8];
        expect_rc("small buffer", d->row_locator_clauses(DB_DML_INSERT, &loc, 1, tiny, sizeof(tiny),
                                                         tail, sizeof(tail)), -1);
    }
    end("OUTPUT inserted.[KEY]");

    /* ---- Test 14 ---- */
    begin("Test 14 (db_type_class, lob_kind)");
    {
        static const struct { const char *t; db_type_class_t c; db_lob_kind_t k; } T[] = {
            /* SQL Server */
            { "CHAR",             DB_TYPE_CLASS_STRING,    DB_LOB_NONE   },
            { "VARCHAR",          DB_TYPE_CLASS_STRING,    DB_LOB_NONE   },
            { "NCHAR",            DB_TYPE_CLASS_STRING,    DB_LOB_NONE   },
            { "NVARCHAR",         DB_TYPE_CLASS_STRING,    DB_LOB_NONE   },
            { "UNIQUEIDENTIFIER", DB_TYPE_CLASS_STRING,    DB_LOB_NONE   },
            { "TIME",             DB_TYPE_CLASS_STRING,    DB_LOB_NONE   },
            { "INT",              DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "BIGINT",           DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "SMALLINT",         DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "TINYINT",          DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "BIT",              DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "DECIMAL",          DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "NUMERIC",          DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "MONEY",            DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "SMALLMONEY",       DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "FLOAT",            DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "REAL",             DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "DATE",             DB_TYPE_CLASS_DATE,      DB_LOB_NONE   },
            { "SMALLDATETIME",    DB_TYPE_CLASS_DATE,      DB_LOB_NONE   },
            { "DATETIME",         DB_TYPE_CLASS_TIMESTAMP, DB_LOB_NONE   },
            { "DATETIME2",        DB_TYPE_CLASS_TIMESTAMP, DB_LOB_NONE   },
            { "datetime2(6)",     DB_TYPE_CLASS_TIMESTAMP, DB_LOB_NONE   },
            { "DATETIMEOFFSET",   DB_TYPE_CLASS_TIMESTAMP, DB_LOB_NONE   },
            { "BINARY",           DB_TYPE_CLASS_BINARY,    DB_LOB_NONE   },
            { "VARBINARY",        DB_TYPE_CLASS_BINARY,    DB_LOB_NONE   },
            { "VARBINARY(50)",    DB_TYPE_CLASS_BINARY,    DB_LOB_NONE   },
            { "VARBINARY(MAX)",   DB_TYPE_CLASS_LOB,       DB_LOB_BINARY },
            { "varbinary(max)",   DB_TYPE_CLASS_LOB,       DB_LOB_BINARY },
            { "IMAGE",            DB_TYPE_CLASS_LOB,       DB_LOB_BINARY },
            { "VARCHAR(MAX)",     DB_TYPE_CLASS_LOB,       DB_LOB_NONE   },
            { "NVARCHAR(MAX)",    DB_TYPE_CLASS_LOB,       DB_LOB_NONE   },
            { "NVARCHAR( MAX )",  DB_TYPE_CLASS_LOB,       DB_LOB_NONE   },
            { "TEXT",             DB_TYPE_CLASS_LOB,       DB_LOB_NONE   },
            { "NTEXT",            DB_TYPE_CLASS_LOB,       DB_LOB_NONE   },
            { "XML",              DB_TYPE_CLASS_LOB,       DB_LOB_NONE   },
            { "ROWVERSION",       DB_TYPE_CLASS_OTHER,     DB_LOB_NONE   },
            { "SQL_VARIANT",      DB_TYPE_CLASS_OTHER,     DB_LOB_NONE   },
            /* Oracle - unchanged */
            { "VARCHAR2",         DB_TYPE_CLASS_STRING,    DB_LOB_NONE   },
            { "NUMBER(10,2)",     DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "BINARY_FLOAT",     DB_TYPE_CLASS_NUMERIC,   DB_LOB_NONE   },
            { "TIMESTAMP(6) WITH TIME ZONE", DB_TYPE_CLASS_TIMESTAMP, DB_LOB_NONE },
            { "INTERVAL DAY(2) TO SECOND(6)", DB_TYPE_CLASS_INTERVAL_DS, DB_LOB_NONE },
            { "INTERVAL YEAR(2) TO MONTH",    DB_TYPE_CLASS_INTERVAL_YM, DB_LOB_NONE },
            { "RAW",              DB_TYPE_CLASS_BINARY,    DB_LOB_NONE   },
            { "ROWID",            DB_TYPE_CLASS_ROWID,     DB_LOB_NONE   },
            { "BLOB",             DB_TYPE_CLASS_LOB,       DB_LOB_BINARY },
            { "CLOB",             DB_TYPE_CLASS_LOB,       DB_LOB_TEXT   },
            { "NCLOB",            DB_TYPE_CLASS_LOB,       DB_LOB_TEXT   },
            { "BFILE",            DB_TYPE_CLASS_LOB,       DB_LOB_NONE   },
        };
        int n = (int)(sizeof(T) / sizeof(T[0]));
        for (int i = 0; i < n; i++)
        {
            db_type_class_t c = db_type_class(T[i].t);
            db_lob_kind_t   k = db_type_lob_kind(T[i].t);
            if (c != T[i].c || k != T[i].k)
            {
                if (!g_fail) printf("\n");
                printf("    %s: want %s/%d, got %s/%d\n", T[i].t,
                       db_type_class_name(T[i].c), (int)T[i].k,
                       db_type_class_name(c), (int)k);
                g_fail = 1;
            }
            /* SQL Server names (the first 37): a column is written after
             * the statement exactly when the SQL gives it a placeholder */
            if (i < 37)
            {
                int has_ph = d->lob_placeholder(T[i].t) != NULL;
                if (has_ph != (k != DB_LOB_NONE))
                {
                    if (!g_fail) printf("\n");
                    printf("    %s: lob_placeholder %s but lob_kind %d\n", T[i].t,
                           has_ph ? "set" : "NULL", (int)k);
                    g_fail = 1;
                }
            }
        }
        /* U6: text LOBs bound in the statement - SQL Server only */
        static const struct { const char *t; int b; } TB[] = {
            { "VARCHAR(MAX)", 1 }, { "nvarchar(max)", 1 }, { "TEXT", 1 }, { "NTEXT", 1 },
            { "XML", 1 }, { "VARBINARY(MAX)", 0 }, { "IMAGE", 0 }, { "VARCHAR", 0 },
            { "NVARCHAR", 0 }, { "CLOB", 0 }, { "NCLOB", 0 }, { "BLOB", 0 },
            { "VARCHAR2", 0 }, { "", 0 },
        };
        for (size_t i = 0; i < sizeof(TB) / sizeof(TB[0]); i++)
            if (db_type_text_bound(TB[i].t) != TB[i].b)
            {
                if (!g_fail) printf("\n");
                printf("    %s: db_type_text_bound want %d\n", TB[i].t, TB[i].b);
                g_fail = 1;
            }
        if (db_type_text_bound(NULL) != 0) { printf("\n    NULL: want 0\n"); g_fail = 1; }
        char note[64];
        snprintf(note, sizeof(note), "%d names", n);
        end(note);
    }

    printf("%s\n", g_failed_tests ? "FAIL" : "PASS");
    return g_failed_tests ? 1 : 0;
}
