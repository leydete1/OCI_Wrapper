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
 * 2026-07-31 fix: all logging in this file now goes to
 * ctx->procedure_logger, not ctx->logger (the main/shared logger).
 * Before this, every module-specific log call here targeted the
 * generic logger, meaning procedure_Data_Manager.log was always
 * correctly created and wired up (see initialise_loggers()'s own
 * gotcha comment in Test_XML_Runner.c about worker_ctx needing every
 * logger explicitly copied) but never actually received a single
 * write - everything landed in Data_Manager.log instead. Found via a
 * genuinely empty (0-byte) log file after a real test run. Purely
 * cosmetic (all the logging was always genuinely happening, just in
 * the wrong file) - not a functional bug, unlike the double
 * metrics_write() found the same day.
 *
 * Internal structure
 * ------------------
 *   build_proc_ctx_from_request() - populate proc_ctx_t from
 *                                   execute_procedure_request_t
 *   build_plsql_block()        - build BEGIN proc(:p1,:p2,...); END;
 *   execute_procedure()        - orchestrate all stages; bind/execute
 *                                 and CURSOR OUT fetch both happen
 *                                 through db_driver_t as of the
 *                                 2026-09-21 driver integration
 *                                 (dml_execute_procedure()/
 *                                 select_open_from_cursor() - see that
 *                                 function's own v2 driver integration
 *                                 comment) - no separate bind or LOB-
 *                                 style fetch functions remain in this
 *                                 file; driver_oracle.c owns that OCI
 *                                 surface now, and CURSOR OUT results
 *                                 go through the same resultset_t/
 *                                 response_write_xml() pipeline every
 *                                 other module already uses.
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
#include "OCI_Connection.h"
#include "OCI_Execute_Query_Batch_Module.h"
#include "OCI_Level2_Parser.h"          /* level2_validate_procedure()   */
#include "OCI_Response_Writer.h"        /* response_write_xml() - reused
                                            for each CURSOR OUT's own
                                            resultset fragment           */
#include "XML_Helper.h"
#include "logger.h"
#include "OCI_Transaction_Manager.h"
#include "metrics.h"
#include "metrics_writer.h"   /* metrics_finalise_and_enqueue() - closure item 5, Stage 2 */
#include "OCI_Blob_Utils.h"
#include "db_driver.h"        /* v2 driver integration, 2026-09-21 -
                                  dml_execute_procedure()/
                                  select_open_from_cursor() - core calls
                                  only the vendor-neutral db_driver_get(),
                                  never driver_oracle.h directly, same as
                                  SELECT/DELETE/UPDATE/INSERT's own
                                  integrations.                        */
#include "OCI_Resultset_Builder.h"  /* resultset_t/resultset_free() -
                                        Stage 6's CURSOR OUT fetch now
                                        goes through the same resultset_t
                                        pipeline execute_query_batch()
                                        uses, not direct XML building */

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/*  MAX_PROC_PARAMS moved to OCI_Execute_Procedure_Module.h 2026-07-30 -
 *  level2_validate_procedure() needs to check the same bound as this
 *  file's own defense-in-depth check, so both now share one constant
 *  rather than two independent copies.                                 */
#define MAX_PARAM_VALUE_SIZE 32768   /* max scalar bind buffer         */
#define MAX_PROC_NAME_LEN    256     /* procedure name incl. owner     */
#define MAX_PLSQL_BLOCK_LEN  8192   /* generated PL/SQL block size    */
#define MAX_CURSOR_COLS      512    /* columns per REFCURSOR result   */

/* ------------------------------------------------------------------ */
/*  param_direction_t is now public - see OCI_Execute_Procedure_        */
/*  Module.h. Moved there 2026-07-29 so the internal proc_param_t       */
/*  below and the public procedure_param_t share one enum rather than   */
/*  two parallel ones for the same three values.                        */
/* ------------------------------------------------------------------ */

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


