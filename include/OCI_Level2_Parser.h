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
 * Currently implemented: SELECT, INSERT.
 * ---------------------------------------
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
 * level2_validate_insert() resolves the target table's real column
 * metadata via ctx->metadata_cache (never anything the client sent -
 * field_value_t deliberately carries no metadata of its own) and
 * reuses validate_field() from OCI_Insert_Validate_Module.h - the same
 * per-type validation rules that module has always enforced, just
 * re-anchored to metadata_cache instead of client-echoed metadata.
 * Unlike SELECT's row-count guard, this DOES touch ctx->metadata_cache
 * (and, on a cache miss, the database) - the "case-by-case, not a
 * blanket rule" carve-out below cuts the other way here, since the
 * metadata cache makes this cheap and safe on the common path.
 *
 * Every other operation type (UPDATE/DELETE/GET_TEMPLATE/
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
#define LEVEL2_ERR_ROW_COUNT_EXCEEDED -6  /* insert_request_t.row_count >
                                            * ctx->ini->max_bulk_inserts       */
#define LEVEL2_ERR_METADATA_LOOKUP    -7  /* metadata_cache_get_or_fetch failed,
                                            * or table_name/owner not found    */
#define LEVEL2_ERR_FIELD_INVALID      -8  /* a field failed validate_field(),
                                            * or referenced an unknown column,
                                            * or a NOT NULL column was omitted */
#define LEVEL2_ERR_ASYNC_INVALID      -9  /* Stage 5 (2026-08-22) - execute_async=1
                                            * with an empty/non-HTTPS
                                            * async_call_back_url, or on a
                                            * multi-operation transaction.     */

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
 * level2_validate_insert()
 *
 * Validates one OP_INSERT operation's already-built insert_request_t
 * (op->payload) against the table's REAL column metadata, resolved via
 * ctx->metadata_cache (metadata_cache_get_or_fetch()) - never against
 * anything the client sent, since field_value_t deliberately carries
 * no metadata of its own (see field_value_t's doc comment in
 * OCI_Insert_Execute_Module.h).
 *
 * Checks, in order, first failure wins:
 *   1. row_count between 1 and ctx->ini->max_bulk_inserts - the one
 *      check Level 2 CAN do without touching the database (it's
 *      already sitting in the parsed struct), per the case-by-case
 *      distinction in this header's own top-level comment. This does
 *      NOT remove the equivalent check inside execute_insert_batch()
 *      itself - that stays as defense-in-depth for any caller that
 *      reaches Stage 3 without going through Level 2 first.
 *   1b. every row sets the same columns (order doesn't matter, the SET
 *      does) - a single bulk INSERT is one SQL statement with one
 *      fixed column list via OCI array binding; if two rows genuinely
 *      need different columns set, that's two INSERT statements, not
 *      one. Also a pure struct comparison, no connection needed - kept
 *      before the metadata_cache lookup for the same reason as check 1.
 *   2. table_name/owner resolve via metadata_cache_get_or_fetch() -
 *      this DOES need a connection (it's a cache-or-fetch, not a pure
 *      struct check), same "case-by-case, not a blanket rule" carve-out
 *      already established for SELECT's row-count guard, just cutting
 *      the other way here since the metadata cache means this is cheap
 *      and safe to do even on the common (cache-hit) path.
 *   3. every field in every row: field_name matches a real column
 *      (FIELD_UNKNOWN_COLUMN if not), then validate_field() from
 *      OCI_Insert_Validate_Module.h - same rules documented there,
 *      just resolved from metadata_cache instead of client-echoed
 *      metadata.
 *   4. every NOT NULL column with no default on the table is present
 *      in the row (FIELD_MISSING_REQUIRED_COLUMN if not) - a check the
 *      old client-echoes-everything model never needed, since the
 *      client always enumerated every column whether they were
 *      setting it or not; the new slim wire format only sends columns
 *      the client actually wants to set, so an omitted NOT NULL column
 *      needs to be caught here or Oracle would reject it later as
 *      ORA-01400 instead of a clean validation failure.
 *
 * Parameters
 *   ctx          - ctx->insert_logger for validation logging (matches
 *                  every existing Insert-path log call),
 *                  ctx->metadata_cache for the lookup, ctx->ini for
 *                  max_bulk_inserts
 *   op           - must have type == OP_INSERT and a non-NULL payload
 *                  (an insert_request_t built by Level 1)
 *   error_detail - populated on any failure. On success,
 *                  error_detail->status_code is 0 and error_code/
 *                  error_text are "-".
 *
 * Returns LEVEL2_OK on success, one of the LEVEL2_ERR_* codes above
 * otherwise. Does not modify op->payload - only reads it.
 */
int level2_validate_insert(oci_context_t        *ctx,
                            input_c_operation_t  *op,
                            operation_status_t   *error_detail);

/*
 * level2_validate_update()
 *
 * Validates one OP_UPDATE operation's already-built update_request_t
 * (op->payload) against the table's REAL column metadata, resolved via
 * ctx->metadata_cache - same reasoning as level2_validate_insert().
 *
 * Checks, in order, first failure wins:
 *   1. table_name is non-empty.
 *   2. key_count > 0 - an UPDATE with no WHERE clause matches every
 *      row in the table; refused outright rather than silently
 *      allowed, since that's almost certainly a client mistake, not
 *      an intentional whole-table update.
 *   3. field_count > 0 - an UPDATE with nothing to SET is meaningless.
 *   4. table_name/owner resolve via metadata_cache_get_or_fetch().
 *   5. every WHERE key: field_name matches a real column
 *      (FIELD_UNKNOWN_COLUMN if not), key_value is non-empty. No
 *      validate_field() type/length checking on WHERE values - that's
 *      about SET clause correctness (data being written), not WHERE
 *      predicate correctness (a value being matched, not stored).
 *   6. every SET field: field_name matches a real column, then
 *      validate_field() - same rules as INSERT's SET/row fields,
 *      reused unchanged.
 *
 * Deliberately has NO equivalent of INSERT's Check 4 (missing required
 * NOT NULL column) - an UPDATE only touches the columns actually
 * listed in SET; the row already exists with everything else already
 * populated, so there's nothing to check there.
 *
 * Parameters - same shape as level2_validate_insert(); see that
 * function's own doc comment above for what ctx/op/error_detail mean.
 *
 * Returns LEVEL2_OK on success, one of the LEVEL2_ERR_* codes above
 * otherwise. Does not modify op->payload - only reads it.
 */
int level2_validate_update(oci_context_t        *ctx,
                            input_c_operation_t  *op,
                            operation_status_t   *error_detail);

/*
 * level2_validate_delete()
 *
 * Validates one OP_DELETE operation's already-built delete_request_t
 * (op->payload) against the table's REAL column metadata, resolved via
 * ctx->metadata_cache - same reasoning as level2_validate_insert()/
 * level2_validate_update().
 *
 * Genuinely simpler than UPDATE - DELETE has no SET clause at all, so
 * this function's entire scope is the WHERE-key checks:
 *
 * Checks, in order, first failure wins:
 *   1. table_name is non-empty.
 *   2. key_count > 0 - a DELETE with no WHERE clause matches (and
 *      removes) every row in the table; refused outright, same
 *      reasoning as level2_validate_update()'s equivalent check, only
 *      more consequential here since there is no equivalent of "the
 *      rows are at least still there" the way an accidental whole-
 *      table UPDATE leaves behind.
 *   3. table_name/owner resolve via metadata_cache_get_or_fetch() -
 *      needed here for the same reason as UPDATE's WHERE keys: the new
 *      where_key_t carries no type information, so the real column
 *      type (for date/timestamp bind wrapping) has to come from
 *      metadata_cache, never the client.
 *   4. every WHERE key: field_name matches a real column
 *      (FIELD_UNKNOWN_COLUMN if not), key_value is non-empty. No
 *      validate_field() type/length checking - same reasoning as
 *      UPDATE's WHERE keys, a value being matched isn't a value being
 *      stored.
 *
 * Parameters - same shape as level2_validate_insert()/
 * level2_validate_update(); see those functions' own doc comments for
 * what ctx/op/error_detail mean.
 *
 * Returns LEVEL2_OK on success, one of the LEVEL2_ERR_* codes above
 * otherwise. Does not modify op->payload - only reads it.
 */
int level2_validate_delete(oci_context_t        *ctx,
                            input_c_operation_t  *op,
                            operation_status_t   *error_detail);

/*
 * level2_validate_procedure()
 *
 * Validates one OP_EXECUTE_PROCEDURE operation's already-built
 * execute_procedure_request_t (op->payload).
 *
 * Deliberately lighter than INSERT/UPDATE/DELETE's own validators -
 * there is no metadata_cache equivalent for a procedure's own
 * parameter signature (Oracle doesn't expose that the way it exposes
 * table columns via ALL_TAB_COLUMNS, and this project has no lookup
 * for it), so there is nothing authoritative to resolve a parameter's
 * declared type against the way find_column()/validate_field() resolve
 * a table column's real type. This is the one place in the whole
 * project where a client-declared type is trusted rather than
 * resolved - not a relaxation of the "never trust the client"
 * principle, just an acknowledgement that there is nothing available
 * here to validate against instead.
 *
 * Checks, in order, first failure wins:
 *   1. procedure_name is non-empty.
 *   2. param_count is in range (0..MAX_PROC_PARAMS) - zero parameters
 *      is valid (a procedure may take none).
 *   3. every parameter: param_name is non-empty, param_type is one of
 *      the recognised set (NUMBER/INTEGER/VARCHAR2/DATE/TIMESTAMP/
 *      CURSOR).
 *   4. every CURSOR parameter's direction is OUT - a REFCURSOR can't
 *      be passed in, and there is nothing meaningful a caller could
 *      supply for an IN or IN_OUT cursor.
 *
 * No audit trail integration for EXECUTE_PROCEDURE at all (2026-07-29
 * decision) - a procedure is a black box from Data_Manager's own
 * perspective, with no visibility into what its PL/SQL actually does
 * internally, so there is nothing meaningful to hold Data_Manager
 * itself accountable for beyond the fact the call was made.
 *
 * Parameters - same shape as level2_validate_insert()/update()/
 * delete(); see those functions' own doc comments for what
 * ctx/op/error_detail mean.
 *
 * Returns LEVEL2_OK on success, one of the LEVEL2_ERR_* codes above
 * otherwise. Does not modify op->payload - only reads it.
 */
int level2_validate_procedure(oci_context_t        *ctx,
                               input_c_operation_t  *op,
                               operation_status_t   *error_detail);

/*
 * level2_validate_authenticate()
 *
 * Security Module Stage 2 (2026-08-27). Validates one OP_AUTHENTICATE
 * operation's already-built authenticate_request_t (op->payload) -
 * username and credential both present, nothing more. See
 * OCI_Auth_Manager.h for the struct itself and Security_Module_Design_
 * Specification.docx Section 6 for why the deeper checks (does the
 * user exist, is the credential correct) live in auth_authenticate()
 * instead of here.
 *
 * Returns LEVEL2_OK on success, LEVEL2_ERR_FIELD_INVALID or
 * LEVEL2_ERR_INVALID_ARG otherwise.
 */
int level2_validate_authenticate(oci_context_t        *ctx,
                                  input_c_operation_t  *op,
                                  operation_status_t   *error_detail);

/*
 * level2_validate_create_user()
 *
 * Independent DDL Module proposal (03-Sep), first operation. Thin
 * adapter: delegates the actual field-by-field checking to
 * validate_create_user_request() in OCI_DDL_Create_User_Module.h
 * (identifier rules, quota format, password safety, etc.) and maps
 * its 0/-1 + error_buf contract onto the LEVEL2_ERR_* /
 * operation_status_t contract every other validator here uses, same
 * adapter shape as level2_validate_authenticate() over auth_manager's
 * own checks.
 *
 * Returns LEVEL2_OK on success, LEVEL2_ERR_FIELD_INVALID or
 * LEVEL2_ERR_INVALID_ARG otherwise.
 */
int level2_validate_create_user(oci_context_t        *ctx,
                                 input_c_operation_t  *op,
                                 operation_status_t   *error_detail);

/*
 * level2_validate_grant()
 *
 * Independent DDL Module proposal (03-Sep), second operation. Same
 * thin-adapter shape as level2_validate_create_user() - delegates to
 * validate_grant_request() in OCI_DDL_Grant_Module.h.
 *
 * Returns LEVEL2_OK on success, LEVEL2_ERR_FIELD_INVALID or
 * LEVEL2_ERR_INVALID_ARG otherwise.
 */
int level2_validate_grant(oci_context_t        *ctx,
                           input_c_operation_t  *op,
                           operation_status_t   *error_detail);

/*
 * level2_validate_create_table()
 *
 * Independent DDL Module proposal (03-Sep), third operation. Same
 * thin-adapter shape as level2_validate_create_user()/
 * level2_validate_grant() - delegates to
 * validate_create_table_request() in OCI_DDL_Create_Table_Module.h.
 *
 * Returns LEVEL2_OK on success, LEVEL2_ERR_FIELD_INVALID or
 * LEVEL2_ERR_INVALID_ARG otherwise.
 */
int level2_validate_create_table(oci_context_t        *ctx,
                                  input_c_operation_t  *op,
                                  operation_status_t   *error_detail);

/*
 * level2_validate_drop_table()
 *
 * Independent DDL Module proposal (03-Sep), fourth operation. Same
 * thin-adapter shape as the other three DDL validators - delegates to
 * validate_drop_table_request() in OCI_DDL_Drop_Table_Module.h.
 *
 * Returns LEVEL2_OK on success, LEVEL2_ERR_FIELD_INVALID or
 * LEVEL2_ERR_INVALID_ARG otherwise.
 */
int level2_validate_drop_table(oci_context_t        *ctx,
                                input_c_operation_t  *op,
                                operation_status_t   *error_detail);

/*
 * level2_validate_create_view()
 *
 * Independent DDL Module proposal (03-Sep), fifth operation. Same
 * thin-adapter shape as the other DDL validators - delegates to
 * validate_create_view_request() in OCI_DDL_Create_View_Module.h.
 */
int level2_validate_create_view(oci_context_t        *ctx,
                                 input_c_operation_t  *op,
                                 operation_status_t   *error_detail);

/*
 * level2_validate_create_procedure()
 *
 * Independent DDL Module proposal (03-Sep), sixth and final operation.
 * Same thin-adapter shape - delegates to
 * validate_create_procedure_request() in
 * OCI_DDL_Create_Procedure_Module.h.
 */
int level2_validate_create_procedure(oci_context_t        *ctx,
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
