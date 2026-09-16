\
/*
 * OCI_Table_Metadata_Module.c
 *
 * Shared Table Metadata Module
 * -----------------------------
 * Implements two public metadata functions:
 *
 *   get_request_metadata()
 *   ----------------------
 *   Queries ALL_TAB_COLUMNS for a SINGLE named table.  Used by INSERT,
 *   UPDATE, and any module targeting exactly one table.  This is the
 *   function that will serve cached results when the metadata cache is
 *   introduced - no changes required in any caller at that point.
 *
 *   get_multi_metadata()
 *   --------------------
 *   Describes and defines result-set columns for execute_query_batch
 *   and execute_procedure cursor fetches via OCI descriptor metadata
 *   (OCIParamGet / OCIAttrGet).  This is the correct approach for:
 *
 *     - Multi-table JOINs: ALL_TAB_COLUMNS cannot be queried per
 *       result column without knowing which source table each column
 *       belongs to.  OCI descriptor metadata always returns the right
 *       type and size regardless of how many tables are joined.
 *
 *     - Views: older Oracle versions do not expose view columns in
 *       ALL_TAB_COLUMNS.  OCI descriptor metadata always works.
 *
 *     - Synonyms and remote DB-link tables: same reason as views.
 *
 *   For the common single-table SELECT the two functions are
 *   equivalent.  When the cache arrives, get_request_metadata() will
 *   serve cached data for INSERT/UPDATE callers; get_multi_metadata()
 *   continues to use live OCI descriptors for query result sets.
 *   This boundary is intentional - both functions stay in this module
 *   so metadata logic never leaks into execute modules.
 *
 * Oracle LONG handling (get_request_metadata only)
 * -------------------------------------------------
 * DATA_DEFAULT in ALL_TAB_COLUMNS is a LONG column. OCI refuses to
 * bind a LONG in a multi-column select (ORA-00932). The workaround is
 * a dedicated single-column secondary query using SQLT_LNG. ROWNUM=1
 * guards against duplicate rows from multi-schema visibility.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "OCI_Table_Metadata_Module.h"
#include "sql_dependency_extractor.h"
#include "logger.h"

/* Date format hygiene (2026-08-08 closure item 3, follow-up review).
 * Deliberately a SEPARATE constant from NLS_DATE_FORMAT_DEFAULT
 * (ini_reader.h) - that one governs the user-configurable
 * nls_date_format setting for DATE-column values; this one is the
 * fixed TO_CHAR() mask used across every metadata query in this file
 * (ALL_OBJECTS.CREATED/LAST_DDL_TIME, ALL_TAB_STATISTICS.LAST_ANALYZED)
 * and is intentionally independent of that config value, so metadata
 * parsing stays predictable regardless of what a user configures for
 * DATE columns elsewhere. Centralised here purely to remove the
 * literal-string duplication across this file's own queries - not
 * because it should ever track nls_date_format. */
#define METADATA_TIMESTAMP_FORMAT_SQL "YYYY-MM-DD HH24:MI:SS"