/* ================================================================== */
/*  Static helpers                                                      */
/* ================================================================== */
/* trim_proc()/extract_tag_proc() removed 2026-07-29 - only ever used
 * by parse_procedure_xml(), which is gone too (see
 * build_proc_ctx_from_request() below). parse_direction() (string ->
 * param_direction_t) moved to OCI_Level1_Parser.c - Level 1 now parses
 * <param_direction> directly into the enum during parsing, so
 * execute_procedure_request_t.parameters[].direction already arrives
 * as param_direction_t, not a string needing conversion here.
 *
 * cur_batch_ctx_t/free_cur_batch_ctx() removed here (Phase 2,
 * 2026-09-21) - no remaining caller once fetch_cursor_to_xml() (below,
 * also removed) was replaced by driver->select_open_from_cursor() +
 * the same select_fetch_batch()/select_close() pair execute_query_batch()
 * already proves, rather than this module's own second, parallel,
 * older-style (direct-XML) fetch implementation. */
static void uppercase_proc(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* response_write_xml() always opens with a literal "<resultset>\n" -
 * confirmed directly against xml_start_resultset()'s own real code,
 * not assumed. Splices in the param_name attribute the original
 * fetch_cursor_to_xml()'s own output always had, which
 * response_write_procedure_xml() depends on (see its own doc comment -
 * "self-contained <resultset param_name=\"...\">...</resultset>
 * fragment"). response_write_xml() itself has no concept of a
 * param_name (it's also used for a plain SELECT's own resultset, which
 * has no such thing), so this module adds it here via simple, known-
 * prefix string surgery rather than needing a new response_write_xml()
 * variant for this one caller. Takes ownership of xml_fragment (frees
 * it), returns a new heap string - matches the same "consumes input,
 * returns replacement" convention as xml_escape_for_csv() elsewhere in
 * this project. */
static char *inject_param_name_attribute(char *xml_fragment, const char *param_name)
{
    static const char *prefix = "<resultset>";
    size_t prefix_len = strlen(prefix);

    if (!xml_fragment || strncmp(xml_fragment, prefix, prefix_len) != 0)
        return xml_fragment;   /* unexpected shape - return unchanged
                                   rather than corrupt it further */

    size_t new_len = strlen(xml_fragment) + strlen(param_name) + 32;
    char *result = malloc(new_len);
    if (!result) { free(xml_fragment); return NULL; }

    snprintf(result, new_len, "<resultset param_name=\"%s\">%s",
             param_name, xml_fragment + prefix_len);

    free(xml_fragment);
    return result;
}

static int build_proc_ctx_from_request(oci_context_t                     *ctx,
                                        const execute_procedure_request_t *req,
                                        proc_ctx_t                        *pc)
{
    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Entering build_proc_ctx_from_request");

    memset(pc, 0, sizeof(*pc));

    if (!req->procedure_name[0])
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                     "Empty procedure_name");
        return -1;
    }
    strncpy(pc->proc_name, req->procedure_name, sizeof(pc->proc_name) - 1);
    strncpy(pc->owner,     req->owner,          sizeof(pc->owner)     - 1);

    /* If owner supplied and proc_name doesn't already contain a dot,
     * prepend owner so the PL/SQL block uses owner.proc_name - same
     * behaviour as the old parser.                                     */
    if (strlen(pc->owner) > 0 && strchr(pc->proc_name, '.') == NULL)
    {
        char fq[MAX_PROC_NAME_LEN];
        snprintf(fq, sizeof(fq), "%s.%s", pc->owner, pc->proc_name);
        strncpy(pc->proc_name, fq, sizeof(pc->proc_name) - 1);
    }

    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "procedure_name='%s'", pc->proc_name);

    if (req->param_count <= 0)
    {
        logger_write(ctx->procedure_logger, LOG_WARN, __func__, 0,
                     "No parameters supplied - "
                     "procedure will be called with no parameters");
    }
    if (req->param_count > MAX_PROC_PARAMS)
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                     "param_count=%d exceeds MAX_PROC_PARAMS=%d - "
                     "level2_validate_procedure() should have caught this",
                     req->param_count, MAX_PROC_PARAMS);
        return -1;
    }

    pc->param_count = req->param_count;

    for (int i = 0; i < req->param_count; i++)
    {
        const procedure_param_t *rp = &req->parameters[i];
        proc_param_t            *p  = &pc->params[i];

        memset(p, 0, sizeof(*p));
        p->indicator = 0;

        strncpy(p->param_name,  rp->param_name,  sizeof(p->param_name)  - 1);
        strncpy(p->param_type,  rp->param_type,  sizeof(p->param_type)  - 1);
        strncpy(p->param_value, rp->param_value, sizeof(p->param_value) - 1);
        uppercase_proc(p->param_type);

        p->direction  = rp->direction;
        p->is_cursor  = (strcmp(p->param_type, "CURSOR")  == 0);
        p->is_integer = (strcmp(p->param_type, "INTEGER") == 0 ||
                         strcmp(p->param_type, "NUMBER")  == 0);

        logger_write(ctx->procedure_logger, LOG_DEBUG, __func__, 0,
                     "Param %d: name='%s' type='%s' dir=%d "
                     "value='%s' is_cursor=%d",
                     i + 1, p->param_name, p->param_type,
                     (int)p->direction, p->param_value, p->is_cursor);
    }

    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "build_proc_ctx_from_request OK: proc='%s' params=%d",
                 pc->proc_name, pc->param_count);
    return 0;
}

