/*
 * MSSQL_Z7_Probe.c   (Z7 - plain reads in autocommit, 25 Oct 2026)
 *
 * A standalone check of how SQL Server and ODBC Driver 18 behave around
 * autocommit, before the Z7 proposal is written. It uses no gateway code
 * and changes no data: the only thing it writes to is a #temp table,
 * which belongs to its own session and disappears when it disconnects.
 *
 * It answers:
 *   P1-P6   which statements open a transaction in manual-commit mode
 *           (@@TRANCOUNT after each), and that autocommit opens none
 *   P7      does switching autocommit ON commit an open transaction?
 *   P8      manual-commit from a write until SQLEndTran, then back to
 *           autocommit - does it behave as expected?
 *   P9      what SQLEndTran returns on an autocommit connection
 *   P10-P11 can autocommit be switched while a result set is still
 *           being read (the cursor case)?
 *   P12     what the switches cost
 *
 * Connects as DATA_MANAGER (never sa). The password is read from the
 * environment variable Z7_PWD, which Run_Z7_Probe.sh sets for this one
 * command from a hidden prompt; it is never printed.
 *
 * Build and run: see Run_Z7_Probe.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sql.h>
#include <sqlext.h>

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
        int n = snprintf(out + used, out_size - used, "%sSQLSTATE %s [native %d]: %s",
                         used ? " | " : "", state, (int)native, m);
        if (n < 0 || (size_t)n >= out_size - used) break;
        used += (size_t)n;
    }
}

static const char *rcname(SQLRETURN r)
{
    switch (r)
    {
        case SQL_SUCCESS:           return "SQL_SUCCESS";
        case SQL_SUCCESS_WITH_INFO: return "SQL_SUCCESS_WITH_INFO";
        case SQL_ERROR:             return "SQL_ERROR";
        case SQL_INVALID_HANDLE:    return "SQL_INVALID_HANDLE";
        case SQL_NO_DATA:           return "SQL_NO_DATA";
        case SQL_STILL_EXECUTING:   return "SQL_STILL_EXECUTING";
        default:                    return "other";
    }
}

static void report(const char *id, const char *what, const char *seen, int ok)
{
    printf("%-5s %-62s %-28s %s\n", id, what, seen, ok ? "ok" : "UNEXPECTED");
    if (!ok) g_unexpected++;
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Runs sql to completion, discarding any rows. 0 or -1 (printed). */
static int exec_sql(const char *sql)
{
    SQLHSTMT st;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st))) return -1;
    SQLRETURN r = SQLExecDirect(st, (SQLCHAR *)sql, SQL_NTS);
    if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA)
    {
        char d[2048];
        diag(SQL_HANDLE_STMT, st, d, sizeof(d));
        printf("      exec failed: %s -- %s\n", sql, d);
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        return -1;
    }
    do { while (SQL_SUCCEEDED(SQLFetch(st))) ; } while (SQLMoreResults(st) == SQL_SUCCESS);
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return 0;
}

/* One integer from a single-row, single-column query; -999 on failure. */
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
        printf("      query failed: %s -- %s\n", sql, d);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, st);
    return v;
}

static long trancount(void) { return query_int("SELECT @@TRANCOUNT"); }

static SQLRETURN set_autocommit(int on, char *d, size_t dsz)
{
    SQLRETURN r = SQLSetConnectAttr(g_dbc, SQL_ATTR_AUTOCOMMIT,
                                    (SQLPOINTER)(uintptr_t)(on ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF), 0);
    if (d) { d[0] = '\0'; if (!SQL_SUCCEEDED(r)) diag(SQL_HANDLE_DBC, g_dbc, d, dsz); }
    return r;
}

static SQLRETURN end_tran(int commit, char *d, size_t dsz)
{
    SQLRETURN r = SQLEndTran(SQL_HANDLE_DBC, g_dbc, commit ? SQL_COMMIT : SQL_ROLLBACK);
    if (d) { d[0] = '\0'; if (!SQL_SUCCEEDED(r)) diag(SQL_HANDLE_DBC, g_dbc, d, dsz); }
    return r;
}

/* Puts the connection in a known state: manual-commit, nothing open. */
static void reset_manual(void)
{
    set_autocommit(0, NULL, 0);
    end_tran(0, NULL, 0);
}

static void tc_check(const char *id, const char *what, long expect)
{
    char seen[64];
    long t = trancount();
    snprintf(seen, sizeof(seen), "@@TRANCOUNT=%ld", t);
    report(id, what, seen, t == expect);
}

