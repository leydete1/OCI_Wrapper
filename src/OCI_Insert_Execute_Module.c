/*
 * OCI_Insert_Execute_Module.c
 *
 * Stage 3 - Insert Execute Module
 * --------------------------------
 * Executes a bulk INSERT from a validated <Insert_Template> XML.
 * Mirrors execute_query_batch in reverse - same conventions,
 * same logging discipline, same error handling pattern.
 *
 * Internal structure
 * ------------------
 *   parse_insert_xml()        - parse XML into row/field arrays
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
#include "OCI_Transaction_Manager.h"
#include "XML_Helper.h"
#include "logger.h"
#include "metrics.h"
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
/* ------------------------------------------------------------------ */
typedef struct {
    char value[MAX_COL_VALUE_SIZE];
    int  is_empty;                   /* 1 = insert_value was empty     */
} field_value_t;

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
    field_value_t *values;   /* [row * MAX_INSERT_COLS + col]         */
} insert_ctx_t;

/* ------------------------------------------------------------------ */
/*  Static helpers                                                      */
/* ------------------------------------------------------------------ */
static void trim_ins(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    { s[len - 1] = '\0'; len--; }
}

/* Extract text between <tag> and </tag> - returns 1 on success */
static int extract_tag_ins(const char *src, const char *tag,
                            char *dest, size_t dest_max)
{
    if (!src || !tag || !dest) return 0;
    char open [132], close[132];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *s = strstr(src, open);
    if (!s) return 0;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return 0;
    size_t len = (size_t)(e - s);
    if (len >= dest_max) len = dest_max - 1;
    memcpy(dest, s, len);
    dest[len] = '\0';
    trim_ins(dest);
    return 1;
}

