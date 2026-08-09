/*
 * OCI_Insert_Execute_Module.c
 *
 * Stage 3 - Insert Execute Module
 * --------------------------------
 * Executes a bulk INSERT from an already-validated insert_request_t
 * (built by Level 1, or by OCI_Audit_Trail_Manager.c directly for the
 * internal audit-trail insert - both paths converge here).
 *
 * level2_validate_insert() is called internally as this function's own
 * first step, not just trusted to have already run in the caller - so
 * both the client-facing business insert AND the internal audit-trail
 * insert get the exact same validation for free, with zero extra code
 * needed in the audit module. See level2_validate_insert()'s own doc
 * comment in OCI_Level2_Parser.h for the full check list.
 *
 * Mirrors execute_query_batch in reverse - same conventions,
 * same logging discipline, same error handling pattern.
 *
 * Internal structure
 * ------------------
 *   build_insert_ctx_from_request() - populate insert_ctx_t from
 *                                      insert_request_t (replaces the
 *                                      old parse_insert_xml() - no XML
 *                                      parsing happens in this file at
 *                                      all anymore)
 *   build_insert_sql()        - build INSERT INTO ... VALUES (?,?...)
 *   setup_scalar_binds()      - OCIBindByPos + OCIBindArrayOfStruct
 *   handle_blob_insert()      - allocate locator, write file chunked
 *   handle_clob_insert()      - allocate locator, write text/file chunked
 *   execute_insert_batch()    - orchestrate all stages
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "OCI_Connection.h"
#include "OCI_Table_Metadata_Module.h"
#include "metadata_cache.h"
#include "metadata_cache_meta.h"
#include "OCI_Insert_Execute_Module.h"
#include "OCI_Insert_Validate_Module.h"
#include "OCI_Level2_Parser.h"          /* level2_validate_insert()      */
#include "OCI_Response_Writer.h"        /* response_write_dml_xml/json() */
#include "OCI_Transaction_Manager.h"
#include "XML_Helper.h"
#include "logger.h"
#include "metrics.h"
#include "metrics_writer.h"   /* metrics_finalise_and_enqueue() - closure item 5, Stage 2 */
#include "OCI_Audit_Trail_Manager.h"



/* ------------------------------------------------------------------ */
/*  OCI error macro - consistent with rest of project                  */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_INS(errhp, status, ctx, label)                        \
    do {                                                                 \
        if ((status) != OCI_SUCCESS &&                                  \
            (status) != OCI_SUCCESS_WITH_INFO)                          \
        {                                                                \
            text   _errbuf[512];                                         \
            sb4    _errcode = 0;                                         \
            OCIErrorGet((errhp), 1, NULL, &_errcode,                    \
                        _errbuf, sizeof(_errbuf), OCI_HTYPE_ERROR);     \
            logger_write((ctx)->logger, LOG_ERROR, __func__, 0,         \
                         "OCI Error %d: %s", _errcode,                  \
                         (char *)_errbuf);                               \
            rc = -1;                                                     \
            goto label;                                                  \
        }                                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define MAX_INSERT_COLS      1024
#define MAX_INSERT_ROWS      5000    /* hard cap regardless of ini     */
#define MAX_COL_VALUE_SIZE   32768   /* max scalar bind buffer per col */
#define CLOB_FILE_PREFIX     "file://"
#define CLOB_FILE_PREFIX_LEN 7

/* ------------------------------------------------------------------ */
/*  Per-field value for one row                                         */
/*  Named insert_col_value_t (not field_value_t) - this is purely       */
/*  internal to this file's old XML-string parsing/binding pipeline     */
/*  (parse_insert_xml/build_insert_sql/setup_scalar_binds), a different */
/*  shape entirely from the new shared field_value_t in                */
/*  OCI_Request_Response_Types.h (field_name+value, used by             */
/*  insert_request_t). The two collided by name once this file's        */
/*  header started pulling in the new one - renamed rather than         */
/*  touching the new shared struct, since this one is private to this   */
/*  file and the new one is shared with Level 1/Level 2/UPDATE.         */
/* ------------------------------------------------------------------ */
typedef struct {
    char  value[MAX_COL_VALUE_SIZE];
    char *large_value;               /* NULL unless value didn't fit -
                                       * mirrors field_value_t's own
                                       * large_value in
                                       * OCI_Request_Response_Types.h,
                                       * one layer down. Only the CLOB
                                       * write path (handle_clob_insert)
                                       * can ever need this - scalar
                                       * columns are already bounded by
                                       * their real Oracle column length
                                       * via level2_validate_insert(),
                                       * long before this struct exists,
                                       * and BLOB values arrive as a
                                       * file path string, never inline
                                       * content, so neither can
                                       * realistically exceed
                                       * MAX_COL_VALUE_SIZE.             */
    int   is_empty;                   /* 1 = insert_value was empty     */
} insert_col_value_t;

/* Real value for one insert_col_value_t - see its own large_value
 * comment above. Mirrors field_value_get() in
 * OCI_Request_Response_Types.h exactly, one layer down.               */
static const char *insert_col_value_get(const insert_col_value_t *v)
{
    return v->large_value ? v->large_value : v->value;
}

/* Frees every entry's large_value (a calloc'd array's untouched
 * entries are already NULL - safe no-ops) before the caller frees
 * values itself. Needed at every free(ic->values) site now that any
 * entry - reached or not, on an early-exit path partway through the
 * row loop - might carry a heap allocation.                           */
static void free_insert_ctx_values(insert_col_value_t *values,
                                    int row_count, int col_count)
{
    if (!values) return;
    for (int i = 0; i < row_count * col_count; i++)
        free(values[i].large_value);
    free(values);
}

/* ------------------------------------------------------------------ */
/*  Parsed insert context - all rows and fields from XML               */
/* ------------------------------------------------------------------ */
typedef struct {
    int           col_count;
    int           row_count;
    char          table_name[128];
    char          owner     [128];
    /* col_names[col] */
    char          col_names [MAX_INSERT_COLS][128];
    /* values[row][col] - heap allocated after row/col counts known   */
    insert_col_value_t *values;   /* [row * MAX_INSERT_COLS + col]         */
} insert_ctx_t;

/* ------------------------------------------------------------------ */
/*  build_insert_ctx_from_request                                       */
/*  Populates insert_ctx_t directly from an already-parsed              */
/*  insert_request_t - replaces the old parse_insert_xml(); no XML      */
/*  parsing happens in this file at all anymore.                        */
/*                                                                       */
/*  Column list/order is taken from row 0 - level2_validate_insert()'s   */
/*  Check 1b already guarantees every row sets the same SET of columns   */
/*  before this is ever called, but not necessarily in the same ORDER,   */
/*  so every row's fields are looked up BY NAME against row 0's column   */
/*  list (not by position) when filling ic->values. This is more         */
/*  lenient than the old XML-string parser, which assumed position i in  */
/*  every row's <field> list was the same column - a client sending the  */
/*  same columns in a different order per row is equally valid now.      */
/* ------------------------------------------------------------------ */
static int build_insert_ctx_from_request(oci_context_t          *ctx,
                                          const insert_request_t *req,
                                          insert_ctx_t           *ic)
{
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Entering build_insert_ctx_from_request");

    memset(ic, 0, sizeof(*ic));

    strncpy(ic->table_name, req->table_name, sizeof(ic->table_name) - 1);
    strncpy(ic->owner,      req->owner,      sizeof(ic->owner) - 1);

    if (req->row_count <= 0 || !req->rows)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "insert_request_t has no rows");
        return -1;
    }
    if (req->row_count > MAX_INSERT_ROWS)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "row_count=%d exceeds MAX_INSERT_ROWS=%d",
                     req->row_count, MAX_INSERT_ROWS);
        return -1;
    }

    const insert_row_t *row0 = &req->rows[0];
    int col_count = row0->field_count;

    if (col_count <= 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Row 1 has no fields");
        return -1;
    }
    if (col_count > MAX_INSERT_COLS)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Column count %d exceeds MAX_INSERT_COLS=%d",
                     col_count, MAX_INSERT_COLS);
        return -1;
    }

    ic->row_count = req->row_count;
    ic->col_count = col_count;

    for (int c = 0; c < col_count; c++)
        strncpy(ic->col_names[c], row0->fields[c].field_name,
                sizeof(ic->col_names[c]) - 1);

    ic->values = calloc((size_t)ic->row_count * col_count,
                         sizeof(insert_col_value_t));
    if (!ic->values)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for values array (%d x %d x %zu bytes)",
                     ic->row_count, col_count, sizeof(insert_col_value_t));
        return -1;
    }

    for (int r = 0; r < req->row_count; r++)
    {
        const insert_row_t *row = &req->rows[r];

        if (row->field_count != col_count)
        {
            /* level2_validate_insert()'s Check 1b should already have
             * caught this - defense-in-depth, same reasoning as the
             * row_count/max_bulk_inserts guard right after this
             * function's caller.                                       */
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "Row %d has %d fields, expected %d (row 1's count) - "
                         "level2_validate_insert() should have caught this",
                         r + 1, row->field_count, col_count);
            free_insert_ctx_values(ic->values, ic->row_count, col_count); ic->values = NULL;
            return -1;
        }

        for (int c = 0; c < col_count; c++)
        {
            /* Look up this row's field BY NAME against row 0's column
             * list - not by position. See this function's own doc
             * comment for why.                                         */
            const field_value_t *fv = NULL;
            for (int f = 0; f < row->field_count; f++)
            {
                if (strcasecmp(row->fields[f].field_name, ic->col_names[c]) == 0)
                {
                    fv = &row->fields[f];
                    break;
                }
            }

            if (!fv)
            {
                /* Same defense-in-depth note as above - Check 1b already
                 * guarantees this can't happen if Level 2 ran first.    */
                logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                             "Row %d has no value for column '%s' - "
                             "level2_validate_insert() should have caught this",
                             r + 1, ic->col_names[c]);
                free_insert_ctx_values(ic->values, ic->row_count, col_count); ic->values = NULL;
                return -1;
            }

            insert_col_value_t *dest = &ic->values[r * col_count + c];
            const char *real_value = field_value_get(fv);
            size_t real_len = strlen(real_value);

            if (real_len < sizeof(dest->value))
            {
                strncpy(dest->value, real_value, sizeof(dest->value) - 1);
                dest->large_value = NULL;
            }
            else
            {
                /* Doesn't fit even MAX_COL_VALUE_SIZE - mirror
                 * field_value_t's own overflow handling, one layer
                 * down. See insert_col_value_t's doc comment.          */
                dest->large_value = malloc(real_len + 1);
                if (dest->large_value)
                    memcpy(dest->large_value, real_value, real_len + 1);
                strncpy(dest->value, real_value, sizeof(dest->value) - 1);
                dest->value[sizeof(dest->value) - 1] = '\0';
            }
            dest->is_empty = (real_len == 0);
        }
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "build_insert_ctx_from_request OK: rows=%d cols=%d "
                 "allocated=%zu bytes",
                 ic->row_count, ic->col_count,
                 (size_t)ic->row_count * ic->col_count *
                 sizeof(insert_col_value_t));
    return 0;
}

