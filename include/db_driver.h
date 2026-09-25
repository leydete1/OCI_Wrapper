/*
 * db_driver.h
 *
 * Vendor-neutral database driver interface (db_driver_t).
 *
 * SCOPE, updated as each module's abstraction lands (see
 * DB_Driver_Abstraction_Notes.md for the running history):
 *   connect / disconnect / pooling / select (cursor) - proven, live
 *   DELETE's scalar-bind execute + commit/rollback - in progress,
 *     2026-09-16 (see dml_execute()/commit()/rollback() below)
 * Deliberately NOT included yet: INSERT's array-bind-batch and BLOB/CLOB
 * SET-value cases, UPDATE, DDL. dml_execute() below is intentionally
 * shaped to be reusable by UPDATE and INSERT's own scalar-bind case
 * once those modules get their own pass - see its own doc comment for
 * why it's scalar-only, one-shot, and not cursor-shaped like SELECT.
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

#include <stdint.h>                  /* uint64_t - db_fetch_batch_stats_t */
#include "OCI_Connection.h"        /* oci_context_t */
#include "Resultset_Types.h"   /* resultset_t */

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
 * db_dml_request_t
 *
 * Everything a driver needs to run one non-SELECT DML statement (DELETE
 * today; UPDATE/INSERT's own scalar-bind case are the intended next
 * reuse - see dml_execute()'s doc comment). Deliberately not cursor-
 * shaped like db_select_request_t/db_select_cursor_t - there are no
 * rows to stream back, just a statement, its bind values, and a row
 * count - so this is a single request struct with no matching cursor
 * type at all.
 *
 * sql is expected fully-built, matching current behavior: today's
 * build_delete_sql() (OCI_Delete_Execute_Module.c) already wraps
 * DATE/TIMESTAMP/INTERVAL bind placeholders in Oracle-specific
 * TO_DATE()/TO_TIMESTAMP()/TO_YMINTERVAL()/TO_DSINTERVAL() syntax
 * *before* this struct is ever built - that SQL-dialect knowledge
 * deliberately stays in core for this pass (see ACID TEST above; a
 * second vendor would need different wrapping syntax, but there is no
 * second vendor's needs to design against yet, same reasoning as BINDS
 * below). bind_values are therefore always plain strings - the wrapper
 * function already turned any date/timestamp/interval value into
 * whatever string TO_DATE()/etc. expects; the driver never needs to
 * know a value was ever anything but a string.
 *
 * BINDS - unlike db_select_request_t, genuinely needed from day one
 * here: every WHERE-key value in a DELETE is bound (OCIBindByPos,
 * always SQLT_STR, matching build_delete_ctx_from_request()'s existing
 * behavior), there being no equivalent to select's "fully-substituted
 * SQL string" today for the write path.
 */
typedef struct {
    const char  *sql;          /* fully-built DELETE ... WHERE ... text,
                                   TO_DATE()/etc. wrapping already
                                   applied - see struct comment above  */
    int          bind_count;
    const char **bind_values;  /* bound by position, 1..bind_count,
                                   always SQLT_STR - array and every
                                   string it points to are caller-owned,
                                   must outlive the dml_execute() call.
                                   bind_values[k] == NULL means bind SQL
                                   NULL at that position (added 2026-09-19,
                                   found during UPDATE's own pass - a
                                   NULL entry used to silently become an
                                   empty string; harmless for DELETE's
                                   own WHERE keys, never legitimately
                                   NULL, but genuinely wrong for
                                   UPDATE's SET clause, which can - see
                                   driver_oracle.c's own fix)          */
} db_dml_request_t;

/*
 * db_dml_returning_request_t / db_rowid_result_t
 *
 * Added 2026-09-18, UPDATE's own abstraction pass. Covers the case
 * dml_execute() deliberately doesn't: a statement with a
 * "RETURNING ROWID INTO :N" clause whose bind iteration count is
 * always 1 (see OCI_Update_Execute_Module.c's own extensive comment on
 * why - a single UPDATE...WHERE execution, not a batch of several),
 * but whose WHERE clause can still match an unknown number of physical
 * rows at execute time - confirmed directly against real data
 * (WHERE NUMBER_COL=300 matching 209 rows). A static, single-slot
 * output bind can only ever receive ONE of those rows' ROWIDs back;
 * this needs OCI's dynamic-bind callback mechanism instead
 * (OCIBindDynamic), which grows to however many rows Oracle actually
 * reports.
 *
 * Correction (2026-09-20, found and fixed before INSERT's own pass ever
 * started, while checking reusability rather than assuming it): the
 * note originally here claimed OCI_Insert_Execute_Module.c "already
 * uses the identical RETURNING ROWID INTO + dynamic-collector pattern"
 * for its own LOB writes. Checked directly against that module's real
 * code - it doesn't. INSERT's own RETURNING ROWID bind is a STATIC
 * array (rowid_bufs, calloc'd to execute_count, bound via plain
 * OCIBindByPos + OCIBindArrayOfStruct) - not OCIBindDynamic at all.
 * That's the correct choice for INSERT's own shape: unlike UPDATE's
 * WHERE clause, INSERT always knows exactly how many rows it's
 * inserting in advance, so there's no "unknown count until execute
 * time" problem to solve with a dynamic bind. row_count below is what
 * actually makes this struct/function reusable for that real shape -
 * see its own doc comment.
 *
 * sql must already contain the RETURNING ROWID INTO :N clause, with N
 * given explicitly via returning_bind_position - build_update_sql()/
 * build_insert_sql() (core-side, untouched by this pass) already
 * compute this position correctly; the driver has no independent way
 * to know it.
 */
typedef struct {
    const char  *sql;
    int          bind_count;
    const char **bind_values;  /* same NULL-means-SQL-NULL convention as
                                   db_dml_request_t's own bind_values -
                                   see its doc comment above.

                                   Layout depends on row_count below:
                                   row_count <= 1 (DELETE/UPDATE's own
                                   shape, unchanged since 2026-09-18) -
                                   exactly bind_count entries, one value
                                   per bind position, same as
                                   db_dml_request_t.

                                   row_count > 1 (added 2026-09-20,
                                   INSERT's own shape) - a FLAT,
                                   ROW-MAJOR array of bind_count *
                                   row_count entries: row 0's bind_count
                                   values first, then row 1's, and so
                                   on - bind_values[row * bind_count +
                                   col]. Matches INSERT's own real
                                   multi-row batch (OCIBindArrayOfStruct,
                                   N distinct values per bind position,
                                   one per row) - not a value repeated
                                   across every row the way UPDATE's own
                                   SET clause is (see
                                   db_dml_returning_request_t's own note
                                   above on why UPDATE never needed this:
                                   its bind iteration count is always 1,
                                   only the WHERE-matched row count
                                   varies, which is what the ROWID
                                   collector already handles).          */
    int          returning_bind_position;
    int          row_count;    /* Added 2026-09-20, INSERT's own
                                   abstraction pass. 0 or 1 = today's
                                   exact single-row behavior, unchanged -
                                   every existing DELETE/UPDATE call site
                                   needs no changes at all, still served
                                   by the dynamic ROWID collector
                                   (OCIBindDynamic). >1 = a real
                                   multi-row batch (INSERT's own
                                   OCIStmtExecute iters, up to
                                   ctx->ini->max_bulk_inserts) - bind_values
                                   is then read as the flat, row-major
                                   array described above, and every
                                   scalar column bind additionally uses
                                   OCIBindArrayOfStruct, matching
                                   OCI_Insert_Execute_Module.c's own
                                   existing bind loop exactly.

                                   Correction (2026-09-20, same day):
                                   this originally planned to route
                                   row_count > 1 through the SAME
                                   dynamic collector DELETE/UPDATE use,
                                   reasoning that its per-iteration
                                   callback should generalize to any
                                   iters count. Real testing
                                   (Driver_Insert_Test.c's own Test
                                   2/3/6) showed that reasoning was
                                   wrong: the array insert itself always
                                   succeeded correctly, but the dynamic
                                   callback only ever fired once
                                   regardless of the real iters value -
                                   a genuine, unresolved OCI-level
                                   limitation for this specific
                                   combination. row_count > 1 now uses a
                                   STATIC array bind instead (matching
                                   OCI_Insert_Execute_Module.c's own
                                   real, already-proven code exactly,
                                   not a second guess) - see
                                   oracle_dml_execute_returning_rowids()'s
                                   own comment in driver_oracle.c for
                                   the full account. Left the original
                                   reasoning above rather than deleting
                                   it, as an honest record of what was
                                   tried and why it changed.            */
} db_dml_returning_request_t;

/*
 * out->rowids is a flat buffer, ROWID_BUF_SIZE-per-entry (matching
 * OCI_Update_Execute_Module.c's own #define - see driver_oracle.c's own
 * copy of that constant, kept in sync deliberately rather than shared
 * across a core/driver boundary that shouldn't otherwise need to agree
 * on a buffer size). out->count is the REAL number of physical rows
 * Oracle reported - not the bind iteration count, which is always 1 for
 * every caller of dml_execute_returning_rowids() today. This is what
 * closes the "response always reports rows_affected=1 regardless of how
 * many rows genuinely matched" gap found during this pass's own
 * analysis (see notes doc) - core reads out->count directly instead of
 * assuming iteration count reflects affected rows.
 *
 * Caller-owned pointer through rowid_result_free() below, not raw
 * free() - the driver may allocate more than just the one buffer
 * internally.
 */
typedef struct {
    char *rowids;
    int   count;
} db_rowid_result_t;

/*
 * db_lob_write_request_t
 *
 * Added 2026-09-18, UPDATE's own abstraction pass - the write-side
 * mirror of SELECT's existing oracle_fetch_blob_field()/
 * oracle_fetch_clob_field() (which read LOBs; this writes them),
 * covering the BLOB/CLOB SET-value case dml_execute() deliberately
 * doesn't (see build_update_sql()'s own EMPTY_BLOB()/EMPTY_CLOB()
 * placeholder handling, core-side, untouched by this pass) - the SQL
 * itself never binds a LOB value directly; a SELECT ... FOR UPDATE on
 * the target row's own ROWID obtains a live locator afterward, which
 * this call then writes to via chunked OCILobWrite.
 *
 * BLOB and CLOB deliberately kept asymmetric here, matching today's
 * real behavior exactly rather than unifying it as part of this
 * abstraction pass (a real design question raised and decided
 * 2026-09-18 - see notes doc): file_path is ALWAYS used for BLOB,
 * streamed by the driver chunk-by-chunk as it's read, never buffered
 * whole. CLOB ALWAYS arrives via inline_text instead - core has
 * already resolved whatever the request supplied (a literal value, or
 * the already-read contents of a file:// value) into memory before
 * this call, exactly as OCI_Update_Execute_Module.c's own
 * handle_clob_update() does today. Exactly one of file_path/
 * inline_text is populated, matching is_blob - the driver does not
 * infer which from whether a field is NULL.
 */
typedef struct {
    int          is_blob;       /* 1 = SQLT_BLOB, 0 = SQLT_CLOB/NCLOB  */
    const char  *table_fq;      /* owner.table, or just table - already
                                    resolved by core, same convention as
                                    db_dml_request_t.sql being fully-built */
    const char  *column_name;
    const char  *rowid_str;
    const char  *file_path;     /* BLOB only - see struct comment above */
    const char  *inline_text;   /* CLOB only - see struct comment above */
    size_t       inline_text_len;
} db_lob_write_request_t;

/*
 * db_proc_param_direction_t / db_proc_param_type_t
 *
 * Added 2026-09-21, EXECUTE_PROCEDURE's own abstraction pass - genuinely
 * new territory, not an extension of anything DELETE/UPDATE/INSERT
 * built (their shared dml_execute()/dml_execute_returning_rowids()
 * family assumes positional, input-only, single-type binds; this
 * module's real bind shape is none of those - see db_proc_param_t's
 * own doc comment below). Deliberately its own enum here, not a reuse
 * of OCI_Execute_Procedure_Module.h's own param_direction_t - db_driver.h
 * stays self-contained, no dependency on any module-specific header,
 * same as every other struct in this file.
 */
typedef enum {
    DB_PROC_DIR_IN     = 0,
    DB_PROC_DIR_OUT    = 1,
    DB_PROC_DIR_IN_OUT = 2
} db_proc_param_direction_t;

typedef enum {
    DB_PROC_TYPE_STR    = 0,   /* VARCHAR2/DATE/TIMESTAMP - bound SQLT_STR.
                                   Any TO_DATE()/TO_TIMESTAMP() wrapping a
                                   DATE/TIMESTAMP value needs is core's
                                   own concern, applied before the value
                                   ever reaches this struct - same
                                   division of responsibility as every
                                   other module's own SQL-text-building
                                   step, untouched by this pass.         */
    DB_PROC_TYPE_INT    = 1,   /* NUMBER/INTEGER - bound SQLT_INT        */
    DB_PROC_TYPE_CURSOR = 2    /* SYS_REFCURSOR - bound SQLT_RSET, OUT
                                   only. Never IN or IN_OUT - a cursor
                                   can't be passed in, confirmed against
                                   level2_validate_procedure()'s own
                                   check rather than assumed.            */
} db_proc_param_type_t;

/*
 * db_proc_param_t
 *
 * One request-side parameter, carrying its own post-execute output slot
 * on the same entry - IN_OUT genuinely needs both together, matching
 * OCI_Execute_Procedure_Module.c's own bind_parameters(): the very same
 * OCI bind buffer is read for the IN value and overwritten with the OUT
 * value by one OCIBindByName call, not two separate binds.
 *
 * name is the bind variable name without its leading ':' -
 * build_plsql_block() (core-side, untouched by this pass) already wrote
 * "BEGIN proc(:P1, :P2...); END;" using these same names before this
 * struct is ever built; the driver binds by NAME here, not position -
 * this module's own genuine bind shape, unlike every other execute
 * module's positional binds.
 *
 * out_value/out_value_size follow the same "caller-owned buffer, driver
 * only writes into it" convention already used elsewhere in this file
 * (see db_lob_write_request_t's own file_path/inline_text, never
 * driver-allocated) - not a db_rowid_result_t-style driver-allocated,
 * separately-freed output. Unlike a dynamically-sized ROWID list, every
 * scalar OUT value here has one known, bounded size decided entirely by
 * core (MAX_PARAM_VALUE_SIZE in the module calling this), so there is
 * nothing here that genuinely needs driver-side allocation.
 */
typedef struct {
    const char                *name;
    db_proc_param_type_t       type;
    db_proc_param_direction_t  direction;

    const char *in_value;      /* IN/IN_OUT value - NULL means SQL NULL,
                                   same convention as every other bind
                                   interface in this file. Ignored for a
                                   pure OUT param, and always for
                                   CURSOR (never has a meaningful value
                                   going in).                            */

    char   *out_value;         /* caller-owned buffer, written for
                                   STR-typed OUT/IN_OUT only, untouched
                                   otherwise - caller must size it
                                   itself via out_value_size.            */
    size_t  out_value_size;
    int     out_int;           /* written for INT-typed OUT/IN_OUT only */
    int     out_is_null;       /* driver sets 1 if the returned OUT
                                   value (any type) is genuinely NULL -
                                   out_value/out_int's own content is
                                   then meaningless. Matches
                                   proc_param_t's own indicator==-1
                                   check today.                          */

    void   *out_cursor_handle; /* CURSOR OUT only - opaque; an Oracle
                                   OCIStmt* under the hood, but never
                                   typed as one here (core stays vendor-
                                   neutral). NULL if this cursor was
                                   never actually opened by the
                                   procedure - a real, valid outcome,
                                   not an error (see
                                   UNIT_TEST_CURSOR_PROC's own
                                   deliberately-conditional OPEN). Pass
                                   directly to select_open_from_cursor()
                                   to fetch it - ownership transfers
                                   there, and select_close() afterward
                                   releases the underlying cursor
                                   statement too, same as it already
                                   releases a normal SELECT's own
                                   prepared statement. Never use this
                                   for a non-CURSOR param.               */
} db_proc_param_t;

/*
 * db_proc_execute_request_t
 *
 * plsql_block must already be a complete, ready-to-prepare anonymous
 * block ("BEGIN proc(:P1,...); END;") - build_plsql_block() (core-side,
 * untouched by this pass) already builds this exactly as today.
 */
typedef struct {
    const char       *plsql_block;
    int                param_count;
    db_proc_param_t   *params;      /* NOT const - dml_execute_procedure()
                                        writes each entry's own out_value/
                                        out_int/out_is_null/
                                        out_cursor_handle fields in place.
                                        Deliberate deviation from the
                                        const-request convention every
                                        other *_request_t in this file
                                        follows - genuinely necessary
                                        here, not an oversight, since
                                        IN_OUT's whole point is a value
                                        that changes.                    */
} db_proc_execute_request_t;

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
 * db_fetch_batch_stats_t
 *
 * Added during real integration (2026-09-14) - see select_fetch_batch()'s
 * doc comment below for why this is necessary, not optional metadata.
 * blob_bytes/clob_bytes are THIS BATCH's totals only, matching
 * blob_count/clob_count - caller accumulates across batches itself.
 */
typedef struct {
    int      blob_count;
    int      clob_count;
    uint64_t blob_bytes;
    uint64_t clob_bytes;
} db_fetch_batch_stats_t;

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
 *   Returns 0 on success. As of v2 (2026-09-14) CLOB/BLOB columns are
 *   supported through this cursor - DB_SELECT_UNSUPPORTED_LOB (see
 *   below) is kept defined for any genuinely-unsupported type that
 *   comes up later, but is no longer returned for CLOB/BLOB specifically.
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
 *   out_stats (added during real integration, 2026-09-14) - optional,
 *   pass NULL if the caller doesn't need it. Reports how many BLOB/CLOB
 *   fields this batch processed and how many bytes were read for each -
 *   found to be genuinely necessary, not cosmetic: execute_query_batch()'s
 *   existing response carries <blobs_extracted>/<clobs_extracted> tags
 *   and feeds metrics.lob_bytes/metrics.clob_bytes from these same
 *   running totals today (accumulated across every batch, exactly the
 *   same shape the existing async path already uses for its own
 *   per-batch batch_blob_index/batch_clob_index/batch_clob_bytes) - a
 *   cursor that fetched real LOB data but never reported it back would
 *   silently make those tags/metrics wrong for any query actually using
 *   this path. Caller sums out_stats across every batch into its own
 *   running totals, same as it already does for the async path.
 *
 * select_close()
 *   Releases the cursor and everything select_open() allocated for it
 *   (prepared statement, per-column OCI arrays, etc.) - must be called
 *   exactly once per successful select_open(), whether or not the
 *   cursor was fully drained first. Safe to call on a cursor that
 *   select_open() never actually returned successfully is NOT
 *   guaranteed - only call this after a 0 return from select_open().
 *
 * dml_execute()
 *   Added 2026-09-16, DELETE's own abstraction pass (see notes doc's
 *   running history). Runs req->sql as one complete, self-contained
 *   operation - prepare, bind every req->bind_values entry by position,
 *   execute (iters=1), read back the row count, release the prepared
 *   statement - all inside this one call, unlike select's three-call
 *   cursor. No streaming to represent, so no cursor-shaped split is
 *   needed the way SELECT genuinely required one (see SELECT IS A
 *   CURSOR above) - this is DELETE's actual shape, not a simplification
 *   of it. *out_rows_affected is only meaningful when this returns 0.
 *   Returns 0 on success, non-zero on failure.
 *
 *   out_error_message/out_error_message_size - added 2026-09-22, the
 *   DDL module's own abstraction pass. Optional, NULL-able (pass
 *   out_error_message=NULL, out_error_message_size=0 to skip this
 *   entirely) - every existing DELETE/UPDATE/INSERT/PROCEDURE call site
 *   needs no changes at all. Unlike every prior caller, which only ever
 *   logs a failure and lets the caller check the log file,
 *   execute_ddl_statement()'s own callers embed the actual Oracle error
 *   text directly in the client-facing response
 *   (<error_message>ORA-00955: ...</error_message>) - confirmed
 *   directly against ddl_execution_result_t's own real struct and
 *   get_ddl_execution_response_xml()'s own real output, not assumed.
 *   When out_error_message is non-NULL and dml_execute() returns
 *   non-zero, it holds the same OCIErrorGet() text already written to
 *   the log - same source, just also handed back to the caller instead
 *   of only logged. Untouched (left as whatever the caller passed in,
 *   typically empty) when this call succeeds.
 *
 *   logger - added deliberately, not an oversight: unlike select_open()/
 *   select_fetch_batch() (which only ever have one caller and so could
 *   safely log via a hardcoded ctx->select_logger), this function is
 *   meant to be reused by UPDATE/INSERT's own scalar-bind case later
 *   (see SCOPE above) - each with its own logger (ctx->delete_logger/
 *   ctx->update_logger/ctx->insert_logger). Hardcoding one inside the
 *   driver would silently mis-attribute every log line the moment a
 *   second caller arrives, so the caller passes its own logger in
 *   explicitly instead - caught before implementation, not after.
 *
 *   Does NOT commit. Whether to commit after a successful dml_execute()
 *   is an application-level decision (ctx->active_tx - is a managed
 *   transaction already in progress, or does this module own its own
 *   standalone commit boundary?) - not a vendor concern, and keeping it
 *   a separate call preserves each calling module's existing
 *   commit-or-not branching exactly as it is today. Call commit() or
 *   rollback() explicitly afterward, same as today's inline
 *   OCITransCommit/OCITransRollback calls in each execute module.
 *
 * commit() / rollback()
 *   Commits or rolls back ctx's current transaction. Same contract as
 *   today's direct OCITransCommit(ctx->svchp, ctx->errhp, OCI_DEFAULT)/
 *   OCITransRollback() calls, now behind the driver boundary - every
 *   other detail (when to call which, based on ctx->active_tx) stays a
 *   core decision untouched by this move, same reasoning as dml_execute()
 *   not committing itself. Same logger reasoning as dml_execute() above.
 *   Returns 0 on success, non-zero on failure.
 *
 * dml_execute_returning_rowids()
 *   Added 2026-09-18, UPDATE's own abstraction pass - see
 *   db_dml_returning_request_t/db_rowid_result_t's own doc comments
 *   above for the full "why does this need to exist separately from
 *   dml_execute()" reasoning (a dynamically-sized RETURNING ROWID INTO
 *   output, not a scalar in, scalar out call). Same one-shot,
 *   self-contained shape as dml_execute() otherwise - prepare, bind,
 *   execute, collect, release, all inside this one call - and the same
 *   "does not commit, caller calls commit()/rollback() explicitly
 *   afterward" contract. *out_result is only meaningful when this
 *   returns 0, and must be released via rowid_result_free() exactly
 *   once when the caller is done with it, whether or not it went on to
 *   call lob_write_by_rowid() for any of the rows in it.
 *
 *   Extended 2026-09-20, INSERT's own abstraction pass - req->row_count
 *   > 1 binds every scalar column as a real array (OCIBindArrayOfStruct,
 *   matching INSERT's own multi-row batch exactly), reading
 *   req->bind_values as the flat, row-major layout its own doc comment
 *   describes, instead of the single-value-per-position bind DELETE/
 *   UPDATE's calls (row_count 0 or 1) still use unchanged. The ROWID
 *   output itself uses a DIFFERENT mechanism for row_count > 1 than
 *   for the single-row case - a static array bind, not the dynamic
 *   collector - after real testing showed the dynamic callback doesn't
 *   fire per-iteration when other positions are array-bound; see
 *   db_dml_returning_request_t's own doc comment above for the full
 *   account of what was tried first and why.
 *
 * rowid_result_free()
 *   Releases everything dml_execute_returning_rowids() allocated into
 *   *result. Safe to call on a zeroed (never-populated) result; not
 *   safe to call twice on the same one.
 *
 * lob_write_by_rowid()
 *   Added 2026-09-18, UPDATE's own abstraction pass - see
 *   db_lob_write_request_t's own doc comment above for the full
 *   BLOB/CLOB asymmetry reasoning (preserved exactly as today's
 *   behavior already has it, a deliberate decision, not an oversight).
 *   One call writes one column's LOB content to one row, identified by
 *   its own ROWID (from a prior dml_execute_returning_rowids() call) -
 *   called once per affected row per LOB column by the caller, same
 *   shape as today's inline per-row loop in OCI_Update_Execute_Module.c
 *   (core decides how many rows and which columns; this call does the
 *   OCI work for exactly one of each). Returns 0 on success (including
 *   a genuinely empty value, which is a no-op success, not an error -
 *   matches today's is_empty short-circuit), non-zero on failure.
 *   *out_bytes_written is only meaningful when this returns 0.
 *
 * dml_execute_procedure()
 *   Added 2026-09-21, EXECUTE_PROCEDURE's own abstraction pass - see
 *   db_proc_param_t's own doc comment above for the full "why is this
 *   genuinely different from dml_execute()" reasoning (named, mixed-
 *   type, two-way binds - nothing already built fits this). One
 *   self-contained call - prepare, bind every param by name and type,
 *   execute (always iters=1, PL/SQL anonymous blocks can't be array-
 *   executed - confirmed directly against the module's own Stage 4
 *   comment), collect every scalar OUT/IN_OUT value in place, stash
 *   every CURSOR OUT param's own open statement handle, release the
 *   outer block's own statement (independent of any CURSOR handles,
 *   which persist until fetched). Does NOT commit, and core never
 *   needs to call commit()/rollback() after this either - procedures
 *   manage their own transactions internally, confirmed directly
 *   against this module's own doc comment ("no commit issued by this
 *   module directly"), not assumed. No row-count concept exists here
 *   at all, unlike every DML call in this file - returns only 0 on
 *   success, non-zero on failure.
 *
 * select_open_from_cursor()
 *   Added 2026-09-21, EXECUTE_PROCEDURE's own abstraction pass - the
 *   piece that makes reusing SELECT's own proven fetch machinery for a
 *   CURSOR OUT parameter genuinely possible, replacing a second,
 *   older-style parallel fetch implementation
 *   (fetch_cursor_to_xml()/cur_batch_ctx_t in
 *   OCI_Execute_Procedure_Module.c) that wrote straight to XML rather
 *   than through the resultset_t/response_write_xml() pipeline every
 *   other module already uses. Takes cursor_handle - a
 *   db_proc_param_t's own out_cursor_handle from a prior
 *   dml_execute_procedure() call - and does only the DESCRIBE/DEFINE
 *   half of what select_open() does; the prepare/execute half already
 *   happened as part of the procedure's own execute. Returns the exact
 *   same db_select_cursor_t/db_column_meta_t[]/column_count/batch_size
 *   shape select_open() returns, so select_fetch_batch()/select_close()
 *   work completely unchanged afterward - genuine reuse of an already-
 *   proven path, not a parallel one. select_close() releases the
 *   underlying cursor statement too, same as it already releases a
 *   normal SELECT's own prepared statement - no separate cleanup call
 *   exists or is needed for a fetched cursor handle. Returns 0 on
 *   success, non-zero on failure (including DB_SELECT_UNSUPPORTED_LOB,
 *   same meaning as select_open()'s own use of it, if the cursor's
 *   result has a LOB column this pass's driver doesn't support through
 *   this interface yet).
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

    int  (*select_fetch_batch)(db_select_cursor_t      *cursor,
                                resultset_t             **out_rs,
                                int                       *out_rows_fetched,
                                db_fetch_batch_stats_t   *out_stats);

    void (*select_close)(db_select_cursor_t *cursor);

    int  (*dml_execute)(oci_context_t            *ctx,
                         logger_t                 *logger,
                         const db_dml_request_t   *req,
                         int                      *out_rows_affected,
                         char                     *out_error_message,
                         size_t                    out_error_message_size);

    int  (*commit)(oci_context_t *ctx, logger_t *logger);
    int  (*rollback)(oci_context_t *ctx, logger_t *logger);

    int  (*dml_execute_returning_rowids)(
                         oci_context_t                     *ctx,
                         logger_t                           *logger,
                         const db_dml_returning_request_t   *req,
                         db_rowid_result_t                  *out_result);

    void (*rowid_result_free)(db_rowid_result_t *result);

    int  (*lob_write_by_rowid)(
                         oci_context_t                  *ctx,
                         logger_t                        *logger,
                         const db_lob_write_request_t   *req,
                         uint64_t                        *out_bytes_written);

    int  (*dml_execute_procedure)(
                         oci_context_t               *ctx,
                         logger_t                     *logger,
                         db_proc_execute_request_t   *req);

    int  (*select_open_from_cursor)(
                         oci_context_t              *ctx,
                         void                       *cursor_handle,
                         db_select_cursor_t        **out_cursor,
                         db_column_meta_t          **out_columns,
                         int                         *out_column_count,
                         int                         *out_batch_size);
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
