/*
 * OCI_Update_Execute_Module.h
 *
 * Stage 3 - Update Execute Module
 * --------------------------------
 * Accepts an already-validated update_request_t (built by Level 1, or
 * directly by another module for an internal update - same pattern as
 * OCI_Insert_Execute_Module), executes the UPDATE against Oracle, and
 * returns a result via cfg->xml->OUTPUT_XML / cfg->OUTPUT_JSON.
 *
 * level2_validate_update() is called internally as this function's own
 * first step, not just trusted to have already run in the caller -
 * same reasoning as execute_insert_batch(), see its own doc comment in
 * OCI_Insert_Execute_Module.h.
 *
 * Design mirrors OCI_Insert_Execute_Module, with the following
 * differences:
 *
 *   - No per-row concept for the SET clause - an UPDATE has exactly
 *     one SET list applied to however many rows match the WHERE
 *     clause, unlike INSERT's insert_row_t[] (see update_request_t
 *     below - flat field_count/fields, not row_count/rows).
 *   - update_request_t.keys[] (where_key_t) identifies which rows to
 *     update; update_request_t.fields[] (field_value_t - the same
 *     struct INSERT uses) is the SET clause.
 *   - SQL built as:
 *       UPDATE owner.table
 *       SET    col1=:1, col2=:2, ...
 *       WHERE  key1=:N, key2=:N+1, ...
 *   - BLOB/CLOB: same EMPTY_BLOB()/EMPTY_CLOB() + SELECT FOR UPDATE
 *     pattern as insert (only applies to SET columns, not WHERE).
 *   - Audit trail: a before-image SELECT
 *     (audit_trail_fetch_before_image(), in OCI_Audit_Trail_Manager.c)
 *     captures old column values before the UPDATE executes; only
 *     columns that actually changed get an AUDIT_TRAIL row
 *     (audit_trail_insert_update(), which diffs old vs new then
 *     delegates to audit_trail_insert() - unchanged by this refactor,
 *     already fixed as part of Insert's).
 *
 * New request-envelope XML shape (see
 * Data_Manager_Request_Definitions.docx for the full spec):
 *   <request version="1.0">
 *     <transaction required="1">
 *       <operation type="UPDATE">
 *         <table_name>OCI_FIELD_TEST</table_name>
 *         <owner>DATA_MANAGER</owner>
 *         <where>
 *           <key><field_name>NUMBER_COL</field_name><key_value>42</key_value></key>
 *         </where>
 *         <set>
 *           <field><field_name>VARCHAR2_COL</field_name><value>Updated text</value></field>
 *         </set>
 *       </operation>
 *     </transaction>
 *   </request>
 */

#ifndef OCI_UPDATE_EXECUTE_MODULE_H
#define OCI_UPDATE_EXECUTE_MODULE_H

#include "OCI_Connection.h"
#include "OCI_Table_Metadata_Module.h"
#include "XML_Helper.h"
#include "logger.h"
#include "OCI_Request_Response_Types.h"   /* field_value_t, where_key_t -
                                            * shared with INSERT/DELETE   */

/* ------------------------------------------------------------------ */
/*  update_request_t                                                    */
/*  Format-agnostic UPDATE request payload - built by Level 1           */
/*  (build_payload_xml()/build_payload_json() in OCI_Level1_Parser.c)   */
/*  and consumed by Level 2 validation, then execute_update_batch().    */
/*  Moved here from OCI_Request_Response_Types.h now that Update is     */
/*  actually being refactored, per that header's own convention note -  */
/*  where_key_t stays there since DELETE (not yet refactored) still     */
/*  needs it.                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char table_name[128];
    char owner[128];
    int  key_count;
    where_key_t   *keys;        /* WHERE clause - AND'd together        */
    int            field_count;
    field_value_t *fields;      /* SET clause                            */
} update_request_t;

/*
 * execute_update_batch()
 *
 * Main Stage-3 entry point for UPDATE. Calls level2_validate_update()
 * internally as its own first step - see this file's own top comment.
 *
 * Parameters
 *   ctx  - OCI context (connection + logger)
 *   req  - already-parsed update_request_t - from Level 1 for a
 *          client-facing request. Not modified - only read.
 *   cfg  - execute_config_t; cfg->xml->OUTPUT_XML always set on
 *          success; cfg->OUTPUT_JSON additionally set when
 *          cfg->ReturnFormat is "JSON" (NULL otherwise, per that
 *          field's own doc comment in OCI_Connection.h)
 *
 * Returns
 *    0  success  - all matching rows updated, cfg->xml->OUTPUT_XML set
 *                  (and cfg->OUTPUT_JSON too, if requested)
 *   -1  error    - logged, rolled back
 */
int execute_update_batch(oci_context_t     *ctx,
                          update_request_t  *req,
                          execute_config_t  *cfg);

#endif /* OCI_UPDATE_EXECUTE_MODULE_H */
