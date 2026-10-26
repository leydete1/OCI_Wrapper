/*
 * MSSQL_C5_Probe.c   (clean-up Part 2, point C5-P, 26 Oct 2026)
 *
 * A standalone check of what ODBC Driver 18 hands back when a write
 * fires a trigger, before C5 is designed in detail. It uses no gateway
 * code. Every case runs in manual-commit and is ROLLED BACK, so it
 * leaves no rows behind (only MS_TRIGGER_LOG's identity moves on).
 *
 * Tables (Create_Trigger_Test_mssql.sql, run once as sa):
 *   UT  UNIT_TEST_FIELD_TEST   no trigger - the comparison
 *   TT  MS_TRIGGER_TEST        trigger without SET NOCOUNT ON
 *   NC  MS_TRIGGER_NC_TEST     the same trigger with SET NOCOUNT ON
 * For a statement touching N rows each trigger writes 3N log rows, then
 * runs an UPDATE matching 0 rows - so the counts N, 3N and 0 never clash.
 *
 * Cases, for each table:
 *   A  plain INSERT (2 rows), UPDATE (2 rows), UPDATE (0 rows), DELETE
 *      (2 rows): every result in order; the first SQLRowCount is what
 *      dml_execute reads today
 *   B  today's INSERT / UPDATE ... OUTPUT inserted.[K] (expect 334 on
 *      TT and NC)
 *   C  the C5 batch  DECLARE @dm_k ...; ... OUTPUT inserted.[K] INTO
 *      @dm_k ...; SELECT k FROM @dm_k  - INSERT prepared once and run
 *      three times with new binds, UPDATE of 2 rows, UPDATE of 0 rows;
 *      and the same with SET NOCOUNT ON ... OFF around it
 *   D  <statement>; SELECT @@ROWCOUNT - is it the statement's own count?
 *      Also with SET NOCOUNT ON ... OFF around it
 *   E  the LOB write as lob_write_by_rowid does it (UPDATE ... SET col
 *      = ? WHERE key = ?, data at execution): SQLRowCount afterwards
 *   F  a duplicate key inside the C5 batch: where the error appears,
 *      and whether SET NOCOUNT is left ON for the session
 *
 * Connects as DATA_MANAGER (never sa). The password comes from the
 * environment variable C5_PWD, which Run_C5_Probe.sh sets for this one
 * command from a hidden prompt; it is never printed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sql.h>
#include <sqlext.h>

#ifndef SQL_SS_LENGTH_UNLIMITED
#define SQL_SS_LENGTH_UNLIMITED 0
#endif

static SQLHENV g_env = SQL_NULL_HENV;
static SQLHDBC g_dbc = SQL_NULL_HDBC;
static int     g_unexpected = 0;

/* ------------------------------------------------------------------ */
static void diag(SQLSMALLINT type, SQLHANDLE h, char *out, size_t out_size)
{
    SQLCHAR     state[6], msg[1024];
    SQLINTEGER  native;
    SQLSMALLINT len;
    size_t      used = 0;
    out[0] = '\0';
    for (SQLSMALLINT i = 1; ; i++)
    {
        if (!SQL_SUCCEEDED(SQLGetDiagRec(type, h, i, state, &native, msg, sizeof(msg), &len)))
            break;
        const char *m = (const char *)msg;
        const char *p = strstr(m, "[SQL Server]");
        if (p) m = p + 12;
        else if ((p = strstr(m, "for SQL Server]"))) m = p + 15;
        int n = snprintf(out + used, out_size - used, "%s%s/%d: %.160s",
                         used ? " | " : "", state, (int)native, m);
        if (n < 0 || (size_t)n >= out_size - used) break;
        used += (size_t)n;
    }
}

static const char *rcname(SQLRETURN r)
{
    switch (r)
    {
        case SQL_SUCCESS:           return "SUCCESS";
        case SQL_SUCCESS_WITH_INFO: return "SUCCESS_WITH_INFO";
        case SQL_ERROR:             return "ERROR";
        case SQL_INVALID_HANDLE:    return "INVALID_HANDLE";
        case SQL_NO_DATA:           return "NO_DATA";
        case SQL_NEED_DATA:         return "NEED_DATA";
        default:                    return "other";
    }
}

static void report(const char *id, const char *what, const char *seen, const char *verdict)
{
    printf("%-6s %-58s %-34s %s\n", id, what, seen, verdict);
    if (!strcmp(verdict, "UNEXPECTED")) g_unexpected++;
}

