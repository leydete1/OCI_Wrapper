
/*
 * OCI_Update_Execute_Module.c
 *
 * Stage 3 - Update Execute Module
 * --------------------------------
 * Executes a bulk UPDATE from an already-validated update_request_t
 * (built by Level 1). Mirrors OCI_Insert_Execute_Module in structure
 * and conventions.
 *
 * level2_validate_update() is called internally as this function's own
 * first step, not just trusted to have already run in the caller -
 * same reasoning as execute_insert_batch(), see its own doc comment in
 * OCI_Insert_Execute_Module.h.
 *
 * Key differences from insert:
 *   - update_request_t.keys[] (where_key_t) identifies rows to update;
 *     update_request_t.fields[] (field_value_t) is the SET clause - no
 *     per-row concept, unlike INSERT's rows[].
 *   - SQL: UPDATE owner.table SET col=:1,... WHERE key=:N,...
 *   - BLOB/CLOB in SET: same EMPTY_BLOB()/EMPTY_CLOB() +
 *     SELECT FOR UPDATE pattern as insert.
 *   - WHERE key columns always bind as SQLT_STR scalars.
 *
 * Reuses:
 *   - OCI_Insert_Validate_Module  (validate_field() - same type rules)
 *   - OCI_Table_Metadata_Module   (metadata_cache_get_or_fetch)
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
#include "OCI_Level2_Parser.h"          /* level2_validate_update()      */
#include "OCI_Response_Writer.h"        /* response_write_dml_xml/json() */
#include "OCI_Audit_Trail_Manager.h"
#include "XML_Helper.h"
#include "logger.h"
#include "metrics.h"
#include "metrics_writer.h"   /* metrics_finalise_and_enqueue() - closure item 5, Stage 2 */
#include "resultset_cache.h"  /* resultset_cache_invalidate_by_table() - closure item 5 follow-up, 2026-08-12 */
#include "OCI_Transaction_Manager.h"

/* Bug fix (2026-08-25/27) - moved to file scope from inside
 * execute_update_batch() itself, since the dynamic ROWID collector
 * helpers below (added 2026-08-27) also need it, and they're defined
 * ahead of that function in this file. Oracle's extended ROWID format
 * is always exactly 18 characters; 24 leaves a safety margin. */
#define ROWID_BUF_SIZE 24

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
/*  large_value mirrors field_value_t's own overflow handling in         */
/*  OCI_Request_Response_Types.h, one layer down - only the CLOB SET     */
/*  write path (handle_clob_update) can ever need this; scalar columns   */
/*  are already bounded by their real Oracle column length via           */
/*  level2_validate_update(), and BLOB values arrive as a file path      */
/*  string, never inline content, so neither can realistically exceed    */
/*  MAX_COL_VALUE_SIZE. See upd_field_value_get() below.                  */
/* ------------------------------------------------------------------ */
typedef struct {
    char  value[MAX_COL_VALUE_SIZE];
    char *large_value;
    int   is_empty;
} upd_field_value_t;

/* Real value for one upd_field_value_t - see its own large_value        */
/* comment above.                                                        */
static const char *upd_field_value_get(const upd_field_value_t *v)
{
    return v->large_value ? v->large_value : v->value;
}

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

/* Frees every entry's large_value (a calloc'd array's untouched
 * entries are already NULL - safe no-ops) before the caller frees
 * values itself - same reasoning as free_insert_ctx_values() in
 * OCI_Insert_Execute_Module.c.                                        */
static void free_update_ctx_values(upd_field_value_t *values, int count)
{
    if (!values) return;
    for (int i = 0; i < count; i++)
        free(values[i].large_value);
    free(values);
}

