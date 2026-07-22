
/*
 * OCI_Update_Execute_Module.c
 *
 * Stage 3 - Update Execute Module
 * --------------------------------
 * Executes a bulk UPDATE from a validated <Update_Template> XML.
 * Mirrors OCI_Insert_Execute_Module in structure and conventions.
 *
 * Key differences from insert:
 *   - XML contains a <where> block with <key_field> entries.
 *   - SET columns  = <row> fields (the update values).
 *   - WHERE columns = <where> key_fields (identify rows to update).
 *   - SQL: UPDATE owner.table SET col=:1,... WHERE key=:N,...
 *   - BLOB/CLOB in SET: same EMPTY_BLOB()/EMPTY_CLOB() +
 *     SELECT FOR UPDATE pattern as insert.
 *   - WHERE key columns always bind as SQLT_STR scalars.
 *
 * Reuses:
 *   - OCI_Insert_Validate_Module  (Stage 2 validation unchanged)
 *   - OCI_Table_Metadata_Module   (get_request_metadata)
 *   - handle_blob_update / handle_clob_update (identical logic to
 *     insert counterparts, renamed for clarity)
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


#include "OCI_Update_Execute_Module.h"
#include "OCI_Insert_Validate_Module.h"
#include "OCI_Audit_Trail_Manager.h"
#include "XML_Helper.h"
#include "logger.h"
#include "metrics.h"
#include "OCI_Transaction_Manager.h"

/* ------------------------------------------------------------------ */
/*  OCI error macro                                                     */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_UPD(errhp, status, ctx, label)                        \
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
#define MAX_UPD_COLS         1024
#define MAX_UPD_ROWS         5000
#define MAX_UPD_KEY_COLS     32
#define MAX_COL_VALUE_SIZE   32768
#define CLOB_FILE_PREFIX     "file://"
#define CLOB_FILE_PREFIX_LEN 7

/* ------------------------------------------------------------------ */
/*  Per-field value                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char value[MAX_COL_VALUE_SIZE];
    int  is_empty;
} upd_field_value_t;

/* ------------------------------------------------------------------ */
/*  Parsed WHERE key field                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    char field_name[128];
    char field_type[128];
    char key_value [MAX_COL_VALUE_SIZE];
} upd_key_field_t;

/* ------------------------------------------------------------------ */
/*  Parsed update context                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    int               col_count;
    int               row_count;
    int               key_count;
    char              table_name[128];
    char              owner     [128];
    char              col_names [MAX_UPD_COLS][128];
    upd_field_value_t *values;       /* [row * MAX_UPD_COLS + col]    */
    upd_key_field_t   keys[MAX_UPD_KEY_COLS];
} update_ctx_t;

/* ------------------------------------------------------------------ */
/*  Static helpers                                                      */
/* ------------------------------------------------------------------ */
static void trim_upd(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    { s[len - 1] = '\0'; len--; }
}

static int extract_tag_upd(const char *src, const char *tag,
                             char *dest, size_t dest_max)
{
    char open[132], close[132];
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
    trim_upd(dest);
    return 1;
}