/* Runs sql to completion, discarding everything. 0 or -1 (printed). */
static int exec_sql(const char *sql)
{
    SQLHSTMT st;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st))) return -1;
    SQLRETURN r = SQLExecDirect(st, (SQLCHAR *)sql, SQL_NTS);
    if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA)
    {
        char d[2048];
        diag(SQL_HANDLE_STMT, st, d, sizeof(d));
        printf("       setup failed: %s -- %s\n", sql, d);
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return -1;
    }
    do { while (SQL_SUCCEEDED(SQLFetch(st))) ; } while (SQL_SUCCEEDED(SQLMoreResults(st)));
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return 0;
}

static long query_int(const char *sql)
{
    SQLHSTMT st;
    long     v = -999;
    SQLLEN   ind = 0;
    SQLINTEGER iv = 0;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st))) return -999;
    SQLRETURN r = SQLExecDirect(st, (SQLCHAR *)sql, SQL_NTS);
    if (SQL_SUCCEEDED(r) && SQL_SUCCEEDED(SQLFetch(st)) &&
        SQL_SUCCEEDED(SQLGetData(st, 1, SQL_C_SLONG, &iv, 0, &ind)) && ind != SQL_NULL_DATA)
        v = iv;
    else
    {
        char d[2048];
        diag(SQL_HANDLE_STMT, st, d, sizeof(d));
        printf("       query failed: %s -- %s\n", sql, d);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return v;
}

static void rollback(void) { SQLEndTran(SQL_HANDLE_DBC, g_dbc, SQL_ROLLBACK); }

/* Is SET NOCOUNT on for the session? (@@OPTIONS bit 512) */
static int nocount_on(void) { return (query_int("SELECT @@OPTIONS & 512") & 512) != 0; }

/* ------------------------------------------------------------------ */
/* What one execution handed back, result by result.                  */
typedef struct {
    SQLRETURN exec_rc;
    long      first_count;        /* SQLRowCount straight after execute */
    char      seq[1024];          /* e.g. "c6 c0 c2 R[9101]"           */
    char      last_rows[512];     /* values of the last result set      */
    int       last_nrows;
    int       nresults;
    int       had_rowset;
    char      err[1024];          /* first error, where it appeared     */
} outcome_t;

static void app(char *b, size_t n, const char *fmt, const char *s, long v)
{
    size_t u = strlen(b);
    if (u + 2 >= n) return;
    if (s) snprintf(b + u, n - u, fmt, s);
    else   snprintf(b + u, n - u, fmt, v);
}

/* Reads every result after an execute that returned rc. */
static void drain(SQLHSTMT st, SQLRETURN rc, outcome_t *o)
{
    char d[1024];
    o->exec_rc = rc;
    o->first_count = -1;
    if (rc == SQL_ERROR)
    {
        diag(SQL_HANDLE_STMT, st, d, sizeof(d));
        snprintf(o->err, sizeof(o->err), "at execute: %s", d);
        app(o->seq, sizeof(o->seq), "%s ", "ERR", 0);
    }
    if (rc == SQL_NO_DATA)
    {
        app(o->seq, sizeof(o->seq), "%s ", "NO_DATA", 0);
    }
    SQLLEN n0 = -1;
    if (SQL_SUCCEEDED(rc) && SQL_SUCCEEDED(SQLRowCount(st, &n0))) o->first_count = (long)n0;

    int first = 1, after_err = 0;
    for (int guard = 0; guard < 50; guard++)
    {
        if ((!first || SQL_SUCCEEDED(rc)) && !after_err)
        {
            SQLSMALLINT nc = 0;
            if (SQL_SUCCEEDED(SQLNumResultCols(st, &nc)) && nc > 0)
            {
                o->had_rowset = 1;
                o->last_rows[0] = '\0';
                o->last_nrows = 0;
                char vals[512] = "";
                SQLRETURN fr;
                while ((fr = SQLFetch(st)) != SQL_NO_DATA)
                {
                    if (!SQL_SUCCEEDED(fr))
                    {
                        diag(SQL_HANDLE_STMT, st, d, sizeof(d));
                        if (!o->err[0]) snprintf(o->err, sizeof(o->err), "at fetch: %s", d);
                        break;
                    }
                    char v[128] = ""; SQLLEN ind = 0;
                    SQLGetData(st, 1, SQL_C_CHAR, v, sizeof(v), &ind);
                    if (ind == SQL_NULL_DATA) snprintf(v, sizeof(v), "NULL");
                    app(vals, sizeof(vals), o->last_nrows ? ",%s" : "%s", v, 0);
                    o->last_nrows++;
                }
                snprintf(o->last_rows, sizeof(o->last_rows), "%s", vals);
                app(o->seq, sizeof(o->seq), "R[%s] ", vals, 0);
            }
            else
            {
                SQLLEN n = -1;
                SQLRowCount(st, &n);
                app(o->seq, sizeof(o->seq), "c%ld ", NULL, (long)n);
            }
            o->nresults++;
        }
        first = 0;
        after_err = 0;
        SQLRETURN mr = SQLMoreResults(st);
        if (mr == SQL_NO_DATA) break;
        if (mr == SQL_ERROR)
        {
            diag(SQL_HANDLE_STMT, st, d, sizeof(d));
            if (!o->err[0]) snprintf(o->err, sizeof(o->err), "at SQLMoreResults after result %d: %s",
                                     o->nresults, d);
            app(o->seq, sizeof(o->seq), "%s ", "ERR", 0);
            /* the driver may still have more results after an error */
            after_err = 1;
            continue;
        }
        if (mr == SQL_SUCCESS_WITH_INFO)
        {
            diag(SQL_HANDLE_STMT, st, d, sizeof(d));
            printf("       (info at SQLMoreResults: %s)\n", d);
        }
    }
}

