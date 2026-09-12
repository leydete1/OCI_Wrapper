/*
 * driver_oracle.c
 *
 * connect()/disconnect() are thin delegating wrappers around the
 * existing OCI_Connect()/OCI_Disconnect() (OCI_Connection.c) - they are
 * NOT reimplemented here, and OCI_Connect()/OCI_Disconnect() themselves
 * are NOT touched or moved out of OCI_Connection.c in this pass.
 *
 * Why delegate instead of relocate:
 *   OCI_Connect()/OCI_Disconnect() have three other direct callers today
 *   (Data_Manager_Bootstrap.c, Level2_Insert_Test.c, OCI_Unit_Test_Module.c
 *   - the last one specifically exercises a standalone, non-pooled
 *   connect/disconnect cycle as a stale-handle test victim). None of
 *   those go through db_driver_t yet. Relocating the function bodies
 *   into this file now would mean either duplicating them or making
 *   three unrelated call sites depend on this new header before the
 *   interface shape is proven - exactly the risk the notes doc (section
 *   2) warns against ("get it wrong and the line has to be redrawn
 *   mid-migration"). Delegating first gives db_driver_t real behavior
 *   to test against those existing fixtures (order-of-attack step 4)
 *   with zero risk to the other three callers, which are completely
 *   unaffected by this file's existence.
 *
 *   Physically moving the OCI_Connect()/OCI_Disconnect() bodies into
 *   this file - and updating the three other callers to go through
 *   db_driver_t instead - is a follow-up, purely mechanical pass once
 *   this delegation has been validated against Level2_Insert_Test.c's
 *   fixture (see Driver_Connect_Test.c).
 *
 * POOLED vs DIRECT dispatch (added after connect/disconnect were
 * validated - PASS confirmed live against freepdb1).
 * connect()/disconnect() below now do the ctx->ini->use_connection_pool
 * if/else that Data_Manager_Bootstrap.c used to do inline at every call
 * site (OCI_Connect_pool()/OCI_Disconnect_pool() vs OCI_Connect()/
 * OCI_Disconnect()). Bootstrap itself is NOT updated to call through
 * db_driver_t in this pass - same reasoning as above: prove it against
 * a fixture first (see Driver_Pool_Test.c), then switch real callers
 * over as a separate, low-risk mechanical step once proven, without
 * ever having two competing sources of truth for the branch itself
 * live at once.
 *
 * get_session()/release_session()/session_is_alive()/reconnect_session()/
 * health_check() are straight delegations to the existing
 * OCI_Pool_* functions (OCI_Connection_Pool.c) - no branching, no new
 * logic. They are only valid to call after a pooled connect() - same
 * precondition OCI_Pool_get_session() etc. already have today, not a
 * new restriction introduced here.
 *
 * execute_select was left NULL in the previous pass. Now implemented
 * below as select_open()/select_fetch_batch()/select_close(), scoped to
 * scalar columns only - see the SELECT IMPLEMENTATION block below for
 * the reasoning and exactly what was reused vs written new.
 */

#include <stdlib.h>
#include <string.h>

#include "driver_oracle.h"
#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "OCI_Table_Metadata_Module.h"   /* get_multi_metadata() - the
                                            same OCIParamGet/OCIDefineByPos/
                                            OCIDefineArrayOfStruct work
                                            execute_query_batch already
                                            uses, reused rather than
                                            reimplemented (see below)    */
#include "OCI_Resultset_Builder.h"       /* resultset_create/get_row/
                                            set_field/free              */

static int oracle_connect(oci_context_t *ctx)
{
    if (ctx->ini->use_connection_pool)
        return OCI_Connect_pool(ctx);

    return OCI_Connect(ctx);
}

static void oracle_disconnect(oci_context_t *ctx)
{
    if (ctx->ini->use_connection_pool)
    {
        OCI_Disconnect_pool(ctx);
        return;
    }

    OCI_Disconnect(ctx);
}

static int oracle_get_session(oci_context_t *ctx, oci_context_t *worker_ctx)
{
    return OCI_Pool_get_session(ctx, worker_ctx);
}

static void oracle_release_session(oci_context_t *ctx, oci_context_t *worker_ctx)
{
    OCI_Pool_release_session(ctx, worker_ctx);
}

