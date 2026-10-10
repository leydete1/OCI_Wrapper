/*
 * driver_oracle.c
 *
 * connect()/disconnect() are thin delegating wrappers around the
 * existing OCI_Connect_standalone()/OCI_Disconnect_standalone() (OCI_Connection.c) - they are
 * NOT reimplemented here, and OCI_Connect_standalone()/OCI_Disconnect_standalone() themselves
 * are NOT touched or moved out of OCI_Connection.c in this pass.
 *
 * Why delegate instead of relocate:
 *   OCI_Connect_standalone()/OCI_Disconnect_standalone() have three other direct callers today
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
 *   Physically moving the OCI_Connect_standalone()/OCI_Disconnect_standalone() bodies into
 *   this file - and updating the three other callers to go through
 *   db_driver_t instead - is a follow-up, purely mechanical pass once
 *   this delegation has been validated against Level2_Insert_Test.c's
 *   fixture (see Driver_Connect_Test.c).
 *
 * POOLED vs DIRECT dispatch (added after connect/disconnect were
 * validated - PASS confirmed live against freepdb1).
 * connect()/disconnect() below now do the ctx->ini->use_connection_pool
 * if/else that Data_Manager_Bootstrap.c used to do inline at every call
 * site (OCI_Connect_pool()/OCI_Disconnect_pool() vs OCI_Connect_standalone()/
 * OCI_Disconnect_standalone()). Bootstrap itself is NOT updated to call through
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
 * scalar columns only in that pass - see the SELECT IMPLEMENTATION
 * block below for the reasoning and exactly what was reused vs written
 * new.
 *
 * v2 - BLOB/CLOB (2026-09-14). select_open() no longer rejects LOB
 * columns with DB_SELECT_UNSUPPORTED_LOB - see LOB IMPLEMENTATION block
 * below. db_driver.h itself did NOT need to change for this: the
 * interface (variable batch size reported back from select_open(),
 * resultset_t as the sole return channel) was already correctly scoped
 * to support this without modification - db_select_cursor_t stays
 * opaque to core either way.
 */

#include <stdarg.h>   /* oracle_fmt() - Stage 2 dialect */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>    /* oracle_describe_table() - Stage 4a */

#include "driver_oracle.h"
#include "Connection.h"
#include "Connection_Pool.h"
/* Transaction_Manager.h is no longer included (Stage 3, 2026-10-03):
 * oracle_commit() makes one attempt and core's tx_commit_with_retry()
 * owns the retry loop, so the driver no longer calls back into core. */
/* Table_Metadata_Module.h is no longer included (Stage 4a, 2026-10-06):
 * the column describe/define work (oracle_define_columns) and the table
 * describe (oracle_describe_table) now live in this file. col_metadata_t
 * and metadata_request_t come from db_metadata.h via db_driver.h. */
#include "Resultset_Builder.h"       /* resultset_create/get_row/
                                            set_field/set_blob_field/free */
#include "Blob_Utils.h"              /* lookup_blob_index(),
                                            write_blob_to_file(),
                                            build_filename_with_timestamp() -
                                            zero OCI dependency, already
                                            proven by production code    */
#include "Clob_Utils.h"              /* build_clob_filename(),
                                            build_clob_url() - same
                                            extraction, same reasoning   */

/* get_mime_type() is declared in XML_Helper.h - deliberately NOT
 * included here. XML_Helper.h itself includes
 * OCI_Execute_Query_Batch_Module.h, which would make this driver depend
 * on the very module it exists to extract logic out of - a real
 * dependency-direction smell, not just an unused-include nit.
 * get_mime_type() itself has zero OCI or XML dependency (it is a pure
 * filename-extension lookup - see XML_Helper.c), so it is forward-
 * declared directly here instead, against the real function that
 * XML_Helper.c already defines and that production code already links.
 * get_mime_type() would be a good candidate to relocate into
 * OCI_Blob_Utils.c/.h alongside the other BLOB-writing helpers it
 * always gets called with - out of scope to move it as part of this
 * pass, since that touches XML_Helper.c/.h and its own other callers. */
const char *get_mime_type(const char *filename);

static int oracle_connect(oci_context_t *ctx)
{
    if (ctx->ini->use_connection_pool)
        return OCI_Connect_pool(ctx);

    return OCI_Connect_standalone(ctx);
}