/* ================================================================== */
/*  parse_update_xml                                                    */
/*  Two-pass parse: count rows/cols, allocate, then extract values.   */
/*  Also parses <where> key fields.                                    */
/* ================================================================== */
static int parse_update_xml(oci_context_t *ctx,
                              const char    *xml,
                              update_ctx_t  *uc)
{
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Entering parse_update_xml");

    memset(uc, 0, sizeof(*uc));

    if (!extract_tag_upd(xml, "table_name",
                          uc->table_name, sizeof(uc->table_name)))
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Missing <table_name> in update XML");
        return -1;
    }
    extract_tag_upd(xml, "owner", uc->owner, sizeof(uc->owner));

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "table='%s' owner='%s'", uc->table_name, uc->owner);

    /* ---- Parse <where> key fields ---- */
    const char *where_start = strstr(xml, "<where>");
    const char *where_end   = strstr(xml, "</where>");

    if (!where_start || !where_end)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Missing <where> block in update XML");
        return -1;
    }

    size_t where_len = (size_t)(where_end - where_start) + 8;
    char  *where_buf = malloc(where_len + 1);
    if (!where_buf) return -1;
    memcpy(where_buf, where_start, where_len);
    where_buf[where_len] = '\0';

    const char *kp = where_buf;
    while ((kp = strstr(kp, "<key_field>")) != NULL)
    {
        const char *ke = strstr(kp, "</key_field>");
        if (!ke || uc->key_count >= MAX_UPD_KEY_COLS) break;

        size_t klen = (size_t)(ke - kp) + 12;
        char  *kbuf = malloc(klen + 1);
        if (!kbuf) { free(where_buf); return -1; }
        memcpy(kbuf, kp, klen);
        kbuf[klen] = '\0';

        upd_key_field_t *kf = &uc->keys[uc->key_count];
        extract_tag_upd(kbuf, "field_name", kf->field_name,
                         sizeof(kf->field_name));
        extract_tag_upd(kbuf, "field_type", kf->field_type,
                         sizeof(kf->field_type));
        extract_tag_upd(kbuf, "key_value",  kf->key_value,
                         sizeof(kf->key_value));

        logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                     "Key field %d: name='%s' type='%s' value='%s'",
                     uc->key_count + 1,
                     kf->field_name, kf->field_type, kf->key_value);

        free(kbuf);
        uc->key_count++;
        kp = ke + 12;
    }
    free(where_buf);

    if (uc->key_count == 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "No <key_field> entries found in <where> block");
        return -1;
    }

    /* ---- Pass 1: count rows and cols ---- */
    int      row_count = 0;
    int      col_count = 0;
    const char *cursor = xml;

    while ((cursor = strstr(cursor, "<row ")) != NULL)
    {
        if (row_count >= MAX_UPD_ROWS)
        {
            logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                         "Row count exceeds MAX_UPD_ROWS=%d", MAX_UPD_ROWS);
            return -1;
        }
        const char *row_end = strstr(cursor, "</row>");
        if (!row_end) break;

        if (row_count == 0)
        {
            const char *fp = cursor;
            while ((fp = strstr(fp, "<field>")) != NULL && fp < row_end)
            { col_count++; fp += 7; }
        }
        row_count++;
        cursor = row_end + 6;
    }

    if (row_count == 0 || col_count == 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "No rows or columns found in update XML");
        return -1;
    }

    if (col_count > MAX_UPD_COLS)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Column count %d exceeds MAX_UPD_COLS=%d",
                     col_count, MAX_UPD_COLS);
        return -1;
    }

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Pass 1: rows=%d cols=%d keys=%d",
                 row_count, col_count, uc->key_count);

    /* ---- Allocate values array ---- */
    uc->values = calloc((size_t)row_count * col_count,
                        sizeof(upd_field_value_t));
    if (!uc->values)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for values array (%d x %d)",
                     row_count, col_count);
        return -1;
    }

    uc->row_count = row_count;
    uc->col_count = col_count;

    /* ---- Pass 2: extract col names and update_value fields ---- */
    cursor = xml;
    int row_idx = 0;

    while ((cursor = strstr(cursor, "<row ")) != NULL)
    {
        const char *row_end = strstr(cursor, "</row>");
        if (!row_end) break;

        size_t row_len = (size_t)(row_end - cursor) + 6;
        char  *row_buf = malloc(row_len + 1);
        if (!row_buf) { free(uc->values); uc->values = NULL; return -1; }
        memcpy(row_buf, cursor, row_len);
        row_buf[row_len] = '\0';

        const char *fp      = row_buf;
        int         col_idx = 0;

        while ((fp = strstr(fp, "<field>")) != NULL)
        {
            const char *fe = strstr(fp, "</field>");
            if (!fe || col_idx >= col_count) break;

            size_t flen = (size_t)(fe - fp) + 8;
            char  *fbuf = malloc(flen + 1);
            if (!fbuf)
            {
                free(row_buf);
                free(uc->values); uc->values = NULL;
                return -1;
            }
            memcpy(fbuf, fp, flen);
            fbuf[flen] = '\0';

            if (row_idx == 0)
                extract_tag_upd(fbuf, "field_name",
                                uc->col_names[col_idx],
                                sizeof(uc->col_names[col_idx]));

            upd_field_value_t *fv =
                &uc->values[row_idx * col_count + col_idx];
            memset(fv, 0, sizeof(*fv));

            /* Try <update_value> first, fall back to <insert_value> */
            if (!extract_tag_upd(fbuf, "update_value",
                                  fv->value, sizeof(fv->value)))
                extract_tag_upd(fbuf, "insert_value",
                                fv->value, sizeof(fv->value));

            fv->is_empty = (strlen(fv->value) == 0);

            free(fbuf);
            col_idx++;
            fp = fe + 8;
        }

        if (col_idx != col_count)
        {
            logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                         "Row %d has %d fields, expected %d",
                         row_idx + 1, col_idx, col_count);
            free(row_buf);
            free(uc->values); uc->values = NULL;
            return -1;
        }

        free(row_buf);
        row_idx++;
        cursor = row_end + 6;
    }

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "parse_update_xml OK: rows=%d cols=%d keys=%d "
                 "allocated=%zu bytes",
                 uc->row_count, uc->col_count, uc->key_count,
                 (size_t)uc->row_count * uc->col_count *
                 sizeof(upd_field_value_t));
    return 0;
}

/* ================================================================== */
/*  get_upd_bind_wrapper                                                */
/*  Returns SQL expression wrapper for date/time/LOB types.            */
/*  BLOB/CLOB use EMPTY_BLOB()/EMPTY_CLOB() in SET clause;            */
/*  WHERE key columns always bind as plain SQLT_STR.                   */
/* ================================================================== */
static const char *get_upd_bind_wrapper(const char *dtype)
{
    if (strcmp(dtype, "DATE") == 0)
        return "TO_DATE(%s,'YYYY-MM-DD HH24:MI:SS')";
    if (strncmp(dtype, "TIMESTAMP", 9) == 0)
        return "TO_TIMESTAMP(%s,'YYYY-MM-DD HH24:MI:SS.FF6')";
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "MONTH"))
        return "TO_YMINTERVAL(%s)";
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "SECOND"))
        return "TO_DSINTERVAL(%s)";
    if (strcmp(dtype, "BLOB") == 0)
        return "EMPTY_BLOB()";
    if (strcmp(dtype, "CLOB")  == 0 ||
        strcmp(dtype, "NCLOB") == 0)
        return "EMPTY_CLOB()";
    return NULL;
}