/* ================================================================== */
/*  build_plsql_block                                                   */
/*  Generates:  BEGIN proc_name(:P1, :P2, :P3); END;                  */
/* ================================================================== */
/*  build_plsql_block                                                   */
/*  Generates:  BEGIN proc_name(:P1, :P2, :P3); END;                  */
/* ================================================================== */
static int build_plsql_block(oci_context_t  *ctx,
                               const proc_ctx_t *pc,
                               char             *block_buf,
                               size_t            block_max)
{
    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Building PL/SQL block for '%s' params=%d",
                 pc->proc_name, pc->param_count);

    if (pc->param_count == 0)
    {
        /* No parameters - simple call */
        int n = snprintf(block_buf, block_max,
                         "BEGIN %s; END;", pc->proc_name);
        if (n < 0 || (size_t)n >= block_max)
        {
            logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
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
            logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                         "PL/SQL block truncated - too many parameters");
            return -1;
        }
    }

    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "PL/SQL block: %s", block_buf);
    return 0;
}

/* ================================================================== */
/*  bind_parameters()/fetch_cursor_to_xml() removed here (Phase 2,     */
/*  2026-09-21) - no remaining caller anywhere in this file once the   */
/*  driver integration landed.                                         */
/*                                                                      */
/*  execute_procedure() now binds and executes through                */
/*  driver->dml_execute_procedure() (see the v2 driver integration     */
/*  comment inside execute_procedure() itself) - same OCIBindByName    */
/*  SQLT_RSET/SQLT_INT/SQLT_STR dispatch bind_parameters() used,       */
/*  confirmed byte-for-byte against driver_oracle.c's own              */
/*  oracle_dml_execute_procedure() before this integration ever        */
/*  started - and fetches every CURSOR OUT parameter through           */
/*  driver->select_open_from_cursor() plus the same select_fetch_batch()/
 *  select_close() pair execute_query_batch() already proves, replacing */
/*  this module's own second, parallel, older-style fetch               */
/*  implementation (fetch_cursor_to_xml()/cur_batch_ctx_t built its     */
/*  own XML directly, never went through the resultset_t/               */
/*  response_write_xml() pipeline everything else in this project uses).*/
/* ================================================================== */

