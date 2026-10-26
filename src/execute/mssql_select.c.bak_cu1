/*
 * mssql_select.c   (driver_mssql Stage 3, 2026-10-19, points T3-T5;
 *                   Stage 5, 2026-10-21, points W1-W6;
 *                   Stage 6, 2026-10-21, points X3-X4)
 *
 * SELECT through ODBC - see mssql_select.h for what each call does.
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
 * Long columns (Stage 5). A result with a varchar(max) / nvarchar(max) /
 * text / ntext / xml column (CLOB) or a varbinary(max) / image column
 * (BLOB) is fetched one row at a time with no bound buffers: every
 * column is read with SQLGetData, in column order (the Microsoft driver
 * reads unbound columns only in order, and only after the last bound
 * one). The batch is still the requested size - it is filled one row
 * at a time. Text LOBs are read as UTF-16 (SQL_C_WCHAR) and converted to
 * UTF-8 once the whole value is in, so where a piece ends never matters;
 * BLOBs as bytes. Then, as driver_oracle does, through the same core
 * helpers (Blob_Utils / Clob_Utils / get_mime_type()): a BLOB is written
 * to BLOB_output_dir and described in the field; a CLOB goes to
 * CLOB_output_dir with its URL in the field, or in the field itself
 * with text_lobs_inline. Unlike driver_oracle, a BLOB file that cannot
 * be written fails the fetch (W4). A result with no long column is
 * fetched exactly as in Stage 3.
 *
 * Procedure result sets (Stage 6). mssql_select_buffer_result() reads the
 * current result set of a procedure call's statement through the same
 * describe/bind (ms_setup_columns) and fetch code as a SELECT, holding
 * every batch; the cursor it returns is "buffered" - fetch_batch hands
 * the held batches out in order and never touches ODBC again.
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
#include <stdint.h>

#include <sql.h>
#include <sqlext.h>

#include "mssql_select.h"
#include "MSSQL_Pool.h"          /* mssql_diag()                         */
#include "Resultset_Builder.h"
#include "Blob_Utils.h"          /* lookup_blob_index(), write_blob_to_file(),
                                    build_filename_with_timestamp(),
                                    lob_output_tag() - Stage 5           */
#include "Clob_Utils.h"          /* build_clob_filename(), build_clob_url() */
#include "mssql_text.h"          /* mssql_prepare_utf8() - Stage 5 Fix 1 */
#include "logger.h"

/* XML_Helper.c - forward-declared, as driver_oracle.c does (XML_Helper.h
 * pulls in the query batch module). A pure file-extension lookup. */
const char *get_mime_type(const char *filename);

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
#ifndef SQL_CA_SS_UDT_TYPE_NAME
#define SQL_CA_SS_UDT_TYPE_NAME 1224    /* SQL_CA_SS_BASE + 24 (+22 is the
                                           catalog - Stage 5 Fix 2) */
#endif
#ifndef SQL_SS_LENGTH_UNLIMITED
#define SQL_SS_LENGTH_UNLIMITED 0       /* (max) */
#endif

#define MS_LOB_NONE   0
#define MS_LOB_TEXT   1
#define MS_LOB_BINARY 2

#define MSSQL_FIELD_BUF_MAX   16004   /* T3 */
#define MSSQL_FIELD_BUF_MIN   64
#define MSSQL_MAX_RESULT_SKIP 16      /* leading row-count results skipped
                                         before the first result set     */