/*  build_insert_sql                                                    */
/*  Builds INSERT INTO owner.table (col1,...) VALUES (expr1,...)        */
/*  Date/Timestamp/Interval columns wrapped with Oracle conversion      */
/*  functions so Oracle converts the string with the correct format     */
/*  rather than relying on NLS session settings (avoids ORA-01861).    */
/* ================================================================== */


/* Return SQL conversion wrapper for a given Oracle type.
 * %s will be replaced with the bind placeholder e.g. :1              */
/*
 * get_bind_wrapper()
 *
 * Writes the SQL wrapper expression for this column's real data type
 * into dest, if one applies. Returns 1 if dest was populated
 * (date/timestamp/interval - still containing exactly one %s
 * placeholder for the caller's own bind-position substitution - or
 * CLOB/BLOB, which takes no placeholder at all), 0 for a plain scalar
 * type (caller uses a bare bind placeholder, no wrapper needed).
 *
 * ctx->ini->nls_date_format is read fresh on every call - no hardcoded
 * date format literal anywhere in this function any more (2026-07-28
 * decision - see OCI_Level2_Parser.c's normalize_client_date_value()
 * for the matching client-side half of this same design: by the time
 * a value reaches this wrapper, Level 2 has already normalized it into
 * whatever nls_date_format currently is, so this wrapper and that
 * normalization step always agree, even if nls_date_format is changed
 * in config.ini - neither one has its own independent, potentially
 * stale copy of the format string any more).
 */
static int get_bind_wrapper(oci_context_t *ctx, const char *dtype,
                             char *dest, size_t dest_max)
{
    if (strcmp(dtype, "DATE") == 0)
    {
        snprintf(dest, dest_max, "TO_DATE(%%s,'%s')", ctx->ini->nls_date_format);
        return 1;
    }
    if (strncmp(dtype, "TIMESTAMP", 9) == 0)
    {
        snprintf(dest, dest_max, "TO_TIMESTAMP(%%s,'%s.FF6')", ctx->ini->nls_date_format);
        return 1;
    }
    if (strstr(dtype, "INTERVAL") != NULL && strstr(dtype, "MONTH") != NULL)
    {
        snprintf(dest, dest_max, "TO_YMINTERVAL(%%s)");
        return 1;
    }
    if (strstr(dtype, "INTERVAL") != NULL && strstr(dtype, "SECOND") != NULL)
    {
        snprintf(dest, dest_max, "TO_DSINTERVAL(%%s)");
        return 1;
    }
    if (strcmp(dtype, "CLOB")  == 0 ||
        strcmp(dtype, "NCLOB") == 0)
    {
        snprintf(dest, dest_max, "EMPTY_CLOB()");
        return 1;
    }
    if (strcmp(dtype, "BLOB") == 0)
    {
        snprintf(dest, dest_max, "EMPTY_BLOB()");
        return 1;
    }

    return 0;   /* plain bind placeholder - no wrapper needed */
}

