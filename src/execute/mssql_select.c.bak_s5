/*
 * mssql_select.c   (driver_mssql Stage 3, 2026-10-19, points T3-T5)
 *
 * Scalar SELECT through ODBC - see mssql_select.h for what each call
 * does and what is not done yet.
 *
 * Buffers. Every column is fetched as text (SQL_C_CHAR - UTF-8, which
 * is what the Microsoft driver uses for narrow text when the process
 * has not called setlocale(); the gateway does not), into a
 * column-wise array of batch-size rows. A column's buffer holds its
 * longest possible value:
 *   char / varchar / nchar / nvarchar   4 bytes per character + 1
 *   binary / varbinary                  2 hex digits per byte + 1
 *   everything else                     64 bytes (numbers, dates, guids)
 * capped at MSSQL_FIELD_BUF_MAX. A value that still does not fit fails
 * the fetch with a logged error - it is never cut short. (The resultset
 * field itself holds 4095 bytes, as for Oracle.)
 *
 * The cursor type. db_select_cursor_t is opaque to core; driver_oracle.c
 * defines struct db_select_cursor_t for its own cursors. This file keeps
 * its own struct (mssql_cursor_t) and casts at the boundary, so the two
 * drivers never give the same struct tag two different layouts.
 *
 * Never includes an Oracle header.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <sql.h>
#include <sqlext.h>

#include "mssql_select.h"
#include "MSSQL_Pool.h"          /* mssql_diag()                         */
#include "Resultset_Builder.h"
#include "logger.h"

/* SQL Server specific type codes (msodbcsql.h - not included, so the
 * build needs nothing beyond unixODBC's headers). */
#ifndef SQL_SS_VARIANT
#define SQL_SS_VARIANT          (-150)
#endif
#ifndef SQL_SS_UDT
#define SQL_SS_UDT              (-151)
#endif
#ifndef SQL_SS_XML
#define SQL_SS_XML              (-152)
#endif
#ifndef SQL_SS_TIME2
#define SQL_SS_TIME2            (-154)
#endif
#ifndef SQL_SS_TIMESTAMPOFFSET
#define SQL_SS_TIMESTAMPOFFSET  (-155)
#endif

#define MSSQL_FIELD_BUF_MAX   16004   /* T3 */
#define MSSQL_FIELD_BUF_MIN   64
#define MSSQL_MAX_RESULT_SKIP 16      /* leading row-count results skipped
                                         before the first result set     */

typedef struct {
    oci_context_t  *ctx;            /* borrowed                          */
    SQLHSTMT        stmt;
    int             ncols;
    int             batch;          /* rows per SQLFetch                 */
    char          (*names)[128];
    const char    **field_types;    /* static strings                    */
    SQLLEN         *buf_size;       /* [col]                             */
    char          **bufs;           /* [col] -> batch * buf_size bytes   */
    SQLLEN        **inds;           /* [col] -> batch indicators         */
    SQLULEN         rows_fetched;
    SQLUSMALLINT   *row_status;     /* [batch]                           */
    SQLLEN         *bind_inds;      /* [bind_count]                      */
    int             exhausted;
} mssql_cursor_t;

static __thread char g_last_error[2048];

const char *mssql_select_last_error(void)
{
    return g_last_error;
}

/* Records and logs a failure: "<what>: <text>" (+ the SQL in the log). */
static void ms_fail(oci_context_t *ctx, const char *func, const char *sql,
                    const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
    if (ctx && ctx->select_logger)
        logger_write(ctx->select_logger, LOG_ERROR, func, 0, "%s - sql=%s",
                     g_last_error, sql ? sql : "(none)");
}

/* A statement-handle failure: SQLSTATE text, plus the D6 hint when SQL
 * Server says a derived-table column has no name (8155) or a duplicate
 * one (8156). */
static void ms_fail_stmt(oci_context_t *ctx, const char *func, const char *sql,
                         SQLHSTMT stmt, const char *what)
{
    char diag[1536];
    mssql_diag(SQL_HANDLE_STMT, stmt, diag, sizeof(diag));
    int d6 = strstr(diag, "[native 8155]") || strstr(diag, "[native 8156]");
    ms_fail(ctx, func, sql, "%s failed: %s%s%s", what,
            diag[0] ? diag : "(no diagnostic record)",
            d6 ? " - " : "", d6 ? MSSQL_D6_HINT : "");
}

