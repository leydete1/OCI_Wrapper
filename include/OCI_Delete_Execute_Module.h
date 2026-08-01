/*
 * OCI_Delete_Execute_Module.h
 *
 * Stage 3 - Delete Execute Module
 * --------------------------------
 * Accepts an already-validated delete_request_t (built by Level 1),
 * executes a DELETE against Oracle, and returns a result via
 * cfg->xml->OUTPUT_XML / cfg->OUTPUT_JSON.
 *
 * level2_validate_delete() is called internally as this function's own
 * first step - same reasoning as execute_insert_batch()/
 * execute_update_batch(), see execute_insert_batch()'s own doc comment
 * in OCI_Insert_Execute_Module.h.
 *
 * Design - genuinely simpler than INSERT/UPDATE
 * ----------------------------------------------
 * DELETE has no SET clause, no per-row values, no LOB handling - just
 * a WHERE clause identifying which rows to remove. Structurally a
 * subset of UPDATE's WHERE handling (see update_request_t in
 * OCI_Update_Execute_Module.h) with the SET side removed entirely.
 *
 *   DELETE FROM owner.table
 *   WHERE  key1 = :1
 *   AND    key2 = TO_DATE(:2,'YYYY-MM-DD HH24:MI:SS')
 *   AND    key3 = :3
 *
 * Unlike the pre-refactor version of this module, WHERE key column
 * types are resolved via metadata_cache (level2_validate_delete() /
 * build_delete_ctx_from_request()), never trusted from the client -
 * the new where_key_t carries no type information at all, matching
 * the "client sends field_name+value, server resolves everything
 * else" design used throughout this project.
 *
 * Audit trail - added as part of this refactor, not carried over
 * ------------------------------------------------------------------
 * The pre-refactor version of this module had NO audit trail
 * integration at all. Added here per 2026-07-26 decision: a before-
 * image SELECT (audit_trail_fetch_before_image(), reused unchanged
 * from UPDATE) captures the WHERE-key columns' values before the
 * DELETE executes, and audit_trail_insert_delete() (new,
 * OCI_Audit_Trail_Manager.c) writes one AUDIT_TRAIL row per matched
 * row per WHERE-key column, unconditionally (no diff against a "new"
 * value the way UPDATE's audit does - there is no new value, the row
 * is simply gone). This happens BEFORE the actual DELETE statement
 * runs, so the attempt is captured even if Oracle itself then rejects
 * the DELETE for lack of privilege - in a GxP context, database
 * records typically aren't deleted at all (a status change to
 * "CLOSED"/similar is used instead), so DATA_MANAGER's own Oracle
 * account may have no DELETE privilege whatsoever; the audit record
 * of the attempt still needs to exist regardless of whether the
 * DELETE itself succeeds.
 *
 * New request-envelope XML shape (see
 * Data_Manager_Request_Definitions.docx for the full spec):
 *   <request version="1.0">
 *     <transaction required="1">
 *       <operation type="DELETE">
 *         <table_name>OCI_FIELD_TEST</table_name>
 *         <owner>DATA_MANAGER</owner>
 *         <where>
 *           <key><field_name>NUMBER_COL</field_name><key_value>99</key_value></key>
 *         </where>
 *       </operation>
 *     </transaction>
 *   </request>
 *
 * No WHERE-clause echo on the response (dml_response_t) - per
 * 2026-07-15 decision documented in Data_Manager_Request_Definitions.docx,
 * removed for consistency with UPDATE (neither echoes it now).
 */

#ifndef OCI_DELETE_EXECUTE_MODULE_H
#define OCI_DELETE_EXECUTE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"
#include "OCI_Request_Response_Types.h"   /* where_key_t - shared with UPDATE */

/* ------------------------------------------------------------------ */
/*  delete_request_t                                                    */
/*  Format-agnostic DELETE request payload - built by Level 1           */
/*  (build_payload_xml()/build_payload_json() in OCI_Level1_Parser.c)   */
/*  and consumed by Level 2 validation, then execute_delete_batch().    */
/*  Moved here from OCI_Request_Response_Types.h now that Delete is     */
/*  actually being refactored, per that header's own convention note -  */
/*  matching insert_request_t/update_request_t's own moves.             */
/* ------------------------------------------------------------------ */
typedef struct {
    char table_name[128];
    char owner[128];
    int  key_count;
    where_key_t *keys;          /* WHERE clause - AND'd together. No SET
                                  * clause - DELETE has nothing else to
                                  * carry, unlike UPDATE.                 */
} delete_request_t;

/*
 * execute_delete_batch()
 *
 * Main Stage-3 entry point for DELETE. Calls level2_validate_delete()
 * internally as its own first step - see this file's own top comment.
 *
 * Parameters
 *   ctx  - OCI context (connection + logger)
 *   req  - already-parsed delete_request_t - from Level 1 for a
 *          client-facing request. Not modified - only read.
 *   cfg  - execute_config_t; cfg->xml->OUTPUT_XML always set on
 *          success; cfg->OUTPUT_JSON additionally set when
 *          cfg->ReturnFormat is "JSON" (NULL otherwise, per that
 *          field's own doc comment in OCI_Connection.h)
 *
 * Returns
 *    0  success  - rows deleted (may be 0 if WHERE matched nothing),
 *                  cfg->xml->OUTPUT_XML set (and cfg->OUTPUT_JSON too,
 *                  if requested). The audit trail entry for the
 *                  attempt is written regardless of this return value
 *                  - see this file's own top comment on why.
 *   -1  error    - logged, rolled back
 */
int execute_delete_batch(oci_context_t     *ctx,
                          delete_request_t  *req,
                          execute_config_t  *cfg);

#endif /* OCI_DELETE_EXECUTE_MODULE_H */