/* ------------------------------------------------------------------ */
/*  OCI error macro - consistent with rest of project                  */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_META(errhp, status, ctx, label)                       \
    do {                                                                 \
        if ((status) != OCI_SUCCESS &&                                  \
            (status) != OCI_SUCCESS_WITH_INFO)                          \
        {                                                                \
            text   _errbuf[512];                                         \
            sb4    _errcode = 0;                                         \
            OCIErrorGet((errhp), 1, NULL, &_errcode,                    \
                        _errbuf, sizeof(_errbuf), OCI_HTYPE_ERROR);     \
            logger_write((ctx)->Metadata_logger, LOG_ERROR, __func__, 0,         \
                         "OCI Error %d: %s", _errcode,                  \
                         (char *)_errbuf);                               \
            rc = -1;                                                     \
            goto label;                                                  \
        }                                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Static helpers                                                      */
/* ------------------------------------------------------------------ */
static void trim_inplace(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    { s[len - 1] = '\0'; len--; }
}

static void uppercase_inplace(char *s)
{
    if (!s) return;
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* ==================================================================
 *  get_request_metadata
 *  Single-table path - queries ALL_TAB_COLUMNS.
 *  This function is NOT modified - preserved exactly as it was.
 * ================================================================== */
int get_request_metadata(oci_context_t      *ctx,
                         metadata_request_t *req,
                         col_metadata_t     *cols,
                         int                *col_count,
                         int                 max_cols)
{
    int      rc          = 0;
    OCIStmt *stmt_owner  = NULL;
    OCIStmt *stmt_dflt   = NULL;
    OCIStmt *stmt_main   = NULL;

    /* ---- Validate arguments ---- */
    if (!ctx || !req || !cols || !col_count || max_cols <= 0)
    {
        if (ctx)
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: one or more NULLs or "
                         "max_cols <= 0");
        return -1;
    }

    *col_count = 0;
    uppercase_inplace(req->table_name);

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Entering get_request_metadata table='%s'",
                 req->table_name);

    /* ================================================================
     *  Step 1: Resolve owner
     *  Use supplied owner if provided, otherwise query ALL_TABLES.
     *  Result is written back into req->owner for the caller.
     * ================================================================ */
    if (strlen(req->owner) > 0)
    {
        uppercase_inplace(req->owner);
        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "Using supplied owner='%s'", req->owner);
    }
    else
    {
        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "No owner supplied - resolving from ALL_TABLES");

        const char *sql_owner =
            "SELECT OWNER FROM ALL_TABLES "
            "WHERE  TABLE_NAME = :tname "
            "AND    ROWNUM     = 1";

        CHECK_OCI_META(ctx->errhp,
            OCIStmtPrepare2(ctx->svchp, &stmt_owner, ctx->errhp,
                            (text *)sql_owner, (ub4)strlen(sql_owner),
                            NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
            ctx, Cleanup);

        OCIBind  *bind_own_tname = NULL;
        OCIDefine *def_owner     = NULL;
        ub2       rlen_owner     = 0;

        CHECK_OCI_META(ctx->errhp,
            OCIBindByName(stmt_owner, &bind_own_tname, ctx->errhp,
                          (text *)":tname", -1,
                          (dvoid *)req->table_name,
                          (sb4)(strlen(req->table_name) + 1),
                          SQLT_STR, NULL, NULL, NULL, 0, NULL,
                          OCI_DEFAULT),
            ctx, Cleanup);

        CHECK_OCI_META(ctx->errhp,
            OCIDefineByPos(stmt_owner, &def_owner, ctx->errhp, 1,
                           req->owner, sizeof(req->owner),
                           SQLT_STR, NULL, &rlen_owner, NULL,
                           OCI_DEFAULT),
            ctx, Cleanup);

        CHECK_OCI_META(ctx->errhp,
            OCIStmtExecute(ctx->svchp, stmt_owner, ctx->errhp,
                           0, 0, NULL, NULL, OCI_DEFAULT),
            ctx, Cleanup);

        sword owner_fetch = OCIStmtFetch2(stmt_owner, ctx->errhp,
                                           1, OCI_FETCH_NEXT,
                                           0, OCI_DEFAULT);
        if (owner_fetch == OCI_NO_DATA)
        {
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Table '%s' not found in ALL_TABLES",
                         req->table_name);
            rc = -1;
            goto Cleanup;
        }
        CHECK_OCI_META(ctx->errhp, owner_fetch, ctx, Cleanup);

        req->owner[sizeof(req->owner) - 1] = '\0';
        trim_inplace(req->owner);
        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "Resolved owner='%s'", req->owner);

        OCIStmtRelease(stmt_owner, ctx->errhp, NULL, 0, OCI_DEFAULT);
        stmt_owner = NULL;
    }

    /* ================================================================
     *  Step 2: Prepare DATA_DEFAULT secondary query (SQLT_LNG)
     *  Prepared once, re-executed per column in the fetch loop.
     *  ROWNUM=1 handles duplicate rows from multi-schema visibility.
     * ================================================================ */
    const char *sql_dflt =
        "SELECT DATA_DEFAULT "
        "FROM   ALL_TAB_COLUMNS "
        "WHERE  TABLE_NAME  = :tname "
        "AND    COLUMN_NAME = :cname "
        "AND    OWNER       = :owner "
        "AND    ROWNUM      = 1";

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Preparing DATA_DEFAULT secondary query (SQLT_LNG)");

    CHECK_OCI_META(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt_dflt, ctx->errhp,
                        (text *)sql_dflt, (ub4)strlen(sql_dflt),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    /* Bind buffers for the secondary query */
    OCIBind *bind_dflt_tname = NULL;
    OCIBind *bind_dflt_cname = NULL;
    OCIBind *bind_dflt_owner = NULL;
    char     bind_cname[128] = {0};   /* updated each iteration        */

    char  data_default[32768] = {0};  /* 32KB covers any column default */
    ub2   long_len            = 0;
    sb2   ind_dflt            = 0;

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt_dflt, &bind_dflt_tname, ctx->errhp,
                      (text *)":tname", -1,
                      (dvoid *)req->table_name,
                      (sb4)(strlen(req->table_name) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt_dflt, &bind_dflt_cname, ctx->errhp,
                      (text *)":cname", -1,
                      (dvoid *)bind_cname, (sb4)sizeof(bind_cname),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt_dflt, &bind_dflt_owner, ctx->errhp,
                      (text *)":owner", -1,
                      (dvoid *)req->owner,
                      (sb4)(strlen(req->owner) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    /* Define DATA_DEFAULT as SQLT_LNG - correct OCI type for LONG     */
    OCIDefine *def_dflt = NULL;
    CHECK_OCI_META(ctx->errhp,
        OCIDefineByPos(stmt_dflt, &def_dflt, ctx->errhp, 1,
                       data_default, sizeof(data_default),
                       SQLT_LNG, &ind_dflt, &long_len, NULL,
                       OCI_DEFAULT),
        ctx, Cleanup);

    /* ================================================================
     *  Step 3: Main query - all columns except DATA_DEFAULT
     * ================================================================ */
    const char *sql_main =
        "SELECT COLUMN_NAME, "
        "       DATA_TYPE, "
        "       DATA_LENGTH, "
        "       NVL(DATA_PRECISION, -1), "
        "       NVL(DATA_SCALE, -1), "
        "       NULLABLE "
        "FROM   ALL_TAB_COLUMNS "
        "WHERE  TABLE_NAME = :tname "
        "AND    OWNER      = :owner "
        "ORDER  BY COLUMN_ID";

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Preparing main ALL_TAB_COLUMNS query "
                 "table='%s' owner='%s'",
                 req->table_name, req->owner);

    CHECK_OCI_META(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt_main, ctx->errhp,
                        (text *)sql_main, (ub4)strlen(sql_main),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_main_tname = NULL;
    OCIBind *bind_main_owner = NULL;

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt_main, &bind_main_tname, ctx->errhp,
                      (text *)":tname", -1,
                      (dvoid *)req->table_name,
                      (sb4)(strlen(req->table_name) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt_main, &bind_main_owner, ctx->errhp,
                      (text *)":owner", -1,
                      (dvoid *)req->owner,
                      (sb4)(strlen(req->owner) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    /* Define output columns */
    OCIDefine *def_name  = NULL;
    OCIDefine *def_type  = NULL;
    OCIDefine *def_len   = NULL;
    OCIDefine *def_prec  = NULL;
    OCIDefine *def_scale = NULL;
    OCIDefine *def_null  = NULL;

    /* Per-row fetch buffers */
    char col_name  [128] = {0};
    char data_type [128] = {0};
    char nullable  [4]   = {0};
    int  data_length     = 0;
    int  data_precision  = 0;
    int  data_scale      = 0;
    ub2  rlen_name       = 0;
    ub2  rlen_type       = 0;
    ub2  rlen_null       = 0;
    sb2  ind_prec        = 0;
    sb2  ind_scale       = 0;

    CHECK_OCI_META(ctx->errhp,
        OCIDefineByPos(stmt_main, &def_name, ctx->errhp, 1,
                       col_name, sizeof(col_name),
                       SQLT_STR, NULL, &rlen_name, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIDefineByPos(stmt_main, &def_type, ctx->errhp, 2,
                       data_type, sizeof(data_type),
                       SQLT_STR, NULL, &rlen_type, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIDefineByPos(stmt_main, &def_len, ctx->errhp, 3,
                       &data_length, sizeof(data_length),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIDefineByPos(stmt_main, &def_prec, ctx->errhp, 4,
                       &data_precision, sizeof(data_precision),
                       SQLT_INT, &ind_prec, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIDefineByPos(stmt_main, &def_scale, ctx->errhp, 5,
                       &data_scale, sizeof(data_scale),
                       SQLT_INT, &ind_scale, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIDefineByPos(stmt_main, &def_null, ctx->errhp, 6,
                       nullable, sizeof(nullable),
                       SQLT_STR, NULL, &rlen_null, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Executing main ALL_TAB_COLUMNS query");

    CHECK_OCI_META(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt_main, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    /* ================================================================
     *  Step 4: Fetch loop
     *  For each column fetch main metadata then DATA_DEFAULT via
     *  the SQLT_LNG secondary query.
     * ================================================================ */
    sword fetch_status;
    while ((fetch_status = OCIStmtFetch2(stmt_main, ctx->errhp,
                                          1, OCI_FETCH_NEXT,
                                          0, OCI_DEFAULT))
           == OCI_SUCCESS)
    {
        if (*col_count >= max_cols)
        {
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Column count exceeds max_cols=%d - "
                         "truncating", max_cols);
            break;
        }

        col_name [sizeof(col_name)  - 1] = '\0';
        data_type[sizeof(data_type) - 1] = '\0';
        nullable [sizeof(nullable)  - 1] = '\0';

        /* ---- Fetch DATA_DEFAULT for this column via SQLT_LNG ---- */
        memset(data_default, 0, sizeof(data_default));
        memset(bind_cname,   0, sizeof(bind_cname));
        strncpy(bind_cname, col_name, sizeof(bind_cname) - 1);
        long_len = 0;
        ind_dflt = 0;

        /* Re-bind :cname and :owner with current values              */
        CHECK_OCI_META(ctx->errhp,
            OCIBindByName(stmt_dflt, &bind_dflt_cname, ctx->errhp,
                          (text *)":cname", -1,
                          (dvoid *)bind_cname,
                          (sb4)(strlen(bind_cname) + 1),
                          SQLT_STR, NULL, NULL, NULL, 0, NULL,
                          OCI_DEFAULT),
            ctx, Cleanup);

        CHECK_OCI_META(ctx->errhp,
            OCIBindByName(stmt_dflt, &bind_dflt_owner, ctx->errhp,
                          (text *)":owner", -1,
                          (dvoid *)req->owner,
                          (sb4)(strlen(req->owner) + 1),
                          SQLT_STR, NULL, NULL, NULL, 0, NULL,
                          OCI_DEFAULT),
            ctx, Cleanup);

        sword dflt_exec = OCIStmtExecute(ctx->svchp, stmt_dflt,
                                          ctx->errhp,
                                          0, 0, NULL, NULL, OCI_DEFAULT);
        if (dflt_exec != OCI_SUCCESS &&
            dflt_exec != OCI_SUCCESS_WITH_INFO)
        {
            logger_write(ctx->Metadata_logger, LOG_WARN, __func__, 0,
                         "DATA_DEFAULT fetch failed for column '%s' "
                         "- treating as empty", col_name);
            data_default[0] = '\0';
        }
        else
        {
            sword dflt_fetch = OCIStmtFetch2(stmt_dflt, ctx->errhp,
                                              1, OCI_FETCH_NEXT,
                                              0, OCI_DEFAULT);
            if (dflt_fetch == OCI_NO_DATA || ind_dflt < 0)
                data_default[0] = '\0';
            else
            {
                data_default[sizeof(data_default) - 1] = '\0';
                trim_inplace(data_default);
            }
        }

        /* ---- Populate the col_metadata_t entry ---- */
        col_metadata_t *col = &cols[*col_count];
        memset(col, 0, sizeof(*col));

        strncpy(col->col_name,     col_name,     sizeof(col->col_name)     - 1);
        strncpy(col->data_type,    data_type,    sizeof(col->data_type)    - 1);
        col->data_length    = data_length;
        col->data_precision = (ind_prec  < 0) ? -1 : data_precision;
        col->data_scale     = (ind_scale < 0) ? -1 : data_scale;
        strncpy(col->nullable,     nullable,     sizeof(col->nullable)     - 1);
        strncpy(col->data_default, data_default, sizeof(col->data_default) - 1);
        /* source_table left empty for single-table path              */

        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Column %d: name='%s' type='%s' len=%d "
                     "prec=%d scale=%d null='%s' default='%s'",
                     *col_count + 1,
                     col->col_name, col->data_type,
                     col->data_length,
                     col->data_precision, col->data_scale,
                     col->nullable,
                     col->data_default[0] ? col->data_default : "(none)");

        (*col_count)++;

        /* Reset fetch buffers */
        memset(col_name,  0, sizeof(col_name));
        memset(data_type, 0, sizeof(data_type));
        memset(nullable,  0, sizeof(nullable));
        data_length    = 0;
        data_precision = 0;
        data_scale     = 0;
        ind_prec       = 0;
        ind_scale      = 0;
    }

    if (fetch_status != OCI_NO_DATA)
    {
        text  errbuf[512];
        sb4   errcode = 0;
        OCIErrorGet(ctx->errhp, 1, NULL, &errcode,
                    errbuf, sizeof(errbuf), OCI_HTYPE_ERROR);
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "Unexpected fetch status %d  OCI Error %d: %s",
                     fetch_status, errcode, (char *)errbuf);
        rc = -1;
        goto Cleanup;
    }

    if (*col_count == 0)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "Table '%s' owner '%s' not found or has no "
                     "accessible columns",
                     req->table_name, req->owner);
        rc = -1;
        goto Cleanup;
    }

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "get_request_metadata OK: table='%s' owner='%s' "
                 "columns=%d",
                 req->table_name, req->owner, *col_count);

Cleanup:
    if (stmt_owner) OCIStmtRelease(stmt_owner, ctx->errhp,
                                    NULL, 0, OCI_DEFAULT);
    if (stmt_dflt)  OCIStmtRelease(stmt_dflt,  ctx->errhp,
                                    NULL, 0, OCI_DEFAULT);
    if (stmt_main)  OCIStmtRelease(stmt_main,  ctx->errhp,
                                    NULL, 0, OCI_DEFAULT);
    return rc;
}

/* ==================================================================
 *  get_multi_metadata
 *
 *  Multi-table / view / join path.
 *
 *  Lifted verbatim from define_columns_batch() which previously lived
 *  as a static function inside OCI_Execute_Query_Batch_Module.c.
 *  Behaviour is identical - only the function signature changed to
 *  accept a multi_meta_request_t instead of individual pointers.
 *
 *  Callers must populate all pointer fields in mmr before calling.
 *  All allocated memory remains owned by the caller and must be freed
 *  by the caller's existing cleanup path (free_batch_ctx or equiv).
 * ================================================================== */
int get_multi_metadata(multi_meta_request_t *mmr)
{
    int rc = 0;

    if (!mmr || !mmr->ctx || !mmr->stmt)
    {
        if (mmr && mmr->ctx)
            logger_write(mmr->ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: mmr, ctx or stmt is NULL");
        return -1;
    }

    oci_context_t *ctx = mmr->ctx;

    /* Validate all required output pointers are present */
    if (!mmr->def        || !mmr->buffers    || !mmr->buf_sizes  ||
        !mmr->indicators || !mmr->data_types || !mmr->data_sizes ||
        !mmr->col_names  || !mmr->col_blob_locs)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "One or more required array pointers are NULL - "
                     "call allocate_batch_buffers first");
        return -1;
    }

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Entering get_multi_metadata col_count=%u "
                 "fetch_count=%u",
                 mmr->col_count, mmr->fetch_count);

    OCIParam *param = NULL;

    for (ub4 i = 1; i <= mmr->col_count; i++)
    {
        ub4 ci = i - 1;   /* 0-based column index */

        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "Processing column %u", i);

        /* ---- Column name ---- */
        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Calling OCIParamGet col=%u", i);
        CHECK_OCI_META(ctx->errhp,
            OCIParamGet(mmr->stmt, OCI_HTYPE_STMT,
                        ctx->errhp, (void **)&param, i),
            ctx, Cleanup);

        text *tmp_name = NULL;
        ub4   tmp_len  = 0;

        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Calling OCIAttrGet OCI_ATTR_NAME col=%u", i);
        CHECK_OCI_META(ctx->errhp,
            OCIAttrGet(param, OCI_DTYPE_PARAM,
                       &tmp_name, &tmp_len,
                       OCI_ATTR_NAME, ctx->errhp),
            ctx, Cleanup);

        if (tmp_len > 255) tmp_len = 255;
        memcpy(mmr->col_names[ci], tmp_name, tmp_len);
        mmr->col_names[ci][tmp_len] = '\0';

        /* ---- Data type ---- */
        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Calling OCIAttrGet OCI_ATTR_DATA_TYPE col=%u", i);
        CHECK_OCI_META(ctx->errhp,
            OCIAttrGet(param, OCI_DTYPE_PARAM,
                       &mmr->data_types[ci], 0,
                       OCI_ATTR_DATA_TYPE, ctx->errhp),
            ctx, Cleanup);

        /* ---- Data size ---- */
        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Calling OCIAttrGet OCI_ATTR_DATA_SIZE col=%u", i);
        CHECK_OCI_META(ctx->errhp,
            OCIAttrGet(param, OCI_DTYPE_PARAM,
                       &mmr->data_sizes[ci], 0,
                       OCI_ATTR_DATA_SIZE, ctx->errhp),
            ctx, Cleanup);

        ub4 buf_size = mmr->data_sizes[ci] + 32;
        if (buf_size < 64) buf_size = 64;
        mmr->buf_sizes[ci] = buf_size;

        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "Column %u name=%s type=%u size=%u buf_size=%u",
                     i, mmr->col_names[ci],
                     mmr->data_types[ci], mmr->data_sizes[ci],
                     buf_size);

        /* ---- Allocate per-column storage and register define ---- */

        if (mmr->data_types[ci] == SQLT_BLOB)
        {
            /* BLOB: one locator per row in the fetch batch.
             * OCIDefineArrayOfStruct strides through the locator array
             * using sizeof(OCILobLocator*) as value_skip.             */
            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "Allocating BLOB locator array col=%u rows=%u",
                         ci, mmr->fetch_count);

            mmr->col_blob_locs[ci] = calloc(mmr->fetch_count,
                                            sizeof(OCILobLocator *));
            if (!mmr->col_blob_locs[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for col_blob_locs[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            for (ub4 r = 0; r < mmr->fetch_count; r++)
            {
                logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                             "OCIDescriptorAlloc BLOB locator "
                             "col=%u row=%u", ci, r);
                CHECK_OCI_META(ctx->errhp,
                    OCIDescriptorAlloc(ctx->envhp,
                                       (void **)&mmr->col_blob_locs[ci][r],
                                       OCI_DTYPE_LOB, 0, NULL),
                    ctx, Cleanup);
            }

            mmr->indicators[ci] = calloc(mmr->fetch_count, sizeof(sb2));
            if (!mmr->indicators[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for indicators[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineByPos BLOB col=%u", i);
            CHECK_OCI_META(ctx->errhp,
                OCIDefineByPos(mmr->stmt, &mmr->def[ci], ctx->errhp,
                               i,
                               (dvoid *)&mmr->col_blob_locs[ci][0],
                               -1,
                               SQLT_BLOB,
                               &mmr->indicators[ci][0],
                               NULL, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineArrayOfStruct BLOB col=%u "
                         "value_skip=%zu ind_skip=%zu",
                         i, sizeof(OCILobLocator *), sizeof(sb2));
            CHECK_OCI_META(ctx->errhp,
                OCIDefineArrayOfStruct(mmr->def[ci], ctx->errhp,
                                       (ub4)sizeof(OCILobLocator *),
                                       (ub4)sizeof(sb2),
                                       0, 0),
                ctx, Cleanup);
        }
        else if (mmr->data_types[ci] == SQLT_CLOB)
        {
            /*
             * CLOB ARRAY FETCH RESTRICTION - OCI QUIRK - DO NOT REMOVE
             * ----------------------------------------------------------
             * OCI does not support array fetch of CLOB locators via
             * OCIDefineArrayOfStruct.  A single shared locator is used
             * and the caller must force fetch_count=1 whenever any CLOB
             * column is present (enforced in execute_query_batch after
             * this function returns, exactly as before).
             */
            mmr->indicators[ci] = calloc(mmr->fetch_count, sizeof(sb2));
            if (!mmr->indicators[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for indicators[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineByPos CLOB col=%u (single locator)",
                         i);
            CHECK_OCI_META(ctx->errhp,
                OCIDefineByPos(mmr->stmt, &mmr->def[ci], ctx->errhp,
                               i,
                               (dvoid *)&mmr->clob_loc,
                               -1,
                               SQLT_CLOB,
                               &mmr->indicators[ci][0],
                               NULL, NULL, OCI_DEFAULT),
                ctx, Cleanup);
            /* No OCIDefineArrayOfStruct for CLOB - OCI restriction   */
        }
        else
        {
            /*
             * Scalar column.
             * buffers[ci] is a flat block: fetch_count rows x buf_size.
             * Row r starts at buffers[ci] + (r * buf_size).
             * OCIDefineArrayOfStruct strides through it with buf_size.
             */
            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "Allocating scalar buffer col=%u "
                         "fetch_count=%u buf_size=%u",
                         ci, mmr->fetch_count, buf_size);

            mmr->buffers[ci] = calloc(mmr->fetch_count, buf_size);
            if (!mmr->buffers[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for buffers[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            mmr->indicators[ci] = calloc(mmr->fetch_count, sizeof(sb2));
            if (!mmr->indicators[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for indicators[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineByPos scalar col=%u buf_size=%u",
                         i, buf_size);
            CHECK_OCI_META(ctx->errhp,
                OCIDefineByPos(mmr->stmt, &mmr->def[ci], ctx->errhp,
                               i,
                               mmr->buffers[ci],
                               (sb4)buf_size,
                               SQLT_STR,
                               &mmr->indicators[ci][0],
                               NULL, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineArrayOfStruct scalar col=%u "
                         "value_skip=%u ind_skip=%zu",
                         i, buf_size, sizeof(sb2));
            CHECK_OCI_META(ctx->errhp,
                OCIDefineArrayOfStruct(mmr->def[ci], ctx->errhp,
                                       buf_size,
                                       (ub4)sizeof(sb2),
                                       0, 0),
                ctx, Cleanup);
        }
    }

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "get_multi_metadata complete col_count=%u",
                 mmr->col_count);

Cleanup:
    return rc;
}







/* ==================================================================
 *  get_table_metadata()
 *
 *  Queries ALL_TABLES for a single object and returns its full row
 *  as a heap-allocated table_metadata_alltabs_t.
 *
 *  Add this function to OCI_Table_Metadata_Module.c
 *  Add the struct and declaration to OCI_Table_Metadata_Module.h
 *  (see table_metadata_additions.h for the exact text to paste in)
 * ================================================================== */

/* ------------------------------------------------------------------ */
/*  free_table_metadata                                                 */
/* ------------------------------------------------------------------ */
void free_table_metadata(table_metadata_alltabs_t *tm)
{
    if (tm) free(tm);
}

/* ------------------------------------------------------------------ */
/*  free_object_metadata                                                */
/* ------------------------------------------------------------------ */
void free_object_metadata(object_metadata_allobjs_t *om)
{
    if (om) free(om);
}

/* ==================================================================
 *  get_object_metadata()
 *
 *  Queries ALL_OBJECTS for a single named object and returns its
 *  metadata as a heap-allocated object_metadata_allobjs_t.
 *
 *  Unlike get_table_metadata() which queries ALL_TABLES and returns
 *  nothing for views, this function correctly handles tables, views,
 *  synonyms, materialized views, and any other object type that can
 *  appear in a SELECT FROM clause.
 *
 *  All columns are fetched as VARCHAR2 via TO_CHAR() so a single OCI
 *  type code (SQLT_STR) covers the entire select list.
 * ================================================================== */
object_metadata_allobjs_t *get_object_metadata(oci_context_t *ctx,
                                                const char    *object_owner,
                                                const char    *object_name)
{
    int       rc   = 0;
    OCIStmt  *stmt = NULL;
    object_metadata_allobjs_t *om = NULL;

    /* ---- Validate ---- */
    if (!ctx || !object_name || object_name[0] == '\0')
    {
        if (ctx)
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or object_name is NULL/empty");
        return NULL;
    }

    /* ---- Resolve and upper-case owner / name ---- */
    char owner_buf[129] = {0};
    char name_buf [129] = {0};

    if (!object_owner || object_owner[0] == '\0')
    {
        strncpy(owner_buf, ctx->ini->username, sizeof(owner_buf) - 1);
        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "No owner supplied - using logged-on user='%s'",
                     owner_buf);
    }
    else
    {
        strncpy(owner_buf, object_owner, sizeof(owner_buf) - 1);
    }

    strncpy(name_buf, object_name, sizeof(name_buf) - 1);

    for (char *p = owner_buf; *p; p++) *p = (char)toupper((unsigned char)*p);
    for (char *p = name_buf;  *p; p++) *p = (char)toupper((unsigned char)*p);

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Entering get_object_metadata owner='%s' object='%s'",
                 owner_buf, name_buf);

    /* ---- Allocate return struct ---- */
    om = calloc(1, sizeof(object_metadata_allobjs_t));
    if (!om)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for object_metadata_allobjs_t");
        return NULL;
    }

    /* ================================================================
     *  SELECT - fetch every useful ALL_OBJECTS column as VARCHAR2.
     *  NUMBER columns use TO_CHAR(); DATE columns use
     *  TO_CHAR(...,METADATA_TIMESTAMP_FORMAT_SQL) for consistent
     *  formatting - see that constant's own doc comment near the top
     *  of this file for why it's a separate, fixed value, not
     *  NLS_DATE_FORMAT_DEFAULT/nls_date_format.
     *  ROWNUM = 1 guards against duplicates from edition visibility.
     * ================================================================ */
    const char *sql =
        "SELECT "
        "  OWNER,"
        "  OBJECT_NAME,"
        "  NVL(SUBOBJECT_NAME,''),"
        "  OBJECT_TYPE,"
        "  TO_CHAR(OBJECT_ID),"
        "  TO_CHAR(DATA_OBJECT_ID),"
        "  STATUS,"
        "  TO_CHAR(CREATED,'" METADATA_TIMESTAMP_FORMAT_SQL "'),"
        "  TO_CHAR(LAST_DDL_TIME,'" METADATA_TIMESTAMP_FORMAT_SQL "'),"
        "  NVL(TIMESTAMP,''),"
        "  TEMPORARY,"
        "  GENERATED,"
        "  SECONDARY,"
        "  TO_CHAR(NAMESPACE),"
        "  NVL(EDITION_NAME,''),"
        "  NVL(SHARING,''),"
        "  NVL(EDITIONABLE,''),"
        "  NVL(ORACLE_MAINTAINED,''),"
        "  NVL(APPLICATION,''),"
        "  NVL(DEFAULT_COLLATION,''),"
        "  NVL(DUPLICATED,''),"
        "  NVL(SHARDED,''),"
        "  NVL(TO_CHAR(CREATED_APPID),''),"
        "  NVL(TO_CHAR(CREATED_VSNID),''),"
        "  NVL(TO_CHAR(MODIFIED_APPID),''),"
        "  NVL(TO_CHAR(MODIFIED_VSNID),'')"
        " FROM ALL_OBJECTS"
        " WHERE OWNER       = :owner"
        " AND   OBJECT_NAME = :oname"
        " AND   ROWNUM      = 1";

#define OM_COL_COUNT  26
#define OM_BUF_SIZE  1024

    char      fetch_bufs[OM_COL_COUNT][OM_BUF_SIZE];
    sb2       indicators[OM_COL_COUNT];
    ub2       ret_lens  [OM_COL_COUNT];
    OCIDefine *defs     [OM_COL_COUNT];

    memset(fetch_bufs, 0, sizeof(fetch_bufs));
    memset(indicators, 0, sizeof(indicators));
    memset(ret_lens,   0, sizeof(ret_lens));
    memset(defs,       0, sizeof(defs));

    /* ---- Prepare ---- */
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCIStmtPrepare2");

    CHECK_OCI_META(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    /* ---- Bind :owner and :oname ---- */
    OCIBind *bind_owner = NULL;
    OCIBind *bind_oname = NULL;

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt, &bind_owner, ctx->errhp,
                      (text *)":owner", -1,
                      (dvoid *)owner_buf, (sb4)(strlen(owner_buf) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt, &bind_oname, ctx->errhp,
                      (text *)":oname", -1,
                      (dvoid *)name_buf, (sb4)(strlen(name_buf) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    /* ---- Define all columns as SQLT_STR ---- */
    for (int c = 0; c < OM_COL_COUNT; c++)
    {
        CHECK_OCI_META(ctx->errhp,
            OCIDefineByPos(stmt, &defs[c], ctx->errhp,
                           (ub4)(c + 1),
                           fetch_bufs[c],
                           OM_BUF_SIZE,
                           SQLT_STR,
                           &indicators[c],
                           &ret_lens[c],
                           NULL,
                           OCI_DEFAULT),
            ctx, Cleanup);
    }

    /* ---- Execute ---- */
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCIStmtExecute");

    CHECK_OCI_META(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    /* ---- Fetch one row ---- */
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCIStmtFetch2");

    sword fetch_rc = OCIStmtFetch2(stmt, ctx->errhp,
                                    1, OCI_FETCH_NEXT,
                                    0, OCI_DEFAULT);

    if (fetch_rc == OCI_NO_DATA)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "Object '%s.%s' not found in ALL_OBJECTS",
                     owner_buf, name_buf);
        free(om);
        om = NULL;
        goto Cleanup;
    }

    CHECK_OCI_META(ctx->errhp, fetch_rc, ctx, Cleanup);

    /* ================================================================
     *  Populate struct - column order matches the SELECT exactly.
     *  Indicator -1 means NULL; leave the field as empty string.
     * ================================================================ */
#define OM_COPY(field, col_idx) \
    if (indicators[(col_idx)] != -1) \
    { strncpy(om->field, fetch_bufs[(col_idx)], sizeof(om->field) - 1); \
      om->field[sizeof(om->field) - 1] = '\0'; }

    OM_COPY(owner,           0);
    OM_COPY(object_name,     1);
    OM_COPY(subobject_name,  2);
    OM_COPY(object_type,     3);
    OM_COPY(object_id,       4);
    OM_COPY(data_object_id,  5);
    OM_COPY(status,          6);
    OM_COPY(created,         7);
    OM_COPY(last_ddl_time,   8);
    OM_COPY(timestamp,       9);
    OM_COPY(temporary,      10);
    OM_COPY(generated,      11);
    OM_COPY(secondary,      12);
    OM_COPY(namespace_,     13);
    OM_COPY(edition_name,   14);
    OM_COPY(sharing,        15);
    OM_COPY(editionable,    16);
    OM_COPY(oracle_maintained, 17);
    OM_COPY(application,    18);
    OM_COPY(default_collation, 19);
    OM_COPY(duplicated,     20);
    OM_COPY(sharded,        21);
    OM_COPY(created_appid,  22);
    OM_COPY(created_vsnid,  23);
    OM_COPY(modified_appid, 24);
    OM_COPY(modified_vsnid, 25);

#undef OM_COPY

    /* ---- Log result ---- */
    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "get_object_metadata OK: owner='%s' object='%s' "
                 "type='%s' status='%s' "
                 "created='%s' last_ddl_time='%s'",
                 om->owner,
                 om->object_name,
                 om->object_type,
                 om->status,
                 om->created,
                 om->last_ddl_time);

    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  subobject_name   = '%s'", om->subobject_name);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  object_id        = '%s'", om->object_id);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  data_object_id   = '%s'", om->data_object_id);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  temporary        = '%s'", om->temporary);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  generated        = '%s'", om->generated);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  secondary        = '%s'", om->secondary);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  sharing          = '%s'", om->sharing);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  editionable      = '%s'", om->editionable);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  oracle_maintained= '%s'", om->oracle_maintained);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  default_collation= '%s'", om->default_collation);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  duplicated       = '%s'", om->duplicated);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  sharded          = '%s'", om->sharded);

Cleanup:
    if (stmt)
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

    if (rc != 0 && om)
    {
        free(om);
        om = NULL;
    }

#undef OM_COL_COUNT
#undef OM_BUF_SIZE

    return om;
}

/* ==================================================================
 *  get_select_metadata()
 *
 *  Combined metadata path for SELECT statements.
 *
 *  Step 1 - For each FROM-clause object in mmr->deps, call
 *           get_object_metadata() (queries ALL_OBJECTS).  This covers
 *           tables, views, synonyms, and materialized views correctly.
 *           get_table_metadata() (ALL_TABLES) is intentionally NOT
 *           used here because SELECT queries are commonly issued
 *           against views which do not appear in ALL_TABLES.
 *           A NULL return means the object was not visible in
 *           ALL_OBJECTS - logged as a warning, not an abort.
 *
 *  Step 2 - Walk mmr->deps->fields[] to build an alias->object_name
 *           map.  For each SELECT-clause field, log which source table
 *           it belongs to.  This cross-reference is diagnostic; the
 *           OCI define work does not depend on it.
 *
 *  Step 3 - Delegate to get_multi_metadata() for the actual OCI
 *           OCIParamGet / OCIDefineByPos / OCIDefineArrayOfStruct work.
 *           The fetch loop and free_batch_ctx() are completely unchanged.
 *
 *  If mmr->deps is NULL the function falls through directly to
 *  get_multi_metadata() so existing callers remain safe.
 * ================================================================== */
int get_select_metadata(multi_meta_request_t *mmr)
{
    if (!mmr || !mmr->ctx || !mmr->stmt)
    {
        if (mmr && mmr->ctx)
            logger_write(mmr->ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "get_select_metadata: mmr, ctx or stmt is NULL");
        return -1;
    }

    oci_context_t *ctx = mmr->ctx;

    /* ----------------------------------------------------------------
     * If no dependency list was supplied fall through to the standard
     * OCI-descriptor-only path with no behaviour change.
     * ---------------------------------------------------------------- */
    if (!mmr->deps)
    {
        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "get_select_metadata: no deps supplied - "
                     "delegating to get_multi_metadata");
        return get_multi_metadata(mmr);
    }

    OCI_DEPENDENCY_LIST *deps = mmr->deps;

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "get_select_metadata: object_count=%d field_count=%d",
                 deps->object_count, deps->field_count);

    /* ================================================================
     *  Step 1 - Retrieve ALL_OBJECTS metadata for every FROM-clause
     *           object.  Using ALL_OBJECTS (not ALL_TABLES) means views,
     *           synonyms, and materialized views are all resolved
     *           correctly.  Results are logged to Metadata_logger.
     *
     *  Step 1b - For objects whose type is TABLE, validate every
     *            SELECT-clause field that references this table against
     *            ALL_TAB_COLUMNS.  Any unknown column name causes an
     *            immediate -1 return (fail fast) before any OCI
     *            execution round-trips are made.
     *
     *            Views, synonyms, materialized views and other object
     *            types are skipped for column validation - Oracle will
     *            catch unknown columns at OCIStmtPrepare2 time.
     * ================================================================ */
    for (int i = 0; i < deps->object_count; i++)
    {
        const OCI_OBJECT_REF *obj = &deps->objects[i];

        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "Step 1 [%d/%d]: get_object_metadata "
                     "owner='%s' object='%s' alias='%s'",
                     i + 1, deps->object_count,
                     obj->owner[0]  ? obj->owner      : "(resolve)",
                     obj->object_name,
                     obj->alias[0]  ? obj->alias       : "(none)");

        object_metadata_allobjs_t *om =
            get_object_metadata(ctx,
                                obj->owner[0] ? obj->owner : NULL,
                                obj->object_name);

        if (om)
        {
            logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                         "  ALL_OBJECTS: owner='%s' object='%s' "
                         "type='%s' status='%s' "
                         "created='%s' last_ddl_time='%s'",
                         om->owner,
                         om->object_name,
                         om->object_type,
                         om->status,
                         om->created,
                         om->last_ddl_time);

            /* --------------------------------------------------------
             *  Step 1b: Column validation for TABLE objects only.
             *
             *  Strategy:
             *    - Fetch the full column list from ALL_TAB_COLUMNS via
             *      get_request_metadata().
             *    - For each SELECT-clause field that belongs to this
             *      table (matched by table_ref against alias or name),
             *      check it exists in the fetched column list.
             *    - Unknown column -> LOG_ERROR and return -1.
             *    - Non-TABLE objects (VIEW, SYNONYM, etc.) are skipped;
             *      Oracle validates them at prepare time.
             *    - Phase B has already run by the time get_select_metadata
             *      is called, so deps->fields[] contains real column names
             *      with no remaining wildcards.
             * -------------------------------------------------------- */
            if (strcasecmp(om->object_type, "TABLE") == 0)
            {
                logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                             "Step 1b: Validating SELECT fields against "
                             "ALL_TAB_COLUMNS for TABLE '%s.%s'",
                             om->owner, om->object_name);

                col_metadata_t  tab_cols[MAX_TABLE_COLUMNS];
                int             tab_col_count = 0;

                metadata_request_t req;
                memset(&req, 0, sizeof(req));
                strncpy(req.table_name, om->object_name,
                        sizeof(req.table_name) - 1);
                strncpy(req.owner, om->owner,
                        sizeof(req.owner) - 1);

                if (get_request_metadata(ctx, &req,
                                         tab_cols, &tab_col_count,
                                         MAX_TABLE_COLUMNS) != 0)
                {
                    /* get_request_metadata already logged the error  */
                    logger_write(ctx->Metadata_logger, LOG_ERROR,
                                 __func__, 0,
                                 "Step 1b: get_request_metadata failed "
                                 "for '%s.%s' - aborting.",
                                 om->owner, om->object_name);
                    free_object_metadata(om);
                    return -1;
                }

                logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                             "Step 1b: Fetched %d column(s) from "
                             "ALL_TAB_COLUMNS for '%s.%s'",
                             tab_col_count, om->owner, om->object_name);

                /* Check each SELECT field that targets this table     */
                for (int f = 0; f < deps->field_count; f++)
                {
                    const OCI_FIELD_REF *field = &deps->fields[f];

                    /* Match field->table_ref to this object by alias
                     * or by object name                               */
                    int alias_match =
                        (obj->alias[0] != '\0' &&
                         strcasecmp(field->table_ref,
                                    obj->alias) == 0);
                    int name_match =
                        (strcasecmp(field->table_ref,
                                    obj->object_name) == 0);

                    if (!alias_match && !name_match)
                        continue;   /* field belongs to another table  */

                    /* Search tab_cols[] for this field name           */
                    int found = 0;
                    for (int c = 0; c < tab_col_count; c++)
                    {
                        if (strcasecmp(field->field_name,
                                       tab_cols[c].col_name) == 0)
                        {
                            found = 1;
                            logger_write(ctx->Metadata_logger,
                                         LOG_INFO, __func__, 0,
                                         "Step 1b: field[%d] '%s.%s' "
                                         "OK (type=%s)",
                                         field->field_pos,
                                         obj->object_name,
                                         field->field_name,
                                         tab_cols[c].data_type);
                            break;
                        }
                    }

                    if (!found)
                    {
                        logger_write(ctx->Metadata_logger,
                                     LOG_ERROR, __func__, 0,
                                     "Step 1b: Unknown column '%s' in "
                                     "table '%s.%s' "
                                     "(SELECT field position %d) - "
                                     "aborting.",
                                     field->field_name,
                                     om->owner,
                                     om->object_name,
                                     field->field_pos);
                        free_object_metadata(om);
                        return -1;
                    }
                }

                logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                             "Step 1b: All SELECT fields validated OK "
                             "for TABLE '%s.%s'",
                             om->owner, om->object_name);
            }
            else
            {
                logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                             "Step 1b: Skipping column validation for "
                             "object type '%s' ('%s.%s') - "
                             "Oracle will validate at prepare time.",
                             om->object_type, om->owner, om->object_name);
            }

            free_object_metadata(om);
            om = NULL;
        }
        else
        {
            /*
             * NULL return means the object was not found in ALL_OBJECTS.
             * ALL_OBJECTS covers tables, views, synonyms, materialised
             * views, and DB-link remote objects visible to this session.
             * If the object is not there it does not exist or is not
             * accessible - there is no point proceeding to execute the
             * query because Oracle will raise ORA-00942 anyway.
             *
             * Fail fast here so:
             *   a) The error is attributed to the correct cause
             *      (object not found) rather than a later OCI error
             *      from the COUNT(*) sub-query.
             *   b) No wasted round-trips to Oracle are made.
             *   c) The metrics record carries the correct error text.
             *
             * If a DB-link or cross-schema object is genuinely
             * inaccessible to ALL_OBJECTS but executable via OCI, the
             * caller should pre-qualify the SQL with the full
             * OWNER.OBJECT_NAME so the parser populates obj->owner and
             * the object can be resolved correctly.
             */
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "  Object not found in ALL_OBJECTS: "
                         "owner='%s' object='%s' - "
                         "table or view does not exist or is not "
                         "accessible. Aborting.",
                         obj->owner[0] ? obj->owner : "(resolve)",
                         obj->object_name);
            return -1;
        }
    }

    /* ================================================================
     *  Step 2 - Cross-reference SELECT-clause fields to their source
     *           tables via the alias map from deps->objects[].
     *           This is purely diagnostic logging; no OCI state is
     *           changed here.  The information is useful when reading
     *           Metadata_logger to understand which table each output
     *           column originates from.
     *
     *  Algorithm:
     *    For field.table_ref, find the matching OCI_OBJECT_REF by
     *    alias first, then by object_name.  Log the resolved owner
     *    and table for each field in SELECT-clause order.
     * ================================================================ */
    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Step 2: Cross-referencing %d SELECT-clause field(s) "
                 "to source tables",
                 deps->field_count);

    for (int f = 0; f < deps->field_count; f++)
    {
        const OCI_FIELD_REF *field = &deps->fields[f];

        /* Find the matching object for field->table_ref */
        const char *resolved_owner = "";
        const char *resolved_table = field->table_ref;

        for (int o = 0; o < deps->object_count; o++)
        {
            const OCI_OBJECT_REF *obj = &deps->objects[o];
            int alias_match = (obj->alias[0] != '\0' &&
                               strcasecmp(field->table_ref,
                                          obj->alias) == 0);
            int name_match  = (strcasecmp(field->table_ref,
                                          obj->object_name) == 0);
            if (alias_match || name_match)
            {
                resolved_owner = obj->owner;
                resolved_table = obj->object_name;
                break;
            }
        }

        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "  Field[%d] pos=%d '%s.%s'%s%s -> "
                     "source owner='%s' table='%s'",
                     f + 1,
                     field->field_pos,
                     field->table_ref,
                     field->field_name,
                     field->field_alias[0] ? " AS " : "",
                     field->field_alias[0] ? field->field_alias : "",
                     resolved_owner[0] ? resolved_owner : "(resolve)",
                     resolved_table);
    }

    /* ================================================================
     *  Step 3 - Delegate to get_multi_metadata() for the OCI work.
     *           This function fills all bc arrays, allocates buffers,
     *           and registers OCIDefineByPos / OCIDefineArrayOfStruct.
     *           The fetch loop and free_batch_ctx() are unchanged.
     * ================================================================ */
    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Step 3: Delegating to get_multi_metadata for "
                 "OCI define work (col_count=%u fetch_count=%u)",
                 mmr->col_count, mmr->fetch_count);

    int rc = get_multi_metadata(mmr);

    if (rc != 0)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "get_select_metadata: get_multi_metadata failed");
        return -1;
    }

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "get_select_metadata complete OK");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Internal helper: safe copy with NUL guarantee                      */
/* ------------------------------------------------------------------ */
static void safe_copy(char *dest, size_t dest_size, const char *src)
{
    if (!src || src[0] == '\0')
    {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/*  get_table_metadata                                                  */
/* ------------------------------------------------------------------ */
table_metadata_alltabs_t *get_table_metadata(oci_context_t *ctx,
                                      const char    *object_owner,
                                      const char    *object_name)
{
    int       rc        = 0;
    OCIStmt  *stmt      = NULL;
    table_metadata_alltabs_t *tm = NULL;

    /* ---- Validate ---- */
    if (!ctx || !object_name || object_name[0] == '\0')
    {
        if (ctx)
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or object_name is NULL/empty");
        return NULL;
    }

    /* ---- Resolve owner ---- */
    char owner_buf[129] = {0};
    char name_buf [129] = {0};

    if (!object_owner || object_owner[0] == '\0')
    {
        /* Use the logged-on username from config */
        strncpy(owner_buf, ctx->ini->username, sizeof(owner_buf) - 1);
        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "No owner supplied - using logged-on user='%s'",
                     owner_buf);
    }
    else
    {
        strncpy(owner_buf, object_owner, sizeof(owner_buf) - 1);
    }

    strncpy(name_buf, object_name, sizeof(name_buf) - 1);

    /* Upper-case both */
    for (char *p = owner_buf; *p; p++) *p = (char)toupper((unsigned char)*p);
    for (char *p = name_buf;  *p; p++) *p = (char)toupper((unsigned char)*p);

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Entering get_table_metadata owner='%s' table='%s'",
                 owner_buf, name_buf);

    /* ---- Allocate return struct ---- */
    tm = calloc(1, sizeof(table_metadata_alltabs_t));
    if (!tm)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for table_metadata_alltabs_t");
        return NULL;
    }

    /* Initialise all doubles to -1.0 (= NULL / not applicable) */
    tm->pct_free                  = -1.0;
    tm->pct_used                  = -1.0;
    tm->ini_trans                 = -1.0;
    tm->max_trans                 = -1.0;
    tm->initial_extent            = -1.0;
    tm->next_extent               = -1.0;
    tm->min_extents               = -1.0;
    tm->max_extents               = -1.0;
    tm->pct_increase              = -1.0;
    tm->freelists                 = -1.0;
    tm->freelist_groups           = -1.0;
    tm->num_rows                  = -1.0;
    tm->blocks                    = -1.0;
    tm->empty_blocks              = -1.0;
    tm->avg_space                 = -1.0;
    tm->chain_cnt                 = -1.0;
    tm->avg_row_len               = -1.0;
    tm->avg_space_freelist_blocks = -1.0;
    tm->num_freelist_blocks       = -1.0;
    tm->sample_size               = -1.0;

    /* ================================================================
     *  Build SELECT - fetch every ALL_TABLES column as VARCHAR2 via
     *  TO_CHAR() for NUMBERs and the DATE so we only need one OCI
     *  type code (SQLT_STR) for all 89 columns.  This keeps the bind
     *  setup simple and avoids indicator complexity for each numeric.
     * ================================================================ */
    const char *sql =
        "SELECT "
        "  OWNER,"
        "  TABLE_NAME,"
        "  TABLESPACE_NAME,"
        "  CLUSTER_NAME,"
        "  IOT_NAME,"
        "  STATUS,"
        "  TO_CHAR(PCT_FREE),"
        "  TO_CHAR(PCT_USED),"
        "  TO_CHAR(INI_TRANS),"
        "  TO_CHAR(MAX_TRANS),"
        "  TO_CHAR(INITIAL_EXTENT),"
        "  TO_CHAR(NEXT_EXTENT),"
        "  TO_CHAR(MIN_EXTENTS),"
        "  TO_CHAR(MAX_EXTENTS),"
        "  TO_CHAR(PCT_INCREASE),"
        "  TO_CHAR(FREELISTS),"
        "  TO_CHAR(FREELIST_GROUPS),"
        "  LOGGING,"
        "  BACKED_UP,"
        "  TO_CHAR(NUM_ROWS),"
        "  TO_CHAR(BLOCKS),"
        "  TO_CHAR(EMPTY_BLOCKS),"
        "  TO_CHAR(AVG_SPACE),"
        "  TO_CHAR(CHAIN_CNT),"
        "  TO_CHAR(AVG_ROW_LEN),"
        "  TO_CHAR(AVG_SPACE_FREELIST_BLOCKS),"
        "  TO_CHAR(NUM_FREELIST_BLOCKS),"
        "  DEGREE,"
        "  INSTANCES,"
        "  CACHE,"
        "  TABLE_LOCK,"
        "  TO_CHAR(SAMPLE_SIZE),"
        "  TO_CHAR(LAST_ANALYZED,'" METADATA_TIMESTAMP_FORMAT_SQL "'),"
        "  PARTITIONED,"
        "  IOT_TYPE,"
        "  TEMPORARY,"
        "  SECONDARY,"
        "  NESTED,"
        "  BUFFER_POOL,"
        "  FLASH_CACHE,"
        "  CELL_FLASH_CACHE,"
        "  ROW_MOVEMENT,"
        "  GLOBAL_STATS,"
        "  USER_STATS,"
        "  DURATION,"
        "  SKIP_CORRUPT,"
        "  MONITORING,"
        "  CLUSTER_OWNER,"
        "  DEPENDENCIES,"
        "  COMPRESSION,"
        "  COMPRESS_FOR,"
        "  DROPPED,"
        "  READ_ONLY,"
        "  SEGMENT_CREATED,"
        "  RESULT_CACHE,"
        "  CLUSTERING,"
        "  ACTIVITY_TRACKING,"
        "  DML_TIMESTAMP,"
        "  HAS_IDENTITY,"
        "  CONTAINER_DATA,"
        "  INMEMORY,"
        "  INMEMORY_PRIORITY,"
        "  INMEMORY_DISTRIBUTE,"
        "  INMEMORY_COMPRESSION,"
        "  INMEMORY_DUPLICATE,"
        "  DEFAULT_COLLATION,"
        "  DUPLICATED,"
        "  SYNCHRONOUS_DUPLICATED,"
        "  SHARDED,"
        "  EXTERNALLY_SHARDED,"
        "  EXTERNALLY_DUPLICATED,"
        "  EXTERNAL,"
        "  HYBRID,"
        "  CELLMEMORY,"
        "  CONTAINERS_DEFAULT,"
        "  CONTAINER_MAP,"
        "  EXTENDED_DATA_LINK,"
        "  EXTENDED_DATA_LINK_MAP,"
        "  INMEMORY_SERVICE,"
        "  INMEMORY_SERVICE_NAME,"
        "  CONTAINER_MAP_OBJECT,"
        "  MEMOPTIMIZE_READ,"
        "  MEMOPTIMIZE_WRITE,"
        "  HAS_SENSITIVE_COLUMN,"
        "  ADMIT_NULL,"
        "  DATA_LINK_DML_ENABLED,"
        "  LOGICAL_REPLICATION,"
        "  STAGING,"
        "  ROW_CHANGE_TRACKING,"
        "  HAS_RESERVABLE_COLUMN,"
        "  VECTOR_INDEX_TYPE "
        "FROM ALL_TABLES "
        "WHERE OWNER      = :owner "
        "AND   TABLE_NAME = :tname "
        "AND   ROWNUM     = 1";

    /* ================================================================
     *  Prepare
     * ================================================================ */
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCIStmtPrepare2");

    CHECK_OCI_META(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql, (ub4)strlen(sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    /* ================================================================
     *  Bind :owner and :tname
     * ================================================================ */
    OCIBind *bind_owner = NULL;
    OCIBind *bind_tname = NULL;

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt, &bind_owner, ctx->errhp,
                      (text *)":owner", -1,
                      (dvoid *)owner_buf, (sb4)(strlen(owner_buf) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_META(ctx->errhp,
        OCIBindByName(stmt, &bind_tname, ctx->errhp,
                      (text *)":tname", -1,
                      (dvoid *)name_buf, (sb4)(strlen(name_buf) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    /* ================================================================
     *  Define all 89 output columns as SQLT_STR into fixed buffers.
     *  Using a local array of pointers keeps the define loop compact.
     * ================================================================ */

    /* Each fetch buffer is 1024 bytes - large enough for any column
     * including INMEMORY_SERVICE_NAME VARCHAR2(1000).               */
#define TM_BUF_SIZE  1024
#define TM_COL_COUNT   89

    char      fetch_bufs[TM_COL_COUNT][TM_BUF_SIZE];
    sb2       indicators[TM_COL_COUNT];
    ub2       ret_lens  [TM_COL_COUNT];
    OCIDefine *defs     [TM_COL_COUNT];

    memset(fetch_bufs, 0, sizeof(fetch_bufs));
    memset(indicators, 0, sizeof(indicators));
    memset(ret_lens,   0, sizeof(ret_lens));
    memset(defs,       0, sizeof(defs));

    for (int c = 0; c < TM_COL_COUNT; c++)
    {
        CHECK_OCI_META(ctx->errhp,
            OCIDefineByPos(stmt, &defs[c], ctx->errhp,
                           (ub4)(c + 1),
                           fetch_bufs[c],
                           TM_BUF_SIZE,
                           SQLT_STR,
                           &indicators[c],
                           &ret_lens[c],
                           NULL,
                           OCI_DEFAULT),
            ctx, Cleanup);
    }

    /* ================================================================
     *  Execute and fetch one row
     * ================================================================ */
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCIStmtExecute");

    CHECK_OCI_META(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCIStmtFetch2");

    sword fetch_rc = OCIStmtFetch2(stmt, ctx->errhp,
                                    1, OCI_FETCH_NEXT,
                                    0, OCI_DEFAULT);

    if (fetch_rc == OCI_NO_DATA)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "Table '%s.%s' not found in ALL_TABLES",
                     owner_buf, name_buf);
        free(tm);
        tm = NULL;
        goto Cleanup;
    }

    CHECK_OCI_META(ctx->errhp, fetch_rc, ctx, Cleanup);

    /* ================================================================
     *  Helper macro - copy a fetch buffer into the struct field.
     *  If the indicator is -1 (NULL) leave the field as empty/zero.
     * ================================================================ */
#define COPY_STR(field, col_idx) \
    if (indicators[(col_idx)] != -1) \
        safe_copy(tm->field, sizeof(tm->field), fetch_bufs[(col_idx)])

#define COPY_NUM(field, col_idx) \
    if (indicators[(col_idx)] != -1 && fetch_bufs[(col_idx)][0] != '\0') \
        tm->field = atof(fetch_bufs[(col_idx)])

    /* ================================================================
     *  Populate struct - column order matches the SELECT exactly
     * ================================================================ */
    COPY_STR(owner,                       0);
    COPY_STR(table_name,                  1);
    COPY_STR(tablespace_name,             2);
    COPY_STR(cluster_name,                3);
    COPY_STR(iot_name,                    4);
    COPY_STR(status,                      5);
    COPY_NUM(pct_free,                    6);
    COPY_NUM(pct_used,                    7);
    COPY_NUM(ini_trans,                   8);
    COPY_NUM(max_trans,                   9);
    COPY_NUM(initial_extent,             10);
    COPY_NUM(next_extent,                11);
    COPY_NUM(min_extents,                12);
    COPY_NUM(max_extents,                13);
    COPY_NUM(pct_increase,               14);
    COPY_NUM(freelists,                  15);
    COPY_NUM(freelist_groups,            16);
    COPY_STR(logging,                    17);
    COPY_STR(backed_up,                  18);
    COPY_NUM(num_rows,                   19);
    COPY_NUM(blocks,                     20);
    COPY_NUM(empty_blocks,               21);
    COPY_NUM(avg_space,                  22);
    COPY_NUM(chain_cnt,                  23);
    COPY_NUM(avg_row_len,                24);
    COPY_NUM(avg_space_freelist_blocks,  25);
    COPY_NUM(num_freelist_blocks,        26);
    COPY_STR(degree,                     27);
    COPY_STR(instances,                  28);
    COPY_STR(cache,                      29);
    COPY_STR(table_lock,                 30);
    COPY_NUM(sample_size,                31);
    COPY_STR(last_analyzed,              32);
    COPY_STR(partitioned,                33);
    COPY_STR(iot_type,                   34);
    COPY_STR(temporary,                  35);
    COPY_STR(secondary,                  36);
    COPY_STR(nested,                     37);
    COPY_STR(buffer_pool,                38);
    COPY_STR(flash_cache,                39);
    COPY_STR(cell_flash_cache,           40);
    COPY_STR(row_movement,               41);
    COPY_STR(global_stats,               42);
    COPY_STR(user_stats,                 43);
    COPY_STR(duration,                   44);
    COPY_STR(skip_corrupt,               45);
    COPY_STR(monitoring,                 46);
    COPY_STR(cluster_owner,              47);
    COPY_STR(dependencies,               48);
    COPY_STR(compression,                49);
    COPY_STR(compress_for,               50);
    COPY_STR(dropped,                    51);
    COPY_STR(read_only,                  52);
    COPY_STR(segment_created,            53);
    COPY_STR(result_cache,               54);
    COPY_STR(clustering,                 55);
    COPY_STR(activity_tracking,          56);
    COPY_STR(dml_timestamp,              57);
    COPY_STR(has_identity,               58);
    COPY_STR(container_data,             59);
    COPY_STR(inmemory,                   60);
    COPY_STR(inmemory_priority,          61);
    COPY_STR(inmemory_distribute,        62);
    COPY_STR(inmemory_compression,       63);
    COPY_STR(inmemory_duplicate,         64);
    COPY_STR(default_collation,          65);
    COPY_STR(duplicated,                 66);
    COPY_STR(synchronous_duplicated,     67);
    COPY_STR(sharded,                    68);
    COPY_STR(externally_sharded,         69);
    COPY_STR(externally_duplicated,      70);
    COPY_STR(external,                   71);
    COPY_STR(hybrid,                     72);
    COPY_STR(cellmemory,                 73);
    COPY_STR(containers_default,         74);
    COPY_STR(container_map,              75);
    COPY_STR(extended_data_link,         76);
    COPY_STR(extended_data_link_map,     77);
    COPY_STR(inmemory_service,           78);
    COPY_STR(inmemory_service_name,      79);
    COPY_STR(container_map_object,       80);
    COPY_STR(memoptimize_read,           81);
    COPY_STR(memoptimize_write,          82);
    COPY_STR(has_sensitive_column,       83);
    COPY_STR(admit_null,                 84);
    COPY_STR(data_link_dml_enabled,      85);
    COPY_STR(logical_replication,        86);
    COPY_STR(staging,                    87);
    COPY_STR(row_change_tracking,        88);
    /* Note: has_reservable_column = col 89, vector_index_type = col 90
     * but we only have 89 columns (indices 0..88).
     * Recount: OWNER=0 ... VECTOR_INDEX_TYPE=88. Correct.            */

    /* ================================================================
     *  Dump all fields to Metadata_logger at DEBUG level
     * ================================================================ */
    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "get_table_metadata OK: owner='%s' table='%s'",
                 tm->owner, tm->table_name);

    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  tablespace_name    = '%s'", tm->tablespace_name);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  cluster_name       = '%s'", tm->cluster_name);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  iot_name           = '%s'", tm->iot_name);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  status             = '%s'", tm->status);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  pct_free           = %.0f", tm->pct_free);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  pct_used           = %.0f", tm->pct_used);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  ini_trans          = %.0f", tm->ini_trans);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  max_trans          = %.0f", tm->max_trans);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  initial_extent     = %.0f", tm->initial_extent);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  next_extent        = %.0f", tm->next_extent);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  min_extents        = %.0f", tm->min_extents);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  max_extents        = %.0f", tm->max_extents);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  pct_increase       = %.0f", tm->pct_increase);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  freelists          = %.0f", tm->freelists);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  freelist_groups    = %.0f", tm->freelist_groups);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  logging            = '%s'", tm->logging);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  backed_up          = '%s'", tm->backed_up);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  num_rows           = %.0f", tm->num_rows);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  blocks             = %.0f", tm->blocks);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  empty_blocks       = %.0f", tm->empty_blocks);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  avg_space          = %.0f", tm->avg_space);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  chain_cnt          = %.0f", tm->chain_cnt);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  avg_row_len        = %.0f", tm->avg_row_len);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  avg_space_freelist_blocks = %.0f",
                 tm->avg_space_freelist_blocks);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  num_freelist_blocks= %.0f", tm->num_freelist_blocks);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  degree             = '%s'", tm->degree);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  instances          = '%s'", tm->instances);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  cache              = '%s'", tm->cache);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  table_lock         = '%s'", tm->table_lock);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  sample_size        = %.0f", tm->sample_size);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  last_analyzed      = '%s'", tm->last_analyzed);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  partitioned        = '%s'", tm->partitioned);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  iot_type           = '%s'", tm->iot_type);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  temporary          = '%s'", tm->temporary);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  secondary          = '%s'", tm->secondary);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  nested             = '%s'", tm->nested);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  buffer_pool        = '%s'", tm->buffer_pool);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  flash_cache        = '%s'", tm->flash_cache);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  cell_flash_cache   = '%s'", tm->cell_flash_cache);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  row_movement       = '%s'", tm->row_movement);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  global_stats       = '%s'", tm->global_stats);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  user_stats         = '%s'", tm->user_stats);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  duration           = '%s'", tm->duration);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  skip_corrupt       = '%s'", tm->skip_corrupt);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  monitoring         = '%s'", tm->monitoring);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  cluster_owner      = '%s'", tm->cluster_owner);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  dependencies       = '%s'", tm->dependencies);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  compression        = '%s'", tm->compression);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  compress_for       = '%s'", tm->compress_for);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  dropped            = '%s'", tm->dropped);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  read_only          = '%s'", tm->read_only);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  segment_created    = '%s'", tm->segment_created);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  result_cache       = '%s'", tm->result_cache);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  clustering         = '%s'", tm->clustering);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  activity_tracking  = '%s'", tm->activity_tracking);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  dml_timestamp      = '%s'", tm->dml_timestamp);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  has_identity       = '%s'", tm->has_identity);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  container_data     = '%s'", tm->container_data);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  inmemory           = '%s'", tm->inmemory);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  inmemory_priority  = '%s'", tm->inmemory_priority);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  inmemory_distribute= '%s'", tm->inmemory_distribute);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  inmemory_compression='%s'", tm->inmemory_compression);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  inmemory_duplicate = '%s'", tm->inmemory_duplicate);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  default_collation  = '%s'", tm->default_collation);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  duplicated         = '%s'", tm->duplicated);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  synchronous_duplicated='%s'", tm->synchronous_duplicated);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  sharded            = '%s'", tm->sharded);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  externally_sharded = '%s'", tm->externally_sharded);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  externally_duplicated='%s'", tm->externally_duplicated);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  external           = '%s'", tm->external);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  hybrid             = '%s'", tm->hybrid);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  cellmemory         = '%s'", tm->cellmemory);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  containers_default = '%s'", tm->containers_default);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  container_map      = '%s'", tm->container_map);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  extended_data_link = '%s'", tm->extended_data_link);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  extended_data_link_map='%s'", tm->extended_data_link_map);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  inmemory_service   = '%s'", tm->inmemory_service);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  inmemory_service_name='%s'", tm->inmemory_service_name);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  container_map_object='%s'", tm->container_map_object);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  memoptimize_read   = '%s'", tm->memoptimize_read);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  memoptimize_write  = '%s'", tm->memoptimize_write);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  has_sensitive_column='%s'", tm->has_sensitive_column);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  admit_null         = '%s'", tm->admit_null);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  data_link_dml_enabled='%s'", tm->data_link_dml_enabled);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  logical_replication= '%s'", tm->logical_replication);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  staging            = '%s'", tm->staging);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  row_change_tracking= '%s'", tm->row_change_tracking);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  has_reservable_column='%s'", tm->has_reservable_column);
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "  vector_index_type  = '%s'", tm->vector_index_type);

Cleanup:
    if (stmt)
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);

    /* rc is set by CHECK_OCI_META on OCI error */
    if (rc != 0 && tm)
    {
        free(tm);
        tm = NULL;
    }

#undef COPY_STR
#undef COPY_NUM
#undef TM_BUF_SIZE
#undef TM_COL_COUNT

    return tm;
}