static int oracle_session_is_alive(oci_context_t *ctx)
{
    return OCI_Pool_session_is_alive(ctx);
}

static int oracle_reconnect_session(oci_context_t *base_ctx, oci_context_t *ctx)
{
    return OCI_Pool_reconnect_session(base_ctx, ctx);
}

static int oracle_health_check(oci_context_t *ctx)
{
    return OCI_Pool_health_check(ctx);
}

/* ================================================================== */
/*  SELECT IMPLEMENTATION - select_open / select_fetch_batch /         */
/*  select_close (db_driver.h). Scalars only in this pass.             */
/*                                                                      */
/*  What's reused vs new:                                              */
/*    - get_multi_metadata() (OCI_Table_Metadata_Module.c) does the     */
/*      real OCIParamGet/OCIDefineByPos/OCIDefineArrayOfStruct work,    */
/*      identical to what execute_query_batch() already calls today -  */
/*      not reimplemented here. It still requires col_blob_locs/        */
/*      clob_loc slots to be allocated even for a scalar-only cursor    */
/*      (multi_meta_request_t's own contract - it has no way to know   */
/*      in advance that none of them will be BLOB/CLOB), so those are  */
/*      allocated below and simply never populated for a pure-scalar    */
/*      result set.                                                     */
/*    - The prepare/describe/execute/fetch OCI calls themselves         */
/*      (OCIStmtPrepare2, OCIStmtExecute x2 - describe-only then a      */
/*      real execute, OCIStmtFetch2) are the same calls                 */
/*      execute_query_batch() makes, in the same order (its own Stage 2 */
/*      describe followed by a separate Stage 3 real execute) - written */
/*      fresh here since execute_query_batch() doesn't expose them as   */
/*      anything callable on their own.                                 */
/*    - Column type -> string mapping (oracle_type_to_string) mirrors   */
/*      build_row_xml_batch()'s own switch exactly, so field_type       */
/*      values match resultset_field_t's existing vocabulary            */
/*      (NUMBER/DATE/STRING/TIMESTAMP/BLOB/CLOB/UNKNOWN) - core code     */
/*      matching against db_column_meta_t.field_type sees the same      */
/*      strings it already does today.                                  */
/*                                                                      */
/*  CLOB/BLOB rejection: get_multi_metadata() has no way to bail out    */
/*  before defining a CLOB/BLOB column - it discovers each column's     */
/*  type via OCIParamGet as it goes. So select_open() lets it run to    */
/*  completion, then inspects the resulting data_types[] and rejects    */
/*  with DB_SELECT_UNSUPPORTED_LOB if any column turned out to be       */
/*  CLOB/BLOB - see db_driver.h. Whatever get_multi_metadata() already   */
/*  allocated in that case is freed by the same cursor teardown path    */
/*  any other failure uses; nothing OCI-side is left dangling.          */
/*                                                                      */
/*  The CLOB array-fetch quirk (OCI cannot array-fetch a CLOB column -  */
/*  see execute_query_batch()'s own "DO NOT REMOVE" comment) can never  */
/*  actually trigger in this pass, since CLOB is already rejected       */
/*  above - the check is kept anyway so this stays correct once v2      */
/*  adds CLOB support on top of this same cursor, rather than quietly   */
/*  relying on the v1 rejection to make it moot.                        */
/* ================================================================== */

/* Local OCI error macro - same shape as execute_query_batch()'s own
 * CHECK_OCI, logging via ctx->select_logger (same destination this
 * fetch path already logs to today). */
#define ORACLE_CHECK_OCI(ctx, status) \
    do { \
        if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
            text errbuf[512]; sb4 errcode = 0; \
            OCIErrorGet((ctx)->errhp, 1, NULL, &errcode, errbuf, \
                        sizeof(errbuf), OCI_HTYPE_ERROR); \
            logger_write((ctx)->select_logger, LOG_ERROR, __func__, 0, \
                         "OCI Error %d: %s", errcode, (char *)errbuf); \
        } \
    } while (0)

/* Opaque to core - matches db_driver.h's forward declaration. */
struct db_select_cursor_t {
    oci_context_t  *ctx;           /* borrowed, not owned                */
    OCIStmt        *stmt;
    ub4             col_count;
    ub4             batch_size;    /* actual fetch_count in use          */