static void ms_cursor_free(mssql_cursor_t *cur)
{
    if (!cur) return;
    if (cur->stmt) SQLFreeHandle(SQL_HANDLE_STMT, cur->stmt);
    for (int c = 0; c < cur->ncols; c++)
    {
        if (cur->bufs) free(cur->bufs[c]);
        if (cur->inds) free(cur->inds[c]);
    }
    free(cur->bufs);
    free(cur->inds);
    free(cur->buf_size);
    free(cur->names);
    free(cur->field_types);
    free(cur->row_status);
    free(cur->bind_inds);
    free(cur);
}

/* Same vocabulary as resultset_field_t.field_type / driver_oracle. */
static const char *ms_field_type(SQLSMALLINT type, const char *type_name)
{
    switch (type)
    {
        case SQL_CHAR: case SQL_VARCHAR: case SQL_WCHAR: case SQL_WVARCHAR:
        case SQL_GUID: case SQL_SS_TIME2: case SQL_TYPE_TIME: case SQL_SS_VARIANT:
            return "STRING";
        case SQL_NUMERIC: case SQL_DECIMAL: case SQL_INTEGER: case SQL_SMALLINT:
        case SQL_TINYINT: case SQL_BIGINT: case SQL_BIT: case SQL_FLOAT:
        case SQL_REAL: case SQL_DOUBLE:
            return "NUMBER";
        case SQL_TYPE_DATE:
            return "DATE";
        case SQL_TYPE_TIMESTAMP:   /* datetime, datetime2, smalldatetime */
            return strcasecmp(type_name, "smalldatetime") == 0 ? "DATE" : "TIMESTAMP";
        case SQL_SS_TIMESTAMPOFFSET:
            return "TIMESTAMP";
        default:
            return "UNKNOWN";        /* binary, varbinary, rowversion ... */
    }
}

/* A column Stage 3 cannot fetch with a bound buffer (T4). */
static int ms_is_long(SQLSMALLINT type, SQLULEN colsize)
{
    switch (type)
    {
        case SQL_LONGVARCHAR: case SQL_WLONGVARCHAR: case SQL_LONGVARBINARY:
        case SQL_SS_XML: case SQL_SS_UDT:
            return 1;
        case SQL_VARCHAR: case SQL_WVARCHAR: case SQL_VARBINARY:
            return colsize == 0;     /* (max) - SQL_SS_LENGTH_UNLIMITED */
        default:
            return 0;
    }
}

static SQLLEN ms_buf_size(SQLSMALLINT type, SQLULEN colsize)
{
    unsigned long long n;
    switch (type)
    {
        case SQL_CHAR: case SQL_VARCHAR: case SQL_WCHAR: case SQL_WVARCHAR:
        case SQL_SS_VARIANT:
            n = (unsigned long long)colsize * 4 + 1;
            break;
        case SQL_BINARY: case SQL_VARBINARY:
            n = (unsigned long long)colsize * 2 + 1;
            break;
        default:
            n = (unsigned long long)colsize + 3;
            break;
    }
    if (n < MSSQL_FIELD_BUF_MIN) n = MSSQL_FIELD_BUF_MIN;
    if (n > MSSQL_FIELD_BUF_MAX) n = MSSQL_FIELD_BUF_MAX;
    return (SQLLEN)n;
}

/* UTF-16 code units in a UTF-8 string (a 4-byte sequence is 2 units) -
 * the column size of an nvarchar parameter. */
static SQLULEN ms_utf16_units(const char *s)
{
    SQLULEN n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        if ((*p & 0xC0) == 0x80) continue;   /* continuation byte */
        n += (*p >= 0xF0) ? 2 : 1;
    }
    return n;
}