/* ------------------------------------------------------------------ */
/*  build_update_ctx_from_request                                       */
/*  Populates update_ctx_t directly from an already-parsed               */
/*  update_request_t - replaces the old parse_update_xml(); no XML       */
/*  parsing happens in this file at all any more.                        */
/*                                                                         */
/*  Always exactly one logical "row" of SET values (uc->row_count = 1) -  */
/*  matches the old XML template's own convention (borrowed from          */
/*  INSERT's per-row shape, but an UPDATE only ever has one flat SET      */
/*  list applied to however many rows the WHERE clause matches at         */
/*  execute time - see update_request_t's own doc comment in              */
/*  OCI_Update_Execute_Module.h).                                         */
/* ------------------------------------------------------------------ */
static int build_update_ctx_from_request(oci_context_t          *ctx,
                                          const update_request_t *req,
                                          update_ctx_t           *uc)
{
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Entering build_update_ctx_from_request");

    memset(uc, 0, sizeof(*uc));

    strncpy(uc->table_name, req->table_name, sizeof(uc->table_name) - 1);
    strncpy(uc->owner,      req->owner,      sizeof(uc->owner) - 1);

    /* ---- WHERE keys ---- */
    if (req->key_count <= 0 || req->key_count > MAX_UPD_KEY_COLS)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "key_count=%d out of range (1..%d) - "
                     "level2_validate_update() should have caught this",
                     req->key_count, MAX_UPD_KEY_COLS);
        return -1;
    }
    uc->key_count = req->key_count;
    for (int k = 0; k < req->key_count; k++)
    {
        strncpy(uc->keys[k].field_name, req->keys[k].field_name,
                sizeof(uc->keys[k].field_name) - 1);
        strncpy(uc->keys[k].key_value, req->keys[k].key_value,
                sizeof(uc->keys[k].key_value) - 1);
        /* field_type left empty - build_update_sql() resolves the real
         * type itself from cols[] metadata, not from this struct.      */
    }

    /* ---- SET fields - always exactly one logical row (see this
     * function's own doc comment).                                     */
    if (req->field_count <= 0 || req->field_count > MAX_UPD_COLS)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "field_count=%d out of range (1..%d) - "
                     "level2_validate_update() should have caught this",
                     req->field_count, MAX_UPD_COLS);
        return -1;
    }
    uc->col_count = req->field_count;
    uc->row_count = 1;

    for (int c = 0; c < req->field_count; c++)
        strncpy(uc->col_names[c], req->fields[c].field_name,
                sizeof(uc->col_names[c]) - 1);

    uc->values = calloc((size_t)uc->col_count, sizeof(upd_field_value_t));
    if (!uc->values)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for values array (%d cols x %zu bytes)",
                     uc->col_count, sizeof(upd_field_value_t));
        return -1;
    }

    for (int c = 0; c < req->field_count; c++)
    {
        const char *real_value = field_value_get(&req->fields[c]);
        size_t      real_len   = strlen(real_value);

        upd_field_value_t *dest = &uc->values[c];

        if (real_len < sizeof(dest->value))
        {
            strncpy(dest->value, real_value, sizeof(dest->value) - 1);
            dest->large_value = NULL;
        }
        else
        {
            /* Doesn't fit even MAX_COL_VALUE_SIZE - mirror
             * field_value_t's own overflow handling. See
             * upd_field_value_t's doc comment.                         */
            dest->large_value = malloc(real_len + 1);
            if (dest->large_value)
                memcpy(dest->large_value, real_value, real_len + 1);
            strncpy(dest->value, real_value, sizeof(dest->value) - 1);
            dest->value[sizeof(dest->value) - 1] = '\0';
        }
        dest->is_empty = (real_len == 0);
    }

    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "build_update_ctx_from_request OK: keys=%d cols=%d "
                 "allocated=%zu bytes",
                 uc->key_count, uc->col_count,
                 (size_t)uc->col_count * sizeof(upd_field_value_t));
    return 0;
}


/* ================================================================== */
/*  get_upd_bind_wrapper                                                */
/*  Returns SQL expression wrapper for date/time/LOB types.            */
/*  BLOB/CLOB use EMPTY_BLOB()/EMPTY_CLOB() in SET clause;            */
/*  WHERE key columns always bind as plain SQLT_STR.                   */
/* ================================================================== */
/*
 * get_upd_bind_wrapper()
 * Same design as OCI_Insert_Execute_Module.c's get_bind_wrapper() -
 * see that function's own doc comment for the full 2026-07-28
 * reasoning (no hardcoded date format literal any more; reads
 * ctx->ini->nls_date_format fresh on every call instead).
 */
