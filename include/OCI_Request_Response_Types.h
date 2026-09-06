/*
 * OCI_Request_Response_Types.h
 *
 * Format-agnostic Request / Response layer
 * -------------------------------------------
 * These are the types that sit BETWEEN the wire format (XML today, JSON
 * soon, HTTP later) and the CRUD modules (select/insert/update/delete/
 * execute_procedure). Once Level 2 parsing has produced an
 * input_c_request_t, nothing downstream of it - not the CRUD modules,
 * not metrics, not audit trail - needs to know or care whether the
 * original request was XML or JSON.
 *
 * Pipeline
 * --------
 *   raw bytes (file today, HTTP body later)
 *     -> Level 1 parser: sniff format, load into a DOM/JSON tree,
 *        verify well-formed, check mandatory envelope fields present
 *        (external_audit_id, session_id, at least one operation).
 *        Produces: input_c_request_t (this file), or a Level 1 failure.
 *     -> Level 2 parser: per-operation validation (reusing existing
 *        validation logic from OCI_Insert_Validate_Module.c etc., just
 *        pointed at the struct instead of raw XML), SQL dependency
 *        parsing where relevant. Fills in each operation's concrete
 *        payload struct (e.g. select_request_t below).
 *     -> CRUD layer: executes each operation in input_c_request_t in
 *        order, inside one transaction if transaction_required is set.
 *        Produces: output_c_response_t (this file).
 *     -> Response layer: converts output_c_response_t back to the
 *        client's original wire format (input_format_t carried through
 *        from the request, so the response layer never has to
 *        re-detect it).
 *
 * Ownership convention
 * ---------------------
 * This header owns only the envelope shapes. Each CRUD module still
 * owns its own concrete per-operation request/response struct (the way
 * OCI_Session_Manager.h owns session_request_t) - this header does not
 * become a dumping ground for every module's fields. select_request_t/
 * dml_response_t below are a first concrete example, added because
 * Select is first in the refactor sequence; insert_request_t etc.
 * should be added the same way, in their own module headers, as each
 * CRUD module is actually refactored - not pre-guessed here.
 *
 * Error reporting
 * ----------------
 * error_code/error_text on output_c_response_t deliberately mirror the
 * shape metrics_record_t and logger_last_error_t already use - same
 * convention, not a new one. On a rolled-back transaction, per your
 * decision: this is populated from whichever operation's failure
 * triggered the rollback (in practice, straight from
 * logger_last_error at the moment execute_*_batch() returned non-zero),
 * not a separate error path - rollback itself is not expected to
 * generate its own error under normal operation.
 */

#ifndef OCI_REQUEST_RESPONSE_TYPES_H
#define OCI_REQUEST_RESPONSE_TYPES_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Wire format                                                         */
/* ------------------------------------------------------------------ */
typedef enum {
    INPUT_FORMAT_UNKNOWN = 0,
    INPUT_FORMAT_XML,
    INPUT_FORMAT_JSON
} input_format_t;

/* ------------------------------------------------------------------ */
/*  Operation type                                                      */
/*  Add new values here as new operation types are supported - this is */
/*  the one place both Level 2 and the CRUD dispatcher switch on.       */
/* ------------------------------------------------------------------ */
typedef enum {
    OP_UNKNOWN = 0,
    OP_GET_TEMPLATE,        /* client asks for a blank field/column
                              * template for a table before building an
                              * INSERT (or, once supported, UPDATE)      */
    OP_SELECT,
    OP_INSERT,
    OP_UPDATE,
    OP_DELETE,
    OP_EXECUTE_PROCEDURE,
    OP_CREATE_SESSION,
    OP_END_SESSION,
    OP_AUTHENTICATE,         /* new - Security Module, Stage 2 (2026-08-27).
                               * LOCAL auth source only for now - see
                               * OCI_Auth_Manager.h.                       */
    OP_CHECK_PERMISSION,     /* new - Security Module, Stage 5 (2026-08-31).
                               * Cache-only permission check - see
                               * OCI_Authz_Manager.h. */
    OP_CREATE_USER,          /* new - Independent DDL Module proposal
                               * (03-Sep), first operation. tgen-only for
                               * now (parse/validate/generate the DDL
                               * text) - no execute module yet, see
                               * OCI_DDL_Create_User_Module.h. */
    OP_GRANT,                /* new - Independent DDL Module proposal
                               * (03-Sep), second operation. Same
                               * tgen-only scope as OP_CREATE_USER - see
                               * OCI_DDL_Grant_Module.h. */
    OP_CREATE_TABLE,         /* new - Independent DDL Module proposal
                               * (03-Sep), third operation. Same
                               * tgen-only scope as OP_CREATE_USER/
                               * OP_GRANT - see
                               * OCI_DDL_Create_Table_Module.h. */
    OP_DROP_TABLE,           /* new - Independent DDL Module proposal
                               * (03-Sep), fourth operation. Same
                               * tgen-only scope - see
                               * OCI_DDL_Drop_Table_Module.h. */
    OP_CREATE_VIEW,          /* new - Independent DDL Module proposal
                               * (03-Sep), fifth operation. Same
                               * tgen-only scope - see
                               * OCI_DDL_Create_View_Module.h. */
    OP_CREATE_PROCEDURE      /* new - Independent DDL Module proposal
                               * (03-Sep), sixth and final operation.
                               * Same tgen-only scope - see
                               * OCI_DDL_Create_Procedure_Module.h. */
} operation_type_t;

