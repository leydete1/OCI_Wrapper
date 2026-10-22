/*
 * mssql_proc.c   (driver_mssql Stage 6, 2026-10-21, points X1-X6;
 *                 Fix 1-2, 2026-10-22: X6 amended)
 *
 * Stored procedures through ODBC - see mssql_proc.h for what each call
 * does and why the result sets are read before the OUT values.
 *
 * Never includes an Oracle header.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sql.h>
#include <sqlext.h>

#include "mssql_proc.h"
#include "mssql_select.h"        /* mssql_select_buffer_result() ...     */
#include "mssql_text.h"          /* mssql_prepare_utf8()                  */
#include "MSSQL_Pool.h"          /* mssql_diag()                          */
#include "logger.h"

#ifndef SQL_SS_LENGTH_UNLIMITED
#define SQL_SS_LENGTH_UNLIMITED 0       /* (max) */
#endif

#define MS_PROC_CALL_MAX    8192
#define MS_PROC_MAX_RESULTS 10000       /* a runaway procedure, not a real one */

static __thread char g_last_error[2048];

const char *mssql_proc_last_error(void)
{
    return g_last_error;
}

static void px_fail(logger_t *logger, const char *call, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
    if (logger)
        logger_write(logger, LOG_ERROR, "mssql_proc_execute", 0, "%s - call=%s",
                     g_last_error, call ? call : "(none)");
}

static void px_fail_stmt(logger_t *logger, const char *call, SQLHSTMT st, const char *what)
{
    char diag[1536];
    mssql_diag(SQL_HANDLE_STMT, st, diag, sizeof(diag));
    px_fail(logger, call, "%s failed: %s", what, diag[0] ? diag : "(no diagnostic record)");
}

/* ------------------------------------------------------------------ */
/*  UTF-8 <-> UTF-16 (SQLWCHAR is 2 bytes with unixODBC)               */
/* ------------------------------------------------------------------ */

/* UTF-16 code units in a UTF-8 string (a 4-byte sequence is 2 units). */
static size_t px_utf16_units(const char *s)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        if ((*p & 0xC0) == 0x80) continue;
        n += (*p >= 0xF0) ? 2 : 1;
    }
    return n;
}

/* UTF-8 into out (cap code units, terminated). Bytes written (not the
 * terminator), or -1 if it does not fit. Bad sequences become U+FFFD. */
static long px_to_utf16(const char *in, SQLWCHAR *out, size_t cap)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t k = 0;
    while (*s)
    {
        uint32_t cp;
        unsigned c = *s;
        if (c < 0x80)                                 { cp = c; s += 1; }
        else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
                                                      { cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu); s += 2; }
        else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
                                                      { cp = ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) |
                                                             (s[2] & 0x3Fu); s += 3; }
        else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
                 (s[3] & 0xC0) == 0x80)               { cp = ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) |
                                                             ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu); s += 4; }
        else                                          { cp = 0xFFFD; s += 1; }

        if (cp >= 0x10000)
        {
            if (k + 2 >= cap) return -1;
            cp -= 0x10000;
            out[k++] = (SQLWCHAR)(0xD800 + (cp >> 10));
            out[k++] = (SQLWCHAR)(0xDC00 + (cp & 0x3FF));
        }
        else
        {
            if (k + 1 >= cap) return -1;
            out[k++] = (SQLWCHAR)cp;
        }
    }
    out[k] = 0;
    return (long)(k * sizeof(SQLWCHAR));
}

/* nbytes of UTF-16 into out (out_size bytes, terminated). 0, or -1 if
 * it does not fit. A lone surrogate becomes U+FFFD. */
