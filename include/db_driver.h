/*
 * db_driver.h
 *
 * Vendor-neutral database driver interface (db_driver_t).
 *
 * PoC SCOPE (see DB_Driver_Abstraction_Notes.md) - THIS PASS ONLY:
 *   connect / disconnect / execute_select
 * Deliberately NOT included yet: insert, update, delete, DDL, explicit
 * transaction begin/commit/rollback, connection pooling entry points.
 * Those get added to this same struct once the interface shape below is
 * validated against a real Oracle driver and, ideally, a second vendor.
 * Adding a field to db_driver_t later is cheap; guessing every field's
 * final shape now and getting the boundary wrong is not - see notes
 * doc, section 2.
 *
 * WHAT STAYS OUT OF THE DRIVER (core, generic, vendor-independent):
 *   - SQL parsing/validation (Level1/Level2 parsers)
 *   - request/response shaping, XML/JSON rendering
 *   - resultset cache, metadata cache lookups
 *   - CLOB/BLOB file-write/mime-type policy
 *   - dispatch, session/auth, metrics
 * A driver's execute_select() hands back rows + column metadata in the
 * existing format-agnostic resultset_t (OCI_Resultset_Types.h) and
 * nothing else. It does not know XML or JSON exist. Building the
 * response document from that resultset_t, and deciding whether to
 * serve/store a cached copy, both stay on the core side of this line -
 * see OCI_Resultset_Builder.h's own doc comment, which was written with
 * exactly this eventual caller in mind.
 *
 * ACID TEST for anything considered for this struct: would the same
 * signature make sense for a SQL Server/ODBC call? If yes, it belongs
 * here. If it only makes sense for Oracle (OCI handle types, OCI-specific
 * modes/flags), it stays inside driver_oracle.c and never appears in
 * this header.
 *
 * CONTEXT TYPE - deliberately NOT changed in this pass.
 * Every function below still takes oci_context_t* (OCI_Connection.h),
 * not a new db_context_t. oci_context_t is threaded through ~100 files
 * today; collapsing it into a driver-neutral db_context_t with an
 * opaque, driver-owned handle for envhp/errhp/srvhp/svchp/authp is a
 * separate, mechanical refactor to do only once this interface shape is
 * proven (notes doc, section 3 - "worth planning as its own pass after
 * the interface shape is validated by the PoC, not before"). Until that
 * pass, connect()/disconnect() are still allowed to reach into ctx's
 * five real OCI-handle fields directly; execute_select() is not - it
 * only reads ctx for logging/config/cache handles it already legitimately
 * needs, and returns data purely through resultset_t.
 *
 * CONNECTION POOLING (added once connect/disconnect were validated -
 * see Driver_Connect_Test.c's PASS run).
 * connect()/disconnect() hide the pooled-vs-direct choice: a single
 * connection is just the pool_min_size=pool_max_size=1 degenerate case
 * conceptually, even though today's Oracle implementation genuinely is
 * two separate code paths (OCI_Connect vs OCI_Connect_pool) rather than
 * one path with size=1 - see driver_oracle.c for exactly where that
 * branch now lives, moved out of every caller (Bootstrap used to do
 * this if/else itself; now it just calls connect()). Callers never
 * branch on ctx->ini->use_connection_pool themselves any more.
 *
 * get_session()/release_session()/session_is_alive()/reconnect_session()/
 * health_check() are the worker-level borrow/return surface and are
 * only meaningful after a pooled connect() - direct mode has no worker
 * threads to borrow a session for (consumer_type=FILE, for one,
 * already refuses to start in direct mode today - see
 * Data_Manager_Bootstrap.c). Calling these after a direct-mode
 * connect() is undefined, same as calling OCI_Pool_get_session() on an
 * unpooled ctx is today - this interface does not change that
 * precondition, only where the call sites live.
 *
 * BINDS - deliberately NOT in db_select_request_t yet.
 * The current select path (execute_query_batch) builds a fully-substituted
 * SQL string and has no OCIBindByName/OCIBindByPos calls anywhere today.
 * Parameterized execute-with-binds is real future scope (notes doc,
 * section 2) but there is no existing behavior to preserve for it yet,
 * so it is left out here rather than guessed at. Add a bind_params
 * field to db_select_request_t when that work actually starts.
 *
 * ASYNC BATCH CALLBACK - deliberately NOT in scope for this pass either.
 * execute_config_t's async_batch_callback (streaming partial resultsets
 * back mid-fetch) is a response-delivery concern, not a fetch-mechanics
 * one, but wiring it through a driver boundary cleanly needs its own
 * think once plain synchronous select is proven. Not part of
 * db_select_request_t or db_driver_t.execute_select in this pass.
 *
 * SELECT IS A CURSOR, NOT A SINGLE CALL - revised after reading
 * execute_query_batch() end to end (cut-point analysis, 2026-09-12).
 * The original single-shot execute_select(req, *out_rs) signature below
 * was wrong for production parity: execute_query_batch()'s fetch loop
 * already runs batch-by-batch (OCIStmtFetch2 with a batch size, not one
 * row and not the whole resultset), and the async-streaming path proves
 * this matters architecturally, not just internally - it builds,
 * renders, delivers, and discards a resultset_t PER BATCH, never
 * accumulating a whole-query one at all. A single-call interface can't
 * represent that without either buffering the entire result in the
 * driver first (defeats the point of batching) or the driver knowing
 * about response delivery (violates the driver/core boundary itself).
 * So the interface is a cursor: select_open() (prepare+describe, no
 * rows yet) -> select_fetch_batch() repeatedly (driver's natural batch
 * size each time) -> select_close(). Core decides what to do with each
 * batch - accumulate into one big resultset_t (today's sync path) or
 * render+deliver+discard immediately (today's async path) - the driver
 * never needs to know which.
 *
 * SCALARS ONLY IN THIS PASS - CLOB/BLOB are NOT being dropped, only
 * sequenced second (confirmed 2026-09-12). Reading the fetch loop
 * surfaced two real, vendor-specific complications living inside
 * CLOB/BLOB handling specifically: OCI silently cannot array-fetch a
 * CLOB (forces fetch_count=1 for the whole batch, an undocumented OCI
 * limitation - see driver_oracle.c), and CLOB's file-write/naming logic
 * was never factored out of the fetch loop the way BLOB's already was
 * (BLOB already delegates cleanly to OCI_Blob_Utils.c, which has zero
 * OCI calls in it - CLOB does the equivalent work inline instead). Both
 * are real, core-relevant behavior - not incidental - and mixing that
 * complexity into proving the cursor shape itself would make it hard
 * to tell which part broke if something did. select_open() reports an
 * explicit "not yet supported" error for any SELECT whose DESCRIBE
 * finds a CLOB/BLOB column (see below) - callers fall back to the
 * existing execute_query_batch() for those until v2 adds LOB support on
 * top of this same, by-then-proven cursor shape.
 */