typedef struct {
    oci_context_t  *ctx;            /* borrowed                          */
    SQLHSTMT        stmt;
    int             ncols;
    int             batch;          /* rows per SQLFetch                 */
    char          (*names)[256];    /* [256]: lookup_blob_index()'s shape */
    const char    **field_types;    /* static strings                    */
    int            *lob_kind;       /* [col] MS_LOB_* (Stage 5)          */
    int             has_lob;        /* any long column: row-at-a-time    */
    int             text_lobs_inline;
    unsigned int    abs_rownum;     /* cursor lifetime, 1-based, as
                                       driver_oracle - CLOB file names   */
    int             blob_index;     /* cursor lifetime, as driver_oracle */
    int             clob_index;
    char            lob_tag[LOB_OUTPUT_TAG_SIZE];
    SQLLEN         *buf_size;       /* [col]                             */
    char          **bufs;           /* [col] -> batch * buf_size bytes   */
    SQLLEN        **inds;           /* [col] -> batch indicators         */
    SQLULEN         rows_fetched;
    SQLUSMALLINT   *row_status;     /* [batch]                           */
    SQLLEN         *bind_inds;      /* [bind_count]                      */
    int             exhausted;

    /* Stage 6 - a procedure's result set, read in full (mssql_proc.c).
     * stmt_borrowed: stmt belongs to the procedure call, never freed
     * here. buffered: every batch is held below and stmt is NULL;
     * select_fetch_batch hands them out in order.                     */
    int             stmt_borrowed;
    int             buffered;
    resultset_t   **bat;            /* [nbat] - NULL once handed out     */
    int            *bat_rows;
    db_fetch_batch_stats_t *bat_stats;
    int             nbat, next;
    db_column_meta_t *cols;         /* [ncols] - copied out by open      */
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
    if (cur->stmt && !cur->stmt_borrowed) SQLFreeHandle(SQL_HANDLE_STMT, cur->stmt);
    for (int b = 0; b < cur->nbat; b++)
        if (cur->bat && cur->bat[b]) resultset_free(cur->bat[b]);
    free(cur->bat);
    free(cur->bat_rows);
    free(cur->bat_stats);
    free(cur->cols);
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
    free(cur->lob_kind);
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

/* W1: a long column, read with SQLGetData. (max) is reported as column
 * size 0 (SQL_SS_LENGTH_UNLIMITED). */
static int ms_lob_kind(SQLSMALLINT type, SQLULEN colsize)
{
    switch (type)
    {
        case SQL_LONGVARCHAR: case SQL_WLONGVARCHAR: case SQL_SS_XML:
            return MS_LOB_TEXT;
        case SQL_LONGVARBINARY:
            return MS_LOB_BINARY;
        case SQL_VARCHAR: case SQL_WVARCHAR:
            return colsize == 0 ? MS_LOB_TEXT : MS_LOB_NONE;
        case SQL_VARBINARY:
            return colsize == 0 ? MS_LOB_BINARY : MS_LOB_NONE;
        default:
            return MS_LOB_NONE;
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

/* Describes the current result set of cur->stmt (ncols columns) and
 * binds it for fetching: per-column buffers and a column-wise array of
 * cur->batch rows, or none when it has a long column (W2). Fills
 * *out_columns (caller frees). 0, or -1 logged - the caller frees cur.
 * Shared by select_open and, from Stage 6, a procedure's result sets. */
static int ms_setup_columns(mssql_cursor_t *cur, const char *sql, SQLSMALLINT ncols,
                            db_column_meta_t **out_columns)
{
    oci_context_t *ctx = cur->ctx;
    SQLRETURN      r;
    cur->ncols       = ncols;
    cur->names       = calloc((size_t)ncols, sizeof(*cur->names));
    cur->field_types = calloc((size_t)ncols, sizeof(*cur->field_types));
    cur->lob_kind    = calloc((size_t)ncols, sizeof(*cur->lob_kind));
    cur->buf_size    = calloc((size_t)ncols, sizeof(*cur->buf_size));
    cur->bufs        = calloc((size_t)ncols, sizeof(*cur->bufs));
    cur->inds        = calloc((size_t)ncols, sizeof(*cur->inds));
    cur->row_status  = calloc((size_t)cur->batch, sizeof(*cur->row_status));
    if (!cur->names || !cur->field_types || !cur->lob_kind || !cur->buf_size ||
        !cur->bufs || !cur->inds || !cur->row_status)
    {
        ms_fail(ctx, __func__, sql, "select_open: out of memory (%d columns)", ncols);
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
            ms_fail_stmt(ctx, __func__, sql, cur->stmt, "SQLDescribeCol");
            return -1;
        }
        SQLColAttribute(cur->stmt, (SQLUSMALLINT)(c + 1), SQL_DESC_TYPE_NAME,
                        type_name, (SQLSMALLINT)sizeof(type_name), NULL, NULL);
        /* "int identity" -> "int" */
        char *sp = strchr(type_name, ' ');
        if (sp) *sp = '\0';

        /* W1: CLR types (geometry, geography, hierarchyid, user types)
         * have no text form the driver can give. */
        if (type == SQL_SS_UDT)
        {
            /* The type name the driver reports is "udt"; the CLR type's
             * own name (hierarchyid ...) is a driver-specific attribute. */
            char   udt_name[64] = "";
            SQLLEN unused = 0;
            if (SQL_SUCCEEDED(SQLColAttribute(cur->stmt, (SQLUSMALLINT)(c + 1),
                                              SQL_CA_SS_UDT_TYPE_NAME, udt_name,
                                              (SQLSMALLINT)sizeof(udt_name), NULL,
                                              &unused)) && udt_name[0])
                snprintf(type_name, sizeof(type_name), "%s", udt_name);
            ms_fail(ctx, __func__, sql,
                    "driver_mssql: column %s is a CLR type (%s) - not supported in a "
                    "SELECT; convert it in the query (for example CAST(... AS "
                    "nvarchar(max)))", cur->names[c][0] ? cur->names[c] : "(no name)",
                    type_name[0] ? type_name : "?");
            return -1;
        }

        cur->lob_kind[c] = ms_lob_kind(type, colsize);
        if (cur->lob_kind[c] != MS_LOB_NONE)
        {
            cur->has_lob        = 1;
            cur->field_types[c] = cur->lob_kind[c] == MS_LOB_BINARY ? "BLOB" : "CLOB";
            logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                         "column %d name=%s type=%s sql_type=%d long - field_type=%s",
                         c + 1, cur->names[c], type_name, (int)type, cur->field_types[c]);
            continue;           /* read with SQLGetData - no buffer */
        }

        cur->field_types[c] = ms_field_type(type, type_name);
        cur->buf_size[c]    = ms_buf_size(type, colsize);
        cur->bufs[c]        = calloc((size_t)cur->batch, (size_t)cur->buf_size[c]);
        cur->inds[c]        = calloc((size_t)cur->batch, sizeof(SQLLEN));
        if (!cur->bufs[c] || !cur->inds[c])
        {
            ms_fail(ctx, __func__, sql,
                    "select_open: out of memory (column %s, %d x %ld bytes)",
                    cur->names[c], cur->batch, (long)cur->buf_size[c]);
            return -1;
        }

        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                     "column %d name=%s type=%s sql_type=%d size=%lu buf=%ld field_type=%s",
                     c + 1, cur->names[c], type_name, (int)type,
                     (unsigned long)colsize, (long)cur->buf_size[c], cur->field_types[c]);
    }

    /* ---- W2: a result with a long column - no bound buffers; rows are
     * fetched one at a time and every column read with SQLGetData. ---- */
    if (cur->has_lob)
    {
        lob_output_tag(cur->lob_tag, sizeof(cur->lob_tag));
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "LOB output tag %s for this SELECT", cur->lob_tag);
    }

    /* ---- column-wise array fetch ---- */
    if (!cur->has_lob &&
        (!SQL_SUCCEEDED(SQLSetStmtAttr(cur->stmt, SQL_ATTR_ROW_BIND_TYPE,
                                       (SQLPOINTER)SQL_BIND_BY_COLUMN, 0)) ||
         !SQL_SUCCEEDED(SQLSetStmtAttr(cur->stmt, SQL_ATTR_ROW_ARRAY_SIZE,
                                       (SQLPOINTER)(SQLULEN)cur->batch, 0)) ||
         !SQL_SUCCEEDED(SQLSetStmtAttr(cur->stmt, SQL_ATTR_ROWS_FETCHED_PTR,
                                       &cur->rows_fetched, 0)) ||
         !SQL_SUCCEEDED(SQLSetStmtAttr(cur->stmt, SQL_ATTR_ROW_STATUS_PTR,
                                       cur->row_status, 0))))
    {
        ms_fail_stmt(ctx, __func__, sql, cur->stmt, "SQLSetStmtAttr(array fetch)");
        return -1;
    }

    for (int c = 0; c < ncols && !cur->has_lob; c++)
    {
        r = SQLBindCol(cur->stmt, (SQLUSMALLINT)(c + 1), SQL_C_CHAR,
                       cur->bufs[c], cur->buf_size[c], cur->inds[c]);
        if (!SQL_SUCCEEDED(r))
        {
            ms_fail_stmt(ctx, __func__, sql, cur->stmt, "SQLBindCol");
            return -1;
        }
    }

    *out_columns = NULL;
    db_column_meta_t *columns = calloc((size_t)ncols, sizeof(*columns));
    if (!columns)
    {
        ms_fail(ctx, __func__, sql, "select_open: out of memory (column list)");
        return -1;
    }
    for (int c = 0; c < ncols; c++)
    {
        snprintf(columns[c].field_name, sizeof(columns[c].field_name), "%s", cur->names[c]);
        snprintf(columns[c].field_type, sizeof(columns[c].field_type), "%s",
                 cur->field_types[c]);
    }

    *out_columns = columns;
    return 0;
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
    cur->text_lobs_inline = req->text_lobs_inline;

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

    r = mssql_prepare_utf8(cur->stmt, req->sql);     /* UTF-16 text (Fix 1) */
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
            /* W6 (as mssql_dml.c Fix 1): beyond nvarchar(4000) it is
             * nvarchar(max) - SQL_WVARCHAR, SQL_SS_LENGTH_UNLIMITED. */
            SQLULEN     colsize = units > 4000 ? SQL_SS_LENGTH_UNLIMITED : (units ? units : 1);
            SQLSMALLINT sqltype = SQL_WVARCHAR;
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

    db_column_meta_t *columns = NULL;
    if (ms_setup_columns(cur, req->sql, ncols, &columns) != 0)
    {
        ms_cursor_free(cur);
        return -1;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "select_open OK: %d column(s), batch_size=%d, %d bind(s)%s",
                 ncols, cur->batch, req->bind_count,
                 cur->has_lob ? ", long columns: row at a time" : "");

    *out_cursor       = (db_select_cursor_t *)cur;
    *out_columns      = columns;
    *out_column_count = ncols;
    *out_batch_size   = cur->batch;
    return 0;
}