static int get_upd_bind_wrapper(oci_context_t *ctx, const char *dtype,
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
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "MONTH"))
    {
        snprintf(dest, dest_max, "TO_YMINTERVAL(%%s)");
        return 1;
    }
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "SECOND"))
    {
        snprintf(dest, dest_max, "TO_DSINTERVAL(%%s)");
        return 1;
    }
    if (strcmp(dtype, "BLOB") == 0)
    {
        snprintf(dest, dest_max, "EMPTY_BLOB()");
        return 1;
    }
    if (strcmp(dtype, "CLOB")  == 0 ||
        strcmp(dtype, "NCLOB") == 0)
    {
        snprintf(dest, dest_max, "EMPTY_CLOB()");
        return 1;
    }
    return 0;
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
                              size_t                sql_max,
                              int                  *out_bind_num)
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

        char wrapper_buf[128] = {0};
        int  has_wrapper = get_upd_bind_wrapper(ctx, dtype, wrapper_buf, sizeof(wrapper_buf));
        const char *wrapper = has_wrapper ? wrapper_buf : NULL;
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

        /* Apply date/timestamp/interval wrapper to WHERE keys too if
         * needed - resolved from cols[] by real column name lookup,
         * same as the SET clause above. Found and fixed 2026-07-26:
         * this used to read uc->keys[k].field_type directly, a field
         * build_update_ctx_from_request() deliberately leaves empty -
         * see that function's own comment - meaning this comparison
         * could never match and the wrapper was silently never applied
         * for any WHERE key, ever. Isolated to just this one section;
         * the SET clause loop above it, and every other type
         * resolution in this project, already does the real lookup.  */
        const char *ktype = "VARCHAR2";
        for (int m = 0; m < col_meta_count; m++)
            if (strcasecmp(cols[m].col_name, uc->keys[k].field_name) == 0)
            { ktype = cols[m].data_type; break; }

        char wrapper_buf[128] = {0};
        int  has_wrapper = get_upd_bind_wrapper(ctx, ktype, wrapper_buf, sizeof(wrapper_buf));
        const char *wrapper = has_wrapper ? wrapper_buf : NULL;

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
    /* Bug fix (2026-08-25) - RETURNING ROWID INTO, same fix as
     * execute_insert_batch()'s own (see build_insert_sql()'s doc
     * comment for the full rationale - OCI_ATTR_ROWID unreliable for
     * multi-row statements, and the old code only ever wrote row_base's
     * LOB content regardless of how many rows the UPDATE actually
     * matched). One difference from the INSERT side worth noting:
     * bind_pos here is POST-incremented throughout the SET and WHERE
     * loops above (used, then incremented) - so its final value is
     * ALREADY the next available bind position, unlike build_insert_
     * sql()'s bind_num which needed +1. Using bind_pos directly here. */
    int rowid_bind_pos = bind_pos;

    if (strlen(uc->owner) > 0)
        n = snprintf(sql_buf, sql_max,
                     "UPDATE %s.%s SET %s WHERE %s "
                     "RETURNING ROWID INTO :%d",
                     uc->owner, uc->table_name, set_list, where_list,
                     rowid_bind_pos);
    else
        n = snprintf(sql_buf, sql_max,
                     "UPDATE %s SET %s WHERE %s "
                     "RETURNING ROWID INTO :%d",
                     uc->table_name, set_list, where_list, rowid_bind_pos);

    if (n < 0 || (size_t)n >= sql_max)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "UPDATE SQL truncated - increase sql_buf size");
        return -1;
    }

    if (out_bind_num) *out_bind_num = rowid_bind_pos;

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
/* ================================================================== */
/*  Dynamic ROWID collector - OCIBindDynamic support for multi-row     */
/*  RETURNING ROWID INTO                                                */
/*  Bug fix (2026-08-27)                                                */
/* ================================================================== */
/*
 * Closes the remaining gap in the 2026-08-25 RETURNING ROWID INTO fix.
 * uc->row_count is ALWAYS 1 (single WHERE + single SET per UPDATE
 * request - see build_update_ctx_from_request()'s own comment), so
 * OCIStmtExecute() always runs with iters=1 - but that ONE execution's
 * WHERE clause can still match MANY physical rows if it isn't unique
 * (confirmed directly against real data: WHERE NUMBER_COL=300 matching
 * 209 rows). A STATIC array bind, sized to execute_count (the number
 * of BIND ITERATIONS, always 1 here - never the number of rows one
 * iteration's WHERE clause might match), can only ever receive ONE
 * ROWID back - Oracle silently reports just one of the 209 affected
 * rows, and only that one row's LOB content ever got written.
 *
 * OCIBindDynamic replaces the static bind for the ROWID output only.
 * Oracle invokes rowid_out_callback() once per row it's actually
 * returning a value for - genuinely unknown in advance how many times,
 * since it depends on how many rows the WHERE clause matches at
 * execute time. The callback grows this collector as needed and hands
 * back a fresh slot each call; after OCIStmtExecute() returns,
 * collector.count is the REAL number of affected rows.
 *
 * All discovered rows share the exact same SET-clause field values -
 * there's only ever ONE set of values, since that's what a normal
 * UPDATE...WHERE genuinely does. This fix's job is purely "write that
 * one set of LOB content to every row Oracle says was actually
 * affected", using each row's own correct ROWID - not "give each row
 * different content", which isn't what's happening here and isn't
 * something today's request schema can even express (see
 * HTTP_Consumer_Design_Specification.docx Section 12 for the separate,
 * still-out-of-scope case that would need genuinely per-row values).
 */
