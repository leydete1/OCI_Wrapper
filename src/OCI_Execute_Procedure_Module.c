/*
 * OCI_Execute_Procedure_Module.c
 *
 * Stored Procedure Execution Module
 * -----------------------------------
 * Executes an Oracle stored procedure or function via a dynamically
 * built anonymous PL/SQL block, handles IN/OUT/IN_OUT scalar parameters
 * of all common Oracle types, and supports SYS_REFCURSOR OUT parameters
 * whose result sets are fetched using the same batch context pattern
 * proven in OCI_Execute_Query_Batch_Module.
 *
 * Internal structure
 * ------------------
 *   parse_procedure_xml()      - parse XML into proc_ctx_t
 *   build_plsql_block()        - build BEGIN proc(:p1,:p2,...); END;
 *   bind_parameters()          - OCIBindByName for all params
 *   fetch_cursor_to_xml()      - describe + batch-fetch one REFCURSOR
 *                                mirrors define_columns_batch /
 *                                build_row_xml_batch from batch module
 *   collect_out_parameters()   - read scalar OUT values post-execute
 *   execute_procedure()        - orchestrate all stages
 *
 * CURSOR fetch notes
 * ------------------
 * A CURSOR OUT parameter is bound as SQLT_RSET.  After OCIStmtExecute
 * the bind buffer holds an OCIStmt* pointing to an open cursor.
 * That cursor handle is then described and fetched exactly as
 * execute_query_batch describes and fetches a SELECT statement.
 * The CLOB array-fetch restriction (force fetch_count=1 when any CLOB
 * column is present) is preserved here for the same reason documented
 * in OCI_Execute_Query_Batch_Module.c.
 *
 * Scalar OUT binding
 * ------------------
 * After OCIStmtExecute, each scalar OUT/IN_OUT bind buffer contains
 * the returned value as a null-terminated string (SQLT_STR) or integer
 * (SQLT_INT for NUMBER/INTEGER).  These are read directly from the
 * bind buffers and emitted into the <out_parameters> XML block.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#include "OCI_Execute_Procedure_Module.h"
#include "OCI_Execute_Query_Module.h"   /* trim_sql_inplace, lob helpers */
#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"
#include "OCI_Transaction_Manager.h"
#include "metrics.h"

/* ------------------------------------------------------------------ */
/*  OCI error macro - same pattern as the rest of the project          */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_PROC(errhp, status, ctx, label)                       \
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
#define MAX_PROC_PARAMS      64      /* max parameters per procedure   */
#define MAX_PARAM_VALUE_SIZE 32768   /* max scalar bind buffer         */
#define MAX_PROC_NAME_LEN    256     /* procedure name incl. owner     */
#define MAX_PLSQL_BLOCK_LEN  8192   /* generated PL/SQL block size    */
#define MAX_CURSOR_COLS      512    /* columns per REFCURSOR result   */

/* ------------------------------------------------------------------ */
/*  Parameter direction enum                                            */
/* ------------------------------------------------------------------ */
typedef enum {
    PARAM_IN     = 0,
    PARAM_OUT    = 1,
    PARAM_IN_OUT = 2
} param_direction_t;

/* ------------------------------------------------------------------ */
/*  Per-parameter descriptor                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char               param_name [128];
    char               param_type [64];
    param_direction_t  direction;
    char               param_value[MAX_PARAM_VALUE_SIZE];

    /* Bind handle - one per parameter */
    OCIBind           *bind_hdl;

    /* For scalar OUT/IN_OUT: post-execute value buffer                */
    char               out_value  [MAX_PARAM_VALUE_SIZE];
    int                out_int;          /* used when param_type=INTEGER/NUMBER */
    sb2                indicator;        /* OCI NULL indicator                  */

    /* For CURSOR OUT: the fetched cursor statement handle             */
    OCIStmt           *cursor_stmt;      /* populated after execute             */
    int                is_cursor;        /* 1 if param_type == "CURSOR"         */
    int                is_integer;       /* 1 if bound as SQLT_INT              */
    int                is_numeric;       /* 1 if NUMBER/FLOAT (bound as str)    */
} proc_param_t;

/* ------------------------------------------------------------------ */
/*  Parsed procedure context                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char          proc_name  [MAX_PROC_NAME_LEN];
    char          owner      [128];
    int           param_count;
    proc_param_t  params     [MAX_PROC_PARAMS];
} proc_ctx_t;

/* ------------------------------------------------------------------ */
/*  Cursor fetch batch context - mirrors batch_ctx_t from batch module */
/* ------------------------------------------------------------------ */
typedef struct {
    ub4              col_count;
    ub4              fetch_count;

    OCIDefine      **def;
    char           **buffers;
    ub4             *buf_sizes;
    sb2            **indicators;
    ub2             *data_types;
    ub4             *data_sizes;
    char           (*col_names)[256];
    OCILobLocator ***col_blob_locs;
    OCILobLocator   *clob_loc;
} cur_batch_ctx_t;

/* ================================================================== */
/*  Static helpers                                                      */
/* ================================================================== */
static void trim_proc(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    { s[len - 1] = '\0'; len--; }
}

static int extract_tag_proc(const char *src, const char *tag,
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
    trim_proc(dest);
    return 1;
}