/* ================================================================== */
/*  Stage 5 - long columns (W2-W5)                                     */
/* ================================================================== */

/* A growing byte buffer. */
typedef struct { unsigned char *p; size_t n, cap; } ms_buf_t;

static int ms_buf_add(ms_buf_t *b, const void *data, size_t n)
{
    if (b->n + n + 1 > b->cap)
    {
        size_t ncap = b->cap ? b->cap : 4096;
        while (b->n + n + 1 > ncap) ncap *= 2;
        unsigned char *np = realloc(b->p, ncap);
        if (!np) return -1;
        b->p = np;
        b->cap = ncap;
    }
    if (n) memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = '\0';
    return 0;
}

/* UTF-16LE (as the driver gives SQL_C_WCHAR) to UTF-8. A lone surrogate
 * becomes U+FFFD. out gets a terminating NUL. */
static int ms_utf16_to_utf8(const unsigned char *in, size_t nbytes, ms_buf_t *out)
{
    size_t units = nbytes / 2;
    for (size_t i = 0; i < units; i++)
    {
        uint32_t cp = (uint32_t)in[2 * i] | ((uint32_t)in[2 * i + 1] << 8);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units)
        {
            uint32_t lo = (uint32_t)in[2 * i + 2] | ((uint32_t)in[2 * i + 3] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
            else cp = 0xFFFD;
        }
        else if (cp >= 0xD800 && cp <= 0xDFFF)
            cp = 0xFFFD;

        unsigned char u[4];
        size_t        k;
        if (cp < 0x80)         { u[0] = (unsigned char)cp; k = 1; }
        else if (cp < 0x800)   { u[0] = (unsigned char)(0xC0 | (cp >> 6));
                                 u[1] = (unsigned char)(0x80 | (cp & 0x3F)); k = 2; }
        else if (cp < 0x10000) { u[0] = (unsigned char)(0xE0 | (cp >> 12));
                                 u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                 u[2] = (unsigned char)(0x80 | (cp & 0x3F)); k = 3; }
        else                   { u[0] = (unsigned char)(0xF0 | (cp >> 18));
                                 u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                                 u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                 u[3] = (unsigned char)(0x80 | (cp & 0x3F)); k = 4; }
        if (ms_buf_add(out, u, k) != 0) return -1;
    }
    if (out->n == 0 && ms_buf_add(out, "", 0) != 0) return -1;   /* "" not NULL */
    return 0;
}

