/*
 * mssql_metadata.c   (driver_mssql Stage 3, 2026-10-19, point T7)
 *
 * describe_table for SQL Server - see mssql_metadata.h for what each
 * col_metadata_t field holds.
 *
 * Two statements, both with the names bound (never concatenated):
 *   owner lookup (only when req->owner is empty) - INFORMATION_SCHEMA.TABLES
 *   columns - INFORMATION_SCHEMA.COLUMNS joined to sys.columns, with
 *             sys.default_constraints, sys.identity_columns and
 *             sys.computed_columns for the default text
 * Values are read with SQLGetData (no bound buffers), one row at a time.
 *
 * Never includes an Oracle header.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <sql.h>
#include <sqlext.h>

#include "mssql_metadata.h"
#include "MSSQL_Pool.h"          /* mssql_diag()                         */
#include "db_type_class.h"
#include "logger.h"

static const char *SQL_OWNER =
    "SELECT TOP (1) TABLE_SCHEMA "
    "FROM   INFORMATION_SCHEMA.TABLES "
    "WHERE  TABLE_NAME = ? AND TABLE_TYPE = 'BASE TABLE' "
    "ORDER  BY CASE WHEN TABLE_SCHEMA = SCHEMA_NAME() THEN 0 ELSE 1 END, TABLE_SCHEMA";

static const char *SQL_COLUMNS =
    "SELECT ic.COLUMN_NAME, "
    "       ic.DATA_TYPE, "
    "       ic.CHARACTER_MAXIMUM_LENGTH, "
    "       ic.NUMERIC_PRECISION, "
    "       ic.NUMERIC_SCALE, "
    "       ic.DATETIME_PRECISION, "
    "       ic.IS_NULLABLE, "
    "       CAST(c.max_length AS int), "
    "       CAST(LEFT(CASE "
    "           WHEN c.is_identity = 1 THEN N'IDENTITY(' "
    "                + CAST(idc.seed_value AS nvarchar(40)) + N',' "
    "                + CAST(idc.increment_value AS nvarchar(40)) + N')' "
    "           WHEN c.is_computed = 1 THEN N'AS ' + cc.definition "
    "           WHEN ic.DATA_TYPE = N'timestamp' THEN N'(rowversion)' "
    "           ELSE COALESCE(dc.definition, N'') END, 1000) AS nvarchar(1000)) "
    "FROM   INFORMATION_SCHEMA.COLUMNS ic "
    "JOIN   sys.columns c "
    "       ON  c.object_id = OBJECT_ID(QUOTENAME(ic.TABLE_SCHEMA) + N'.' + QUOTENAME(ic.TABLE_NAME)) "
    "       AND c.name = ic.COLUMN_NAME "
    "LEFT   JOIN sys.default_constraints dc ON dc.object_id = c.default_object_id "
    "LEFT   JOIN sys.identity_columns idc   ON idc.object_id = c.object_id AND idc.column_id = c.column_id "
    "LEFT   JOIN sys.computed_columns cc    ON cc.object_id = c.object_id AND cc.column_id = c.column_id "
    "WHERE  ic.TABLE_SCHEMA = ? AND ic.TABLE_NAME = ? "
    "ORDER  BY ic.ORDINAL_POSITION";

static void upper_inplace(char *s)
{
    for (; s && *s; s++) *s = (char)toupper((unsigned char)*s);
}

static void meta_fail(oci_context_t *ctx, SQLHSTMT stmt, const char *what,
                      const char *table, const char *owner)
{
    char diag[1536] = "";
    if (stmt) mssql_diag(SQL_HANDLE_STMT, stmt, diag, sizeof(diag));
    logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                 "describe_table %s.%s: %s failed: %s", owner, table, what,
                 diag[0] ? diag : "(no diagnostic record)");
}

/* Cuts an incomplete UTF-8 sequence off the end of s. */
static void utf8_trim_partial(char *s)
{
    size_t n = strlen(s);
    if (n == 0) return;
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    unsigned char lead = (unsigned char)s[i];
    size_t need = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (i + need > n) s[i] = '\0';
}

/* Text column into buf (always NUL-terminated). A value longer than the
 * buffer is cut at a UTF-8 character boundary - only the default text
 * can be that long, and col_metadata_t.data_default holds 511 bytes,
 * as on Oracle. Returns 0 (is_null set), or -1. */
