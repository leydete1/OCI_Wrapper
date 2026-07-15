/*
 * OCI_Update_Execute_Module.h
 *
 * Stage 3 - Update Execute Module
 * --------------------------------
 * Accepts a validated <Update_Template> XML, executes the UPDATE
 * against Oracle, and returns a result XML via cfg->xml->OUTPUT_XML.
 *
 * Design mirrors OCI_Insert_Execute_Module exactly, with the
 * following differences:
 *
 *   - XML has a <where> block identifying key columns.
 *     These columns form the WHERE clause; all other columns
 *     form the SET clause.
 *   - SQL built as:
 *       UPDATE owner.table
 *       SET    col1=:1, col2=:2, ...
 *       WHERE  key1=:N, key2=:N+1, ...
 *   - BLOB/CLOB: same EMPTY_BLOB()/EMPTY_CLOB() + SELECT FOR UPDATE
 *     pattern as insert (only applies to SET columns, not WHERE).
 *   - Reuses Stage 1 (get_insert_template) and Stage 2
 *     (validate_insert_template) unchanged.
 *
 * XML input layout
 * ----------------
 *   <Update_Template>
 *     <operation>UPDATE</operation>
 *     <table_name>OCI_FIELD_TEST</table_name>
 *     <owner>DATA_MANAGER</owner>
 *     <where>
 *       <key_field>
 *         <field_name>NUMBER_COL</field_name>
 *         <field_type>NUMBER</field_type>
 *         <key_value>42</key_value>
 *       </key_field>
 *     </where>
 *     <row number="1">
 *       <field>
 *         <field_number>1</field_number>
 *         <field_name>VARCHAR2_COL</field_name>
 *         <field_type>VARCHAR2</field_type>
 *         <field_length>100</field_length>
 *         <field_precision>-1</field_precision>
 *         <field_scale>-1</field_scale>
 *         <field_nullable>Y</field_nullable>
 *         <field_default></field_default>
 *         <update_value>Updated text</update_value>
 *       </field>
 *     </row>
 *   </Update_Template>
 */

#ifndef OCI_UPDATE_EXECUTE_MODULE_H
#define OCI_UPDATE_EXECUTE_MODULE_H

#include "OCI_Connection.h"
#include "OCI_Table_Metadata_Module.h"
#include "XML_Helper.h"
#include "logger.h"

/*
 * execute_update_batch()
 *
 * Main Stage-3 entry point for UPDATE.
 *
 * Parameters
 *   ctx          - OCI context (connection + logger)
 *   template_xml - validated <Update_Template> XML string
 *   cfg          - execute_config_t; OUTPUT_XML set on success
 *
 * Returns
 *    0  success  - all rows updated, cfg->xml->OUTPUT_XML set
 *   -1  error    - logged, rolled back
 */
int execute_update_batch(oci_context_t    *ctx,
                          const char       *template_xml,
                          execute_config_t *cfg);

#endif /* OCI_UPDATE_EXECUTE_MODULE_H */