typedef struct {
    char           *bufs;       /* flat, growable: [count * ROWID_BUF_SIZE] */
    sb2            *inds;       /* growable: [count]                        */
    ub2            *rcodes;     /* growable: [count] - return codes, kept
                                    for completeness, not currently
                                    inspected                                */
    ub4             alen;       /* struct-owned, not shared/static - see
                                    rowid_out_callback()'s own note below    */
    ub4             count;      /* rows Oracle has actually reported so far */
    ub4             capacity;   /* allocated capacity, in rows               */
    oci_context_t  *ctx;        /* for logging on growth failure only        */
} dynamic_rowid_collector_t;

static void dynamic_rowid_collector_init(dynamic_rowid_collector_t *c, oci_context_t *ctx)
{
    memset(c, 0, sizeof(*c));
    c->ctx = ctx;
}

static void dynamic_rowid_collector_free(dynamic_rowid_collector_t *c)
{
    free(c->bufs);
    free(c->inds);
    free(c->rcodes);
    memset(c, 0, sizeof(*c));
}

/* Grows the collector to hold at least (index+1) rows if it doesn't
 * already - doubling growth, starting capacity 16 (handles the common
 * "matches a handful of rows" case with zero reallocation). Returns 1
 * on success, 0 on allocation failure. */
static int dynamic_rowid_collector_ensure(dynamic_rowid_collector_t *c, ub4 index)
{
    if (index < c->capacity) return 1;

    ub4 new_capacity = c->capacity == 0 ? 16 : c->capacity * 2;
    while (new_capacity <= index) new_capacity *= 2;

    char *new_bufs   = realloc(c->bufs,   (size_t)new_capacity * ROWID_BUF_SIZE);
    sb2  *new_inds   = realloc(c->inds,   (size_t)new_capacity * sizeof(sb2));
    ub2  *new_rcodes = realloc(c->rcodes, (size_t)new_capacity * sizeof(ub2));
    if (!new_bufs || !new_inds || !new_rcodes)
    {
        logger_write(c->ctx->update_logger, LOG_ERROR, __func__, 0,
                     "dynamic_rowid_collector: realloc failed growing to "
                     "%u rows", new_capacity);
        if (new_bufs)   c->bufs   = new_bufs;
        if (new_inds)   c->inds   = new_inds;
        if (new_rcodes) c->rcodes = new_rcodes;
        return 0;
    }

    /* Zero the newly-added region - each new ROWID slot is naturally
     * NUL-terminated once Oracle writes its fixed-length 18 characters,
     * same reasoning the earlier static array bind's own calloc used. */
    memset(new_bufs + (size_t)c->capacity * ROWID_BUF_SIZE, 0,
           (size_t)(new_capacity - c->capacity) * ROWID_BUF_SIZE);

    c->bufs = new_bufs;
    c->inds = new_inds;
    c->rcodes = new_rcodes;
    c->capacity = new_capacity;
    return 1;
}

/* OCI's required input-callback signature for a dynamically-bound
 * placeholder - even though this bind is RETURNING-only (write-only
 * from Oracle's perspective; nothing for us to supply going in), OCI
 * still requires a real, non-NULL input callback to be registered.
 * Always hands back the same empty, read-only, zero-length buffer -
 * genuinely safe to share across any number of calls, since nothing
 * ever writes through it. */