/* W3/W4: one long column of the current row, whole, in chunk_read_size
 * pieces. Text arrives as UTF-16 and is returned as UTF-8; binary as is.
 * *is_null set for SQL NULL. 0 or -1 (logged). */
static int ms_read_long(mssql_cursor_t *cur, int c, ms_buf_t *val, int *is_null)
{
    oci_context_t *ctx  = cur->ctx;
    int            text = cur->lob_kind[c] == MS_LOB_TEXT;
    size_t chunk = ctx->ini && ctx->ini->chunk_read_size ? (size_t)ctx->ini->chunk_read_size : 65536;
    if (chunk < 64) chunk = 64;
    chunk &= ~(size_t)1;                       /* whole UTF-16 units */
    size_t   cap  = chunk + (text ? 2 : 0);    /* + the wide NUL      */
    unsigned char *piece = malloc(cap);
    ms_buf_t raw = { 0 };
    int      rc  = -1;

    *is_null = 0;
    if (!piece)
    {
        ms_fail(ctx, __func__, NULL, "out of memory reading column %s", cur->names[c]);
        return -1;
    }

    for (;;)
    {
        SQLLEN    ind = 0;
        SQLRETURN r   = SQLGetData(cur->stmt, (SQLUSMALLINT)(c + 1),
                                   text ? SQL_C_WCHAR : SQL_C_BINARY,
                                   piece, (SQLLEN)cap, &ind);
        if (r == SQL_NO_DATA) break;           /* all pieces read (or empty) */
        if (!SQL_SUCCEEDED(r))
        {
            char what[160];
            snprintf(what, sizeof(what), "SQLGetData (column %.128s)", cur->names[c]);
            ms_fail_stmt(ctx, __func__, NULL, cur->stmt, what);
            goto Done;
        }
        if (ind == SQL_NULL_DATA)
        {
            *is_null = 1;
            rc = 0;
            goto Done;
        }
        size_t room = cap - (text ? 2 : 0);
        size_t got  = (ind == SQL_NO_TOTAL || ind < 0 || (size_t)ind > room) ? room : (size_t)ind;
        if (ms_buf_add(&raw, piece, got) != 0)
        {
            ms_fail(ctx, __func__, NULL, "out of memory reading column %s (%zu bytes so far)",
                    cur->names[c], raw.n);
            goto Done;
        }
        if (r == SQL_SUCCESS) break;           /* that was the last piece */
    }

    if (text)
    {
        if (ms_utf16_to_utf8(raw.p ? raw.p : (const unsigned char *)"", raw.n, val) != 0)
        {
            ms_fail(ctx, __func__, NULL, "out of memory converting column %s", cur->names[c]);
            goto Done;
        }
    }
    else
    {
        free(val->p);                          /* the previous row's value */
        *val = raw;
        raw.p = NULL;
        if (!val->p && ms_buf_add(val, "", 0) != 0) goto Done;
    }
    rc = 0;

Done:
    free(raw.p);
    free(piece);
    return rc;
}

