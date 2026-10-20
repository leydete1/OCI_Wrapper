/*
 * mssql_dialect.c   (driver_mssql Stage 3, 2026-10-19, points T1/T2)
 *
 * SQL Server's db_dialect_t. See mssql_dialect.h for every form, and
 * db_driver.h (DIALECT) for the contract each hook follows: write into
 * the caller's buffer and return 0, or -1 when the buffer is too small
 * or an argument is invalid; hooks returning const char * return static
 * strings.
 *
 * 5a - ORDER BY inside a wrapped query. SQL Server refuses ORDER BY in a
 * derived table unless TOP or OFFSET is also given (error 1033); Oracle
 * accepts it. mssql_sql_shape() finds an ORDER BY at the top level of
 * the user's SQL, and then:
 *   count_rows_sql  appends " OFFSET 0 ROWS" to the inner query (order
 *                   does not matter for a count);
 *   row_limit_sql   does not wrap: it appends
 *                   " OFFSET 0 ROWS FETCH NEXT n ROWS ONLY", which keeps
 *                   the user's order guaranteed (an outer TOP over an
 *                   ordered derived table does not).
 * A query that already has a top-level OFFSET after its ORDER BY, or
 * starts SELECT [ALL|DISTINCT] TOP, is valid inside a derived table and
 * is wrapped as written.
 *
 * A query ending in a -- comment would swallow anything appended on the
 * same line, so in that case the appended text starts on a new line.
 *
 * 5e - every column of a derived table needs a distinct name on SQL
 * Server. The wrappers here do not rewrite the user's select list (D6);
 * mssql_select.c turns errors 8155/8156 into a message asking for an
 * alias.
 *
 * Never includes an Oracle header.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "mssql_dialect.h"

/* snprintf into out; 0 on success, -1 on NULL/overflow. */
static int ms_fmt(char *out, size_t out_max, const char *fmt, ...)
{
    if (!out || out_max == 0) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, out_max, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= out_max)
    {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

/* ================================================================== */
/*  Type names                                                          */
/* ================================================================== */

/* Length of the leading type word ("DATETIME2(7)" -> 9). */
static size_t ms_base_len(const char *s)
{
    size_t n = 0;
    while (s[n] && s[n] != '(' && !isspace((unsigned char)s[n]))
        n++;
    return n;
}

static int ms_base_is(const char *s, size_t n, const char *name)
{
    return strlen(name) == n && strncasecmp(s, name, n) == 0;
}

/* "(MAX)" right after the base name, spaces allowed. */
static int ms_has_max(const char *after_base)
{
    const char *p = after_base;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '(') return 0;
    p++;
    while (isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, "MAX", 3) != 0) return 0;
    p += 3;
    while (isspace((unsigned char)*p)) p++;
    return *p == ')';
}

/* ================================================================== */
/*  5a scanner                                                          */
/* ================================================================== */

static int ms_word_char(char c)
{
    unsigned char u = (unsigned char)c;
    return isalnum(u) || c == '_' || c == '@' || c == '#' || c == '$' || u >= 0x80;
}