/* ================================================================== */
/*  build_update_sql                                                    */
/*  UPDATE owner.table                                                  */
/*  SET col1=:1, col2=EMPTY_BLOB(), ...                                */
/*  WHERE key1=:N, key2=:N+1, ...                                      */
/* ================================================================== */
static int build_update_sql(oci_context_t        *ctx,
                              const update_ctx_t   *uc,
                              const col_metadata_t *cols,
                              int                   col_meta_count,
                              char                 *sql_buf,
                              size_t                sql_max)
{
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Building UPDATE SQL table='%s'", uc->table_name);

    char set_list [MAX_UPD_COLS * 256] = {0};
    int  bind_pos = 1;

    /* ---- SET clause ---- */
    for (int i = 0; i < uc->col_count; i++)
    {
        if (i > 0)
            strncat(set_list, ", ",
                    sizeof(set_list) - strlen(set_list) - 1);

        /* Find type */
        const char *dtype = "VARCHAR2";
        for (int m = 0; m < col_meta_count; m++)
            if (strcasecmp(cols[m].col_name, uc->col_names[i]) == 0)
            { dtype = cols[m].data_type; break; }

        const char *wrapper = get_upd_bind_wrapper(dtype);
        char assignment[256] = {0};

        if (wrapper &&
            (strcmp(wrapper, "EMPTY_BLOB()") == 0 ||
             strcmp(wrapper, "EMPTY_CLOB()") == 0))
        {
            /* LOB: no bind placeholder - use literal directly */
            snprintf(assignment, sizeof(assignment),
                     "%s=%s", uc->col_names[i], wrapper);
        }
        else
        {
            char bind_ph[16];
            snprintf(bind_ph, sizeof(bind_ph), ":%d", bind_pos++);

            if (wrapper)
            {
                char expr[128] = {0};
                snprintf(expr, sizeof(expr), wrapper, bind_ph);
                snprintf(assignment, sizeof(assignment),
                         "%s=%s", uc->col_names[i], expr);
            }
            else
            {
                snprintf(assignment, sizeof(assignment),
                         "%s=%s", uc->col_names[i], bind_ph);
            }
        }

        strncat(set_list, assignment,
                sizeof(set_list) - strlen(set_list) - 1);
    }

    /* ---- WHERE clause ---- */
    char where_list[MAX_UPD_KEY_COLS * 256] = {0};

    for (int k = 0; k < uc->key_count; k++)
    {
        if (k > 0)
            strncat(where_list, " AND ",
                    sizeof(where_list) - strlen(where_list) - 1);

        char bind_ph[16];
        snprintf(bind_ph, sizeof(bind_ph), ":%d", bind_pos++);

        /* Apply date wrapper to WHERE keys too if needed */
        const char *ktype   = uc->keys[k].field_type;
        const char *wrapper = NULL;
        if (strcmp(ktype, "DATE") == 0)
            wrapper = "TO_DATE(%s,'YYYY-MM-DD HH24:MI:SS')";
        else if (strncmp(ktype, "TIMESTAMP", 9) == 0)
            wrapper = "TO_TIMESTAMP(%s,'YYYY-MM-DD HH24:MI:SS.FF6')";

        char cond[256] = {0};
        if (wrapper)
        {
            char expr[128] = {0};
            snprintf(expr, sizeof(expr), wrapper, bind_ph);
            snprintf(cond, sizeof(cond),
                     "%s=%s", uc->keys[k].field_name, expr);
        }
        else
        {
            snprintf(cond, sizeof(cond),
                     "%s=%s", uc->keys[k].field_name, bind_ph);
        }

        strncat(where_list, cond,
                sizeof(where_list) - strlen(where_list) - 1);
    }

    /* ---- Assemble ---- */
    int n;
    if (strlen(uc->owner) > 0)
        n = snprintf(sql_buf, sql_max,
                     "UPDATE %s.%s SET %s WHERE %s",
                     uc->owner, uc->table_name, set_list, where_list);
    else
        n = snprintf(sql_buf, sql_max,
                     "UPDATE %s SET %s WHERE %s",
                     uc->table_name, set_list, where_list);

    if (n < 0 || (size_t)n >= sql_max)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "UPDATE SQL truncated - increase sql_buf size");
        return -1;
    }

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "UPDATE SQL: %s", sql_buf);
    return 0;
}