static SQLRETURN bind_text(SQLHSTMT st, int pos, const char *v, SQLLEN *ind)
{
    /* as mssql_dml.c bind_text: SQL_C_CHAR into nvarchar */
    size_t len = strlen(v);
    *ind = (SQLLEN)len;
    return SQLBindParameter(st, (SQLUSMALLINT)pos, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                            len ? len : 1, 0, (SQLPOINTER)v, (SQLLEN)len, ind);
}

/* Prepare sql, then execute it once per bind set (nb text values each). */
static void run_prepared(const char *sql, int nexec, int nb, const char *const *binds,
                         outcome_t *out)
{
    SQLHSTMT st;
    SQLLEN   ind[8];
    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
    SQLRETURN r = SQLPrepare(st, (SQLCHAR *)sql, SQL_NTS);
    if (!SQL_SUCCEEDED(r))
    {
        char d[1024];
        diag(SQL_HANDLE_STMT, st, d, sizeof(d));
        for (int e = 0; e < nexec; e++)
        {
            memset(&out[e], 0, sizeof(out[e]));
            out[e].exec_rc = r;
            snprintf(out[e].err, sizeof(out[e].err), "at prepare: %s", d);
            snprintf(out[e].seq, sizeof(out[e].seq), "PREPARE-ERR");
        }
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return;
    }
    for (int e = 0; e < nexec; e++)
    {
        memset(&out[e], 0, sizeof(out[e]));
        for (int k = 0; k < nb; k++)
            bind_text(st, k + 1, binds[(size_t)e * nb + k], &ind[k]);
        r = SQLExecute(st);
        drain(st, r, &out[e]);
        SQLFreeStmt(st, SQL_CLOSE);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
}

static void print_outcome(const char *id, const outcome_t *o)
{
    printf("       %s  execute=%s  first SQLRowCount=%ld  results: %s\n",
           id, rcname(o->exec_rc), o->first_count, o->seq);
    if (o->err[0]) printf("       %s  error %s\n", id, o->err);
}

/* ------------------------------------------------------------------ */
typedef struct {
    const char *tag;      /* UT / TT / NC */
    const char *table;    /* DATA_MANAGER.xxx */
    const char *key;      /* key column */
    const char *txt;      /* a text column */
    const char *lob;      /* varbinary(max) column, or NULL */
    const char *clob;     /* (n)varchar(max) column */
    int         trig;     /* has a trigger */
    int         nocount;  /* trigger has SET NOCOUNT ON */
} tbl_t;

static long log_rows(void)
{
    return query_int("SELECT COUNT(*) FROM DATA_MANAGER.MS_TRIGGER_LOG WHERE ID BETWEEN 9100 AND 9199");
}

static void seed2(const tbl_t *t, int a, int b)
{
    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT INTO %s (%s, %s) VALUES (%d, 'seed'), (%d, 'seed')",
             t->table, t->key, t->txt, a, b);
    exec_sql(sql);
}

/* Trigger counts expected after a statement touching n rows. */
static void verdict_counts(const char *id, const char *what, const tbl_t *t,
                           const outcome_t *o, long n)
{
    char seen[128];
    snprintf(seen, sizeof(seen), "first=%ld [%s]", o->first_count, o->seq);
    /* information only: what the driver would read today */
    report(id, what, seen, o->first_count == n ? "same as statement" : "DIFFERS (finding)");
    (void)t;
}