int mssql_sql_shape(const char *sql, int *order_by, int *offset,
                    int *starts_top, int *ends_in_line_comment)
{
    if (order_by)             *order_by = 0;
    if (offset)               *offset = 0;
    if (starts_top)           *starts_top = 0;
    if (ends_in_line_comment) *ends_in_line_comment = 0;
    if (!sql) return -1;

    int depth        = 0;
    int after_order  = 0;   /* last significant top-level token was ORDER */
    int seen_order   = 0;
    int seen_offset  = 0;
    int offset_pending = 0; /* top-level OFFSET seen, waiting for ROWS   */
    int after_by     = 0;   /* last significant token was ORDER BY's BY */
    int top_words    = 0;   /* top-level words seen so far               */
    int first_select = 0;   /* first top-level word was SELECT           */
    int quantifier   = 0;   /* second top-level word was ALL / DISTINCT  */
    int top          = 0;
    int line_comment_at_end = 0;

    const char *p = sql;
    while (*p)
    {
        char c = *p;

        if (isspace((unsigned char)c)) { p++; continue; }

        /* -- comment, to end of line */
        if (c == '-' && p[1] == '-')
        {
            p += 2;
            while (*p && *p != '\n') p++;
            if (!*p) { line_comment_at_end = 1; break; }
            continue;
        }

        /* block comment - T-SQL allows them nested */
        if (c == '/' && p[1] == '*')
        {
            int level = 1;
            p += 2;
            while (*p && level > 0)
            {
                if (p[0] == '/' && p[1] == '*')      { level++; p += 2; }
                else if (p[0] == '*' && p[1] == '/') { level--; p += 2; }
                else p++;
            }
            continue;
        }

        line_comment_at_end = 0;

        /* quoted text - the closing quote doubled is an escaped quote */
        if (c == '\'' || c == '"' || c == '[')
        {
            char close = (c == '[') ? ']' : c;
            p++;
            while (*p)
            {
                if (*p == close)
                {
                    if (p[1] == close) { p += 2; continue; }
                    p++;
                    break;
                }
                p++;
            }
            after_order = after_by = 0;
            continue;
        }

        if (c == '(') { depth++; after_order = after_by = 0; p++; continue; }
        if (c == ')') { if (depth > 0) depth--; after_order = after_by = 0; p++; continue; }

        if (ms_word_char(c))
        {
            const char *w = p;
            while (*p && ms_word_char(*p)) p++;
            size_t n = (size_t)(p - w);

            /* N'...' - the N is part of the literal, not a word */
            if (n == 1 && (w[0] == 'N' || w[0] == 'n') && *p == '\'')
                continue;

            if (depth == 0)
            {
                /* SELECT TOP ... / SELECT ALL|DISTINCT TOP ... */
                if (top_words == 0)
                    first_select = ms_base_is(w, n, "SELECT");
                else if (top_words == 1 && first_select)
                {
                    if (ms_base_is(w, n, "TOP"))
                        top = 1;
                    else if (ms_base_is(w, n, "ALL") || ms_base_is(w, n, "DISTINCT"))
                        quantifier = 1;
                }
                else if (top_words == 2 && quantifier && ms_base_is(w, n, "TOP"))
                    top = 1;

                /* OFFSET/FETCH: OFFSET after the ORDER BY list (not the
                 * word straight after BY - that is a column called
                 * OFFSET), confirmed by a later ROW or ROWS */
                if (offset_pending &&
                    (ms_base_is(w, n, "ROW") || ms_base_is(w, n, "ROWS")))
                    seen_offset = 1;
                if (seen_order && !after_by && ms_base_is(w, n, "OFFSET"))
                    offset_pending = 1;

                after_by = after_order && ms_base_is(w, n, "BY");
                if (after_by)
                    seen_order = 1;

                after_order = ms_base_is(w, n, "ORDER");
                top_words++;
            }
            continue;
        }

        /* any other punctuation */
        after_order = after_by = 0;
        p++;
    }

    if (order_by)            *order_by = seen_order;
    if (offset)               *offset = seen_offset;
    if (starts_top)           *starts_top = top;
    if (ends_in_line_comment) *ends_in_line_comment = line_comment_at_end;
    return 0;
}

/* ================================================================== */
/*  Hooks                                                               */
/* ================================================================== */

static int ms_bind_placeholder(int pos, char *out, size_t out_max)
{
    if (pos < 1) return -1;
    return ms_fmt(out, out_max, "?");
}

static int ms_value_expr(const char *dtype, const char *operand,
                         char *out, size_t out_max)
{
    if (!operand) return -1;
    if (!dtype) dtype = "";
    while (isspace((unsigned char)*dtype)) dtype++;
    size_t n = ms_base_len(dtype);

    if (ms_base_is(dtype, n, "DATETIME2") || ms_base_is(dtype, n, "DATETIME") ||
        ms_base_is(dtype, n, "SMALLDATETIME"))
        return ms_fmt(out, out_max, "CAST(%s AS datetime2(7))", operand);
    if (ms_base_is(dtype, n, "DATE"))
        return ms_fmt(out, out_max, "CAST(%s AS date)", operand);
    if (ms_base_is(dtype, n, "DATETIMEOFFSET"))
        return ms_fmt(out, out_max, "CAST(%s AS datetimeoffset(7))", operand);
    if (ms_base_is(dtype, n, "TIME"))
        return ms_fmt(out, out_max, "CAST(%s AS time(7))", operand);

    return ms_fmt(out, out_max, "%s", operand);
}

/* Must agree with db_type_lob_kind() (db_type_class.c): a column gets
 * a placeholder exactly when it is written after the statement. */
static const char *ms_lob_placeholder(const char *dtype)
{
    if (!dtype) return NULL;
    while (isspace((unsigned char)*dtype)) dtype++;
    size_t n = ms_base_len(dtype);

    if (ms_has_max(dtype + n))
    {
        if (ms_base_is(dtype, n, "VARBINARY")) return "0x";
        if (ms_base_is(dtype, n, "VARCHAR"))   return "''";
        if (ms_base_is(dtype, n, "NVARCHAR"))  return "N''";
        return NULL;
    }
    if (ms_base_is(dtype, n, "IMAGE")) return "0x";
    if (ms_base_is(dtype, n, "TEXT"))  return "''";
    if (ms_base_is(dtype, n, "NTEXT")) return "N''";
    return NULL;
}

