/*
 * OCI_Level2_Parser.h
 *
 * Level 2 Parser
 * ----------------
 * Second stage of the input pipeline (see OCI_Level1_Parser.h and
 * Data_Manager_Request_Definitions.docx for the full design).
 * Deliberately format-blind - it only ever sees the C structs Level 1
 * already built (select_request_t etc.), never raw XML/JSON. It does
 * not touch a live database connection either - see the design note
 * below on why the row-count guard specifically does NOT live here.
 *
 * Currently implemented: SELECT only.
 * ------------------------------------
 * level2_validate_select() runs extract_sql_dependencies() against the
 * SQL Level 1 already extracted into select_request_t.sql - pure
 * syntax/structure analysis, no connection needed. This is the entire
 * scope of Level 2 for SELECT, per 2026-07-17 decision: the row-count
 * guard is NOT duplicated here, because it fundamentally cannot be -
 * it needs a live COUNT(*) against an open session, and Level 2 is a
 * pre-execution, connection-independent stage by design. There is no
 * session yet at this point for it to run against. That check stays
 * exactly where it already lives, inside execute_query_batch()'s own
 * Stage 1.
 *
 * Every other operation type (INSERT/UPDATE/DELETE/GET_TEMPLATE/
 * EXECUTE_PROCEDURE/CREATE_SESSION/END_SESSION) is explicitly rejected
 * with LEVEL2_ERR_NOT_IMPLEMENTED for now - fail closed, not fail open.
 * An operation type without an implemented validator must not silently
 * proceed to the CRUD layer unvalidated.
 *
 * A useful distinction worth carrying forward as each operation type
 * gets implemented here: if the answer to a validation question comes
 * from the request itself (e.g. INSERT's row_count vs max_bulk_inserts -
 * both already sitting in the parsed struct), it belongs in Level 2. If
 * it only exists once you touch the database (SELECT's row-count guard,
 * resolving field_value_t against real column types via metadata_cache),
 * it either stays at execution time or Level 2 needs a live connection
 * for that specific operation - a case-by-case call, not a blanket rule.
 */

#ifndef OCI_LEVEL2_PARSER_H
#define OCI_LEVEL2_PARSER_H

#include "OCI_Connection.h"
#include "OCI_Request_Response_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Result codes                                                        */
/* ------------------------------------------------------------------ */
#define LEVEL2_OK                     0
#define LEVEL2_ERR_INVALID_ARG       -1
#define LEVEL2_ERR_EMPTY_SQL         -2
#define LEVEL2_ERR_SQL_INVALID       -3   /* extract_sql_dependencies failed */
#define LEVEL2_ERR_NOT_IMPLEMENTED   -4   /* no validator for this operation type yet */
#define LEVEL2_ERR_VALIDATION_FAILED -5   /* returned by level2_validate() when
                                            * any operation in the request fails */

/*
 * level2_validate_select()
 *
 * Validates one OP_SELECT operation's already-built select_request_t
 * (op->payload). Runs extract_sql_dependencies() against req->sql -
 * catches malformed SQL and unsupported constructs (UNION/INTERSECT/
 * EXCEPT, functions in SELECT, etc. - see sql_dependency_extractor.h's
 * documented rules) before anything reaches a connection.
 *
 * Parameters
 *   ctx          - for ctx->select_logger, passed through to
 *                  extract_sql_dependencies() (matches how that
 *                  function is called everywhere else in the project)
 *   op           - must have type == OP_SELECT and a non-NULL payload
 *                  (a select_request_t built by Level 1)
 *   error_detail - populated on any failure. On success,
 *                  error_detail->status_code is 0 and error_code/
 *                  error_text are "-".
 *
 * Returns LEVEL2_OK on success, one of the LEVEL2_ERR_* codes above
 * otherwise. Does not modify op->payload - only reads it.
 */
int level2_validate_select(oci_context_t        *ctx,
                            input_c_operation_t  *op,
                            operation_status_t   *error_detail);

/*
 * level2_validate()
 *
 * Runs the appropriate validator against every operation in request,
 * in order, populating each operation's validation_status as it goes.
 * Fails fast: stops at the first operation whose validation fails and
 * returns immediately - consistent with the whole request being atomic
 * and reporting a single error either way (see Response Envelope /
 * Rollback behaviour in Data_Manager_Request_Definitions.docx).
 * Operations already validated before the failing one keep their own
 * validation_status recorded, for the same internal-bookkeeping reason
 * output_c_response_t's per-operation results are kept even after a
 * rollback - this is not what the client sees, just what's on record.
 *
 * Returns LEVEL2_OK if every operation validated successfully,
 * LEVEL2_ERR_VALIDATION_FAILED if any operation failed (check that
 * operation's own validation_status for the specific reason),
 * LEVEL2_ERR_INVALID_ARG for NULL ctx/request.
 */
int level2_validate(oci_context_t      *ctx,
                     input_c_request_t *request);

#ifdef __cplusplus
}
#endif

#endif /* OCI_LEVEL2_PARSER_H */