static void oracle_disconnect(oci_context_t *ctx)
{
    if (ctx->ini->use_connection_pool)
    {
        OCI_Disconnect_pool(ctx);
        return;
    }

    OCI_Disconnect_standalone(ctx);
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
/*    - oracle_define_columns() (below; was get_multi_metadata() in    */
/*      Table_Metadata_Module.c until Stage 4a) does the real           */
/*      OCIParamGet/OCIDefineByPos/OCIDefineArrayOfStruct work. It      */
/*      still needs the col_blob_locs/clob_locs slots allocated even    */
/*      for a scalar-only cursor (it has no way to know in advance that */
/*      none of the columns will be BLOB/CLOB), so those are allocated  */
/*      below and simply never populated for a pure-scalar result set.  */
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
/*  CLOB/BLOB (v2, 2026-09-14): oracle_define_columns() can't know        */
/*  in advance that a column will be BLOB/CLOB - it discovers each        */
/*  column's type via OCIParamGet as it goes, and its BLOB/CLOB branches  */
/*  already allocate everything a fetch needs (a per-row                 */
/*  OCILobLocator array for each BLOB column, a                           */
/*  locator for each CLOB column). select_open() no longer rejects        */
/*  these columns - oracle_fetch_blob_field()/oracle_fetch_clob_field()   */
/*  below (called from select_fetch_batch()) read straight from what      */
/*  oracle_define_columns() already set up, the same way handle_blob_     */
/*  column_batch()/handle_clob_column_batch() do in execute_query_batch() */
/*  today - not reimplemented, just called from a different fetch loop.   */
/*                                                                        */
/*  The CLOB array-fetch quirk (OCI cannot array-fetch a CLOB column -    */
/*  see execute_query_batch()'s own "DO NOT REMOVE" comment) is real and  */
/*  live here now: select_open() downgrades cur->batch_size to 1 whenever */
/*  a CLOB column is present, and reports that actual (possibly smaller)  */
/*  batch size back through *out_batch_size - callers must read that,     */
/*  not assume req->fetch_array_size held (see db_select_request_t's own  */
/*  doc comment in db_driver.h).                                          */
/* ================================================================== */

/* Local OCI error macro - same shape as execute_query_batch()'s own
 * CHECK_OCI, logging via ctx->select_logger (same destination this
 * fetch path already logs to today). */
/* Stage 5 (2026-10-08): status is evaluated ONCE. These macros are
 * often passed the OCI call itself, e.g. ORACLE_CHECK_OCI(ctx,
 * OCIStmtPrepare2(...)); the old "(status) != OCI_SUCCESS &&
 * (status) != OCI_SUCCESS_WITH_INFO" text ran that call a second time
 * whenever the first one failed. Success paths are unchanged. */
#define ORACLE_CHECK_OCI(ctx, status) \
    do { \
        sword _chk_st = (status); \
        if (_chk_st != OCI_SUCCESS && _chk_st != OCI_SUCCESS_WITH_INFO) { \
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
    ub4             batch_size;    /* ACTUAL fetch_count used at
                                       OCIStmtFetch2 time - may be
                                       smaller than alloc_batch_size (see
                                       below) if a CLOB column forced it
                                       down to 1 after allocation.        */
    ub4             alloc_batch_size; /* batch size oracle_define_columns()
                                       actually sized every array to
                                       (req->fetch_array_size, before any
                                       CLOB downgrade). Cleanup MUST loop
                                       to this, not batch_size - freeing
                                       only up to a since-downgraded
                                       batch_size would leak the unused
                                       BLOB locator slots on a query that
                                       has both a BLOB and a CLOB column
                                       together (found during v2 review,
                                       2026-09-14, before it ever shipped
                                       as a real leak).                   */

    OCIDefine      **def;
    char           **buffers;
    ub4             *buf_sizes;
    sb2            **indicators;
    ub2             *data_types;
    ub4             *data_sizes;
    char           (*col_names)[256];

    OCILobLocator ***col_blob_locs;   /* [col][row], row < alloc_batch_size */
    /* 2026-10-10 - one locator PER CLOB/NCLOB column, [col]; NULL for
     * every other column. Until now there was one locator shared by
     * every CLOB column of the statement (carried over unchanged from
     * get_multi_metadata()): each column's define pointed at the same
     * address, so on every row the last CLOB column's fetch overwrote
     * the earlier ones. OCI_FIELD_TEST has CLOB_COL and NCLOB_COL, so
     * every SELECT * returned CLOB_COL as "" (when NCLOB_COL was NULL -
     * OCILobGetLength failed on the reset locator, logged as a stale
     * ORA-01403, and the length stayed 0) or as NCLOB_COL's text. Still
     * one row per fetch - OCI cannot array-fetch these (see the CLOB
     * downgrade in oracle_select_describe_and_define()). */
    OCILobLocator  **clob_locs;

    /* v2 - persist across every select_fetch_batch() call on this
     * cursor, exactly like execute_query_batch()'s own abs_rownum/
     * BLOB_index/CLOB_index locals persist across its internal batch
     * loop (Stage 5) today - these are NOT reset per batch. Needed so
     * BLOB/CLOB output filenames stay unique and monotonically
     * numbered across the whole query, not just within one batch. */
    unsigned int    abs_rownum;
    int             blob_index;
    int             clob_index;

    /* Bug fix (2026-09-15, found via a real ORA-01002 storm under full
     * load - 31+ occurrences in one run, root-caused by tracing an
     * unfiltered log sequence rather than guessed at). A short fetch
     * (fewer rows returned than requested) makes Oracle signal
     * OCI_NO_DATA on THAT SAME call, not on a subsequent one - this
     * cursor was already exhausted the moment that happened, but
     * nothing recorded it, so the next select_fetch_batch() call
     * issued a real OCIStmtFetch2 on an already-exhausted cursor,
     * which is exactly what ORA-01002 means. Every batch that
     * previously happened to be an EXACT match to the requested size
     * (every earlier test's truncated-to-5-rows queries) never hit
     * this, since Oracle only learns "no more" on the NEXT call in
     * that case - only a genuinely short fetch exposes it, which nothing
     * before this load test's real query mix ever exercised. */
    int             exhausted;   /* 0/1 - set once OCI_NO_DATA is seen */

    /* Added 2026-09-21, EXECUTE_PROCEDURE's own abstraction pass - a
     * cursor built by select_open_from_cursor() wraps a REFCURSOR
     * statement handle that was originally obtained via OCIHandleAlloc()
     * in oracle_dml_execute_procedure() (matching
     * OCI_Execute_Procedure_Module.c's own bind_parameters(), which
     * does the same), NOT via OCIStmtPrepare2() the way every select_
     * open() cursor's own stmt always has been. These two need
     * genuinely different release calls - OCIStmtRelease() expects
     * state OCIStmtPrepare2() sets up (a statement-cache key) that a
     * bare OCIHandleAlloc()'d handle never has; the original code
     * correctly uses OCIHandleFree() for exactly this case. Found and
     * fixed before it ever shipped, not after a real failure - see
     * oracle_select_cursor_free()'s own branch on this flag below. */
    int             stmt_owned_via_handle_alloc;

    /* Stage 5 (2026-10-08). text_lobs_inline is copied from the
     * request (db_select_request_t) - see oracle_fetch_clob_field().
     * bind_inds holds one NULL indicator per bind position; OCI keeps
     * a pointer to it from OCIBindByPos until the statement is
     * released, so it lives as long as the cursor and is freed after
     * the statement in oracle_select_cursor_free(). */
    int             text_lobs_inline;
    sb2            *bind_inds;

    /* The cursor-owned multi_meta_request_t that used to sit here (bug
     * fix 2026-09-30 - Driver_LOB_Select_Test.c Test 5, ASan SEGV in
     * kpccld2i under OCIStmtFetch2) is gone as of Stage 4a. The CLOB
     * column used to be defined against the address of a field in that
     * request struct, and OCI reads a define address at OCIStmtFetch2
     * time, so the struct had to live as long as the cursor.
     * oracle_define_columns() now defines it against &cur->clob_locs[col]
     * - owned by this heap-allocated struct - so the address OCI holds is
     * valid for exactly the cursor's lifetime by construction, with no
     * separate request struct to keep alive. */
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
                for (ub4 r = 0; r < cur->alloc_batch_size; r++)
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

    if (cur->clob_locs)
    {
        for (ub4 i = 0; i < cur->col_count; i++)
            if (cur->clob_locs[i])
                OCIDescriptorFree(cur->clob_locs[i], OCI_DTYPE_LOB);
        free(cur->clob_locs);
    }

    if (cur->def)        free(cur->def);
    if (cur->buf_sizes)  free(cur->buf_sizes);
    if (cur->data_types) free(cur->data_types);
    if (cur->data_sizes) free(cur->data_sizes);
    if (cur->col_names)  free(cur->col_names);

    /* Statement released last, same order execute_query_batch()'s own
     * Cleanup label uses (free_batch_ctx() before OCIStmtRelease).
     * Branches on stmt_owned_via_handle_alloc - see that field's own
     * comment in the struct definition above for why a REFCURSOR's
     * handle genuinely can't use the same release call a prepared
     * statement's own handle uses. */
    if (cur->stmt && cur->ctx)
    {
        if (cur->stmt_owned_via_handle_alloc)
            OCIHandleFree(cur->stmt, OCI_HTYPE_STMT);
        else
            OCIStmtRelease(cur->stmt, cur->ctx->errhp, NULL, 0, OCI_DEFAULT);
    }

    free(cur->bind_inds);   /* Stage 5 - after the statement, see struct */
    free(cur);
}

/* ==================================================================
 *  COLUMN DESCRIBE/DEFINE (Stage 4a, 2026-10-06)
 *
 *  oracle_define_columns() was get_multi_metadata() in
 *  Table_Metadata_Module.c. Moved here verbatim; the only changes are
 *  mechanical: it reads and fills the cursor's own arrays directly
 *  (cur->X where it used to take mmr->X, cur->alloc_batch_size where it
 *  took mmr->fetch_count - the same value at this point), and each CLOB
 *  column is defined against its own &cur->clob_locs[col] (2026-10-10;
 *  it was one shared locator - see the struct). Log messages are unchanged
 *  apart from the function name the logger stamps, so the Stage 4a
 *  log comparison can match them line for line.
 *
 *  CHECK_OCI_META is the same macro the module used: it logs to
 *  ctx->Metadata_logger, sets rc = -1 and jumps to the label.
 * ================================================================== */
/* Evaluates status once - see ORACLE_CHECK_OCI (Stage 5). */
#define CHECK_OCI_META(errhp, status, ctx, label)                       \
    do {                                                                 \
        sword _chk_st = (status);                                       \
        if (_chk_st != OCI_SUCCESS &&                                   \
            _chk_st != OCI_SUCCESS_WITH_INFO)                           \
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

static int oracle_define_columns(oci_context_t *ctx, db_select_cursor_t *cur)
{
    int rc = 0;

    if (!ctx || !cur || !cur->stmt)
    {
        if (ctx)
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx, cursor or stmt is NULL");
        return -1;
    }

    /* Validate all required output pointers are present */
    if (!cur->def        || !cur->buffers    || !cur->buf_sizes  ||
        !cur->indicators || !cur->data_types || !cur->data_sizes ||
        !cur->col_names  || !cur->col_blob_locs || !cur->clob_locs)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "One or more required array pointers are NULL - "
                     "call allocate_batch_buffers first");
        return -1;
    }

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "Entering get_multi_metadata col_count=%u "
                 "fetch_count=%u",
                 cur->col_count, cur->alloc_batch_size);

    OCIParam *param = NULL;

    for (ub4 i = 1; i <= cur->col_count; i++)
    {
        ub4 ci = i - 1;   /* 0-based column index */

        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "Processing column %u", i);

        /* ---- Column name ---- */
        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Calling OCIParamGet col=%u", i);
        CHECK_OCI_META(ctx->errhp,
            OCIParamGet(cur->stmt, OCI_HTYPE_STMT,
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
        memcpy(cur->col_names[ci], tmp_name, tmp_len);
        cur->col_names[ci][tmp_len] = '\0';

        /* ---- Data type ---- */
        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Calling OCIAttrGet OCI_ATTR_DATA_TYPE col=%u", i);
        CHECK_OCI_META(ctx->errhp,
            OCIAttrGet(param, OCI_DTYPE_PARAM,
                       &cur->data_types[ci], 0,
                       OCI_ATTR_DATA_TYPE, ctx->errhp),
            ctx, Cleanup);

        /* ---- Data size ---- */
        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "Calling OCIAttrGet OCI_ATTR_DATA_SIZE col=%u", i);
        CHECK_OCI_META(ctx->errhp,
            OCIAttrGet(param, OCI_DTYPE_PARAM,
                       &cur->data_sizes[ci], 0,
                       OCI_ATTR_DATA_SIZE, ctx->errhp),
            ctx, Cleanup);

        ub4 buf_size = cur->data_sizes[ci] + 32;
        if (buf_size < 64) buf_size = 64;
        cur->buf_sizes[ci] = buf_size;

        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "Column %u name=%s type=%u size=%u buf_size=%u",
                     i, cur->col_names[ci],
                     cur->data_types[ci], cur->data_sizes[ci],
                     buf_size);

        /* ---- Allocate per-column storage and register define ---- */

        if (cur->data_types[ci] == SQLT_BLOB)
        {
            /* BLOB: one locator per row in the fetch batch.
             * OCIDefineArrayOfStruct strides through the locator array
             * using sizeof(OCILobLocator*) as value_skip.             */
            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "Allocating BLOB locator array col=%u rows=%u",
                         ci, cur->alloc_batch_size);

            cur->col_blob_locs[ci] = calloc(cur->alloc_batch_size,
                                            sizeof(OCILobLocator *));
            if (!cur->col_blob_locs[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for col_blob_locs[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            for (ub4 r = 0; r < cur->alloc_batch_size; r++)
            {
                logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                             "OCIDescriptorAlloc BLOB locator "
                             "col=%u row=%u", ci, r);
                CHECK_OCI_META(ctx->errhp,
                    OCIDescriptorAlloc(ctx->envhp,
                                       (void **)&cur->col_blob_locs[ci][r],
                                       OCI_DTYPE_LOB, 0, NULL),
                    ctx, Cleanup);
            }

            cur->indicators[ci] = calloc(cur->alloc_batch_size, sizeof(sb2));
            if (!cur->indicators[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for indicators[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineByPos BLOB col=%u", i);
            CHECK_OCI_META(ctx->errhp,
                OCIDefineByPos(cur->stmt, &cur->def[ci], ctx->errhp,
                               i,
                               (dvoid *)&cur->col_blob_locs[ci][0],
                               -1,
                               SQLT_BLOB,
                               &cur->indicators[ci][0],
                               NULL, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineArrayOfStruct BLOB col=%u "
                         "value_skip=%zu ind_skip=%zu",
                         i, sizeof(OCILobLocator *), sizeof(sb2));
            CHECK_OCI_META(ctx->errhp,
                OCIDefineArrayOfStruct(cur->def[ci], ctx->errhp,
                                       (ub4)sizeof(OCILobLocator *),
                                       (ub4)sizeof(sb2),
                                       0, 0),
                ctx, Cleanup);
        }
        else if (cur->data_types[ci] == SQLT_CLOB)
        {
            /*
             * CLOB ARRAY FETCH RESTRICTION - OCI QUIRK - DO NOT REMOVE
             * ----------------------------------------------------------
             * OCI does not support array fetch of CLOB locators via
             * OCIDefineArrayOfStruct, so the caller must force
             * fetch_count=1 whenever any CLOB column is present
             * (oracle_select_describe_and_define(), after this returns).
             *
             * One locator per CLOB column (2026-10-10). It used to be one
             * locator shared by every CLOB column, so a second CLOB/NCLOB
             * column overwrote the first on every row - see the struct.
             * NCLOB arrives here as SQLT_CLOB too; its character set form
             * is read from the locator when the value is read.
             */
            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDescriptorAlloc CLOB locator col=%u", ci);
            CHECK_OCI_META(ctx->errhp,
                OCIDescriptorAlloc(ctx->envhp,
                                   (void **)&cur->clob_locs[ci],
                                   OCI_DTYPE_LOB, 0, NULL),
                ctx, Cleanup);

            cur->indicators[ci] = calloc(cur->alloc_batch_size, sizeof(sb2));
            if (!cur->indicators[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for indicators[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineByPos CLOB col=%u (own locator)",
                         i);
            CHECK_OCI_META(ctx->errhp,
                OCIDefineByPos(cur->stmt, &cur->def[ci], ctx->errhp,
                               i,
                               (dvoid *)&cur->clob_locs[ci],
                               -1,
                               SQLT_CLOB,
                               &cur->indicators[ci][0],
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
                         ci, cur->alloc_batch_size, buf_size);

            cur->buffers[ci] = calloc(cur->alloc_batch_size, buf_size);
            if (!cur->buffers[ci])
            {
                logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                             "calloc failed for buffers[%u]", ci);
                rc = -1;
                goto Cleanup;
            }

            cur->indicators[ci] = calloc(cur->alloc_batch_size, sizeof(sb2));
            if (!cur->indicators[ci])
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
                OCIDefineByPos(cur->stmt, &cur->def[ci], ctx->errhp,
                               i,
                               cur->buffers[ci],
                               (sb4)buf_size,
                               SQLT_STR,
                               &cur->indicators[ci][0],
                               NULL, NULL, OCI_DEFAULT),
                ctx, Cleanup);

            logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                         "OCIDefineArrayOfStruct scalar col=%u "
                         "value_skip=%u ind_skip=%zu",
                         i, buf_size, sizeof(sb2));
            CHECK_OCI_META(ctx->errhp,
                OCIDefineArrayOfStruct(cur->def[ci], ctx->errhp,
                                       buf_size,
                                       (ub4)sizeof(sb2),
                                       0, 0),
                ctx, Cleanup);
        }
    }

    logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                 "get_multi_metadata complete col_count=%u",
                 cur->col_count);

Cleanup:
    return rc;
}

/* Shared describe/define logic between oracle_select_open() (a freshly
 * prepared+described statement) and oracle_select_open_from_cursor()
 * (an already-open REFCURSOR from a procedure call) - factored out
 * 2026-09-21 rather than duplicated a third time, once
 * select_open_from_cursor() made the overlap obvious. needs_execute
 * distinguishes the one real difference: select_open()'s own statement
 * still needs its real OCIStmtExecute (its own prior call only
 * described it); a REFCURSOR is already open and fetching-ready from
 * the procedure's own OPEN statement, so this must NOT execute it
 * again. cur->stmt/cur->batch_size must already be set by the caller
 * before this runs. */
static int oracle_select_describe_and_define(oci_context_t       *ctx,
                                              db_select_cursor_t  *cur,
                                              int                  needs_execute,
                                              db_column_meta_t   **out_columns,
                                              int                 *out_column_count,
                                              int                 *out_batch_size)
{
    sword pcount_rc = OCIAttrGet(cur->stmt, OCI_HTYPE_STMT,
                                  &cur->col_count, 0,
                                  OCI_ATTR_PARAM_COUNT, ctx->errhp);
    ORACLE_CHECK_OCI(ctx, pcount_rc);
    /* Bug fix (2026-09-21, found while investigating a NULL-logger
     * report on a real test failure - prompted a closer check of
     * ORACLE_CHECK_OCI's own contract rather than assuming the logger
     * gap alone explained it). ORACLE_CHECK_OCI is log-only - unlike
     * the older CHECK_OCI_XXX macros elsewhere in this project, it has
     * no control-flow of its own; every caller must check the real
     * status explicitly. This call never did - only col_count==0 was
     * checked below, which is the OUTPUT value, not whether the call
     * itself succeeded. A genuine OCIAttrGet failure would previously
     * fall through with cur->col_count holding whatever it held before
     * the failed call (uninitialized/garbage, not reliably zero),
     * sizing every array below from bad data. Pre-existing in the
     * original oracle_select_open() before this function was factored
     * out of it - carried forward faithfully during that refactor, not
     * introduced by it, and not caught until now. */
    if (pcount_rc != OCI_SUCCESS && pcount_rc != OCI_SUCCESS_WITH_INFO)
        return -1;

    if (cur->col_count == 0)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "No columns returned for select_open");
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
    /* 2026-10-10 - one slot per column; oracle_define_columns()
     * allocates a locator only for CLOB columns. Replaces the single
     * shared locator that used to be allocated here for every cursor. */
    cur->clob_locs     = calloc(cur->col_count, sizeof(OCILobLocator *));

    if (!cur->def || !cur->buffers || !cur->buf_sizes || !cur->indicators ||
        !cur->data_types || !cur->data_sizes || !cur->col_names ||
        !cur->col_blob_locs || !cur->clob_locs)
        return -1;

    if (oracle_define_columns(ctx, cur) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "oracle_define_columns failed in select_open");
        return -1;
    }

    /* CLOB array-fetch restriction - OCI QUIRK - DO NOT REMOVE (same
     * quirk execute_query_batch() documents at its own call site). */
    for (ub4 i = 0; i < cur->col_count; i++)
    {
        if (cur->data_types[i] == SQLT_CLOB)
        {
            cur->batch_size = 1;
            break;
        }
    }

    if (needs_execute)
    {
        /* Bug fix (found via Driver_Select_Test.c's first real run) -
         * OCIStmtExecute(...OCI_DESCRIBE_ONLY) only describes the
         * statement - it does NOT open a live cursor to fetch from.
         * Not needed here for select_open_from_cursor()'s own case -
         * a REFCURSOR is already open and fetch-ready from the
         * procedure's own OPEN statement, executing it again would be
         * wrong, not just redundant. */
        /* Stage 5 (2026-10-08): a failed execute now fails
         * select_open(). It used to be logged only, select_open()
         * returned 0, and the failure surfaced at the first fetch
         * as a second, misleading error. */
        sword exec_rc = OCIStmtExecute(ctx->svchp, cur->stmt, ctx->errhp,
                                       0, 0, NULL, NULL, OCI_DEFAULT);
        ORACLE_CHECK_OCI(ctx, exec_rc);
        if (exec_rc != OCI_SUCCESS && exec_rc != OCI_SUCCESS_WITH_INFO)
            return -1;
    }

    db_column_meta_t *columns = calloc(cur->col_count, sizeof(db_column_meta_t));
    if (!columns) return -1;

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
    return 0;
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
    cur->alloc_batch_size = cur->batch_size;   /* size every array below is
                                                   actually sized to - see
                                                   struct comment. Only
                                                   cur->batch_size (not
                                                   this) may be downgraded
                                                   below.                  */

    cur->text_lobs_inline = req->text_lobs_inline ? 1 : 0;   /* Stage 5 */

    if (req->bind_count < 0 || (req->bind_count > 0 && !req->bind_values))
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Invalid binds: bind_count=%d bind_values=%s",
                     req->bind_count, req->bind_values ? "set" : "NULL");
        free(cur);
        return -1;
    }

    ORACLE_CHECK_OCI(ctx,
        OCIStmtPrepare2(ctx->svchp, &cur->stmt, ctx->errhp,
                        (text *)req->sql, (ub4)strlen(req->sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));
    if (!cur->stmt) { oracle_select_cursor_free(cur); return -1; }

    /* Stage 5 (2026-10-08) - bind values by position, same rules as
     * oracle_dml_execute(): always SQLT_STR, a NULL entry binds SQL
     * NULL. Bound before the execute below, which reads them; the
     * caller's strings only need to outlive this call. */
    if (req->bind_count > 0)
    {
        cur->bind_inds = calloc((size_t)req->bind_count, sizeof(sb2));
        if (!cur->bind_inds)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "calloc failed for bind indicators (bind_count=%d)",
                         req->bind_count);
            oracle_select_cursor_free(cur);
            return -1;
        }

        for (int k = 0; k < req->bind_count; k++)
        {
            const char *value = req->bind_values[k];
            OCIBind    *bind  = NULL;   /* owned by the statement */

            if (!value) cur->bind_inds[k] = -1;

            sword bind_rc = OCIBindByPos(cur->stmt, &bind, ctx->errhp,
                                         (ub4)(k + 1),
                                         value ? (void *)value : NULL,
                                         value ? (sb4)(strlen(value) + 1) : 0,
                                         SQLT_STR,
                                         &cur->bind_inds[k],
                                         NULL, NULL, 0, NULL, OCI_DEFAULT);
            ORACLE_CHECK_OCI(ctx, bind_rc);
            if (bind_rc != OCI_SUCCESS && bind_rc != OCI_SUCCESS_WITH_INFO)
            {
                oracle_select_cursor_free(cur);
                return -1;
            }
        }
    }

    sword desc_rc = OCIStmtExecute(ctx->svchp, cur->stmt, ctx->errhp,
                                   0, 0, NULL, NULL, OCI_DESCRIBE_ONLY);
    ORACLE_CHECK_OCI(ctx, desc_rc);
    if (desc_rc != OCI_SUCCESS && desc_rc != OCI_SUCCESS_WITH_INFO)
    {
        /* Stage 5 - was logged only; see the execute in
         * oracle_select_describe_and_define(). */
        oracle_select_cursor_free(cur);
        return -1;
    }

    db_column_meta_t *columns      = NULL;
    int                column_count = 0;
    int                batch_size   = 0;

    if (oracle_select_describe_and_define(ctx, cur, 1 /* needs_execute */,
                                           &columns, &column_count,
                                           &batch_size) != 0)
    {
        oracle_select_cursor_free(cur);
        return -1;
    }

    *out_columns      = columns;
    *out_column_count = column_count;
    *out_batch_size   = batch_size;
    *out_cursor       = cur;

    return 0;
}