#ifndef DB_DRIVER_H
#define DB_DRIVER_H

#include "OCI_Connection.h"        /* oci_context_t */
#include "OCI_Resultset_Types.h"   /* resultset_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * db_select_request_t
 *
 * Everything a driver needs to run a SELECT and fetch rows - and nothing
 * a driver has no business seeing. Deliberately narrower than
 * execute_config_t (OCI_Connection.h): ReturnFormat, xml, OUTPUT_JSON,
 * input_file_name, and async_batch_callback/async_batch_user_data all
 * stay on execute_config_t and never cross into this struct - those are
 * response-shaping/delivery concerns, not fetch mechanics. If a field
 * wouldn't make sense on a SQL Server driver too, it does not belong
 * here (see ACID TEST above).
 *
 * SQL is expected fully-substituted, matching current behavior - see
 * "BINDS" note above.
 */
typedef struct {
    const char *sql;              /* fully-substituted SELECT text      */
    int         max_rows;         /* row-count guard, mirrors
                                      execute_config_t.max_rows          */
    int         max_memory_bytes; /* mirrors execute_config_t.max_memory_bytes */
    int         fetch_array_size; /* requested batch size - a REQUEST,
                                      not a guarantee: the driver may
                                      report a smaller actual batch size
                                      from select_open() (e.g. Oracle
                                      forcing 1 when a CLOB column is
                                      present - see SELECT IS A CURSOR
                                      above). Core must read the actual
                                      size back from select_open(), never
                                      assume this value held.            */
    int         query_timeout;    /* seconds, mirrors
                                      execute_config_t.query_timeout     */
    int         include_column_names; /* mirrors execute_config_t field */
} db_select_request_t;

