/*
 * mssql_dml.c   (driver_mssql Stage 4, 2026-10-20, points U1-U5)
 *
 * The write side of driver_mssql - see mssql_dml.h for what each call
 * does. Every value is bound as text (SQL_C_CHAR -> nvarchar), as in
 * mssql_select.c; the dialect's value_expr() has already wrapped the
 * placeholders that need a conversion (dates, binary).
 *
 * Errors are logged on the caller's logger in the M7 form
 * ("SQLSTATE x [native n]: text"), with the SQL.
 *
 * Fix 1 (2026-10-20, first live run): text longer than nvarchar(4000)
 * is bound as SQL_WVARCHAR with column size SQL_SS_LENGTH_UNLIMITED -
 * nvarchar(max), as Microsoft documents for SQLBindParameter - never as
 * SQL_WLONGVARCHAR. Sent at execution (lob_write_by_rowid), the driver
 * turned SQL_WLONGVARCHAR into the legacy ntext type: SQLPutData failed
 * with 22001 (data longer than the column size, checked in bytes), and
 * the server refuses ntext into a UTF-8 column (4189).
 *
 * Never includes an Oracle header.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <sql.h>
#include <sqlext.h>

#include "mssql_dml.h"
#include "MSSQL_Pool.h"          /* mssql_diag()                         */
#include "logger.h"

#ifndef SQL_SS_LENGTH_UNLIMITED
#define SQL_SS_LENGTH_UNLIMITED 0       /* msodbcsql.h: (max) */
#endif

/* ================================================================== */
/*  Shared helpers                                                      */
/* ================================================================== */

/* "<what> failed: SQLSTATE ..." on the logger, and into out (if any). */
static void dml_fail(logger_t *logger, const char *func, SQLSMALLINT htype, SQLHANDLE h,
                     const char *what, const char *sql, char *out, size_t out_size)
{
    char diag[1536] = "";
    if (h) mssql_diag(htype, h, diag, sizeof(diag));
    logger_write(logger, LOG_ERROR, func, 0, "%s failed: %s - sql=%s", what,
                 diag[0] ? diag : "(no diagnostic record)", sql ? sql : "(none)");
    if (out && out_size)
        snprintf(out, out_size, "%s", diag[0] ? diag : what);
}

static void dml_fail_text(logger_t *logger, const char *func, char *out, size_t out_size,
                          const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    logger_write(logger, LOG_ERROR, func, 0, "%s", msg);
    if (out && out_size) snprintf(out, out_size, "%s", msg);
}

/* UTF-16 code units in a UTF-8 string - an nvarchar parameter's size. */
static SQLULEN utf16_units(const char *s, size_t len)
{
    SQLULEN n = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80) continue;
        n += (c >= 0xF0) ? 2 : 1;
    }
    return n;
}

/* Text in, nvarchar on the wire - nvarchar(n) up to 4000 UTF-16 units,
 * nvarchar(max) beyond (Fix 1). NULL value = SQL NULL. *ind must live
 * until SQLExecute. */
static SQLRETURN bind_text(SQLHSTMT stmt, int pos, const char *v, SQLLEN *ind)
{
    size_t  len     = v ? strlen(v) : 0;
    SQLULEN units   = v ? utf16_units(v, len) : 0;
    SQLULEN colsize = units > 4000 ? SQL_SS_LENGTH_UNLIMITED : (units ? units : 1);
    *ind = v ? (SQLLEN)len : SQL_NULL_DATA;
    return SQLBindParameter(stmt, (SQLUSMALLINT)pos, SQL_PARAM_INPUT, SQL_C_CHAR,
                            SQL_WVARCHAR, colsize, 0, (SQLPOINTER)v, (SQLLEN)len, ind);
}

static void upper_copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

/* [name] with ] doubled. */
static int bracket(char *out, size_t n, const char *name)
{
    size_t u = 0;
    if (n < 3) return -1;
    out[u++] = '[';
    for (const char *p = name; *p; p++)
    {
        if (u + 3 >= n) return -1;
        out[u++] = *p;
        if (*p == ']') out[u++] = ']';
    }
    out[u++] = ']';
    out[u] = '\0';
    return 0;
}

