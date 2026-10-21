/*
 * MSSQL_Driver_LOB_Select_Test.c   (driver_mssql Stage 5, points W1-W5, W7)
 *
 * Checks driver_mssql's SELECT of long columns (mssql_select.c) against
 * a real SQL Server on one standalone connection. Files the driver
 * writes are compared by SHA-256 with HASHBYTES('SHA2_256', ...) on the
 * server, and removed at the end.
 *
 * Usage:  MSSQL_Driver_LOB_Select_Test [config_mssql.ini]
 *
 * Needs the six seed photos in OCI_LOB_TEST (ID 1-6,
 * Create_MSSQL_Test_Objects.sql) and writable BLOB_output_dir /
 * CLOB_output_dir from the ini. Reserved rows: OCI_LOB_TEST.ID and
 * OCI_CLOB_TEST.ID 999601-999609, OCI_FIELD_TEST.NUMBER_COL
 * 999601-999609 - removed at start and end.
 *
 *   Test 1   standalone connect; test rows written (Stage 4 DML)
 *   Test 2   the six seed photos, batches of 4: rows 4 + 2, field type
 *            BLOB, every file's size and SHA-256 match the server,
 *            file name from FILE_NAME (BLOB_default_file_name_col),
 *            blob counts
 *   Test 3   BLOB in the middle of the select list; NULL and empty
 *            values give "" and no file, and are still counted
 *   Test 4   CLOBs to files in batches of 2 (2 + 2 + 1) with
 *            chunk_read_size 1000: 1- to 4-byte characters across every
 *            chunk boundary, 50 KB; files match by SHA-256; NULL and
 *            empty give ""; clob counts and UTF-8 bytes
 *   Test 5   text_lobs_inline: the text in the field; NULL gives "";
 *            a value longer than the field fails the fetch, not cut
 *   Test 6   nvarchar(max) to a file (50 KB multibyte); xml inline; and
 *            non-ASCII text in the statement itself (not bound) - in a
 *            SELECT and in an UPDATE - arrives intact (Stage 5 Fix 1)
 *   Test 7   field types from select_open: BLOB / CLOB / scalars
 *   Test 8   a BLOB file that cannot be written fails the fetch (W4)
 *   Test 9   100 open/fetch/close cycles with long columns, partly read
 *            and unread - no leaks (ASan); connection still answers
 *
 * Prints PASS or FAIL as the last line; exit code 0 / 1.
 */

#define _POSIX_C_SOURCE 200809L

#include <strings.h>
#include <unistd.h>

#include "mssql_harness_util.h"

#define T_LOB   "DATA_MANAGER.OCI_LOB_TEST"
#define T_CLOB  "DATA_MANAGER.OCI_CLOB_TEST"
#define T_FIELD "DATA_MANAGER.OCI_FIELD_TEST"

static const db_driver_t *D;
static oci_context_t     *C;
static app_config_t      *CFG;
static h_loggers_t        L;
static int                g_fail;

/* files the driver wrote, removed at the end */
static char g_files[256][1024];
static int  g_nfiles;

static void bad(const char *fmt, const char *a)
{
    if (!g_fail) printf("FAILED\n");
    printf("    ");
    printf(fmt, a);
    printf("\n");
    g_fail = 1;
}

static int exec_sql(const char *sql, int nb, const char **b)
{
    db_dml_request_t r = { sql, nb, b };
    int  n = 0;
    char err[1024];
    int  rc = D->dml_execute(C, &L.dml, &r, &n, err, sizeof(err));
    if (rc != 0) fprintf(stderr, "    exec failed: %s\n      %s\n", sql, err);
    return rc;
}

/* ---- one whole SELECT through the driver ---- */
typedef struct {
    int                rc;            /* 0, or -1 (open or fetch failed) */
    char               error[2048];
    int                ncols;
    db_column_meta_t  *cols;
    int                nrows;
    resultset_field_t *f;             /* nrows * ncols                    */
    int                batches[64];
    int                nbatches;
    db_fetch_batch_stats_t st;        /* summed                           */
} sel_t;

static void sel_free(sel_t *r)
{
    free(r->cols);
    free(r->f);
    memset(r, 0, sizeof(*r));
}

