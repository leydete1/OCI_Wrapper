/*
 * MSSQL_Driver_Transaction_Test.c   (driver_mssql Stage 4, point U8)
 *
 * Two sessions against a real SQL Server: a deadlock (1205) and a lock
 * timeout (1222), and what each leaves behind. Session A runs in a child
 * process with its own connection, so a statement blocked in A can never
 * hold up B inside the ODBC driver manager; the two processes step each
 * other through pipes.
 *
 * Usage:  MSSQL_Driver_Transaction_Test [config_mssql.ini]
 *
 * Reserved UNIT_TEST_FIELD_TEST.NUMBER_COL: 999301-999309. Removed at
 * start and end.
 *
 *   Test 1   both sessions connect (Z7: B starts in autocommit, with
 *            @@TRANCOUNT 0); the two rows set up and committed
 *   Test 2   deadlock: B (DEADLOCK_PRIORITY LOW) holds 999302, A holds
 *            999301 then waits for 999302, B asks for 999301. B is the
 *            victim: dml_execute returns -1 with [native 1205]; A's
 *            statement completes and A commits
 *   Test 3   after 1205 B's whole transaction is gone (both rows hold
 *            A's values, also after B's commit), commit() returns 0 -
 *            never DB_TX_RETRYABLE - and B's connection still works
 *   Test 4   lock timeout: A holds 999301 uncommitted; B, with
 *            SET LOCK_TIMEOUT 1000, gets -1 with [native 1222] after
 *            about a second, not a hang; A rolls back
 *   Test 5   session A ended cleanly (exit status 0)
 *
 * Z7 (25 Oct) - connections sit in autocommit; a write switches to
 * manual-commit until commit() / rollback(). Reserved 999303-999309.
 *   Test 6   after a read: autocommit, @@TRANCOUNT 0
 *   Test 7   a write: manual-commit, @@TRANCOUNT 1, not visible to a
 *            checker; after commit: visible, autocommit, @@TRANCOUNT 0
 *   Test 8   a write then rollback: undone, back in autocommit
 *   Test 9   inside a managed transaction: a read before the first write
 *            opens nothing; a read after it sees its own write and is in
 *            the transaction; rollback undoes it
 *   Test 10  a commit that fails leaves the connection in manual-commit
 *            (switching autocommit on would commit the failed work)
 *   Test 11  autocommit_mode = 1: a write commits at once and the
 *            connection never switches
 *
 * Prints PASS or FAIL as the last line; exit code 0 / 1.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mssql_harness_util.h"

#define T_UNIT "DATA_MANAGER.UNIT_TEST_FIELD_TEST"
#define STEP_TIMEOUT_S 60

static const db_driver_t *D;
static h_loggers_t        L;
static int                g_fail;
static int                to_child[2], to_parent[2];

static void bad(const char *fmt, const char *a)
{
    if (!g_fail) printf("FAILED\n");
    printf("    ");
    printf(fmt, a);
    printf("\n");
    g_fail = 1;
}

static int exec_sql(oci_context_t *c, const char *sql, int nb, const char **b,
                    char *err, size_t errsz)
{
    db_dml_request_t r = { sql, nb, b };
    int n = 0;
    char local[1024];
    if (!err) { err = local; errsz = sizeof(local); }
    err[0] = '\0';
    return D->dml_execute(c, c->update_logger, &r, &n, err, errsz);
}

static int set_value(oci_context_t *c, const char *key, const char *val, char *err, size_t errsz)
{
    const char *b[] = { val, key };
    return exec_sql(c, "UPDATE " T_UNIT " SET VARCHAR2_COL = ? WHERE NUMBER_COL = ?", 2, b, err, errsz);
}

static void send_byte(int fd, char c)
{
    while (write(fd, &c, 1) < 0 && errno == EINTR) ;
}

/* Next byte from the other process; 0 on timeout or a closed pipe. */
static char recv_byte(int fd)
{
    fd_set s;
    struct timeval tv = { STEP_TIMEOUT_S, 0 };
    FD_ZERO(&s);
    FD_SET(fd, &s);
    if (select(fd + 1, &s, NULL, NULL, &tv) <= 0) return 0;
    char c = 0;
    return read(fd, &c, 1) == 1 ? c : 0;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/*  Session A (child process)                                          */
/*  Replies: 'R' ready, 'L' holds 999301, 'D' its 999302 update done   */
/*  and committed, 'K' holds 999301 again, 'E' rolled back. 'X' = an   */
/*  unexpected failure.                                                */
/* ------------------------------------------------------------------ */
static int session_a(const char *ini)
{
    oci_context_t a; app_config_t config; h_loggers_t la;
    int in = to_child[0], out = to_parent[1];
    if (h_init(ini, &a, &config, &la) != 0 || D->connect(&a) != 0) { send_byte(out, 'X'); return 1; }
    send_byte(out, 'R');

    if (recv_byte(in) != 'g') return 1;
    if (set_value(&a, "999301", "A", NULL, 0) != 0) { send_byte(out, 'X'); return 1; }
    send_byte(out, 'L');

    if (recv_byte(in) != 'h') return 1;
    int rc = set_value(&a, "999302", "A", NULL, 0);    /* blocks until B is the victim */
    if (rc == 0) rc = D->commit(&a, a.update_logger);
    send_byte(out, rc == 0 ? 'D' : 'X');

    if (recv_byte(in) != 'k') return 1;
    if (set_value(&a, "999301", "held by A", NULL, 0) != 0) { send_byte(out, 'X'); return 1; }
    send_byte(out, 'K');

    if (recv_byte(in) != 'e') return 1;
    D->rollback(&a, a.update_logger);
    D->disconnect(&a);
    send_byte(out, 'E');
    h_close(&la);
    return 0;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ini = argc > 1 ? argv[1] : CONFIG_INI_MSSQL;
    D = mssql_driver_get();
    if (pipe(to_child) != 0 || pipe(to_parent) != 0) { printf("pipe failed\nFAIL\n"); return 1; }

    /* Fork before any ODBC call, so the child has its own driver manager
     * state and its own connection. */
    pid_t pid = fork();
    if (pid < 0) { printf("fork failed\nFAIL\n"); return 1; }
    if (pid == 0)
    {
        /* Session A reports through the pipe only; its ini dump and
         * logging stay out of the results. */
        if (!freopen("/dev/null", "w", stdout)) _exit(1);
        close(to_child[1]);
        close(to_parent[0]);
        _exit(session_a(ini));
    }
    close(to_child[0]);
    close(to_parent[1]);
    int in = to_parent[0], out = to_child[1];

    oci_context_t ctx; app_config_t config;
    if (h_init(ini, &ctx, &config, &L) != 0)
    {
        printf("INIT FAILED\nFAIL\n");
        kill(pid, SIGTERM);
        return 1;
    }
    oci_context_t *B = &ctx;
    int failed = 0;
    char err[1024], v[1][256];

    /* ---- Test 1 ---- */
    printf("Test 1 (two sessions, rows set up)   ... ");
    g_fail = 0;
    if (D->connect(B) != 0 || !B->svchp) bad("session B connect failed; see %s", config.connection_log_file_name);
    else if (mssql_tx_in_manual(B->svchp) != 0 || h_trancount(B) != 0)
        bad("a new connection is not in autocommit with nothing open (Z7)%s", "");
    char r = recv_byte(in);
    if (r != 'R') bad("session A did not connect (%s)", r ? "reported a failure" : "no answer");
    if (g_fail)
    {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        printf("FAIL\n");
        return 1;
    }
    exec_sql(B, "DELETE FROM " T_UNIT " WHERE NUMBER_COL IN (999301, 999302)", 0, NULL, NULL, 0);
    const char *seed[] = { "999301", "seed", "999302", "seed" };
    if (exec_sql(B, "INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, seed, err, sizeof(err)) ||
        exec_sql(B, "INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, seed + 2, err, sizeof(err)) ||
        D->commit(B, &L.dml) != 0)
        bad("set-up failed: %s", err);
    if (!g_fail) printf("OK\n");
    failed |= g_fail;

    /* ---- Test 2 ---- */
    printf("Test 2 (deadlock, B the victim)      ... ");
    g_fail = 0;
    double t0 = 0, waited = 0;
    {
        if (h_exec(B, "SET DEADLOCK_PRIORITY LOW", err, sizeof(err)) != 0)
            bad("SET DEADLOCK_PRIORITY LOW failed: %s", err);
        if (set_value(B, "999302", "B", err, sizeof(err)) != 0)
            bad("B's update of 999302 failed: %s", err);
        send_byte(out, 'g');
        if ((r = recv_byte(in)) != 'L') bad("session A could not lock 999301%s", "");
        send_byte(out, 'h');
        sleep(1);                                   /* A is now waiting for 999302 */
        t0 = now_s();
        int rc = set_value(B, "999301", "B", err, sizeof(err));
        waited = now_s() - t0;
        if (rc == 0) bad("B's update of 999301 succeeded - want the deadlock victim%s", "");
        else if (!strstr(err, "[native 1205]")) bad("want [native 1205], got '%s'", err);
        if ((r = recv_byte(in)) != 'D') bad("session A's update/commit did not complete%s", "");
    }
    if (!g_fail) printf("OK (1205 after %.1f s; A completed and committed)\n", waited);
    failed |= g_fail;

    /* ---- Test 3 ---- */
    printf("Test 3 (after 1205)                  ... ");
    g_fail = 0;
    {
        if (h_query(B, "SELECT VARCHAR2_COL FROM " T_UNIT " WHERE NUMBER_COL = 999302", 0, NULL, v, 1) != 1 ||
            strcmp(v[0], "A"))
            bad("999302 holds '%s', want A (B's earlier update must be gone)", v[0]);
        int rc = D->commit(B, &L.dml);
        if (rc == DB_TX_RETRYABLE) bad("commit() returned DB_TX_RETRYABLE%s", "");
        else if (rc != 0) bad("commit() failed%s", "");
        if (h_query(B, "SELECT VARCHAR2_COL FROM " T_UNIT " WHERE NUMBER_COL = 999302", 0, NULL, v, 1) != 1 ||
            strcmp(v[0], "A"))
            bad("after B's commit 999302 holds '%s', want A", v[0]);
        if (h_query(B, "SELECT VARCHAR2_COL FROM " T_UNIT " WHERE NUMBER_COL = 999301", 0, NULL, v, 1) != 1 ||
            strcmp(v[0], "A"))
            bad("999301 holds '%s', want A", v[0]);
        if (set_value(B, "999302", "B again", err, sizeof(err)) != 0 || D->rollback(B, &L.dml) != 0)
            bad("B's connection unusable: %s", err);
    }
    if (!g_fail) printf("OK (B's transaction gone; commit 0; connection usable)\n");
    failed |= g_fail;

    /* ---- Test 4 ---- */
    printf("Test 4 (lock timeout)                ... ");
    g_fail = 0;
    {
        send_byte(out, 'k');
        if ((r = recv_byte(in)) != 'K') bad("session A could not hold 999301%s", "");
        if (h_exec(B, "SET LOCK_TIMEOUT 1000", err, sizeof(err)) != 0)
            bad("SET LOCK_TIMEOUT failed: %s", err);
        t0 = now_s();
        int rc = set_value(B, "999301", "B", err, sizeof(err));
        waited = now_s() - t0;
        if (rc == 0) bad("B's update succeeded while A holds the row%s", "");
        else if (!strstr(err, "[native 1222]")) bad("want [native 1222], got '%s'", err);
        if (waited > 10.0) bad("took too long%s", "");
        D->rollback(B, &L.dml);
        h_exec(B, "SET LOCK_TIMEOUT -1", NULL, 0);
        send_byte(out, 'e');
        if ((r = recv_byte(in)) != 'E') bad("session A did not roll back%s", "");
    }
    if (!g_fail) printf("OK (1222 after %.1f s)\n", waited);
    failed |= g_fail;

    /* ---- Test 5 ---- */
    printf("Test 5 (session A ended)             ... ");
    g_fail = 0;
    {
        int status = -1;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            bad("session A exit status %s", "not 0");
    }
    if (!g_fail) printf("OK\n");
    failed |= g_fail;

    /* ================================================================
     * Z7 (25 Oct): connections sit in autocommit; a write switches to
     * manual-commit until commit() or rollback(). Checked on B's own
     * connection (mode and @@TRANCOUNT) and, for what is committed, on an
     * independent checker connection that reads only committed data.
     * ================================================================ */
    void *chk = mssql_open_connection(B->envhp, &config, &L.conn);
    if (!chk) { printf("checker connection failed - Tests 6-11 not run\n"); failed = 1; }
    else
    {
        exec_sql(B, "DELETE FROM " T_UNIT " WHERE NUMBER_COL BETWEEN 999303 AND 999309", 0, NULL, NULL, 0);
        D->commit(B, &L.dml);
        char m[256];

        /* ---- Test 6 ---- */
        printf("Test 6 (Z7: idle after a read)       ... ");
        g_fail = 0;
        if (h_query(B, "SELECT COUNT(*) FROM " T_UNIT, 0, NULL, v, 1) != 1) bad("read failed%s", "");
        if (mssql_tx_in_manual(B->svchp) != 0) bad("connection in manual-commit after a read%s", "");
        long tc = h_trancount(B);
        if (tc != 0) { snprintf(m, sizeof(m), "@@TRANCOUNT %ld after a read (want 0)", tc); bad("%s", m); }
        if (!g_fail) printf("OK (autocommit, @@TRANCOUNT 0)\n");
        failed |= g_fail;

        /* ---- Test 7 ---- */
        printf("Test 7 (Z7: write, then commit)      ... ");
        g_fail = 0;
        {
            const char *r7[] = { "999303", "z7" };
            if (exec_sql(B, "INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, r7, err, sizeof(err)) != 0)
                bad("insert failed: %s", err);
            long t1 = h_trancount(B), c1 = h_raw_long(chk, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999303");
            if (mssql_tx_in_manual(B->svchp) != 1 || t1 != 1)
            { snprintf(m, sizeof(m), "after the write: manual-commit %d, @@TRANCOUNT %ld (want 1, 1)", mssql_tx_in_manual(B->svchp), t1); bad("%s", m); }
            if (c1 != 0) bad("the checker sees the row before the commit%s", "");
            if (D->commit(B, &L.dml) != 0) bad("commit failed%s", "");
            long t2 = h_trancount(B), c2 = h_raw_long(chk, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999303");
            if (mssql_tx_in_manual(B->svchp) != 0 || t2 != 0)
            { snprintf(m, sizeof(m), "after the commit: manual-commit %d, @@TRANCOUNT %ld (want 0, 0)", mssql_tx_in_manual(B->svchp), t2); bad("%s", m); }
            if (c2 != 1) { snprintf(m, sizeof(m), "the checker sees %ld rows after the commit (want 1)", c2); bad("%s", m); }
        }
        if (!g_fail) printf("OK (manual-commit while pending; committed; back in autocommit)\n");
        failed |= g_fail;

        /* ---- Test 8 ---- */
        printf("Test 8 (Z7: write, then rollback)    ... ");
        g_fail = 0;
        {
            if (set_value(B, "999303", "rolled back", err, sizeof(err)) != 0) bad("update failed: %s", err);
            if (mssql_tx_in_manual(B->svchp) != 1) bad("not in manual-commit after the write%s", "");
            if (D->rollback(B, &L.dml) != 0) bad("rollback failed%s", "");
            if (mssql_tx_in_manual(B->svchp) != 0 || h_trancount(B) != 0) bad("not back in autocommit after the rollback%s", "");
            if (h_raw_long(chk, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999303 AND VARCHAR2_COL = 'z7'") != 1)
                bad("the rolled-back update is visible%s", "");
        }
        if (!g_fail) printf("OK (update undone; back in autocommit)\n");
        failed |= g_fail;

        /* ---- Test 9 ---- */
        printf("Test 9 (Z7: managed tx read/write)   ... ");
        g_fail = 0;
        {
            static int dummy_tx;
            B->active_tx = (void *)&dummy_tx;
            if (h_query(B, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999304", 0, NULL, v, 1) != 1 || strcmp(v[0], "0"))
                bad("first read: '%s' (want 0)", v[0]);
            if (h_trancount(B) != 0) bad("a read before the first write opened a transaction%s", "");
            const char *r9[] = { "999304", "z7 tx" };
            if (exec_sql(B, "INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, r9, err, sizeof(err)) != 0)
                bad("insert failed: %s", err);
            if (h_query(B, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999304", 0, NULL, v, 1) != 1 || strcmp(v[0], "1"))
                bad("read after the write: '%s' (want 1 - its own write)", v[0]);
            if (h_trancount(B) != 1) bad("the read after the write is not in the transaction%s", "");
            B->active_tx = NULL;
            D->rollback(B, &L.dml);
            if (h_raw_long(chk, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999304") != 0)
                bad("row 999304 survived the rollback%s", "");
        }
        if (!g_fail) printf("OK (read outside, write and read inside; rolled back)\n");
        failed |= g_fail;

        /* ---- Test 10 ---- */
        printf("Test 10 (Z7: commit fails - stays)   ... ");
        g_fail = 0;
        {
            /* A commit that fails: a connection handle that was never
             * connected, set to manual-commit, then commit(). SQLEndTran
             * fails there (08003); the driver must not then switch the
             * handle to autocommit. (A live commit failure cannot be made
             * on demand as DATA_MANAGER.) */
            void *dead = NULL;
            if (SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, (SQLHENV)B->envhp, (SQLHDBC *)&dead)))
            {
                oci_context_t dctx;
                memset(&dctx, 0, sizeof(dctx));
                dctx.svchp = dead;
                dctx.ini   = &config;
                SQLSetConnectAttr((SQLHDBC)dead, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
                int rc = D->commit(&dctx, &L.dml);
                if (rc == 0) bad("commit on an unconnected handle succeeded%s", "");
                if (mssql_tx_in_manual(dead) != 1) bad("switched to autocommit after a failed commit%s", "");
                SQLFreeHandle(SQL_HANDLE_DBC, (SQLHDBC)dead);
            }
            else bad("could not allocate a handle%s", "");
        }
        if (!g_fail) printf("OK (commit -1; left in manual-commit)\n");
        failed |= g_fail;

        /* ---- Test 11 ---- */
        printf("Test 11 (Z7: autocommit_mode = 1)    ... ");
        g_fail = 0;
        {
            config.autocommit_mode = 1;
            const char *r11[] = { "999305", "autocommit" };
            if (exec_sql(B, "INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, r11, err, sizeof(err)) != 0)
                bad("insert failed: %s", err);
            if (mssql_tx_in_manual(B->svchp) != 0) bad("switched to manual-commit with autocommit_mode = 1%s", "");
            if (h_raw_long(chk, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999305") != 1)
                bad("the row is not committed at once%s", "");
            if (D->commit(B, &L.dml) != 0) bad("commit (nothing pending) failed%s", "");
            config.autocommit_mode = 0;
        }
        if (!g_fail) printf("OK (committed at once; never manual-commit)\n");
        failed |= g_fail;

        exec_sql(B, "DELETE FROM " T_UNIT " WHERE NUMBER_COL BETWEEN 999303 AND 999309", 0, NULL, NULL, 0);
        D->commit(B, &L.dml);
        mssql_close_connection(chk, &L.conn);
    }

    exec_sql(B, "DELETE FROM " T_UNIT " WHERE NUMBER_COL IN (999301, 999302)", 0, NULL, NULL, 0);
    D->commit(B, &L.dml);
    D->disconnect(B);
    printf("%s\n", failed ? "FAIL" : "PASS");
    h_close(&L);
    return failed ? 1 : 0;
}
