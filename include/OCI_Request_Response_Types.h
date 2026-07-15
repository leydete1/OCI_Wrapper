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
 * select_response_t below are a first concrete example, added because
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
    OP_SELECT,
    OP_INSERT,
    OP_UPDATE,
    OP_DELETE,
    OP_EXECUTE_PROCEDURE,
    OP_CREATE_SESSION,
    OP_END_SESSION
} operation_type_t;

/* ------------------------------------------------------------------ */
/*  Status / error - same shape as metrics_record_t / logger_last_error */
/*  Reused deliberately rather than inventing a parallel convention.    */
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

    int    operation_count;
    input_c_operation_t *operations; /* array of operation_count entries */

} input_c_request_t;

/* ------------------------------------------------------------------ */
/*  output_c_operation_result_t                                         */
/*  Result of one executed operation.                                    */
/*                                                                       */
/*  Like input_c_operation_t's payload, result_payload is void* here -   */
/*  each CRUD module produces its own concrete result struct (e.g. a     */
/*  select_response_t* for OP_SELECT) and the Response layer switches    */
/*  on `type` to know how to render it back into the original format.   */
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
} select_request_t;

typedef struct {
    int      num_rows;
    uint64_t execution_time_total_us;
    int      fetch_batch_size;
    int      blobs_extracted;
    int      clobs_extracted;
    char    *resultset_xml_fragment; /* the <resultset>...</resultset> body
                                       * already produced internally by
                                       * execute_query_batch() today -
                                       * reused as-is; the Response layer
                                       * decides whether to emit it as XML
                                       * verbatim or convert to JSON      */
} select_response_t;

#ifdef __cplusplus
}
#endif

#endif /* OCI_REQUEST_RESPONSE_TYPES_H */