static void sel(sel_t *r, const char *sql, int nb, const char **b, int fetch, int inline_text)
{
    memset(r, 0, sizeof(*r));
    db_select_request_t req;
    memset(&req, 0, sizeof(req));
    req.sql = sql;
    req.fetch_array_size = fetch;
    req.bind_count = nb;
    req.bind_values = b;
    req.text_lobs_inline = inline_text;
    db_select_cursor_t *cur = NULL;
    int bs = 0;
    if (D->select_open(C, &req, &cur, &r->cols, &r->ncols, &bs) != 0)
    {
        r->rc = -1;
        snprintf(r->error, sizeof(r->error), "%s", mssql_select_last_error());
        return;
    }
    for (;;)
    {
        resultset_t *rs = NULL;
        int rows = 0;
        db_fetch_batch_stats_t st;
        if (D->select_fetch_batch(cur, &rs, &rows, &st) != 0)
        {
            r->rc = -1;
            snprintf(r->error, sizeof(r->error), "%s", mssql_select_last_error());
            break;
        }
        if (rows == 0) break;
        if (r->nbatches < 64) r->batches[r->nbatches++] = rows;
        r->st.blob_count += st.blob_count;  r->st.blob_bytes += st.blob_bytes;
        r->st.clob_count += st.clob_count;  r->st.clob_bytes += st.clob_bytes;
        r->f = realloc(r->f, sizeof(resultset_field_t) * (size_t)(r->nrows + rows) * (size_t)r->ncols);
        for (int i = 0; i < rows; i++)
            for (int c = 0; c < r->ncols; c++)
                r->f[(size_t)(r->nrows + i) * r->ncols + c] = rs->records[i].fields[c];
        r->nrows += rows;
        resultset_free(rs);
    }
    D->select_close(cur);
}

static resultset_field_t *fld(sel_t *r, int row, int col) { return &r->f[(size_t)row * r->ncols + col]; }

/* SHA-256 of a file (and its size) - -1 if it cannot be read. A file
 * whose name carries the SELECT's tag (every CLOB file; BLOB files when
 * BLOB_append_file_timestamp = 1) is removed at the end; one named
 * plainly from FILE_NAME may be the gateway's own and is left. */
