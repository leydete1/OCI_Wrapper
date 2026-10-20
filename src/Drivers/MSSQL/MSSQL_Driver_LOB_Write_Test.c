/*
 * MSSQL_Driver_LOB_Write_Test.c   (driver_mssql Stage 4, points U5, U6, U8)
 *
 * Checks driver_mssql's LOB writes against a real SQL Server on one
 * standalone connection. Content is compared by SHA-256 against
 * HASHBYTES('SHA2_256', ...) on the server - reading LOBs back through
 * the driver is Stage 5.
 *
 * Usage:  MSSQL_Driver_LOB_Write_Test [config_mssql.ini]
 *
 * Reserved rows: OCI_LOB_TEST.ID and OCI_CLOB_TEST.ID 999501-999509,
 * OCI_FIELD_TEST.NUMBER_COL 999501-999509. Removed at start and end. The
 * test files go in /tmp and are removed at the end.
 *
 *   Test 1   standalone connect
 *   Test 2   BLOB: INSERT with the 0x placeholder and OUTPUT key, then
 *            lob_write_by_rowid of a 300 KB file (DATA_AT_EXEC chunks);
 *            bytes written, DATALENGTH and SHA-256 match
 *   Test 3   BLOB rewritten with a 1000-byte file: the whole value is
 *            replaced, not overlaid
 *   Test 4   BLOB from an empty file: 0 bytes, the 0x stays (as Oracle)
 *   Test 5   refusals: a missing file, a key no row has, a table with no
 *            key - each -1, the row unchanged
 *   Test 6   text through lob_write_by_rowid with chunk_read_size 1000:
 *            chunks never split a UTF-8 character (2-, 3- and 4-byte
 *            characters across every boundary); varchar(max) and
 *            nvarchar(max) both match by SHA-256 of their UTF-8 bytes
 *   Test 7   200 KB nvarchar(max) bound in the INSERT itself (U6), the
 *            way core sends a text LOB on SQL Server; SHA-256 matches
 *   Test 8   AUDIT_TRAIL stays write-once: an INSERT with both text LOBs
 *            inline succeeds; an UPDATE is refused (229), and so is a
 *            LOB write - also with 229, read from the update log; rolled
 *            back
 *
 * Prints PASS or FAIL as the last line; exit code 0 / 1.
 */

#define _POSIX_C_SOURCE 200809L

#include <unistd.h>

#include "mssql_harness_util.h"
#include "mssql_dml.h"

#define T_LOB   "DATA_MANAGER.OCI_LOB_TEST"
#define T_CLOB  "DATA_MANAGER.OCI_CLOB_TEST"
#define T_FIELD "DATA_MANAGER.OCI_FIELD_TEST"
#define T_AUDIT "DATA_MANAGER.AUDIT_TRAIL"

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

static int exec_sql(const char *sql, int nb, const char **b, char *err, size_t errsz)
{
    db_dml_request_t r = { sql, nb, b };
    int n = 0;
    if (err && errsz) err[0] = '\0';
    return D->dml_execute(C, &L.dml, &r, &n, err, errsz);
}

/* INSERT ... OUTPUT inserted.[key] ...; the one key into key. */
static int insert_key(const char *sql, int nb, const char **b, char *key, size_t keysz)
{
    db_dml_returning_request_t r;
    db_rowid_result_t res = { NULL, 0 };
    memset(&r, 0, sizeof(r));
    r.sql = sql; r.bind_count = nb; r.bind_values = b; r.returning_bind_position = nb + 1;
    r.row_count = 1;
    key[0] = '\0';
    int rc = D->dml_execute_returning_rowids(C, &L.dml, &r, &res);
    if (rc == 0 && res.count == 1) snprintf(key, keysz, "%s", res.rowids);
    if (rc == 0) D->rowid_result_free(&res);
    return rc == 0 && key[0] ? 0 : -1;
}

static int lob_write(int is_blob, const char *table, const char *col, const char *key,
                     const char *file, const char *text, size_t text_len, uint64_t *bytes)
{
    db_lob_write_request_t r = { is_blob, table, col, key, file, text, text_len };
    *bytes = 0;
    return D->lob_write_by_rowid(C, &L.dml, &r, bytes);
}

static int write_file(const char *path, const unsigned char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = n ? fwrite(data, 1, n, f) : 0;
    fclose(f);
    return w == n ? 0 : -1;
}

/* Server-side SHA-256 / DATALENGTH of one value, as text. */
static void server_hash(const char *expr, const char *table, const char *keycol, const char *key,
                        char *hash, char *len)
{
    char sql[512], v[2][256];
    const char *k[] = { key };
    snprintf(sql, sizeof(sql),
             "SELECT CONVERT(varchar(64), HASHBYTES('SHA2_256', %s), 2), DATALENGTH(%s) "
             "FROM %s WHERE %s = ?", expr, expr, table, keycol);
    if (h_query(C, sql, 1, k, v, 2) != 1) { strcpy(hash, "(no row)"); strcpy(len, "-"); return; }
    snprintf(hash, 65, "%.64s", v[0]);
    snprintf(len, 32, "%.31s", v[1]);
}