int mssql_select_open(oci_context_t              *ctx,
                      const db_select_request_t  *req,
                      db_select_cursor_t        **out_cursor,
                      db_column_meta_t          **out_columns,
                      int                        *out_column_count,
                      int                        *out_batch_size)
{
    g_last_error[0] = '\0';
    if (out_cursor)       *out_cursor = NULL;
    if (out_columns)      *out_columns = NULL;
    if (out_column_count) *out_column_count = 0;
    if (out_batch_size)   *out_batch_size = 0;

    if (!ctx || !req || !req->sql || !out_cursor || !out_columns ||
        !out_column_count || !out_batch_size)
    {
        ms_fail(ctx, __func__, req ? req->sql : NULL, "select_open: invalid arguments");
        return -1;
    }
    if (!ctx->svchp)
    {
        ms_fail(ctx, __func__, req->sql,
                "select_open: no SQL Server connection on this session");
        return -1;
    }
    if (req->bind_count < 0 || (req->bind_count > 0 && !req->bind_values))
    {
        ms_fail(ctx, __func__, req->sql, "select_open: invalid binds (bind_count=%d)",
                req->bind_count);
        return -1;
    }

    mssql_cursor_t *cur = calloc(1, sizeof(*cur));
    if (!cur)
    {
        ms_fail(ctx, __func__, req->sql, "select_open: out of memory");
        return -1;
    }
    cur->ctx   = ctx;
    cur->batch = req->fetch_array_size > 0 ? req->fetch_array_size : 1;

    SQLRETURN r = SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)ctx->svchp, &cur->stmt);
    if (!SQL_SUCCEEDED(r))
    {
        char diag[1024];
        mssql_diag(SQL_HANDLE_DBC, ctx->svchp, diag, sizeof(diag));
        ms_fail(ctx, __func__, req->sql, "SQLAllocHandle(STMT) failed: %s",
                diag[0] ? diag : "(no diagnostic record)");
        cur->stmt = SQL_NULL_HSTMT;
        ms_cursor_free(cur);
        return -1;
    }

    if (req->query_timeout > 0)
    {
        r = SQLSetStmtAttr(cur->stmt, SQL_ATTR_QUERY_TIMEOUT,
                           (SQLPOINTER)(SQLULEN)req->query_timeout, 0);
        if (!SQL_SUCCEEDED(r))
        {
            ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, "SQL_ATTR_QUERY_TIMEOUT");
            ms_cursor_free(cur);
            return -1;
        }
    }

    r = SQLPrepare(cur->stmt, (SQLCHAR *)req->sql, SQL_NTS);
    if (!SQL_SUCCEEDED(r))
    {
        ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, "SQLPrepare");
        ms_cursor_free(cur);
        return -1;
    }

    /* ---- binds: text in, nvarchar on the wire; NULL entry = SQL NULL.
     * The values are read by SQLExecute below, inside this call, so the
     * caller's strings only have to outlive it; the indicators live in
     * the cursor. ---- */
    if (req->bind_count > 0)
    {
        cur->bind_inds = calloc((size_t)req->bind_count, sizeof(SQLLEN));
        if (!cur->bind_inds)
        {
            ms_fail(ctx, __func__, req->sql, "select_open: out of memory (binds)");
            ms_cursor_free(cur);
            return -1;
        }
        for (int k = 0; k < req->bind_count; k++)
        {
            const char *v = req->bind_values[k];
            SQLULEN     units = v ? ms_utf16_units(v) : 0;
            SQLULEN     colsize = units ? units : 1;
            SQLSMALLINT sqltype = units > 4000 ? SQL_WLONGVARCHAR : SQL_WVARCHAR;
            cur->bind_inds[k] = v ? (SQLLEN)strlen(v) : SQL_NULL_DATA;

            r = SQLBindParameter(cur->stmt, (SQLUSMALLINT)(k + 1), SQL_PARAM_INPUT,
                                 SQL_C_CHAR, sqltype, colsize, 0,
                                 (SQLPOINTER)v, v ? (SQLLEN)strlen(v) : 0,
                                 &cur->bind_inds[k]);
            if (!SQL_SUCCEEDED(r))
            {
                char what[64];
                snprintf(what, sizeof(what), "SQLBindParameter(%d)", k + 1);
                ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, what);
                ms_cursor_free(cur);
                return -1;
            }
        }
    }

    r = SQLExecute(cur->stmt);
    if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA)
    {
        ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, "SQLExecute");
        ms_cursor_free(cur);
        return -1;
    }

    /* The first result set. A batch may start with statements that only
     * report a row count (SET NOCOUNT OFF ...); those are skipped. */
    SQLSMALLINT ncols = 0;
    for (int skip = 0; ; skip++)
    {
        r = SQLNumResultCols(cur->stmt, &ncols);
        if (!SQL_SUCCEEDED(r))
        {
            ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, "SQLNumResultCols");
            ms_cursor_free(cur);
            return -1;
        }
        if (ncols > 0 || skip >= MSSQL_MAX_RESULT_SKIP) break;
        r = SQLMoreResults(cur->stmt);
        if (r == SQL_NO_DATA) break;
        if (!SQL_SUCCEEDED(r))
        {
            ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, "SQLMoreResults");
            ms_cursor_free(cur);
            return -1;
        }
    }
    if (ncols <= 0)
    {
        ms_fail(ctx, __func__, req->sql, "select_open: the statement returned no result set");
        ms_cursor_free(cur);
        return -1;
    }

    cur->ncols       = ncols;
    cur->names       = calloc((size_t)ncols, sizeof(*cur->names));
    cur->field_types = calloc((size_t)ncols, sizeof(*cur->field_types));
    cur->buf_size    = calloc((size_t)ncols, sizeof(*cur->buf_size));
    cur->bufs        = calloc((size_t)ncols, sizeof(*cur->bufs));
    cur->inds        = calloc((size_t)ncols, sizeof(*cur->inds));
    cur->row_status  = calloc((size_t)cur->batch, sizeof(*cur->row_status));
    if (!cur->names || !cur->field_types || !cur->buf_size || !cur->bufs ||
        !cur->inds || !cur->row_status)
    {
        ms_fail(ctx, __func__, req->sql, "select_open: out of memory (%d columns)", ncols);
        ms_cursor_free(cur);
        return -1;
    }

    /* ---- describe ---- */
    for (int c = 0; c < ncols; c++)
    {
        SQLSMALLINT name_len = 0, type = 0, dec = 0, nullable = 0;
        SQLULEN     colsize  = 0;
        char        type_name[64] = "";

        r = SQLDescribeCol(cur->stmt, (SQLUSMALLINT)(c + 1),
                           (SQLCHAR *)cur->names[c], (SQLSMALLINT)sizeof(cur->names[c]),
                           &name_len, &type, &colsize, &dec, &nullable);
        if (!SQL_SUCCEEDED(r))
        {
            ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, "SQLDescribeCol");
            ms_cursor_free(cur);
            return -1;
        }
        SQLColAttribute(cur->stmt, (SQLUSMALLINT)(c + 1), SQL_DESC_TYPE_NAME,
                        type_name, (SQLSMALLINT)sizeof(type_name), NULL, NULL);
        /* "int identity" -> "int" */
        char *sp = strchr(type_name, ' ');
        if (sp) *sp = '\0';

        if (ms_is_long(type, colsize))
        {
            ms_fail(ctx, __func__, req->sql,
                    "driver_mssql: long column %s (%s%s) in SELECT not implemented "
                    "yet (stage 5)", cur->names[c][0] ? cur->names[c] : "(no name)",
                    type_name[0] ? type_name : "?",
                    colsize == 0 && type != SQL_SS_XML && type != SQL_SS_UDT ? "(max)" : "");
            ms_cursor_free(cur);
            return -1;
        }

        cur->field_types[c] = ms_field_type(type, type_name);
        cur->buf_size[c]    = ms_buf_size(type, colsize);
        cur->bufs[c]        = calloc((size_t)cur->batch, (size_t)cur->buf_size[c]);
        cur->inds[c]        = calloc((size_t)cur->batch, sizeof(SQLLEN));
        if (!cur->bufs[c] || !cur->inds[c])
        {
            ms_fail(ctx, __func__, req->sql,
                    "select_open: out of memory (column %s, %d x %ld bytes)",
                    cur->names[c], cur->batch, (long)cur->buf_size[c]);
            ms_cursor_free(cur);
            return -1;
        }

        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                     "column %d name=%s type=%s sql_type=%d size=%lu buf=%ld field_type=%s",
                     c + 1, cur->names[c], type_name, (int)type,
                     (unsigned long)colsize, (long)cur->buf_size[c], cur->field_types[c]);
    }

    /* ---- column-wise array fetch ---- */
    if (!SQL_SUCCEEDED(SQLSetStmtAttr(cur->stmt, SQL_ATTR_ROW_BIND_TYPE,
                                      (SQLPOINTER)SQL_BIND_BY_COLUMN, 0)) ||
        !SQL_SUCCEEDED(SQLSetStmtAttr(cur->stmt, SQL_ATTR_ROW_ARRAY_SIZE,
                                      (SQLPOINTER)(SQLULEN)cur->batch, 0)) ||
        !SQL_SUCCEEDED(SQLSetStmtAttr(cur->stmt, SQL_ATTR_ROWS_FETCHED_PTR,
                                      &cur->rows_fetched, 0)) ||
        !SQL_SUCCEEDED(SQLSetStmtAttr(cur->stmt, SQL_ATTR_ROW_STATUS_PTR,
                                      cur->row_status, 0)))
    {
        ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, "SQLSetStmtAttr(array fetch)");
        ms_cursor_free(cur);
        return -1;
    }

    for (int c = 0; c < ncols; c++)
    {
        r = SQLBindCol(cur->stmt, (SQLUSMALLINT)(c + 1), SQL_C_CHAR,
                       cur->bufs[c], cur->buf_size[c], cur->inds[c]);
        if (!SQL_SUCCEEDED(r))
        {
            ms_fail_stmt(ctx, __func__, req->sql, cur->stmt, "SQLBindCol");
            ms_cursor_free(cur);
            return -1;
        }
    }

    db_column_meta_t *columns = calloc((size_t)ncols, sizeof(*columns));
    if (!columns)
    {
        ms_fail(ctx, __func__, req->sql, "select_open: out of memory (column list)");
        ms_cursor_free(cur);
        return -1;
    }
    for (int c = 0; c < ncols; c++)
    {
        snprintf(columns[c].field_name, sizeof(columns[c].field_name), "%s", cur->names[c]);
        snprintf(columns[c].field_type, sizeof(columns[c].field_type), "%s",
                 cur->field_types[c]);
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "select_open OK: %d column(s), batch_size=%d, %d bind(s)",
                 ncols, cur->batch, req->bind_count);

    *out_cursor       = (db_select_cursor_t *)cur;
    *out_columns      = columns;
    *out_column_count = ncols;
    *out_batch_size   = cur->batch;
    return 0;
}