static int build_insert_sql(oci_context_t        *ctx,
                              const insert_ctx_t   *ic,
                              const col_metadata_t *cols,
                              int                   col_meta_count,
                              char                 *sql_buf,
                              size_t                sql_max)
{
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Building INSERT SQL table='%s'", ic->table_name);

    char col_list [MAX_INSERT_COLS * 132] = {0};
    char bind_list[MAX_INSERT_COLS * 256] = {0};  /* wider for wrappers */

    /*
     * bind_num tracks the OCI placeholder number (:1, :2, ...).
     * It only increments for columns that actually get a bind variable.
     * LOB columns (BLOB, CLOB, NCLOB) emit EMPTY_BLOB()/EMPTY_CLOB()
     * literals with no bind placeholder and must NOT advance bind_num.
     * Without this fix, LOBs in the middle of the column list create
     * gaps in the placeholder sequence (e.g. :4, EMPTY_CLOB(), :7)
     * which causes ORA-01006 bind variable does not exist.
     */
    int bind_num = 0;

    for (int i = 0; i < ic->col_count; i++)
    {
        if (i > 0)
        {
            strncat(col_list,  ", ", sizeof(col_list)  - strlen(col_list)  - 1);
            strncat(bind_list, ", ", sizeof(bind_list) - strlen(bind_list) - 1);
        }
        strncat(col_list, ic->col_names[i],
                sizeof(col_list) - strlen(col_list) - 1);

        /* Find data type for this column in metadata */
        const char *dtype = "VARCHAR2";
        for (int m = 0; m < col_meta_count; m++)
        {
            if (strcasecmp(cols[m].col_name, ic->col_names[i]) == 0)
            {
                dtype = cols[m].data_type;
                break;
            }
        }

        char wrapper_buf[128] = {0};
        int  has_wrapper = get_bind_wrapper(ctx, dtype, wrapper_buf, sizeof(wrapper_buf));
        const char *wrapper = has_wrapper ? wrapper_buf : NULL;

        if (strcmp(dtype, "BLOB")  == 0 ||
            strcmp(dtype, "CLOB")  == 0 ||
            strcmp(dtype, "NCLOB") == 0)
        {
            /* LOB column: emit literal only, no bind placeholder.
             * bind_num is NOT incremented - numbering stays continuous
             * for the scalar columns that follow.                      */
            strncat(bind_list, wrapper,
                    sizeof(bind_list) - strlen(bind_list) - 1);
        }
        else if (wrapper)
        {
            /* Date / Timestamp / Interval: wrap placeholder with
             * Oracle conversion function e.g. TO_DATE(:5,...)         */
            bind_num++;
            char bind_ph[16];
            snprintf(bind_ph, sizeof(bind_ph), ":%d", bind_num);
            char wrapped[128] = {0};
            snprintf(wrapped, sizeof(wrapped), wrapper, bind_ph);
            strncat(bind_list, wrapped,
                    sizeof(bind_list) - strlen(bind_list) - 1);
        }
        else
        {
            /* Plain scalar: emit bind placeholder directly            */
            bind_num++;
            char bind_ph[16];
            snprintf(bind_ph, sizeof(bind_ph), ":%d", bind_num);
            strncat(bind_list, bind_ph,
                    sizeof(bind_list) - strlen(bind_list) - 1);
        }
    }

    int n;
    if (strlen(ic->owner) > 0)
        n = snprintf(sql_buf, sql_max,
                     "INSERT INTO %s.%s (%s) VALUES (%s)",
                     ic->owner, ic->table_name,
                     col_list, bind_list);
    else
        n = snprintf(sql_buf, sql_max,
                     "INSERT INTO %s (%s) VALUES (%s)",
                     ic->table_name, col_list, bind_list);

    if (n < 0 || (size_t)n >= sql_max)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "INSERT SQL truncated - increase sql_buf size");
        return -1;
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "INSERT SQL: %s", sql_buf);
    return 0;
}

/* ================================================================== */
/* ================================================================== */
/*  handle_blob_insert                                                  */
/*  BLOB insert via EMPTY_BLOB() + SELECT FOR UPDATE pattern           */
/*  ------------------------------------------------------------------ */
/*  BLOB columns have EMPTY_BLOB() in the INSERT SQL (generated by     */
/*  get_bind_wrapper). After OCIStmtExecute, this function retrieves   */
/*  the persistent LOB locator via SELECT col FOR UPDATE WHERE ROWID=  */
/*  and writes the file data in chunks.                                */
/*                                                                      */
/*  This avoids temporary LOB instability with OCILobWrite on OCI 23c  */
/*  which causes a silent SIGSEGV on multi-chunk FIRST_PIECE writes.   */
/* ================================================================== */
static int handle_blob_insert(oci_context_t *ctx,
                               const char    *col_name,
                               const char    *table_name,
                               const char    *rowid_str,
                               const char    *file_path,
                               int            is_empty,
                               uint64_t      *bytes_out)
{
    int            rc       = 0;
    OCIStmt       *stmt_sel = NULL;
    OCILobLocator *lob_loc  = NULL;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Entering col='%s' rowid='%s' is_empty=%d",
                 col_name, rowid_str, is_empty);

    if (is_empty)
    {
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "BLOB is empty - EMPTY_BLOB() already in row");
        return 0;
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Failed to open BLOB input file: %s", file_path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0)
    {
        logger_write(ctx->insert_logger, LOG_WARN, __func__, 0,
                     "BLOB input file empty: %s", file_path);
        fclose(fp);
        return 0;
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "BLOB file='%s' size=%ld chunk_size=%lu",
                 file_path, file_size, ctx->ini->chunk_read_size);

    /* SELECT col FOR UPDATE using ROWID to get persistent locator */
    char sql_sel[512];
    snprintf(sql_sel, sizeof(sql_sel),
             "SELECT %s FROM %s WHERE ROWID = :rid FOR UPDATE",
             col_name, table_name);

    logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                 "SELECT FOR UPDATE: %s  rid=%s", sql_sel, rowid_str);

    CHECK_OCI_INS(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt_sel, ctx->errhp,
                        (text *)sql_sel, (ub4)strlen(sql_sel),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_rid = NULL;
    CHECK_OCI_INS(ctx->errhp,
        OCIBindByName(stmt_sel, &bind_rid, ctx->errhp,
                      (text *)":rid", -1,
                      (dvoid *)rowid_str,
                      (sb4)(strlen(rowid_str) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL,
                      OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_INS(ctx->errhp,
        OCIDescriptorAlloc(ctx->envhp, (void **)&lob_loc,
                           OCI_DTYPE_LOB, 0, NULL),
        ctx, Cleanup);

    OCIDefine *def_lob = NULL;
    CHECK_OCI_INS(ctx->errhp,
        OCIDefineByPos(stmt_sel, &def_lob, ctx->errhp, 1,
                       &lob_loc,
                       (sb4)sizeof(OCILobLocator *),
                       SQLT_BLOB, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_INS(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt_sel, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_INS(ctx->errhp,
        OCIStmtFetch2(stmt_sel, ctx->errhp,
                      1, OCI_FETCH_NEXT, 0, OCI_DEFAULT),
        ctx, Cleanup);

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Persistent BLOB locator obtained - writing chunks");

    ub1   *chunk_buf = malloc(ctx->ini->chunk_read_size);
    if (!chunk_buf)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "malloc failed for BLOB chunk buffer");
        fclose(fp);
        rc = -1;
        goto Cleanup;
    }

    ub4    offset          = 1;
    size_t bytes_remaining = (size_t)file_size;

    while (bytes_remaining > 0)
    {
        size_t chunk = ctx->ini->chunk_read_size;
        if (chunk > bytes_remaining) chunk = bytes_remaining;

        size_t nread = fread(chunk_buf, 1, chunk, fp);
        if (nread == 0)
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "fread returned 0 unexpectedly");
            free(chunk_buf);
            fclose(fp);
            rc = -1;
            goto Cleanup;
        }

        ub4 amount = (ub4)nread;

        logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                     "OCILobWrite offset=%u chunk=%zu remaining=%zu",
                     offset, nread, bytes_remaining - nread);

        CHECK_OCI_INS(ctx->errhp,
            OCILobWrite(ctx->svchp, ctx->errhp,
                        lob_loc, &amount, offset,
                        chunk_buf, (ub4)nread,
                        OCI_ONE_PIECE,
                        NULL, NULL, 0, SQLCS_IMPLICIT),
            ctx, Cleanup);

        offset          += (ub4)nread;
        bytes_remaining -= nread;
    }

    free(chunk_buf);
    fclose(fp);
    fp = NULL;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "BLOB write complete size=%ld", file_size);

    if (bytes_out) *bytes_out += (uint64_t)file_size;

Cleanup:
    if (fp)      fclose(fp);
    if (lob_loc) OCIDescriptorFree(lob_loc, OCI_DTYPE_LOB);
    if (stmt_sel) OCIStmtRelease(stmt_sel, ctx->errhp,
                                  NULL, 0, OCI_DEFAULT);
    return rc;
}