/* ================================================================== */
/*  U1 - table_key, cached per table                                    */
/* ================================================================== */

#define KEY_CACHE_MAX 512

typedef struct {
    char name[260];      /* "OWNER.TABLE" upper case; ".TABLE" if no owner */
    char key[128];
    int  count;          /* 0 or 1 */
} key_entry_t;

static key_entry_t     g_keys[KEY_CACHE_MAX];
static int             g_nkeys;
static pthread_mutex_t g_keys_mu = PTHREAD_MUTEX_INITIALIZER;

void mssql_table_key_cache_clear(void)
{
    pthread_mutex_lock(&g_keys_mu);
    g_nkeys = 0;
    pthread_mutex_unlock(&g_keys_mu);
}

static const char *SQL_KEY =
    "SELECT CAST(i.is_primary_key AS int), COUNT(*), MAX(c.name), "
    "       MAX(CAST(c.is_nullable AS int)) "
    "FROM   sys.indexes i "
    "JOIN   sys.index_columns ic ON ic.object_id = i.object_id "
    "                           AND ic.index_id = i.index_id "
    "                           AND ic.is_included_column = 0 "
    "JOIN   sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
    "WHERE  i.object_id = OBJECT_ID(?) "
    "  AND  (i.is_primary_key = 1 OR (i.is_unique = 1 AND i.has_filter = 0)) "
    "GROUP  BY i.index_id, i.is_primary_key "
    "ORDER  BY i.is_primary_key DESC, i.index_id";

int mssql_table_key(oci_context_t *ctx, const char *owner, const char *table_name,
                    char *key_column, size_t key_column_size, int *key_count)
{
    if (!ctx || !table_name || !table_name[0] || !key_column || key_column_size == 0 ||
        !key_count)
        return -1;
    key_column[0] = '\0';
    *key_count    = 0;
    logger_t *log = ctx->Metadata_logger;

    char cache_name[260], up_owner[128], up_table[128];
    upper_copy(up_owner, sizeof(up_owner), owner ? owner : "");
    upper_copy(up_table, sizeof(up_table), table_name);
    snprintf(cache_name, sizeof(cache_name), "%s.%s", up_owner, up_table);

    pthread_mutex_lock(&g_keys_mu);
    for (int i = 0; i < g_nkeys; i++)
        if (strcmp(g_keys[i].name, cache_name) == 0)
        {
            snprintf(key_column, key_column_size, "%s", g_keys[i].key);
            *key_count = g_keys[i].count;
            pthread_mutex_unlock(&g_keys_mu);
            return 0;
        }
    pthread_mutex_unlock(&g_keys_mu);

    if (!ctx->svchp)
    {
        logger_write(log, LOG_ERROR, __func__, 0, "table_key: no SQL Server connection");
        return -1;
    }

    /* Two-part name for OBJECT_ID(): [OWNER].[TABLE], or [TABLE] (the
     * caller's default schema) when no owner is given. */
    char qo[300], qt[300], objname[620];
    if (bracket(qt, sizeof(qt), up_table) != 0 ||
        (up_owner[0] && bracket(qo, sizeof(qo), up_owner) != 0))
        return -1;
    if (up_owner[0]) snprintf(objname, sizeof(objname), "%s.%s", qo, qt);
    else             snprintf(objname, sizeof(objname), "%s", qt);

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    SQLLEN   ind  = 0;
    int      rc   = -1;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)ctx->svchp, &stmt)))
    {
        dml_fail(log, __func__, SQL_HANDLE_DBC, ctx->svchp, "SQLAllocHandle", NULL, NULL, 0);
        return -1;
    }
    if (!SQL_SUCCEEDED(bind_text(stmt, 1, objname, &ind)) ||
        !SQL_SUCCEEDED(SQLExecDirect(stmt, (SQLCHAR *)SQL_KEY, SQL_NTS)))
    {
        dml_fail(log, __func__, SQL_HANDLE_STMT, stmt, "key lookup", SQL_KEY, NULL, 0);
        goto Done;
    }

    char found[128] = "";
    int  nfound = 0, saw_composite = 0;
    SQLRETURN r;
    while ((r = SQLFetch(stmt)) != SQL_NO_DATA)
    {
        if (!SQL_SUCCEEDED(r))
        {
            dml_fail(log, __func__, SQL_HANDLE_STMT, stmt, "key fetch", SQL_KEY, NULL, 0);
            goto Done;
        }
        SQLINTEGER is_pk = 0, ncols = 0, nullable = 1;
        SQLLEN     i1, i2, i3, i4;
        char       col[129] = "";
        SQLGetData(stmt, 1, SQL_C_SLONG, &is_pk, 0, &i1);
        SQLGetData(stmt, 2, SQL_C_SLONG, &ncols, 0, &i2);
        SQLGetData(stmt, 3, SQL_C_CHAR, col, sizeof(col), &i3);
        SQLGetData(stmt, 4, SQL_C_SLONG, &nullable, 0, &i4);
        if (ncols != 1) { saw_composite = 1; continue; }
        if (nullable)   continue;
        snprintf(found, sizeof(found), "%.127s", col);
        nfound = 1;
        break;
    }
    (void)saw_composite;

    if (!nfound)
        logger_write(log, LOG_WARN, __func__, 0,
                     "%s has no single-column primary or unique key on NOT NULL columns%s - "
                     "INSERT/UPDATE work, LOB writes are refused (5c, 5d)",
                     objname, saw_composite ? " (only a composite key)" : "");
    else
        logger_write(log, LOG_INFO, __func__, 0, "row key for %s: %s", objname, found);

    pthread_mutex_lock(&g_keys_mu);
    if (g_nkeys < KEY_CACHE_MAX)
    {
        key_entry_t *e = &g_keys[g_nkeys++];
        snprintf(e->name, sizeof(e->name), "%s", cache_name);
        snprintf(e->key, sizeof(e->key), "%s", found);
        e->count = nfound;
    }
    pthread_mutex_unlock(&g_keys_mu);

    snprintf(key_column, key_column_size, "%s", found);
    *key_count = nfound;
    rc = 0;