/* ------------------------------------------------------------------ */
/*  field_spec_t                                                        */
/*  One column's worth of metadata + value. Deliberately shared, not    */
/*  duplicated, across three uses that are all structurally the same    */
/*  shape already in the existing XML - a template response, an        */
/*  INSERT operation's row fields, and an UPDATE operation's SET        */
/*  fields (WHERE key fields are a separate, smaller concept - see      */
/*  where_key_t below):                                                 */
/*                                                                       */
/*    - get_template_response_t.columns: value left empty ("")          */
/*    - insert_request_t.rows[n].fields:  value = the insert value      */
/*    - update_request_t.set_fields:      value = the new value         */
/*                                                                       */
/*  This is the field structure Level 2 will need to define concretely  */
/*  once Insert/Update are actually refactored - sketched here now      */
/*  because get_template_response_t needs it today, and duplicating a   */
/*  near-identical struct later under a different name would defeat     */
/*  the point.                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    int  field_number;
    char field_name    [128];
    char field_type     [32];   /* NUMBER, VARCHAR2, DATE, BLOB, CLOB... */
    int  field_length;
    int  field_precision;       /* -1 = not applicable                  */
    int  field_scale;           /* -1 = not applicable                  */
    int  field_nullable;        /* 1 = Y, 0 = N                          */
    char field_default  [256];  /* "" = no default                       */
    char value          [4096]; /* insert_value / update_value / "" for
                                  * a template response                  */
} field_spec_t;

/* ------------------------------------------------------------------ */
/*  GET_TEMPLATE request/response                                       */
/*  Mirrors template_request_t / get_insert_template() in                */
/*  OCI_Insert_Template_Module.h - target_operation is carried through   */
/*  rather than hardcoded, since that module's own template_request_t   */
/*  already anticipates other operations (UPDATE) being supported later, */
/*  even though only INSERT is implemented today.                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char target_operation[32];   /* "INSERT" today; "UPDATE" reserved   */
    char table_name[128];
    char owner[128];             /* "" = auto-resolve, matches existing
                                   * template_request_t behaviour        */
} get_template_request_t;

typedef struct {
    char table_name[128];
    char owner[128];
    int  column_count;
    field_spec_t *columns;       /* array of column_count entries, every
                                   * .value left empty - client fills   */
                                  /* these in and resubmits as the real  */
                                  /* INSERT/UPDATE operation             */
} get_template_response_t;

/* ------------------------------------------------------------------ */
/*  field_value_t                                                       */
/*  Deliberately NOT field_spec_t. field_spec_t (full metadata) only    */
/*  ever travels server -> client, on a GET_TEMPLATE response. The      */
/*  client has no authority over field_type/length/precision/scale/     */
/*  nullable/default - those live in the database and are already       */
/*  served by metadata_cache - so asking the client to echo them back   */
/*  on every INSERT/UPDATE is pure overhead, and worse, opens a class   */
/*  of bug where client-supplied metadata could disagree with what the  */
/*  server actually has. The actual client -> server request just needs */
/*  field_name + value; Level 2 validation resolves the real metadata   */
/*  itself via metadata_cache before executing.                         */
/*                                                                        */
/*  value[4096] covers the overwhelming majority of real field values    */
/*  cheaply - important since insert_request_t can hold hundreds of      */
/*  rows x dozens of columns, and a much larger fixed buffer per field   */
/*  would multiply that memory footprint for every bulk insert whether   */
/*  it needs it or not (500 rows x 20 cols x 32KB =~320MB vs ~40MB       */
/*  today, for example).                                                 */
/*                                                                        */
/*  large_value is the escape hatch for the real exception: CLOB values  */
/*  (client-supplied, or OCI_Audit_Trail_Manager.c's own NEW_VALUE row   */
/*  snapshot, which serialises every column of the business row and can  */
/*  trivially exceed 4096 bytes on its own). NULL in the common case.    */
/*  When non-NULL, it - not value[] - is the real value; value[] then    */
/*  holds only a truncated preview for debug/log readability, not the    */
/*  authoritative content. Always use field_value_get() below to read    */
/*  the real value rather than touching value[] directly, so there's     */
/*  exactly one place that decision is made.                             */
/*                                                                        */
/*  Ownership: whoever sets large_value (Level 1's parsers, or           */
/*  OCI_Audit_Trail_Manager.c building a snapshot row directly) owns the  */
/*  allocation; level1_free_request() frees it as part of freeing the    */
/*  enclosing insert_row_t.                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    char  field_name[128];
    char  value      [4096];
    char *large_value;    /* NULL unless value[] didn't fit - see above */
    char  client_date_format[64];  /* Optional - see the
                                     * <client_date_format> tag's own
                                     * doc comment near
                                     * level2_validate_insert() in
                                     * OCI_Level2_Parser.h. Empty
                                     * means "already in nls_date_format
                                     * from config.ini", the same
                                     * assumption this project has
                                     * always made - this field just
                                     * makes an escape hatch for
                                     * clients that can't send it that
                                     * way, rather than silently
                                     * hoping every client's date
                                     * strings happen to match.        */
} field_value_t;