/* ================================================================== */
/*  handle_blob_update                                                  */
/*  Identical to handle_blob_insert - persistent locator via           */
/*  SELECT FOR UPDATE on the just-updated row ROWID.                   */
/* ================================================================== */
static int handle_blob_update(oci_context_t *ctx,
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

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Entering col='%s' rowid='%s' is_empty=%d",
                 col_name, rowid_str, is_empty);

    if (is_empty) return 0;

    FILE *fp = fopen(file_path, "rb");
    if (!fp)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Failed to open BLOB file: %s", file_path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0)
    {
        fclose(fp);
        return 0;
    }

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "BLOB file='%s' size=%ld", file_path, file_size);

    char sql_sel[512];
    snprintf(sql_sel, sizeof(sql_sel),
             "SELECT %s FROM %s WHERE ROWID = :rid FOR UPDATE",
             col_name, table_name);

    CHECK_OCI_UPD(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt_sel, ctx->errhp,
                        (text *)sql_sel, (ub4)strlen(sql_sel),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_rid = NULL;
    CHECK_OCI_UPD(ctx->errhp,
        OCIBindByName(stmt_sel, &bind_rid, ctx->errhp,
                      (text *)":rid", -1,
                      (dvoid *)rowid_str,
                      (sb4)(strlen(rowid_str) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_UPD(ctx->errhp,
        OCIDescriptorAlloc(ctx->envhp, (void **)&lob_loc,
                           OCI_DTYPE_LOB, 0, NULL),
        ctx, Cleanup);

    OCIDefine *def_lob = NULL;
    CHECK_OCI_UPD(ctx->errhp,
        OCIDefineByPos(stmt_sel, &def_lob, ctx->errhp, 1,
                       &lob_loc,
                       (sb4)sizeof(OCILobLocator *),
                       SQLT_BLOB, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_UPD(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt_sel, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_UPD(ctx->errhp,
        OCIStmtFetch2(stmt_sel, ctx->errhp,
                      1, OCI_FETCH_NEXT, 0, OCI_DEFAULT),
        ctx, Cleanup);

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Persistent BLOB locator obtained - writing chunks");

    ub1   *chunk_buf = malloc(ctx->ini->chunk_read_size);
    if (!chunk_buf)
    {
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
            free(chunk_buf);
            fclose(fp);
            rc = -1;
            goto Cleanup;
        }

        ub4 amount = (ub4)nread;

        logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                     "OCILobWrite offset=%u chunk=%zu remaining=%zu",
                     offset, nread, bytes_remaining - nread);

        CHECK_OCI_UPD(ctx->errhp,
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

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
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
/*  handle_clob_update                                                  */
/*  Identical to handle_clob_insert.                                   */
/* ================================================================== */
static int handle_clob_update(oci_context_t *ctx,
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

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Entering col='%s' type='%s' rowid='%s' is_empty=%d",
                 col_name, col_type, rowid_str, is_empty);

    if (is_empty) return 0;

    /* ---- Determine text source ---- */
    const char *text_data = NULL;
    size_t      text_len  = 0;

    if (strncmp(insert_value, CLOB_FILE_PREFIX, CLOB_FILE_PREFIX_LEN) == 0)
    {
        const char *path = insert_value + CLOB_FILE_PREFIX_LEN;
        FILE *fp = fopen(path, "r");
        if (!fp)
        {
            logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                         "Failed to open CLOB file: %s", path);
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
        text_data = insert_value;
        text_len  = strlen(insert_value);
    }

    if (!text_data || text_len == 0)
    {
        if (file_buf) free(file_buf);
        return 0;
    }

    char sql_sel[512];
    snprintf(sql_sel, sizeof(sql_sel),
             "SELECT %s FROM %s WHERE ROWID = :rid FOR UPDATE",
             col_name, table_name);

    CHECK_OCI_UPD(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt_sel, ctx->errhp,
                        (text *)sql_sel, (ub4)strlen(sql_sel),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    OCIBind *bind_rid = NULL;
    CHECK_OCI_UPD(ctx->errhp,
        OCIBindByName(stmt_sel, &bind_rid, ctx->errhp,
                      (text *)":rid", -1,
                      (dvoid *)rowid_str,
                      (sb4)(strlen(rowid_str) + 1),
                      SQLT_STR, NULL, NULL, NULL, 0, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_UPD(ctx->errhp,
        OCIDescriptorAlloc(ctx->envhp, (void **)&lob_loc,
                           OCI_DTYPE_LOB, 0, NULL),
        ctx, Cleanup);

    OCIDefine *def_lob = NULL;
    CHECK_OCI_UPD(ctx->errhp,
        OCIDefineByPos(stmt_sel, &def_lob, ctx->errhp, 1,
                       &lob_loc,
                       (sb4)sizeof(OCILobLocator *),
                       SQLT_CLOB, NULL, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_UPD(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt_sel, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);

    CHECK_OCI_UPD(ctx->errhp,
        OCIStmtFetch2(stmt_sel, ctx->errhp,
                      1, OCI_FETCH_NEXT, 0, OCI_DEFAULT),
        ctx, Cleanup);

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Persistent CLOB locator obtained - writing text");

    ub4    offset          = 1;
    size_t bytes_remaining = text_len;

    while (bytes_remaining > 0)
    {
        size_t chunk = ctx->ini->chunk_read_size;
        if (chunk > bytes_remaining) chunk = bytes_remaining;

        ub4 amount = (ub4)chunk;

        logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                     "OCILobWrite offset=%u chunk=%zu remaining=%zu",
                     offset, chunk, bytes_remaining - chunk);

        CHECK_OCI_UPD(ctx->errhp,
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

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
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
/*  execute_update_batch - main entry point                            */
/* ================================================================== */
int execute_update_batch(oci_context_t    *ctx,
                          const char       *template_xml,
                          execute_config_t *cfg)
{
    int            rc           = 0;
    OCIStmt       *stmt         = NULL;
    xml_builder_t *xml          = NULL;
    update_ctx_t  *uc           = NULL;
    OCIBind      **bind_hdls    = NULL;
    char         **scalar_bufs  = NULL;
    sb2           *indicators   = NULL;
    int            execute_count= 0;
    int            rows_updated = 0;
    int            lob_count    = 0;
    uint64_t       lob_bytes    = 0;   /* total BLOB bytes written    */
    uint64_t       clob_bytes   = 0;   /* total CLOB bytes written    */
    struct timespec ts_start, ts_end;
    audit_old_value_t *old_values    = NULL;  /* before-image for audit */
    int                old_row_count = 0;

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Entering execute_update_batch");

    if (!ctx || !template_xml || !cfg)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments");
        return -1;
    }


    metrics_record_t metrics;
     metrics_init(&metrics);
     metrics_set_context(&metrics, ctx);
     metrics.start_time_us = metrics_now_us();
     strncpy(metrics.operation, "UPDATE", sizeof(metrics.operation) - 1);

     /* Set transaction_id immediately so every write path carries it  */
            if (ctx->active_tx)
                strncpy(metrics.transaction_id,
                        tx_get_id(ctx->active_tx),
                        sizeof(metrics.transaction_id) - 1);
            else
                strncpy(metrics.transaction_id, "-",
                        sizeof(metrics.transaction_id) - 1);

    /* ================================================================
     *  Stage 1 - Validate
     *  Reuses validate_insert_template - it validates field types and
     *  values regardless of operation.
     * ================================================================ */
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Validating all rows");

    char val_error[512] = {0};
    if (validate_insert_template(ctx, template_xml,
                                  val_error, sizeof(val_error)) != 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Stage 1 validation failed: %s", val_error);
        return -1;
    }
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 1 validation passed");

    /* ================================================================
     *  Stage 2 - Parse XML and prepare statement
     * ================================================================ */
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 2: Parsing XML and preparing statement");

    uc = calloc(1, sizeof(update_ctx_t));
    if (!uc)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for update_ctx_t");
        rc = -1;
        goto Cleanup;
    }

    if (parse_update_xml(ctx, template_xml, uc) != 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "parse_update_xml failed");
        rc = -1;
        goto Cleanup;
    }
    strncpy(metrics.object_name, uc->table_name,
             sizeof(metrics.object_name) - 1);


    /* Cap at max_bulk_inserts (reuse same ini setting) */
    int max_batch = ctx->ini->max_bulk_inserts;
    if (max_batch < 1) max_batch = 1;
    if (uc->row_count > max_batch)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "row_count=%d exceeds max_bulk_inserts=%d",
                     uc->row_count, max_batch);
        rc = -1;
        goto Cleanup;
    }

    /* Load column metadata for type mapping */
    col_metadata_t     cols[MAX_UPD_COLS];
    int                col_meta_count = 0;
    metadata_request_t meta_req;

    memset(&meta_req, 0, sizeof(meta_req));
    strncpy(meta_req.table_name, uc->table_name,
            sizeof(meta_req.table_name) - 1);
    strncpy(meta_req.owner, uc->owner,
            sizeof(meta_req.owner) - 1);

    metadata_cache_result_t meta_result;
    memset(&meta_result, 0, sizeof(meta_result));

    if (metadata_cache_get_or_fetch(ctx->metadata_cache,
                                     ctx,
                                     &meta_req,
                                     cols,
                                     &col_meta_count,
                                     MAX_UPD_COLS,
                                     &meta_result) != 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "metadata_cache_get_or_fetch failed");
        rc = -1;
        goto Cleanup;
    }

    /* Wire metadata cache stats into metrics                          */
    metrics.cache_hit       = meta_result.was_cache_hit;
    metrics.cache_lookup_us = meta_result.cache_lookup_us;
    metrics.cache_key_hash  = meta_result.cache_key_hash;

    /* Build UPDATE SQL */
    char sql_buf[65536] = {0};
    if (build_update_sql(ctx, uc, cols, col_meta_count,
                          sql_buf, sizeof(sql_buf)) != 0)
    {
        rc = -1;
        goto Cleanup;
    }

    /* sql_hash: hash the built SQL for traceability in metrics        */
    if (ctx->metadata_cache)
        metrics.sql_hash = cache_hash_string(ctx->metadata_cache, sql_buf);

    CHECK_OCI_UPD(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)sql_buf, (ub4)strlen(sql_buf),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    /* No CLOB/BLOB binds so full batch is fine */
    execute_count = uc->row_count;

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "execute_count=%d rows=%d cols=%d keys=%d",
                 execute_count, uc->row_count,
                 uc->col_count, uc->key_count);

    /* ================================================================
     *  Stage 3 - Allocate bind structures
     *  Total binds = SET cols + WHERE key cols per row
     * ================================================================ */
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 3: Allocating bind structures");

    int total_bind_cols = uc->col_count + uc->key_count;

    bind_hdls   = calloc(total_bind_cols, sizeof(OCIBind *));
    scalar_bufs = calloc(total_bind_cols, sizeof(char *));
    indicators  = calloc(total_bind_cols * execute_count, sizeof(sb2));

    if (!bind_hdls || !scalar_bufs || !indicators)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for bind structures");
        rc = -1;
        goto Cleanup;
    }

    /* Allocate scalar buffers for SET columns */
    for (int c = 0; c < uc->col_count; c++)
    {
        const char *dtype   = "VARCHAR2";
        int         buf_size = MAX_COL_VALUE_SIZE;

        for (int m = 0; m < col_meta_count; m++)
            if (strcasecmp(cols[m].col_name, uc->col_names[c]) == 0)
            {
                dtype = cols[m].data_type;
                if (cols[m].data_length > 0 &&
                    cols[m].data_length + 64 < MAX_COL_VALUE_SIZE)
                    buf_size = cols[m].data_length + 64;
                break;
            }

        /* LOB columns use EMPTY_BLOB/CLOB in SQL - no scalar buffer */
        if (strcmp(dtype, "BLOB")  == 0 ||
            strcmp(dtype, "CLOB")  == 0 ||
            strcmp(dtype, "NCLOB") == 0)
        {
            scalar_bufs[c] = NULL;
            continue;
        }

        scalar_bufs[c] = calloc((size_t)execute_count, (size_t)buf_size);
        if (!scalar_bufs[c]) { rc = -1; goto Cleanup; }
    }

    /* Allocate scalar buffers for WHERE key columns */
    for (int k = 0; k < uc->key_count; k++)
    {
        int idx = uc->col_count + k;
        scalar_bufs[idx] = calloc((size_t)execute_count,
                                   MAX_COL_VALUE_SIZE);
        if (!scalar_bufs[idx]) { rc = -1; goto Cleanup; }
    }

    /* ================================================================
     *  Stage 2 Audit - Fetch before-image BEFORE the UPDATE executes
     *
     *  We capture the current column values now, while they still hold
     *  the pre-update state.  The before-image is a SELECT on the same
     *  session and same transaction so it sees the consistent snapshot
     *  of the data as it exists before our UPDATE statement runs.
     *
     *  audit_trail_fetch_before_image() builds:
     *    SELECT col1, col2, ... FROM owner.table
     *    WHERE  key1 = 'val1' AND key2 = 'val2' ...
     *  and parses the result into old_values[row * col_count + col].
     *
     *  Key names/values come from uc->keys[] which was parsed from the
     *  <where> block of the Update_Template XML.
     * ================================================================ */
    if (!audit_trail_in_progress)
    {
        /* Build key name/value arrays from uc->keys[] */
        char (*key_names) [128]   = NULL;
        char (*key_vals)  [32768] = NULL;

        key_names = calloc((size_t)uc->key_count, sizeof(*key_names));
        key_vals  = calloc((size_t)uc->key_count, sizeof(*key_vals));

        if (key_names && key_vals)
        {
            for (int k = 0; k < uc->key_count; k++)
            {
                strncpy(key_names[k], uc->keys[k].field_name,
                        sizeof(key_names[k]) - 1);
                strncpy(key_vals[k],  uc->keys[k].key_value,
                        sizeof(key_vals[k])  - 1);
            }

            int fetch_rc =
                audit_trail_fetch_before_image(ctx,
                                               uc->table_name,
                                               uc->owner,
                                               uc->col_names,
                                               uc->col_count,
                                               key_names,
                                               key_vals,
                                               uc->key_count,
                                               &old_values,
                                               &old_row_count);
            if (fetch_rc != 0)
            {
                logger_write(ctx->update_logger, LOG_WARN, __func__, 0,
                             "Before-image fetch failed (rc=%d) for "
                             "table='%s' - UPDATE will proceed but "
                             "audit trail OLD_VALUE will be unavailable",
                             fetch_rc, uc->table_name);
                /* old_values remains NULL - audit will be skipped below */
            }
            else
            {
                logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                             "Before-image captured: %d row(s) "
                             "for table='%s'",
                             old_row_count, uc->table_name);
            }
        }

        free(key_names);
        free(key_vals);
    }

    /* ================================================================
     *  Stage 4 - Execute loop
     * ================================================================ */
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 4: Execute loop");

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    int row_base = 0;

    while (row_base < uc->row_count)
    {
        int batch_rows = execute_count;
        if (row_base + batch_rows > uc->row_count)
            batch_rows = uc->row_count - row_base;

        logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                     "Batch: row_base=%d batch_rows=%d",
                     row_base, batch_rows);

        /* ---- Bind SET columns ---- */
        /*
         * scalar_bind_pos mirrors the bind_pos counter in build_update_sql:
         * it starts at 1 and increments only for non-LOB SET columns.
         * LOB columns use EMPTY_BLOB()/EMPTY_CLOB() literals in the SQL
         * and have no bind placeholder, so they must be skipped here too.
         * Using (c + 1) instead would produce wrong positions whenever any
         * LOB column appears before a scalar column in the SET list.
         */
        int scalar_bind_pos = 1;

        for (int c = 0; c < uc->col_count; c++)
        {
            const char *dtype   = "VARCHAR2";
            int         buf_size = MAX_COL_VALUE_SIZE;

            for (int m = 0; m < col_meta_count; m++)
                if (strcasecmp(cols[m].col_name, uc->col_names[c]) == 0)
                {
                    dtype = cols[m].data_type;
                    if (cols[m].data_length > 0 &&
                        cols[m].data_length + 64 < MAX_COL_VALUE_SIZE)
                        buf_size = cols[m].data_length + 64;
                    break;
                }

            logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                         "Binding SET col=%d name='%s' type='%s'",
                         c, uc->col_names[c], dtype);

            /* LOB: EMPTY_BLOB/CLOB in SQL - no placeholder, skip bind */
            if (strcmp(dtype, "BLOB")  == 0 ||
                strcmp(dtype, "CLOB")  == 0 ||
                strcmp(dtype, "NCLOB") == 0)
                continue;

            /* Scalar: fill array buffer */
            for (int r = 0; r < batch_rows; r++)
            {
                int row_idx = row_base + r;
                const upd_field_value_t *fv =
                    &uc->values[row_idx * uc->col_count + c];
                char *slot = scalar_bufs[c] + ((size_t)r * buf_size);

                if (fv->is_empty)
                {
                    slot[0] = '\0';
                    indicators[c * execute_count + r] = -1;
                }
                else
                {
                    strncpy(slot, fv->value, buf_size - 1);
                    slot[buf_size - 1] = '\0';
                    indicators[c * execute_count + r] = 0;
                }
            }

            logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                         "OCIBindByPos SET col=%d name='%s' "
                         "bind_pos=%d",
                         c, uc->col_names[c], scalar_bind_pos);

            CHECK_OCI_UPD(ctx->errhp,
                OCIBindByPos(stmt, &bind_hdls[c], ctx->errhp,
                             (ub4)scalar_bind_pos,
                             scalar_bufs[c],
                             (sb4)buf_size,
                             SQLT_STR,
                             &indicators[c * execute_count],
                             NULL, NULL, 0, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            scalar_bind_pos++;   /* advance only for real placeholders */

            if (batch_rows > 1)
            {
                logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                             "OCIBindArrayOfStruct SET col=%d "
                             "buf_size=%d batch_rows=%d",
                             c, buf_size, batch_rows);

                CHECK_OCI_UPD(ctx->errhp,
                    OCIBindArrayOfStruct(bind_hdls[c], ctx->errhp,
                                         (ub4)buf_size,
                                         (ub4)sizeof(sb2),
                                         0, 0),
                    ctx, Cleanup);
            }
        }

        /* ---- Bind WHERE key columns ---- */
        /*
         * scalar_bind_pos now holds (number of scalar SET binds + 1),
         * which is exactly the first WHERE placeholder number - matching
         * the sequence that build_update_sql wrote into the SQL string.
         */
        for (int k = 0; k < uc->key_count; k++)
        {
            int bind_pos = scalar_bind_pos + k;   /* 1-based, LOB-aware */
            int idx      = uc->col_count + k;

            /* Same key value for every row in the batch */
            for (int r = 0; r < batch_rows; r++)
            {
                char *slot = scalar_bufs[idx] +
                             ((size_t)r * MAX_COL_VALUE_SIZE);
                strncpy(slot, uc->keys[k].key_value,
                        MAX_COL_VALUE_SIZE - 1);
                slot[MAX_COL_VALUE_SIZE - 1] = '\0';
                indicators[idx * execute_count + r] = 0;
            }

            logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                         "Binding WHERE key=%d name='%s' value='%s'",
                         k, uc->keys[k].field_name,
                         uc->keys[k].key_value);

            CHECK_OCI_UPD(ctx->errhp,
                OCIBindByPos(stmt, &bind_hdls[idx], ctx->errhp,
                             (ub4)bind_pos,
                             scalar_bufs[idx],
                             (sb4)MAX_COL_VALUE_SIZE,
                             SQLT_STR,
                             &indicators[idx * execute_count],
                             NULL, NULL, 0, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            if (batch_rows > 1)
            {
                CHECK_OCI_UPD(ctx->errhp,
                    OCIBindArrayOfStruct(bind_hdls[idx], ctx->errhp,
                                         (ub4)MAX_COL_VALUE_SIZE,
                                         (ub4)sizeof(sb2),
                                         0, 0),
                    ctx, Cleanup);
            }
        }

        /* ---- Execute ---- */
        logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                     "Calling OCIStmtExecute iters=%d", batch_rows);

        CHECK_OCI_UPD(ctx->errhp,
            OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                           (ub4)batch_rows,
                           0, NULL, NULL, OCI_DEFAULT),
            ctx, Cleanup);
        metrics.execution_us += metrics_now_us() - metrics.start_time_us;
         metrics.rows_affected = (uint64_t)rows_updated;

        rows_updated += batch_rows;

        /* ---- Post-execute: write LOB data ---- */
        {
            int has_lob = 0;
            for (int bc = 0; bc < uc->col_count && !has_lob; bc++)
                for (int m = 0; m < col_meta_count; m++)
                    if (strcasecmp(cols[m].col_name,
                                   uc->col_names[bc]) == 0 &&
                        (strcmp(cols[m].data_type, "BLOB")  == 0 ||
                         strcmp(cols[m].data_type, "CLOB")  == 0 ||
                         strcmp(cols[m].data_type, "NCLOB") == 0) &&
                        !uc->values[row_base * uc->col_count + bc].is_empty)
                    { has_lob = 1; break; }

            if (has_lob)
            {
                OCIRowid *rid_desc = NULL;
                char      rid_str[100];
                ub2       rid_len = sizeof(rid_str) - 1;

                CHECK_OCI_UPD(ctx->errhp,
                    OCIDescriptorAlloc(ctx->envhp,
                                       (void **)&rid_desc,
                                       OCI_DTYPE_ROWID, 0, NULL),
                    ctx, Cleanup);

                CHECK_OCI_UPD(ctx->errhp,
                    OCIAttrGet(stmt, OCI_HTYPE_STMT,
                               rid_desc, NULL,
                               OCI_ATTR_ROWID, ctx->errhp),
                    ctx, Cleanup);

                CHECK_OCI_UPD(ctx->errhp,
                    OCIRowidToChar(rid_desc,
                                   (OraText *)rid_str,
                                   &rid_len, ctx->errhp),
                    ctx, Cleanup);

                rid_str[rid_len] = '\0';
                OCIDescriptorFree(rid_desc, OCI_DTYPE_ROWID);

                logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                             "Updated row ROWID='%s'", rid_str);

                char tbl_fq[256] = {0};
                if (strlen(uc->owner) > 0)
                    snprintf(tbl_fq, sizeof(tbl_fq), "%s.%s",
                             uc->owner, uc->table_name);
                else
                    snprintf(tbl_fq, sizeof(tbl_fq), "%s",
                             uc->table_name);

                for (int bc = 0; bc < uc->col_count; bc++)
                {
                    const char *btype = "VARCHAR2";
                    for (int m = 0; m < col_meta_count; m++)
                        if (strcasecmp(cols[m].col_name,
                                       uc->col_names[bc]) == 0)
                        { btype = cols[m].data_type; break; }

                    const upd_field_value_t *fv =
                        &uc->values[row_base * uc->col_count + bc];
                    if (fv->is_empty) continue;

                    if (strcmp(btype, "BLOB") == 0)
                    {
                        if (handle_blob_update(ctx,
                                               uc->col_names[bc],
                                               tbl_fq, rid_str,
                                               fv->value, 0,
                                               &lob_bytes) != 0)
                        { rc = -1; goto Cleanup; }
                        lob_count++;
                    }
                    else if (strcmp(btype, "CLOB")  == 0 ||
                             strcmp(btype, "NCLOB") == 0)
                    {
                        if (handle_clob_update(ctx,
                                               uc->col_names[bc],
                                               btype, tbl_fq, rid_str,
                                               fv->value, 0,
                                               &clob_bytes) != 0)
                        { rc = -1; goto Cleanup; }
                        lob_count++;
                    }
                }
            }
        }

        row_base += batch_rows;

        logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                     "Batch updated: rows_updated=%d", rows_updated);
    }

    /* ================================================================
     *  Stage 2 Audit - Write field-level audit rows for UPDATE
     *
     *  Now that the UPDATE has executed successfully, write one
     *  AUDIT_TRAIL row per (row × changed column).
     *  Columns where OLD_VALUE == NEW_VALUE produce no audit row.
     *
     *  old_values was captured before the UPDATE above.
     *  atr.new_values = uc->values (the UPDATE SET values).
     *
     *  The cycle-guard (audit_trail_in_progress) is checked inside
     *  audit_trail_insert_update() so no guard is needed here.
     * ================================================================ */
    if (!audit_trail_in_progress && rc == 0 && old_values)
    {
        logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                     "Calling audit_trail_insert_update for table='%s' "
                     "rows=%d cols=%d",
                     uc->table_name, uc->row_count, uc->col_count);

        audit_trail_request_t atr;
        memset(&atr, 0, sizeof(atr));

        strncpy(atr.table_name,  uc->table_name,
                sizeof(atr.table_name)  - 1);
        strncpy(atr.action_type, "UPDATE",
                sizeof(atr.action_type) - 1);
        strncpy(atr.changed_by,  ctx->ini->username,
                sizeof(atr.changed_by)  - 1);
        strncpy(atr.module_name, "OCI_Update_Execute",
                sizeof(atr.module_name) - 1);

        /* change_reason: use transaction name if available            */
        if (ctx->active_tx && ctx->active_tx->tx_name[0] &&
            strcmp(ctx->active_tx->tx_name, "-") != 0)
            strncpy(atr.change_reason, ctx->active_tx->tx_name,
                    sizeof(atr.change_reason) - 1);
        else
            strncpy(atr.change_reason, "Business UPDATE via Data_Manager",
                    sizeof(atr.change_reason) - 1);

        /* record_id: use first key value as the record identifier     */
        if (uc->key_count > 0)
            strncpy(atr.record_id, uc->keys[0].key_value,
                    sizeof(atr.record_id) - 1);
        else
            strncpy(atr.record_id, "-", sizeof(atr.record_id) - 1);

        atr.col_names  = uc->col_names;
        atr.col_types  = cols;
        atr.new_values = uc->values;    /* UPDATE SET values           */
        atr.old_values = NULL;          /* supplied via old_values arg */
        atr.col_count  = uc->col_count;
        atr.row_count  = uc->row_count;
        atr.audit_mode = AUDIT_MODE_FIELD;

        int audit_rc = audit_trail_insert_update(ctx, &atr, old_values);
        if (audit_rc != 0)
            logger_write(ctx->update_logger, LOG_WARN, __func__, 0,
                         "Audit trail UPDATE failed (rc=%d) for "
                         "table='%s' - business UPDATE is NOT rolled back.",
                         audit_rc, uc->table_name);
    }
    else if (!old_values && !audit_trail_in_progress && rc == 0)
    {
        logger_write(ctx->update_logger, LOG_WARN, __func__, 0,
                     "Skipping UPDATE audit - before-image unavailable "
                     "for table='%s'", uc->table_name);
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
        logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                     "Commit successful rows_updated=%d lobs=%d",
                     rows_updated, lob_count);
    }
    else
    {
        /* ---- Commit ---- */
        logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                     "Calling OCITransCommit");

        CHECK_OCI_UPD(ctx->errhp,
            OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT),
            ctx, Cleanup);

        logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                     "Commit successful rows_updated=%d lobs=%d",
                     rows_updated, lob_count);
    }



    /* ================================================================
     *  Stage 5 - Build result XML
     * ================================================================ */
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    xml = xml_create(4096);
    if (!xml) { rc = -1; goto Cleanup; }

    xml_start_document(xml);
    xml_start_execution(xml);
    xml_append(xml, "<operation>UPDATE</operation>\n");
    xml_append(xml, "<table_name>%s</table_name>\n", uc->table_name);
    xml_append(xml, "<owner>%s</owner>\n",            uc->owner);
    xml_append(xml, "<rows_updated>%d</rows_updated>\n",   rows_updated);
    xml_append(xml, "<lobs_written>%d</lobs_written>\n",   lob_count);
    xml_append(xml, "<execution_time>%.6f</execution_time>\n", elapsed);
    xml_append(xml, "<execute_batch_size>%d</execute_batch_size>\n",
               execute_count);
    xml_end_execution(xml);
    xml_finalize(xml);

    metrics.end_time_us      = metrics_now_us();
     metrics.status_code     = 0;
     strncpy(metrics.error_code, "-", sizeof(metrics.error_code) - 1);
     strncpy(metrics.error_text, "-", sizeof(metrics.error_text) - 1);
     metrics.rows_affected    = rows_updated;
     metrics.output_xml_bytes = xml ? (uint64_t)strlen(xml->buffer) : 0;
     metrics.lob_bytes        = lob_bytes;
     metrics.clob_bytes       = clob_bytes;
     /* transaction_id already set at init time */

    if (!cfg->xml) cfg->xml = calloc(1, sizeof(*cfg->xml));
    cfg->xml->OUTPUT_XML = strdup(xml->buffer);

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "execute_update_batch complete: table='%s' "
                 "rows=%d elapsed=%.6f",
                 uc->table_name, rows_updated, elapsed);