static void uppercase_proc(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* Map "IN" / "OUT" / "IN_OUT" string to enum */
static param_direction_t parse_direction(const char *s)
{
    if (strcasecmp(s, "OUT")    == 0) return PARAM_OUT;
    if (strcasecmp(s, "IN_OUT") == 0) return PARAM_IN_OUT;
    return PARAM_IN;   /* default */
}

/* ================================================================== */
/*  free_cur_batch_ctx                                                  */
/*  Mirrors free_batch_ctx from OCI_Execute_Query_Batch_Module         */
/* ================================================================== */
static void free_cur_batch_ctx(oci_context_t *ctx, cur_batch_ctx_t *bc)
{
    if (!bc) return;

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Entering free_cur_batch_ctx col_count=%u", bc->col_count);

    if (bc->col_blob_locs)
    {
        for (ub4 i = 0; i < bc->col_count; i++)
        {
            if (bc->col_blob_locs[i])
            {
                for (ub4 r = 0; r < bc->fetch_count; r++)
                    if (bc->col_blob_locs[i][r])
                        OCIDescriptorFree(bc->col_blob_locs[i][r],
                                          OCI_DTYPE_LOB);
                free(bc->col_blob_locs[i]);
                bc->col_blob_locs[i] = NULL;
            }
        }
        free(bc->col_blob_locs);
        bc->col_blob_locs = NULL;
    }

    if (bc->buffers)
    {
        for (ub4 i = 0; i < bc->col_count; i++)
            if (bc->buffers[i]) { free(bc->buffers[i]); bc->buffers[i] = NULL; }
        free(bc->buffers);
        bc->buffers = NULL;
    }

    if (bc->indicators)
    {
        for (ub4 i = 0; i < bc->col_count; i++)
            if (bc->indicators[i]) { free(bc->indicators[i]); bc->indicators[i] = NULL; }
        free(bc->indicators);
        bc->indicators = NULL;
    }

    if (bc->clob_loc)
    {
        OCIDescriptorFree(bc->clob_loc, OCI_DTYPE_LOB);
        bc->clob_loc = NULL;
    }

    if (bc->def)        { free(bc->def);        bc->def        = NULL; }
    if (bc->buf_sizes)  { free(bc->buf_sizes);  bc->buf_sizes  = NULL; }
    if (bc->data_types) { free(bc->data_types); bc->data_types = NULL; }
    if (bc->data_sizes) { free(bc->data_sizes); bc->data_sizes = NULL; }
    if (bc->col_names)  { free(bc->col_names);  bc->col_names  = NULL; }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "free_cur_batch_ctx complete");
}

/* ================================================================== */
/*  parse_procedure_xml                                                 */
/*  Extract procedure_name, owner, and all <parameter> blocks.        */
/* ================================================================== */
static int parse_procedure_xml(oci_context_t *ctx,
                                 const char    *xml,
                                 proc_ctx_t    *pc)
{
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Entering parse_procedure_xml");

    memset(pc, 0, sizeof(*pc));

    /* ---- procedure_name (required) ---- */
    if (!extract_tag_proc(xml, "procedure_name",
                           pc->proc_name, sizeof(pc->proc_name)))
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Missing <procedure_name> in XML");
        return -1;
    }

    /* ---- owner (optional prefix - if set, prepended to proc_name) ---- */
    extract_tag_proc(xml, "owner", pc->owner, sizeof(pc->owner));

    /* If owner supplied and proc_name doesn't already contain a dot,
     * prepend owner so the PL/SQL block uses owner.proc_name           */
    if (strlen(pc->owner) > 0 &&
        strchr(pc->proc_name, '.') == NULL)
    {
        char fq[MAX_PROC_NAME_LEN];
        snprintf(fq, sizeof(fq), "%s.%s", pc->owner, pc->proc_name);
        strncpy(pc->proc_name, fq, sizeof(pc->proc_name) - 1);
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "procedure_name='%s'", pc->proc_name);

    /* ---- Locate <parameters> block ---- */
    const char *params_start = strstr(xml, "<parameters>");
    const char *params_end   = strstr(xml, "</parameters>");

    if (!params_start || !params_end)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Missing <parameters> block in XML");
        return -1;
    }

    size_t pblock_len = (size_t)(params_end - params_start) + 13;
    char  *pblock     = malloc(pblock_len + 1);
    if (!pblock) return -1;
    memcpy(pblock, params_start, pblock_len);
    pblock[pblock_len] = '\0';

    /* ---- Walk every <parameter> entry ---- */
    const char *pp = pblock;
    while ((pp = strstr(pp, "<parameter>")) != NULL)
    {
        const char *pe = strstr(pp, "</parameter>");
        if (!pe || pc->param_count >= MAX_PROC_PARAMS) break;

        size_t plen = (size_t)(pe - pp) + 12;
        char  *pbuf = malloc(plen + 1);
        if (!pbuf) { free(pblock); return -1; }
        memcpy(pbuf, pp, plen);
        pbuf[plen] = '\0';

        proc_param_t *p = &pc->params[pc->param_count];
        memset(p, 0, sizeof(*p));
        p->indicator = 0;

        extract_tag_proc(pbuf, "param_name",      p->param_name,  sizeof(p->param_name));
        extract_tag_proc(pbuf, "param_type",      p->param_type,  sizeof(p->param_type));
        extract_tag_proc(pbuf, "param_value",     p->param_value, sizeof(p->param_value));

        char dir_str[32] = {0};
        extract_tag_proc(pbuf, "param_direction", dir_str,        sizeof(dir_str));
        uppercase_proc(dir_str);
        uppercase_proc(p->param_type);
        p->direction  = parse_direction(dir_str);
        p->is_cursor  = (strcmp(p->param_type, "CURSOR")  == 0);
        p->is_integer = (strcmp(p->param_type, "INTEGER") == 0 ||
                         strcmp(p->param_type, "NUMBER")  == 0);

        logger_write(ctx->logger, LOG_DEBUG, __func__, 0,
                     "Param %d: name='%s' type='%s' dir='%s' "
                     "value='%s' is_cursor=%d",
                     pc->param_count + 1,
                     p->param_name, p->param_type, dir_str,
                     p->param_value, p->is_cursor);

        free(pbuf);
        pc->param_count++;
        pp = pe + 12;
    }

    free(pblock);

    if (pc->param_count == 0)
    {
        logger_write(ctx->logger, LOG_WARN, __func__, 0,
                     "No <parameter> entries found - "
                     "procedure will be called with no parameters");
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "parse_procedure_xml OK: proc='%s' params=%d",
                 pc->proc_name, pc->param_count);
    return 0;
}