    OCIDefine      **def;
    char           **buffers;
    ub4             *buf_sizes;
    sb2            **indicators;
    ub2             *data_types;
    ub4             *data_sizes;
    char           (*col_names)[256];

    /* Required by multi_meta_request_t's contract even though this
     * cursor is scalar-only and neither of these is ever populated -
     * see block comment above. */
    OCILobLocator ***col_blob_locs;
    OCILobLocator   *clob_loc;
};

/* Mirrors build_row_xml_batch()'s own type_str switch exactly (see
 * OCI_Execute_Query_Batch_Module.c) so db_column_meta_t.field_type uses
 * the same vocabulary resultset_field_t.field_type already does. */
static const char *oracle_type_to_string(ub2 data_type)
{
    switch (data_type)
    {
        case SQLT_NUM:       return "NUMBER";
        case SQLT_DAT:       return "DATE";
        case SQLT_CHR:
        case SQLT_AFC:
        case SQLT_STR:       return "STRING";
        case SQLT_TIMESTAMP: return "TIMESTAMP";
        case SQLT_BLOB:      return "BLOB";
        case SQLT_CLOB:      return "CLOB";
        default:             return "UNKNOWN";
    }
}

/* Reverse-allocation-order teardown, same discipline free_batch_ctx()
 * follows - safe to call on a partially-built cursor from any
 * select_open() failure path, or on a fully-built one from
 * select_close(). NULL-safe throughout. */
static void oracle_select_cursor_free(db_select_cursor_t *cur)
{
    if (!cur) return;

    if (cur->col_blob_locs)
    {
        for (ub4 i = 0; i < cur->col_count; i++)
        {
            if (cur->col_blob_locs[i])
            {
                for (ub4 r = 0; r < cur->batch_size; r++)
                    if (cur->col_blob_locs[i][r])
                        OCIDescriptorFree(cur->col_blob_locs[i][r], OCI_DTYPE_LOB);
                free(cur->col_blob_locs[i]);
            }
        }
        free(cur->col_blob_locs);
    }

    if (cur->buffers)
    {
        for (ub4 i = 0; i < cur->col_count; i++)
            if (cur->buffers[i]) free(cur->buffers[i]);
        free(cur->buffers);
    }

    if (cur->indicators)
    {
        for (ub4 i = 0; i < cur->col_count; i++)
            if (cur->indicators[i]) free(cur->indicators[i]);
        free(cur->indicators);
    }

    if (cur->clob_loc)
        OCIDescriptorFree(cur->clob_loc, OCI_DTYPE_LOB);

    if (cur->def)        free(cur->def);
    if (cur->buf_sizes)  free(cur->buf_sizes);
    if (cur->data_types) free(cur->data_types);
    if (cur->data_sizes) free(cur->data_sizes);
    if (cur->col_names)  free(cur->col_names);

    /* Statement released last, same order execute_query_batch()'s own
     * Cleanup label uses (free_batch_ctx() before OCIStmtRelease). */
    if (cur->stmt && cur->ctx)
        OCIStmtRelease(cur->stmt, cur->ctx->errhp, NULL, 0, OCI_DEFAULT);

    free(cur);
}