/* Added 2026-09-21, EXECUTE_PROCEDURE's own abstraction pass - see
 * db_driver.h's own doc comment on select_open_from_cursor() for the
 * full design reasoning. cursor_handle is a db_proc_param_t's own
 * out_cursor_handle from a prior dml_execute_procedure() call - opaque
 * to core, but genuinely an OCIStmt* under the hood, already open and
 * fetch-ready from the procedure's own OPEN statement. */
static int oracle_select_open_from_cursor(oci_context_t        *ctx,
                                           void                 *cursor_handle,
                                           db_select_cursor_t  **out_cursor,
                                           db_column_meta_t    **out_columns,
                                           int                  *out_column_count,
                                           int                  *out_batch_size)
{
    /* Stage 6 (2026-10-09): the handle is ours from here on, success or
     * failure (db_driver.h, select_open_from_cursor() - Ownership). The
     * describe-failure path already released it through
     * oracle_select_cursor_free(); these two early returns did not, so
     * the caller could not know whether to release it - and the
     * procedure module released it again after a describe failure. */
    if (!cursor_handle) return -1;

    if (!ctx || !out_cursor || !out_columns ||
        !out_column_count || !out_batch_size)
    {
        OCIHandleFree(cursor_handle, OCI_HTYPE_STMT);
        return -1;
    }

    *out_cursor       = NULL;
    *out_columns      = NULL;
    *out_column_count = 0;
    *out_batch_size   = 0;

    db_select_cursor_t *cur = calloc(1, sizeof(*cur));
    if (!cur)
    {
        OCIHandleFree(cursor_handle, OCI_HTYPE_STMT);
        return -1;
    }

    cur->ctx  = ctx;
    cur->stmt = (OCIStmt *)cursor_handle;
    /* See struct comment on this field - a REFCURSOR's handle was
     * obtained via OCIHandleAlloc() in oracle_dml_execute_procedure(),
     * not OCIStmtPrepare2(), and needs the matching release call. */
    cur->stmt_owned_via_handle_alloc = 1;

    cur->batch_size = (ub4)ctx->ini->query_fetch_batch_size;
    if (cur->batch_size < 1) cur->batch_size = 1;
    cur->alloc_batch_size = cur->batch_size;

    db_column_meta_t *columns      = NULL;
    int                column_count = 0;
    int                batch_size   = 0;

    /* needs_execute=0 - see oracle_select_describe_and_define()'s own
     * comment on why. */
    if (oracle_select_describe_and_define(ctx, cur, 0,
                                           &columns, &column_count,
                                           &batch_size) != 0)
    {
        oracle_select_cursor_free(cur);
        return -1;
    }

    *out_columns      = columns;
    *out_column_count = column_count;
    *out_batch_size   = batch_size;
    *out_cursor       = cur;

    return 0;
}

/* Mirrors handle_blob_column_batch()'s exact logic (NULL check, length
 * check, chunked OCILobRead, filename/mime-type resolution via the
 * existing OCI_Blob_Utils.h functions, write_blob_to_file(),
 * resultset_set_blob_field()) - not reimplemented, just called from a
 * different fetch loop, reading straight from what oracle_define_columns()
 * already set up in cur->col_blob_locs[col_idx][row].
 *
 * One deliberate improvement over the original: applies the same
 * explicit-OCILobRead-return-code check that handle_clob_column_batch()
 * got in its own 2026-08-06 bug fix (CHECK_OCI-style macros only log,
 * they don't stop control flow - a failed read could otherwise leave
 * 'amount' unchanged, so "if (amount == 0)" alone would silently miss
 * it and treat corrupted output as success). handle_blob_column_batch()
 * itself was found to still lack this fix during this port - worth
 * applying there too as a separate, core-side follow-up.
 *
 * Ownership note: unlike the original's persistent, whole-query
 * BLOB_list[] array (indexed by BLOB_index_ptr, presumably kept alive
 * for the whole request), resultset_set_blob_field() was checked first
 * and confirmed to strncpy() every string into resultset_field_t's own
 * fixed buffers - nothing here needs to outlive this one call, so a
 * stack-local lob_item_t with an immediate free() below is sufficient
 * and simpler, without changing any observable behavior. */
static int oracle_fetch_blob_field(oci_context_t *ctx, db_select_cursor_t *cur,
                                    ub4 row, ub4 col_idx, resultset_row_t *rs_row,
                                    uint64_t *out_bytes)
{
    int blob_index = cur->blob_index;
    *out_bytes = 0;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering col=%u row=%u abs_rownum=%u blob_index=%d",
                 col_idx, row, cur->abs_rownum, blob_index);

    lob_item_t item;
    memset(&item, 0, sizeof(item));

    item.is_null = (cur->indicators[col_idx][row] == -1);
    if (item.is_null)
    {
        resultset_set_field(rs_row, (int)col_idx, cur->col_names[col_idx], "BLOB", "");
        cur->blob_index++;
        return 0;
    }

    ORACLE_CHECK_OCI(ctx,
        OCIDescriptorAlloc(ctx->envhp, (void **)&item.lob_loc, OCI_DTYPE_LOB, 0, NULL));
    if (!item.lob_loc) return -1;

    ORACLE_CHECK_OCI(ctx,
        OCILobLocatorAssign(ctx->svchp, ctx->errhp,
                            cur->col_blob_locs[col_idx][row], &item.lob_loc));

    ORACLE_CHECK_OCI(ctx,
        OCILobGetLength(ctx->svchp, ctx->errhp, item.lob_loc, &item.blob_size));

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "BLOB col=%u row=%u size=%u index=%d",
                 col_idx, row, item.blob_size, blob_index);

    if (item.blob_size == 0)
    {
        resultset_set_field(rs_row, (int)col_idx, cur->col_names[col_idx], "BLOB", "");
        OCIDescriptorFree(item.lob_loc, OCI_DTYPE_LOB);
        cur->blob_index++;
        return 0;
    }

    ub4 total_size = item.blob_size;
    ub4 offset     = 1;

    item.blob_data = malloc(total_size);
    if (!item.blob_data)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "malloc failed for BLOB data size=%u", total_size);
        OCIDescriptorFree(item.lob_loc, OCI_DTYPE_LOB);
        return -1;
    }

    ub4 bytes_remaining = total_size;
    unsigned char *write_ptr = item.blob_data;

    while (bytes_remaining > 0)
    {
        ub4 chunk = (ub4)ctx->ini->chunk_read_size;
        if (chunk > bytes_remaining) chunk = bytes_remaining;
        ub4 amount = chunk;

        sword lob_read_rc = OCILobRead(ctx->svchp, ctx->errhp, item.lob_loc,
                                        &amount, offset, write_ptr, chunk,
                                        NULL, NULL, 0, SQLCS_IMPLICIT);
        ORACLE_CHECK_OCI(ctx, lob_read_rc);

        if (lob_read_rc != OCI_SUCCESS && lob_read_rc != OCI_SUCCESS_WITH_INFO)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "OCILobRead failed (rc=%d) at offset=%u - aborting "
                         "this BLOB read rather than silently treating the "
                         "failure as success", (int)lob_read_rc, offset);
            free(item.blob_data);
            OCIDescriptorFree(item.lob_loc, OCI_DTYPE_LOB);
            return -1;
        }

        if (amount == 0)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "OCILobRead returned 0 bytes unexpectedly");
            free(item.blob_data);
            OCIDescriptorFree(item.lob_loc, OCI_DTYPE_LOB);
            return -1;
        }

        write_ptr       += amount;
        offset          += amount;
        bytes_remaining -= amount;
    }

    /* ---- Filename, MIME type - same lookup precedence as
     * handle_blob_column_batch() ---- */
    const char *search_col = ctx->ini->BLOB_default_file_name_col;
    switch (blob_index)
    {
        case 1: search_col = ctx->ini->BLOB_default_file_name_col_1; break;
        case 2: search_col = ctx->ini->BLOB_default_file_name_col_2; break;
        case 3: search_col = ctx->ini->BLOB_default_file_name_col_3; break;
        case 4: search_col = ctx->ini->BLOB_default_file_name_col_4; break;
        case 5: search_col = ctx->ini->BLOB_default_file_name_col_5; break;
    }

    int name_col_idx = lookup_blob_index(cur->col_names, (int)cur->col_count,
                                         search_col, ctx);

    char final_name[512];
    if (name_col_idx >= 0 &&
        cur->indicators[name_col_idx][row] != -1 &&
        cur->buffers[name_col_idx] != NULL)
    {
        const char *name_val = cur->buffers[name_col_idx] +
                                ((size_t)row * cur->buf_sizes[name_col_idx]);

        if (ctx->ini->BLOB_append_file_timestamp == 1)
            build_filename_with_timestamp(name_val, final_name,
                                          sizeof(final_name), blob_index, ctx);
        else
            snprintf(final_name, sizeof(final_name), "%s", name_val);
    }
    else
    {
        snprintf(final_name, sizeof(final_name), "%s_%d",
                 ctx->ini->BLOB_default_file_name, blob_index);
    }

    item.column_name = cur->col_names[col_idx];
    item.file_name   = strdup(final_name);
    item.mime_type   = strdup(get_mime_type(item.file_name));

    /* output_file_url/output_file_destination point straight into
     * ctx->ini's own config strings when sharing is disabled - do NOT
     * free those; only the strdup("N/A") branch is heap-owned here.   */
    int url_is_heap = 0, dest_is_heap = 0;

    if (ctx->ini->xml_share_BLOB_URL_path)
        item.output_file_url = ctx->ini->BLOB_URL_path;
    else { item.output_file_url = strdup("N/A"); url_is_heap = 1; }

    if (ctx->ini->xml_share_BLOB_host_path)
        item.output_file_destination = ctx->ini->BLOB_output_dir;
    else { item.output_file_destination = strdup("N/A"); dest_is_heap = 1; }

    write_blob_to_file(&item, ctx->ini->BLOB_output_dir, ctx);

    resultset_set_blob_field(rs_row, (int)col_idx, item.column_name,
                              item.file_name, item.output_file_destination,
                              item.output_file_url, item.blob_size,
                              item.mime_type);

    *out_bytes = item.blob_size;

    free(item.file_name);
    free(item.mime_type);
    free(item.blob_data);
    if (url_is_heap)  free(item.output_file_url);
    if (dest_is_heap) free(item.output_file_destination);
    OCIDescriptorFree(item.lob_loc, OCI_DTYPE_LOB);

    cur->blob_index++;
    return 0;
}