/* ================================================================== */
/*  build_plsql_block                                                   */
/*  Generates:  BEGIN proc_name(:P1, :P2, :P3); END;                  */
/* ================================================================== */
static int build_plsql_block(oci_context_t  *ctx,
                               const proc_ctx_t *pc,
                               char             *block_buf,
                               size_t            block_max)
{
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Building PL/SQL block for '%s' params=%d",
                 pc->proc_name, pc->param_count);

    if (pc->param_count == 0)
    {
        /* No parameters - simple call */
        int n = snprintf(block_buf, block_max,
                         "BEGIN %s; END;", pc->proc_name);
        if (n < 0 || (size_t)n >= block_max)
        {
            logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                         "PL/SQL block truncated");
            return -1;
        }
    }
    else
    {
        /* Build parameter list :P1, :P2, ... */
        char param_list[MAX_PLSQL_BLOCK_LEN] = {0};

        for (int i = 0; i < pc->param_count; i++)
        {
            if (i > 0)
                strncat(param_list, ", ",
                        sizeof(param_list) - strlen(param_list) - 1);

            char bind_ref[132];
            snprintf(bind_ref, sizeof(bind_ref),
                     ":%s", pc->params[i].param_name);
            strncat(param_list, bind_ref,
                    sizeof(param_list) - strlen(param_list) - 1);
        }

        int n = snprintf(block_buf, block_max,
                         "BEGIN %s(%s); END;",
                         pc->proc_name, param_list);
        if (n < 0 || (size_t)n >= block_max)
        {
            logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                         "PL/SQL block truncated - too many parameters");
            return -1;
        }
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "PL/SQL block: %s", block_buf);
    return 0;
}