/* ------------------------------------------------------------------ */
/* P10/P11 - switch autocommit while a result set is still being read. */
static void pending_switch(const char *id, int start_on)
{
    char d[2048], seen[64];
    SQLHSTMT st, st2;

    if (start_on) set_autocommit(1, NULL, 0); else reset_manual();

    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st);
    SQLRETURN r = SQLExecDirect(st, (SQLCHAR *)
        "SELECT c.object_id, c.column_id, c.name FROM sys.all_columns c "
        "CROSS JOIN (SELECT TOP (5) 1 AS k FROM sys.all_objects) x", SQL_NTS);
    if (!SQL_SUCCEEDED(r) || !SQL_SUCCEEDED(SQLFetch(st)))
    {
        diag(SQL_HANDLE_STMT, st, d, sizeof(d));
        printf("      %s: could not open the result set: %s\n", id, d);
        SQLFreeHandle(SQL_HANDLE_STMT, st);
        g_unexpected++;
        return;
    }

    /* While rows are still unread: is the connection busy for others? */
    SQLAllocHandle(SQL_HANDLE_STMT, g_dbc, &st2);
    r = SQLExecDirect(st2, (SQLCHAR *)"SELECT 1", SQL_NTS);
    diag(SQL_HANDLE_STMT, st2, d, sizeof(d));
    printf("      %s: second statement while rows are unread: %s %s\n", id, rcname(r), d);
    SQLFreeHandle(SQL_HANDLE_STMT, st2);

    /* The switch itself, rows still unread. */
    r = set_autocommit(!start_on, d, sizeof(d));
    snprintf(seen, sizeof(seen), "%s", rcname(r));
    printf("      %s: switch autocommit %s with rows unread: %s %s\n",
           id, start_on ? "OFF" : "ON", rcname(r), d);
    report(id, start_on ? "switch OFF while a result set is open (information)"
                        : "switch ON while a result set is open (information)", seen, 1);

    /* The cursor still readable afterwards? */
    long more = 0;
    while (SQL_SUCCEEDED(SQLFetch(st))) more++;
    printf("      %s: rows read after the switch attempt: %ld\n", id, more);

    SQLFreeHandle(SQL_HANDLE_STMT, st);

    /* After the result set is closed. */
    r = set_autocommit(!start_on, d, sizeof(d));
    snprintf(seen, sizeof(seen), "%s", rcname(r));
    report(id, "same switch after the statement is freed", seen, SQL_SUCCEEDED(r));
    if (!SQL_SUCCEEDED(r)) printf("      %s\n", d);
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *server = argc > 1 ? argv[1] : "localhost";
    const char *port   = argc > 2 ? argv[2] : "1433";
    const char *db     = argc > 3 ? argv[3] : "DataManager";
    const char *user   = argc > 4 ? argv[4] : "DATA_MANAGER";
    const char *pwd    = getenv("Z7_PWD");
    if (!pwd || !*pwd)
    {
        fprintf(stderr, "Z7_PWD is not set - run this through Run_Z7_Probe.sh\n");
        return 2;
    }

    /* PWD={...}: braces so any character is allowed; '}' doubled. */
    char pw[512]; size_t j = 0;
    for (const char *p = pwd; *p && j + 2 < sizeof(pw); p++)
    { pw[j++] = *p; if (*p == '}') pw[j++] = '}'; }
    pw[j] = '\0';

    char cs[1024];
    snprintf(cs, sizeof(cs),
             "Driver={ODBC Driver 18 for SQL Server};Server=tcp:%s,%s;Database=%s;"
             "UID=%s;PWD={%s};Encrypt=yes;TrustServerCertificate=yes;APP=DataSol_Z7_Probe;",
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

    time_t t0 = time(NULL);
    printf("MSSQL_Z7_Probe - %s", ctime(&t0));
    printf("server %s,%s  database %s  login %s  (APP=DataSol_Z7_Probe)\n\n", server, port, db, user);
    printf("%-5s %-62s %-28s %s\n", "Test", "What", "Seen", "Result");

    char d[2048], seen[128];

    /* ---- P1-P6: which statements open a transaction --------------- */
    reset_manual();
    tc_check("P1", "manual-commit, nothing run yet", 0);

    exec_sql("SELECT COUNT(*) FROM OCI_FIELD_TEST");
    tc_check("P2", "manual-commit, after SELECT from a table (Z7)", 1);
    reset_manual();

    exec_sql("SELECT 1");
    tc_check("P3", "manual-commit, after SELECT 1 (ping / health check)", 0);
    reset_manual();

    exec_sql("SELECT SYSDATETIME()");
    tc_check("P3b", "manual-commit, after SELECT SYSDATETIME() (no table)", 0);
    reset_manual();

    exec_sql("SELECT COUNT(*) FROM sys.indexes WHERE object_id = OBJECT_ID('OCI_FIELD_TEST')");
    {
        long t = trancount();
        snprintf(seen, sizeof(seen), "@@TRANCOUNT=%ld", t);
        report("P4", "manual-commit, after a catalog-view query (table_key-like)", seen, 1);
    }
    reset_manual();

    exec_sql("SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_NAME = 'OCI_FIELD_TEST'");
    {
        long t = trancount();
        snprintf(seen, sizeof(seen), "@@TRANCOUNT=%ld", t);
        report("P4b", "manual-commit, after INFORMATION_SCHEMA query (describe_table)", seen, 1);
    }
    reset_manual();

    set_autocommit(1, NULL, 0);
    exec_sql("SELECT COUNT(*) FROM OCI_FIELD_TEST");
    tc_check("P5", "autocommit, after SELECT from a table", 0);

    set_autocommit(0, NULL, 0);
    tc_check("P6", "switching autocommit OFF opens nothing by itself", 0);
    reset_manual();

    /* ---- P7: switching ON commits what is open --------------------- */
    set_autocommit(1, NULL, 0);
    exec_sql("IF OBJECT_ID('tempdb..#z7') IS NOT NULL DROP TABLE #z7");
    exec_sql("CREATE TABLE #z7 (n int)");
    set_autocommit(0, NULL, 0);
    exec_sql("INSERT INTO #z7 VALUES (1)");
    tc_check("P7a", "manual-commit, after INSERT into #z7", 1);
    r = set_autocommit(1, d, sizeof(d));
    snprintf(seen, sizeof(seen), "%s", rcname(r));
    report("P7b", "switch autocommit ON with the INSERT open", seen, SQL_SUCCEEDED(r));
    tc_check("P7c", "after the switch", 0);
    end_tran(0, NULL, 0);                         /* a rollback now - too late? */
    {
        long n = query_int("SELECT COUNT(*) FROM #z7");
        snprintf(seen, sizeof(seen), "rows=%ld", n);
        report("P7d", "row still there after a later rollback (switch committed it)", seen, n == 1);
    }

    /* ---- P8: manual from a write until SQLEndTran, then autocommit -- */
    set_autocommit(0, NULL, 0);
    exec_sql("INSERT INTO #z7 VALUES (2)");
    r = end_tran(1, d, sizeof(d));
    snprintf(seen, sizeof(seen), "%s", rcname(r));
    report("P8a", "write, then SQLEndTran(COMMIT)", seen, SQL_SUCCEEDED(r));
    set_autocommit(1, NULL, 0);
    tc_check("P8b", "back in autocommit", 0);
    set_autocommit(0, NULL, 0);
    exec_sql("INSERT INTO #z7 VALUES (3)");
    end_tran(0, NULL, 0);
    set_autocommit(1, NULL, 0);
    {
        long n = query_int("SELECT COUNT(*) FROM #z7");
        snprintf(seen, sizeof(seen), "rows=%ld", n);
        report("P8c", "write, SQLEndTran(ROLLBACK), autocommit: row 3 gone", seen, n == 2);
    }
    exec_sql("DROP TABLE #z7");

    /* ---- P9: SQLEndTran on an autocommit connection ---------------- */
    set_autocommit(1, NULL, 0);
    r = end_tran(1, d, sizeof(d));
    snprintf(seen, sizeof(seen), "%s", rcname(r));
    report("P9a", "SQLEndTran(COMMIT) in autocommit, nothing open", seen, SQL_SUCCEEDED(r));
    if (d[0]) printf("      %s\n", d);
    r = end_tran(0, d, sizeof(d));
    snprintf(seen, sizeof(seen), "%s", rcname(r));
    report("P9b", "SQLEndTran(ROLLBACK) in autocommit, nothing open", seen, SQL_SUCCEEDED(r));
    if (d[0]) printf("      %s\n", d);

    /* ---- P10/P11: switching while a result set is open ------------- */
    pending_switch("P10", 0);
    pending_switch("P11", 1);

    /* ---- P12: cost -------------------------------------------------- */
    {
        const int N = 300;
        double a, b;

        reset_manual();
        a = now_us();
        for (int i = 0; i < N; i++) exec_sql("SELECT COUNT(*) FROM OCI_FIELD_TEST");
        b = now_us();
        double plain = (b - a) / N;
        reset_manual();

        a = now_us();
        for (int i = 0; i < N; i++)
        {
            end_tran(0, NULL, 0);
            set_autocommit(1, NULL, 0);
            exec_sql("SELECT COUNT(*) FROM OCI_FIELD_TEST");
            set_autocommit(0, NULL, 0);
        }
        b = now_us();
        double toggled = (b - a) / N;

        set_autocommit(1, NULL, 0);
        a = now_us();
        for (int i = 0; i < N; i++) exec_sql("SELECT COUNT(*) FROM OCI_FIELD_TEST");
        b = now_us();
        double inauto = (b - a) / N;

        a = now_us();
        for (int i = 0; i < N; i++) { set_autocommit(0, NULL, 0); set_autocommit(1, NULL, 0); }
        b = now_us();
        double pair = (b - a) / N;

        printf("\nP12   cost, average of %d (microseconds)\n", N);
        printf("      read in manual-commit (as today, transaction left open)  %8.0f\n", plain);
        printf("      rollback + ON + read + OFF   (switch around each read)   %8.0f\n", toggled);
        printf("      read with the connection left in autocommit              %8.0f\n", inauto);
        printf("      one OFF + ON pair on its own                             %8.0f\n", pair);
    }

    reset_manual();
    printf("\nRESULT: %d unexpected\n", g_unexpected);

    SQLDisconnect(g_dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, g_dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, g_env);
    return g_unexpected ? 1 : 0;
}