/*
 * db_column_meta_t
 *
 * One entry per column, returned by select_open() from the DESCRIBE
 * step - before any row is fetched. field_type uses the same string
 * vocabulary resultset_field_t.field_type already does (NUMBER,
 * VARCHAR2, DATE, STRING, TIMESTAMP, BLOB, CLOB, UNKNOWN) so core code
 * matching against it (e.g. the wildcard/unqualified-column expansion
 * in today's execute_query_batch, which currently calls OCIParamGet
 * directly for this - see cut-point analysis) can consume this instead
 * of making its own vendor calls.
 */
typedef struct {
    char field_name[128];
    char field_type[32];
} db_column_meta_t;

/*
 * db_select_cursor_t
 *
 * Opaque, driver-owned. Core never looks inside this - it exists so
 * select_fetch_batch()/select_close() know which open, in-progress
 * SELECT they're continuing/tearing down. driver_oracle.c's definition
 * wraps an OCIStmt* and the handful of per-column OCI arrays
 * allocate_batch_buffers() already builds today; a different vendor's
 * driver would wrap whatever its own prepared-statement/cursor handle
 * is - core code never needs to know or care which.
 */
typedef struct db_select_cursor_t db_select_cursor_t;

/*
 * db_driver_t
 *
 * One instance per running process (see notes doc, section 1 - this is
 * a vtable, not a lookup map: only one db_type is ever active at a
 * time). Populated once at startup from config.ini's db_type and called
 * through uniformly everywhere execute_query_batch/OCI_Connect are
 * called today.
 *
 * connect()
 *   Establishes the process-level connection - pooled or direct,
 *   decided internally from ctx->ini->use_connection_pool, so callers
 *   no longer branch on that flag themselves (see CONNECTION POOLING
 *   above). Populates ctx's OCI (or future vendor-equivalent) handle
 *   fields for the direct case, or ctx->pool_handle for the pooled
 *   case. Returns 0 on success, non-zero on failure, logs via
 *   ctx->connection_logger - same contract as today's OCI_Connect()/
 *   OCI_Connect_pool(), whichever it dispatches to.
 *
 * disconnect()
 *   Tears down whatever connect() set up - same pooled-vs-direct
 *   dispatch, same contract as OCI_Disconnect()/OCI_Disconnect_pool().
 *   Must be safe to call on a ctx that never successfully connected.
 *
 * get_session() / release_session()
 *   Worker-level borrow/return from an already-connected pool. Same
 *   contract as today's OCI_Pool_get_session()/OCI_Pool_release_session().
 *   Only valid after a pooled connect() - see precondition note above.
 *
 * session_is_alive()
 *   Same contract as OCI_Pool_session_is_alive(): 1 if ctx's currently-
 *   held session is alive, 0 if not or if the check itself couldn't be
 *   performed (conservative by design - matches existing behavior).
 *
 * reconnect_session()
 *   Same contract as OCI_Pool_reconnect_session(): releases ctx's
 *   current session and borrows + re-initialises a fresh one in its
 *   place. Caller must still reset ctx->active_tx = NULL afterward -
 *   that remains a worker-level concern, not a driver one, same as
 *   today.
 *
 * health_check()
 *   Same contract as OCI_Pool_health_check(): pings free slots, recycles
 *   expired/idle ones, reopens dead ones. Returns the number of slots
 *   that could not be recovered (0 = all healthy). Safe to call from a
 *   background heartbeat thread, same as today.
 *
 * select_open()
 *   Prepares req->sql and runs the DESCRIBE step only - no rows fetched
 *   yet. On success: *out_cursor is a live cursor ready for
 *   select_fetch_batch(), *out_columns and *out_column_count describe every
 *   column (caller owns the array - free with plain free(), it is a
 *   flat calloc'd block, same convention as bc->col_names today), and
 *   *out_batch_size reports the ACTUAL batch size this cursor will use -
 *   which may be smaller than req->fetch_array_size (Oracle forces 1
 *   when any CLOB column is present - see SELECT IS A CURSOR above).
 *   Returns 0 on success. Returns DB_SELECT_UNSUPPORTED_LOB (see below)
 *   if DESCRIBE finds a CLOB or BLOB column - this pass's driver does
 *   not support them yet (see SCALARS ONLY above); caller should fall
 *   back to the existing execute_query_batch() for that request.
 *   Returns a negative value on any other failure, logged via
 *   ctx->select_logger, same as today.
 *
 * select_fetch_batch()
 *   Fetches the cursor's next batch (its actual batch size, from
 *   select_open()) into a newly-allocated resultset_t sized to however
 *   many rows actually came back this call. *out_rows_fetched == 0
 *   means the cursor is exhausted - *out_rs is left NULL in that case,
 *   nothing more to free from this call. Core is expected to keep
 *   calling this in a loop until that happens, same shape
 *   execute_query_batch()'s own fetch loop already has today - whether
 *   core accumulates every batch into one whole-query resultset_t or
 *   renders+delivers+discards each one immediately (today's sync vs
 *   async paths, respectively) is entirely a core decision the driver
 *   never sees. Returns 0 on success (including the 0-rows/exhausted
 *   case), non-zero on failure, logged via ctx->select_logger.
 *
 * select_close()
 *   Releases the cursor and everything select_open() allocated for it
 *   (prepared statement, per-column OCI arrays, etc.) - must be called
 *   exactly once per successful select_open(), whether or not the
 *   cursor was fully drained first. Safe to call on a cursor that
 *   select_open() never actually returned successfully is NOT
 *   guaranteed - only call this after a 0 return from select_open().
 */