Done:
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return rc;
}

/* ================================================================== */
/*  U3 - dml_execute                                                    */
/* ================================================================== */

int mssql_dml_execute(oci_context_t *ctx, logger_t *logger,
                      const db_dml_request_t *req, int *out_rows_affected,
                      char *out_error_message, size_t out_error_message_size)
{
    if (!ctx || !req || !req->sql || !out_rows_affected) return -1;
    if (req->bind_count < 0 || (req->bind_count > 0 && !req->bind_values)) return -1;
    *out_rows_affected = 0;
    if (!ctx->svchp)
    {
        dml_fail_text(logger, __func__, out_error_message, out_error_message_size,
                      "dml_execute: no SQL Server connection on this session");
        return -1;
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    SQLLEN  *inds = NULL;
    int      rc   = -1;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)ctx->svchp, &stmt)))
    {
        dml_fail(logger, __func__, SQL_HANDLE_DBC, ctx->svchp, "SQLAllocHandle", req->sql,
                 out_error_message, out_error_message_size);
        return -1;
    }
    if (!SQL_SUCCEEDED(SQLPrepare(stmt, (SQLCHAR *)req->sql, SQL_NTS)))
    {
        dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLPrepare", req->sql,
                 out_error_message, out_error_message_size);
        goto Done;
    }
    if (req->bind_count > 0)
    {
        inds = calloc((size_t)req->bind_count, sizeof(SQLLEN));
        if (!inds) goto Done;
        for (int k = 0; k < req->bind_count; k++)
            if (!SQL_SUCCEEDED(bind_text(stmt, k + 1, req->bind_values[k], &inds[k])))
            {
                dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLBindParameter", req->sql,
                         out_error_message, out_error_message_size);
                goto Done;
            }
    }

    SQLRETURN r = SQLExecute(stmt);
    if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA)
    {
        dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLExecute", req->sql,
                 out_error_message, out_error_message_size);
        goto Done;
    }
    SQLLEN n = 0;
    if (r != SQL_NO_DATA && !SQL_SUCCEEDED(SQLRowCount(stmt, &n))) n = 0;
    if (n < 0) n = 0;
    *out_rows_affected = (int)n;
    logger_write(logger, LOG_INFO, __func__, 0, "dml_execute OK rows_affected=%d bind_count=%d",
                 *out_rows_affected, req->bind_count);
    rc = 0;