/*
 * field_value_get()
 *
 * Returns the real value for fv - large_value if set, value[]
 * otherwise. Every reader of a field_value_t's content (Level 2
 * validation, build_insert_ctx_from_request(), anywhere else a value
 * is actually used rather than just previewed in a log line) should
 * go through this rather than touching value[]/large_value directly.
 */
static inline const char *field_value_get(const field_value_t *fv)
{
    return fv->large_value ? fv->large_value : fv->value;
}

/* ------------------------------------------------------------------ */
/*  First concrete sketch: INSERT / UPDATE request shapes               */
/*  Rough - these belong in their own module headers once Insert/       */
/*  Update are actually refactored (same convention as select_request_t */
/*  below), included here just to anchor field_value_t against          */
/*  something real.                                                     */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/*  insert_row_t / insert_request_t                                    */
/*  Moved to OCI_Insert_Execute_Module.h - Insert is now being          */
/*  refactored, which is exactly the trigger this section originally    */
/*  said to watch for ("belong in their own module headers once         */
/*  Insert/Update are actually refactored").                            */
/*                                                                        */
/*  update_request_t has moved the same way, to                         */
/*  OCI_Update_Execute_Module.h, now that Update was refactored, and     */
/*  delete_request_t has moved to OCI_Delete_Execute_Module.h the same   */
/*  way now that Delete is being refactored too. where_key_t stays here  */
/*  - it's genuinely shared, not owned by either module.                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char field_name[128];
    char key_value  [4096];
    char client_date_format[64];  /* Optional - same meaning and
                                    * default as field_value_t's own
                                    * client_date_format above. WHERE
                                    * keys can be dates too (see
                                    * Delete Round 3's own fixture).    */
} where_key_t;

/* ------------------------------------------------------------------ */
/*  END_SESSION request                                                 */
/*  Maps to session_end(ctx, session_id, SESSION_STATUS_LOGGED_OUT,      */
/*  reason, ...) - status is always LOGGED_OUT for a client-initiated    */
/*  request; EXPIRED/EXPIRED_ORPHAN are system-detected conditions       */
/*  (TTL timeout, startup reconciliation), never something a client      */
/*  explicitly asks for.                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    char session_id[64];
    char reason[256];           /* optional; "-" if not supplied         */
} end_session_request_t;

/* ------------------------------------------------------------------ */
/*  EXECUTE_PROCEDURE - param_direction_t, procedure_param_t,          */
/*  execute_procedure_request_t, procedure_resultset_t,                 */
/*  execute_procedure_response_t all moved to OCI_Execute_Procedure_     */
/*  Module.h now that Execute_Procedure is being refactored, same        */
/*  pattern as insert_request_t/update_request_t/delete_request_t's      */
/*  own moves out of this file as each of THOSE modules got refactored.  */
/*  This block used to live here, pre-staged ahead of the refactor       */
/*  actually happening - found and removed 2026-07-31 via a genuine      */
/*  build error (duplicate-definition conflict) after the new header     */
/*  was written fresh without checking whether a copy already existed    */
/*  here first, unlike every other struct relocation in this project.    */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Status / error - same shape as metrics_record_t / logger_last_error */
/*  Reused deliberately rather than inventing a parallel convention.    */
/*                                                                       */
/*  error_code/error_text are populated here at BOTH levels this struct  */
/*  is used - per-operation (output_c_operation_result_t.status) and     */
/*  request-level (output_c_response_t.request_status) - and the         */
/*  Response layer MUST render both into the actual XML/JSON the client  */
/*  receives, not just keep them as internal C fields. Per your          */
/*  feedback 2026-07-15: this level of detail has been genuinely         */
/*  valuable for debugging throughout this project (logger_last_error,   */
/*  metrics_Data_Manager.csv's own error_code/error_text columns) and     */
/*  that value only exists if it's actually visible in what a client/    */
/*  tester sees, not just logged internally.                             */
/* ------------------------------------------------------------------ */
typedef struct {
    int  status_code;         /* 0 = OK, non-zero = failure            */
    char error_code [64];     /* "-" when status_code == 0             */
    char error_text [256];    /* "-" when status_code == 0             */
} operation_status_t;