typedef struct db_driver_t {
    const char *driver_name;   /* e.g. "oracle" - for logging only,
                                   not used for dispatch (db_type in
                                   config.ini already decided which
                                   driver struct got populated)          */

    int  (*connect)(oci_context_t *ctx);
    void (*disconnect)(oci_context_t *ctx);

    int  (*get_session)(oci_context_t *ctx, oci_context_t *worker_ctx);
    void (*release_session)(oci_context_t *ctx, oci_context_t *worker_ctx);
    int  (*session_is_alive)(oci_context_t *ctx);
    int  (*reconnect_session)(oci_context_t *base_ctx, oci_context_t *ctx);
    int  (*health_check)(oci_context_t *ctx);

    int  (*select_open)(oci_context_t              *ctx,
                         const db_select_request_t  *req,
                         db_select_cursor_t         **out_cursor,
                         db_column_meta_t           **out_columns,
                         int                         *out_column_count,
                         int                         *out_batch_size);

    int  (*select_fetch_batch)(db_select_cursor_t *cursor,
                                resultset_t        **out_rs,
                                int                  *out_rows_fetched);

    void (*select_close)(db_select_cursor_t *cursor);
} db_driver_t;

/*
 * DB_SELECT_UNSUPPORTED_LOB
 *
 * select_open()'s specific return value meaning "this request has a
 * CLOB/BLOB column and this pass's driver doesn't support that through
 * the cursor interface yet" - a real, expected outcome in this pass,
 * not a generic error. Deliberately distinct from a plain non-zero
 * failure so core can tell "fall back to execute_query_batch() for
 * this one" apart from "something actually went wrong."
 */
#define DB_SELECT_UNSUPPORTED_LOB (-100)


/*
 * db_driver_get()
 *
 * Returns the populated db_driver_t for the currently-configured
 * db_type (ctx->ini->db_type once that config.ini key exists - not yet
 * added; see notes doc, section "Suggested order of attack", step 3).
 * NULL if db_type is unrecognized. Exactly one driver is ever live per
 * process, so this is a plain lookup at startup, not a per-call cost -
 * callers are expected to fetch it once and hold the pointer, same as
 * any other startup-resolved config.
 *
 * Deliberately not implemented in this header - lives in db_driver.c
 * once driver_oracle.c exists to populate the struct it returns.
 */
const db_driver_t *db_driver_get(const oci_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DB_DRIVER_H */