Done:
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    free(inds);
    return rc;
}

/* ================================================================== */
/*  U4 - dml_execute_returning_rowids                                   */
/* ================================================================== */

typedef struct {
    char *buf;
    int   count;
    int   cap;
} keybuf_t;

static int keybuf_add(keybuf_t *k, const char *v)
{
    if (k->count == k->cap)
    {
        int   ncap = k->cap ? k->cap * 2 : 16;
        char *nb   = realloc(k->buf, (size_t)ncap * MSSQL_ROWID_BUF_SIZE);
        if (!nb) return -1;
        memset(nb + (size_t)k->cap * MSSQL_ROWID_BUF_SIZE, 0,
               (size_t)(ncap - k->cap) * MSSQL_ROWID_BUF_SIZE);
        k->buf = nb;
        k->cap = ncap;
    }
    snprintf(k->buf + (size_t)k->count * MSSQL_ROWID_BUF_SIZE, MSSQL_ROWID_BUF_SIZE, "%s", v);
    k->count++;
    return 0;
}

/* Reads every result of one execution: the OUTPUT rows (keys), or, for a
 * statement without OUTPUT, SQLRowCount blank entries. */
static int collect_keys(SQLHSTMT stmt, logger_t *logger, const char *sql, keybuf_t *keys)
{
    int    saw_output = 0;
    SQLLEN counted    = 0;

    for (;;)
    {
        SQLSMALLINT nc = 0;
        if (!SQL_SUCCEEDED(SQLNumResultCols(stmt, &nc)))
        {
            dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLNumResultCols", sql, NULL, 0);
            return -1;
        }
        if (nc > 0)
        {
            saw_output = 1;
            SQLRETURN fr;
            while ((fr = SQLFetch(stmt)) != SQL_NO_DATA)
            {
                if (!SQL_SUCCEEDED(fr))
                {
                    dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLFetch (OUTPUT)", sql, NULL, 0);
                    return -1;
                }
                char   v[64] = "";
                SQLLEN ind   = 0;
                SQLRETURN gr = SQLGetData(stmt, 1, SQL_C_CHAR, v, sizeof(v), &ind);
                if (!SQL_SUCCEEDED(gr))
                {
                    dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLGetData (OUTPUT)", sql, NULL, 0);
                    return -1;
                }
                if (ind == SQL_NULL_DATA) v[0] = '\0';
                if (ind != SQL_NULL_DATA && (ind == SQL_NO_TOTAL || ind >= MSSQL_ROWID_BUF_SIZE))
                {
                    logger_write(logger, LOG_ERROR, __func__, 0,
                                 "row key value is longer than %d characters - the gateway "
                                 "keeps %d (ROWID_BUF_SIZE); use a shorter key - sql=%s",
                                 MSSQL_ROWID_BUF_SIZE - 1, MSSQL_ROWID_BUF_SIZE - 1, sql);
                    return -1;
                }
                if (keybuf_add(keys, v) != 0) return -1;
            }
        }
        else
        {
            SQLLEN n = 0;
            if (SQL_SUCCEEDED(SQLRowCount(stmt, &n)) && n > 0) counted += n;
        }

        SQLRETURN mr = SQLMoreResults(stmt);
        if (mr == SQL_NO_DATA) break;
        if (!SQL_SUCCEEDED(mr))
        {
            dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLMoreResults", sql, NULL, 0);
            return -1;
        }
    }

    if (!saw_output)
        for (SQLLEN i = 0; i < counted; i++)
            if (keybuf_add(keys, "") != 0) return -1;
    return 0;
}