Cleanup:
    /* Stage 6 - Cleanup */
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0, "Stage 6: Cleanup");
    metrics.end_time_us = metrics_now_us();
    metrics.status_code = rc;

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
	if (ctx->ini && ctx->ini->metrics_display_input_file_name && cfg->input_file_name)
	    metrics.input_file_name = flatten_for_csv(cfg->input_file_name);

	if (ctx->ini && ctx->ini->metrics_display_input_request && ctx->INPUT_XML)
	    metrics.input_request = flatten_for_csv3(ctx->INPUT_XML);


	if (ctx->ini && ctx->ini->metrics_display_output_response)
	{
	    /* UPDATE doesn't render a JSON response yet (only the SELECT
	     * batch path does) - this is a no-op fallback to XML until it
	     * does, kept consistent with the other execute modules.       */
	    int is_json = (cfg->ReturnFormat &&
	                   strcasecmp(cfg->ReturnFormat, "JSON") == 0);

	    if (is_json && cfg->OUTPUT_JSON)
	        metrics.output_response = flatten_for_csv3(cfg->OUTPUT_JSON);
	    else if (cfg->xml && cfg->xml->OUTPUT_XML)
	        metrics.output_response = flatten_for_csv3(cfg->xml->OUTPUT_XML);
	}





	metrics_finalise(&metrics);
    metrics_write(ctx->metrics_logger, &metrics);
    logger_clear_last_error();   // reset for next operation



    if (rc != 0 && rows_updated > 0)
    {
        logger_write(ctx->update_logger, LOG_WARN, __func__, 0,
                     "Rolling back due to error");
        OCITransRollback(ctx->svchp, ctx->errhp, OCI_DEFAULT);
    }

    if (scalar_bufs)
    {
        for (int c = 0; c < uc->col_count + uc->key_count; c++)
            if (scalar_bufs[c])
            {
                logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                             "free(scalar_bufs[%d])", c);
                free(scalar_bufs[c]);
            }
        free(scalar_bufs);
    }

    if (old_values) { free(old_values); old_values = NULL; }
    if (indicators) { free(indicators);  }
    if (bind_hdls)  { free(bind_hdls);   }

    if (uc)
    {
        if (uc->values) free(uc->values);
        free(uc);
    }

    if (xml)  { xml_free(xml); }

    if (stmt)
    {
        logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                     "OCIStmtRelease stmt");
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    }

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Cleanup complete rc=%d", rc);
    return rc;
}