/* ------------------------------------------------------------------ */
/*  input_c_operation_t                                                  */
/*  One operation within a request's <transaction> block.               */
/*                                                                       */
/*  payload is intentionally void* at this stage: Level 2 parsing        */
/*  allocates and fills in the concrete struct appropriate to `type`     */
/*  (e.g. a select_request_t* for OP_SELECT), owned by that operation's  */
/*  own module header - not by this file. The CRUD dispatcher switches   */
/*  on `type` to know which concrete struct to cast payload to.          */
/* ------------------------------------------------------------------ */
typedef struct {
    operation_type_t type;
    void            *payload;          /* cast per `type` - see above  */
    operation_status_t validation_status; /* set by Level 2, before execution */
} input_c_operation_t;

/* ------------------------------------------------------------------ */
/*  input_c_request_t                                                   */
/*  Produced by Level 1 (envelope) + Level 2 (per-operation) parsing.   */
/*  This is what the CRUD dispatcher actually receives - it never sees  */
/*  raw XML/JSON, only this.                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char   version[16];              /* request schema version, e.g. "1.0" */
    char   external_audit_id[64];    /* client-supplied; mandatory per Level 1 */
    char   session_id[64];           /* "-" or CREATE_SESSION marker    */

    input_format_t source_format;    /* carried through so the response
                                       * layer knows what to convert back to,
                                       * without re-detecting anything   */

    int    transaction_required;     /* client-specified: 1 = whole request
                                       * is atomic (tx_begin/commit/rollback
                                       * wraps every operation below); 0 =
                                       * operations run/commit independently */

    char   transaction_name[128];    /* optional business label for the
                                       * transaction (e.g. "Save Booking"),
                                       * carried straight through to
                                       * tx_begin()'s own tx_name argument -
                                       * see OCI_Transaction_Manager.h.
                                       * Only meaningful when
                                       * transaction_required is set. Client
                                       * may omit it entirely; Level 1
                                       * always fills this with a real value
                                       * ("No Name Specified" as the
                                       * fallback) so downstream code never
                                       * has to check for an empty string. */

    int    operation_count;
    input_c_operation_t *operations; /* array of operation_count entries */

} input_c_request_t;

/* ------------------------------------------------------------------ */
/*  output_c_operation_result_t                                         */
/*  Result of one executed operation.                                    */
/*                                                                       */
/*  Like input_c_operation_t's payload, result_payload is void* here -   */
/*  each CRUD module produces its own concrete result struct (e.g. a     */
/*  dml_response_t* for OP_SELECT/OP_INSERT/OP_UPDATE/OP_DELETE) and the */
/*  Response layer switches on `type` to know how to render it back     */
/*  into the original format.                                            */
/*                                                                       */
/*  If the overall request rolled back (input_c_request_t.               */
/*  transaction_required was set and a later operation failed), earlier  */
/*  operations in this array may show status_code == 0 (they DID         */
/*  execute successfully before the rollback) even though none of it is  */
/*  durable - per your call, the client-facing response reports only     */
/*  the final rollback-triggering error at the request level (see        */
/*  output_c_response_t below), not per-operation success here. This     */
/*  array is primarily for internal bookkeeping / debugging, matching    */
/*  how metrics/audit already log each operation's own row regardless    */
/*  of a later rollback.                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    operation_type_t     type;
    void                 *result_payload;   /* cast per `type`          */
    operation_status_t   status;
} output_c_operation_result_t;