int mssql_dml_execute_returning_rowids(oci_context_t *ctx, logger_t *logger,
                                       const db_dml_returning_request_t *req,
                                       db_rowid_result_t *out_result)
{
    if (!ctx || !req || !req->sql || !out_result) return -1;
    if (req->bind_count < 0 || (req->bind_count > 0 && !req->bind_values)) return -1;
    memset(out_result, 0, sizeof(*out_result));
    if (!ctx->svchp)
    {
        logger_write(logger, LOG_ERROR, __func__, 0, "no SQL Server connection on this session");
        return -1;
    }

    int      iters = req->row_count > 1 ? req->row_count : 1;
    SQLHSTMT stmt  = SQL_NULL_HSTMT;
    SQLLEN  *inds  = NULL;
    keybuf_t keys  = { 0 };
    int      rc    = -1;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)ctx->svchp, &stmt)))
    {
        dml_fail(logger, __func__, SQL_HANDLE_DBC, ctx->svchp, "SQLAllocHandle", req->sql, NULL, 0);
        return -1;
    }
    if (!SQL_SUCCEEDED(SQLPrepare(stmt, (SQLCHAR *)req->sql, SQL_NTS)))
    {
        dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLPrepare", req->sql, NULL, 0);
        goto Done;
    }
    if (req->bind_count > 0 && !(inds = calloc((size_t)req->bind_count, sizeof(SQLLEN))))
        goto Done;

    for (int it = 0; it < iters; it++)
    {
        for (int k = 0; k < req->bind_count; k++)
        {
            const char *v = req->row_count > 1
                          ? req->bind_values[(size_t)it * req->bind_count + k]
                          : req->bind_values[k];
            if (!SQL_SUCCEEDED(bind_text(stmt, k + 1, v, &inds[k])))
            {
                dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLBindParameter", req->sql, NULL, 0);
                goto Done;
            }
        }

        SQLRETURN r = SQLExecute(stmt);
        if (r == SQL_NO_DATA) continue;            /* nothing matched */
        if (!SQL_SUCCEEDED(r))
        {
            char what[64];
            snprintf(what, sizeof(what), "SQLExecute (row %d of %d)", it + 1, iters);
            dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, what, req->sql, NULL, 0);
            goto Done;
        }
        if (collect_keys(stmt, logger, req->sql, &keys) != 0)
            goto Done;
        SQLFreeStmt(stmt, SQL_CLOSE);
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "dml_execute_returning_rowids OK rows_reported=%d executions=%d",
                 keys.count, iters);
    out_result->rowids = keys.buf;
    out_result->count  = keys.count;
    keys.buf = NULL;
    rc = 0;

Done:
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    free(inds);
    free(keys.buf);
    return rc;
}

void mssql_rowid_result_free(db_rowid_result_t *result)
{
    if (!result) return;
    free(result->rowids);
    result->rowids = NULL;
    result->count  = 0;
}

/* ================================================================== */
/*  U5 - lob_write_by_rowid                                             */
/* ================================================================== */