static int get_text(SQLHSTMT stmt, int col, char *buf, size_t size, int *is_null)
{
    SQLLEN ind = 0;
    buf[0] = '\0';
    SQLRETURN r = SQLGetData(stmt, (SQLUSMALLINT)col, SQL_C_CHAR, buf, (SQLLEN)size, &ind);
    if (!SQL_SUCCEEDED(r)) return -1;
    *is_null = (ind == SQL_NULL_DATA);
    if (*is_null) buf[0] = '\0';
    buf[size - 1] = '\0';
    if (r == SQL_SUCCESS_WITH_INFO && !*is_null)
        utf8_trim_partial(buf);   /* truncated: no half character at the end */
    return 0;
}

static int get_int(SQLHSTMT stmt, int col, int *value, int *is_null)
{
    SQLINTEGER v = 0;
    SQLLEN ind = 0;
    SQLRETURN r = SQLGetData(stmt, (SQLUSMALLINT)col, SQL_C_SLONG, &v, 0, &ind);
    if (!SQL_SUCCEEDED(r)) return -1;
    *is_null = (ind == SQL_NULL_DATA);
    *value   = *is_null ? 0 : (int)v;
    return 0;
}

static int bind_name(SQLHSTMT stmt, int pos, const char *name, SQLLEN *ind)
{
    *ind = (SQLLEN)strlen(name);
    SQLULEN colsize = *ind > 0 ? (SQLULEN)*ind : 1;
    return SQL_SUCCEEDED(SQLBindParameter(stmt, (SQLUSMALLINT)pos, SQL_PARAM_INPUT,
                                          SQL_C_CHAR, SQL_WVARCHAR, colsize, 0,
                                          (SQLPOINTER)name, *ind, ind)) ? 0 : -1;
}

static int is_base(const char *type, const char *name)
{
    return strcasecmp(type, name) == 0;
}

/* Fills req->owner. 0 found, 1 not found, -1 error. */
static int resolve_owner(oci_context_t *ctx, SQLHDBC hdbc, metadata_request_t *req)
{
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    SQLLEN   ind  = 0;
    int      rc   = -1;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &stmt)))
    {
        meta_fail(ctx, SQL_NULL_HSTMT, "SQLAllocHandle (owner lookup)", req->table_name, "?");
        return -1;
    }
    if (bind_name(stmt, 1, req->table_name, &ind) != 0)
    {
        meta_fail(ctx, stmt, "bind (owner lookup)", req->table_name, "?");
        goto Done;
    }
    if (!SQL_SUCCEEDED(SQLExecDirect(stmt, (SQLCHAR *)SQL_OWNER, SQL_NTS)))
    {
        meta_fail(ctx, stmt, "owner lookup", req->table_name, "?");
        goto Done;
    }

    SQLRETURN r = SQLFetch(stmt);
    if (r == SQL_NO_DATA)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "Table '%s' not found in INFORMATION_SCHEMA.TABLES",
                     req->table_name);
        rc = 1;
        goto Done;
    }
    int is_null = 0;
    if (!SQL_SUCCEEDED(r) ||
        get_text(stmt, 1, req->owner, sizeof(req->owner), &is_null) != 0 || is_null)
    {
        meta_fail(ctx, stmt, "owner fetch", req->table_name, "?");
        goto Done;
    }
    upper_inplace(req->owner);
    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Resolved owner='%s' for table '%s'", req->owner, req->table_name);
    rc = 0;

Done:
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return rc;
}