static sb4 rowid_in_callback(void *ictxp, OCIBind *bindp, ub4 iter, ub4 index,
                              void **bufpp, ub4 *alenp, ub1 *piecep, void **indpp)
{
    (void)ictxp; (void)bindp; (void)iter; (void)index;
    static const char empty = '\0';
    *bufpp  = (void *)&empty;
    *alenp  = 0;
    *piecep = OCI_ONE_PIECE;
    *indpp  = NULL;
    return OCI_CONTINUE;
}

/* OCI's required output-callback signature. Invoked once per row
 * Oracle is returning a RETURNING value for - iter is always 0 here
 * (this statement has exactly one bind iteration; the multiplicity is
 * in ROWS the WHERE clause matches, not iterations), index increments
 * 0, 1, 2... once per row discovered, in whatever order Oracle
 * processes them.
 *
 * c->alen is struct-owned, deliberately not a shared/static local.
 * Every write path in this codebase is already serialized through a
 * single dedicated writer queue (both consumers' own contention
 * managers), so a static variable would very likely have been "safe"
 * in practice - but relying on that invariant holding forever, this
 * deep inside an OCI callback, is exactly the kind of thing that
 * becomes a silent, hard-to-diagnose bug the one time it doesn't. */
static sb4 rowid_out_callback(void *octxp, OCIBind *bindp,
                               ub4 iter, ub4 index,
                               void **outbufpp, ub4 **alenpp,
                               ub1 *piecep, void **indpp, ub2 **rcodepp)
{
    dynamic_rowid_collector_t *c = (dynamic_rowid_collector_t *)octxp;
    (void)bindp; (void)iter;

    if (!dynamic_rowid_collector_ensure(c, index))
    {
        *outbufpp = NULL;
        return OCI_ERROR;
    }

    c->alen   = (ub4)ROWID_BUF_SIZE;
    *outbufpp = c->bufs + (size_t)index * ROWID_BUF_SIZE;
    *alenpp   = &c->alen;
    *piecep   = OCI_ONE_PIECE;
    *indpp    = &c->inds[index];
    *rcodepp  = &c->rcodes[index];

    if (index + 1 > c->count) c->count = index + 1;

    return OCI_CONTINUE;
}