/* Mirrors handle_clob_column_batch()'s exact logic, including its
 * 2026-08-06 explicit-return-code fix for OCILobRead (see block comment
 * above oracle_fetch_blob_field() for why that matters). CLOB always
 * uses row index [0] here, matching the original ("bc->indicators
 * [col_idx][0]") - safe because cur->batch_size is always 1 whenever a
 * CLOB column is present (see the CLOB array-fetch quirk in
 * oracle_select_open()), so 'row' passed in from
 * oracle_select_fetch_batch()'s loop is always 0 in that case anyway.
 *
 * 2026-10-10: reads cur->clob_locs[col_idx] (this column's own
 * locator, not one shared by every CLOB column), fails the fetch if
 * the length cannot be read, and reads NCLOB in its own character set
 * form. */
static int oracle_fetch_clob_field(oci_context_t *ctx, db_select_cursor_t *cur,
                                    ub4 col_idx, resultset_row_t *rs_row,
                                    uint64_t *out_bytes)
{
    int clob_index = cur->clob_index;
    *out_bytes = 0;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering col=%u name=%s abs_rownum=%u clob_index=%d",
                 col_idx, cur->col_names[col_idx], cur->abs_rownum, clob_index);

    if (cur->indicators[col_idx][0] == -1)
    {
        resultset_set_field(rs_row, (int)col_idx, cur->col_names[col_idx], "CLOB", "");
        cur->clob_index++;
        return 0;
    }

    /* 2026-10-10 - this column's own locator (see the struct), and the
     * length call is now checked. It used to be logged only: a failure
     * left lob_len at 0 and the value came back as "" with the fetch
     * reporting success. A value that cannot be read now fails the
     * fetch, the same rule as a failed OCILobRead below. The rc is
     * logged as well as OCIErrorGet's text, because an invalid locator
     * (OCI_INVALID_HANDLE) sets no error and that text is then left
     * over from an earlier call - which is how a failed length call
     * showed up as "ORA-01403: no data found". */
    OCILobLocator *loc = cur->clob_locs[col_idx];
    ub4 lob_len = 0;
    sword len_rc = OCILobGetLength(ctx->svchp, ctx->errhp, loc, &lob_len);
    if (len_rc != OCI_SUCCESS && len_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI(ctx, len_rc);
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "OCILobGetLength failed (rc=%d) for col=%s - fetch "
                     "failed rather than returning the value as empty",
                     (int)len_rc, cur->col_names[col_idx]);
        return -1;
    }

    /* NCLOB (2026-10-10): read in the locator's own character set
     * form. Reading an NCLOB as SQLCS_IMPLICIT is rejected by Oracle
     * (ORA-24806, LOB form mismatch). */
    ub1 csfrm = SQLCS_IMPLICIT;
    sword form_rc = OCILobCharSetForm(ctx->envhp, ctx->errhp, loc, &csfrm);
    if (form_rc != OCI_SUCCESS && form_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI(ctx, form_rc);
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "OCILobCharSetForm failed (rc=%d) for col=%s - fetch "
                     "failed", (int)form_rc, cur->col_names[col_idx]);
        return -1;
    }
    if (csfrm != SQLCS_NCHAR) csfrm = SQLCS_IMPLICIT;

    if (lob_len == 0)
    {
        resultset_set_field(rs_row, (int)col_idx, cur->col_names[col_idx], "CLOB", "");
        cur->clob_index++;
        return 0;
    }

    char *clob_buf = calloc(1, lob_len + 1);
    if (!clob_buf)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for CLOB buffer size=%u", lob_len + 1);
        return -1;
    }

    ub4   offset          = 1;
    ub4   bytes_remaining = lob_len;
    char *write_ptr       = clob_buf;

    while (bytes_remaining > 0)
    {
        ub4 chunk = (ub4)ctx->ini->chunk_read_size;
        if (chunk > bytes_remaining) chunk = bytes_remaining;
        ub4 amount = chunk;

        sword lob_read_rc = OCILobRead(ctx->svchp, ctx->errhp, loc,
                                        &amount, offset, write_ptr, chunk,
                                        NULL, NULL, 0, csfrm);
        ORACLE_CHECK_OCI(ctx, lob_read_rc);

        if (lob_read_rc != OCI_SUCCESS && lob_read_rc != OCI_SUCCESS_WITH_INFO)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "OCILobRead failed (rc=%d) at offset=%u - aborting "
                         "this CLOB read rather than silently treating the "
                         "failure as success", (int)lob_read_rc, offset);
            free(clob_buf);
            return -1;
        }

        if (amount == 0)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "OCILobRead returned 0 bytes unexpectedly");
            free(clob_buf);
            return -1;
        }

        write_ptr       += amount;
        offset          += amount;
        bytes_remaining -= amount;
    }

    clob_buf[lob_len] = '\0';

    /* Stage 5 (2026-10-08) - db_select_request_t.text_lobs_inline: the
     * text goes in the field and no file is written. A value the field
     * cannot hold fails the fetch rather than being cut short. */
    if (cur->text_lobs_inline)
    {
        size_t field_max = sizeof(rs_row->fields[0].value) - 1;
        if ((size_t)lob_len > field_max)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "CLOB col=%s length=%u does not fit the %zu-byte "
                         "field (text_lobs_inline) - fetch failed, value "
                         "not truncated", cur->col_names[col_idx], lob_len,
                         field_max);
            free(clob_buf);
            return -1;
        }
        resultset_set_field(rs_row, (int)col_idx, cur->col_names[col_idx],
                            "CLOB", clob_buf);
        *out_bytes = lob_len;
        free(clob_buf);
        cur->clob_index++;
        return 0;
    }

    char clob_filename[512];
    build_clob_filename(cur->col_names[col_idx], cur->abs_rownum, clob_index,
                         clob_filename, sizeof(clob_filename), ctx);

    char clob_filepath[768];
    snprintf(clob_filepath, sizeof(clob_filepath), "%s/%s",
             ctx->ini->CLOB_output_dir, clob_filename);

    lob_item_t clob_item;
    memset(&clob_item, 0, sizeof(clob_item));
    clob_item.file_name = clob_filename;
    clob_item.blob_data = (unsigned char *)clob_buf;
    clob_item.blob_size = lob_len;

    if (write_blob_to_file(&clob_item, ctx->ini->CLOB_output_dir, ctx) != 0)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "write_blob_to_file failed for CLOB %s - emitting "
                     "inline content instead of a file reference",
                     clob_filepath);
        resultset_set_field(rs_row, (int)col_idx, cur->col_names[col_idx], "CLOB", clob_buf);
        *out_bytes = lob_len;   /* real bytes were still read from Oracle,
                                    just not written to a file - metrics
                                    should reflect that regardless        */
        free(clob_buf);
        cur->clob_index++;
        return 0;
    }

    free(clob_buf);

    char clob_url[768];
    build_clob_url(clob_filename, clob_filepath, clob_url, sizeof(clob_url), ctx);

    resultset_set_field(rs_row, (int)col_idx, cur->col_names[col_idx], "CLOB", clob_url);

    *out_bytes = lob_len;

    cur->clob_index++;
    return 0;
}

static int oracle_select_fetch_batch(db_select_cursor_t      *cursor,
                                      resultset_t             **out_rs,
                                      int                       *out_rows_fetched,
                                      db_fetch_batch_stats_t   *out_stats)
{
    if (!cursor || !out_rs || !out_rows_fetched) return -1;

    *out_rs           = NULL;
    *out_rows_fetched = 0;
    if (out_stats) memset(out_stats, 0, sizeof(*out_stats));

    /* Short-circuit: a previous call already saw OCI_NO_DATA, meaning
     * Oracle already told us this cursor is exhausted. Issuing a real
     * OCIStmtFetch2 again here is exactly what produces ORA-01002 -
     * see the struct comment on cursor->exhausted for the full
     * reasoning. Safe, cheap no-op matching the caller's existing
     * "rows_fetched == 0 means stop" contract. */
    if (cursor->exhausted)
        return 0;

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

    /* OCI_NO_DATA can arrive WITH a non-zero row count - a short fetch
     * (fewer rows available than requested) signals exhaustion on this
     * same call, not a subsequent one. Record it now regardless of how
     * many rows came back this time, so the NEXT call (if the caller's
     * loop makes one, which it always does - it doesn't know yet
     * whether this was the last real batch) hits the short-circuit
     * above instead of issuing a real fetch on an already-done cursor. */
    if (fetch_status == OCI_NO_DATA)
        cursor->exhausted = 1;

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

    /* Batch-local row numbering (1..rows_fetched) for resultset_get_row -
     * same convention the async path already uses for its own per-batch
     * resultset_t (see db_driver.h, SELECT IS A CURSOR). cur->abs_rownum
     * below is a SEPARATE, cursor-lifetime counter (see struct comment)
     * used only for BLOB/CLOB filename numbering - the two numbering
     * schemes are intentionally different and must not be confused. */
    for (ub4 r = 0; r < rows_fetched; r++)
    {
        resultset_row_t *rs_row = resultset_get_row(rs, (int)(r + 1));

        /* Incremented BEFORE column dispatch, not after - CLOB filenames
         * (build_clob_filename(), called from oracle_fetch_clob_field()
         * below) read cursor->abs_rownum directly, and the original,
         * pre-integration execute_query_batch() uses 1-based numbering
         * for its own equivalent abs_rownum (row 1 is "row1", not
         * "row0"). Found via a real before/after comparison
         * (2026-09-13): incrementing after dispatch meant every CLOB
         * filename was off by one row number versus the original -
         * content was byte-identical, only the filename's row number
         * was wrong. BLOB filenames were unaffected - they use
         * blob_index, a separate counter, not this one. */
        cursor->abs_rownum++;

        for (ub4 c = 0; c < cursor->col_count; c++)
        {
            if (cursor->data_types[c] == SQLT_BLOB)
            {
                uint64_t field_bytes = 0;
                if (oracle_fetch_blob_field(ctx, cursor, r, c, rs_row, &field_bytes) != 0)
                {
                    resultset_free(rs);
                    return -1;
                }
                if (out_stats)
                {
                    out_stats->blob_count++;
                    out_stats->blob_bytes += field_bytes;
                }
                continue;
            }

            if (cursor->data_types[c] == SQLT_CLOB)
            {
                uint64_t field_bytes = 0;
                if (oracle_fetch_clob_field(ctx, cursor, c, rs_row, &field_bytes) != 0)
                {
                    resultset_free(rs);
                    return -1;
                }
                if (out_stats)
                {
                    out_stats->clob_count++;
                    out_stats->clob_bytes += field_bytes;
                }
                continue;
            }

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

/* ================================================================
 * DML EXECUTE / COMMIT / ROLLBACK (2026-09-16) - DELETE's own
 * abstraction pass (see db_driver.h's own doc comments for
 * dml_execute()/commit()/rollback() for the full design reasoning -
 * the logger parameter, why this doesn't commit itself, why it's one
 * self-contained call rather than a cursor). Mirrors
 * OCI_Delete_Execute_Module.c's own Stage 2/4/5 exactly - same OCI
 * functions, same sequence, same bind_hdls cleanup convention (a plain
 * free() of the local pointer array, never an explicit per-handle OCI
 * release - OCIStmtRelease already owns that internally, confirmed
 * against that module's own Cleanup section rather than assumed) -
 * just behind the driver boundary instead of inline in that module.
 * ================================================================ */

/* Same shape as ORACLE_CHECK_OCI above, but takes an explicit logger
 * instead of assuming ctx->select_logger - see dml_execute()'s doc
 * comment in db_driver.h for why that assumption doesn't hold for a
 * function with more than one eventual caller. */
/* Evaluates status once - see ORACLE_CHECK_OCI (Stage 5). */
#define ORACLE_CHECK_OCI_LOG(ctx, logger, status) \
    do { \
        sword _chk_st = (status); \
        if (_chk_st != OCI_SUCCESS && _chk_st != OCI_SUCCESS_WITH_INFO) { \
            text errbuf[512]; sb4 errcode = 0; \
            OCIErrorGet((ctx)->errhp, 1, NULL, &errcode, errbuf, \
                        sizeof(errbuf), OCI_HTYPE_ERROR); \
            logger_write((logger), LOG_ERROR, __func__, 0, \
                         "OCI Error %d: %s", errcode, (char *)errbuf); \
        } \
    } while (0)

/* Added 2026-09-22, the DDL module's own abstraction pass - see
 * db_driver.h's own doc comment on dml_execute()'s out_error_message
 * parameter for why this needs to exist at all (execute_ddl_statement()'s
 * own callers embed the real Oracle error text in the client-facing
 * response, unlike every prior caller of this function). Deliberately
 * NOT folded into ORACLE_CHECK_OCI_LOG itself - that macro is used
 * pervasively across every function in this file, and widening its own
 * contract would touch far more call sites than this one function
 * actually needs changed. A second OCIErrorGet() call here (alongside
 * ORACLE_CHECK_OCI_LOG's own) is accepted, minor duplication - OCIErrorGet
 * is idempotent, just re-reading the same already-set error state, and
 * this is far safer than reworking the shared macro for one caller's
 * own need. out_buf/out_buf_size may be NULL/0 - a no-op then, matching
 * dml_execute()'s own optional contract. */
static void oracle_capture_error_text(oci_context_t *ctx, sword status,
                                       char *out_buf, size_t out_buf_size)
{
    if (!out_buf || out_buf_size == 0) return;
    if (status == OCI_SUCCESS || status == OCI_SUCCESS_WITH_INFO) return;

    text errbuf[512]; sb4 errcode = 0;
    OCIErrorGet(ctx->errhp, 1, NULL, &errcode, errbuf,
                sizeof(errbuf), OCI_HTYPE_ERROR);
    snprintf(out_buf, out_buf_size, "OCI Error %d: %s", (int)errcode, (char *)errbuf);
}

static int oracle_dml_execute(oci_context_t            *ctx,
                               logger_t                 *logger,
                               const db_dml_request_t   *req,
                               int                      *out_rows_affected,
                               char                     *out_error_message,
                               size_t                    out_error_message_size)
{
    if (!ctx || !req || !req->sql || !out_rows_affected) return -1;
    if (req->bind_count > 0 && !req->bind_values) return -1;

    *out_rows_affected = 0;

    OCIStmt *stmt = NULL;

    sword prepare_rc = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                                        (text *)req->sql,
                                        (ub4)strlen(req->sql),
                                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (prepare_rc != OCI_SUCCESS && prepare_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, prepare_rc);
        oracle_capture_error_text(ctx, prepare_rc, out_error_message, out_error_message_size);
        return -1;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "OCIStmtPrepare2 OK bind_count=%d", req->bind_count);

    /* Bind handles are allocated/attached internally by OCIBindByPos -
     * this array only ever holds the resulting pointers for the
     * duration of the call, never individually released; OCIStmtRelease
     * below already owns that (confirmed against
     * OCI_Delete_Execute_Module.c's own bind_hdls handling). */
    OCIBind **bind_hdls = NULL;
    /* Indicators - one sb2 per bind position, persisting until
     * OCIStmtExecute actually reads them (NOT at bind time - a classic
     * OCI lifetime trap this fix nearly fell into: a loop-local sb2
     * would go out of scope long before execute runs). Only entries
     * for a NULL bind_values[k] are ever set to -1; every other slot
     * stays 0 (calloc'd), meaning "not null", exactly matching
     * OCIBindByPos's own indp=NULL convention for a value that's
     * always present - see the loop below. */
    sb2 *null_inds = NULL;
    if (req->bind_count > 0)
    {
        bind_hdls  = calloc((size_t)req->bind_count, sizeof(OCIBind *));
        null_inds  = calloc((size_t)req->bind_count, sizeof(sb2));
        if (!bind_hdls || !null_inds)
        {
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "calloc failed for bind_hdls/null_inds (bind_count=%d)",
                         req->bind_count);
            free(bind_hdls);
            free(null_inds);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
            return -1;
        }
    }

    for (int k = 0; k < req->bind_count; k++)
    {
        const char *value = req->bind_values[k];

        /* Bug fix (2026-09-19, found during UPDATE's own abstraction
         * pass, before it was ever integrated - a NULL bind_values[k]
         * used to silently become an empty string, not SQL NULL. That
         * was harmless for DELETE's own WHERE keys (never legitimately
         * NULL), but UPDATE's SET clause genuinely needs to set a
         * column to NULL - binding '' isn't reliably the same as
         * binding NULL outside Oracle's own VARCHAR2 empty-string
         * quirk, and relying on that quirk rather than being explicit
         * is fragile. null_inds[k] (persisting until OCIStmtExecute
         * actually reads it, unlike a loop-local sb2 would) now says
         * so explicitly - already 0 ("not null") from calloc for every
         * position with a real value; only set to -1 here when
         * value is NULL. */
        if (!value) null_inds[k] = -1;

        sword bind_rc = OCIBindByPos(stmt, &bind_hdls[k], ctx->errhp,
                                      (ub4)(k + 1),
                                      value ? (void *)value : NULL,
                                      value ? (sb4)(strlen(value) + 1) : 0,
                                      SQLT_STR,
                                      value ? NULL : &null_inds[k],
                                      NULL, NULL, 0, NULL,
                                      OCI_DEFAULT);
        if (bind_rc != OCI_SUCCESS && bind_rc != OCI_SUCCESS_WITH_INFO)
        {
            ORACLE_CHECK_OCI_LOG(ctx, logger, bind_rc);
            oracle_capture_error_text(ctx, bind_rc, out_error_message, out_error_message_size);
            free(bind_hdls);
            free(null_inds);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
            return -1;
        }

        logger_write(logger, LOG_DEBUG, __func__, 0,
                     "Bound value at position %d (%s)", k + 1,
                     value ? "value" : "SQL NULL");
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Calling OCIStmtExecute iters=1");

    sword exec_rc = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                                    1, 0, NULL, NULL, OCI_DEFAULT);
    if (exec_rc != OCI_SUCCESS && exec_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, exec_rc);
        oracle_capture_error_text(ctx, exec_rc, out_error_message, out_error_message_size);
        free(bind_hdls);
        free(null_inds);
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    ub4 rows_affected = 0;
    sword attr_rc = OCIAttrGet(stmt, OCI_HTYPE_STMT,
                                &rows_affected, NULL,
                                OCI_ATTR_ROW_COUNT, ctx->errhp);
    if (attr_rc != OCI_SUCCESS && attr_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, attr_rc);
        oracle_capture_error_text(ctx, attr_rc, out_error_message, out_error_message_size);
        free(bind_hdls);
        free(null_inds);
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    *out_rows_affected = (int)rows_affected;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "OCIStmtExecute OK rows_affected=%d", *out_rows_affected);

    free(bind_hdls);
    free(null_inds);
    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return 0;
}