int mssql_describe_table(oci_context_t      *ctx,
                         metadata_request_t *req,
                         col_metadata_t     *cols,
                         int                *col_count,
                         int                 max_cols)
{
    if (!ctx || !req || !cols || !col_count || max_cols <= 0)
    {
        if (ctx)
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: one or more NULLs or max_cols <= 0");
        return -1;
    }
    *col_count = 0;
    if (!ctx->svchp)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "describe_table: no SQL Server connection on this session");
        return -1;
    }

    SQLHDBC hdbc = (SQLHDBC)ctx->svchp;
    upper_inplace(req->table_name);

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Entering describe_table table='%s' owner='%s'",
                 req->table_name, req->owner);

    if (req->owner[0])
        upper_inplace(req->owner);
    else if (resolve_owner(ctx, hdbc, req) != 0)
        return -1;

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    SQLLEN   ind_owner = 0, ind_table = 0;
    int      rc = -1;

    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &stmt)))
    {
        meta_fail(ctx, SQL_NULL_HSTMT, "SQLAllocHandle", req->table_name, req->owner);
        return -1;
    }
    if (bind_name(stmt, 1, req->owner, &ind_owner) != 0 ||
        bind_name(stmt, 2, req->table_name, &ind_table) != 0)
    {
        meta_fail(ctx, stmt, "bind", req->table_name, req->owner);
        goto Done;
    }
    if (!SQL_SUCCEEDED(SQLExecDirect(stmt, (SQLCHAR *)SQL_COLUMNS, SQL_NTS)))
    {
        meta_fail(ctx, stmt, "column query", req->table_name, req->owner);
        goto Done;
    }

    SQLRETURN r;
    while ((r = SQLFetch(stmt)) != SQL_NO_DATA)
    {
        if (!SQL_SUCCEEDED(r))
        {
            meta_fail(ctx, stmt, "column fetch", req->table_name, req->owner);
            goto Done;
        }
        if (*col_count >= max_cols)
        {
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Column count exceeds max_cols=%d - truncating", max_cols);
            break;
        }

        char name[256], type[128], nullable[8], dflt[4096];
        int  char_len = 0, prec = 0, scale = 0, dt_prec = 0, max_len = 0;
        int  n_name, n_type, n_len, n_prec, n_scale, n_dt, n_null, n_max, n_dflt;

        if (get_text(stmt, 1, name, sizeof(name), &n_name)         != 0 ||
            get_text(stmt, 2, type, sizeof(type), &n_type)         != 0 ||
            get_int (stmt, 3, &char_len, &n_len)                   != 0 ||
            get_int (stmt, 4, &prec, &n_prec)                      != 0 ||
            get_int (stmt, 5, &scale, &n_scale)                    != 0 ||
            get_int (stmt, 6, &dt_prec, &n_dt)                     != 0 ||
            get_text(stmt, 7, nullable, sizeof(nullable), &n_null) != 0 ||
            get_int (stmt, 8, &max_len, &n_max)                    != 0 ||
            get_text(stmt, 9, dflt, sizeof(dflt), &n_dflt)         != 0)
        {
            meta_fail(ctx, stmt, "SQLGetData", req->table_name, req->owner);
            goto Done;
        }

        col_metadata_t *col = &cols[*col_count];
        memset(col, 0, sizeof(*col));

        /* ---- data_type ---- */
        upper_inplace(type);
        if (is_base(type, "TIMESTAMP"))
            snprintf(type, sizeof(type), "ROWVERSION");
        int is_max = !n_len && char_len == -1 &&
                     (is_base(type, "VARCHAR") || is_base(type, "NVARCHAR") ||
                      is_base(type, "VARBINARY"));
        snprintf(col->col_name, sizeof(col->col_name), "%.127s", name);
        snprintf(col->data_type, sizeof(col->data_type), "%.120s%s", type, is_max ? "(MAX)" : "");

        /* ---- data_length ---- */
        if (db_type_class(col->data_type) == DB_TYPE_CLASS_LOB)
            col->data_length = 0;
        else if (!n_len)
            col->data_length = char_len;
        else
            col->data_length = n_max ? 0 : max_len;

        /* ---- precision / scale ---- */
        col->data_precision = n_prec ? -1 : prec;
        if (!n_scale)
            col->data_scale = scale;
        else if (!n_dt && (is_base(type, "DATETIME2") || is_base(type, "DATETIMEOFFSET") ||
                           is_base(type, "TIME")))
            col->data_scale = dt_prec;
        else
            col->data_scale = -1;

        snprintf(col->nullable, sizeof(col->nullable), "%s",
                 strcasecmp(nullable, "YES") == 0 ? "Y" : "N");

        /* ---- data_default: 511 bytes, cut at a character boundary ---- */
        snprintf(col->data_default, sizeof(col->data_default), "%.511s", dflt);
        if (strlen(dflt) >= sizeof(col->data_default))
            utf8_trim_partial(col->data_default);

        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Column %d: name='%s' type='%s' len=%d prec=%d scale=%d "
                     "null='%s' default='%s' class=%s",
                     *col_count + 1, col->col_name, col->data_type, col->data_length,
                     col->data_precision, col->data_scale, col->nullable,
                     col->data_default[0] ? col->data_default : "(none)",
                     db_type_class_name(db_type_class(col->data_type)));

        (*col_count)++;
    }

    if (*col_count == 0)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "Table '%s' owner '%s' not found or has no accessible columns",
                     req->table_name, req->owner);
        goto Done;
    }

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "describe_table OK: table='%s' owner='%s' columns=%d",
                 req->table_name, req->owner, *col_count);
    rc = 0;

Done:
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (rc != 0) *col_count = 0;
    return rc;
}