/* ================================================================== */
/*  handle_clob_insert                                                  */
/*  CLOB/NCLOB insert via EMPTY_CLOB() + SELECT FOR UPDATE pattern     */
/*  ------------------------------------------------------------------ */
/*  Mirrors handle_blob_insert exactly but for text data.               */
/*  Supports inline text and file:// source.                           */
/* ================================================================== */
static int handle_clob_insert(oci_context_t *ctx,
                               const char    *col_name,
                               const char    *col_type,
                               const char    *table_name,
                               const char    *rowid_str,
                               const char    *insert_value,
                               int            is_empty,
                               uint64_t      *bytes_out)
{
    int            rc       = 0;
    OCIStmt       *stmt_sel = NULL;
    OCILobLocator *lob_loc  = NULL;
    char          *file_buf = NULL;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Entering col='%s' type='%s' rowid='%s' is_empty=%d",
                 col_name, col_type, rowid_str, is_empty);

    if (is_empty)
    {
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "CLOB is empty - EMPTY_CLOB() already in row");
        return 0;
    }

    /* ---- Determine text source ---- */
    const char *text_data = NULL;
    size_t      text_len  = 0;

    if (strncmp(insert_value, CLOB_FILE_PREFIX, CLOB_FILE_PREFIX_LEN) == 0)
    {
        const char *path = insert_value + CLOB_FILE_PREFIX_LEN;
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "CLOB source: file '%s'", path);

        FILE *fp = fopen(path, "r");
        if (!fp)
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "Failed to open CLOB input file: %s", path);
            return -1;
        }
        fseek(fp, 0, SEEK_END);
        long fsz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (fsz > 0)
        {
            file_buf = malloc((size_t)fsz + 1);
            if (!file_buf) { fclose(fp); return -1; }
            text_len = fread(file_buf, 1, (size_t)fsz, fp);
            file_buf[text_len] = '\0';
            text_data = file_buf;
        }
        fclose(fp);
    }
    else
    {
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "CLOB source: inline text len=%zu",
                     strlen(insert_value));
        text_data = insert_value;
        text_len  = strlen(insert_value);
    }

    if (!text_data || text_len == 0)
    {
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "CLOB text is empty - nothing to write");
        if (file_buf) free(file_buf);
        return 0;
    }

    /* ---- SELECT col FOR UPDATE using ROWID ---- */
    char sql_sel[512];
    snprintf(sql_sel, sizeof(sql_sel),
             "SELECT %s FROM %s WHERE ROWID = :rid FOR UPDATE",
             col_name, table_name);

    logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                 "SELECT FOR UPDATE: %s  rid=%s", sql_sel, rowid_str);

    CHECK_OCI_INS(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt_sel, ctx->errhp,
                        (text *)sql_sel, (ub4)strlen(sql_sel),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_rid = NULL;
    CHECK_OCI_INS(ctx->errhp,
        OCIBindByName(stmt_sel, &bind_rid, ctx->errhp,
                      (text *)":rid", -1,
                      (dvoid *)rowid_str,
                      (sb4)(strlen(rowid_str) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL,
                      OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_INS(ctx->errhp,
        OCIDescriptorAlloc(ctx->envhp, (void **)&lob_loc,
                           OCI_DTYPE_LOB, 0, NULL),
        ctx, Cleanup);

    OCIDefine *def_lob = NULL;
    ub2        sqlt    = (strcmp(col_type, "NCLOB") == 0)
                         ? SQLT_CLOB : SQLT_CLOB;  /* both use SQLT_CLOB */

    CHECK_OCI_INS(ctx->errhp,
        OCIDefineByPos(stmt_sel, &def_lob, ctx->errhp, 1,
                       &lob_loc,
                       (sb4)sizeof(OCILobLocator *),
                       sqlt, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_INS(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt_sel, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_INS(ctx->errhp,
        OCIStmtFetch2(stmt_sel, ctx->errhp,
                      1, OCI_FETCH_NEXT, 0, OCI_DEFAULT),
        ctx, Cleanup);

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Persistent CLOB locator obtained - writing text");

    /* ---- Write text in chunks ---- */
    ub4    offset          = 1;
    size_t bytes_remaining = text_len;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Writing CLOB total=%zu chunk_size=%lu",
                 text_len, ctx->ini->chunk_read_size);

    while (bytes_remaining > 0)
    {
        size_t chunk = ctx->ini->chunk_read_size;
        if (chunk > bytes_remaining) chunk = bytes_remaining;

        ub4 amount = (ub4)chunk;

        logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                     "OCILobWrite offset=%u chunk=%zu remaining=%zu",
                     offset, chunk, bytes_remaining - chunk);

        CHECK_OCI_INS(ctx->errhp,
            OCILobWrite(ctx->svchp, ctx->errhp,
                        lob_loc, &amount, offset,
                        (dvoid *)(text_data + (text_len - bytes_remaining)),
                        (ub4)chunk,
                        OCI_ONE_PIECE,
                        NULL, NULL, 0, SQLCS_IMPLICIT),
            ctx, Cleanup);

        offset          += (ub4)chunk;
        bytes_remaining -= chunk;
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "CLOB write complete total=%zu", text_len);

    if (bytes_out) *bytes_out += (uint64_t)text_len;

Cleanup:
    if (file_buf) free(file_buf);
    if (lob_loc)  OCIDescriptorFree(lob_loc, OCI_DTYPE_LOB);
    if (stmt_sel) OCIStmtRelease(stmt_sel, ctx->errhp,
                                  NULL, 0, OCI_DEFAULT);
    return rc;
}


/* ================================================================== */
/*  execute_insert_batch                                                */
/*  Main Stage-3 entry point. Orchestrates all stages.                 */
/* ================================================================== */
int execute_insert_batch(oci_context_t    *ctx,
                          insert_request_t *req,
                          execute_config_t *cfg)
{
    int            rc          = 0;
    OCIStmt       *stmt        = NULL;
    xml_builder_t *xml         = NULL;
    insert_ctx_t  *ic          = NULL;
    OCILobLocator **lob_locs   = NULL;   /* [col] per current row      */
    OCIBind       **bind_hdls  = NULL;   /* [col]                      */
    char          **scalar_bufs= NULL;   /* [col] flat array bind bufs */
    sb2            *indicators  = NULL;   /* [col * max_rows] flat      */
    int             execute_count = 0;   /* rows per OCIStmtExecute    */
    int             rows_inserted = 0;
    int             lob_count     = 0;
    uint64_t        lob_bytes     = 0;   /* total BLOB bytes written    */
    uint64_t        clob_bytes    = 0;   /* total CLOB bytes written    */
    struct timespec ts_start, ts_end;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Entering execute_insert_batch");

    if (!ctx || !req || !cfg)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments: ctx, req or cfg is NULL");
        return -1;
    }

    /* Give this call its own transaction identity if it doesn't already
     * have one - fixes the 2026-07-26 GxP traceability gap where a
     * standalone call's before-image/audit/actual-change metrics rows
     * had no shared transaction_id at all. See the full reasoning in
     * OCI_Transaction_Manager.h's own doc comment for these two
     * functions. owns_standalone_tx tells Cleanup whether it's this
     * call's job to clear ctx->active_tx back to NULL again.           */
    tx_handle_t local_tx;
    int owns_standalone_tx = begin_standalone_tx_if_needed(ctx, &local_tx);


    metrics_record_t metrics;
    metrics_init(&metrics);
    metrics.start_time_us = metrics_now_us();
    strncpy(metrics.operation, "INSERT", sizeof(metrics.operation) - 1);
    /* object_name filled after build_insert_ctx_from_request() succeeds */
    /* Set transaction_id immediately so every write path carries it  */
      if (ctx->active_tx)
          strncpy(metrics.transaction_id,
                  tx_get_id(ctx->active_tx),
                  sizeof(metrics.transaction_id) - 1);
      else
          strncpy(metrics.transaction_id, "-",
                  sizeof(metrics.transaction_id) - 1);
      metrics_set_context(&metrics, ctx);

      /* metrics_set_context() unconditionally copies ctx->level1_parse_us/
       * level2_parse_us - correct for a client-driven request (Test_XML_
       * Runner.c's dispatcher sets those once per file, timing its own
       * level1_parse()/level2_validate() calls), but wrong here: if
       * audit_trail_in_progress is ALREADY 1 when this call begins, this
       * is OCI_Audit_Trail_Manager.c's own nested execute_insert_batch()
       * call for the AUDIT_TRAIL row itself - built directly as a C
       * struct, never touching Level 1/2 at all. Without this, the
       * audit row's own metrics line reports the OUTER business
       * insert's parse timing as if it were its own (confirmed via a
       * real run - every AUDIT_TRAIL metrics row showed identical
       * level1_parse_us/level2_parse_us to the business insert right
       * next to it). Doesn't affect correctness of the actual insert or
       * audit row - only this metrics attribution.                     */
      if (audit_trail_in_progress)
      {
          metrics.level1_parse_us = 0;
          metrics.level2_parse_us = 0;
      }


    /* ================================================================
     *  Stage 1 - Validate all rows
     *  Any validation failure -> abort before touching the database.
     *  Called internally rather than trusted to have already run in
     *  the caller - see this file's own top-of-file doc comment for
     *  why: both the client-facing business insert and the internal
     *  audit-trail insert (OCI_Audit_Trail_Manager.c) get the exact
     *  same validation this way, with zero extra code needed there.
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Validating all rows");

    input_c_operation_t validate_op;
    memset(&validate_op, 0, sizeof(validate_op));
    validate_op.type    = OP_INSERT;
    validate_op.payload = (void *)req;

    operation_status_t val_status;
    memset(&val_status, 0, sizeof(val_status));

    if (level2_validate_insert(ctx, &validate_op, &val_status) != LEVEL2_OK)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Stage 1 validation failed: %s", val_status.error_text);
        return -1;
    }
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 1 validation passed");

    /* ================================================================
     *  Stage 2 - Build insert context and prepare statement
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 2: Building insert context and preparing statement");

    ic = calloc(1, sizeof(insert_ctx_t));
    if (!ic)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for insert_ctx_t");
        rc = -1;
        goto Cleanup;
    }

    if (build_insert_ctx_from_request(ctx, req, ic) != 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "build_insert_ctx_from_request failed");
        rc = -1;
        goto Cleanup;
    }

    strncpy(metrics.object_name, ic->table_name,
            sizeof(metrics.object_name) - 1);


    /* ---- Cap row count at max_bulk_inserts ----
     * Also checked in level2_validate_insert()'s Check 1 above - kept
     * here too as defense-in-depth, same reasoning as that check's own
     * doc comment in OCI_Level2_Parser.h.                              */
    int max_batch = ctx->ini->max_bulk_inserts;
    if (max_batch < 1) max_batch = 1;
    if (ic->row_count > max_batch)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "row_count=%d exceeds max_bulk_inserts=%d",
                     ic->row_count, max_batch);
        rc = -1;
        goto Cleanup;
    }

    /* ---- Load column metadata to get data types ---- */
    col_metadata_t     cols[MAX_INSERT_COLS];
    int                col_meta_count = 0;
    metadata_request_t meta_req;

    memset(&meta_req, 0, sizeof(meta_req));
    strncpy(meta_req.table_name, ic->table_name,
            sizeof(meta_req.table_name) - 1);
    strncpy(meta_req.owner, ic->owner,
            sizeof(meta_req.owner) - 1);

    metadata_cache_result_t meta_result;
    memset(&meta_result, 0, sizeof(meta_result));

    if (metadata_cache_get_or_fetch(ctx->metadata_cache,
									ctx,
									 &meta_req,
									 cols,
									 &col_meta_count,
									 MAX_INSERT_COLS,
									 &meta_result) != 0)
	{
		logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
					 "metadata_cache_get_or_fetch failed");
		rc = -1;
	   goto Cleanup;
	}

    /* Wire metadata cache stats into metrics                          */
    metrics.cache_hit       = meta_result.was_cache_hit;
    metrics.cache_lookup_us = meta_result.cache_lookup_us;
    metrics.cache_key_hash  = meta_result.cache_key_hash;

    /* ---- Build INSERT SQL ---- */
    char sql_buf[65536] = {0};
    if (build_insert_sql(ctx, ic, cols, col_meta_count, sql_buf, sizeof(sql_buf)) != 0)
    {
        rc = -1;
        goto Cleanup;
    }

    /* sql_hash: hash the built SQL for traceability in metrics        */
    if (ctx->metadata_cache)
        metrics.sql_hash = cache_hash_string(ctx->metadata_cache, sql_buf);

    /* ---- Prepare statement ---- */
    CHECK_OCI_INS(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql_buf, (ub4)strlen(sql_buf),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    /* CLOB/BLOB now use EMPTY_CLOB()/EMPTY_BLOB() in SQL with data
     * written post-execute via SELECT FOR UPDATE. No temporary LOB
     * binds so the array bind restriction no longer applies.          */
    execute_count = ic->row_count;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "execute_count=%d rows=%d cols=%d",
                 execute_count, ic->row_count, ic->col_count);

    /* ================================================================
     *  Stage 3 - Allocate bind structures
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 3: Allocating bind structures");

    bind_hdls  = calloc(ic->col_count, sizeof(OCIBind *));
    lob_locs   = calloc(ic->col_count, sizeof(OCILobLocator *));
    scalar_bufs= calloc(ic->col_count, sizeof(char *));
    indicators  = calloc(ic->col_count * execute_count, sizeof(sb2));

    if (!bind_hdls || !lob_locs || !scalar_bufs || !indicators)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for bind structures");
        rc = -1;
        goto Cleanup;
    }

    /* Allocate scalar array bind buffers per column */
    for (int c = 0; c < ic->col_count; c++)
    {
        /* Find metadata for this column */
        const char *dtype = "VARCHAR2";
        int buf_size = MAX_COL_VALUE_SIZE;

        for (int m = 0; m < col_meta_count; m++)
        {
            if (strcasecmp(cols[m].col_name, ic->col_names[c]) == 0)
            {
                dtype = cols[m].data_type;
                /* For scalar types cap buffer to data_length + padding */
                if (cols[m].data_length > 0 &&
                    cols[m].data_length + 64 < MAX_COL_VALUE_SIZE)
                    buf_size = cols[m].data_length + 64;
                break;
            }
        }

        /* LOB columns don't use scalar buffer */
        if (strcmp(dtype, "BLOB")  == 0 ||
            strcmp(dtype, "CLOB")  == 0 ||
            strcmp(dtype, "NCLOB") == 0)
        {
            scalar_bufs[c] = NULL;
            continue;
        }

        scalar_bufs[c] = calloc((size_t)execute_count, (size_t)buf_size);
        if (!scalar_bufs[c])
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "calloc failed for scalar_bufs[%d]", c);
            rc = -1;
            goto Cleanup;
        }
    }

    /* ================================================================
     *  Stage 4 - Execute loop
     *  Process rows in batches of execute_count.
     *  For each batch: bind scalars as array, LOBs row-by-row.
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 4: Execute loop");

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int  row_base          = 0;
    char rowid_str[100]    = {0};   /* ROWID of last inserted row -
                                       populated inside batch loop,
                                       used by audit block after loop  */

    while (row_base < ic->row_count)
    {
        int batch_rows = execute_count;
        if (row_base + batch_rows > ic->row_count)
            batch_rows = ic->row_count - row_base;

        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "Batch: row_base=%d batch_rows=%d",
                     row_base, batch_rows);

        /* ---- Free any LOB locators from previous batch ---- */
        for (int c = 0; c < ic->col_count; c++)
        {
            if (lob_locs[c])
            {
                OCILobFreeTemporary(ctx->svchp, ctx->errhp, lob_locs[c]);
                OCIDescriptorFree(lob_locs[c], OCI_DTYPE_LOB);
                lob_locs[c] = NULL;
            }
        }

        /* ---- Bind all columns for this batch ---- */
        /*
         * bind_pos mirrors the bind_num counter in build_insert_sql().
         * It only increments for non-LOB columns so the position passed
         * to OCIBindByPos matches the placeholder number in the SQL.
         * LOB columns use EMPTY_CLOB()/EMPTY_BLOB() literals with no
         * placeholder and must NOT advance bind_pos.
         * Without this fix, LOBs in the middle of the column list cause
         * OCIBindByPos to reference a placeholder that does not exist,
         * resulting in a crash rather than a clean OCI error.
         */
        int bind_pos = 0;

        for (int c = 0; c < ic->col_count; c++)
        {
            /* Find metadata type for this column */
            const char *dtype    = "VARCHAR2";
            int         buf_size = MAX_COL_VALUE_SIZE;

            for (int m = 0; m < col_meta_count; m++)
            {
                if (strcasecmp(cols[m].col_name, ic->col_names[c]) == 0)
                {
                    dtype = cols[m].data_type;
                    if (cols[m].data_length > 0 &&
                        cols[m].data_length + 64 < MAX_COL_VALUE_SIZE)
                        buf_size = cols[m].data_length + 64;
                    break;
                }
            }

            logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                         "Binding col=%d name='%s' type='%s'",
                         c, ic->col_names[c], dtype);

            /* ---- BLOB ---- */
            /* EMPTY_BLOB() is in the SQL - no bind needed here.
             * Data written after execute via SELECT FOR UPDATE.
             * bind_pos NOT incremented.                                */
            if (strcmp(dtype, "BLOB") == 0)
                continue;

            /* ---- CLOB / NCLOB ---- */
            /* EMPTY_CLOB() is in the SQL - no bind needed here.
             * Data written after execute via SELECT FOR UPDATE.
             * bind_pos NOT incremented.                                */
            if (strcmp(dtype, "CLOB")  == 0 ||
                strcmp(dtype, "NCLOB") == 0)
                continue;

            /* ---- Scalar column - advance bind position ---- */
            bind_pos++;

            /* Fill flat array buffer: row r at offset r*buf_size     */
            for (int r = 0; r < batch_rows; r++)
            {
                int row_idx = row_base + r;
                const insert_col_value_t *fv = &ic->values[row_idx * ic->col_count + c];
                char *slot = scalar_bufs[c] + ((size_t)r * buf_size);
                int   ind_idx = r;

                if (fv->is_empty)
                {
                    slot[0] = '\0';
                    indicators[c * execute_count + ind_idx] = -1; /* NULL */
                }
                else
                {
                    strncpy(slot, fv->value, buf_size - 1);
                    slot[buf_size - 1] = '\0';
                    indicators[c * execute_count + ind_idx] = 0;
                }
            }

            /* Bind using bind_pos - matches placeholder in SQL        */
            CHECK_OCI_INS(ctx->errhp,
                OCIBindByPos(stmt, &bind_hdls[c], ctx->errhp,
                             (ub4)bind_pos,
                             scalar_bufs[c],
                             (sb4)buf_size,
                             SQLT_STR,
                             &indicators[c * execute_count],
                             NULL, NULL, 0, NULL,
                             OCI_DEFAULT),
                ctx, Cleanup);

            if (batch_rows > 1)
            {
                logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                             "OCIBindArrayOfStruct col=%d bind_pos=%d "
                             "buf_size=%d batch_rows=%d",
                             c, bind_pos, buf_size, batch_rows);

                CHECK_OCI_INS(ctx->errhp,
                    OCIBindArrayOfStruct(bind_hdls[c], ctx->errhp,
                                         (ub4)buf_size,
                                         (ub4)sizeof(sb2),
                                         0, 0),
                    ctx, Cleanup);
            }
        }

        /* ---- Execute this batch ---- */
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "Calling OCIStmtExecute iters=%d", batch_rows);

        CHECK_OCI_INS(ctx->errhp,
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                           (ub4)batch_rows,
                           0, NULL, NULL, OCI_DEFAULT),
            ctx, Cleanup);

        uint64_t ins_exec_start_us = metrics_now_us();   /* before execute */
        metrics.execution_us += metrics_now_us() - ins_exec_start_us;
        metrics.rows_affected += (uint64_t)batch_rows;

        rows_inserted += batch_rows;

        /* ---- Always obtain ROWID after execute ----
         * rowid_str declared at function scope above the while loop
         * so it is in scope for the audit block after the loop.       */
        ub2       rid_len  = sizeof(rowid_str) - 1;
        OCIRowid *rid_desc = NULL;
        memset(rowid_str, 0, sizeof(rowid_str));

        if (OCIDescriptorAlloc(ctx->envhp, (void **)&rid_desc,
                               OCI_DTYPE_ROWID, 0, NULL) == OCI_SUCCESS)
        {
            if (OCIAttrGet(stmt, OCI_HTYPE_STMT,
                           rid_desc, NULL,
                           OCI_ATTR_ROWID, ctx->errhp) == OCI_SUCCESS)
            {
                OCIRowidToChar(rid_desc,
                               (OraText *)rowid_str, &rid_len,
                               ctx->errhp);
                rowid_str[rid_len] = '\0';
            }
            OCIDescriptorFree(rid_desc, OCI_DTYPE_ROWID);
            rid_desc = NULL;
        }

        logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                     "Inserted batch ROWID='%s'", rowid_str);

        /* ---- Post-execute: write LOB data via SELECT FOR UPDATE ---- */
        {
            int has_blob = 0;
            for (int bc = 0; bc < ic->col_count && !has_blob; bc++)
                for (int m = 0; m < col_meta_count; m++)
                    if (strcasecmp(cols[m].col_name, ic->col_names[bc])==0
                        && (strcmp(cols[m].data_type, "BLOB")  == 0 ||
                            strcmp(cols[m].data_type, "CLOB")  == 0 ||
                            strcmp(cols[m].data_type, "NCLOB") == 0)
                        && !ic->values[row_base * ic->col_count + bc].is_empty)
                    { has_blob = 1; break; }

            if (has_blob)
            {
                /* rowid_str already obtained above - no second fetch  */
                char tbl_fq[256];
                if (strlen(ic->owner) > 0)
                    snprintf(tbl_fq, sizeof(tbl_fq), "%s.%s",
                             ic->owner, ic->table_name);
                else
                    snprintf(tbl_fq, sizeof(tbl_fq), "%s",
                             ic->table_name);

                for (int bc = 0; bc < ic->col_count; bc++)
                {
                    const char *btype = "VARCHAR2";
                    for (int m = 0; m < col_meta_count; m++)
                        if (strcasecmp(cols[m].col_name,
                                       ic->col_names[bc]) == 0)
                        { btype = cols[m].data_type; break; }

                    const insert_col_value_t *fv =
                        &ic->values[row_base * ic->col_count + bc];
                    if (fv->is_empty) continue;

                    if (strcmp(btype, "BLOB") == 0)
                    {
                        if (handle_blob_insert(ctx, ic->col_names[bc],
                                               tbl_fq, rowid_str,
                                               fv->value, 0,
                                               &lob_bytes) != 0)
                        {
                            logger_write(ctx->insert_logger, LOG_ERROR,
                                         __func__, 0,
                                         "handle_blob_insert failed "
                                         "col=%d row=%d", bc, row_base);
                            rc = -1;
                            goto Cleanup;
                        }
                        lob_count++;
                    }
                    else if (strcmp(btype, "CLOB")  == 0 ||
                             strcmp(btype, "NCLOB") == 0)
                    {
                        if (handle_clob_insert(ctx, ic->col_names[bc],
                                               btype, tbl_fq, rowid_str,
                                               insert_col_value_get(fv), 0,
                                               &clob_bytes) != 0)
                        {
                            logger_write(ctx->insert_logger, LOG_ERROR,
                                         __func__, 0,
                                         "handle_clob_insert failed "
                                         "col=%d row=%d", bc, row_base);
                            rc = -1;
                            goto Cleanup;
                        }
                        lob_count++;
                    }
                }
            }
        }


        row_base += batch_rows;

        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "Batch inserted: rows_inserted=%d", rows_inserted);
    }

    /* ================================================================
     *  Audit Trail - one snapshot row per business row.
     *  audit_trail_insert() detects ACTION_TYPE="INSERT" and dispatches
     *  to audit_trail_insert_snapshot() automatically.
     *  The cycle-guard (audit_trail_in_progress) prevents the audit
     *  insert from triggering a further audit insert.
     * ================================================================ */
    if (!audit_trail_in_progress && rc == 0)
    {
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "Calling audit_trail_insert for table='%s' rows=%d",
                     ic->table_name, ic->row_count);

        audit_trail_request_t atr;
        memset(&atr, 0, sizeof(atr));

        strncpy(atr.table_name,  ic->table_name,
                sizeof(atr.table_name)  - 1);
        strncpy(atr.action_type, "INSERT",
                sizeof(atr.action_type) - 1);
        strncpy(atr.changed_by,  ctx->ini->username,
                sizeof(atr.changed_by)  - 1);
        strncpy(atr.module_name, "OCI_Insert_Execute",
                sizeof(atr.module_name) - 1);

        /* change_reason: use transaction name if available            */
        if (ctx->active_tx && ctx->active_tx->tx_name[0] &&
            strcmp(ctx->active_tx->tx_name, "-") != 0)
            strncpy(atr.change_reason, ctx->active_tx->tx_name,
                    sizeof(atr.change_reason) - 1);
        else
            strncpy(atr.change_reason, "Business INSERT via Data_Manager",
                    sizeof(atr.change_reason) - 1);

        /* record_id: ROWID obtained after OCIStmtExecute above        */
        strncpy(atr.record_id, rowid_str[0] ? rowid_str : "-",
                sizeof(atr.record_id) - 1);

        atr.col_names  = ic->col_names;
        atr.col_types  = cols;
        atr.new_values = ic->values;
        atr.old_values = NULL;           /* INSERT: no old values       */
        atr.row_count  = ic->row_count;
        atr.col_count  = ic->col_count;

        int audit_rc = audit_trail_insert(ctx, &atr);
        if (audit_rc != 0)
            logger_write(ctx->insert_logger, LOG_WARN, __func__, 0,
                         "Audit trail insert failed (rc=%d) for "
                         "table='%s' - business insert is NOT rolled back.",
                         audit_rc, ic->table_name);
    }

    /* ---- Commit (skipped when a managed transaction is active) ---- */
    /*
     * If ctx->active_tx is set, the caller (e.g. Test_XML_Runner) has
     * opened an explicit transaction via tx_begin().  In that case the
     * work inserted here must stay uncommitted so the caller can batch
     * it with other DML steps and commit or roll back the whole unit
     * atomically via tx_commit() / tx_rollback().
     *
     * If ctx->active_tx is NULL this module owns the commit, which is
     * the original standalone behaviour.
     */
    if (ctx->active_tx)
    {
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                      "Skipping OCITransCommit - managed transaction active "
                      "tx_id='%s'  rows_inserted=%d lobs=%d",
                      ctx->active_tx->transaction_id,
                      rows_inserted, lob_count);

    }
    else
    {
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "Calling OCITransCommit (no managed transaction)");

        CHECK_OCI_INS(ctx->errhp,
            OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT),
            ctx, Cleanup);

        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "Commit successful rows_inserted=%d lobs=%d",
                     rows_inserted, lob_count);
    }

    /* ================================================================
     *  Stage 5 - Build result response
     *  Uses response_write_dml_xml()/response_write_dml_json() -
     *  OCI_Response_Writer.c's first writers for anything other than a
     *  SELECT resultset, added as part of this refactor. This closes
     *  the "INSERT doesn't render a JSON response yet" gap that used
     *  to sit in this function's Cleanup block - cfg->OUTPUT_JSON is
     *  now genuinely populated when requested, not a silent XML
     *  fallback.
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 5: Building result response");

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    dml_response_t resp;
    memset(&resp, 0, sizeof(resp));
    strncpy(resp.table_name, ic->table_name, sizeof(resp.table_name) - 1);
    strncpy(resp.owner,      ic->owner,      sizeof(resp.owner) - 1);
    resp.rows_affected          = rows_inserted;
    resp.lobs_written           = lob_count;
    resp.execution_time_seconds = elapsed;
    /* sql_query / resultset_xml_fragment stay NULL - SELECT-only per
     * dml_response_t's own doc comment.                                */

    char *dml_xml_fragment = response_write_dml_xml(ctx, OP_INSERT, &resp);
    if (!dml_xml_fragment)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "response_write_dml_xml returned NULL");
        rc = -1;
        goto Cleanup;
    }

    xml = xml_create(4096);
    if (!xml) { free(dml_xml_fragment); rc = -1; goto Cleanup; }

    xml_start_document(xml);
    xml_start_execution(xml);
    /* xml_append_raw(), not xml_append(xml,"%s",...) - the latter
     * formats into a fixed 8192-byte stack buffer and silently
     * corrupts anything longer (see the 2026-07-22 fix in
     * OCI_Execute_Query_Batch_Module.c for the exact failure mode).
     * This fragment is small today, but there's no reason to
     * reintroduce that risk here for the sake of consistency.         */
    xml_append_raw(xml, dml_xml_fragment);
    xml_end_execution(xml);
    xml_finalize(xml);
    free(dml_xml_fragment);

    /* cfg->OUTPUT_JSON's own doc comment in OCI_Connection.h: "set
     * only when ReturnFormat is JSON. NULL otherwise." - only render
     * JSON when actually requested, not unconditionally.               */
    if (cfg->ReturnFormat && strcasecmp(cfg->ReturnFormat, "JSON") == 0)
    {
        cfg->OUTPUT_JSON = response_write_dml_json(ctx, OP_INSERT, &resp);
        if (!cfg->OUTPUT_JSON)
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "response_write_dml_json returned NULL - "
                         "OUTPUT_JSON will be missing for this JSON-format request");
    }

    metrics.end_time_us      = metrics_now_us();
    metrics.status_code      = 0;
    metrics.rows_affected    = ic->row_count;
    metrics.output_xml_bytes = xml ? (uint64_t)strlen(xml->buffer) : 0;
    metrics.lob_bytes        = lob_bytes;
    metrics.clob_bytes       = clob_bytes;
    /* bytes_processed = output_xml_bytes + lob_bytes + clob_bytes
       computed automatically in metrics_finalise()                    */

    strncpy(metrics.error_code, "-", sizeof(metrics.error_code) - 1);
    strncpy(metrics.error_text, "-", sizeof(metrics.error_text) - 1);


    if (!cfg->xml)
        cfg->xml = calloc(1, sizeof(*cfg->xml));

    logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                 "Setting cfg->xml->OUTPUT_XML");
    cfg->xml->OUTPUT_XML = strdup(xml->buffer);

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "execute_insert_batch complete: table='%s' "
                 "rows=%d elapsed=%.6f",
                 ic->table_name, rows_inserted, elapsed);