/*
 * oracle_commit() - one OCITransCommit attempt (Stage 3, 2026-10-03;
 * classification narrowed in item 3c, 2026-10-05). See db_driver.h,
 * commit(), for the result contract; core's tx_commit_with_retry()
 * owns the retry loop.
 *
 * Classification:
 *   OCI_SUCCESS / OCI_SUCCESS_WITH_INFO      -> 0
 *   ORA-00000 / ORA-01085 on a non-success   -> 0  ("soft success", no
 *                                               outstanding work -
 *                                               unchanged from Stage 3)
 *   ORA-04020 / ORA-04021 / ORA-04031        -> DB_TX_RETRYABLE
 *   anything else                            -> -1 (do not retry)
 *
 * Why so few retryable codes (3c). A failed COMMIT is only worth
 * retrying when the transaction is certainly still pending - the
 * commit did not happen and nothing was undone. That is true of the
 * transient lock/memory errors above (a library-cache lock deadlock or
 * timeout, shared-pool exhaustion), which is what the original
 * tx_commit() header said was retried. It is NOT true of:
 *   - ORA-02091 "transaction rolled back" (e.g. a deferred constraint
 *     violated at commit): Oracle has already rolled the work back, so
 *     a retry commits nothing, succeeds, and the caller was told its
 *     work was saved when it was not. Before 3c this happened.
 *   - a lost connection (ORA-03113/03114/03135, ORA-12xxx): the
 *     outcome is UNKNOWN - the commit may or may not have reached the
 *     server - and a retry on the same session cannot find out.
 * Both now fail at once; tx_commit() then rolls back (a no-op if the
 * server already did) and reports ABORTED.
 */
static int oracle_commit_is_retryable(sb4 ora_code)
{
    return ora_code == 4020 || ora_code == 4021 || ora_code == 4031;
}

static int oracle_commit_outcome_unknown(sb4 ora_code)
{
    return ora_code == 3113 || ora_code == 3114 || ora_code == 3135 ||
           (ora_code >= 12150 && ora_code <= 12699);
}

static int oracle_commit(oci_context_t *ctx, logger_t *logger)
{
    if (!ctx) return -1;

    sword status = OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT);
    if (status == OCI_SUCCESS || status == OCI_SUCCESS_WITH_INFO)
    {
        logger_write(logger, LOG_INFO, __func__, 0, "OCITransCommit OK");
        return 0;
    }

    text errbuf[512] = {0};
    sb4  ora_code    = 0;
    OCIErrorGet(ctx->errhp, 1, NULL, &ora_code,
                errbuf, sizeof(errbuf), OCI_HTYPE_ERROR);

    logger_write(logger, LOG_WARN, __func__, 0,
                 "OCITransCommit returned non-success  "
                 "oci_status=%d  ORA-%05d: %s",
                 (int)status, (int)ora_code, (char *)errbuf);

    if (ora_code == 0 || ora_code == 1085)
    {
        logger_write(logger, LOG_INFO, __func__, 0,
                     "ORA-%05d treated as soft success "
                     "(no outstanding work to commit)",
                     (int)ora_code);
        return 0;
    }

    if (oracle_commit_is_retryable(ora_code))
        return DB_TX_RETRYABLE;

    if (oracle_commit_outcome_unknown(ora_code))
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "ORA-%05d: connection lost during COMMIT - the "
                     "outcome is unknown (the commit may or may not have "
                     "reached the server); not retried",
                     (int)ora_code);
    else
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "ORA-%05d: COMMIT failed and is not retryable "
                     "(the transaction may already have been rolled back "
                     "by the server)", (int)ora_code);

    return -1;
}

static int oracle_rollback(oci_context_t *ctx, logger_t *logger)
{
    if (!ctx) return -1;

    sword status = OCITransRollback(ctx->svchp, ctx->errhp, OCI_DEFAULT);
    if (status != OCI_SUCCESS && status != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, status);
        return -1;
    }

    logger_write(logger, LOG_INFO, __func__, 0, "OCITransRollback OK");
    return 0;
}

/* ================================================================
 * DML EXECUTE RETURNING ROWIDS / LOB WRITE BY ROWID (2026-09-18) -
 * UPDATE's own abstraction pass (see db_driver.h's own doc comments
 * for the full design reasoning - why this can't just be dml_execute()
 * again, the rows_affected fix, the BLOB/CLOB asymmetry decision).
 * Mirrors OCI_Update_Execute_Module.c's own dynamic_rowid_collector_t/
 * rowid_in_callback()/rowid_out_callback()/handle_blob_update()/
 * handle_clob_update() exactly - same OCI functions, same sequence,
 * just behind the driver boundary instead of inline in that module.
 * ================================================================ */

#define ORACLE_ROWID_BUF_SIZE 24   /* matches OCI_Update_Execute_Module.c's
                                       own ROWID_BUF_SIZE - kept as this
                                       driver's own copy rather than
                                       shared across the core/driver
                                       boundary, see db_rowid_result_t's
                                       own doc comment in db_driver.h    */

typedef struct {
    char           *bufs;
    sb2            *inds;
    ub2            *rcodes;
    ub4             alen;
    ub4             count;
    ub4             capacity;
    oci_context_t  *ctx;
    logger_t       *logger;
} oracle_rowid_collector_t;

static void oracle_rowid_collector_init(oracle_rowid_collector_t *c,
                                         oci_context_t *ctx, logger_t *logger)
{
    memset(c, 0, sizeof(*c));
    c->ctx    = ctx;
    c->logger = logger;
}

static void oracle_rowid_collector_free(oracle_rowid_collector_t *c)
{
    free(c->bufs);
    free(c->inds);
    free(c->rcodes);
    memset(c, 0, sizeof(*c));
}

static int oracle_rowid_collector_ensure(oracle_rowid_collector_t *c, ub4 index)
{
    if (index < c->capacity) return 1;

    ub4 new_capacity = c->capacity == 0 ? 16 : c->capacity * 2;
    while (new_capacity <= index) new_capacity *= 2;

    char *new_bufs   = realloc(c->bufs,   (size_t)new_capacity * ORACLE_ROWID_BUF_SIZE);
    sb2  *new_inds   = realloc(c->inds,   (size_t)new_capacity * sizeof(sb2));
    ub2  *new_rcodes = realloc(c->rcodes, (size_t)new_capacity * sizeof(ub2));
    if (!new_bufs || !new_inds || !new_rcodes)
    {
        logger_write(c->logger, LOG_ERROR, __func__, 0,
                     "oracle_rowid_collector: realloc failed growing to "
                     "%u rows", new_capacity);
        if (new_bufs)   c->bufs   = new_bufs;
        if (new_inds)   c->inds   = new_inds;
        if (new_rcodes) c->rcodes = new_rcodes;
        return 0;
    }

    memset(new_bufs + (size_t)c->capacity * ORACLE_ROWID_BUF_SIZE, 0,
           (size_t)(new_capacity - c->capacity) * ORACLE_ROWID_BUF_SIZE);

    c->bufs     = new_bufs;
    c->inds     = new_inds;
    c->rcodes   = new_rcodes;
    c->capacity = new_capacity;
    return 1;
}

static sb4 oracle_rowid_in_callback(void *ictxp, OCIBind *bindp, ub4 iter,
                                     ub4 index, void **bufpp, ub4 *alenp,
                                     ub1 *piecep, void **indpp)
{
    (void)ictxp; (void)bindp; (void)iter; (void)index;
    static const char empty = '\0';
    *bufpp  = (void *)&empty;
    *alenp  = 0;
    *piecep = OCI_ONE_PIECE;
    *indpp  = NULL;
    return OCI_CONTINUE;
}