static long file_sha(const char *path, char hex[65], int remove_after)
{
    hex[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    h_sha256_hex(b, got, hex);
    free(b);
    if (remove_after && g_nfiles < 256) snprintf(g_files[g_nfiles++], sizeof(g_files[0]), "%s", path);
    return (long)got;
}

static void server_sha(const char *expr, const char *table, const char *keycol, const char *key,
                       char hex[65], long *len)
{
    char sql[512], v[2][256];
    const char *k[] = { key };
    snprintf(sql, sizeof(sql),
             "SELECT CONVERT(varchar(64), HASHBYTES('SHA2_256', %s), 2), DATALENGTH(%s) "
             "FROM %s WHERE %s = ?", expr, expr, table, keycol);
    hex[0] = '\0';
    *len = -1;
    if (h_query(C, sql, 1, k, v, 2) == 1)
    {
        snprintf(hex, 65, "%.64s", v[0]);
        *len = v[1][0] ? atol(v[1]) : -1;
    }
}

/* 1- to 4-byte characters in a 23-byte cycle (see LOB_Write Test 6). */
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

static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

/* file_name starts with FILE_NAME's stem (before its extension) */
static int from_file_name(const char *file_name, const char *name_col)
{
    const char *dot = strrchr(name_col, '.');
    size_t stem = dot ? (size_t)(dot - name_col) : strlen(name_col);
    return stem > 0 && strncmp(file_name, name_col, stem) == 0;
}

static void cleanup_rows(void)
{
    exec_sql("DELETE FROM " T_LOB   " WHERE ID BETWEEN 999601 AND 999609", 0, NULL);
    exec_sql("DELETE FROM " T_CLOB  " WHERE ID BETWEEN 999601 AND 999609", 0, NULL);
    exec_sql("DELETE FROM " T_FIELD " WHERE NUMBER_COL BETWEEN 999601 AND 999609", 0, NULL);
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
    CFG = &config;
    int failed = 0;
    char want[65], got[65], path[1100];
    long n;
    sel_t R;

    /* test values */
    size_t mb_len = 0, long_len = 0;
    char *mb = make_text(50000, &mb_len);
    char  short_text[] = "Hello CLOB \xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
    char *long_ascii = malloc(5001);
    for (int i = 0; i < 5000; i++) long_ascii[i] = (char)('A' + i % 26);
    long_ascii[5000] = '\0';
    long_len = 5000;
    size_t blob_n = 40000;
    unsigned char *blob = malloc(blob_n);
    char *blob_hex = malloc(blob_n * 2 + 1);
    unsigned int seed = 4242;
    for (size_t i = 0; i < blob_n; i++)
    {
        seed = seed * 1103515245u + 12345u;
        blob[i] = (unsigned char)(seed >> 16);
        sprintf(blob_hex + 2 * i, "%02X", blob[i]);
    }

    /* ---- Test 1 ---- */
    printf("Test 1 (connect, test rows written)  ... ");
    g_fail = 0;
    if (D->connect(&ctx) != 0 || !ctx.svchp)
    {
        printf("FAILED - connect(); see %s\nFAIL\n", config.connection_log_file_name);
        return 1;
    }
    cleanup_rows();
    {
        const char *c1[] = { "999601", "short", short_text };
        const char *c2[] = { "999602", "null", NULL };
        const char *c3[] = { "999603", "empty", "" };
        const char *c4[] = { "999604", "5000 bytes", long_ascii };
        const char *c5[] = { "999605", "50 KB multibyte", mb };
        const char *ins = "INSERT INTO " T_CLOB " (ID, DESCRIPTION, LARGE_CLOB) VALUES (?, ?, ?)";
        const char *b1[] = { "999601", "lob select", "test_blob.bin", blob_hex };
        const char *b2[] = { "999602", "null photo", "null.bin" };
        const char *b3[] = { "999603", "empty photo", "empty.bin" };
        const char *f1[] = { "999601", mb, "clob 999601" };
        if (exec_sql(ins, 3, c1) || exec_sql(ins, 3, c2) || exec_sql(ins, 3, c3) ||
            exec_sql(ins, 3, c4) || exec_sql(ins, 3, c5) ||
            exec_sql("INSERT INTO " T_LOB " (ID, DESCRIPTION, FILE_NAME, PHOTO) "
                     "VALUES (?, ?, ?, CONVERT(varbinary(max), ?, 2))", 4, b1) ||
            exec_sql("INSERT INTO " T_LOB " (ID, DESCRIPTION, FILE_NAME, PHOTO) VALUES (?, ?, ?, NULL)", 3, b2) ||
            exec_sql("INSERT INTO " T_LOB " (ID, DESCRIPTION, FILE_NAME, PHOTO) VALUES (?, ?, ?, 0x)", 3, b3) ||
            exec_sql("INSERT INTO " T_FIELD " (NUMBER_COL, NCLOB_COL, CLOB_COL) VALUES (?, ?, ?)", 3, f1) ||
            D->commit(&ctx, &L.dml) != 0)
            bad("could not write the test rows%s", "");
    }
    if (!g_fail) printf("OK\n");
    failed |= g_fail;

    /* ---- Test 2 ---- */
    printf("Test 2 (seed photos, batches of 4)   ... ");
    g_fail = 0;
    {
        long seeds = h_count(&ctx, "SELECT COUNT(*) FROM " T_LOB " WHERE ID BETWEEN 1 AND 6", 0, NULL);
        sel(&R, "SELECT ID, DESCRIPTION, FILE_NAME, PHOTO FROM " T_LOB
                " WHERE ID BETWEEN 1 AND 6 ORDER BY ID", 0, NULL, 4, 0);
        uint64_t total = 0;
        if (seeds != 6) bad("want the 6 seed photos (ID 1-6) in OCI_LOB_TEST, found %s",
                            seeds < 0 ? "none" : "a different number");
        else if (R.rc != 0) bad("select failed: %s", R.error);
        else
        {
            if (R.nrows != 6 || R.nbatches != 2 || R.batches[0] != 4 || R.batches[1] != 2)
                bad("want batches 4 + 2%s", "");
            for (int i = 0; i < R.nrows; i++)
            {
                resultset_field_t *p = fld(&R, i, 3), *fn = fld(&R, i, 2), *id = fld(&R, i, 0);
                if (!p->is_blob || strcmp(p->field_type, "BLOB")) { bad("ID %s: not a BLOB field", id->value); continue; }
                if (!from_file_name(p->blob_detail.file_name, fn->value))
                    bad("file name '%s' not built from FILE_NAME", p->blob_detail.file_name);
                snprintf(path, sizeof(path), "%s/%s", config.BLOB_output_dir, p->blob_detail.file_name);
                long fsz = file_sha(path, got, config.BLOB_append_file_timestamp == 1);
                server_sha("PHOTO", T_LOB, "ID", id->value, want, &n);
                if (fsz != n || (uint64_t)n != p->blob_detail.file_size || strcmp(got, want))
                    bad("ID %s: file differs from the server (size/SHA-256)", id->value);
                size_t fl = strlen(p->blob_detail.file_name);
                if (fl > 4 && !strcasecmp(p->blob_detail.file_name + fl - 4, ".jpg") &&
                    strcmp(p->blob_detail.mime_type, "image/jpeg"))
                    bad("MIME type '%s', want image/jpeg", p->blob_detail.mime_type);
                total += p->blob_detail.file_size;
            }
            if (R.st.blob_count != 6 || R.st.blob_bytes != total || R.st.clob_count != 0)
                bad("counts: want 6 BLOBs%s", "");
        }
        if (!g_fail) printf("OK (6 files, %llu bytes, SHA-256 match)\n", (unsigned long long)total);
        sel_free(&R);
    }
    failed |= g_fail;

    /* ---- Test 3 ---- */
    printf("Test 3 (BLOB mid-list, NULL, empty)  ... ");
    g_fail = 0;
    {
        sel(&R, "SELECT ID, PHOTO, FILE_NAME FROM " T_LOB " WHERE ID BETWEEN 999601 AND 999603 ORDER BY ID",
            0, NULL, 10, 0);
        if (R.rc != 0 || R.nrows != 3) bad("select failed: %s", R.error);
        else
        {
            resultset_field_t *p = fld(&R, 0, 1);
            snprintf(path, sizeof(path), "%s/%s", config.BLOB_output_dir, p->blob_detail.file_name);
            h_sha256_hex(blob, blob_n, want);
            if (!p->is_blob || file_sha(path, got, config.BLOB_append_file_timestamp == 1) != (long)blob_n ||
                strcmp(got, want))
                bad("999601: file differs (%s)", path);
            if (!strstr(p->blob_detail.file_name, "test_blob"))
                bad("999601: file name '%s' not from FILE_NAME (a later column)", p->blob_detail.file_name);
            for (int i = 1; i < 3; i++)
            {
                resultset_field_t *q = fld(&R, i, 1);
                if (q->is_blob || strcmp(q->field_type, "BLOB") || q->value[0])
                    bad("NULL/empty BLOB: want \"\" and type BLOB (row %s)", fld(&R, i, 0)->value);
            }
            if (strcmp(fld(&R, 2, 2)->value, "empty.bin")) bad("scalar after the BLOB wrong%s", "");
            if (R.st.blob_count != 3 || R.st.blob_bytes != blob_n) bad("counts: want 3 BLOBs, 40000 bytes%s", "");
        }
        sel_free(&R);
    }
    if (!g_fail) printf("OK (file from a later FILE_NAME; NULL and empty: \"\")\n");
    failed |= g_fail;

    /* ---- Test 4 ---- */
    printf("Test 4 (CLOBs to files, chunk 1000)  ... ");
    g_fail = 0;
    {
        unsigned long saved = config.chunk_read_size;
        config.chunk_read_size = 1000;
        sel(&R, "SELECT ID, LARGE_CLOB, DESCRIPTION FROM " T_CLOB
                " WHERE ID BETWEEN 999601 AND 999605 ORDER BY ID", 0, NULL, 2, 0);
        config.chunk_read_size = saved;
        const char *texts[5] = { short_text, NULL, "", long_ascii, mb };
        size_t lens[5] = { strlen(short_text), 0, 0, long_len, mb_len };
        uint64_t bytes = 0;
        if (R.rc != 0 || R.nrows != 5) bad("select failed: %s", R.error);
        else
        {
            if (R.nbatches != 3 || R.batches[0] != 2 || R.batches[1] != 2 || R.batches[2] != 1)
                bad("want batches 2 + 2 + 1%s", "");
            for (int i = 0; i < 5; i++)
            {
                resultset_field_t *c = fld(&R, i, 1);
                if (strcmp(c->field_type, "CLOB")) bad("type '%s', want CLOB", c->field_type);
                if (lens[i] == 0)
                {
                    if (c->value[0]) bad("NULL/empty CLOB: want \"\", got '%s'", c->value);
                    continue;
                }
                if (!strstr(c->value, "_row") || !strstr(c->value, "_clob"))
                    bad("field '%s' is not a CLOB file reference", c->value);
                snprintf(path, sizeof(path), "%s/%s", config.CLOB_output_dir, base_name(c->value));
                h_sha256_hex(texts[i], lens[i], want);
                if (file_sha(path, got, 1) != (long)lens[i] || strcmp(got, want))
                    bad("file %s differs from the text written", path);
                char sw[65];
                server_sha("CAST(LARGE_CLOB AS varbinary(max))", T_CLOB, "ID", fld(&R, i, 0)->value, sw, &n);
                if (strcmp(sw, want)) bad("server value of ID %s differs from the text written", fld(&R, i, 0)->value);
                bytes += lens[i];
            }
            if (R.st.clob_count != 5 || R.st.clob_bytes != bytes || R.st.blob_count != 0)
                bad("counts: want 5 CLOBs and the UTF-8 bytes%s", "");
        }
        if (!g_fail) printf("OK (3 files, %llu UTF-8 bytes; NULL and empty: \"\")\n", (unsigned long long)bytes);
        sel_free(&R);
    }
    failed |= g_fail;

    /* ---- Test 5 ---- */
    printf("Test 5 (text_lobs_inline)            ... ");
    g_fail = 0;
    {
        const char *k1[] = { "999601" }, *k2[] = { "999602" }, *k4[] = { "999604" };
        const char *q = "SELECT LARGE_CLOB FROM " T_CLOB " WHERE ID = ?";
        sel(&R, q, 1, k1, 1, 1);
        if (R.rc != 0 || R.nrows != 1 || strcmp(fld(&R, 0, 0)->value, short_text))
            bad("short text: got '%s'", R.nrows ? fld(&R, 0, 0)->value : R.error);
        else if (R.st.clob_count != 1 || R.st.clob_bytes != strlen(short_text))
            bad("short text: counts wrong%s", "");
        sel_free(&R);
        sel(&R, q, 1, k2, 1, 1);
        if (R.rc != 0 || R.nrows != 1 || fld(&R, 0, 0)->value[0]) bad("NULL: want \"\"%s", "");
        sel_free(&R);
        sel(&R, q, 1, k4, 1, 1);
        if (R.rc == 0) bad("5000 bytes: want the fetch to fail%s", "");
        else if (!strstr(R.error, "does not fit")) bad("5000 bytes: error '%s'", R.error);
        sel_free(&R);
        if (h_count(&ctx, "SELECT 1 AS X", 0, NULL) != 1) bad("connection unusable afterwards%s", "");
    }
    if (!g_fail) printf("OK (inline; NULL \"\"; 5000 bytes refused)\n");
    failed |= g_fail;

    /* ---- Test 6 ---- */
    printf("Test 6 (nvarchar(max) file, xml)     ... ");
    g_fail = 0;
    {
        unsigned long saved = config.chunk_read_size;
        config.chunk_read_size = 1000;
        sel(&R, "SELECT NUMBER_COL, NCLOB_COL, CLOB_COL FROM " T_FIELD " WHERE NUMBER_COL = 999601",
            0, NULL, 5, 0);
        config.chunk_read_size = saved;
        if (R.rc != 0 || R.nrows != 1) bad("select failed: %s", R.error);
        else
        {
            snprintf(path, sizeof(path), "%s/%s", config.CLOB_output_dir, base_name(fld(&R, 0, 1)->value));
            h_sha256_hex(mb, mb_len, want);
            if (file_sha(path, got, 1) != (long)mb_len || strcmp(got, want))
                bad("nvarchar(max) file %s differs", path);
            snprintf(path, sizeof(path), "%s/%s", config.CLOB_output_dir, base_name(fld(&R, 0, 2)->value));
            h_sha256_hex("clob 999601", 11, want);
            if (file_sha(path, got, 1) != 11 || strcmp(got, want)) bad("varchar(max) file %s differs", path);
        }
        sel_free(&R);
        sel(&R, "SELECT CAST(N'<a>\xc3\xa9\xf0\x9f\x98\x80</a>' AS xml) AS X", 0, NULL, 1, 1);
        if (R.rc != 0 || R.nrows != 1 || strcmp(fld(&R, 0, 0)->value, "<a>\xc3\xa9\xf0\x9f\x98\x80</a>"))
            bad("xml: got '%s'", R.nrows ? fld(&R, 0, 0)->value : R.error);
        sel_free(&R);

        /* Fix 1: a literal in the statement text, written by DML and
         * compared with a bound value on the server. */
        const char *lit = "lit \xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
        const char *kb[] = { lit, "999601" };
        if (exec_sql("UPDATE " T_CLOB " SET DESCRIPTION = N'lit \xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80' "
                     "WHERE ID = 999601", 0, NULL) != 0 || D->commit(&ctx, &L.dml) != 0)
            bad("UPDATE with a non-ASCII literal failed%s", "");
        else if (h_count(&ctx, "SELECT COUNT(*) FROM " T_CLOB " WHERE DESCRIPTION = ? AND ID = ?", 2, kb) != 1)
            bad("UPDATE literal arrived garbled (bound comparison found no row)%s", "");
    }
    if (!g_fail) printf("OK (%zu-byte nvarchar(max) file; xml inline)\n", mb_len);
    failed |= g_fail;

    /* ---- Test 7 ---- */
    printf("Test 7 (field types)                 ... ");
    g_fail = 0;
    {
        sel(&R, "SELECT ID, PHOTO, FILE_NAME FROM " T_LOB " WHERE ID = 0", 0, NULL, 1, 0);
        if (R.rc != 0 || R.ncols != 3 || strcmp(R.cols[1].field_type, "BLOB") ||
            strcmp(R.cols[0].field_type, "NUMBER") || strcmp(R.cols[2].field_type, "STRING"))
            bad("OCI_LOB_TEST: want NUMBER, BLOB, STRING%s", "");
        sel_free(&R);
        sel(&R, "SELECT LARGE_CLOB, CAST(N'x' AS nvarchar(max)) AS N, "
                "CAST('<x/>' AS xml) AS X FROM " T_CLOB " WHERE ID = 0", 0, NULL, 1, 0);
        if (R.rc != 0 || R.ncols != 3)
            bad("select failed: %s", R.error);
        else
            for (int c = 0; c < 3; c++)
                if (strcmp(R.cols[c].field_type, "CLOB")) bad("want CLOB, got %s", R.cols[c].field_type);
        sel_free(&R);
    }
    if (!g_fail) printf("OK (varbinary(max) BLOB; varchar(max), nvarchar(max), xml CLOB)\n");
    failed |= g_fail;

    /* ---- Test 8 ---- */
    printf("Test 8 (BLOB file not writable)      ... ");
    g_fail = 0;
    {
        char saved[256];
        snprintf(saved, sizeof(saved), "%s", config.BLOB_output_dir);
        snprintf(config.BLOB_output_dir, sizeof(config.BLOB_output_dir), "/nonexistent/mssql_lob_test");
        sel(&R, "SELECT ID, FILE_NAME, PHOTO FROM " T_LOB " WHERE ID = 999601", 0, NULL, 1, 0);
        snprintf(config.BLOB_output_dir, sizeof(config.BLOB_output_dir), "%s", saved);
        if (R.rc == 0) bad("want the fetch to fail%s", "");
        else if (!strstr(R.error, "could not write")) bad("error '%s'", R.error);
        sel_free(&R);
        if (h_count(&ctx, "SELECT 1 AS X", 0, NULL) != 1) bad("connection unusable afterwards%s", "");
    }
    if (!g_fail) printf("OK (fetch failed, connection still answers)\n");
    failed |= g_fail;

    /* ---- Test 9 ---- */
    printf("Test 9 (100 open/fetch/close)        ... ");
    g_fail = 0;
    {
        for (int i = 0; i < 100 && !g_fail; i++)
        {
            db_select_request_t req;
            memset(&req, 0, sizeof(req));
            req.sql = "SELECT ID, LARGE_CLOB FROM " T_CLOB " WHERE ID BETWEEN 999601 AND 999605 ORDER BY ID";
            req.fetch_array_size = 2;
            req.text_lobs_inline = 1;
            db_select_cursor_t *cur = NULL;
            db_column_meta_t *cols = NULL;
            int nc = 0, bs = 0;
            if (D->select_open(&ctx, &req, &cur, &cols, &nc, &bs) != 0) { bad("open %s failed", "cycle"); break; }
            if (i % 3 != 2)                      /* read one batch (2 short rows) or none */
            {
                resultset_t *rs = NULL;
                int rows = 0;
                if (i % 3 == 0 && D->select_fetch_batch(cur, &rs, &rows, NULL) != 0) bad("fetch failed%s", "");
                resultset_free(rs);
            }
            D->select_close(cur);
            free(cols);
        }
        if (h_count(&ctx, "SELECT 1 AS X", 0, NULL) != 1) bad("connection unusable afterwards%s", "");
    }
    if (!g_fail) printf("OK (100 cycles; SELECT 1 still answers)\n");
    failed |= g_fail;

    cleanup_rows();
    for (int i = 0; i < g_nfiles; i++) unlink(g_files[i]);
    D->disconnect(&ctx);
    free(mb); free(long_ascii); free(blob); free(blob_hex);
    printf("%s\n", failed ? "FAIL" : "PASS");
    h_close(&L);
    return failed ? 1 : 0;
}