Cleanup:
	metrics.end_time_us = metrics_now_us();
	metrics.status_code = rc;
	/* Error path - in Cleanup label when rc != 0 */
	if (rc != 0)
	{
	    strncpy(metrics.error_code,
	            logger_last_error.error_code,
	            sizeof(metrics.error_code) - 1);
	    strncpy(metrics.error_text,
	            logger_last_error.error_text,
	            sizeof(metrics.error_text) - 1);
	}
	if(ctx->active_tx)
		strncpy(metrics.transaction_id , tx_get_id(ctx->active_tx),sizeof(tx_get_id(ctx->active_tx))-1);
	else
		strncpy(metrics.transaction_id , "-",sizeof("-")-1);
	metrics.connection_wait_us    = ctx->connection_wait_us;
	metrics.connection_create_us  = ctx->connection_create_us;
	metrics.connection_acquire_us = ctx->connection_acquire_us;

	//Process final 3 metrics
	//printf("DEBUG : cfg->input_file_name=%s\n",cfg->input_file_name);
	//printf("DEBUG : ctx->ini->metrics_display_input_file_name=%d\n",ctx->ini->metrics_display_input_file_name);
	if (ctx->ini && ctx->ini->metrics_display_input_file_name && cfg->input_file_name)
	    metrics.input_file_name = flatten_for_csv(cfg->input_file_name);
	//printf("DEBUG : metrics.input_request=%s\n",metrics.input_request);

	//printf("DEBUG : OCI_execute_query)batch.c ctx->INPUT_XML=%s\n",ctx->INPUT_XML);
	//printf("DEBUG : OCI_execute_query)batch.c  ctx->ini->metrics_display_input_request=%d\n",ctx->ini->metrics_display_input_request);
	if (ctx->ini && ctx->ini->metrics_display_input_request && ctx->INPUT_XML)
	    metrics.input_request = flatten_for_csv3(ctx->INPUT_XML);
	//printf("DEBUG :  OCI_execute_query)batch.c  metrics.input_request=%s\n",metrics.input_request);


	//printf("DEBUG : ctx->OUTPUT_XML=%s\n",ctx->OUTPUT_XML);
	//printf("DEBUG : ctx->ini->metrics_display_output_response=%d\n",ctx->ini->metrics_display_output_response);
	if (ctx->ini && ctx->ini->metrics_display_output_response)
	{
	    /* INSERT now renders a real JSON response too (Stage 5 above,
	     * via response_write_dml_json()) when cfg->ReturnFormat is
	     * JSON - cfg->OUTPUT_JSON is genuinely populated in that case,
	     * not a placeholder. This check's own logic didn't need to
	     * change - it already preferred OUTPUT_JSON when present and
	     * fell back to XML otherwise; it was only ever falling back
	     * because OUTPUT_JSON was never actually set before now.       */
	    int is_json = (cfg->ReturnFormat &&
	                   strcasecmp(cfg->ReturnFormat, "JSON") == 0);

	    if (is_json && cfg->OUTPUT_JSON)
	        metrics.output_response = flatten_for_csv3(cfg->OUTPUT_JSON);
	    else if (cfg->xml && cfg->xml->OUTPUT_XML)
	        metrics.output_response = flatten_for_csv3(cfg->xml->OUTPUT_XML);
	}
	//printf("DEBUG : metrics.output_response=%s\n",metrics.output_response);




	metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &metrics);
    logger_clear_last_error();   // reset for next operation