static void case_A(const tbl_t *t)
{
    char sql[512], id[16], what[128];
    outcome_t o;
    const char *b2[4] = { "9101", "a", "9102", "b" };

    /* A1 INSERT 2 rows */
    snprintf(sql, sizeof(sql), "INSERT INTO %s (%s, %s) VALUES (?, ?), (?, ?)", t->table, t->key, t->txt);
    long l0 = log_rows();
    run_prepared(sql, 1, 4, b2, &o);
    long l1 = log_rows();
    snprintf(id, sizeof(id), "A1-%s", t->tag);
    snprintf(what, sizeof(what), "plain INSERT 2 rows (log +%ld)", l1 - l0);
    print_outcome(id, &o);
    verdict_counts(id, what, t, &o, 2);
    rollback();

    /* A2 UPDATE 2 rows */
    seed2(t, 9103, 9104);
    const char *u2[3] = { "x", "9103", "9104" };
    snprintf(sql, sizeof(sql), "UPDATE %s SET %s = ? WHERE %s IN (?, ?)", t->table, t->txt, t->key);
    l0 = log_rows();
    run_prepared(sql, 1, 3, u2, &o);
    l1 = log_rows();
    snprintf(id, sizeof(id), "A2-%s", t->tag);
    snprintf(what, sizeof(what), "plain UPDATE 2 rows (log +%ld)", l1 - l0);
    print_outcome(id, &o);
    verdict_counts(id, what, t, &o, 2);
    rollback();

    /* A3 UPDATE 0 rows */
    const char *u0[2] = { "x", "9199" };
    snprintf(sql, sizeof(sql), "UPDATE %s SET %s = ? WHERE %s = ?", t->table, t->txt, t->key);
    run_prepared(sql, 1, 2, u0, &o);
    snprintf(id, sizeof(id), "A3-%s", t->tag);
    print_outcome(id, &o);
    {
        char seen[128];
        snprintf(seen, sizeof(seen), "%s first=%ld [%s]", rcname(o.exec_rc), o.first_count, o.seq);
        report(id, "plain UPDATE 0 rows: does execute say NO_DATA?", seen, "info");
    }
    rollback();

    /* A4 DELETE 2 rows */
    seed2(t, 9105, 9106);
    const char *d2[2] = { "9105", "9106" };
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE %s IN (?, ?)", t->table, t->key);
    l0 = log_rows();
    run_prepared(sql, 1, 2, d2, &o);
    l1 = log_rows();
    snprintf(id, sizeof(id), "A4-%s", t->tag);
    snprintf(what, sizeof(what), "plain DELETE 2 rows (log +%ld)", l1 - l0);
    print_outcome(id, &o);
    verdict_counts(id, what, t, &o, 2);
    rollback();
}

static void case_B(const tbl_t *t)
{
    char sql[512], id[16], seen[160];
    outcome_t o;

    const char *b1[2] = { "9111", "a" };
    snprintf(sql, sizeof(sql), "INSERT INTO %s (%s, %s) OUTPUT inserted.[%s] VALUES (?, ?)",
             t->table, t->key, t->txt, t->key);
    run_prepared(sql, 1, 2, b1, &o);
    snprintf(id, sizeof(id), "B1-%s", t->tag);
    print_outcome(id, &o);
    int got334 = strstr(o.err, "/334:") != NULL;
    snprintf(seen, sizeof(seen), "%s", got334 ? "error 334" : o.seq);
    report(id, "INSERT ... OUTPUT inserted.[K] (today)", seen,
           t->trig ? (got334 ? "ok (334 as expected)" : "UNEXPECTED")
                   : (o.last_nrows == 1 ? "ok" : "UNEXPECTED"));
    rollback();

    seed2(t, 9112, 9113);
    const char *u2[3] = { "x", "9112", "9113" };
    snprintf(sql, sizeof(sql), "UPDATE %s SET %s = ? OUTPUT inserted.[%s] WHERE %s IN (?, ?)",
             t->table, t->txt, t->key, t->key);
    run_prepared(sql, 1, 3, u2, &o);
    snprintf(id, sizeof(id), "B2-%s", t->tag);
    print_outcome(id, &o);
    got334 = strstr(o.err, "/334:") != NULL;
    snprintf(seen, sizeof(seen), "%s", got334 ? "error 334" : o.seq);
    report(id, "UPDATE ... OUTPUT inserted.[K] (today)", seen,
           t->trig ? (got334 ? "ok (334 as expected)" : "UNEXPECTED")
                   : (o.last_nrows == 2 ? "ok" : "UNEXPECTED"));
    rollback();
}