static sb4 oracle_rowid_out_callback(void *octxp, OCIBind *bindp,
                                      ub4 iter, ub4 index,
                                      void **outbufpp, ub4 **alenpp,
                                      ub1 *piecep, void **indpp, ub2 **rcodepp)
{
    oracle_rowid_collector_t *c = (oracle_rowid_collector_t *)octxp;
    (void)bindp; (void)iter;

    if (!oracle_rowid_collector_ensure(c, index))
    {
        *outbufpp = NULL;
        return OCI_ERROR;
    }

    c->alen   = (ub4)ORACLE_ROWID_BUF_SIZE;
    *outbufpp = c->bufs + (size_t)index * ORACLE_ROWID_BUF_SIZE;
    *alenpp   = &c->alen;
    *piecep   = OCI_ONE_PIECE;
    *indpp    = &c->inds[index];
    *rcodepp  = &c->rcodes[index];

    if (index + 1 > c->count) c->count = index + 1;

    return OCI_CONTINUE;
}

static void oracle_free_dml_returning_binds(OCIBind **bind_hdls,
                                             sb2 *null_inds,
                                             char **array_bufs,
                                             sb2 **array_inds,
                                             int bind_count)
{
    free(bind_hdls);
    free(null_inds);
    if (array_bufs || array_inds)
    {
        for (int j = 0; j < bind_count; j++)
        {
            if (array_bufs) free(array_bufs[j]);
            if (array_inds) free(array_inds[j]);
        }
        free(array_bufs);
        free(array_inds);
    }
}

static int oracle_dml_execute_returning_rowids(
                         oci_context_t                     *ctx,
                         logger_t                           *logger,
                         const db_dml_returning_request_t   *req,
                         db_rowid_result_t                  *out_result)
{
    if (!ctx || !req || !req->sql || !out_result) return -1;
    if (req->bind_count > 0 && !req->bind_values) return -1;

    memset(out_result, 0, sizeof(*out_result));

    OCIStmt *stmt = NULL;

    sword prepare_rc = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                                        (text *)req->sql,
                                        (ub4)strlen(req->sql),
                                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (prepare_rc != OCI_SUCCESS && prepare_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, prepare_rc);
        return -1;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "OCIStmtPrepare2 OK bind_count=%d returning_pos=%d "
                 "row_count=%d",
                 req->bind_count, req->returning_bind_position,
                 req->row_count);

    /* iters is what OCIStmtExecute actually runs - req->row_count <= 1
     * means "today's DELETE/UPDATE shape", always exactly 1 iteration,
     * unchanged since 2026-09-18. row_count > 1 is INSERT's own real
     * batch (added 2026-09-20) - see db_dml_returning_request_t's own
     * doc comment in db_driver.h for the full reasoning on why the
     * bind SHAPE differs below but the ROWID collector doesn't need to
     * change at all. */
    int iters = (req->row_count > 1) ? req->row_count : 1;

    OCIBind **bind_hdls = NULL;
    sb2      *null_inds = NULL;   /* only used when iters == 1 - see
                                      oracle_dml_execute()'s own comment
                                      on this same pattern            */
    /* Only used when iters > 1 - one flat, row-major buffer and one
     * indicator array per bind position, freed after execute (must
     * stay valid until OCIStmtExecute actually reads them, same
     * lifetime trap null_inds above already had to be built around).  */
    char **array_bufs = NULL;
    sb2   **array_inds = NULL;

    if (req->bind_count > 0)
    {
        bind_hdls = calloc((size_t)req->bind_count, sizeof(OCIBind *));
        if (iters == 1)
            null_inds = calloc((size_t)req->bind_count, sizeof(sb2));
        else
        {
            array_bufs = calloc((size_t)req->bind_count, sizeof(char *));
            array_inds = calloc((size_t)req->bind_count, sizeof(sb2 *));
        }

        if (!bind_hdls || (iters == 1 && !null_inds) ||
            (iters > 1 && (!array_bufs || !array_inds)))
        {
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "calloc failed for bind structures (bind_count=%d "
                         "iters=%d)", req->bind_count, iters);
            oracle_free_dml_returning_binds(bind_hdls, null_inds,
                                             array_bufs, array_inds,
                                             req->bind_count);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
            return -1;
        }
    }

    if (iters == 1)
    {
        /* Today's DELETE/UPDATE shape, unchanged since 2026-09-18 - one
         * value per bind position. */
        for (int k = 0; k < req->bind_count; k++)
        {
            const char *value = req->bind_values[k];
            if (!value) null_inds[k] = -1;

            sword bind_rc = OCIBindByPos(stmt, &bind_hdls[k], ctx->errhp,
                                          (ub4)(k + 1),
                                          value ? (void *)value : NULL,
                                          value ? (sb4)(strlen(value) + 1) : 0,
                                          SQLT_STR,
                                          value ? NULL : &null_inds[k],
                                          NULL, NULL, 0, NULL,
                                          OCI_DEFAULT);
            if (bind_rc != OCI_SUCCESS && bind_rc != OCI_SUCCESS_WITH_INFO)
            {
                ORACLE_CHECK_OCI_LOG(ctx, logger, bind_rc);
                oracle_free_dml_returning_binds(bind_hdls, null_inds,
                                                 array_bufs, array_inds,
                                                 req->bind_count);
                OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
                return -1;
            }
        }
    }
    else
    {
        /* INSERT's own real multi-row batch (added 2026-09-20) - each
         * bind position gets a genuine array of row_count distinct
         * values (OCIBindArrayOfStruct), read from req->bind_values'
         * flat, row-major layout - see db_dml_returning_request_t's
         * own doc comment in db_driver.h for that layout's exact
         * shape. OCIBindArrayOfStruct needs a uniform stride across
         * every row for a given column - computed here as the longest
         * value actually present for that column, not a blanket
         * worst-case size, to avoid wasting memory on every other
         * column when only one happens to hold a long value. Matches
         * OCI_Insert_Execute_Module.c's own bind loop in spirit (same
         * OCIBindByPos + OCIBindArrayOfStruct pair, same skip-for-
         * empty/NULL handling) even though that module computes its
         * own stride from real column metadata rather than the data
         * itself - the driver has no metadata to consult, only the
         * values it was actually handed, and sizing from those is
         * exactly as correct for what OCI itself requires (a stride
         * long enough for every row's value, nothing more). */
        for (int k = 0; k < req->bind_count; k++)
        {
            size_t max_len = 0;
            for (int r = 0; r < req->row_count; r++)
            {
                const char *v = req->bind_values[(size_t)r * req->bind_count + k];
                if (v)
                {
                    size_t l = strlen(v);
                    if (l > max_len) max_len = l;
                }
            }
            size_t stride = max_len + 1;   /* +1 for NUL terminator */

            array_bufs[k] = calloc((size_t)req->row_count, stride);
            array_inds[k] = calloc((size_t)req->row_count, sizeof(sb2));
            if (!array_bufs[k] || !array_inds[k])
            {
                logger_write(logger, LOG_ERROR, __func__, 0,
                             "calloc failed for array bind col=%d "
                             "row_count=%d stride=%zu",
                             k, req->row_count, stride);
                oracle_free_dml_returning_binds(bind_hdls, null_inds,
                                                 array_bufs, array_inds,
                                                 req->bind_count);
                OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
                return -1;
            }

            for (int r = 0; r < req->row_count; r++)
            {
                const char *v = req->bind_values[(size_t)r * req->bind_count + k];
                char *slot = array_bufs[k] + (size_t)r * stride;
                if (v)
                {
                    strncpy(slot, v, stride - 1);
                    array_inds[k][r] = 0;
                }
                else
                {
                    slot[0] = '\0';
                    array_inds[k][r] = -1;
                }
            }

            sword bind_rc = OCIBindByPos(stmt, &bind_hdls[k], ctx->errhp,
                                          (ub4)(k + 1),
                                          array_bufs[k],
                                          (sb4)stride,
                                          SQLT_STR,
                                          array_inds[k],
                                          NULL, NULL, 0, NULL,
                                          OCI_DEFAULT);
            if (bind_rc == OCI_SUCCESS || bind_rc == OCI_SUCCESS_WITH_INFO)
                bind_rc = OCIBindArrayOfStruct(bind_hdls[k], ctx->errhp,
                                                (ub4)stride,
                                                (ub4)sizeof(sb2),
                                                0, 0);
            if (bind_rc != OCI_SUCCESS && bind_rc != OCI_SUCCESS_WITH_INFO)
            {
                ORACLE_CHECK_OCI_LOG(ctx, logger, bind_rc);
                oracle_free_dml_returning_binds(bind_hdls, null_inds,
                                                 array_bufs, array_inds,
                                                 req->bind_count);
                OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
                return -1;
            }
        }
    }

    /* RETURNING ROWID INTO. Two genuinely different mechanisms, chosen
     * deliberately by iters rather than one mechanism stretched to fit
     * both - a design correction (2026-09-20) from the original
     * intent of reusing one proven mechanism everywhere (see
     * db_dml_returning_request_t's own doc comment in db_driver.h,
     * left in place as the honest history of that decision and why it
     * changed).
     *
     * iters == 1 (DELETE/UPDATE's own shape, unchanged since
     * 2026-09-18) - a dynamic bind (OCIBindDynamic), because the WHERE
     * clause can match an unknown number of physical rows at execute
     * time; a static, single-slot bind could only ever receive one of
     * them back.
     *
     * iters > 1 (INSERT's own real batch, added 2026-09-20) - a STATIC
     * array (OCIBindByPos + OCIBindArrayOfStruct on a calloc'd flat
     * buffer, sized to iters exactly), not the dynamic collector.
     * Tried the dynamic mechanism here first, reasoning that its
     * per-iteration callback should generalize to any iters count the
     * same way the static array binds already do - that reasoning
     * turned out wrong: real testing (Driver_Insert_Test.c's own Test
     * 2/3/6, with diagnostics added specifically to pin this down)
     * showed the array insert itself always succeeded correctly (every
     * row landed, confirmed via an independent direct COUNT), but the
     * dynamic callback only ever fired once regardless of the real
     * iters value - a genuine, unresolved OCI-level limitation for
     * this specific combination, not a bug in the surrounding bind
     * logic. Rather than keep debugging an uncertain mechanism, this
     * uses the SAME static-array approach OCI_Insert_Execute_Module.c's
     * own real code already proves works for exactly this shape
     * (row count known in advance, not discovered at execute time) -
     * a known-correct path, not a second guess. */
    oracle_rowid_collector_t collector;
    char  *static_rowid_bufs = NULL;
    sb2   *static_rowid_inds = NULL;
    OCIBind *rowid_bind_hdl  = NULL;
    sword rowid_bind_rc, dynamic_or_array_rc;

    if (iters == 1)
    {
        oracle_rowid_collector_init(&collector, ctx, logger);

        rowid_bind_rc = OCIBindByPos(stmt, &rowid_bind_hdl, ctx->errhp,
                                      (ub4)req->returning_bind_position,
                                      NULL,
                                      (sb4)ORACLE_ROWID_BUF_SIZE,
                                      SQLT_STR,
                                      NULL,
                                      NULL, NULL, 0, NULL,
                                      OCI_DATA_AT_EXEC);
        if (rowid_bind_rc == OCI_SUCCESS || rowid_bind_rc == OCI_SUCCESS_WITH_INFO)
            dynamic_or_array_rc = OCIBindDynamic(
                rowid_bind_hdl, ctx->errhp,
                (void *)&collector, oracle_rowid_in_callback,
                (void *)&collector, oracle_rowid_out_callback);
        else
            dynamic_or_array_rc = rowid_bind_rc;
    }
    else
    {
        static_rowid_bufs = calloc((size_t)iters, ORACLE_ROWID_BUF_SIZE);
        static_rowid_inds = calloc((size_t)iters, sizeof(sb2));
        if (!static_rowid_bufs || !static_rowid_inds)
        {
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "calloc failed for static_rowid_bufs/inds (iters=%d)",
                         iters);
            free(static_rowid_bufs);
            free(static_rowid_inds);
            oracle_free_dml_returning_binds(bind_hdls, null_inds,
                                             array_bufs, array_inds,
                                             req->bind_count);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
            return -1;
        }

        rowid_bind_rc = OCIBindByPos(stmt, &rowid_bind_hdl, ctx->errhp,
                                      (ub4)req->returning_bind_position,
                                      static_rowid_bufs,
                                      (sb4)ORACLE_ROWID_BUF_SIZE,
                                      SQLT_STR,
                                      static_rowid_inds,
                                      NULL, NULL, 0, NULL,
                                      OCI_DEFAULT);
        if (rowid_bind_rc == OCI_SUCCESS || rowid_bind_rc == OCI_SUCCESS_WITH_INFO)
            dynamic_or_array_rc = OCIBindArrayOfStruct(
                rowid_bind_hdl, ctx->errhp,
                (ub4)ORACLE_ROWID_BUF_SIZE, (ub4)sizeof(sb2), 0, 0);
        else
            dynamic_or_array_rc = rowid_bind_rc;
    }

    if ((rowid_bind_rc != OCI_SUCCESS && rowid_bind_rc != OCI_SUCCESS_WITH_INFO) ||
        (dynamic_or_array_rc != OCI_SUCCESS && dynamic_or_array_rc != OCI_SUCCESS_WITH_INFO))
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger,
            rowid_bind_rc != OCI_SUCCESS && rowid_bind_rc != OCI_SUCCESS_WITH_INFO
                ? rowid_bind_rc : dynamic_or_array_rc);
        free(static_rowid_bufs);
        free(static_rowid_inds);
        oracle_free_dml_returning_binds(bind_hdls, null_inds,
                                         array_bufs, array_inds,
                                         req->bind_count);
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Calling OCIStmtExecute iters=%d", iters);

    sword exec_rc = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                                    (ub4)iters, 0, NULL, NULL, OCI_DEFAULT);
    if (exec_rc != OCI_SUCCESS && exec_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, exec_rc);
        free(static_rowid_bufs);
        free(static_rowid_inds);
        oracle_free_dml_returning_binds(bind_hdls, null_inds,
                                         array_bufs, array_inds,
                                         req->bind_count);
        if (iters == 1) oracle_rowid_collector_free(&collector);
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    if (iters == 1)
    {
        logger_write(logger, LOG_INFO, __func__, 0,
                     "OCIStmtExecute OK rows_reported=%u (RETURNING ROWID "
                     "via OCIBindDynamic)", collector.count);

        /* Ownership of the rowid buffer transfers to out_result -
         * inds/rcodes are not part of db_rowid_result_t's own contract
         * (core never inspected them even before this abstraction -
         * see dynamic_rowid_collector_t's own comment in
         * OCI_Update_Execute_Module.c), freed here rather than exposed. */
        out_result->rowids = collector.bufs;
        out_result->count  = (int)collector.count;
        collector.bufs = NULL;   /* ownership transferred, don't free below */
        free(collector.inds);
        free(collector.rcodes);
    }
    else
    {
        /* A successful array-bound INSERT of iters rows always
         * produces exactly iters ROWIDs - unlike UPDATE's WHERE clause,
         * there is no "how many actually happened" discovery needed
         * here; the row count IS the batch size, by definition. */
        logger_write(logger, LOG_INFO, __func__, 0,
                     "OCIStmtExecute OK rows_reported=%d (RETURNING ROWID "
                     "via static array bind)", iters);

        out_result->rowids = static_rowid_bufs;
        out_result->count  = iters;
        static_rowid_bufs = NULL;   /* ownership transferred - static_rowid_inds
                                        is NOT transferred, freed by the
                                        unconditional cleanup right below,
                                        same as every other path through
                                        this function - freeing it here
                                        too was the double-free (found via
                                        Driver_Insert_Test.c's own Test 2,
                                        caught by ASan immediately rather
                                        than silently corrupting memory). */
    }

    free(static_rowid_bufs);
    free(static_rowid_inds);
    oracle_free_dml_returning_binds(bind_hdls, null_inds,
                                     array_bufs, array_inds,
                                     req->bind_count);
    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return 0;
}

