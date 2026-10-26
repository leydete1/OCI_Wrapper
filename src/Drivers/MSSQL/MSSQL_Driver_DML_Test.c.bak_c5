/*
 * MSSQL_Driver_DML_Test.c   (driver_mssql Stage 4, point U8)
 *
 * Checks driver_mssql's DML (mssql_dml.c) against a real SQL Server on
 * one standalone connection. Statements are built the way core builds
 * them (Insert/Update/Delete_Execute_Module.c): the dialect's
 * placeholders, value_expr() and row_locator_clauses() fed by
 * table_key().
 *
 * Usage:  MSSQL_Driver_DML_Test [config_mssql.ini]
 *
 * Reserved OCI_FIELD_TEST.NUMBER_COL range: 999401-999499;
 * UNIT_TEST_FIELD_TEST.NUMBER_COL: 999401-999499. Removed at start and
 * end; the seed rows are not touched.
 *
 *   Test 1   standalone connect
 *   Test 2   table_key: primary key, IDENTITY key, composite key (none),
 *            unknown table (none), second call from the cache
 *   Test 3   INSERT one row with OUTPUT inserted.[FIELD_TEST_ID]: one
 *            key back; date, binary (CONVERT ... 2) and both text LOBs
 *            (bound in the statement, U6) read back
 *   Test 4   multi-row INSERT (row_count 3): 3 keys, in row order, a NULL
 *            bind stored as NULL
 *   Test 5   UPDATE with OUTPUT matching 3 rows: the same 3 keys; matching
 *            none: 0
 *   Test 6   DELETE through dml_execute: rows_affected 3; again: 0
 *   Test 7   a table with no key (#temp): INSERT/UPDATE without OUTPUT,
 *            the row count reported as blank keys
 *   Test 8   a key longer than 23 characters (uniqueidentifier) fails
 *            the statement cleanly
 *   Test 9   errors come back as SQLSTATE text in out_error_message:
 *            duplicate key (2627), NOT NULL (515), bad table (208)
 *   Test 10  rollback removes an INSERT, commit keeps one
 *   Test 11  a 100 KB UTF-8 text LOB bound in the INSERT (U6), length
 *            checked on the server
 *
 * Prints PASS or FAIL as the last line; exit code 0 / 1.
 */

#define _POSIX_C_SOURCE 200809L

#include "mssql_harness_util.h"
#include "mssql_dml.h"
#include "MSSQL_Pool.h"

#define T_FIELD "DATA_MANAGER.OCI_FIELD_TEST"
#define T_UNIT  "DATA_MANAGER.UNIT_TEST_FIELD_TEST"

static const db_driver_t *D;
static oci_context_t     *C;
static h_loggers_t        L;
static int                g_fail;

static void bad(const char *fmt, const char *a)
{
    if (!g_fail) printf("FAILED\n");
    printf("    ");
    printf(fmt, a);
    printf("\n");
    g_fail = 1;
}

static int exec_sql(const char *sql, int nb, const char **b, int *rows, char *err, size_t errsz)
{
    db_dml_request_t r = { sql, nb, b };
    int n = 0;
    if (err && errsz) err[0] = '\0';
    int rc = D->dml_execute(C, &L.dml, &r, &n, err, errsz);
    if (rows) *rows = n;
    return rc;
}

/* INSERT/UPDATE built as core does it; returns the driver's rc. */
static int returning(const char *sql, int nb, const char **b, int row_count, db_rowid_result_t *res)
{
    db_dml_returning_request_t r;
    memset(&r, 0, sizeof(r));
    r.sql = sql;
    r.bind_count = nb;
    r.bind_values = b;
    r.returning_bind_position = nb + 1;
    r.row_count = row_count;
    return D->dml_execute_returning_rowids(C, &L.dml, &r, res);
}

/* The row-locator middle for a table, as core gets it (table_key). */
static void locator(db_dml_kind_t kind, const char *owner, const char *table, char *mid, size_t n)
{
    char key[128] = "", tail[64];
    int kc = 0;
    D->table_key(C, owner, table, key, sizeof(key), &kc);
    const char *keys[1] = { key };
    db_row_locator_ctx_t loc = { table, owner, kc ? keys : NULL, kc };
    if (D->dialect->row_locator_clauses(kind, &loc, 1, mid, n, tail, sizeof(tail)) != 0)
        mid[0] = '\0';
}