/* The C5 batch, plain (nc = 0) or wrapped in SET NOCOUNT ON ... OFF. */
static void case_C(const tbl_t *t, int nc)
{
    char sql[1024], id[16], seen[200], what[128];
    const char *pre  = nc ? "SET NOCOUNT ON; " : "";
    const char *post = nc ? " SET NOCOUNT OFF;" : "";
    const char *tg   = nc ? "CN" : "C";
    outcome_t o[3];

    /* C1: INSERT, prepared once, three executions */
    snprintf(sql, sizeof(sql),
             "%sDECLARE @dm_k TABLE (k nvarchar(4000)); "
             "INSERT INTO %s (%s, %s) OUTPUT inserted.[%s] INTO @dm_k (k) VALUES (?, ?); "
             "SELECT k FROM @dm_k;%s",
             pre, t->table, t->key, t->txt, t->key, post);
    const char *b3[6] = { "9121", "a", "9122", "b", "9123", "c" };
    long l0 = log_rows();
    run_prepared(sql, 3, 2, b3, o);
    long l1 = log_rows();
    for (int e = 0; e < 3; e++)
    {
        snprintf(id, sizeof(id), "%s1%c-%s", tg, 'a' + e, t->tag);
        print_outcome(id, &o[e]);
        snprintf(seen, sizeof(seen), "last set [%s] [%s]", o[e].last_rows, o[e].seq);
        snprintf(what, sizeof(what), "%sbatch INSERT, execution %d of 3", nc ? "NOCOUNT " : "", e + 1);
        report(id, what, seen, (o[e].last_nrows == 1 && !strcmp(o[e].last_rows, b3[e * 2]))
                               ? "ok" : "UNEXPECTED");
    }
    printf("       log +%ld for 3 rows inserted%s\n", l1 - l0, t->trig ? " (expect 9)" : "");
    if (nc)
    {
        int on = nocount_on();
        snprintf(id, sizeof(id), "CN1z-%s", t->tag);
        report(id, "NOCOUNT back OFF after the batch?", on ? "still ON" : "OFF", on ? "UNEXPECTED" : "ok");
        if (on) exec_sql("SET NOCOUNT OFF");
    }
    rollback();

    /* C2: UPDATE 2 rows */
    seed2(t, 9124, 9125);
    snprintf(sql, sizeof(sql),
             "%sDECLARE @dm_k TABLE (k nvarchar(4000)); "
             "UPDATE %s SET %s = ? OUTPUT inserted.[%s] INTO @dm_k (k) WHERE %s IN (?, ?); "
             "SELECT k FROM @dm_k;%s",
             pre, t->table, t->txt, t->key, t->key, post);
    const char *u2[3] = { "x", "9124", "9125" };
    run_prepared(sql, 1, 3, u2, o);
    snprintf(id, sizeof(id), "%s2-%s", tg, t->tag);
    print_outcome(id, &o[0]);
    snprintf(seen, sizeof(seen), "last set [%s] [%s]", o[0].last_rows, o[0].seq);
    report(id, nc ? "NOCOUNT batch UPDATE 2 rows" : "batch UPDATE 2 rows", seen,
           o[0].last_nrows == 2 ? "ok" : "UNEXPECTED");
    rollback();

    /* C3: UPDATE 0 rows */
    const char *u0[3] = { "x", "9198", "9199" };
    run_prepared(sql, 1, 3, u0, o);
    snprintf(id, sizeof(id), "%s3-%s", tg, t->tag);
    print_outcome(id, &o[0]);
    snprintf(seen, sizeof(seen), "%s last set [%s] n=%d", rcname(o[0].exec_rc), o[0].last_rows,
             o[0].last_nrows);
    report(id, nc ? "NOCOUNT batch UPDATE 0 rows" : "batch UPDATE 0 rows", seen,
           (o[0].had_rowset && o[0].last_nrows == 0) ? "ok" : "UNEXPECTED");
    if (nc && nocount_on()) exec_sql("SET NOCOUNT OFF");
    rollback();
}