/* W4 - as oracle_fetch_blob_field() after its read: file name from the
 * BLOB_default_file_name_col[_n] column of the same row (any position -
 * the row is read first), timestamp option, MIME type, share options,
 * write_blob_to_file(), resultset_set_blob_field(). A file that cannot
 * be written fails the fetch. NULL or empty: "" and no file. */
static int ms_blob_field(mssql_cursor_t *cur, int c, resultset_row_t *row,
                         const int *scalar_null, const ms_buf_t *v, int is_null,
                         uint64_t *out_bytes)
{
    oci_context_t *ctx        = cur->ctx;
    int            blob_index = cur->blob_index;
    *out_bytes = 0;

    if (is_null || v->n == 0)
    {
        resultset_set_field(row, c, cur->names[c], "BLOB", "");
        cur->blob_index++;
        return 0;
    }
    if (v->n > UINT32_MAX)
    {
        ms_fail(ctx, __func__, NULL, "BLOB column %s is %zu bytes - over 4 GB",
                cur->names[c], v->n);
        return -1;
    }

    const char *search_col = ctx->ini->BLOB_default_file_name_col;
    switch (blob_index)
    {
        case 1: search_col = ctx->ini->BLOB_default_file_name_col_1; break;
        case 2: search_col = ctx->ini->BLOB_default_file_name_col_2; break;
        case 3: search_col = ctx->ini->BLOB_default_file_name_col_3; break;
        case 4: search_col = ctx->ini->BLOB_default_file_name_col_4; break;
        case 5: search_col = ctx->ini->BLOB_default_file_name_col_5; break;
    }
    int name_col = lookup_blob_index(cur->names, cur->ncols, search_col, ctx);

    char final_name[512];
    if (name_col >= 0 && cur->lob_kind[name_col] == MS_LOB_NONE && !scalar_null[name_col])
    {
        const char *name_val = row->fields[name_col].value;
        if (ctx->ini->BLOB_append_file_timestamp == 1)
            build_filename_with_timestamp(name_val, cur->lob_tag, final_name,
                                          sizeof(final_name), blob_index, ctx);
        else
            snprintf(final_name, sizeof(final_name), "%.511s", name_val);
    }
    else
        snprintf(final_name, sizeof(final_name), "%s_%s_%d",
                 ctx->ini->BLOB_default_file_name, cur->lob_tag, blob_index);

    lob_item_t item;
    memset(&item, 0, sizeof(item));
    item.column_name = cur->names[c];
    item.file_name   = final_name;
    item.mime_type   = (char *)get_mime_type(final_name);
    item.blob_data   = v->p;
    item.blob_size   = (uint32_t)v->n;
    item.output_file_url         = ctx->ini->xml_share_BLOB_URL_path  ? ctx->ini->BLOB_URL_path
                                                                      : (char *)"N/A";
    item.output_file_destination = ctx->ini->xml_share_BLOB_host_path ? ctx->ini->BLOB_output_dir
                                                                      : (char *)"N/A";

    if (write_blob_to_file(&item, ctx->ini->BLOB_output_dir, ctx) != 0)
    {
        ms_fail(ctx, __func__, NULL, "BLOB column %s: could not write %s/%s (%zu bytes) - "
                "fetch failed", cur->names[c], ctx->ini->BLOB_output_dir, final_name, v->n);
        return -1;
    }

    resultset_set_blob_field(row, c, item.column_name, item.file_name,
                             item.output_file_destination, item.output_file_url,
                             item.blob_size, item.mime_type);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "BLOB col=%s row=%u index=%d bytes=%zu file=%s", cur->names[c],
                 cur->abs_rownum, blob_index, v->n, final_name);
    *out_bytes = v->n;
    cur->blob_index++;
    return 0;
}