/* Stage 4: OUTPUT inserted.<key> needs the key from describe_table
 * (5c). Until then core logs the failure and fails the statement. */
static int ms_row_locator_clauses(db_dml_kind_t kind,
                                  const db_row_locator_ctx_t *loc, int pos,
                                  char *mid, size_t mid_max,
                                  char *tail, size_t tail_max)
{
    (void)kind; (void)loc; (void)pos;
    if (mid && mid_max)   mid[0] = '\0';
    if (tail && tail_max) tail[0] = '\0';
    return -1;
}

static int ms_row_limit_sql(const char *sql, int max_rows,
                            char *out, size_t out_max)
{
    if (!sql || max_rows < 0) return -1;

    int ob = 0, off = 0, top = 0, elc = 0;
    mssql_sql_shape(sql, &ob, &off, &top, &elc);

    if (ob && !off && !top)
        return ms_fmt(out, out_max, "%s%sOFFSET 0 ROWS FETCH NEXT %d ROWS ONLY",
                      sql, elc ? "\n" : " ", max_rows);

    return ms_fmt(out, out_max, "SELECT TOP (%d) * FROM (%s%s) AS dm_t",
                  max_rows, sql, elc ? "\n" : "");
}

static int ms_count_rows_sql(const char *sql, char *out, size_t out_max)
{
    if (!sql) return -1;

    int ob = 0, off = 0, top = 0, elc = 0;
    mssql_sql_shape(sql, &ob, &off, &top, &elc);

    const char *mid;
    if (ob && !off && !top) mid = elc ? "\nOFFSET 0 ROWS" : " OFFSET 0 ROWS";
    else                    mid = elc ? "\n" : "";

    return ms_fmt(out, out_max,
                  "SELECT COUNT(*) FROM (SELECT 1 AS dm_c FROM (%s%s) AS dm_a) AS dm_b",
                  sql, mid);
}

/* D7: the server clock, in the container's local time zone. */
static const char *ms_now_expr(void)
{
    return "SYSDATETIME()";
}

/* DATEADD takes an int number: a bigint expression (OCI_SESSION.TTL_SECONDS
 * is bigint) fails with error 8116, so the seconds are cast to int. */
static int ms_add_seconds_expr(const char *base_expr, const char *seconds_expr,
                               char *out, size_t out_max)
{
    if (!base_expr || !seconds_expr) return -1;
    return ms_fmt(out, out_max, "DATEADD(second, CAST(%s AS int), %s)",
                  seconds_expr, base_expr);
}

/* ODBC call escape; parameters bind by position, so the names only
 * have to be present (same validation as the Oracle hook). */
static int ms_procedure_call_sql(const char *proc_name,
                                 const char * const *param_names, int count,
                                 char *out, size_t out_max)
{
    if (!proc_name || count < 0 || (count > 0 && !param_names)) return -1;
    if (!out || out_max == 0) return -1;

    if (count == 0)
        return ms_fmt(out, out_max, "{CALL %s}", proc_name);

    size_t used = 0;
    int n = snprintf(out, out_max, "{CALL %s(", proc_name);
    if (n < 0 || (size_t)n >= out_max) { out[0] = '\0'; return -1; }
    used = (size_t)n;

    for (int i = 0; i < count; i++)
    {
        if (!param_names[i]) { out[0] = '\0'; return -1; }
        n = snprintf(out + used, out_max - used, "%s?", i > 0 ? ", " : "");
        if (n < 0 || (size_t)n >= out_max - used) { out[0] = '\0'; return -1; }
        used += (size_t)n;
    }

    n = snprintf(out + used, out_max - used, ")}");
    if (n < 0 || (size_t)n >= out_max - used) { out[0] = '\0'; return -1; }
    return 0;
}

const db_dialect_t mssql_dialect = {
    .bind_placeholder    = ms_bind_placeholder,
    .value_expr          = ms_value_expr,
    .lob_placeholder     = ms_lob_placeholder,
    .row_locator_clauses = ms_row_locator_clauses,
    .row_limit_sql       = ms_row_limit_sql,
    .count_rows_sql      = ms_count_rows_sql,
    .now_expr            = ms_now_expr,
    .add_seconds_expr    = ms_add_seconds_expr,
    .procedure_call_sql  = ms_procedure_call_sql,
};

const db_dialect_t *mssql_dialect_get(void)
{
    return &mssql_dialect;
}