int execute_procedure(oci_context_t                *ctx,
                       execute_procedure_request_t  *req,
                       execute_config_t             *cfg)
{
    int            rc    = 0;
    xml_builder_t *xml   = NULL;
    proc_ctx_t    *pc    = NULL;
    struct timespec ts_start, ts_end;

    /* Declared here, not down in Stage 5, and memset immediately -
     * Cleanup below frees resp's own heap-allocated fields
     * (out_parameters, resultsets[]), and several earlier stages
     * (2/3/4) can goto Cleanup before Stage 5 would otherwise have
     * declared/initialised this - a struct declared mid-function has
     * indeterminate contents until its own declaration point runs, so
     * freeing garbage pointers from an uninitialised resp would be a
     * real, if intermittent, crash risk. Same reasoning as every other
     * execute module's own top-of-function NULL-initialised locals.    */
    execute_procedure_response_t resp;
    memset(&resp, 0, sizeof(resp));

    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Entering execute_procedure");

    if (!ctx || !req || !cfg)
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
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
    /* Same source as transaction_id above, just the name - closure
     * item 5 follow-up (2026-08-10).                                  */
    strncpy(metrics.transaction_name,
            ctx->active_tx ? ctx->active_tx->tx_name : "-",
            sizeof(metrics.transaction_name) - 1);



    /* ================================================================
     *  Stage 1 - Validate
     *  Called internally rather than trusted to have already run in
     *  the caller - see this file's own top-of-file doc comment.
     *  Deliberately light - see level2_validate_procedure()'s own doc
     *  comment in OCI_Level2_Parser.h for why.
     * ================================================================ */
    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Validating request");

    input_c_operation_t validate_op;
    memset(&validate_op, 0, sizeof(validate_op));
    validate_op.type    = OP_EXECUTE_PROCEDURE;
    validate_op.payload = (void *)req;

    operation_status_t val_status;
    memset(&val_status, 0, sizeof(val_status));

    if (level2_validate_procedure(ctx, &validate_op, &val_status) != LEVEL2_OK)
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                     "Stage 1 validation failed: %s", val_status.error_text);
        return -1;
    }
    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Stage 1 validation passed");

    /* ================================================================
     *  Stage 2 - Build procedure context, PL/SQL block
     * ================================================================ */
    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Stage 2: Building PL/SQL block");

    pc = calloc(1, sizeof(proc_ctx_t));
    if (!pc)
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for proc_ctx_t");
        rc = -1;
        goto Cleanup;
    }

    if (build_proc_ctx_from_request(ctx, req, pc) != 0)
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                     "build_proc_ctx_from_request failed");
        rc = -1;
        goto Cleanup;
    }

    strncpy(metrics.object_name, pc->proc_name,
             sizeof(metrics.object_name) - 1);

    char plsql_block[MAX_PLSQL_BLOCK_LEN] = {0};
    if (build_plsql_block(ctx, pc, plsql_block, sizeof(plsql_block)) != 0)
    {
        rc = -1;
        goto Cleanup;
    }

    /* v2 driver integration (2026-09-21). The old, separate
     * OCIStmtPrepare2 call that used to sit here is gone -
     * dml_execute_procedure() does prepare/bind/execute/collect as one
     * atomic call (see db_driver.h's own doc comment).
     *
     * Core only ever calls the vendor-neutral db_driver_get() - never
     * reaches for driver_oracle.h directly, same as SELECT/DELETE/
     * UPDATE/INSERT's own integrations. */
    const db_driver_t *driver = db_driver_get(ctx);
    if (!driver || !driver->dml_execute_procedure ||
        !driver->select_open_from_cursor || !driver->select_fetch_batch ||
        !driver->select_close)
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                     "db_driver_get() returned an incomplete driver");
        rc = -1;
        goto Cleanup;
    }

    /* ================================================================
     *  Stage 3 - Build driver-level params, bind and execute
     * ================================================================ */
    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Stage 3: Building bind params and executing");

    db_proc_param_t *driver_params = NULL;
    if (pc->param_count > 0)
    {
        driver_params = calloc((size_t)pc->param_count, sizeof(db_proc_param_t));
        if (!driver_params) { rc = -1; goto Cleanup; }
    }

    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t     *p  = &pc->params[i];
        db_proc_param_t  *dp = &driver_params[i];

        dp->name      = p->param_name;
        /* param_direction_t (core) and db_proc_param_direction_t
         * (driver) share identical underlying values (IN=0/OUT=1/
         * IN_OUT=2) - confirmed directly against both enum
         * definitions, not assumed - so a plain cast is correct. */
        dp->direction = (db_proc_param_direction_t)p->direction;

        if (p->is_cursor)
            dp->type = DB_PROC_TYPE_CURSOR;
        else if (p->is_integer)
            dp->type = DB_PROC_TYPE_INT;
        else
            dp->type = DB_PROC_TYPE_STR;

        /* Matches bind_parameters()'s own unconditional copy - a pure
         * OUT param's own param_value is simply empty from a well-
         * formed request, not specially suppressed here. */
        dp->in_value = (strlen(p->param_value) > 0) ? p->param_value : NULL;

        if (dp->type == DB_PROC_TYPE_STR)
        {
            /* Reuses proc_param_t's own existing out_value buffer -
             * same buffer bind_parameters() used to bind directly,
             * now just handed to the driver instead. */
            dp->out_value      = p->out_value;
            dp->out_value_size = sizeof(p->out_value);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    db_proc_execute_request_t dexec_req;
    memset(&dexec_req, 0, sizeof(dexec_req));
    dexec_req.plsql_block = plsql_block;
    dexec_req.param_count = pc->param_count;
    dexec_req.params      = driver_params;

    if (driver->dml_execute_procedure(ctx, ctx->procedure_logger, &dexec_req) != 0)
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                     "driver dml_execute_procedure failed for proc='%s'",
                     pc->proc_name);
        free(driver_params);
        rc = -1;
        goto Cleanup;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "PL/SQL execute OK elapsed=%.6f", elapsed);

    /* Copy driver-populated OUT values back into pc->params[] - core's
     * own struct, which Stage 5/6/Cleanup below already know how to
     * read, unchanged from before this pass. out_int/out_value were
     * already written directly by the driver (out_value via the same
     * buffer aliased above); out_is_null and out_cursor_handle need an
     * explicit copy back here. */
    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t     *p  = &pc->params[i];
        db_proc_param_t  *dp = &driver_params[i];

        p->indicator = dp->out_is_null ? -1 : 0;
        if (dp->type == DB_PROC_TYPE_INT) p->out_int = dp->out_int;
        if (p->is_cursor) p->cursor_stmt = (OCIStmt *)dp->out_cursor_handle;
    }

    free(driver_params);

    /* ================================================================
     *  Stage 5 - Collect scalar OUT/IN_OUT parameter values
     * ================================================================ */
    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Stage 5: Collecting scalar OUT/IN_OUT parameters");

    strncpy(resp.procedure_name, pc->proc_name, sizeof(resp.procedure_name) - 1);

    int out_scalar_count = 0;
    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t *p = &pc->params[i];
        if (!p->is_cursor &&
            (p->direction == PARAM_DIR_OUT || p->direction == PARAM_DIR_IN_OUT))
            out_scalar_count++;
    }

    if (out_scalar_count > 0)
    {
        resp.out_parameters = calloc((size_t)out_scalar_count, sizeof(procedure_param_t));
        if (!resp.out_parameters) { rc = -1; goto Cleanup; }
    }

    int out_idx = 0;
    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t *p = &pc->params[i];
        if (p->is_cursor) continue;
        if (p->direction != PARAM_DIR_OUT &&
            p->direction != PARAM_DIR_IN_OUT) continue;

        procedure_param_t *op = &resp.out_parameters[out_idx++];
        strncpy(op->param_name, p->param_name, sizeof(op->param_name) - 1);
        strncpy(op->param_type, p->param_type, sizeof(op->param_type) - 1);
        op->direction = p->direction;

        /* Determine the returned value string */
        if (p->indicator == -1)
        {
            /* NULL returned - param_value stays empty */
        }
        else if (p->is_integer)
        {
            snprintf(op->param_value, sizeof(op->param_value), "%d", p->out_int);
        }
        else
        {
            strncpy(op->param_value, p->out_value, sizeof(op->param_value) - 1);
        }

        logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                     "OUT param '%s' type='%s' value='%s'",
                     op->param_name, op->param_type, op->param_value);
    }
    resp.out_param_count = out_idx;

    /* ================================================================
     *  Stage 6 - Fetch CURSOR OUT result sets
     *  v2 driver integration: each CURSOR OUT parameter's own handle
     *  (p->cursor_stmt, populated by the copy-back loop above from the
     *  driver's own out_cursor_handle) is opened via
     *  driver->select_open_from_cursor() and fetched through the SAME
     *  select_fetch_batch()/select_close() pair execute_query_batch()
     *  already proves - replacing this module's own former, older-
     *  style, direct-XML fetch_cursor_to_xml()/cur_batch_ctx_t
     *  implementation. A cursor's total row count isn't known upfront
     *  the way a COUNT(*)-backed SELECT's is, so batches are collected
     *  first, then merged into one resultset_t before calling
     *  response_write_xml() once - matching that function's own "one
     *  call, one complete fragment" contract, then
     *  inject_param_name_attribute() (above) adds back the param_name
     *  attribute response_write_procedure_xml() depends on, which
     *  response_write_xml() itself has no concept of.
     * ================================================================ */
    int cursor_out_count = 0;
    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t *p = &pc->params[i];
        if (p->is_cursor && p->direction != PARAM_DIR_IN) cursor_out_count++;
    }

    if (cursor_out_count > 0)
    {
        resp.resultsets = calloc((size_t)cursor_out_count, sizeof(procedure_resultset_t));
        if (!resp.resultsets) { rc = -1; goto Cleanup; }
    }

    int resultset_idx = 0;
    for (int i = 0; i < pc->param_count; i++)
    {
        proc_param_t *p = &pc->params[i];
        if (!p->is_cursor || p->direction == PARAM_DIR_IN) continue;

        procedure_resultset_t *rs = &resp.resultsets[resultset_idx++];
        strncpy(rs->param_name, p->param_name, sizeof(rs->param_name) - 1);

        if (!p->cursor_stmt)
        {
            logger_write(ctx->procedure_logger, LOG_WARN, __func__, 0,
                         "CURSOR param '%s' has NULL stmt handle - "
                         "skipping", p->param_name);
            continue;
        }

        if (p->indicator == -1)
        {
            logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                         "CURSOR param '%s' is NULL - "
                         "emitting empty resultset", p->param_name);
            char empty_frag[128];
            snprintf(empty_frag, sizeof(empty_frag),
                     "<resultset param_name=\"%s\"/>\n", p->param_name);
            rs->resultset_xml_fragment = strdup(empty_frag);
            /* Bound successfully but never opened by the procedure
             * (see UNIT_TEST_CURSOR_PROC) - the driver already freed
             * this handle itself (see oracle_dml_execute_procedure()'s
             * own comment on this exact case), so this is just
             * defensive - matches the driver's own out_cursor_handle=
             * NULL contract for that case, not expected to fire. */
            p->cursor_stmt = NULL;
            continue;
        }

        logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                     "Stage 6: Fetching CURSOR param='%s'",
                     p->param_name);

        db_select_cursor_t *cur         = NULL;
        db_column_meta_t   *columns     = NULL;
        int                  column_count = 0;
        int                  batch_size   = 0;

        if (driver->select_open_from_cursor(ctx, p->cursor_stmt, &cur,
                                             &columns, &column_count,
                                             &batch_size) != 0)
        {
            logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                         "select_open_from_cursor failed param='%s'",
                         p->param_name);
            rc = -1;
            goto Cleanup;
        }
        /* Ownership transferred to select_close() below - Cleanup must
         * not also try to free this handle. */
        p->cursor_stmt = NULL;

        resultset_t **batches       = NULL;
        int          *batch_counts  = NULL;
        int           num_batches   = 0;
        int           batch_capacity= 0;
        int           total_rows    = 0;
        int           fetch_failed  = 0;

        while (1)
        {
            resultset_t *rs_batch    = NULL;
            int          rows_fetched = 0;
            db_fetch_batch_stats_t stats;
            memset(&stats, 0, sizeof(stats));

            if (driver->select_fetch_batch(cur, &rs_batch, &rows_fetched,
                                            &stats) != 0)
            {
                logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                             "select_fetch_batch failed param='%s'",
                             p->param_name);
                fetch_failed = 1;
                break;
            }

            if (rows_fetched == 0)
            {
                if (rs_batch) resultset_free(rs_batch);
                break;
            }

            if (num_batches >= batch_capacity)
            {
                batch_capacity = batch_capacity == 0 ? 8 : batch_capacity * 2;
                batches      = realloc(batches,
                                        (size_t)batch_capacity * sizeof(resultset_t *));
                batch_counts = realloc(batch_counts,
                                        (size_t)batch_capacity * sizeof(int));
            }
            batches[num_batches]      = rs_batch;
            batch_counts[num_batches] = rows_fetched;
            num_batches++;
            total_rows += rows_fetched;

            if (rows_fetched < batch_size) break;
        }

        driver->select_close(cur);

        if (fetch_failed)
        {
            for (int b = 0; b < num_batches; b++) resultset_free(batches[b]);
            free(batches); free(batch_counts); free(columns);
            rc = -1;
            goto Cleanup;
        }

        resultset_t *combined = resultset_create(total_rows, column_count);
        if (!combined)
        {
            for (int b = 0; b < num_batches; b++) resultset_free(batches[b]);
            free(batches); free(batch_counts); free(columns);
            rc = -1;
            goto Cleanup;
        }

        int dest_row = 1;
        for (int b = 0; b < num_batches; b++)
        {
            for (int r = 1; r <= batch_counts[b]; r++)
            {
                resultset_row_t *src_row = resultset_get_row(batches[b], r);
                resultset_row_t *dst_row = resultset_get_row(combined, dest_row);
                for (int c = 0; c < column_count; c++)
                {
                    resultset_field_t *sf = &src_row->fields[c];
                    if (sf->is_blob)
                        resultset_set_blob_field(dst_row, c, sf->field_name,
                                                  sf->blob_detail.file_name,
                                                  sf->blob_detail.file_path,
                                                  sf->blob_detail.file_url,
                                                  sf->blob_detail.file_size,
                                                  sf->blob_detail.mime_type);
                    else
                        resultset_set_field(dst_row, c, sf->field_name,
                                             sf->field_type, sf->value);
                }
                dest_row++;
            }
            resultset_free(batches[b]);
        }
        free(batches);
        free(batch_counts);

        char *combined_frag = response_write_xml(ctx, combined);
        resultset_free(combined);
        free(columns);

        if (!combined_frag)
        {
            logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                         "response_write_xml failed param='%s'", p->param_name);
            rc = -1;
            goto Cleanup;
        }

        rs->resultset_xml_fragment =
            inject_param_name_attribute(combined_frag, p->param_name);
        if (!rs->resultset_xml_fragment)
        {
            rc = -1;
            goto Cleanup;
        }
    }
    resp.resultset_count = resultset_idx;

    /* ================================================================
     *  Stage 6b - Build the response
     *  elapsed already computed right after Stage 4's execute above -
     *  reused here rather than recomputed, so execution_time reports
     *  actual PL/SQL execution time, not response-building time too.   */
    resp.execution_time_seconds = elapsed;

    char *proc_xml_fragment = response_write_procedure_xml(ctx, &resp);
    if (!proc_xml_fragment)
    {
        logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                     "response_write_procedure_xml returned NULL");
        rc = -1;
        goto Cleanup;
    }

    xml = xml_create(16384);
    if (!xml) { free(proc_xml_fragment); rc = -1; goto Cleanup; }

    xml_start_document(xml);
    xml_append(xml, "<Procedure_Result>\n");
    xml_start_execution(xml);
    /* xml_append_raw() - never xml_append(xml,"%s",...) - same
     * reasoning as every other raw-fragment splice in this project.    */
    xml_append_raw(xml, proc_xml_fragment);
    xml_end_execution(xml);
    xml_append(xml, "</Procedure_Result>\n");
    xml_finalize(xml);
    free(proc_xml_fragment);

    /* cfg->OUTPUT_JSON's own doc comment in OCI_Connection.h: "set
     * only when ReturnFormat is JSON. NULL otherwise."                  */
    if (cfg->ReturnFormat && strcasecmp(cfg->ReturnFormat, "JSON") == 0)
    {
        cfg->OUTPUT_JSON = response_write_procedure_json(ctx, &resp);
        if (!cfg->OUTPUT_JSON)
            logger_write(ctx->procedure_logger, LOG_ERROR, __func__, 0,
                         "response_write_procedure_json returned NULL - "
                         "OUTPUT_JSON will be missing for this JSON-format request");
    }

    /* input_file_name/input_request/output_response are only
     * meaningful once a response actually exists, so they're still
     * computed here (success path only) - but the actual
     * metrics_finalise()/metrics_write() pair now happens exactly
     * once, in Cleanup below, for both success and failure - found
     * 2026-07-31 via a real duplicate metrics row for every successful
     * procedure call (this file had two separate metrics_write() calls
     * since before this refactor touched it; every other execute
     * module already writes metrics exactly once, in Cleanup).        */
    if (ctx->ini && ctx->ini->metrics_display_input_file_name && cfg->input_file_name)
        metrics.input_file_name = strdup(cfg->input_file_name);

    if (ctx->ini && ctx->ini->metrics_display_input_request && ctx->INPUT_XML)
        metrics.input_request = xml_escape_for_csv(ctx->INPUT_XML);

    if (ctx->ini && ctx->ini->metrics_display_output_response)
    {
        /* PROCEDURE now renders a real JSON response too (Stage 6b
         * above, via response_write_procedure_json()) when
         * cfg->ReturnFormat is JSON - cfg->OUTPUT_JSON is genuinely
         * populated in that case, not a placeholder. This check's own
         * logic didn't need to change - same as INSERT/UPDATE/
         * DELETE's identical fix.                                     */
        int is_json = (cfg->ReturnFormat &&
                       strcasecmp(cfg->ReturnFormat, "JSON") == 0);

        if (is_json && cfg->OUTPUT_JSON)
            metrics.output_response = xml_escape_for_csv(cfg->OUTPUT_JSON);
        else if (cfg->xml && cfg->xml->OUTPUT_XML)
            metrics.output_response = xml_escape_for_csv(cfg->xml->OUTPUT_XML);
    }

    if (!cfg->xml) cfg->xml = calloc(1, sizeof(*cfg->xml));
    cfg->xml->OUTPUT_XML = strdup(xml->buffer);

    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "execute_procedure complete proc='%s' elapsed=%.6f",
                 pc->proc_name, elapsed);