/* W3 - as oracle_fetch_clob_field() after its read: inline (fails when
 * longer than the field) or a file under CLOB_output_dir with its URL in
 * the field; a file that cannot be written puts the text in the field,
 * as driver_oracle does. Bytes are UTF-8 bytes. */
static int ms_clob_field(mssql_cursor_t *cur, int c, resultset_row_t *row,
                         const ms_buf_t *v, int is_null, uint64_t *out_bytes)
{
    oci_context_t *ctx        = cur->ctx;
    int            clob_index = cur->clob_index;
    *out_bytes = 0;

    if (is_null || v->n == 0)
    {
        resultset_set_field(row, c, cur->names[c], "CLOB", "");
        cur->clob_index++;
        return 0;
    }

    if (cur->text_lobs_inline)
    {
        size_t field_max = sizeof(row->fields[0].value) - 1;
        if (v->n > field_max)
        {
            ms_fail(ctx, __func__, NULL,
                    "CLOB col=%s length=%zu does not fit the %zu-byte field "
                    "(text_lobs_inline) - fetch failed, value not truncated",
                    cur->names[c], v->n, field_max);
            return -1;
        }
        resultset_set_field(row, c, cur->names[c], "CLOB", (const char *)v->p);
        *out_bytes = v->n;
        cur->clob_index++;
        return 0;
    }

    char clob_filename[512], clob_filepath[768], clob_url[768];
    build_clob_filename(cur->names[c], cur->lob_tag, cur->abs_rownum, clob_index,
                        clob_filename, sizeof(clob_filename), ctx);
    snprintf(clob_filepath, sizeof(clob_filepath), "%s/%s",
             ctx->ini->CLOB_output_dir, clob_filename);

    lob_item_t item;
    memset(&item, 0, sizeof(item));
    item.file_name = clob_filename;
    item.blob_data = v->p;
    item.blob_size = (uint32_t)v->n;

    if (v->n > UINT32_MAX || write_blob_to_file(&item, ctx->ini->CLOB_output_dir, ctx) != 0)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "write_blob_to_file failed for CLOB %s - emitting inline "
                     "content instead of a file reference", clob_filepath);
        resultset_set_field(row, c, cur->names[c], "CLOB", (const char *)v->p);
    }
    else
    {
        build_clob_url(clob_filename, clob_filepath, clob_url, sizeof(clob_url), ctx);
        resultset_set_field(row, c, cur->names[c], "CLOB", clob_url);
    }
    *out_bytes = v->n;
    cur->clob_index++;
    return 0;
}

/* W2: up to batch rows, one SQLFetch each, every column by SQLGetData
 * in order; then the row's LOBs (left to right, as driver_oracle
 * numbers them). */