static void cleanup(void)
{
    exec_sql("DELETE FROM " T_FIELD " WHERE NUMBER_COL BETWEEN 999401 AND 999499", 0, NULL, NULL, NULL, 0);
    exec_sql("DELETE FROM " T_UNIT  " WHERE NUMBER_COL BETWEEN 999401 AND 999499", 0, NULL, NULL, NULL, 0);
    D->commit(C, &L.dml);
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ini = argc > 1 ? argv[1] : CONFIG_INI_MSSQL;
    oci_context_t ctx; app_config_t config;
    if (h_init(ini, &ctx, &config, &L) != 0) { printf("INIT FAILED\nFAIL\n"); return 1; }
    D = mssql_driver_get();
    C = &ctx;
    const db_dialect_t *dl = D->dialect;
    int failed = 0;
    char sql[4096], mid[256], v[4][256];
    db_rowid_result_t res;

    /* ---- Test 1 ---- */
    printf("Test 1 (standalone connect)          ... ");
    if (D->connect(&ctx) != 0 || !ctx.svchp)
    {
        printf("FAILED - connect(); see %s\nFAIL\n", config.connection_log_file_name);
        return 1;
    }
    printf("OK\n");
    cleanup();

    /* ---- Test 2 ---- */
    printf("Test 2 (table_key)                   ... ");
    g_fail = 0;
    {
        char key[128]; int kc = -1;
        if (D->table_key(&ctx, "DATA_MANAGER", "OCI_FIELD_TEST", key, sizeof(key), &kc) != 0 ||
            kc != 1 || strcmp(key, "FIELD_TEST_ID") != 0)
            bad("OCI_FIELD_TEST: want FIELD_TEST_ID, got '%s'", key);
        if (D->table_key(&ctx, "data_manager", "unit_test_field_test", key, sizeof(key), &kc) != 0 ||
            kc != 1 || strcmp(key, "NUMBER_COL") != 0)
            bad("UNIT_TEST_FIELD_TEST: want NUMBER_COL, got '%s'", key);
        if (D->table_key(&ctx, "", "APP_USER", key, sizeof(key), &kc) != 0 ||
            kc != 1 || strcmp(key, "USER_ID") != 0)
            bad("APP_USER (no owner): want USER_ID, got '%s'", key);
        if (D->table_key(&ctx, "DATA_MANAGER", "USER_ROLE", key, sizeof(key), &kc) != 0 || kc != 0)
            bad("USER_ROLE (composite key): want none, got '%s'", key);
        if (D->table_key(&ctx, "DATA_MANAGER", "NO_SUCH_TABLE", key, sizeof(key), &kc) != 0 || kc != 0)
            bad("NO_SUCH_TABLE: want none, got '%s'", key);
        if (D->table_key(&ctx, "DATA_MANAGER", "OCI_FIELD_TEST", key, sizeof(key), &kc) != 0 ||
            kc != 1 || strcmp(key, "FIELD_TEST_ID") != 0)
            bad("cached OCI_FIELD_TEST: got '%s'", key);
    }
    if (!g_fail) printf("OK (FIELD_TEST_ID, NUMBER_COL, USER_ID; composite and unknown: none)\n");
    failed |= g_fail;

    /* ---- Test 3 ---- */
    printf("Test 3 (INSERT with OUTPUT key)      ... ");
    g_fail = 0;
    char key3[32] = "";
    {
        char e_date[64], e_raw[64];
        dl->value_expr("DATETIME2", "?", e_date, sizeof(e_date));
        dl->value_expr("VARBINARY", "?", e_raw, sizeof(e_raw));
        locator(DB_DML_INSERT, "DATA_MANAGER", "OCI_FIELD_TEST", mid, sizeof(mid));
        snprintf(sql, sizeof(sql),
                 "INSERT INTO " T_FIELD " (NUMBER_COL, VARCHAR2_COL, DATE_COL, RAW_COL, CLOB_COL, "
                 "NCLOB_COL) %s VALUES (?, ?, %s, %s, ?, ?)", mid, e_date, e_raw);
        const char *b[] = { "999401", "dml test 3", "2030-02-03 04:05:06", "CAFEBABE",
                            "clob text 3", "nclob \xe6\x97\xa5\xe6\x9c\xac 3" };
        if (strcmp(mid, "OUTPUT inserted.[FIELD_TEST_ID]") != 0)
            bad("row locator: '%s'", mid);
        if (returning(sql, 6, b, 1, &res) != 0) bad("insert failed: %s", sql);
        else
        {
            if (res.count != 1 || !res.rowids[0]) bad("want 1 key, got %s", res.count == 1 ? "a blank" : "other");
            snprintf(key3, sizeof(key3), "%s", res.rowids);
            D->rowid_result_free(&res);
            const char *k[] = { key3 };
            int n = h_query(&ctx, "SELECT NUMBER_COL, CONVERT(varchar(19), DATE_COL, 120), "
                            "CONVERT(varchar(100), RAW_COL, 2), "
                            "CAST(LEFT(CLOB_COL, 40) AS varchar(40)) FROM " T_FIELD
                            " WHERE FIELD_TEST_ID = ?", 1, k, v, 4);
            if (n != 1) bad("row with key %s not found", key3);
            else
            {
                if (strcmp(v[0], "999401")) bad("NUMBER_COL '%s'", v[0]);
                if (strcmp(v[1], "2030-02-03 04:05:06")) bad("DATE_COL '%s'", v[1]);
                if (strcmp(v[2], "CAFEBABE")) bad("RAW_COL '%s'", v[2]);
                if (strcmp(v[3], "clob text 3")) bad("CLOB_COL '%s'", v[3]);
            }
            n = h_query(&ctx, "SELECT CAST(LEFT(NCLOB_COL, 40) AS nvarchar(40)) FROM " T_FIELD
                        " WHERE FIELD_TEST_ID = ?", 1, k, v, 1);
            if (n != 1 || strcmp(v[0], "nclob \xe6\x97\xa5\xe6\x9c\xac 3")) bad("NCLOB_COL '%s'", v[0]);
        }
    }
    if (!g_fail) printf("OK (key %s; date, binary, CLOB, NCLOB read back)\n", key3);
    failed |= g_fail;

    /* ---- Test 4 ---- */
    printf("Test 4 (multi-row INSERT, 3 rows)    ... ");
    g_fail = 0;
    char keys4[3][32] = { "", "", "" };
    {
        locator(DB_DML_INSERT, "DATA_MANAGER", "OCI_FIELD_TEST", mid, sizeof(mid));
        snprintf(sql, sizeof(sql), "INSERT INTO " T_FIELD " (NUMBER_COL, VARCHAR2_COL) %s VALUES (?, ?)", mid);
        const char *b[] = { "999411", "group 4", "999412", NULL, "999413", "group 4" };
        if (returning(sql, 2, b, 3, &res) != 0) bad("insert failed%s", "");
        else
        {
            if (res.count != 3) bad("want 3 keys%s", "");
            for (int i = 0; i < 3 && i < res.count; i++)
                snprintf(keys4[i], sizeof(keys4[i]), "%s", res.rowids + (size_t)i * MSSQL_ROWID_BUF_SIZE);
            D->rowid_result_free(&res);
            if (!(atol(keys4[0]) < atol(keys4[1]) && atol(keys4[1]) < atol(keys4[2])))
                bad("keys not in row order: %s", keys4[0]);
            if (h_count(&ctx, "SELECT COUNT(*) FROM " T_FIELD
                        " WHERE NUMBER_COL BETWEEN 999411 AND 999413", 0, NULL) != 3)
                bad("want 3 rows in the table%s", "");
            const char *k[] = { keys4[1] };
            if (h_count(&ctx, "SELECT COUNT(*) FROM " T_FIELD
                        " WHERE FIELD_TEST_ID = ? AND VARCHAR2_COL IS NULL", 1, k) != 1)
                bad("NULL bind not stored as NULL (key %s)", keys4[1]);
        }
    }
    if (!g_fail) printf("OK (keys %s, %s, %s)\n", keys4[0], keys4[1], keys4[2]);
    failed |= g_fail;

    /* ---- Test 5 ---- */
    printf("Test 5 (UPDATE with OUTPUT)          ... ");
    g_fail = 0;
    {
        locator(DB_DML_UPDATE, "DATA_MANAGER", "OCI_FIELD_TEST", mid, sizeof(mid));
        snprintf(sql, sizeof(sql), "UPDATE " T_FIELD " SET VARCHAR2_COL=? %s WHERE NUMBER_COL BETWEEN ? AND ?", mid);
        const char *b[] = { "updated 5", "999411", "999413" };
        if (returning(sql, 3, b, 0, &res) != 0) bad("update failed%s", "");
        else
        {
            if (res.count != 3) bad("want 3 keys back%s", "");
            for (int i = 0; i < res.count; i++)
            {
                const char *k = res.rowids + (size_t)i * MSSQL_ROWID_BUF_SIZE;
                if (strcmp(k, keys4[0]) && strcmp(k, keys4[1]) && strcmp(k, keys4[2]))
                    bad("unexpected key %s", k);
            }
            D->rowid_result_free(&res);
        }
        const char *b0[] = { "none", "999498", "999499" };
        if (returning(sql, 3, b0, 0, &res) != 0 || res.count != 0) bad("no-match update: want 0 keys%s", "");
        D->rowid_result_free(&res);
    }
    if (!g_fail) printf("OK (3 keys; no match: 0)\n");
    failed |= g_fail;

    /* ---- Test 6 ---- */
    printf("Test 6 (DELETE through dml_execute)  ... ");
    g_fail = 0;
    {
        int rows = -1;
        const char *b[] = { "999411", "999413" };
        if (exec_sql("DELETE FROM " T_FIELD " WHERE NUMBER_COL BETWEEN ? AND ?", 2, b, &rows, NULL, 0) != 0 ||
            rows != 3)
            bad("want rows_affected 3%s", "");
        rows = -1;
        if (exec_sql("DELETE FROM " T_FIELD " WHERE NUMBER_COL BETWEEN ? AND ?", 2, b, &rows, NULL, 0) != 0 ||
            rows != 0)
            bad("second delete: want 0 rows and success%s", "");
    }
    if (!g_fail) printf("OK (3 rows, then 0)\n");
    failed |= g_fail;

    /* ---- Test 7 ---- */
    printf("Test 7 (table with no key)           ... ");
    g_fail = 0;
    {
        if (h_exec(&ctx, "CREATE TABLE #DM_NOKEY (A int, B varchar(20))", NULL, 0) != 0)
            bad("CREATE TABLE #DM_NOKEY failed%s", "");
        locator(DB_DML_INSERT, "", "#DM_NOKEY", mid, sizeof(mid));
        if (mid[0]) bad("no key: want no OUTPUT clause, got '%s'", mid);
        const char *b[] = { "1", "x", "2", "y" };
        if (returning("INSERT INTO #DM_NOKEY (A, B) VALUES (?, ?)", 2, b, 2, &res) != 0 || res.count != 2 ||
            res.rowids[0] != '\0')
            bad("insert: want 2 blank keys%s", "");
        D->rowid_result_free(&res);
        const char *u[] = { "z" };
        if (returning("UPDATE #DM_NOKEY SET B=? WHERE A > 0", 1, u, 0, &res) != 0 || res.count != 2)
            bad("update: want 2 rows reported%s", "");
        D->rowid_result_free(&res);
    }
    if (!g_fail) printf("OK (no OUTPUT; 2 rows reported each time)\n");
    failed |= g_fail;

    /* ---- Test 8 ---- */
    printf("Test 8 (key longer than 23 chars)    ... ");
    g_fail = 0;
    {
        if (h_exec(&ctx, "CREATE TABLE #DM_GUID (ID uniqueidentifier NOT NULL DEFAULT NEWID() PRIMARY KEY, "
                   "B varchar(20))", NULL, 0) != 0)
            bad("CREATE TABLE #DM_GUID failed%s", "");
        const char *b[] = { "g" };
        if (returning("INSERT INTO #DM_GUID (B) OUTPUT inserted.[ID] VALUES (?)", 1, b, 1, &res) == 0)
        {
            bad("want a failure, got %s", res.count ? res.rowids : "no key");
            D->rowid_result_free(&res);
        }
        if (h_count(&ctx, "SELECT 1 AS X", 0, NULL) != 1) bad("connection unusable afterwards%s", "");
    }
    if (!g_fail) printf("OK (-1, logged; connection still answers)\n");
    failed |= g_fail;

    /* ---- Test 9 ---- */
    printf("Test 9 (errors as SQLSTATE text)     ... ");
    g_fail = 0;
    {
        char err[1024];
        int rows;
        const char *b[] = { "999421", "dup" };
        exec_sql("INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, b, &rows, NULL, 0);
        if (exec_sql("INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, b, &rows,
                     err, sizeof(err)) == 0 || !strstr(err, "[native 2627]"))
            bad("duplicate key: want [native 2627], got '%s'", err);
        const char *bn[] = { NULL, "no key" };
        if (exec_sql("INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, bn, &rows,
                     err, sizeof(err)) == 0 || !strstr(err, "[native 515]"))
            bad("NOT NULL: want [native 515], got '%s'", err);
        if (exec_sql("DELETE FROM DATA_MANAGER.NO_SUCH_TABLE", 0, NULL, &rows, err, sizeof(err)) == 0 ||
            !strstr(err, "[native 208]"))
            bad("bad table: want [native 208], got '%s'", err);
        D->rollback(&ctx, &L.dml);
    }
    if (!g_fail) printf("OK (2627, 515, 208)\n");
    failed |= g_fail;

    /* ---- Test 10 ---- */
    printf("Test 10 (rollback and commit)        ... ");
    g_fail = 0;
    {
        int rows;
        const char *b1[] = { "999431", "rolled back" };
        exec_sql("INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, b1, &rows, NULL, 0);
        D->rollback(&ctx, &L.dml);
        if (h_count(&ctx, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999431", 0, NULL) != 0)
            bad("rolled-back row still there%s", "");
        const char *b2[] = { "999432", "committed" };
        exec_sql("INSERT INTO " T_UNIT " (NUMBER_COL, VARCHAR2_COL) VALUES (?, ?)", 2, b2, &rows, NULL, 0);
        D->commit(&ctx, &L.dml);
        if (h_count(&ctx, "SELECT COUNT(*) FROM " T_UNIT " WHERE NUMBER_COL = 999432", 0, NULL) != 1)
            bad("committed row missing%s", "");
    }
    if (!g_fail) printf("OK\n");
    failed |= g_fail;

    /* ---- Test 11 ---- */
    printf("Test 11 (100 KB text LOB, bound)     ... ");
    g_fail = 0;
    {
        size_t n = 0, cap = 110000;
        char *big = malloc(cap);
        const char *piece = "abc \xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80 ";   /* 1+1+1+1+2+3+4+1 bytes */
        size_t pl = strlen(piece);
        while (n + pl < 100000) { memcpy(big + n, piece, pl); n += pl; }
        big[n] = '\0';
        locator(DB_DML_INSERT, "DATA_MANAGER", "OCI_FIELD_TEST", mid, sizeof(mid));
        snprintf(sql, sizeof(sql), "INSERT INTO " T_FIELD " (NUMBER_COL, CLOB_COL) %s VALUES (?, ?)", mid);
        const char *b[] = { "999441", big };
        if (returning(sql, 2, b, 1, &res) != 0 || res.count != 1) bad("insert failed%s", "");
        else
        {
            const char *k[] = { res.rowids };
            char want[32];
            snprintf(want, sizeof(want), "%zu", n);
            if (h_query(&ctx, "SELECT DATALENGTH(CLOB_COL) FROM " T_FIELD " WHERE FIELD_TEST_ID = ?",
                        1, k, v, 1) != 1 || strcmp(v[0], want))
                bad("DATALENGTH: got %s", v[0]);
            else printf("OK (%s bytes)\n", v[0]);
            D->rowid_result_free(&res);
        }
        free(big);
    }
    failed |= g_fail;

    cleanup();
    D->disconnect(&ctx);
    printf("%s\n", failed ? "FAIL" : "PASS");
    h_close(&L);
    return failed ? 1 : 0;
}