static int px_to_utf8(const SQLWCHAR *in, size_t nbytes, char *out, size_t out_size)
{
    size_t units = nbytes / sizeof(SQLWCHAR), k = 0;
    for (size_t i = 0; i < units; i++)
    {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units &&
            in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF)
        {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00u);
            i++;
        }
        else if (cp >= 0xD800 && cp <= 0xDFFF)
            cp = 0xFFFD;

        unsigned char u[4];
        size_t        n;
        if (cp < 0x80)         { u[0] = (unsigned char)cp; n = 1; }
        else if (cp < 0x800)   { u[0] = (unsigned char)(0xC0 | (cp >> 6));
                                 u[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
        else if (cp < 0x10000) { u[0] = (unsigned char)(0xE0 | (cp >> 12));
                                 u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                 u[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
        else                   { u[0] = (unsigned char)(0xF0 | (cp >> 18));
                                 u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                                 u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                                 u[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
        if (k + n >= out_size) return -1;
        memcpy(out + k, u, n);
        k += n;
    }
    out[k] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/*  X1 - call text                                                     */
/* ------------------------------------------------------------------ */

/* block is the dialect's "{CALL name}" or "{CALL name(?, ?, ...)}" with
 * one "?" per parameter. Writes it again with a "?" only for the
 * parameters that are not CURSORs. 0 or -1 (unexpected text). */
static int px_call_text(const char *block, const db_proc_param_t *params, int n,
                        char *out, size_t out_max)
{
    static const char head[] = "{CALL ";
    size_t blen = strlen(block);
    if (strncmp(block, head, sizeof(head) - 1) != 0 || blen < sizeof(head) || block[blen - 1] != '}')
        return -1;

    const char *name = block + sizeof(head) - 1;
    const char *open = strchr(name, '(');
    size_t      name_len = open ? (size_t)(open - name) : (size_t)(block + blen - 1 - name);
    if (name_len == 0) return -1;

    /* the parameter list must be exactly n "?" */
    if (n == 0)
    {
        if (open) return -1;
    }
    else
    {
        if (!open) return -1;
        const char *p = open + 1;
        for (int i = 0; i < n; i++)
        {
            if (i > 0) { if (strncmp(p, ", ", 2) != 0) return -1; p += 2; }
            if (*p != '?') return -1;
            p++;
        }
        if (strcmp(p, ")}") != 0) return -1;
    }

    int kept = 0;
    for (int i = 0; i < n; i++)
        if (params[i].type != DB_PROC_TYPE_CURSOR) kept++;

    size_t used = 0;
    int w = snprintf(out, out_max, "{CALL %.*s", (int)name_len, name);
    if (w < 0 || (size_t)w >= out_max) return -1;
    used = (size_t)w;
    for (int k = 0; k < kept; k++)
    {
        w = snprintf(out + used, out_max - used, "%s?", k == 0 ? "(" : ", ");
        if (w < 0 || (size_t)w >= out_max - used) return -1;
        used += (size_t)w;
    }
    w = snprintf(out + used, out_max - used, "%s}", kept ? ")" : "");
    if (w < 0 || (size_t)w >= out_max - used) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  dml_execute_procedure                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    SQLLEN              ind;
    SQLINTEGER          ival;
    SQLWCHAR           *wbuf;       /* OUT / IN_OUT STR                 */
    SQLLEN              wcap;       /* bytes                            */
    db_select_cursor_t *cursor;     /* CURSOR: its result set, or NULL  */
} px_slot_t;

static int px_is_out(const db_proc_param_t *p)
{
    return p->direction == DB_PROC_DIR_OUT || p->direction == DB_PROC_DIR_IN_OUT;
}

int mssql_proc_execute(oci_context_t *ctx, logger_t *logger, db_proc_execute_request_t *req)
{
    g_last_error[0] = '\0';
    if (!ctx || !req || !req->plsql_block || req->param_count < 0 ||
        (req->param_count > 0 && !req->params))
    {
        px_fail(logger, req ? req->plsql_block : NULL,
                "dml_execute_procedure: invalid arguments");
        return -1;
    }
    if (!ctx->svchp)
    {
        px_fail(logger, req->plsql_block,
                "dml_execute_procedure: no SQL Server connection on this session");
        return -1;
    }

    int              n      = req->param_count;
    db_proc_param_t *params = req->params;
    for (int i = 0; i < n; i++)
    {
        params[i].out_is_null       = 0;
        params[i].out_cursor_handle = NULL;
        if (!params[i].name)
        {
            px_fail(logger, req->plsql_block, "parameter %d has no name", i + 1);
            return -1;
        }
        if (params[i].type == DB_PROC_TYPE_CURSOR && params[i].direction != DB_PROC_DIR_OUT)
        {
            px_fail(logger, req->plsql_block, "CURSOR parameter %s must be OUT", params[i].name);
            return -1;
        }
        if (params[i].type == DB_PROC_TYPE_STR && px_is_out(&params[i]) &&
            (!params[i].out_value || params[i].out_value_size < 2))
        {
            px_fail(logger, req->plsql_block, "OUT parameter %s has no out_value buffer",
                    params[i].name);
            return -1;
        }
    }

    char call[MS_PROC_CALL_MAX];
    if (px_call_text(req->plsql_block, params, n, call, sizeof(call)) != 0)
    {
        px_fail(logger, req->plsql_block,
                "dml_execute_procedure: unexpected call text (expected the SQL Server "
                "dialect's {CALL name(?, ...)} with one ? per parameter)");
        return -1;
    }

    int ncursor = 0, nbind = 0;
    for (int i = 0; i < n; i++)
    {
        if (params[i].type == DB_PROC_TYPE_CURSOR) ncursor++;
        else nbind++;
    }
    logger_write(logger, LOG_INFO, __func__, 0,
                 "procedure call: %s - %d parameter(s) bound, %d CURSOR(s)", call, nbind, ncursor);

    int        rc = -1, autocommit_on = 0;
    SQLHSTMT   st = SQL_NULL_HSTMT;
    px_slot_t *sl = calloc(n > 0 ? (size_t)n : 1, sizeof(*sl));
    if (!sl)
    {
        px_fail(logger, call, "dml_execute_procedure: out of memory");
        return -1;
    }

    SQLRETURN r = SQLAllocHandle(SQL_HANDLE_STMT, (SQLHDBC)ctx->svchp, &st);
    if (!SQL_SUCCEEDED(r))
    {
        char diag[1024];
        mssql_diag(SQL_HANDLE_DBC, ctx->svchp, diag, sizeof(diag));
        px_fail(logger, call, "SQLAllocHandle(STMT) failed: %s",
                diag[0] ? diag : "(no diagnostic record)");
        st = SQL_NULL_HSTMT;
        goto Done;
    }

    /* X6 (Fix 2): with no managed transaction the procedure owns its
     * transaction, so it is called in autocommit mode. In manual-commit
     * mode the statement that calls it opens a transaction (ODBC: "the
     * command sent after SQLEndTran starts the next transaction"), so a
     * procedure that commits would always end with error 266. First the
     * connection's open transaction is rolled back - only reads can be
     * pending, every write outside a managed transaction having been
     * committed by its own module - so that switching autocommit on (which
     * commits whatever is open) commits nothing. Manual-commit is restored
     * after the call, success or failure (Done below). Not done when the
     * connection is configured for autocommit, or inside a managed
     * transaction, where nothing is touched. */
    if (!ctx->active_tx && !(ctx->ini && ctx->ini->autocommit_mode))
    {
        SQLRETURN er = SQLEndTran(SQL_HANDLE_DBC, (SQLHDBC)ctx->svchp, SQL_ROLLBACK);
        if (SQL_SUCCEEDED(er))
            er = SQLSetConnectAttr((SQLHDBC)ctx->svchp, SQL_ATTR_AUTOCOMMIT,
                                   (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0);
        if (!SQL_SUCCEEDED(er))
        {
            char diag[1024];
            mssql_diag(SQL_HANDLE_DBC, ctx->svchp, diag, sizeof(diag));
            px_fail(logger, call, "could not end the open read transaction and switch to "
                    "autocommit before the call: %s", diag[0] ? diag : "(no diagnostic record)");
            goto Done;
        }
        autocommit_on = 1;
        logger_write(logger, LOG_DEBUG, __func__, 0,
                     "no managed transaction - read transaction rolled back, call runs in autocommit");
    }

    r = mssql_prepare_utf8(st, call);
    if (!SQL_SUCCEEDED(r))
    {
        px_fail_stmt(logger, call, st, "SQLPrepare");
        goto Done;
    }

    /* ---- X2: binds, by position, CURSORs skipped ---- */
    SQLUSMALLINT pos = 0;
    for (int i = 0; i < n; i++)
    {
        db_proc_param_t *p = &params[i];
        px_slot_t       *s = &sl[i];
        if (p->type == DB_PROC_TYPE_CURSOR) continue;
        pos++;
        char what[192];
        snprintf(what, sizeof(what), "SQLBindParameter(%u, %.128s)", (unsigned)pos, p->name);

        if (p->direction == DB_PROC_DIR_IN)
        {
            const char *v     = p->in_value;
            size_t      units = v ? px_utf16_units(v) : 0;
            SQLULEN     colsz = units > 4000 ? SQL_SS_LENGTH_UNLIMITED : (units ? units : 1);
            s->ind = v ? (SQLLEN)strlen(v) : SQL_NULL_DATA;
            r = SQLBindParameter(st, pos, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR, colsz, 0,
                                 (SQLPOINTER)v, v ? (SQLLEN)strlen(v) : 0, &s->ind);
        }
        else if (p->type == DB_PROC_TYPE_INT)
        {
            s->ind = sizeof(SQLINTEGER);
            if (p->direction == DB_PROC_DIR_IN_OUT)
            {
                if (!p->in_value)
                    s->ind = SQL_NULL_DATA;
                else
                {
                    char *end = NULL;
                    errno = 0;
                    long  v = strtol(p->in_value, &end, 10);
                    if (errno || end == p->in_value || *end || v < INT_MIN || v > INT_MAX)
                    {
                        px_fail(logger, call, "IN_OUT parameter %s: '%.64s' is not a whole number "
                                "in the int range", p->name, p->in_value);
                        goto Done;
                    }
                    s->ival = (SQLINTEGER)v;
                }
            }
            r = SQLBindParameter(st, pos,
                                 p->direction == DB_PROC_DIR_OUT ? SQL_PARAM_OUTPUT
                                                                 : SQL_PARAM_INPUT_OUTPUT,
                                 SQL_C_SLONG, SQL_INTEGER, 0, 0, &s->ival, 0, &s->ind);
        }
        else
        {
            /* STR OUT / IN_OUT: nvarchar(max), a buffer as large as the
             * caller's out_value can take back (one UTF-16 unit per
             * UTF-8 byte at most). */
            size_t units = p->out_value_size;
            s->wcap = (SQLLEN)((units + 1) * sizeof(SQLWCHAR));
            s->wbuf = calloc(units + 1, sizeof(SQLWCHAR));
            if (!s->wbuf)
            {
                px_fail(logger, call, "dml_execute_procedure: out of memory (parameter %s)", p->name);
                goto Done;
            }
            s->ind = 0;
            if (p->direction == DB_PROC_DIR_IN_OUT)
            {
                if (!p->in_value)
                    s->ind = SQL_NULL_DATA;
                else
                {
                    long bytes = px_to_utf16(p->in_value, s->wbuf, units + 1);
                    if (bytes < 0)
                    {
                        px_fail(logger, call, "IN_OUT parameter %s: value is longer than its "
                                "%zu-byte buffer", p->name, p->out_value_size - 1);
                        goto Done;
                    }
                    s->ind = (SQLLEN)bytes;
                }
            }
            r = SQLBindParameter(st, pos,
                                 p->direction == DB_PROC_DIR_OUT ? SQL_PARAM_OUTPUT
                                                                 : SQL_PARAM_INPUT_OUTPUT,
                                 SQL_C_WCHAR, SQL_WVARCHAR, SQL_SS_LENGTH_UNLIMITED, 0,
                                 s->wbuf, s->wcap, &s->ind);
        }
        if (!SQL_SUCCEEDED(r))
        {
            px_fail_stmt(logger, call, st, what);
            goto Done;
        }
    }

    r = SQLExecute(st);
    if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA)
    {
        px_fail_stmt(logger, call, st, "SQLExecute");
        goto Done;
    }

    /* ---- X3/X4: every result, in order; OUT values come after ---- */
    int batch    = ctx->ini && ctx->ini->query_fetch_batch_size > 0 ? ctx->ini->query_fetch_batch_size : 1;
    int max_rows = ctx->ini ? ctx->ini->query_max_record_count : 0;
    int next_cursor = 0, set_no = 0;
    for (int guard = 0; ; guard++)
    {
        if (guard >= MS_PROC_MAX_RESULTS)
        {
            px_fail(logger, call, "more than %d results from one procedure call", MS_PROC_MAX_RESULTS);
            goto Done;
        }
        SQLSMALLINT ncols = 0;
        r = SQLNumResultCols(st, &ncols);
        if (!SQL_SUCCEEDED(r))
        {
            px_fail_stmt(logger, call, st, "SQLNumResultCols");
            goto Done;
        }
        if (ncols > 0)
        {
            set_no++;
            int target = -1;
            for (int i = 0, k = 0; i < n; i++)
                if (params[i].type == DB_PROC_TYPE_CURSOR && k++ == next_cursor) { target = i; break; }

            if (target >= 0)
            {
                if (mssql_select_buffer_result(ctx, st, call, batch, max_rows,
                                               &sl[target].cursor) != 0)
                {
                    px_fail(logger, call, "result set %d (CURSOR %s): %s", set_no,
                            params[target].name, mssql_select_last_error());
                    goto Done;
                }
                logger_write(logger, LOG_INFO, __func__, 0,
                             "result set %d -> CURSOR %s: %d column(s), %d row(s)", set_no,
                             params[target].name, (int)ncols,
                             mssql_select_buffered_rows(sl[target].cursor));
                next_cursor++;
            }
            else
                logger_write(logger, LOG_WARN, __func__, 0,
                             "result set %d discarded - the request has %d CURSOR parameter(s) "
                             "and the procedure returned more result sets", set_no, ncursor);
        }

        r = SQLMoreResults(st);          /* also discards unread rows */
        if (r == SQL_NO_DATA) break;
        if (!SQL_SUCCEEDED(r))
        {
            px_fail_stmt(logger, call, st, "SQLMoreResults");
            goto Done;
        }
    }

    /* ---- OUT values (only now filled in by the driver) ---- */
    for (int i = 0; i < n; i++)
    {
        db_proc_param_t *p = &params[i];
        px_slot_t       *s = &sl[i];

        if (p->type == DB_PROC_TYPE_CURSOR)
        {
            if (!s->cursor)
            {
                p->out_is_null = 1;
                logger_write(logger, LOG_INFO, __func__, 0,
                             "CURSOR %s: no result set returned for it - NULL (empty)", p->name);
            }
            continue;
        }
        if (!px_is_out(p)) continue;

        if (s->ind == SQL_NULL_DATA)
        {
            p->out_is_null = 1;
            if (p->type == DB_PROC_TYPE_INT) p->out_int = 0;
            else p->out_value[0] = '\0';
            continue;
        }
        if (p->type == DB_PROC_TYPE_INT)
        {
            p->out_int = (int)s->ival;
            continue;
        }
        if (s->ind == SQL_NO_TOTAL || s->ind < 0 || s->ind > s->wcap - (SQLLEN)sizeof(SQLWCHAR))
        {
            px_fail(logger, call, "OUT parameter %s: the value (%ld bytes as UTF-16) does not fit "
                    "its %zu-byte buffer - call failed, value not cut short", p->name,
                    (long)s->ind, p->out_value_size - 1);
            goto Done;
        }
        if (px_to_utf8(s->wbuf, (size_t)s->ind, p->out_value, p->out_value_size) != 0)
        {
            px_fail(logger, call, "OUT parameter %s: the value does not fit its %zu-byte buffer "
                    "as UTF-8 - call failed, value not cut short", p->name, p->out_value_size - 1);
            goto Done;
        }
    }

    /* ---- hand the held result sets over ---- */
    for (int i = 0; i < n; i++)
        if (sl[i].cursor)
        {
            params[i].out_cursor_handle = sl[i].cursor;
            sl[i].cursor = NULL;
        }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "procedure call OK: %d result set(s), %d to CURSOR parameters", set_no, next_cursor);
    rc = 0;

Done:
    for (int i = 0; i < n; i++)
    {
        if (sl[i].cursor) mssql_select_close(sl[i].cursor);
        free(sl[i].wbuf);
    }
    free(sl);
    if (st) SQLFreeHandle(SQL_HANDLE_STMT, st);

    /* Fix 2: back to manual-commit, whatever happened. A connection left
     * in autocommit would silently change every later request on it, so
     * a failure here fails the call and is logged as such. */
    if (autocommit_on)
    {
        SQLRETURN ar = SQLSetConnectAttr((SQLHDBC)ctx->svchp, SQL_ATTR_AUTOCOMMIT,
                                         (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
        if (!SQL_SUCCEEDED(ar))
        {
            char diag[1024];
            mssql_diag(SQL_HANDLE_DBC, ctx->svchp, diag, sizeof(diag));
            px_fail(logger, call, "could not switch the connection back to manual-commit "
                    "after the call - it is left in autocommit: %s",
                    diag[0] ? diag : "(no diagnostic record)");
            if (rc == 0)
                for (int i = 0; i < n; i++)
                    if (params[i].out_cursor_handle)
                        mssql_select_close(params[i].out_cursor_handle);
            rc = -1;
        }
    }

    if (rc != 0)
        for (int i = 0; i < n; i++)
            params[i].out_cursor_handle = NULL;
    return rc;
}

/* ------------------------------------------------------------------ */
/*  select_open_from_cursor / cursor_handle_free                       */
/* ------------------------------------------------------------------ */

int mssql_proc_open_cursor(oci_context_t *ctx, void *cursor_handle,
                           db_select_cursor_t **out_cursor, db_column_meta_t **out_columns,
                           int *out_column_count, int *out_batch_size)
{
    if (out_cursor) *out_cursor = NULL;
    if (!cursor_handle || !out_cursor)
    {
        /* Ownership passes at the call (db_driver.h): released on failure. */
        mssql_proc_cursor_free(cursor_handle);
        px_fail(ctx ? ctx->procedure_logger : NULL, NULL,
                "select_open_from_cursor: invalid arguments");
        return -1;
    }
    db_select_cursor_t *cur = (db_select_cursor_t *)cursor_handle;
    if (mssql_select_open_buffered(cur, out_columns, out_column_count, out_batch_size) != 0)
    {
        mssql_proc_cursor_free(cursor_handle);
        px_fail(ctx ? ctx->procedure_logger : NULL, NULL,
                "select_open_from_cursor: not a procedure result handle, already opened, "
                "or out of memory");
        return -1;
    }
    *out_cursor = cur;
    return 0;
}

void mssql_proc_cursor_free(void *cursor_handle)
{
    if (cursor_handle) mssql_select_close((db_select_cursor_t *)cursor_handle);
}