static int ms_fetch_lob_batch(mssql_cursor_t *cur, resultset_t **out_rs,
                              int *out_rows_fetched, db_fetch_batch_stats_t *out_stats)
{
    oci_context_t *ctx  = cur->ctx;
    int            rows = 0, rc = -1;
    resultset_t   *rs   = resultset_create(cur->batch, cur->ncols);
    ms_buf_t      *vals = calloc((size_t)cur->ncols, sizeof(*vals));
    int           *nul  = calloc((size_t)cur->ncols, sizeof(*nul));
    if (!rs || !vals || !nul)
    {
        ms_fail(ctx, __func__, NULL, "out of memory (%d rows x %d columns)", cur->batch, cur->ncols);
        goto Done;
    }

    while (rows < cur->batch)
    {
        SQLRETURN r = SQLFetch(cur->stmt);
        if (r == SQL_NO_DATA)
        {
            cur->exhausted = 1;
            break;
        }
        if (!SQL_SUCCEEDED(r))
        {
            ms_fail_stmt(ctx, __func__, NULL, cur->stmt, "SQLFetch");
            goto Done;
        }
        cur->abs_rownum++;
        resultset_row_t *row = resultset_get_row(rs, rows + 1);

        for (int c = 0; c < cur->ncols; c++)
        {
            vals[c].n = 0;
            nul[c]    = 0;
            if (cur->lob_kind[c] != MS_LOB_NONE)
            {
                if (ms_read_long(cur, c, &vals[c], &nul[c]) != 0) goto Done;
                continue;
            }
            SQLLEN    ind = 0;
            SQLRETURN g   = SQLGetData(cur->stmt, (SQLUSMALLINT)(c + 1), SQL_C_CHAR,
                                       cur->bufs[c], cur->buf_size[c], &ind);
            if (!SQL_SUCCEEDED(g))
            {
                char what[160];
                snprintf(what, sizeof(what), "SQLGetData (column %.128s)", cur->names[c]);
                ms_fail_stmt(ctx, __func__, NULL, cur->stmt, what);
                goto Done;
            }
            const char *value = "";
            if (ind == SQL_NULL_DATA)
                nul[c] = 1;
            else
            {
                if (ind == SQL_NO_TOTAL || ind < 0 || ind >= cur->buf_size[c])
                {
                    ms_fail(ctx, __func__, NULL,
                            "value of column %s is longer than its %ld-byte fetch "
                            "buffer (length %ld) - fetch failed, value not cut short",
                            cur->names[c], (long)cur->buf_size[c], (long)ind);
                    goto Done;
                }
                value = cur->bufs[c];
            }
            resultset_set_field(row, c, cur->names[c], cur->field_types[c], value);
        }

        for (int c = 0; c < cur->ncols; c++)
        {
            uint64_t bytes = 0;
            if (cur->lob_kind[c] == MS_LOB_BINARY)
            {
                if (ms_blob_field(cur, c, row, nul, &vals[c], nul[c], &bytes) != 0) goto Done;
                if (out_stats) { out_stats->blob_count++; out_stats->blob_bytes += bytes; }
            }
            else if (cur->lob_kind[c] == MS_LOB_TEXT)
            {
                if (ms_clob_field(cur, c, row, &vals[c], nul[c], &bytes) != 0) goto Done;
                if (out_stats) { out_stats->clob_count++; out_stats->clob_bytes += bytes; }
            }
        }
        rows++;
    }

    /* The batch may be short: drop the rows that were never filled. */
    for (int i = rows; i < rs->record_count; i++)
    {
        free(rs->records[i].fields);
        rs->records[i].fields = NULL;
    }
    rs->record_count = rows;
    if (rows == 0)
    {
        resultset_free(rs);
        rs = NULL;
    }
    *out_rs           = rs;
    *out_rows_fetched = rows;
    rs = NULL;
    rc = 0;

Done:
    if (rc != 0 && out_stats) memset(out_stats, 0, sizeof(*out_stats));
    if (rs) resultset_free(rs);
    if (vals) for (int c = 0; c < cur->ncols; c++) free(vals[c].p);
    free(vals);
    free(nul);
    return rc;
}