Cleanup:
    /* ================================================================
     *  Stage 7 - Cleanup: cursor handles, xml, stmt, pc
     * ================================================================ */
    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0, "Stage 7: Cleanup");
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
	else
	{
	    strncpy(metrics.error_code, "-", sizeof(metrics.error_code) - 1);
	    strncpy(metrics.error_text, "-", sizeof(metrics.error_text) - 1);
	}
	if(ctx->active_tx)
		strncpy(metrics.transaction_id , tx_get_id(ctx->active_tx),sizeof(metrics.transaction_id)-1);
	else
		strncpy(metrics.transaction_id , "-",sizeof(metrics.transaction_id)-1);
	strncpy(metrics.transaction_name , ctx->active_tx ? ctx->active_tx->tx_name : "-", sizeof(metrics.transaction_name)-1);
	metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &metrics);
	logger_clear_last_error();   // reset for next operation
#
    if (pc)
    {
        for (int i = 0; i < pc->param_count; i++)
        {
            if (pc->params[i].cursor_stmt)
            {
                logger_write(ctx->procedure_logger, LOG_DEBUG, __func__, 0,
                             "OCIHandleFree cursor_stmt param=%d", i);
                OCIHandleFree(pc->params[i].cursor_stmt, OCI_HTYPE_STMT);
                pc->params[i].cursor_stmt = NULL;
            }
        }
        free(pc);
        pc = NULL;
    }

    /* resp itself is stack-allocated, but its own pointer fields are
     * heap-allocated (Stage 5/6b above) and need freeing here - same
     * "free every heap allocation this function made, reverse order"
     * discipline as every other execute module's own Cleanup.          */
    if (resp.out_parameters) { free(resp.out_parameters); resp.out_parameters = NULL; }
    if (resp.resultsets)
    {
        for (int i = 0; i < resp.resultset_count; i++)
            free(resp.resultsets[i].resultset_xml_fragment);
        free(resp.resultsets);
        resp.resultsets = NULL;
    }

    if (xml)    { xml_free(xml);  xml  = NULL; }

    logger_write(ctx->procedure_logger, LOG_INFO, __func__, 0,
                 "Cleanup complete rc=%d", rc);
    return rc;
}