/* ------------------------------------------------------------------ */
/*  output_c_response_t                                                 */
/*  Produced by the CRUD layer, consumed by the Response layer.         */
/* ------------------------------------------------------------------ */
typedef struct {
    char   version[16];
    char   external_audit_id[64];    /* echoed back from the request    */
    char   session_id[64];           /* echoed back (or newly created  */
                                      /* session_id if this request was  */
                                      /* itself a CREATE_SESSION)        */

    input_format_t source_format;    /* copied from the request - tells */
                                      /* the Response layer what to      */
                                      /* render back into                */

    int    result_count;
    output_c_operation_result_t *results;  /* array of result_count entries;
                                             * see note above re: rollback */

    /* Request-level status - what the client actually sees.
     * On success: status_code == 0.
     * On a rolled-back transactional request: status_code != 0, and
     * error_code/error_text come from whichever operation triggered the
     * rollback (sourced from logger_last_error at that point) - not a
     * separate rollback-specific error, per your call that rollback
     * itself isn't expected to fail under normal operation.            */
    operation_status_t request_status;

} output_c_response_t;

/* ================================================================== */
/*  First concrete example: SELECT                                      */
/*  Select is first in the refactor sequence - sketched here to anchor  */
/*  the design against something real. This would move to (or be        */
/*  re-declared in) OCI_Execute_Query_Module.h once that module is       */
/*  actually refactored; kept here for now purely for the initial        */
/*  request/response file conversion pass.                              */
/* ================================================================== */

typedef struct {
    char sql[4096];              /* validated SQL text (post Level 2)   */
    int  max_rows;                /* from ctx->ini->query_max_record_count
                                    * unless overridden per-request      */
    int  fetch_batch_size;
    int  include_column_names;

    /* Stage 5 (2026-08-22) - execute_async / async_call_back_url. Level 1
     * only extracts what's on the wire; Level 2 is responsible for
     * rejecting execute_async=1 on anything that isn't a clean, single-
     * operation SELECT, and for requiring a valid HTTPS
     * async_call_back_url whenever execute_async=1 (see
     * OCI_Level2_Parser.c's own doc comment on this pair for the full
     * validation contract - TLS-only callback URLs, no exceptions, same
     * stance as HTTP consumer's own inbound listener). */
    int  execute_async;               /* 0 (default) = normal synchronous
                                          response, unchanged from every
                                          prior stage. 1 = stream each
                                          fetched batch to
                                          async_call_back_url as it's
                                          retrieved instead of building one
                                          combined response.             */
    char async_call_back_url[512];    /* "" unless execute_async=1. Must be
                                          https:// - see Level 2 note
                                          above.                          */
} select_request_t;

/* ================================================================== */
/*  dml_response_t                                                       */
/*  Shared response shape for SELECT / INSERT / UPDATE / DELETE.         */
/*  Anchored directly against real output_xml captured in                */
/*  metrics_Data_Manager.csv across all four operations - they turned    */
/*  out to be structurally near-identical (operation/table_name/owner/   */
/*  execution_time always present; rows_affected just has a different    */
/*  XML tag name per operation today - rows_inserted/rows_updated/        */
/*  rows_deleted/implicitly num_rows for SELECT), so one struct with      */
/*  optional fields is used instead of four almost-duplicate ones.        */
/*  Which fields are meaningful depends on operation_type_t on the        */
/*  enclosing output_c_operation_result_t:                                */
/*    SELECT  - sql_query, resultset_xml_fragment populated; table_name/  */
/*              owner/lobs_written not applicable                        */
/*    INSERT  - table_name/owner/rows_affected/lobs_written populated;    */
/*              sql_query/resultset not applicable                       */
/*    UPDATE  - as INSERT                                                */
/*    DELETE  - as UPDATE, minus lobs_written (nothing written)           */
/*                                                                        */
/*  No WHERE-clause echo on the response - DELETE's existing output_xml   */
/*  used to include one (where_keys) and UPDATE's didn't; per your call   */
/*  2026-07-14, removed from DELETE for consistency rather than added to  */
/*  UPDATE, so neither echoes it.                                        */
/* ================================================================== */
typedef struct {
    char   table_name[128];      /* "" for SELECT                        */
    char   owner[128];           /* "" for SELECT                        */

    int    rows_affected;        /* rows_inserted/updated/deleted, or
                                   * num_rows for SELECT                  */
    int    lobs_written;         /* INSERT/UPDATE only; 0 otherwise       */
    int    lobs_extracted;       /* SELECT only; 0 otherwise              */
    double execution_time_seconds;

    char  *sql_query;              /* SELECT only                        */
    char  *resultset_xml_fragment; /* SELECT only - same reused shape as
                                     * execute_query_batch's existing
                                     * <resultset> body                  */
} dml_response_t;

#ifdef __cplusplus
}
#endif

#endif /* OCI_REQUEST_RESPONSE_TYPES_H */
