/*
 * OCI_DDL_Execute_Module.h
 *
 * Independent DDL Module proposal (03-Sep) - Execute stage
 * ------------------------------------------------------------
 * Shared, generic executor for all six DDL operations (CREATE_USER,
 * GRANT, CREATE_TABLE, DROP_TABLE, CREATE_VIEW, CREATE_PROCEDURE).
 * One function does the actual OCI work; each operation's dispatcher
 * function (dispatcher.c) supplies the already-validated DDL text via
 * that module's own build_*_ddl_text() and gets a uniform result back.
 *
 * Why one shared executor instead of six
 * -----------------------------------------
 *   Every one of the six tgen stages already reduces its request down
 *   to a single, self-contained DDL statement string with no bind
 *   variables (build_create_user_ddl_text(), build_grant_ddl_text(),
 *   etc.). Executing that string is identical work regardless of
 *   which operation produced it - OCIStmtPrepare2 + OCIStmtExecute,
 *   no binds, no rows returned. Six copies of that would be six copies
 *   of the same fifteen lines with different log labels.
 *
 * Oracle DDL auto-commit - the one thing that makes this different
 * from every other execute module in this codebase
 * -----------------------------------------------------------------
 *   Every other execute module (OCI_Insert_Execute_Module.c etc.)
 *   respects ctx->active_tx: if the client has an explicit managed
 *   transaction open (tx_begin()), those modules leave the work
 *   uncommitted so the client can batch it and commit/rollback the
 *   whole unit atomically later.
 *
 *   DDL cannot honour that contract. Oracle implicitly commits before
 *   AND after every DDL statement, unconditionally - there is no OCI
 *   flag or mode that suppresses this. If a client had other DML
 *   pending inside an open tx_begin()/tx_commit() unit and this
 *   executor ran a DDL statement on the same session, that pending
 *   DML would be silently and permanently committed as a side effect,
 *   with no way for the client to still roll it back.
 *
 *   execute_ddl_statement() therefore REFUSES to run at all when
 *   ctx->active_tx is set - returns an error result immediately,
 *   executes nothing. This is a hard safety boundary, not a
 *   configurable option: it protects transactional integrity for
 *   business DML that has nothing to do with the DDL request itself.
 *
 * DROP_TABLE confirmation (Terry, 07-Sep)
 * ------------------------------------------
 *   DROP_TABLE is irreversible, so its dispatcher function
 *   (dispatch_drop_table_new() in dispatcher.c) requires
 *   drop_table_request_t.confirm == 1 before ever calling
 *   execute_ddl_statement() for it. This executor itself has no
 *   opinion on which operations need confirmation - that policy lives
 *   at the dispatch layer, one operation at a time, since it's a
 *   per-operation product decision, not a property of "executing
 *   DDL" in general.
 */

#ifndef OCI_DDL_EXECUTE_MODULE_H
#define OCI_DDL_EXECUTE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

#define DDL_EXEC_ERROR_MSG_LEN   512

/* ------------------------------------------------------------------ */
/*  ddl_execution_result_t                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    int    success;                            /* 1 = executed OK      */
    char   error_message[DDL_EXEC_ERROR_MSG_LEN]; /* "" when success   */
    double execution_time_seconds;
} ddl_execution_result_t;

/*
 * execute_ddl_statement()
 *
 * Executes ddl_text (a single, complete, already-validated DDL
 * statement with no bind variables) against ctx's current session.
 * operation_label is used only for logging (e.g. "CREATE_USER",
 * "DROP_TABLE") via ctx->ddl_logger.
 *
 * is_plsql_block MUST be 1 for CREATE PROCEDURE/FUNCTION/TRIGGER/
 * PACKAGE (BODY) text - anything whose body is a PL/SQL block ending
 * in "END [name];". For those, the final ';' is NOT an optional
 * SQL*Plus-style terminator - it's the syntactically mandatory close
 * of the END clause, and stripping it produces PLS-00103 ("end-of-
 * file when expecting ;"), discovered against a live instance
 * 07-Sep/08-Sep. is_plsql_block MUST be 0 for every other DDL
 * operation (CREATE_USER, GRANT, CREATE_TABLE, DROP_TABLE,
 * CREATE_VIEW) - for those, the trailing ';' IS the optional
 * SQL*Plus-style terminator the six build_*_ddl_text() functions
 * append for human-readable display, and OCIStmtPrepare2/
 * OCIStmtExecute reject it if left in (ORA-03048/ORA-00922 depending
 * on grammar context - also discovered against a live instance).
 *
 * Refuses to run - returns 0 with result->success = 0 and an
 * explanatory error_message, executes nothing - when:
 *   - ctx, ddl_text, or result is NULL
 *   - ctx->active_tx is set (see header doc comment on DDL auto-commit)
 *
 * Returns 0 on success (result->success will be 1) or on a handled
 * OCI failure (result->success will be 0, result->error_message set
 * from OCIErrorGet). Returns -1 only for the invalid-argument case
 * above (ctx/ddl_text/result NULL) - a caller bug, not a DDL failure.
 */
int execute_ddl_statement(oci_context_t            *ctx,
                           const char               *ddl_text,
                           const char               *operation_label,
                           int                       is_plsql_block,
                           ddl_execution_result_t   *result);

/*
 * get_ddl_execution_response_xml() / get_ddl_execution_response_json()
 *
 * Render a ddl_execution_result_t as the client-facing response body
 * for a DDL execute call. Shape (XML):
 *
 *   <DDL_Execution_Result>
 *     <operation>CREATE_USER</operation>
 *     <status>SUCCESS</status>              (or FAILED)
 *     <executed_ddl>...</executed_ddl>
 *     <execution_time>0.012345</execution_time>
 *     <error_message>...</error_message>    (only present when FAILED)
 *   </DDL_Execution_Result>
 *
 * JSON uses the same fields, snake_case keys, execution_time as a
 * real JSON number - same convention response_write_dml_json() uses
 * for rows_inserted/execution_time (OCI_Response_Writer.h).
 *
 * Returns a malloc'd string the caller must free(), or NULL if ctx,
 * operation_label, ddl_text, or result is NULL.
 */
char *get_ddl_execution_response_xml (oci_context_t                  *ctx,
                                       const char                     *operation_label,
                                       const char                     *ddl_text,
                                       const ddl_execution_result_t   *result);
char *get_ddl_execution_response_json(oci_context_t                  *ctx,
                                       const char                     *operation_label,
                                       const char                     *ddl_text,
                                       const ddl_execution_result_t   *result);

#endif /* OCI_DDL_EXECUTE_MODULE_H */
