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
    int         fetch_array_size; /* OCI fetch batch size today; any
                                      driver's equivalent prefetch/batch
                                      tuning knob                        */
    int         query_timeout;    /* seconds, mirrors
                                      execute_config_t.query_timeout     */
    int         include_column_names; /* mirrors execute_config_t field */
} db_select_request_t;

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
 *   Establishes the connection and populates ctx's OCI (or future
 *   vendor-equivalent) handle fields. Same contract as today's
 *   OCI_Connect(): returns 0 on success, non-zero on failure, logs via
 *   ctx->connection_logger.
 *
 * disconnect()
 *   Tears down whatever connect() set up. Same contract as today's
 *   OCI_Disconnect(). Must be safe to call on a ctx that never
 *   successfully connected.
 *
 * execute_select()
 *   Runs req->sql and fetches the full resultset (or up to
 *   req->max_rows) into a newly-allocated resultset_t, via the existing
 *   OCI_Resultset_Builder.h API (resultset_create/resultset_get_row/
 *   resultset_set_field/resultset_set_blob_field) - callers already do
 *   this in parallel with the XML build today (execute_query_batch,
 *   "ADD" tag comments), so the plumbing driver_oracle.c will wrap here
 *   already exists and is already exercised in production.
 *   Returns 0 on success with *out_rs populated (caller owns it - see
 *   resultset_free()); non-zero on failure with *out_rs left NULL. Errors
 *   are logged via ctx->select_logger before returning, same as today.
 *   CLOB/BLOB fetch mechanics ARE in scope here (they are part of
 *   getting correct row data out of Oracle); deciding what to DO with
 *   a BLOB (file write, URL, mime type policy) is NOT - see notes doc,
 *   section 4, and stays on the core side same as it does today.
 */
typedef struct db_driver_t {
    const char *driver_name;   /* e.g. "oracle" - for logging only,
                                   not used for dispatch (db_type in
                                   config.ini already decided which
                                   driver struct got populated)          */

    int  (*connect)(oci_context_t *ctx);
    void (*disconnect)(oci_context_t *ctx);

    int  (*execute_select)(oci_context_t              *ctx,
                            const db_select_request_t  *req,
                            resultset_t                **out_rs);
} db_driver_t;

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