/* ================================================================== */
/*  bind_parameters                                                     */
/*  OCIBindByName for every parameter in direction order.              */
/*  CURSOR OUT: bind as SQLT_RSET using the cursor_stmt handle.        */
/*  INTEGER/NUMBER OUT: bind as SQLT_INT so value is directly usable.  */
/*  All other scalars: bind as SQLT_STR.                               */
/* ================================================================== */
static int bind_parameters(oci_context_t *ctx,
                             proc_ctx_t    *pc,
                             OCIStmt       *stmt)
{
    int rc = 0;

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Entering bind_parameters param_count=%d",
                 pc->param_count);

    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t *p = &pc->params[i];

        /* Build bind name :PARAM_NAME */
        char bind_name[132];
        snprintf(bind_name, sizeof(bind_name), ":%s", p->param_name);


        logger_write(ctx->logger, LOG_DEBUG, __func__, 0,
                     "Binding param %d name='%s' type='%s' "
                     "dir=%d is_cursor=%d",
                     i + 1, p->param_name, p->param_type,
                     p->direction, p->is_cursor);

        if (p->is_cursor)
        {
            /* ---- CURSOR OUT: allocate stmt handle, bind as SQLT_RSET ---- */
            logger_write(ctx->logger, LOG_INFO, __func__, 0,
                         "Allocating cursor stmt handle for '%s'",
                         p->param_name);

            CHECK_OCI_PROC(ctx->errhp,
                OCIHandleAlloc(ctx->envhp,
                               (void **)&p->cursor_stmt,
                               OCI_HTYPE_STMT, 0, NULL),
                ctx, Cleanup);

            CHECK_OCI_PROC(ctx->errhp,
                OCIBindByName(stmt, &p->bind_hdl, ctx->errhp,
                              (text *)bind_name, -1,
                              &p->cursor_stmt,
                              (sb4)sizeof(OCIStmt *),
                              SQLT_RSET,
                              &p->indicator,
                              NULL, NULL, 0, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            logger_write(ctx->logger, LOG_INFO, __func__, 0,
                         "CURSOR bind OK param='%s'", p->param_name);
        }
        else if (p->is_integer &&
                 (p->direction == PARAM_OUT ||
                  p->direction == PARAM_IN_OUT))
        {
            /* ---- INTEGER/NUMBER OUT: bind as SQLT_INT ---- */
            /* Pre-populate with IN value for IN_OUT */
            if (p->direction == PARAM_IN_OUT && strlen(p->param_value) > 0)
                p->out_int = atoi(p->param_value);

            CHECK_OCI_PROC(ctx->errhp,
                OCIBindByName(stmt, &p->bind_hdl, ctx->errhp,
                              (text *)bind_name, -1,
                              &p->out_int,
                              (sb4)sizeof(int),
                              SQLT_INT,
                              &p->indicator,
                              NULL, NULL, 0, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            logger_write(ctx->logger, LOG_DEBUG, __func__, 0,
                         "INTEGER OUT bind OK param='%s'", p->param_name);
        }
        else
        {
            /* ---- Scalar IN / OUT / IN_OUT: bind as SQLT_STR ---- */
            /* Copy IN value into the out_value buffer which OCI     */
            /* will overwrite for OUT/IN_OUT after execute           */
            strncpy(p->out_value, p->param_value,
                    sizeof(p->out_value) - 1);
            p->out_value[sizeof(p->out_value) - 1] = '\0';

            /* NULL indicator for empty IN values */
            if (p->direction == PARAM_IN &&
                strlen(p->out_value) == 0)
                p->indicator = -1;
            else
                p->indicator = 0;

            CHECK_OCI_PROC(ctx->errhp,
                OCIBindByName(stmt, &p->bind_hdl, ctx->errhp,
                              (text *)bind_name, -1,
                              p->out_value,
                              (sb4)MAX_PARAM_VALUE_SIZE,
                              SQLT_STR,
                              &p->indicator,
                              NULL, NULL, 0, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            logger_write(ctx->logger, LOG_DEBUG, __func__, 0,
                         "Scalar bind OK param='%s' value='%s' "
                         "indicator=%d",
                         p->param_name, p->out_value, p->indicator);
        }
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "bind_parameters complete");

Cleanup:
    return rc;
}

/* ================================================================== */
/*  fetch_cursor_to_xml                                                 */
/*  Describes and batch-fetches one REFCURSOR result set into XML.     */
/*  Directly mirrors the describe/define/fetch pattern from            */
/*  OCI_Execute_Query_Batch_Module with full BLOB/CLOB support.        */
/* ================================================================== */
static int fetch_cursor_to_xml(oci_context_t  *ctx,
                                OCIStmt        *cursor_stmt,
                                const char     *param_name,
                                xml_builder_t  *xml)
{
    int             rc   = 0;
    lob_item_t     *BLOB_list = NULL;
    cur_batch_ctx_t bc;
    memset(&bc, 0, sizeof(bc));

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Entering fetch_cursor_to_xml param='%s'", param_name);

    /* ---- fetch_count from ini ---- */
    bc.fetch_count = (ub4)ctx->ini->query_fetch_batch_size;
    if (bc.fetch_count < 1) bc.fetch_count = 1;

    /* ---- Describe the cursor ---- */
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Calling OCIAttrGet OCI_ATTR_PARAM_COUNT");

    CHECK_OCI_PROC(ctx->errhp,
        OCIAttrGet(cursor_stmt, OCI_HTYPE_STMT,
                   &bc.col_count, 0,
                   OCI_ATTR_PARAM_COUNT, ctx->errhp),
        ctx, Cleanup);

    if (bc.col_count == 0)
    {
        logger_write(ctx->logger, LOG_WARN, __func__, 0,
                     "Cursor '%s' has no columns", param_name);
        goto Cleanup;
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Cursor '%s' col_count=%u", param_name, bc.col_count);

    /* ---- Allocate CLOB locator ---- */
    CHECK_OCI_PROC(ctx->errhp,
        OCIDescriptorAlloc(ctx->envhp,
                           (void **)&bc.clob_loc,
                           OCI_DTYPE_LOB, 0, NULL),
        ctx, Cleanup);

    /* ---- Allocate top-level column arrays ---- */
    bc.def           = calloc(bc.col_count, sizeof(OCIDefine *));
    bc.buffers       = calloc(bc.col_count, sizeof(char *));
    bc.buf_sizes     = calloc(bc.col_count, sizeof(ub4));
    bc.indicators    = calloc(bc.col_count, sizeof(sb2 *));
    bc.data_types    = calloc(bc.col_count, sizeof(ub2));
    bc.data_sizes    = calloc(bc.col_count, sizeof(ub4));
    bc.col_names     = calloc(bc.col_count, sizeof(*bc.col_names));
    bc.col_blob_locs = calloc(bc.col_count, sizeof(OCILobLocator **));

    if (!bc.def || !bc.buffers || !bc.buf_sizes || !bc.indicators ||
        !bc.data_types || !bc.data_sizes || !bc.col_names ||
        !bc.col_blob_locs)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "calloc failed for cursor batch arrays");
        rc = -1;
        goto Cleanup;
    }

    /* ---- Define columns - mirrors define_columns_batch ---- */
    OCIParam *param = NULL;

    for (ub4 ci_1 = 1; ci_1 <= bc.col_count; ci_1++)
    {
        ub4 ci = ci_1 - 1;

        CHECK_OCI_PROC(ctx->errhp,
            OCIParamGet(cursor_stmt, OCI_HTYPE_STMT,
                        ctx->errhp, (void **)&param, ci_1),
            ctx, Cleanup);

        text *tmp_name = NULL;
        ub4   tmp_len  = 0;

        CHECK_OCI_PROC(ctx->errhp,
            OCIAttrGet(param, OCI_DTYPE_PARAM,
                       &tmp_name, &tmp_len,
                       OCI_ATTR_NAME, ctx->errhp),
            ctx, Cleanup);

        if (tmp_len > 255) tmp_len = 255;
        memcpy(bc.col_names[ci], tmp_name, tmp_len);
        bc.col_names[ci][tmp_len] = '\0';

        CHECK_OCI_PROC(ctx->errhp,
            OCIAttrGet(param, OCI_DTYPE_PARAM,
                       &bc.data_types[ci], 0,
                       OCI_ATTR_DATA_TYPE, ctx->errhp),
            ctx, Cleanup);

        CHECK_OCI_PROC(ctx->errhp,
            OCIAttrGet(param, OCI_DTYPE_PARAM,
                       &bc.data_sizes[ci], 0,
                       OCI_ATTR_DATA_SIZE, ctx->errhp),
            ctx, Cleanup);

        ub4 buf_size = bc.data_sizes[ci] + 32;
        if (buf_size < 64) buf_size = 64;
        bc.buf_sizes[ci] = buf_size;

        logger_write(ctx->logger, LOG_INFO, __func__, 0,
                     "Cursor col %u name=%s type=%u buf_size=%u",
                     ci_1, bc.col_names[ci],
                     bc.data_types[ci], buf_size);

        if (bc.data_types[ci] == SQLT_BLOB)
        {
            bc.col_blob_locs[ci] = calloc(bc.fetch_count,
                                          sizeof(OCILobLocator *));
            if (!bc.col_blob_locs[ci]) { rc = -1; goto Cleanup; }

            for (ub4 r = 0; r < bc.fetch_count; r++)
            {
                CHECK_OCI_PROC(ctx->errhp,
                    OCIDescriptorAlloc(ctx->envhp,
                                       (void **)&bc.col_blob_locs[ci][r],
                                       OCI_DTYPE_LOB, 0, NULL),
                    ctx, Cleanup);
            }

            bc.indicators[ci] = calloc(bc.fetch_count, sizeof(sb2));
            if (!bc.indicators[ci]) { rc = -1; goto Cleanup; }

            CHECK_OCI_PROC(ctx->errhp,
                OCIDefineByPos(cursor_stmt, &bc.def[ci], ctx->errhp,
                               ci_1,
                               (dvoid *)&bc.col_blob_locs[ci][0],
                               -1, SQLT_BLOB,
                               &bc.indicators[ci][0],
                               NULL, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            CHECK_OCI_PROC(ctx->errhp,
                OCIDefineArrayOfStruct(bc.def[ci], ctx->errhp,
                                       (ub4)sizeof(OCILobLocator *),
                                       (ub4)sizeof(sb2), 0, 0),
                ctx, Cleanup);
        }
        else if (bc.data_types[ci] == SQLT_CLOB)
        {
            bc.indicators[ci] = calloc(bc.fetch_count, sizeof(sb2));
            if (!bc.indicators[ci]) { rc = -1; goto Cleanup; }

            CHECK_OCI_PROC(ctx->errhp,
                OCIDefineByPos(cursor_stmt, &bc.def[ci], ctx->errhp,
                               ci_1,
                               (dvoid *)&bc.clob_loc,
                               -1, SQLT_CLOB,
                               &bc.indicators[ci][0],
                               NULL, NULL, OCI_DEFAULT),
                ctx, Cleanup);
            /* No ArrayOfStruct for CLOB - OCI restriction */
        }
        else
        {
            bc.buffers[ci] = calloc(bc.fetch_count, buf_size);
            if (!bc.buffers[ci]) { rc = -1; goto Cleanup; }

            bc.indicators[ci] = calloc(bc.fetch_count, sizeof(sb2));
            if (!bc.indicators[ci]) { rc = -1; goto Cleanup; }

            CHECK_OCI_PROC(ctx->errhp,
                OCIDefineByPos(cursor_stmt, &bc.def[ci], ctx->errhp,
                               ci_1,
                               bc.buffers[ci],
                               (sb4)buf_size,
                               SQLT_STR,
                               &bc.indicators[ci][0],
                               NULL, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            CHECK_OCI_PROC(ctx->errhp,
                OCIDefineArrayOfStruct(bc.def[ci], ctx->errhp,
                                       buf_size,
                                       (ub4)sizeof(sb2), 0, 0),
                ctx, Cleanup);
        }
    }

    /* ---- CLOB array fetch restriction - same guard as batch module ---- */
    for (ub4 ci = 0; ci < bc.col_count; ci++)
    {
        if (bc.data_types[ci] == SQLT_CLOB)
        {
            logger_write(ctx->logger, LOG_INFO, __func__, 0,
                         "CLOB col=%u in cursor '%s' - "
                         "forcing fetch_count=1",
                         ci, param_name);
            bc.fetch_count = 1;
            break;
        }
    }

    /* ---- Allocate BLOB list ---- */
    int max_lobs  = ctx->ini->query_max_record_count *
                    ctx->ini->max_BLOBS_per_record;
    int max_clobs = ctx->ini->query_max_record_count *
                    ctx->ini->max_CLOBS_per_record;
    int BLOB_index = 0;
    int CLOB_index = 0;

    if (max_lobs > 0)
    {
        BLOB_list = calloc(max_lobs, sizeof(lob_item_t));
        if (!BLOB_list)
        {
            logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                         "calloc failed for BLOB_list");
            rc = -1;
            goto Cleanup;
        }
    }

    /* ---- Emit resultset opening tag with param_name attribute ---- */
    xml_append(xml, "<resultset param_name=\"%s\">\n", param_name);

    /* ---- Batch fetch loop ---- */
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Starting fetch loop cursor='%s' fetch_count=%u",
                 param_name, bc.fetch_count);

    unsigned int abs_rownum  = 0;
    sword        fetch_status;
    int          row_limit   = ctx->ini->query_max_record_count;

    for (;;)
    {
        fetch_status = OCIStmtFetch2(cursor_stmt, ctx->errhp,
                                     bc.fetch_count,
                                     OCI_FETCH_NEXT, 0,
                                     OCI_DEFAULT);

        if (fetch_status != OCI_SUCCESS         &&
            fetch_status != OCI_SUCCESS_WITH_INFO &&
            fetch_status != OCI_NO_DATA)
        {
            logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                         "OCIStmtFetch2 unexpected status=%d cursor='%s'",
                         fetch_status, param_name);
            rc = -1;
            goto Cleanup;
        }

        ub4 rows_fetched = 0;
        OCIAttrGet(cursor_stmt, OCI_HTYPE_STMT,
                   &rows_fetched, 0,
                   OCI_ATTR_ROWS_FETCHED, ctx->errhp);

        if (rows_fetched == 0) break;

        for (ub4 r = 0; r < rows_fetched; r++)
        {
            abs_rownum++;

            if ((int)abs_rownum > row_limit)
            {
                logger_write(ctx->logger, LOG_WARN, __func__, 0,
                             "Cursor '%s' row limit %d reached - "
                             "truncating", param_name, row_limit);
                goto FetchDone;
            }

            /* ---- Build one XML row ---- */
            xml_add_row_start(xml, abs_rownum);

            for (ub4 ci = 0; ci < bc.col_count; ci++)
            {
                const char *type_str = "STRING";
                switch (bc.data_types[ci])
                {
                    case SQLT_NUM:       type_str = "NUMBER";    break;
                    case SQLT_DAT:       type_str = "DATE";      break;
                    case SQLT_TIMESTAMP: type_str = "TIMESTAMP"; break;
                    case SQLT_BLOB:      type_str = "BLOB";      break;
                    case SQLT_CLOB:      type_str = "CLOB";      break;
                    default:             type_str = "STRING";    break;
                }

                if (bc.data_types[ci] == SQLT_BLOB)
                {
                    /* BLOB: NULL/empty guard before any LOB call */
                    if (bc.indicators[ci][r] == -1 || BLOB_index >= max_lobs)
                    {
                        xml_add_field(xml, bc.col_names[ci], "BLOB", "");
                    }
                    else
                    {
                        lob_item_t *item = &BLOB_list[BLOB_index];
                        item->is_null    = 0;

                        CHECK_OCI_PROC(ctx->errhp,
                            OCIDescriptorAlloc(ctx->envhp,
                                               (void **)&item->lob_loc,
                                               OCI_DTYPE_LOB, 0, NULL),
                            ctx, Cleanup);

                        CHECK_OCI_PROC(ctx->errhp,
                            OCILobLocatorAssign(ctx->svchp, ctx->errhp,
                                                bc.col_blob_locs[ci][r],
                                                &item->lob_loc),
                            ctx, Cleanup);

                        CHECK_OCI_PROC(ctx->errhp,
                            OCILobGetLength(ctx->svchp, ctx->errhp,
                                            item->lob_loc, &item->blob_size),
                            ctx, Cleanup);

                        if (item->blob_size == 0)
                        {
                            xml_add_field(xml, bc.col_names[ci], "BLOB", "");
                        }
                        else
                        {
                            item->blob_data = malloc(item->blob_size);
                            if (item->blob_data)
                            {
                                ub4 offset = 1;
                                ub4 remaining = item->blob_size;
                                ub1 *wp = item->blob_data;
                                while (remaining > 0)
                                {
                                    ub4 chunk = (ub4)ctx->ini->chunk_read_size;
                                    if (chunk > remaining) chunk = remaining;
                                    ub4 amount = chunk;
                                    OCILobRead(ctx->svchp, ctx->errhp,
                                               item->lob_loc, &amount, offset,
                                               wp, chunk, NULL, NULL, 0,
                                               SQLCS_IMPLICIT);
                                    wp        += amount;
                                    offset    += amount;
                                    remaining -= amount;
                                }
                                item->column_name = bc.col_names[ci];
                                item->file_name   = strdup(
                                    ctx->ini->BLOB_default_file_name);
                                item->mime_type   = strdup(
                                    ctx->ini->BLOB_default_MIME_TYPE);
                                item->column_index = BLOB_index;
                                item->output_file_destination =
                                    ctx->ini->BLOB_output_dir;
                                item->output_file_url =
                                    ctx->ini->BLOB_URL_path;
                                write_blob_to_file(item,
                                    ctx->ini->BLOB_output_dir, ctx);
                                xml_add_blob_field_1(xml, item, ctx);
                            }
                        }
                        BLOB_index++;
                    }
                }
                else if (bc.data_types[ci] == SQLT_CLOB)
                {
                    /* CLOB: NULL guard before OCILobGetLength */
                    if (bc.indicators[ci][0] == -1)
                    {
                        xml_add_field(xml, bc.col_names[ci], "CLOB", "");
                    }
                    else
                    {
                        ub4 lob_len = 0;
                        OCILobGetLength(ctx->svchp, ctx->errhp,
                                        bc.clob_loc, &lob_len);
                        if (lob_len == 0)
                        {
                            xml_add_field(xml, bc.col_names[ci], "CLOB", "");
                        }
                        else
                        {
                            char *clob_buf = calloc(1, lob_len + 1);
                            if (clob_buf)
                            {
                                ub4 offset = 1;
                                ub4 remaining = lob_len;
                                char *wp = clob_buf;
                                while (remaining > 0)
                                {
                                    ub4 chunk = (ub4)ctx->ini->chunk_read_size;
                                    if (chunk > remaining) chunk = remaining;
                                    ub4 amount = chunk;
                                    OCILobRead(ctx->svchp, ctx->errhp,
                                               bc.clob_loc, &amount, offset,
                                               wp, chunk, NULL, NULL, 0,
                                               SQLCS_IMPLICIT);
                                    wp        += amount;
                                    offset    += amount;
                                    remaining -= amount;
                                }
                                clob_buf[lob_len] = '\0';
                                xml_add_field(xml, bc.col_names[ci],
                                              "CLOB", clob_buf);
                                free(clob_buf);
                            }
                        }
                        CLOB_index++;
                    }
                }
                else
                {
                    /* Scalar */
                    const char *value =
                        bc.buffers[ci] +
                        ((size_t)r * bc.buf_sizes[ci]);

                    if (bc.indicators[ci][r] == -1)
                        value = "";

                    xml_add_field(xml, bc.col_names[ci], type_str, value);
                }
            }

            xml_add_row_end(xml);
        }

        if (fetch_status == OCI_NO_DATA) break;
    }

FetchDone:
    xml_append(xml, "</resultset>\n");

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "fetch_cursor_to_xml complete cursor='%s' rows=%u "
                 "blobs=%d clobs=%d",
                 param_name, abs_rownum, BLOB_index, CLOB_index);

Cleanup:
    /* Free BLOB list */
    if (BLOB_list)
    {
        for (int i = 0; i < BLOB_index; i++)
        {
            if (BLOB_list[i].lob_loc)
                OCIDescriptorFree(BLOB_list[i].lob_loc, OCI_DTYPE_LOB);
            if (BLOB_list[i].blob_data) free(BLOB_list[i].blob_data);
            if (BLOB_list[i].file_name) free(BLOB_list[i].file_name);
        }
        free(BLOB_list);
    }

    free_cur_batch_ctx(ctx, &bc);
    return rc;
}

/* ================================================================== */
/*  execute_procedure - main entry point                               */
/* ================================================================== */
int execute_procedure(oci_context_t    *ctx,
                       const char       *template_xml,
                       execute_config_t *cfg)
{
    int            rc    = 0;
    OCIStmt       *stmt  = NULL;
    xml_builder_t *xml   = NULL;
    proc_ctx_t    *pc    = NULL;
    struct timespec ts_start, ts_end;

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Entering execute_procedure");

    if (!ctx || !template_xml || !cfg)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments");
        return -1;
    }
    metrics_record_t metrics;
    metrics_init(&metrics);
    metrics_set_context(&metrics, ctx);
    metrics.start_time_us = metrics_now_us();
    strncpy(metrics.operation, "PROCEDURE", sizeof(metrics.operation) - 1);

    /* Set transaction_id immediately so every write path carries it  */
    if (ctx->active_tx)
        strncpy(metrics.transaction_id,
                tx_get_id(ctx->active_tx),
                sizeof(metrics.transaction_id) - 1);
    else
        strncpy(metrics.transaction_id, "-",
                sizeof(metrics.transaction_id) - 1);



    /* ================================================================
     *  Stage 1 - Parse XML
     * ================================================================ */
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Stage 1: Parsing procedure XML");

    pc = calloc(1, sizeof(proc_ctx_t));
    if (!pc)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "calloc failed for proc_ctx_t");
        rc = -1;
        goto Cleanup;
    }

    if (parse_procedure_xml(ctx, template_xml, pc) != 0)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "parse_procedure_xml failed");
        rc = -1;
        goto Cleanup;
    }

    strncpy(metrics.object_name, pc->proc_name,
             sizeof(metrics.object_name) - 1);



    /* ================================================================
     *  Stage 2 - Build PL/SQL block and prepare statement
     * ================================================================ */
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Stage 2: Building PL/SQL block");

    char plsql_block[MAX_PLSQL_BLOCK_LEN] = {0};
    if (build_plsql_block(ctx, pc, plsql_block, sizeof(plsql_block)) != 0)
    {
        rc = -1;
        goto Cleanup;
    }

    CHECK_OCI_PROC(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)plsql_block, (ub4)strlen(plsql_block),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT),
        ctx, Cleanup);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "OCIStmtPrepare2 OK");

    /* ================================================================
     *  Stage 3 - Bind all parameters
     * ================================================================ */
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Stage 3: Binding parameters");

    if (bind_parameters(ctx, pc, stmt) != 0)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "bind_parameters failed");
        rc = -1;
        goto Cleanup;
    }

    /* ================================================================
     *  Stage 4 - Execute the PL/SQL block
     *  iters=1 is required for PL/SQL anonymous blocks
     * ================================================================ */
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Stage 4: Executing PL/SQL block");

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    CHECK_OCI_PROC(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                       1, 0, NULL, NULL, OCI_DEFAULT),
        ctx, Cleanup);
    //metrics.execution_us = (uint64_t)(elapsed * 1e6);   /* elapsed already computed , TL:09-Jun for some reason this is undeclated */


    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "PL/SQL execute OK elapsed=%.6f", elapsed);

    /* ================================================================
     *  Stage 5 - Build result XML header and scalar OUT parameters
     * ================================================================ */
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Stage 5: Building result XML");

    xml = xml_create(16384);
    if (!xml) { rc = -1; goto Cleanup; }

    xml_start_document(xml);
    xml_append(xml, "<Procedure_Result>\n");
    xml_start_execution(xml);
    xml_append(xml, "<procedure_name>%s</procedure_name>\n",
               pc->proc_name);
    xml_append(xml, "<execution_time>%.6f</execution_time>\n", elapsed);
    xml_append(xml, "<param_count>%d</param_count>\n", pc->param_count);

    /* ---- Emit scalar OUT / IN_OUT parameter values ---- */
    int has_out_scalars = 0;
    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t *p = &pc->params[i];
        if (!p->is_cursor &&
            (p->direction == PARAM_OUT || p->direction == PARAM_IN_OUT))
            has_out_scalars = 1;
    }

    if (has_out_scalars)
    {
        xml_append(xml, "<out_parameters>\n");
        for (int i = 0; i < pc->param_count; i++)
        {
            proc_param_t *p = &pc->params[i];
            if (p->is_cursor) continue;
            if (p->direction != PARAM_OUT &&
                p->direction != PARAM_IN_OUT) continue;

            /* Determine the returned value string */
            char val_str[64] = {0};
            if (p->indicator == -1)
            {
                /* NULL returned */
                val_str[0] = '\0';
            }
            else if (p->is_integer)
            {
                snprintf(val_str, sizeof(val_str), "%d", p->out_int);
            }
            else
            {
                strncpy(val_str, p->out_value, sizeof(val_str) - 1);
            }

            logger_write(ctx->logger, LOG_INFO, __func__, 0,
                         "OUT param '%s' type='%s' value='%s'",
                         p->param_name, p->param_type, val_str);

            xml_append(xml,
                       "  <parameter>"
                       "<param_name>%s</param_name>"
                       "<param_type>%s</param_type>"
                       "<param_value>%s</param_value>"
                       "</parameter>\n",
                       p->param_name,
                       p->param_type,
                       val_str);
        }
        xml_append(xml, "</out_parameters>\n");
    }

    xml_end_execution(xml);

    /* ================================================================
     *  Stage 6 - Fetch CURSOR OUT result sets
     *  Each CURSOR OUT parameter produces its own <resultset> block.
     * ================================================================ */
    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t *p = &pc->params[i];
        if (!p->is_cursor || p->direction == PARAM_IN) continue;

        if (!p->cursor_stmt)
        {
            logger_write(ctx->logger, LOG_WARN, __func__, 0,
                         "CURSOR param '%s' has NULL stmt handle - "
                         "skipping", p->param_name);
            continue;
        }

        if (p->indicator == -1)
        {
            logger_write(ctx->logger, LOG_INFO, __func__, 0,
                         "CURSOR param '%s' is NULL - "
                         "emitting empty resultset", p->param_name);
            xml_append(xml,
                       "<resultset param_name=\"%s\"/>\n",
                       p->param_name);
            continue;
        }

        logger_write(ctx->logger, LOG_INFO, __func__, 0,
                     "Stage 6: Fetching CURSOR param='%s'",
                     p->param_name);

        int cur_rc = fetch_cursor_to_xml(ctx,
                                          p->cursor_stmt,
                                          p->param_name,
                                          xml);
        if (cur_rc != 0)
        {
            logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                         "fetch_cursor_to_xml failed param='%s'",
                         p->param_name);
            rc = -1;
            goto Cleanup;
        }
    }

    xml_append(xml, "</Procedure_Result>\n");
    xml_finalize(xml);
    metrics.end_time_us = metrics_now_us();
     metrics.status_code = 0;
     strncpy(metrics.error_code, "-", sizeof(metrics.error_code) - 1);
     strncpy(metrics.error_text, "-", sizeof(metrics.error_text) - 1);
     /* transaction_id already set at init time */
 	if (ctx->ini && ctx->ini->metrics_display_input_file_name && cfg->input_file_name)
 	    metrics.input_file_name = strdup(cfg->input_file_name);

 	if (ctx->ini && ctx->ini->metrics_display_input_request && ctx->INPUT_XML)
 	    metrics.input_request = xml_escape_for_csv(ctx->INPUT_XML);

 	if (ctx->ini && ctx->ini->metrics_display_output_response)
 	{
 	    /* PROCEDURE doesn't render a JSON response yet (only the SELECT
 	     * batch path does) - this is a no-op fallback to XML until it
 	     * does, kept consistent with the other execute modules.       */
 	    int is_json = (cfg->ReturnFormat &&
 	                   strcasecmp(cfg->ReturnFormat, "JSON") == 0);

 	    if (is_json && cfg->OUTPUT_JSON)
 	        metrics.output_response = xml_escape_for_csv(cfg->OUTPUT_JSON);
 	    else if (cfg->xml && cfg->xml->OUTPUT_XML)
 	        metrics.output_response = xml_escape_for_csv(cfg->xml->OUTPUT_XML);
 	}
    metrics_finalise(&metrics);
     metrics_write(ctx->metrics_logger, &metrics);



    if (!cfg->xml) cfg->xml = calloc(1, sizeof(*cfg->xml));
    cfg->xml->OUTPUT_XML = strdup(xml->buffer);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "execute_procedure complete proc='%s' elapsed=%.6f",
                 pc->proc_name, elapsed);

Cleanup:
    /* ================================================================
     *  Stage 7 - Cleanup: cursor handles, xml, stmt, pc
     * ================================================================ */
    logger_write(ctx->logger, LOG_INFO, __func__, 0, "Stage 7: Cleanup");
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
	metrics_finalise(&metrics);
	metrics_write(ctx->metrics_logger, &metrics);
	logger_clear_last_error();   // reset for next operation
#
    if (pc)
    {
        for (int i = 0; i < pc->param_count; i++)
        {
            if (pc->params[i].cursor_stmt)
            {
                logger_write(ctx->logger, LOG_DEBUG, __func__, 0,
                             "OCIHandleFree cursor_stmt param=%d", i);
                OCIHandleFree(pc->params[i].cursor_stmt, OCI_HTYPE_STMT);
                pc->params[i].cursor_stmt = NULL;
            }
        }
        free(pc);
        pc = NULL;
    }

    if (xml)    { xml_free(xml);  xml  = NULL; }

    if (stmt)
    {
        logger_write(ctx->logger, LOG_INFO, __func__, 0,
                     "OCIStmtRelease stmt");
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        stmt = NULL;
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Cleanup complete rc=%d", rc);
    return rc;
}