int mssql_lob_write_by_rowid(oci_context_t *ctx, logger_t *logger,
                             const db_lob_write_request_t *req,
                             uint64_t *out_bytes_written)
{
    if (!ctx || !req || !req->table_fq || !req->column_name || !req->rowid_str ||
        !out_bytes_written)
        return -1;
    *out_bytes_written = 0;

    /* An empty value is a no-op success, as on Oracle. */
    if (req->is_blob && !req->file_path) return 0;
    if (!req->is_blob && (!req->inline_text || req->inline_text_len == 0)) return 0;

    /* The key column for this table (U1). */
    char owner[128] = "", table[128] = "";
    const char *dot = strrchr(req->table_fq, '.');
    if (dot)
    {
        snprintf(owner, sizeof(owner), "%.*s", (int)(dot - req->table_fq), req->table_fq);
        snprintf(table, sizeof(table), "%s", dot + 1);
    }
    else
        snprintf(table, sizeof(table), "%s", req->table_fq);

    char key[128] = "";
    int  key_count = 0;
    if (mssql_table_key(ctx, owner, table, key, sizeof(key), &key_count) != 0)
        return -1;
    if (key_count != 1 || !req->rowid_str[0])
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "LOB write needs a primary or unique key on %s", req->table_fq);
        return -1;
    }

    char qkey[300], sql[1024];
    if (bracket(qkey, sizeof(qkey), key) != 0) return -1;
    snprintf(sql, sizeof(sql), "UPDATE %s SET %s = ? WHERE %s = ?",
             req->table_fq, req->column_name, qkey);

    size_t chunk = ctx->ini && ctx->ini->chunk_read_size ? (size_t)ctx->ini->chunk_read_size : 65536;
    FILE  *fp    = NULL;
    size_t total = 0;
    unsigned char *buf = NULL;

    if (req->is_blob)
    {
        fp = fopen(req->file_path, "rb");
        if (!fp)
        {
            logger_write(logger, LOG_ERROR, __func__, 0, "Failed to open BLOB file: %s",
                         req->file_path);
            return -1;
        }
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (fsize <= 0) { fclose(fp); return 0; }   /* as Oracle: 0x stays */
        total = (size_t)fsize;
        buf = malloc(chunk);
        if (!buf) { fclose(fp); return -1; }
    }
    else
        total = req->inline_text_len;

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    SQLLEN   ind_val = SQL_LEN_DATA_AT_EXEC((SQLLEN)total), ind_key = 0;
    int      rc = -1;
    size_t   sent = 0;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)ctx->svchp, &stmt)))
    {
        dml_fail(logger, __func__, SQL_HANDLE_DBC, ctx->svchp, "SQLAllocHandle", sql, NULL, 0);
        goto Done;
    }
    SQLRETURN r;
    if (req->is_blob)
        r = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_LONGVARBINARY,
                             (SQLULEN)total, 0, (SQLPOINTER)1, 0, &ind_val);
    else
        r = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                             SQL_SS_LENGTH_UNLIMITED, 0, (SQLPOINTER)1, 0,   /* Fix 1 */
                             &ind_val);
    if (!SQL_SUCCEEDED(r) || !SQL_SUCCEEDED(bind_text(stmt, 2, req->rowid_str, &ind_key)))
    {
        dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLBindParameter", sql, NULL, 0);
        goto Done;
    }

    r = SQLExecDirect(stmt, (SQLCHAR *)sql, SQL_NTS);
    if (r != SQL_NEED_DATA)
    {
        dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLExecDirect (expected NEED_DATA)",
                 sql, NULL, 0);
        goto Done;
    }

    SQLPOINTER token = NULL;
    while ((r = SQLParamData(stmt, &token)) == SQL_NEED_DATA)
    {
        if (token != (SQLPOINTER)1) continue;     /* only parameter 1 is at-exec */
        while (sent < total)
        {
            size_t n = total - sent < chunk ? total - sent : chunk;
            const void *piece;
            if (req->is_blob)
            {
                size_t got = fread(buf, 1, n, fp);
                if (got == 0)
                {
                    logger_write(logger, LOG_ERROR, __func__, 0,
                                 "short read on %s after %zu of %zu bytes",
                                 req->file_path, sent, total);
                    SQLCancel(stmt);
                    goto Done;
                }
                n = got;
                piece = buf;
            }
            else
            {
                /* never end a chunk inside a UTF-8 character */
                if (sent + n < total)
                {
                    size_t e = sent + n;
                    while (e > sent && ((unsigned char)req->inline_text[e] & 0xC0) == 0x80) e--;
                    if (e > sent) n = e - sent;
                }
                piece = req->inline_text + sent;
            }
            if (!SQL_SUCCEEDED(SQLPutData(stmt, (SQLPOINTER)piece, (SQLLEN)n)))
            {
                dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLPutData", sql, NULL, 0);
                SQLCancel(stmt);
                goto Done;
            }
            sent += n;
        }
    }
    if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA)
    {
        dml_fail(logger, __func__, SQL_HANDLE_STMT, stmt, "SQLParamData", sql, NULL, 0);
        goto Done;
    }

    SQLLEN rows = 0;
    if (r == SQL_NO_DATA || !SQL_SUCCEEDED(SQLRowCount(stmt, &rows)) || rows < 1)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "LOB write: no row of %s has %s = '%s'", req->table_fq, key, req->rowid_str);
        goto Done;
    }

    *out_bytes_written = (uint64_t)sent;
    logger_write(logger, LOG_INFO, __func__, 0, "%s write complete %s.%s key=%s bytes=%zu",
                 req->is_blob ? "BLOB" : "text", req->table_fq, req->column_name,
                 req->rowid_str, sent);
    rc = 0;

Done:
    if (stmt) SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (fp) fclose(fp);
    free(buf);
    return rc;
}