/* ================================================================== */
/* ================================================================== */
/*  parse_insert_xml                                                    */
/*  Two-pass parse: pass 1 counts rows/cols, allocates exact memory,   */
/*  pass 2 extracts all values. This avoids the MAX_ROWS*MAX_COLS*32KB  */
/*  upfront allocation that would require ~163GB.                       */
/* ================================================================== */
static int parse_insert_xml(oci_context_t *ctx,
                              const char    *xml,
                              insert_ctx_t  *ic)
{
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Entering parse_insert_xml");

    memset(ic, 0, sizeof(*ic));

    /* ---- Table name and owner ---- */
    if (!extract_tag_ins(xml, "table_name",
                         ic->table_name, sizeof(ic->table_name)))
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Missing <table_name> in template XML");
        return -1;
    }
    extract_tag_ins(xml, "owner", ic->owner, sizeof(ic->owner));

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "table='%s' owner='%s'", ic->table_name, ic->owner);

    /* ================================================================
     *  Pass 1: Count rows and columns
     * ================================================================ */
    int      row_count = 0;
    int      col_count = 0;
    const char *cursor = xml;

    while ((cursor = strstr(cursor, "<row ")) != NULL)
    {
        if (row_count >= MAX_INSERT_ROWS)
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "Row count exceeds MAX_INSERT_ROWS=%d",
                         MAX_INSERT_ROWS);
            return -1;
        }

        const char *row_end = strstr(cursor, "</row>");
        if (!row_end) break;

        /* Count fields in first row only */
        if (row_count == 0)
        {
            const char *fp = cursor;
            while ((fp = strstr(fp, "<field>")) != NULL &&
                   fp < row_end)
            {
                col_count++;
                fp += 7;
            }
        }

        row_count++;
        cursor = row_end + 6;
    }

    if (row_count == 0 || col_count == 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "No rows or columns found in template XML");
        return -1;
    }

    if (col_count > MAX_INSERT_COLS)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Column count %d exceeds MAX_INSERT_COLS=%d",
                     col_count, MAX_INSERT_COLS);
        return -1;
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Pass 1 complete: rows=%d cols=%d", row_count, col_count);

    /* ================================================================
     *  Allocate exact memory: rows * cols * sizeof(field_value_t)
     * ================================================================ */
    ic->values = calloc((size_t)row_count * col_count,
                        sizeof(field_value_t));
    if (!ic->values)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for values array (%d x %d x %zu bytes)",
                     row_count, col_count, sizeof(field_value_t));
        return -1;
    }

    ic->row_count = row_count;
    ic->col_count = col_count;

    /* ================================================================
     *  Pass 2: Extract col_names and all insert_value fields
     * ================================================================ */
    cursor = xml;
    int row_idx = 0;

    while ((cursor = strstr(cursor, "<row ")) != NULL)
    {
        const char *row_end = strstr(cursor, "</row>");
        if (!row_end) break;

        size_t row_len = (size_t)(row_end - cursor) + 6;
        char  *row_buf = malloc(row_len + 1);
        if (!row_buf)
        {
            free(ic->values); ic->values = NULL;
            return -1;
        }
        memcpy(row_buf, cursor, row_len);
        row_buf[row_len] = '\0';

        const char *fp        = row_buf;
        int         col_idx   = 0;

        while ((fp = strstr(fp, "<field>")) != NULL)
        {
            const char *fe = strstr(fp, "</field>");
            if (!fe || col_idx >= col_count) break;

            size_t flen = (size_t)(fe - fp) + 8;
            char  *fbuf = malloc(flen + 1);
            if (!fbuf) { free(row_buf); free(ic->values); ic->values = NULL; return -1; }
            memcpy(fbuf, fp, flen);
            fbuf[flen] = '\0';

            /* Extract col_name from first row only */
            if (row_idx == 0)
                extract_tag_ins(fbuf, "field_name",
                                ic->col_names[col_idx],
                                sizeof(ic->col_names[col_idx]));

            /* Extract insert_value into flat array */
            field_value_t *fv = &ic->values[row_idx * col_count + col_idx];
            memset(fv, 0, sizeof(*fv));

            if (!extract_tag_ins(fbuf, "insert_value",
                                 fv->value, sizeof(fv->value)))
                fv->value[0] = '\0';

            fv->is_empty = (strlen(fv->value) == 0);

            free(fbuf);
            col_idx++;
            fp = fe + 8;
        }

        if (col_idx != col_count)
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "Row %d has %d fields, expected %d",
                         row_idx + 1, col_idx, col_count);
            free(row_buf);
            free(ic->values); ic->values = NULL;
            return -1;
        }

        free(row_buf);
        row_idx++;
        cursor = row_end + 6;
    }

    /* Fix all flat index accesses to use ic->col_count */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "parse_insert_xml OK: rows=%d cols=%d "
                 "allocated=%zu bytes",
                 ic->row_count, ic->col_count,
                 (size_t)ic->row_count * ic->col_count *
                 sizeof(field_value_t));
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
static const char *get_bind_wrapper(const char *dtype)
{
    if (strcmp(dtype, "DATE") == 0)
        return "TO_DATE(%s,'YYYY-MM-DD HH24:MI:SS')";
    if (strncmp(dtype, "TIMESTAMP", 9) == 0)
        return "TO_TIMESTAMP(%s,'YYYY-MM-DD HH24:MI:SS.FF6')";
    if (strstr(dtype, "INTERVAL") != NULL && strstr(dtype, "MONTH") != NULL)
        return "TO_YMINTERVAL(%s)";
    if (strstr(dtype, "INTERVAL") != NULL && strstr(dtype, "SECOND") != NULL)
        return "TO_DSINTERVAL(%s)";
    if (strcmp(dtype, "CLOB")  == 0 ||
        strcmp(dtype, "NCLOB") == 0)
        return "EMPTY_CLOB()";

    if (strcmp(dtype, "BLOB") == 0)
        return "EMPTY_BLOB()";

    return NULL;   /* plain bind placeholder - no wrapper needed */
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

        const char *wrapper = get_bind_wrapper(dtype);

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
                          const char       *template_xml,
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

    if (!ctx || !template_xml || !cfg)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments: ctx, template_xml or cfg is NULL");
        return -1;
    }


    metrics_record_t metrics;
    metrics_init(&metrics);
    metrics.start_time_us = metrics_now_us();
    strncpy(metrics.operation, "INSERT", sizeof(metrics.operation) - 1);
    /* object_name filled after parse_insert_xml() succeeds */
    /* Set transaction_id immediately so every write path carries it  */
      if (ctx->active_tx)
          strncpy(metrics.transaction_id,
                  tx_get_id(ctx->active_tx),
                  sizeof(metrics.transaction_id) - 1);
      else
          strncpy(metrics.transaction_id, "-",
                  sizeof(metrics.transaction_id) - 1);
      metrics_set_context(&metrics, ctx);


    /* ================================================================
     *  Stage 1 - Validate all rows
     *  Any validation failure -> abort before touching the database.
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Validating all rows");

    char val_error[512] = {0};
    if (validate_insert_template(ctx, template_xml,
                                  val_error, sizeof(val_error)) != 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Stage 1 validation failed: %s", val_error);
        return -1;
    }
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 1 validation passed");

    /* ================================================================
     *  Stage 2 - Parse XML and prepare statement
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 2: Parsing XML and preparing statement");

    ic = calloc(1, sizeof(insert_ctx_t));
    if (!ic)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for insert_ctx_t");
        rc = -1;
        goto Cleanup;
    }

    if (parse_insert_xml(ctx, template_xml, ic) != 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "parse_insert_xml failed");
        rc = -1;
        goto Cleanup;
    }

    strncpy(metrics.object_name, ic->table_name,
            sizeof(metrics.object_name) - 1);


    /* ---- Cap row count at max_bulk_inserts ---- */
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
                const field_value_t *fv = &ic->values[row_idx * ic->col_count + c];
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

                    const field_value_t *fv =
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
                                               fv->value, 0,
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
     *  Stage 5 - Build result XML
     * ================================================================ */
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Stage 5: Building result XML");

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    xml = xml_create(4096);
    if (!xml) { rc = -1; goto Cleanup; }

    xml_start_document(xml);
    xml_start_execution(xml);
    xml_append(xml, "<operation>INSERT</operation>\n");
    xml_append(xml, "<table_name>%s</table_name>\n", ic->table_name);
    xml_append(xml, "<owner>%s</owner>\n",           ic->owner);
    xml_append(xml, "<rows_inserted>%d</rows_inserted>\n", rows_inserted);
    xml_append(xml, "<lobs_written>%d</lobs_written>\n",   lob_count);
    xml_append(xml, "<execution_time>%.6f</execution_time>\n", elapsed);
    xml_append(xml, "<execute_batch_size>%d</execute_batch_size>\n",execute_count);
    xml_end_execution(xml);
    xml_finalize(xml);
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
	if (ctx->ini && ctx->ini->metrics_display_output_response &&
	    cfg->xml && cfg->xml->OUTPUT_XML)
	    metrics.output_response = flatten_for_csv3(cfg->xml->OUTPUT_XML);
	//printf("DEBUG : metrics.output_response=%s\n",metrics.output_response);




	metrics_finalise(&metrics);
	metrics_write(ctx->metrics_logger, &metrics);
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
        if (ic->values) { free(ic->values); ic->values = NULL; }
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
    return rc;
}