/* <statement>; SELECT @@ROWCOUNT, plain or inside SET NOCOUNT ON ... OFF */
static void case_D(const tbl_t *t, int nc)
{
    char sql[1024], id[16], seen[200], what[128];
    const char *pre  = nc ? "SET NOCOUNT ON; " : "";
    const char *post = nc ? " SET NOCOUNT OFF;" : "";
    const char *tg   = nc ? "DN" : "D";
    outcome_t o;

    const char *b2[4] = { "9131", "a", "9132", "b" };
    snprintf(sql, sizeof(sql), "%sINSERT INTO %s (%s, %s) VALUES (?, ?), (?, ?); SELECT @@ROWCOUNT;%s",
             pre, t->table, t->key, t->txt, post);
    run_prepared(sql, 1, 4, b2, &o);
    snprintf(id, sizeof(id), "%s1-%s", tg, t->tag);
    print_outcome(id, &o);
    snprintf(seen, sizeof(seen), "@@ROWCOUNT=%s [%s]", o.last_rows, o.seq);
    snprintf(what, sizeof(what), "%sINSERT 2 rows; SELECT @@ROWCOUNT", nc ? "NOCOUNT " : "");
    report(id, what, seen, !strcmp(o.last_rows, "2") ? "ok (statement's own)" : "DIFFERS (finding)");
    if (nc && nocount_on()) { report(id, "NOCOUNT left ON", "still ON", "UNEXPECTED"); exec_sql("SET NOCOUNT OFF"); }
    rollback();

    seed2(t, 9133, 9134);
    const char *u2[3] = { "x", "9133", "9134" };
    snprintf(sql, sizeof(sql), "%sUPDATE %s SET %s = ? WHERE %s IN (?, ?); SELECT @@ROWCOUNT;%s",
             pre, t->table, t->txt, t->key, post);
    run_prepared(sql, 1, 3, u2, &o);
    snprintf(id, sizeof(id), "%s2-%s", tg, t->tag);
    print_outcome(id, &o);
    snprintf(seen, sizeof(seen), "@@ROWCOUNT=%s [%s]", o.last_rows, o.seq);
    snprintf(what, sizeof(what), "%sUPDATE 2 rows; SELECT @@ROWCOUNT", nc ? "NOCOUNT " : "");
    report(id, what, seen, !strcmp(o.last_rows, "2") ? "ok (statement's own)" : "DIFFERS (finding)");
    if (nc && nocount_on()) exec_sql("SET NOCOUNT OFF");
    rollback();

    seed2(t, 9135, 9136);
    const char *d2[2] = { "9135", "9136" };
    snprintf(sql, sizeof(sql), "%sDELETE FROM %s WHERE %s IN (?, ?); SELECT @@ROWCOUNT;%s",
             pre, t->table, t->key, post);
    run_prepared(sql, 1, 2, d2, &o);
    snprintf(id, sizeof(id), "%s3-%s", tg, t->tag);
    print_outcome(id, &o);
    snprintf(seen, sizeof(seen), "@@ROWCOUNT=%s [%s]", o.last_rows, o.seq);
    snprintf(what, sizeof(what), "%sDELETE 2 rows; SELECT @@ROWCOUNT", nc ? "NOCOUNT " : "");
    report(id, what, seen, !strcmp(o.last_rows, "2") ? "ok (statement's own)" : "DIFFERS (finding)");
    if (nc && nocount_on()) exec_sql("SET NOCOUNT OFF");
    rollback();
}

/* The LOB write as lob_write_by_rowid does it: data at execution. */
static void lob_write(const tbl_t *t, const char *col, int is_blob, const char *idtag)
{
    char sql[512], id[16], seen[200], what[128], d[1024];
    seed2(t, 9141, 9142);
    snprintf(sql, sizeof(sql), "UPDATE %s SET %s = ? WHERE %s = ?", t->table, col, t->key);

    size_t total = 100000;
    unsigned char *buf = malloc(total);
    for (size_t i = 0; i < total; i++) buf[i] = is_blob ? (unsigned char)(i * 7) : (unsigned char)('a' + i % 26);

    SQLHSTMT st;
    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
    SQLLEN ind_val = SQL_LEN_DATA_AT_EXEC((SQLLEN)total), ind_key = 0;
    if (is_blob)
        SQLBindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_LONGVARBINARY,
                         (SQLULEN)total, 0, (SQLPOINTER)1, 0, &ind_val);
    else
        SQLBindParameter(st, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                         SQL_SS_LENGTH_UNLIMITED, 0, (SQLPOINTER)1, 0, &ind_val);
    bind_text(st, 2, "9141", &ind_key);

    long l0 = log_rows();
    SQLRETURN r = SQLExecDirect(st, (SQLCHAR *)sql, SQL_NTS);
    SQLRETURN pr = r;
    if (r == SQL_NEED_DATA)
    {
        SQLPOINTER tok;
        size_t sent = 0;
        while ((pr = SQLParamData(st, &tok)) == SQL_NEED_DATA)
            while (sent < total)
            {
                size_t n = total - sent > 65536 ? 65536 : total - sent;
                SQLPutData(st, buf + sent, (SQLLEN)n);
                sent += n;
            }
    }
    outcome_t o;
    memset(&o, 0, sizeof(o));
    drain(st, pr, &o);
    long l1 = log_rows();
    snprintf(id, sizeof(id), "%s-%s", idtag, t->tag);
    printf("       %s  SQLExecDirect=%s  last SQLParamData=%s\n", id, rcname(r), rcname(pr));
    print_outcome(id, &o);
    if (pr == SQL_ERROR) { diag(SQL_HANDLE_STMT, st, d, sizeof(d)); printf("       %s  %s\n", id, d); }
    snprintf(seen, sizeof(seen), "SQLRowCount=%ld [%s] log +%ld", o.first_count, o.seq, l1 - l0);
    snprintf(what, sizeof(what), "LOB write (%s, 1 row, data at exec)", is_blob ? "varbinary" : "text");
    report(id, what, seen, o.first_count == 1 ? "same as statement" : "DIFFERS (finding)");
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    free(buf);
    rollback();
}