int mssql_select_fetch_batch(db_select_cursor_t      *cursor,
                             resultset_t            **out_rs,
                             int                     *out_rows_fetched,
                             db_fetch_batch_stats_t  *out_stats)
{
    mssql_cursor_t *cur = (mssql_cursor_t *)cursor;

    if (out_stats) memset(out_stats, 0, sizeof(*out_stats));   /* no LOBs yet */
    if (out_rs) *out_rs = NULL;
    if (out_rows_fetched) *out_rows_fetched = 0;
    if (!cur || !out_rs || !out_rows_fetched) return -1;

    g_last_error[0] = '\0';
    if (cur->exhausted) return 0;

    oci_context_t *ctx = cur->ctx;
    cur->rows_fetched = 0;

    SQLRETURN r = SQLFetch(cur->stmt);
    if (r == SQL_NO_DATA)
    {
        cur->exhausted = 1;
        return 0;
    }
    if (!SQL_SUCCEEDED(r))
    {
        ms_fail_stmt(ctx, __func__, NULL, cur->stmt, "SQLFetch");
        return -1;
    }

    int rows = (int)cur->rows_fetched;
    if (rows <= 0)
    {
        cur->exhausted = 1;
        return 0;
    }
    /* A short batch is the last one, but the next call still asks the
     * driver (it answers SQL_NO_DATA): that reads the end of the result
     * off the connection, so nothing is left pending on it. ODBC has no
     * equivalent of Oracle's ORA-01002 for fetching past the end. */

    resultset_t *rs = resultset_create(rows, cur->ncols);
    if (!rs)
    {
        ms_fail(ctx, __func__, NULL, "resultset_create failed (%d rows x %d columns)",
                rows, cur->ncols);
        return -1;
    }

    for (int i = 0; i < rows; i++)
    {
        if (cur->row_status[i] == SQL_ROW_ERROR)
        {
            ms_fail_stmt(ctx, __func__, NULL, cur->stmt, "SQLFetch (row error)");
            resultset_free(rs);
            return -1;
        }
        resultset_row_t *row = resultset_get_row(rs, i + 1);
        for (int c = 0; c < cur->ncols; c++)
        {
            SQLLEN      ind   = cur->inds[c][i];
            const char *value = "";
            if (ind != SQL_NULL_DATA)
            {
                if (ind == SQL_NO_TOTAL || ind < 0 || ind >= cur->buf_size[c])
                {
                    ms_fail(ctx, __func__, NULL,
                            "value of column %s is longer than its %ld-byte fetch "
                            "buffer (length %ld) - fetch failed, value not cut short",
                            cur->names[c], (long)cur->buf_size[c], (long)ind);
                    resultset_free(rs);
                    return -1;
                }
                value = cur->bufs[c] + (size_t)i * (size_t)cur->buf_size[c];
            }
            resultset_set_field(row, c, cur->names[c], cur->field_types[c], value);
        }
    }

    *out_rs           = rs;
    *out_rows_fetched = rows;
    return 0;
}

void mssql_select_close(db_select_cursor_t *cursor)
{
    ms_cursor_free((mssql_cursor_t *)cursor);
}