static void cleanup(void)
{
    exec_sql("DELETE FROM " T_LOB   " WHERE ID BETWEEN 999501 AND 999509", 0, NULL, NULL, 0);
    exec_sql("DELETE FROM " T_CLOB  " WHERE ID BETWEEN 999501 AND 999509", 0, NULL, NULL, 0);
    exec_sql("DELETE FROM " T_FIELD " WHERE NUMBER_COL BETWEEN 999501 AND 999509", 0, NULL, NULL, 0);
    D->commit(C, &L.dml);
}

/* Text of about n bytes of 1-, 2-, 3- and 4-byte characters in a
 * 23-byte cycle, so 1000-byte chunk boundaries fall inside characters
 * (1000 is not a multiple of 23). */
static char *make_text(size_t n, size_t *out_len)
{
    static const char *cyc[] = { "a", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "Z",
                                 "\xe6\x97\xa5", "\xd0\x96", "\xf0\x9d\x84\x9e", "\xe2\x82\xac" };
    char *t = malloc(n + 8);
    size_t u = 0;
    for (int i = 0; u < n; i++)
    {
        const char *c = cyc[i % 9];
        size_t l = strlen(c);
        memcpy(t + u, c, l);
        u += l;
    }
    t[u] = '\0';
    *out_len = u;
    return t;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ini = argc > 1 ? argv[1] : CONFIG_INI_MSSQL;
    oci_context_t ctx; app_config_t config;
    if (h_init(ini, &ctx, &config, &L) != 0) { printf("INIT FAILED\nFAIL\n"); return 1; }
    D = mssql_driver_get();
    C = &ctx;
    int failed = 0;
    char key[32], hash[65], len[32], want[65];
    uint64_t bytes;

    char f_big[64], f_small[64], f_empty[64];
    snprintf(f_big,   sizeof(f_big),   "/tmp/mssql_lob_big_%d.bin",   (int)getpid());
    snprintf(f_small, sizeof(f_small), "/tmp/mssql_lob_small_%d.bin", (int)getpid());
    snprintf(f_empty, sizeof(f_empty), "/tmp/mssql_lob_empty_%d.bin", (int)getpid());
    size_t big_n = 300 * 1024 + 7;
    unsigned char *big = malloc(big_n);
    unsigned int seed = 12345;
    for (size_t i = 0; i < big_n; i++) { seed = seed * 1103515245u + 12345u; big[i] = (unsigned char)(seed >> 16); }
    unsigned char small[1000];
    for (int i = 0; i < 1000; i++) small[i] = (unsigned char)(i * 7);
    if (write_file(f_big, big, big_n) || write_file(f_small, small, sizeof(small)) ||
        write_file(f_empty, NULL, 0))
    {
        printf("cannot write test files in /tmp\nFAIL\n");
        return 1;
    }

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
    printf("Test 2 (300 KB BLOB, chunked)        ... ");
    g_fail = 0;
    char blob_key[32] = "";
    {
        const char *b[] = { "999501", "lob write test 2", "big.bin" };
        if (insert_key("INSERT INTO " T_LOB " (ID, DESCRIPTION, FILE_NAME, PHOTO) "
                       "OUTPUT inserted.[ID] VALUES (?, ?, ?, 0x)", 3, b, blob_key, sizeof(blob_key)) != 0)
            bad("insert failed%s", "");
        else
        {
            if (strcmp(blob_key, "999501")) bad("key '%s'", blob_key);
            if (lob_write(1, T_LOB, "PHOTO", blob_key, f_big, NULL, 0, &bytes) != 0)
                bad("lob_write_by_rowid failed%s", "");
            else if (bytes != big_n) bad("bytes written wrong%s", "");
            D->commit(&ctx, &L.dml);
            h_sha256_hex(big, big_n, want);
            server_hash("PHOTO", T_LOB, "ID", blob_key, hash, len);
            if (strcmp(hash, want)) bad("SHA-256 differs: server %s", hash);
            if ((size_t)atol(len) != big_n) bad("DATALENGTH %s", len);
        }
    }
    if (!g_fail) printf("OK (%zu bytes, SHA-256 %.16s...)\n", big_n, want);
    failed |= g_fail;

    /* ---- Test 3 ---- */
    printf("Test 3 (BLOB rewritten smaller)      ... ");
    g_fail = 0;
    if (!blob_key[0]) bad("no row from Test 2%s", "");
    else
    {
        if (lob_write(1, T_LOB, "PHOTO", blob_key, f_small, NULL, 0, &bytes) != 0 || bytes != 1000)
            bad("lob_write_by_rowid failed%s", "");
        D->commit(&ctx, &L.dml);
        h_sha256_hex(small, sizeof(small), want);
        server_hash("PHOTO", T_LOB, "ID", blob_key, hash, len);
        if (strcmp(hash, want) || atol(len) != 1000) bad("value not replaced: DATALENGTH %s", len);
    }
    if (!g_fail) printf("OK (1000 bytes)\n");
    failed |= g_fail;

    /* ---- Test 4 ---- */
    printf("Test 4 (BLOB from an empty file)     ... ");
    g_fail = 0;
    {
        const char *b[] = { "999502", "lob write test 4", "empty.bin" };
        if (insert_key("INSERT INTO " T_LOB " (ID, DESCRIPTION, FILE_NAME, PHOTO) "
                       "OUTPUT inserted.[ID] VALUES (?, ?, ?, 0x)", 3, b, key, sizeof(key)) != 0)
            bad("insert failed%s", "");
        else
        {
            if (lob_write(1, T_LOB, "PHOTO", key, f_empty, NULL, 0, &bytes) != 0 || bytes != 0)
                bad("want rc 0 and 0 bytes%s", "");
            server_hash("PHOTO", T_LOB, "ID", key, hash, len);
            if (strcmp(len, "0")) bad("DATALENGTH %s, want 0", len);
        }
        D->commit(&ctx, &L.dml);
    }
    if (!g_fail) printf("OK (0 bytes, value 0x)\n");
    failed |= g_fail;

    /* ---- Test 5 ---- */
    printf("Test 5 (refusals)                    ... ");
    g_fail = 0;
    {
        if (lob_write(1, T_LOB, "PHOTO", blob_key, "/tmp/no/such/file.bin", NULL, 0, &bytes) == 0)
            bad("missing file: want -1%s", "");
        if (lob_write(1, T_LOB, "PHOTO", "999599", f_small, NULL, 0, &bytes) == 0)
            bad("key no row has: want -1%s", "");
        if (lob_write(1, T_LOB, "PHOTO", "", f_small, NULL, 0, &bytes) == 0)
            bad("blank key (table with no key): want -1%s", "");
        h_exec(&ctx, "CREATE TABLE #DM_NOKEY_LOB (A int, P varbinary(max))", NULL, 0);
        const char *b[] = { "1" };
        exec_sql("INSERT INTO #DM_NOKEY_LOB (A, P) VALUES (?, 0x)", 1, b, NULL, 0);
        if (lob_write(1, "#DM_NOKEY_LOB", "P", "1", f_small, NULL, 0, &bytes) == 0)
            bad("table with no key: want -1%s", "");
        D->rollback(&ctx, &L.dml);
        h_sha256_hex(small, sizeof(small), want);
        server_hash("PHOTO", T_LOB, "ID", blob_key, hash, len);
        if (strcmp(hash, want)) bad("Test 3 row changed: DATALENGTH %s", len);
    }
    if (!g_fail) printf("OK (missing file, unknown key, blank key, no-key table: -1)\n");
    failed |= g_fail;

    /* ---- Test 6 ---- */
    printf("Test 6 (text in 1000-byte chunks)    ... ");
    g_fail = 0;
    {
        size_t tl = 0;
        char *text = make_text(50000, &tl);
        unsigned long saved = config.chunk_read_size;
        config.chunk_read_size = 1000;
        h_sha256_hex(text, tl, want);

        const char *b[] = { "999503", "lob write test 6", "text.txt" };
        if (insert_key("INSERT INTO " T_CLOB " (ID, DESCRIPTION, FILE_NAME) OUTPUT inserted.[ID] "
                       "VALUES (?, ?, ?)", 3, b, key, sizeof(key)) != 0)
            bad("insert (varchar(max)) failed%s", "");
        else
        {
            if (lob_write(0, T_CLOB, "LARGE_CLOB", key, NULL, text, tl, &bytes) != 0 || bytes != tl)
                bad("lob_write_by_rowid (varchar(max)) failed%s", "");
            server_hash("CAST(LARGE_CLOB AS varbinary(max))", T_CLOB, "ID", key, hash, len);
            if (strcmp(hash, want)) bad("varchar(max) SHA-256 differs: DATALENGTH %s", len);
        }

        char fkey[32];
        const char *bf[] = { "999503" };
        if (insert_key("INSERT INTO " T_FIELD " (NUMBER_COL) OUTPUT inserted.[FIELD_TEST_ID] "
                       "VALUES (?)", 1, bf, fkey, sizeof(fkey)) != 0)
            bad("insert (nvarchar(max)) failed%s", "");
        else
        {
            if (lob_write(0, T_FIELD, "NCLOB_COL", fkey, NULL, text, tl, &bytes) != 0 || bytes != tl)
                bad("lob_write_by_rowid (nvarchar(max)) failed%s", "");
            server_hash("CAST(CAST(NCLOB_COL AS varchar(max)) AS varbinary(max))", T_FIELD,
                        "FIELD_TEST_ID", fkey, hash, len);
            if (strcmp(hash, want)) bad("nvarchar(max) SHA-256 differs: DATALENGTH %s", len);
        }
        D->commit(&ctx, &L.dml);
        config.chunk_read_size = saved;
        if (!g_fail) printf("OK (%zu bytes into varchar(max) and nvarchar(max))\n", tl);
        free(text);
    }
    failed |= g_fail;

    /* ---- Test 7 ---- */
    printf("Test 7 (200 KB nvarchar(max), bound) ... ");
    g_fail = 0;
    {
        size_t tl = 0;
        char *text = make_text(200 * 1024, &tl);
        h_sha256_hex(text, tl, want);
        const char *b[] = { "999504", text };
        if (insert_key("INSERT INTO " T_FIELD " (NUMBER_COL, NCLOB_COL) OUTPUT inserted.[FIELD_TEST_ID] "
                       "VALUES (?, ?)", 2, b, key, sizeof(key)) != 0)
            bad("insert failed%s", "");
        else
        {
            D->commit(&ctx, &L.dml);
            server_hash("CAST(CAST(NCLOB_COL AS varchar(max)) AS varbinary(max))", T_FIELD,
                        "FIELD_TEST_ID", key, hash, len);
            if (strcmp(hash, want)) bad("SHA-256 differs: DATALENGTH %s", len);
        }
        if (!g_fail) printf("OK (%zu bytes)\n", tl);
        free(text);
    }
    failed |= g_fail;

    /* ---- Test 8 ---- */
    printf("Test 8 (AUDIT_TRAIL write-once)      ... ");
    g_fail = 0;
    {
        size_t tl = 0;
        char *text = make_text(20000, &tl);
        char err[1024];
        const char *b[] = { "OCI_FIELD_TEST", "999505", "CLOB_COL", "UPDATE", "old value", text,
                            "CLOB", "MSSQL_Driver_LOB_Write_Test", "Stage 4 harness",
                            "TX-STAGE4", "0000000000000000000000000000000000000000000000000000000000000000" };
        if (insert_key("INSERT INTO " T_AUDIT " (TABLE_NAME, RECORD_ID, FIELD_NAME, ACTION_TYPE, "
                       "OLD_VALUE, NEW_VALUE, DATA_TYPE, CHANGED_BY, CHANGE_REASON, TRANSACTION_ID, "
                       "ROW_HASH) OUTPUT inserted.[AUDIT_ID] VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                       11, b, key, sizeof(key)) != 0)
            bad("insert with inline text LOBs failed%s", "");
        else
        {
            server_hash("CAST(NEW_VALUE AS varbinary(max))", T_AUDIT, "AUDIT_ID", key, hash, len);
            h_sha256_hex(text, tl, want);
            if (strcmp(hash, want)) bad("NEW_VALUE SHA-256 differs: DATALENGTH %s", len);
            const char *u[] = { "changed", key };
            if (exec_sql("UPDATE " T_AUDIT " SET NEW_VALUE = ? WHERE AUDIT_ID = ?", 2, u,
                         err, sizeof(err)) == 0 || !strstr(err, "[native 229]"))
                bad("UPDATE: want [native 229], got '%s'", err);
            long log_at = h_log_size(config.update_log_file_name);
            if (lob_write(0, T_AUDIT, "NEW_VALUE", key, NULL, "changed", 7, &bytes) == 0)
                bad("LOB write: want -1%s", "");
            else if (!h_log_has(config.update_log_file_name, log_at, "[native 229]"))
                bad("LOB write refused, but not with [native 229] - see %s",
                    config.update_log_file_name);
            server_hash("CAST(NEW_VALUE AS varbinary(max))", T_AUDIT, "AUDIT_ID", key, hash, len);
            if (strcmp(hash, want)) bad("NEW_VALUE changed: DATALENGTH %s", len);
        }
        D->rollback(&ctx, &L.dml);
        const char *k[] = { key };
        if (key[0] && h_count(&ctx, "SELECT COUNT(*) FROM " T_AUDIT " WHERE AUDIT_ID = ?", 1, k) != 0)
            bad("audit row %s still there after rollback", key);
        if (!g_fail) printf("OK (INSERT inline; UPDATE 229; LOB write refused; rolled back)\n");
        free(text);
    }
    failed |= g_fail;

    cleanup();
    D->disconnect(&ctx);
    unlink(f_big); unlink(f_small); unlink(f_empty);
    free(big);
    printf("%s\n", failed ? "FAIL" : "PASS");
    h_close(&L);
    return failed ? 1 : 0;
}
