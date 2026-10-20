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
 * Reserved UNIT_TEST_FIELD_TEST.NUMBER_COL: 999301 and 999302. Removed
 * at start and end.
 *
 *   Test 1   both sessions connect; the two rows set up and committed
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

    exec_sql(B, "DELETE FROM " T_UNIT " WHERE NUMBER_COL IN (999301, 999302)", 0, NULL, NULL, 0);
    D->commit(B, &L.dml);
    D->disconnect(B);
    printf("%s\n", failed ? "FAIL" : "PASS");
    h_close(&L);
    return failed ? 1 : 0;
}