static int oracle_select_open(oci_context_t              *ctx,
                               const db_select_request_t  *req,
                               db_select_cursor_t         **out_cursor,
                               db_column_meta_t           **out_columns,
                               int                         *out_column_count,
                               int                         *out_batch_size)
{
    if (!ctx || !req || !req->sql || !out_cursor || !out_columns ||
        !out_column_count || !out_batch_size)
        return -1;

    *out_cursor       = NULL;
    *out_columns      = NULL;
    *out_column_count = 0;
    *out_batch_size   = 0;

    db_select_cursor_t *cur = calloc(1, sizeof(*cur));
    if (!cur) return -1;

    cur->ctx        = ctx;
    cur->batch_size = (ub4)((req->fetch_array_size > 0) ? req->fetch_array_size : 1);

    ORACLE_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &cur->stmt, ctx->errhp,
                        (text *)req->sql, (ub4)strlen(req->sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!cur->stmt) { oracle_select_cursor_free(cur); return -1; }

    ORACLE_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, cur->stmt, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DESCRIBE_ONLY));

    ORACLE_CHECK_OCI(ctx,
        OCIAttrGet(cur->stmt, OCI_HTYPE_STMT,
                   &cur->col_count, 0, OCI_ATTR_PARAM_COUNT, ctx->errhp));

    if (cur->col_count == 0)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "No columns returned for select_open");
        oracle_select_cursor_free(cur);
        return -1;
    }

    cur->def           = calloc(cur->col_count, sizeof(OCIDefine *));
    cur->buffers       = calloc(cur->col_count, sizeof(char *));
    cur->buf_sizes     = calloc(cur->col_count, sizeof(ub4));
    cur->indicators    = calloc(cur->col_count, sizeof(sb2 *));
    cur->data_types    = calloc(cur->col_count, sizeof(ub2));
    cur->data_sizes    = calloc(cur->col_count, sizeof(ub4));
    cur->col_names     = calloc(cur->col_count, sizeof(*cur->col_names));
    cur->col_blob_locs = calloc(cur->col_count, sizeof(OCILobLocator **));

    if (!cur->def || !cur->buffers || !cur->buf_sizes || !cur->indicators ||
        !cur->data_types || !cur->data_sizes || !cur->col_names || !cur->col_blob_locs)
    {
        oracle_select_cursor_free(cur);
        return -1;
    }

    /* Required by get_multi_metadata()'s contract even for a scalar-
     * only cursor - see block comment above. */
    ORACLE_CHECK_OCI(ctx,
        OCIDescriptorAlloc(ctx->envhp, (void **)&cur->clob_loc,
                           OCI_DTYPE_LOB, 0, NULL));
    if (!cur->clob_loc) { oracle_select_cursor_free(cur); return -1; }

    multi_meta_request_t mmr;
    memset(&mmr, 0, sizeof(mmr));
    mmr.ctx           = ctx;
    mmr.stmt          = cur->stmt;
    mmr.col_count     = cur->col_count;
    mmr.fetch_count   = cur->batch_size;
    mmr.def           = cur->def;
    mmr.buffers       = cur->buffers;
    mmr.buf_sizes     = cur->buf_sizes;
    mmr.indicators    = cur->indicators;
    mmr.data_types    = cur->data_types;
    mmr.data_sizes    = cur->data_sizes;
    mmr.col_names     = cur->col_names;
    mmr.col_blob_locs = cur->col_blob_locs;
    mmr.clob_loc      = cur->clob_loc;
    mmr.deps          = NULL;   /* get_multi_metadata() ignores this field entirely */

    if (get_multi_metadata(&mmr) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "get_multi_metadata failed in select_open");
        oracle_select_cursor_free(cur);
        return -1;
    }

    /* Scalars only in this pass - see block comment above. */
    for (ub4 i = 0; i < cur->col_count; i++)
    {
        if (cur->data_types[i] == SQLT_CLOB || cur->data_types[i] == SQLT_BLOB)
        {
            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                         "select_open: column '%s' is %s - not supported "
                         "by this cursor yet, caller should fall back to "
                         "execute_query_batch()",
                         cur->col_names[i],
                         cur->data_types[i] == SQLT_CLOB ? "CLOB" : "BLOB");
            oracle_select_cursor_free(cur);
            return DB_SELECT_UNSUPPORTED_LOB;
        }
    }

    /* Not reachable while CLOB is rejected above - kept for when v2
     * adds CLOB support on top of this cursor (see block comment). */
    for (ub4 i = 0; i < cur->col_count; i++)
    {
        if (cur->data_types[i] == SQLT_CLOB)
        {
            cur->batch_size = 1;
            break;
        }
    }

    /* Bug fix (found via Driver_Select_Test.c's first real run) -
     * OCIStmtExecute(...OCI_DESCRIBE_ONLY) above only describes the
     * statement - it does NOT open a live cursor to fetch from.
     * execute_query_batch() always follows its own describe step with
     * a SEPARATE, real OCIStmtExecute(...OCI_DEFAULT) (its Stage 3)
     * before any OCIStmtFetch2 call - this was missing here entirely,
     * so select_fetch_batch()'s first fetch had nothing to fetch from.
     * Deliberately done here, after the LOB rejection check above, not
     * before it - no point spending a real execute round-trip on a
     * request this cursor is about to reject anyway. */
    ORACLE_CHECK_OCI(ctx,
        OCIStmtExecute(ctx->svchp, cur->stmt, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT));

    db_column_meta_t *columns = calloc(cur->col_count, sizeof(db_column_meta_t));
    if (!columns) { oracle_select_cursor_free(cur); return -1; }

    for (ub4 i = 0; i < cur->col_count; i++)
    {
        snprintf(columns[i].field_name, sizeof(columns[i].field_name),
                 "%s", cur->col_names[i]);
        snprintf(columns[i].field_type, sizeof(columns[i].field_type),
                 "%s", oracle_type_to_string(cur->data_types[i]));
    }

    *out_columns      = columns;
    *out_column_count = (int)cur->col_count;
    *out_batch_size   = (int)cur->batch_size;
    *out_cursor       = cur;

    return 0;
}