int execute_update_batch(oci_context_t     *ctx,
                          update_request_t  *req,
                          execute_config_t  *cfg)
{
    int            rc           = 0;
    OCIStmt       *stmt         = NULL;
    xml_builder_t *xml          = NULL;
    update_ctx_t  *uc           = NULL;
    OCIBind      **bind_hdls    = NULL;
    char         **scalar_bufs  = NULL;
    sb2           *indicators   = NULL;

    /* Bug fix (2026-08-25, superseded by the dynamic version 2026-08-27) -
     * RETURNING ROWID INTO. Originally a static array bind mirroring
     * execute_insert_batch()'s own fix, sized to execute_count - but
     * execute_count is always 1 for UPDATE (see build_update_ctx_from_
     * request()'s own comment), while the WHERE clause it binds against
     * can still match many physical rows if it isn't unique. Replaced
     * with dynamic_rowid_collector_t (defined above this function) -
     * see that struct's own doc comment for the full rationale. */
    OCIBind                   *rowid_bind_hdl = NULL;
    dynamic_rowid_collector_t  rowid_collector = {0};

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

    if (!ctx || !req || !cfg)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments");
        return -1;
    }

    /* Give this call its own transaction identity if it doesn't already
     * have one - fixes the 2026-07-26 GxP traceability gap. See
     * execute_insert_batch()'s identical fix and the full reasoning in
     * OCI_Transaction_Manager.h's own doc comment for these functions.
     * Particularly relevant here: this is exactly the call chain
     * (before-image SELECT -> AUDIT_TRAIL INSERT -> the UPDATE itself)
     * that first surfaced the gap, via session_end()'s standalone
     * calls.                                                            */
    tx_handle_t local_tx;
    int owns_standalone_tx = begin_standalone_tx_if_needed(ctx, &local_tx);


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
            /* Same source as transaction_id above, just the name -
             * closure item 5 follow-up (2026-08-10).                  */
            strncpy(metrics.transaction_name,
                    ctx->active_tx ? ctx->active_tx->tx_name : "-",
                    sizeof(metrics.transaction_name) - 1);

    /* ================================================================
     *  Stage 1 - Validate
     *  Called internally rather than trusted to have already run in
     *  the caller - see this file's own top-of-file doc comment.
     * ================================================================ */
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Validating request");

    input_c_operation_t validate_op;
    memset(&validate_op, 0, sizeof(validate_op));
    validate_op.type    = OP_UPDATE;
    validate_op.payload = (void *)req;

    operation_status_t val_status;
    memset(&val_status, 0, sizeof(val_status));

    if (level2_validate_update(ctx, &validate_op, &val_status) != LEVEL2_OK)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "Stage 1 validation failed: %s", val_status.error_text);
        return -1;
    }
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 1 validation passed");

    /* ================================================================
     *  Stage 2 - Build update context and prepare statement
     * ================================================================ */
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 2: Building update context and preparing statement");

    uc = calloc(1, sizeof(update_ctx_t));
    if (!uc)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for update_ctx_t");
        rc = -1;
        goto Cleanup;
    }

    if (build_update_ctx_from_request(ctx, req, uc) != 0)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "build_update_ctx_from_request failed");
        rc = -1;
        goto Cleanup;
    }
    strncpy(metrics.object_name, uc->table_name,
             sizeof(metrics.object_name) - 1);

    /* No row_count cap here any more - uc->row_count is always exactly
     * 1 for UPDATE now (one flat SET list; see build_update_ctx_from_
     * request()'s own doc comment), not a client-supplied number that
     * needs bounding the way INSERT's row_count does.                  */

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
    int  rowid_bind_num = 0;
    if (build_update_sql(ctx, uc, cols, col_meta_count,
                          sql_buf, sizeof(sql_buf), &rowid_bind_num) != 0)
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

    /* Bug fix (2026-08-27) - the dynamic collector needs no upfront
     * allocation at all (that's the whole point - it grows to whatever
     * size Oracle actually reports at execute time, not a size guessed
     * in advance). Just initialize it here, in scope for the whole
     * function. */
    dynamic_rowid_collector_init(&rowid_collector, ctx);

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
        /* Build key name/value/type arrays from uc->keys[] */
        char (*key_names) [128]   = NULL;
        char (*key_vals)  [32768] = NULL;
        char (*key_types) [128]   = NULL;

        key_names = calloc((size_t)uc->key_count, sizeof(*key_names));
        key_vals  = calloc((size_t)uc->key_count, sizeof(*key_vals));
        key_types = calloc((size_t)uc->key_count, sizeof(*key_types));

        if (key_names && key_vals && key_types)
        {
            for (int k = 0; k < uc->key_count; k++)
            {
                strncpy(key_names[k], uc->keys[k].field_name,
                        sizeof(key_names[k]) - 1);
                strncpy(key_vals[k],  uc->keys[k].key_value,
                        sizeof(key_vals[k])  - 1);

                /* Real type from cols[] - never trust a client-
                 * supplied type (there isn't one here anyway; see the
                 * 2026-07-26 fix in OCI_Audit_Trail_Manager.c/.h for
                 * why this parameter exists at all now).               */
                strncpy(key_types[k], "VARCHAR2", sizeof(key_types[k]) - 1);
                for (int m = 0; m < col_meta_count; m++)
                    if (strcasecmp(cols[m].col_name, uc->keys[k].field_name) == 0)
                    {
                        strncpy(key_types[k], cols[m].data_type,
                                sizeof(key_types[k]) - 1);
                        break;
                    }
            }

            int fetch_rc =
                audit_trail_fetch_before_image(ctx,
                                               uc->table_name,
                                               uc->owner,
                                               uc->col_names,
                                               uc->col_count,
                                               key_names,
                                               key_vals,
                                               key_types,
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
        free(key_types);
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

        /* Bug fix (2026-08-27) - bind the RETURNING ROWID INTO output
         * via OCIBindDynamic instead of a static array (see
         * dynamic_rowid_collector_t's own doc comment above this
         * function for the full rationale). OCIBindByPos() still
         * establishes the position/datatype exactly as before, but
         * with a NULL value buffer and OCI_DATA_AT_EXEC mode (instead
         * of OCI_DEFAULT) - this tells OCI the real data will be
         * supplied dynamically via callback, not directly here.
         * OCIBindDynamic() then registers the two callback functions
         * that do the actual work. */
        rowid_collector.count = 0;   /* defensive reset - see
                                         dynamic_rowid_collector_t's own
                                         comment on why the outer while
                                         loop only ever runs once today
                                         (uc->row_count is always 1),
                                         making this a no-op in
                                         practice, not dead code for a
                                         reason - if that assumption is
                                         ever relaxed later, this still
                                         does the right thing. */

        CHECK_OCI_UPD(ctx->errhp,
            OCIBindByPos(stmt, &rowid_bind_hdl, ctx->errhp,
                         (ub4)rowid_bind_num,
                         NULL,
                         (sb4)ROWID_BUF_SIZE,
                         SQLT_STR,
                         NULL,
                         NULL, NULL, 0, NULL,
                         OCI_DATA_AT_EXEC),
            ctx, Cleanup);

        CHECK_OCI_UPD(ctx->errhp,
            OCIBindDynamic(rowid_bind_hdl, ctx->errhp,
                           (void *)&rowid_collector, rowid_in_callback,
                           (void *)&rowid_collector, rowid_out_callback),
            ctx, Cleanup);

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

        /* ---- Post-execute: write LOB data ----
         * Bug fix (2026-08-25, extended 2026-08-27) - this block used
         * to only ever touch uc->values[row_base * uc->col_count + bc] -
         * ONLY the first row of the batch, for every column, regardless
         * of how many physical rows the WHERE clause actually matched.
         * The 2026-08-25 fix wrapped this in a per-row loop bounded by
         * batch_rows - correct in shape, but batch_rows is always 1 for
         * UPDATE (see build_update_ctx_from_request()'s own comment),
         * so it never actually exercised more than one row either. The
         * real bound is rowid_collector.count - the number of rows
         * Oracle's own RETURNING clause reported as affected, which can
         * genuinely be many when the WHERE clause isn't unique
         * (confirmed directly: WHERE NUMBER_COL=300 matching 209 rows).
         *
         * The value lookup is deliberately NOT row_base + r here - r
         * now indexes DISCOVERED ROWS (from the dynamic collector), not
         * VALUE rows. There is only ever one set of SET-clause values
         * for the whole UPDATE (uc->values[row_base * col_count + bc],
         * fixed), and that same one set of values gets written to
         * EVERY row Oracle reports as affected - that's what a normal
         * UPDATE...WHERE genuinely does; this fix's job is making sure
         * every one of those rows actually receives it, not giving each
         * row different content (which isn't what's happening here and
         * isn't something today's request schema could even express).
         *
         * has_lob_column is a pure schema check - the per-row is_empty
         * check inside the loop still correctly skips a genuinely empty
         * LOB value.                                                    */
        {
            int has_lob_column = 0;
            for (int bc = 0; bc < uc->col_count && !has_lob_column; bc++)
                for (int m = 0; m < col_meta_count; m++)
                    if (strcasecmp(cols[m].col_name, uc->col_names[bc]) == 0 &&
                        (strcmp(cols[m].data_type, "BLOB")  == 0 ||
                         strcmp(cols[m].data_type, "CLOB")  == 0 ||
                         strcmp(cols[m].data_type, "NCLOB") == 0))
                    { has_lob_column = 1; break; }

            if (has_lob_column)
            {
                char tbl_fq[256] = {0};
                if (strlen(uc->owner) > 0)
                    snprintf(tbl_fq, sizeof(tbl_fq), "%s.%s",
                             uc->owner, uc->table_name);
                else
                    snprintf(tbl_fq, sizeof(tbl_fq), "%s",
                             uc->table_name);

                logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                             "UPDATE affected %u row(s) (RETURNING ROWID "
                             "reported via OCIBindDynamic) - writing LOB "
                             "content to every one", rowid_collector.count);

                for (ub4 r = 0; r < rowid_collector.count; r++)
                {
                    const char *row_rowid =
                        rowid_collector.bufs + (size_t)r * ROWID_BUF_SIZE;

                    logger_write(ctx->update_logger, LOG_DEBUG, __func__, 0,
                                 "Updated row r=%u ROWID='%s'", r, row_rowid);

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
                                                   tbl_fq, row_rowid,
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
                                                   btype, tbl_fq, row_rowid,
                                                   upd_field_value_get(fv), 0,
                                                   &clob_bytes) != 0)
                            { rc = -1; goto Cleanup; }
                            lob_count++;
                        }
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

    /* Table-level resultset cache invalidation (closure item 5
     * follow-up, 2026-08-12) - regression fix for UT-SEL-004. Same
     * "fires regardless of managed-tx vs auto-commit, erring toward
     * eager invalidation over any risk of stale reads" reasoning as
     * OCI_Insert_Execute_Module.c's own identical block - see that
     * one's own comment for the full explanation.                     */
    resultset_cache_invalidate_by_table(ctx->resultset_cache, req->table_name);



    /* ================================================================
     *  Stage 5 - Build result response
     *  Uses response_write_dml_xml()/response_write_dml_json() - same
     *  writers built for INSERT, reused unchanged here (that's the
     *  whole reason dml_response_t is one shared struct rather than
     *  three near-duplicates - see its own doc comment in
     *  OCI_Request_Response_Types.h).
     * ================================================================ */
    logger_write(ctx->update_logger, LOG_INFO, __func__, 0,
                 "Stage 5: Building result response");

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    dml_response_t resp;
    memset(&resp, 0, sizeof(resp));
    strncpy(resp.table_name, uc->table_name, sizeof(resp.table_name) - 1);
    strncpy(resp.owner,      uc->owner,      sizeof(resp.owner) - 1);
    resp.rows_affected          = rows_updated;
    resp.lobs_written           = lob_count;
    resp.execution_time_seconds = elapsed;

    char *dml_xml_fragment = response_write_dml_xml(ctx, OP_UPDATE, &resp);
    if (!dml_xml_fragment)
    {
        logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                     "response_write_dml_xml returned NULL");
        rc = -1;
        goto Cleanup;
    }

    xml = xml_create(4096);
    if (!xml) { free(dml_xml_fragment); rc = -1; goto Cleanup; }

    xml_start_document(xml);
    xml_start_execution(xml);
    /* xml_append_raw(), not xml_append(xml,"%s",...) - see the
     * 2026-07-22 fix in OCI_Execute_Query_Batch_Module.c for why:
     * the latter formats into a fixed 8192-byte stack buffer and
     * silently corrupts anything longer.                               */
    xml_append_raw(xml, dml_xml_fragment);
    xml_end_execution(xml);
    xml_finalize(xml);
    free(dml_xml_fragment);

    /* cfg->OUTPUT_JSON's own doc comment in OCI_Connection.h: "set
     * only when ReturnFormat is JSON. NULL otherwise."                  */
    if (cfg->ReturnFormat && strcasecmp(cfg->ReturnFormat, "JSON") == 0)
    {
        cfg->OUTPUT_JSON = response_write_dml_json(ctx, OP_UPDATE, &resp);
        if (!cfg->OUTPUT_JSON)
            logger_write(ctx->update_logger, LOG_ERROR, __func__, 0,
                         "response_write_dml_json returned NULL - "
                         "OUTPUT_JSON will be missing for this JSON-format request");
    }

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
		strncpy(metrics.transaction_id , tx_get_id(ctx->active_tx),sizeof(metrics.transaction_id)-1);
	else
		strncpy(metrics.transaction_id , "-",sizeof(metrics.transaction_id)-1);
	strncpy(metrics.transaction_name , ctx->active_tx ? ctx->active_tx->tx_name : "-", sizeof(metrics.transaction_name)-1);
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





	metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &metrics);
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
    /* Bug fix (2026-08-27) - free the dynamic ROWID collector's own
     * growable buffers. rowid_bind_hdl needs no explicit free - OCI
     * bind handles are owned by the statement, released with it via
     * OCIStmtRelease, same as before this fix. */
    dynamic_rowid_collector_free(&rowid_collector);

    if (uc)
    {
        if (uc->values) free_update_ctx_values(uc->values, uc->col_count);
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

    end_standalone_tx_if_owned(ctx, owns_standalone_tx);

    return rc;
}