static void oracle_rowid_result_free(db_rowid_result_t *result)
{
    if (!result) return;
    free(result->rowids);
    result->rowids = NULL;
    result->count  = 0;
}

static int oracle_lob_write_by_rowid(
                         oci_context_t                  *ctx,
                         logger_t                        *logger,
                         const db_lob_write_request_t   *req,
                         uint64_t                        *out_bytes_written)
{
    if (!ctx || !req || !req->table_fq || !req->column_name ||
        !req->rowid_str || !out_bytes_written)
        return -1;

    *out_bytes_written = 0;

    /* Matches handle_blob_update()/handle_clob_update()'s own is_empty
     * short-circuit - a no-op success, not an error. */
    if (req->is_blob && !req->file_path)
        return 0;
    if (!req->is_blob && (!req->inline_text || req->inline_text_len == 0))
        return 0;

    char sql_sel[512];
    snprintf(sql_sel, sizeof(sql_sel),
             "SELECT %s FROM %s WHERE ROWID = :rid FOR UPDATE",
             req->column_name, req->table_fq);

    OCIStmt *stmt_sel = NULL;
    sword prepare_rc = OCIStmtPrepare2(ctx->svchp, &stmt_sel, ctx->errhp,
                                        (text *)sql_sel, (ub4)strlen(sql_sel),
                                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (prepare_rc != OCI_SUCCESS && prepare_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, prepare_rc);
        return -1;
    }

    OCIBind *bind_rid = NULL;
    sword bind_rc = OCIBindByName(stmt_sel, &bind_rid, ctx->errhp,
                                   (text *)":rid", -1,
                                   (dvoid *)req->rowid_str,
                                   (sb4)(strlen(req->rowid_str) + 1),
                                   SQLT_STR, NULL, NULL, NULL, 0, NULL,
                                   OCI_DEFAULT);
    if (bind_rc != OCI_SUCCESS && bind_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, bind_rc);
        OCIStmtRelease(stmt_sel, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    OCILobLocator *lob_loc = NULL;
    sword desc_rc = OCIDescriptorAlloc(ctx->envhp, (void **)&lob_loc,
                                        OCI_DTYPE_LOB, 0, NULL);
    if (desc_rc != OCI_SUCCESS && desc_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, desc_rc);
        OCIStmtRelease(stmt_sel, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    OCIDefine *def_lob = NULL;
    sword define_rc = OCIDefineByPos(stmt_sel, &def_lob, ctx->errhp, 1,
                                      &lob_loc,
                                      (sb4)sizeof(OCILobLocator *),
                                      req->is_blob ? SQLT_BLOB : SQLT_CLOB,
                                      NULL, NULL, NULL, OCI_DEFAULT);
    if (define_rc != OCI_SUCCESS && define_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, define_rc);
        OCIDescriptorFree(lob_loc, OCI_DTYPE_LOB);
        OCIStmtRelease(stmt_sel, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    sword sel_exec_rc = OCIStmtExecute(ctx->svchp, stmt_sel, ctx->errhp,
                                        0, 0, NULL, NULL, OCI_DEFAULT);
    if (sel_exec_rc != OCI_SUCCESS && sel_exec_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, sel_exec_rc);
        OCIDescriptorFree(lob_loc, OCI_DTYPE_LOB);
        OCIStmtRelease(stmt_sel, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    sword fetch_rc = OCIStmtFetch2(stmt_sel, ctx->errhp,
                                    1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
    if (fetch_rc != OCI_SUCCESS && fetch_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, fetch_rc);
        OCIDescriptorFree(lob_loc, OCI_DTYPE_LOB);
        OCIStmtRelease(stmt_sel, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Persistent %s locator obtained - writing",
                 req->is_blob ? "BLOB" : "CLOB");

    int rc = 0;

    if (req->is_blob)
    {
        /* BLOB: always streamed from file_path, chunk by chunk, never
         * buffered whole - matches handle_blob_update() exactly, and
         * the explicit decision (2026-09-18) not to unify this with
         * CLOB's approach - see db_lob_write_request_t's own doc
         * comment in db_driver.h. */
        FILE *fp = fopen(req->file_path, "rb");
        if (!fp)
        {
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "Failed to open BLOB file: %s", req->file_path);
            rc = -1;
            goto Cleanup;
        }

        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (file_size <= 0)
        {
            fclose(fp);
            goto Cleanup;
        }

        ub1 *chunk_buf = malloc(ctx->ini->chunk_read_size);
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

            sword write_rc = OCILobWrite(ctx->svchp, ctx->errhp,
                                          lob_loc, &amount, offset,
                                          chunk_buf, (ub4)nread,
                                          OCI_ONE_PIECE,
                                          NULL, NULL, 0, SQLCS_IMPLICIT);
            if (write_rc != OCI_SUCCESS && write_rc != OCI_SUCCESS_WITH_INFO)
            {
                ORACLE_CHECK_OCI_LOG(ctx, logger, write_rc);
                free(chunk_buf);
                fclose(fp);
                rc = -1;
                goto Cleanup;
            }

            offset          += (ub4)nread;
            bytes_remaining -= nread;
        }

        free(chunk_buf);
        fclose(fp);

        *out_bytes_written = (uint64_t)file_size;
        logger_write(logger, LOG_INFO, __func__, 0,
                     "BLOB write complete size=%ld", file_size);
    }
    else
    {
        /* CLOB: always written from an already-resolved in-memory
         * buffer (inline_text/inline_text_len) - core has already done
         * whatever file read or literal-value resolution was needed,
         * matching handle_clob_update() exactly. */
        ub4    offset          = 1;
        size_t bytes_remaining = req->inline_text_len;

        while (bytes_remaining > 0)
        {
            size_t chunk = ctx->ini->chunk_read_size;
            if (chunk > bytes_remaining) chunk = bytes_remaining;

            ub4 amount = (ub4)chunk;

            sword write_rc = OCILobWrite(
                ctx->svchp, ctx->errhp, lob_loc, &amount, offset,
                (dvoid *)(req->inline_text +
                          (req->inline_text_len - bytes_remaining)),
                (ub4)chunk, OCI_ONE_PIECE,
                NULL, NULL, 0, SQLCS_IMPLICIT);
            if (write_rc != OCI_SUCCESS && write_rc != OCI_SUCCESS_WITH_INFO)
            {
                ORACLE_CHECK_OCI_LOG(ctx, logger, write_rc);
                rc = -1;
                goto Cleanup;
            }

            offset          += (ub4)chunk;
            bytes_remaining -= chunk;
        }

        *out_bytes_written = (uint64_t)req->inline_text_len;
        logger_write(logger, LOG_INFO, __func__, 0,
                     "CLOB write complete total=%zu", req->inline_text_len);
    }

Cleanup:
    OCIDescriptorFree(lob_loc, OCI_DTYPE_LOB);
    OCIStmtRelease(stmt_sel, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return rc;
}

/* ================================================================
 * DML EXECUTE PROCEDURE (2026-09-21) - EXECUTE_PROCEDURE's own
 * abstraction pass (see db_driver.h's own doc comments for
 * dml_execute_procedure()/db_proc_param_t for the full design
 * reasoning - named, mixed-type, two-way binds, nothing already built
 * fits this shape). Mirrors OCI_Execute_Procedure_Module.c's own
 * bind_parameters() exactly - same three-way SQLT_RSET/SQLT_INT/
 * SQLT_STR dispatch, same by-name binding, same "copy IN value into
 * the buffer OCI will overwrite for OUT/IN_OUT" pattern - just behind
 * the driver boundary instead of inline in that module.
 * ================================================================ */
static int oracle_dml_execute_procedure(oci_context_t               *ctx,
                                         logger_t                    *logger,
                                         db_proc_execute_request_t   *req)
{
    if (!ctx || !req || !req->plsql_block) return -1;
    if (req->param_count > 0 && !req->params) return -1;

    OCIStmt *stmt = NULL;

    sword prepare_rc = OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                                        (text *)req->plsql_block,
                                        (ub4)strlen(req->plsql_block),
                                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (prepare_rc != OCI_SUCCESS && prepare_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, prepare_rc);
        return -1;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "OCIStmtPrepare2 OK param_count=%d", req->param_count);

    /* Indicators - one sb2 per param, persisting until OCIStmtExecute
     * actually reads them. A loop-local sb2 here would repeat the exact
     * bind-lifetime bug already found and fixed once in this project
     * (oracle_dml_execute()'s own first NULL-bind attempt) - caught
     * this time before it was ever built, not after. */
    sb2 *indicators = NULL;
    if (req->param_count > 0)
    {
        indicators = calloc((size_t)req->param_count, sizeof(sb2));
        if (!indicators)
        {
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "calloc failed for indicators (param_count=%d)",
                         req->param_count);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
            return -1;
        }
    }

    for (int i = 0; i < req->param_count; i++)
    {
        db_proc_param_t *p = &req->params[i];
        OCIBind         *bind_hdl = NULL;

        char bind_name[132];
        snprintf(bind_name, sizeof(bind_name), ":%s", p->name);

        sword bind_rc;

        if (p->type == DB_PROC_TYPE_CURSOR)
        {
            OCIStmt *cursor_stmt = NULL;

            sword alloc_rc = OCIHandleAlloc(ctx->envhp, (void **)&cursor_stmt,
                                             OCI_HTYPE_STMT, 0, NULL);
            if (alloc_rc != OCI_SUCCESS && alloc_rc != OCI_SUCCESS_WITH_INFO)
            {
                ORACLE_CHECK_OCI_LOG(ctx, logger, alloc_rc);
                free(indicators);
                OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
                return -1;
            }

            bind_rc = OCIBindByName(stmt, &bind_hdl, ctx->errhp,
                                     (text *)bind_name, -1,
                                     &cursor_stmt, (sb4)sizeof(OCIStmt *),
                                     SQLT_RSET, &indicators[i],
                                     NULL, NULL, 0, NULL, OCI_DEFAULT);

            if (bind_rc != OCI_SUCCESS && bind_rc != OCI_SUCCESS_WITH_INFO)
            {
                /* Bind itself failed - cursor_stmt was already
                 * allocated above and would otherwise leak here; the
                 * shared bind_rc check below can't reach it (out of
                 * scope), so it's freed right here instead. Found
                 * while reviewing this branch, not by a test - a real,
                 * if unlikely (bind failures are rare), leak on a path
                 * nothing would have exercised without a genuine OCI
                 * failure to trigger it. */
                OCIHandleFree(cursor_stmt, OCI_HTYPE_STMT);
                ORACLE_CHECK_OCI_LOG(ctx, logger, bind_rc);
                free(indicators);
                OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
                return -1;
            }

            /* Stashed now, corrected after execute below if the
             * indicator turns out to mean the cursor was never opened
             * (indicators[i]==-1 - see UNIT_TEST_CURSOR_PROC's own
             * deliberately-conditional OPEN). bind_rc is guaranteed
             * success here - the failure case already returned above. */
            p->out_cursor_handle = (void *)cursor_stmt;

            logger_write(logger, LOG_DEBUG, __func__, 0,
                         "CURSOR bind param='%s'", p->name);
        }
        else if (p->type == DB_PROC_TYPE_INT &&
                 (p->direction == DB_PROC_DIR_OUT ||
                  p->direction == DB_PROC_DIR_IN_OUT))
        {
            if (p->direction == DB_PROC_DIR_IN_OUT && p->in_value)
                p->out_int = atoi(p->in_value);

            bind_rc = OCIBindByName(stmt, &bind_hdl, ctx->errhp,
                                     (text *)bind_name, -1,
                                     &p->out_int, (sb4)sizeof(int),
                                     SQLT_INT, &indicators[i],
                                     NULL, NULL, 0, NULL, OCI_DEFAULT);

            logger_write(logger, LOG_DEBUG, __func__, 0,
                         "INTEGER OUT bind param='%s'", p->name);
        }
        else
        {
            /* Scalar IN / OUT / IN_OUT, and INT-typed pure IN too
             * (matches bind_parameters()'s own fallthrough - a pure IN
             * NUMBER/INTEGER value is still bound as SQLT_STR there,
             * only OUT/IN_OUT INT gets the SQLT_INT path). Copy the IN
             * value into out_value - the same buffer OCI will overwrite
             * for OUT/IN_OUT - matching bind_parameters()'s own
             * approach exactly; a pure OUT param's out_value simply
             * starts empty. */
            if (!p->out_value || p->out_value_size == 0)
            {
                logger_write(logger, LOG_ERROR, __func__, 0,
                             "param '%s' is STR-typed but out_value/"
                             "out_value_size not provided", p->name);
                free(indicators);
                OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
                return -1;
            }

            if (p->in_value)
            {
                strncpy(p->out_value, p->in_value, p->out_value_size - 1);
                p->out_value[p->out_value_size - 1] = '\0';
            }
            else
            {
                p->out_value[0] = '\0';
            }

            if (p->direction == DB_PROC_DIR_IN && strlen(p->out_value) == 0)
                indicators[i] = -1;
            else
                indicators[i] = 0;

            bind_rc = OCIBindByName(stmt, &bind_hdl, ctx->errhp,
                                     (text *)bind_name, -1,
                                     p->out_value, (sb4)p->out_value_size,
                                     SQLT_STR, &indicators[i],
                                     NULL, NULL, 0, NULL, OCI_DEFAULT);

            logger_write(logger, LOG_DEBUG, __func__, 0,
                         "Scalar bind param='%s' value='%s' indicator=%d",
                         p->name, p->out_value, indicators[i]);
        }

        if (bind_rc != OCI_SUCCESS && bind_rc != OCI_SUCCESS_WITH_INFO)
        {
            ORACLE_CHECK_OCI_LOG(ctx, logger, bind_rc);
            free(indicators);
            OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
            return -1;
        }
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Calling OCIStmtExecute iters=1 (PL/SQL anonymous block)");

    sword exec_rc = OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                                    1, 0, NULL, NULL, OCI_DEFAULT);
    if (exec_rc != OCI_SUCCESS && exec_rc != OCI_SUCCESS_WITH_INFO)
    {
        ORACLE_CHECK_OCI_LOG(ctx, logger, exec_rc);
        free(indicators);
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        return -1;
    }

    logger_write(logger, LOG_INFO, __func__, 0, "PL/SQL execute OK");

    /* Collect scalar OUT/IN_OUT values and finalise CURSOR handles - now
     * that execute has actually run and indicators[] holds real,
     * meaningful values. out_int/out_value were already written
     * directly into by OCI itself during execute (bound straight to
     * &p->out_int / p->out_value above) - nothing further to copy for
     * those; only out_is_null (every type) and out_cursor_handle
     * (CURSOR only, when genuinely never opened) need setting here. */
    for (int i = 0; i < req->param_count; i++)
    {
        db_proc_param_t *p = &req->params[i];
        int is_null = (indicators[i] == -1);

        if (p->type == DB_PROC_TYPE_CURSOR)
        {
            if (is_null && p->out_cursor_handle)
            {
                /* Bound successfully, but the procedure never actually
                 * opened it (see UNIT_TEST_CURSOR_PROC) - free the
                 * handle allocated above rather than leak it, and
                 * report NULL, matching bind_parameters()'s own
                 * "if (!p->cursor_stmt) skip" / "if (indicator==-1)
                 * emit empty resultset" handling. */
                OCIHandleFree(p->out_cursor_handle, OCI_HTYPE_STMT);
                p->out_cursor_handle = NULL;
            }
            p->out_is_null = is_null;
            continue;
        }

        if (p->direction != DB_PROC_DIR_OUT &&
            p->direction != DB_PROC_DIR_IN_OUT) continue;

        p->out_is_null = is_null;

        logger_write(logger, LOG_INFO, __func__, 0,
                     "OUT param '%s' is_null=%d", p->name, is_null);
    }

    free(indicators);
    OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    return 0;
}

/* ================================================================
 * TABLE DESCRIBE (Stage 4a, 2026-10-06) - db_driver_t.describe_table.
 *
 * oracle_describe_table() was get_request_metadata() in
 * Table_Metadata_Module.c, moved here verbatim (only the name changed).
 * The three statements it runs are byte-for-byte the same:
 *   - SELECT OWNER FROM ALL_TABLES ... ROWNUM = 1   (owner not given)
 *   - SELECT DATA_DEFAULT FROM ALL_TAB_COLUMNS ...  (per column; LONG)
 *   - SELECT COLUMN_NAME, DATA_TYPE, ... ORDER BY COLUMN_ID
 * Log messages keep their old text (including "get_request_metadata
 * OK") so the Stage 4a log comparison matches them line for line; only
 * the function name the logger stamps differs.
 *
 * Known, deliberately unchanged in 4a (open items):
 *   - the owner lookup takes ROWNUM = 1 from ALL_TABLES, so a table
 *     name that exists in several visible schemas resolves to an
 *     arbitrary one;
 *   - DATA_DEFAULT costs one round trip per column on a cache miss.
 * ================================================================ */

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

static int oracle_describe_table(oci_context_t      *ctx,
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

/* ================================================================
 * DIALECT (Oracle dialect extraction, Stage 2, 2026-10-02)
 *
 * Every Oracle-specific SQL fragment core used to write itself - see
 * db_dialect_t in db_driver.h for the contract of each hook. Each one
 * reproduces the text core produced before Stage 2 byte for byte (the
 * Stage 2 proof is that every logged statement is unchanged), with one
 * deliberate difference: date/timestamp conversion uses a FIXED ISO
 * mask instead of reading ctx->ini->nls_date_format. Level 2 always
 * hands the driver canonical ISO values (date_normalize.c, Stage 1),
 * so the mask that reads them must be fixed too; nls_date_format is
 * now only the session's display format (ALTER SESSION in
 * Connection_Pool.c), which decides how dates look in SELECT results.
 * With nls_date_format at its deployed value ('YYYY-MM-DD HH24:MI:SS',
 * Stage 0 check 0.3) the generated text is identical.
 * ================================================================ */

#define ORACLE_ISO_DATE_MASK  "YYYY-MM-DD HH24:MI:SS"
#define ORACLE_ISO_TS_MASK    "YYYY-MM-DD HH24:MI:SS.FF6"

/* snprintf into out; 0 on success, -1 on NULL/overflow. */
static int oracle_fmt(char *out, size_t out_max, const char *fmt, ...)
{
    if (!out || out_max == 0) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, out_max, fmt, ap);
    va_end(ap);
    return (n < 0 || (size_t)n >= out_max) ? -1 : 0;
}

static int oracle_bind_placeholder(int pos, char *out, size_t out_max)
{
    if (pos < 1) return -1;
    return oracle_fmt(out, out_max, ":%d", pos);
}

/* Same type tests, in the same order, as the four wrappers this
 * replaces (get_bind_wrapper / get_upd_bind_wrapper /
 * get_del_key_wrapper / audit_get_date_wrapper). */
static int oracle_value_expr(const char *dtype, const char *operand,
                             char *out, size_t out_max)
{
    if (!operand) return -1;
    if (!dtype) dtype = "";

    if (strcmp(dtype, "DATE") == 0)
        return oracle_fmt(out, out_max, "TO_DATE(%s,'%s')",
                          operand, ORACLE_ISO_DATE_MASK);
    if (strncmp(dtype, "TIMESTAMP", 9) == 0)
        return oracle_fmt(out, out_max, "TO_TIMESTAMP(%s,'%s')",
                          operand, ORACLE_ISO_TS_MASK);
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "MONTH"))
        return oracle_fmt(out, out_max, "TO_YMINTERVAL(%s)", operand);
    if (strstr(dtype, "INTERVAL") && strstr(dtype, "SECOND"))
        return oracle_fmt(out, out_max, "TO_DSINTERVAL(%s)", operand);

    return oracle_fmt(out, out_max, "%s", operand);
}