static int oracle_select_fetch_batch(db_select_cursor_t *cursor,
                                      resultset_t        **out_rs,
                                      int                  *out_rows_fetched)
{
    if (!cursor || !out_rs || !out_rows_fetched) return -1;

    *out_rs           = NULL;
    *out_rows_fetched = 0;

    oci_context_t *ctx = cursor->ctx;

    sword fetch_status = OCIStmtFetch2(cursor->stmt, ctx->errhp,
                                        cursor->batch_size,
                                        OCI_FETCH_NEXT, 0, OCI_DEFAULT);

    if (fetch_status != OCI_SUCCESS &&
        fetch_status != OCI_SUCCESS_WITH_INFO &&
        fetch_status != OCI_NO_DATA)
    {
        ORACLE_CHECK_OCI(ctx, fetch_status);
        return -1;
    }

    ub4 rows_fetched = 0;
    OCIAttrGet(cursor->stmt, OCI_HTYPE_STMT,
               &rows_fetched, 0, OCI_ATTR_ROWS_FETCHED, ctx->errhp);

    if (rows_fetched == 0)
        return 0;   /* cursor exhausted - *out_rs stays NULL */

    resultset_t *rs = resultset_create((int)rows_fetched, (int)cursor->col_count);
    if (!rs)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "resultset_create failed in select_fetch_batch "
                     "rows_fetched=%u col_count=%u",
                     rows_fetched, cursor->col_count);
        return -1;
    }

    /* Batch-local row numbering (1..rows_fetched) - same convention the
     * async path already uses for its own per-batch resultset_t (see
     * db_driver.h, SELECT IS A CURSOR). Global row position across
     * batches, if a caller needs it, is theirs to track - the driver
     * has no concept of "the whole query" once select_open() returns. */
    for (ub4 r = 0; r < rows_fetched; r++)
    {
        resultset_row_t *rs_row = resultset_get_row(rs, (int)(r + 1));

        for (ub4 c = 0; c < cursor->col_count; c++)
        {
            const char *type_str = oracle_type_to_string(cursor->data_types[c]);
            const char *value = cursor->buffers[c] +
                                 ((size_t)r * cursor->buf_sizes[c]);

            if (cursor->indicators[c][r] == -1)
                value = "";

            resultset_set_field(rs_row, (int)c, cursor->col_names[c],
                                 type_str, value);
        }
    }

    *out_rs           = rs;
    *out_rows_fetched = (int)rows_fetched;
    return 0;
}

static void oracle_select_close(db_select_cursor_t *cursor)
{
    oracle_select_cursor_free(cursor);
}

static const db_driver_t oracle_driver = {
    .driver_name          = "oracle",
    .connect              = oracle_connect,
    .disconnect           = oracle_disconnect,
    .get_session          = oracle_get_session,
    .release_session      = oracle_release_session,
    .session_is_alive     = oracle_session_is_alive,
    .reconnect_session    = oracle_reconnect_session,
    .health_check         = oracle_health_check,
    .select_open          = oracle_select_open,
    .select_fetch_batch   = oracle_select_fetch_batch,
    .select_close         = oracle_select_close
};

const db_driver_t *oracle_driver_get(void)
{
    return &oracle_driver;
}