/* A duplicate key inside the batch, NOCOUNT-wrapped: where does the error
 * appear, does the rest of the batch run, is NOCOUNT left ON? */
static void case_F(const tbl_t *t)
{
    char sql[1024], id[16], seen[200];
    outcome_t o;
    seed2(t, 9151, 9152);
    snprintf(sql, sizeof(sql),
             "SET NOCOUNT ON; DECLARE @dm_k TABLE (k nvarchar(4000)); "
             "INSERT INTO %s (%s, %s) OUTPUT inserted.[%s] INTO @dm_k (k) VALUES (?, ?); "
             "SELECT k FROM @dm_k; SET NOCOUNT OFF;",
             t->table, t->key, t->txt, t->key);
    const char *b[2] = { "9151", "dup" };
    run_prepared(sql, 1, 2, b, &o);
    snprintf(id, sizeof(id), "F1-%s", t->tag);
    print_outcome(id, &o);
    int dup = strstr(o.err, "/2627:") != NULL;
    snprintf(seen, sizeof(seen), "%s", dup ? "2627" : "no 2627");
    report(id, "NOCOUNT batch, duplicate key: error reported?", seen, dup ? "ok" : "UNEXPECTED");
    int on = nocount_on();
    report(id, "... NOCOUNT afterwards", on ? "still ON" : "OFF", "info");
    if (on) exec_sql("SET NOCOUNT OFF");
    rollback();

    /* same without NOCOUNT */
    snprintf(sql, sizeof(sql),
             "DECLARE @dm_k TABLE (k nvarchar(4000)); "
             "INSERT INTO %s (%s, %s) OUTPUT inserted.[%s] INTO @dm_k (k) VALUES (?, ?); "
             "SELECT k FROM @dm_k;",
             t->table, t->key, t->txt, t->key);
    seed2(t, 9151, 9152);
    run_prepared(sql, 1, 2, b, &o);
    snprintf(id, sizeof(id), "F2-%s", t->tag);
    print_outcome(id, &o);
    dup = strstr(o.err, "/2627:") != NULL;
    report(id, "batch, duplicate key: error reported?", dup ? "2627" : "no 2627", dup ? "ok" : "UNEXPECTED");
    rollback();
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *server = argc > 1 ? argv[1] : "localhost";
    const char *port   = argc > 2 ? argv[2] : "1433";
    const char *db     = argc > 3 ? argv[3] : "DataManager";
    const char *user   = argc > 4 ? argv[4] : "DATA_MANAGER";
    const char *pwd    = getenv("C5_PWD");
    if (!pwd || !*pwd)
    {
        fprintf(stderr, "C5_PWD is not set - run this through Run_C5_Probe.sh\n");
        return 2;
    }

    char pw[512]; size_t j = 0;
    for (const char *p = pwd; *p && j + 2 < sizeof(pw); p++)
    { pw[j++] = *p; if (*p == '}') pw[j++] = '}'; }
    pw[j] = '\0';

    char cs[1024];
    snprintf(cs, sizeof(cs),
             "Driver={ODBC Driver 18 for SQL Server};Server=tcp:%s,%s;Database=%s;"
             "UID=%s;PWD={%s};Encrypt=yes;TrustServerCertificate=yes;APP=DataSol_C5_Probe;",
             server, port, db, user, pw);
    memset(pw, 0, sizeof(pw));

    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &g_env);
    SQLSetEnvAttr(g_env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3_80, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, g_env, &g_dbc);
    SQLRETURN r = SQLDriverConnect(g_dbc, NULL, (SQLCHAR *)cs, SQL_NTS, NULL, 0, NULL,
                                   SQL_DRIVER_NOPROMPT);
    memset(cs, 0, sizeof(cs));
    if (!SQL_SUCCEEDED(r))
    {
        char d[2048];
        diag(SQL_HANDLE_DBC, g_dbc, d, sizeof(d));
        fprintf(stderr, "connect failed: %s\n", d);
        return 2;
    }

    /* Every case in manual-commit, rolled back at its end. */
    SQLSetConnectAttr(g_dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)(uintptr_t)SQL_AUTOCOMMIT_OFF, 0);
    rollback();

    time_t t0 = time(NULL);
    printf("MSSQL_C5_Probe - %s", ctime(&t0));
    printf("server %s,%s  database %s  login %s  (APP=DataSol_C5_Probe)\n", server, port, db, user);
    printf("results: cN = a row count of N, R[..] = a result set and its values, ERR = an error\n\n");

    long have = query_int("SELECT COUNT(*) FROM sys.triggers tr JOIN sys.tables t ON t.object_id = tr.parent_id "
                          "WHERE t.name IN ('MS_TRIGGER_TEST','MS_TRIGGER_NC_TEST') AND tr.is_disabled = 0");
