/*
 * OCI_Delete_Execute_Module.h
 *
 * Stage 3 - Delete Execute Module
 * --------------------------------
 * Accepts a <Delete_Template> XML, executes a DELETE against Oracle,
 * and returns a result XML via cfg->xml->OUTPUT_XML.
 *
 * Design
 * ------
 * Based on OCI_Update_Execute_Module with the SET clause removed
 * entirely.  Only the <where> block is needed - there are no row
 * columns, no LOB handling, and no column metadata query.
 *
 * The WHERE key columns support the same date/timestamp type wrappers
 * as the update module (TO_DATE, TO_TIMESTAMP, TO_YMINTERVAL,
 * TO_DSINTERVAL) so key columns of any scalar Oracle type work
 * correctly without relying on NLS session settings.
 *
 * Multiple <key_field> entries produce a compound WHERE clause joined
 * with AND:
 *
 *   DELETE FROM owner.table
 *   WHERE  key1 = :1
 *   AND    key2 = TO_DATE(:2,'YYYY-MM-DD HH24:MI:SS')
 *   AND    key3 = :3
 *
 * XML input layout
 * ----------------
 *   <Delete_Template>
 *     <operation>DELETE</operation>
 *     <table_name>OCI_FIELD_TEST</table_name>
 *     <owner>DATA_MANAGER</owner>           <!-- optional: auto-resolved -->
 *     <where>
 *       <key_field>
 *         <field_name>NUMBER_COL</field_name>
 *         <field_type>NUMBER</field_type>
 *         <key_value>42</key_value>
 *       </key_field>
 *       <!-- add further <key_field> blocks for compound keys -->
 *     </where>
 *   </Delete_Template>
 *
 * Result XML (cfg->xml->OUTPUT_XML on success)
 * --------------------------------------------
 *   <execution>
 *     <operation>DELETE</operation>
 *     <table_name>...</table_name>
 *     <owner>...</owner>
 *     <rows_deleted>N</rows_deleted>
 *     <execution_time>0.000123</execution_time>
 *   </execution>
 *
 * Safety guards
 * -------------
 *   - An empty <where> block (no <key_field> entries) is rejected
 *     before any OCI call - prevents accidental full-table deletes.
 *   - max_bulk_inserts (reused ini setting) caps the number of
 *     discrete delete statements that may be issued in one call.
 *     For a single-key delete this is always 1.
 *   - Any OCI error triggers an immediate rollback before returning -1.
 *
 * Integration with Test_XML_Runner
 * ---------------------------------
 * Add "DELETE" to the dispatch table in Test_XML_Runner.c:
 *
 *   #include "OCI_Delete_Execute_Module.h"
 *   ...
 *   else if (strcmp(operation, "DELETE") == 0)
 *       rc = dispatch_delete(ctx, filename, xml);
 *
 * where dispatch_delete() follows the same pattern as dispatch_update().
 *
 * Compile additions (append to existing gcc command)
 * ---------------------------------------------------
 *   OCI_Delete_Execute_Module.c
 */

#ifndef OCI_DELETE_EXECUTE_MODULE_H
#define OCI_DELETE_EXECUTE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/*
 * execute_delete_batch()
 *
 * Main entry point for DELETE.
 *
 * Parameters
 *   ctx          - OCI context (connection + logger)
 *   template_xml - <Delete_Template> XML string
 *   cfg          - execute_config_t; OUTPUT_XML set on success
 *
 * Returns
 *    0  success  - rows deleted (may be 0 if WHERE matched nothing),
 *                  cfg->xml->OUTPUT_XML populated
 *   -1  error    - logged, transaction rolled back
 */
int execute_delete_batch(oci_context_t    *ctx,
                         const char       *template_xml,
                         execute_config_t *cfg);

#endif /* OCI_DELETE_EXECUTE_MODULE_H */