static const char *oracle_lob_placeholder(const char *dtype)
{
    if (!dtype) return NULL;
    if (strcmp(dtype, "BLOB") == 0)  return "EMPTY_BLOB()";
    if (strcmp(dtype, "CLOB") == 0 ||
        strcmp(dtype, "NCLOB") == 0) return "EMPTY_CLOB()";
    return NULL;
}

/* ROWID is implicit in Oracle - nothing in mid, RETURNING at the end,
 * for both INSERT and UPDATE. loc is not needed. */
static int oracle_row_locator_clauses(db_dml_kind_t kind,
                                      const db_row_locator_ctx_t *loc, int pos,
                                      char *mid, size_t mid_max,
                                      char *tail, size_t tail_max)
{
    (void)loc;
    if (kind != DB_DML_INSERT && kind != DB_DML_UPDATE) return -1;
    if (pos < 1 || !mid || mid_max == 0) return -1;
    mid[0] = '\0';
    return oracle_fmt(tail, tail_max, "RETURNING ROWID INTO :%d", pos);
}

static int oracle_row_limit_sql(const char *sql, int max_rows,
                                char *out, size_t out_max)
{
    if (!sql || max_rows < 0) return -1;
    return oracle_fmt(out, out_max,
                      "SELECT * FROM (%s) WHERE ROWNUM <= %d", sql, max_rows);
}

/* SELECT 1 strips every column type, so a CLOB in sql cannot raise
 * ORA-00932 inside COUNT(*) - the reason this wrapper exists. */
static int oracle_count_rows_sql(const char *sql, char *out, size_t out_max)
{
    if (!sql) return -1;
    return oracle_fmt(out, out_max,
                      "SELECT COUNT(*) FROM (SELECT 1 FROM (%s))", sql);
}

static const char *oracle_now_expr(void)
{
    return "SYSTIMESTAMP";
}

static int oracle_add_seconds_expr(const char *base_expr,
                                   const char *seconds_expr,
                                   char *out, size_t out_max)
{
    if (!base_expr || !seconds_expr) return -1;
    return oracle_fmt(out, out_max, "%s + NUMTODSINTERVAL(%s, 'SECOND')",
                      base_expr, seconds_expr);
}

static int oracle_procedure_call_sql(const char *proc_name,
                                     const char * const *param_names,
                                     int count, char *out, size_t out_max)
{
    if (!proc_name || count < 0 || (count > 0 && !param_names)) return -1;

    if (count == 0)
        return oracle_fmt(out, out_max, "BEGIN %s; END;", proc_name);

    if (!out || out_max == 0) return -1;
    size_t used = 0;
    int n = snprintf(out, out_max, "BEGIN %s(", proc_name);
    if (n < 0 || (size_t)n >= out_max) return -1;
    used = (size_t)n;

    for (int i = 0; i < count; i++)
    {
        if (!param_names[i]) return -1;
        n = snprintf(out + used, out_max - used, "%s:%s",
                     i > 0 ? ", " : "", param_names[i]);
        if (n < 0 || (size_t)n >= out_max - used) return -1;
        used += (size_t)n;
    }

    n = snprintf(out + used, out_max - used, "); END;");
    if (n < 0 || (size_t)n >= out_max - used) return -1;
    return 0;
}

static const db_dialect_t oracle_dialect = {
    .bind_placeholder    = oracle_bind_placeholder,
    .value_expr          = oracle_value_expr,
    .lob_placeholder     = oracle_lob_placeholder,
    .row_locator_clauses = oracle_row_locator_clauses,
    .row_limit_sql       = oracle_row_limit_sql,
    .count_rows_sql      = oracle_count_rows_sql,
    .now_expr            = oracle_now_expr,
    .add_seconds_expr    = oracle_add_seconds_expr,
    .procedure_call_sql  = oracle_procedure_call_sql,
};

/* Stage 6 (2026-10-09) - see db_driver.h, cursor_handle_free(). The
 * handle is a REF CURSOR statement obtained with OCIHandleAlloc() in
 * oracle_dml_execute_procedure(), so it is released the same way. */
static void oracle_cursor_handle_free(void *cursor_handle)
{
    if (cursor_handle)
        OCIHandleFree(cursor_handle, OCI_HTYPE_STMT);
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
    .select_close         = oracle_select_close,
    .dml_execute          = oracle_dml_execute,
    .commit               = oracle_commit,
    .rollback             = oracle_rollback,
    .dml_execute_returning_rowids = oracle_dml_execute_returning_rowids,
    .rowid_result_free            = oracle_rowid_result_free,
    .lob_write_by_rowid           = oracle_lob_write_by_rowid,
    .dml_execute_procedure        = oracle_dml_execute_procedure,
    .select_open_from_cursor      = oracle_select_open_from_cursor,
    .describe_table               = oracle_describe_table,
    .cursor_handle_free           = oracle_cursor_handle_free,
    .dialect                      = &oracle_dialect
};


const db_driver_t *oracle_driver_get(void)
{
    return &oracle_driver;
}