int mssql_select_fetch_batch(db_select_cursor_t      *cursor,
                             resultset_t            **out_rs,
                             int                     *out_rows_fetched,
                             db_fetch_batch_stats_t  *out_stats)
{
    mssql_cursor_t *cur = (mssql_cursor_t *)cursor;

    if (out_stats) memset(out_stats, 0, sizeof(*out_stats));
    if (out_rs) *out_rs = NULL;
    if (out_rows_fetched) *out_rows_fetched = 0;
    if (!cur || !out_rs || !out_rows_fetched) return -1;

    g_last_error[0] = '\0';
    if (cur->buffered)                       /* Stage 6: a procedure result */
    {
        if (cur->next >= cur->nbat) return 0;
        int b = cur->next++;
        *out_rs           = cur->bat[b];
        *out_rows_fetched = cur->bat_rows[b];
        cur->bat[b]       = NULL;
        if (out_stats) *out_stats = cur->bat_stats[b];
        return 0;
    }
    if (cur->exhausted) return 0;
    if (cur->has_lob) return ms_fetch_lob_batch(cur, out_rs, out_rows_fetched, out_stats);

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

/* ================================================================== */
/*  Stage 6 - a procedure's result sets (X3, X4)                       */
/* ================================================================== */

int mssql_select_buffer_result(oci_context_t *ctx, void *stmt, const char *what,
                               int batch, int max_rows, db_select_cursor_t **out)
{
    g_last_error[0] = '\0';
    if (out) *out = NULL;
    if (!ctx || !stmt || !out)
    {
        ms_fail(ctx, __func__, what, "buffer_result: invalid arguments");
        return -1;
    }

    SQLHSTMT        st  = (SQLHSTMT)stmt;
    mssql_cursor_t *cur = calloc(1, sizeof(*cur));
    if (!cur)
    {
        ms_fail(ctx, __func__, what, "buffer_result: out of memory");
        return -1;
    }
    cur->ctx           = ctx;
    cur->stmt          = st;
    cur->stmt_borrowed = 1;
    cur->batch         = batch > 0 ? batch : 1;

    int         rc = -1, total = 0, cap = 0;
    SQLSMALLINT ncols = 0;
    if (!SQL_SUCCEEDED(SQLNumResultCols(st, &ncols)) || ncols <= 0)
    {
        ms_fail_stmt(ctx, __func__, what, st, "SQLNumResultCols");
        goto Done;
    }
    if (ms_setup_columns(cur, what, ncols, &cur->cols) != 0) goto Done;
    if (cur->has_lob) lob_output_tag(cur->lob_tag, sizeof(cur->lob_tag));

    for (;;)
    {
        resultset_t           *rs = NULL;
        int                    rows = 0;
        db_fetch_batch_stats_t stats;
        if (mssql_select_fetch_batch((db_select_cursor_t *)cur, &rs, &rows, &stats) != 0)
            goto Done;                       /* logged by fetch_batch */
        if (rows == 0) break;
        total += rows;
        if (max_rows > 0 && total > max_rows)
        {
            resultset_free(rs);
            ms_fail(ctx, __func__, what,
                    "a procedure result set has more than %d rows "
                    "(query_max_record_count) - the call is failed, not cut short",
                    max_rows);
            goto Done;
        }
        if (cur->nbat == cap)
        {
            int ncap = cap ? cap * 2 : 8;
            resultset_t **nb = realloc(cur->bat, (size_t)ncap * sizeof(*nb));
            if (nb) cur->bat = nb;
            int *nr = realloc(cur->bat_rows, (size_t)ncap * sizeof(*nr));
            if (nr) cur->bat_rows = nr;
            db_fetch_batch_stats_t *ns = realloc(cur->bat_stats, (size_t)ncap * sizeof(*ns));
            if (ns) cur->bat_stats = ns;
            if (!nb || !nr || !ns)
            {
                resultset_free(rs);
                ms_fail(ctx, __func__, what, "buffer_result: out of memory (%d rows)", total);
                goto Done;
            }
            cap = ncap;
        }
        cur->bat[cur->nbat]       = rs;
        cur->bat_rows[cur->nbat]  = rows;
        cur->bat_stats[cur->nbat] = stats;
        cur->nbat++;
    }
    rc = 0;

Done:
    /* The statement goes on to the next result: no bound columns or
     * row array may be left on it. */
    SQLFreeStmt(st, SQL_UNBIND);
    if (!cur->has_lob)
    {
        SQLSetStmtAttr(st, SQL_ATTR_ROWS_FETCHED_PTR, NULL, 0);
        SQLSetStmtAttr(st, SQL_ATTR_ROW_STATUS_PTR, NULL, 0);
        SQLSetStmtAttr(st, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER)(SQLULEN)1, 0);
    }
    cur->stmt = SQL_NULL_HSTMT;
    for (int c = 0; c < cur->ncols; c++)      /* fetch buffers: no longer needed */
    {
        if (cur->bufs) { free(cur->bufs[c]); cur->bufs[c] = NULL; }
        if (cur->inds) { free(cur->inds[c]); cur->inds[c] = NULL; }
    }
    if (rc != 0)
    {
        ms_cursor_free(cur);
        return -1;
    }
    cur->buffered = 1;
    cur->next     = 0;
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "procedure result set buffered: %d column(s), %d row(s) in %d batch(es) of %d",
                 cur->ncols, total, cur->nbat, cur->batch);
    *out = (db_select_cursor_t *)cur;
    return 0;
}

int mssql_select_buffered_rows(const db_select_cursor_t *cursor)
{
    const mssql_cursor_t *cur = (const mssql_cursor_t *)cursor;
    int n = 0;
    if (!cur || !cur->buffered) return -1;
    for (int b = 0; b < cur->nbat; b++) n += cur->bat_rows[b];
    return n;
}

int mssql_select_open_buffered(db_select_cursor_t *cursor, db_column_meta_t **out_columns,
                               int *out_column_count, int *out_batch_size)
{
    mssql_cursor_t *cur = (mssql_cursor_t *)cursor;
    if (out_columns)      *out_columns = NULL;
    if (out_column_count) *out_column_count = 0;
    if (out_batch_size)   *out_batch_size = 0;
    if (!cur || !cur->buffered || cur->next != 0 || !out_columns || !out_column_count ||
        !out_batch_size)
        return -1;
    db_column_meta_t *cols = calloc((size_t)cur->ncols, sizeof(*cols));
    if (!cols) return -1;
    memcpy(cols, cur->cols, (size_t)cur->ncols * sizeof(*cols));
    *out_columns      = cols;
    *out_column_count = cur->ncols;
    *out_batch_size   = cur->batch;
    return 0;
}