#ifdef PROBE_NO_GUARD   /* local dry run against the stand-in library only */
    have = 2;
#endif
    if (have != 2)
    {
        printf("The trigger test tables are missing (found %ld of 2 enabled triggers).\n"
               "Run Create_Trigger_Test_mssql.sql as sa first - see Run_C5_Probe.sh.\n", have);
        return 2;
    }
    printf("NOCOUNT at start: %s\n\n", nocount_on() ? "ON" : "OFF");
    printf("%-6s %-58s %-34s %s\n", "Test", "What", "Seen", "Result");

    tbl_t tabs[3] = {
        { "UT", "DATA_MANAGER.UNIT_TEST_FIELD_TEST", "NUMBER_COL", "VARCHAR2_COL", NULL,  "CLOB_COL", 0, 0 },
        { "TT", "DATA_MANAGER.MS_TRIGGER_TEST",      "ID",         "NAME",         "DOC", "NOTE",     1, 0 },
        { "NC", "DATA_MANAGER.MS_TRIGGER_NC_TEST",   "ID",         "NAME",         "DOC", "NOTE",     1, 1 },
    };

    for (int i = 0; i < 3; i++)
    {
        const tbl_t *t = &tabs[i];
        printf("\n==== %s  %s  (%s)\n", t->tag, t->table,
               !t->trig ? "no trigger" : t->nocount ? "trigger WITH SET NOCOUNT ON" : "trigger WITHOUT SET NOCOUNT ON");
        case_A(t);
        case_B(t);
        case_C(t, 0);
        case_C(t, 1);
        case_D(t, 0);
        case_D(t, 1);
        if (t->lob) lob_write(t, t->lob, 1, "E1");
        lob_write(t, t->clob, 0, "E2");
        case_F(t);
    }

    long left = query_int("SELECT (SELECT COUNT(*) FROM DATA_MANAGER.MS_TRIGGER_TEST WHERE ID BETWEEN 9100 AND 9199)"
                          " + (SELECT COUNT(*) FROM DATA_MANAGER.MS_TRIGGER_NC_TEST WHERE ID BETWEEN 9100 AND 9199)"
                          " + (SELECT COUNT(*) FROM DATA_MANAGER.UNIT_TEST_FIELD_TEST WHERE NUMBER_COL BETWEEN 9100 AND 9199)"
                          " + (SELECT COUNT(*) FROM DATA_MANAGER.MS_TRIGGER_LOG WHERE ID BETWEEN 9100 AND 9199)");
    printf("\nrows left behind (all rolled back, expect 0): %ld\n", left);
    if (left != 0) g_unexpected++;
    printf("NOCOUNT at end: %s\n", nocount_on() ? "ON" : "OFF");
    printf("\nDONE - %d unexpected result(s). DIFFERS / info lines are findings, not failures.\n",
           g_unexpected);

    rollback();
    SQLDisconnect(g_dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, g_dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, g_env);
    return g_unexpected ? 1 : 0;
}