/* ================================================================
     *  Stage 6 - Cleanup: reverse allocation order, all guards
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0, "Stage 6: Cleanup");

    /* Rollback on any error (skipped when a managed transaction is active) */
    if (rc != 0 && rows_inserted > 0)
    {
        if (ctx->active_tx)
        {
            logger_write(ctx->insert_logger, LOG_WARN, __func__, 0,
                         "Error detected but managed transaction is active - "
                         "leaving rollback to tx_abort()  tx_id='%s'",
                         ctx->active_tx->transaction_id);
        }
        else
        {
            logger_write(ctx->insert_logger, LOG_WARN, __func__, 0,
                         "Rolling back due to error (no managed transaction)");
            OCITransRollback(ctx->svchp, ctx->errhp, OCI_DEFAULT);
        }
    }

    /* Free LOB locators */
    if (lob_locs)
    {
        for (int c = 0; c < ic->col_count; c++)
        {
            if (lob_locs[c])
            {
                logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                             "OCILobFreeTemporary + DescriptorFree col=%d",
                             c);
                OCILobFreeTemporary(ctx->svchp, ctx->errhp, lob_locs[c]);
                OCIDescriptorFree(lob_locs[c], OCI_DTYPE_LOB);
                lob_locs[c] = NULL;
            }
        }
        free(lob_locs);
        lob_locs = NULL;
    }

    /* Free scalar buffers */
    if (scalar_bufs)
    {
        for (int c = 0; c < ic->col_count; c++)
        {
            if (scalar_bufs[c])
            {
                logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                             "free(scalar_bufs[%d])", c);
                free(scalar_bufs[c]);
                scalar_bufs[c] = NULL;
            }
        }
        free(scalar_bufs);
        scalar_bufs = NULL;
    }

    if (indicators) { free(indicators); indicators = NULL; }
    if (bind_hdls)  { free(bind_hdls);  bind_hdls  = NULL; }
    if (ic)
    {
        if (ic->values) { free_insert_ctx_values(ic->values, ic->row_count, ic->col_count); ic->values = NULL; }
        free(ic);
        ic = NULL;
    }

    if (xml)
    {
        logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0, "xml_free");
        xml_free(xml);
        xml = NULL;
    }

    if (stmt)
    {
        logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                     "OCIStmtRelease stmt");
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        stmt = NULL;
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Cleanup complete rc=%d", rc);

    end_standalone_tx_if_owned(ctx, owns_standalone_tx);

    return rc;
}
