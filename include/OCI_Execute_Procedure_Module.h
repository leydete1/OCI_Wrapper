/*
 * OCI_Execute_Procedure_Module.h
 *
 * Stored Procedure Execution Module
 * -----------------------------------
 * Executes an Oracle stored procedure or function via an anonymous
 * PL/SQL block and returns a result XML via cfg->xml->OUTPUT_XML.
 *
 * Supported parameter directions
 * --------------------------------
 *   IN        - scalar value passed into the procedure
 *   OUT       - scalar value returned from the procedure
 *   IN_OUT    - scalar value passed in and returned modified
 *
 * Supported parameter types
 * --------------------------
 *   NUMBER    - bound as SQLT_INT  (integer) or SQLT_FLT (float)
 *   INTEGER   - bound as SQLT_INT
 *   VARCHAR2  - bound as SQLT_STR
 *   DATE      - bound as SQLT_STR  with TO_DATE wrapper on IN
 *   TIMESTAMP - bound as SQLT_STR  with TO_TIMESTAMP wrapper on IN
 *   CURSOR    - bound as SQLT_RSET (SYS_REFCURSOR)
 *               OUT only; triggers full batch fetch producing XML rows
 *               identical to execute_query_batch output
 *
 * A procedure may have any combination of the above.  Multiple CURSOR
 * OUT parameters are supported - each produces its own <resultset>
 * block in the output XML, identified by the parameter name.
 *
 * PL/SQL block generated internally
 * -----------------------------------
 * Given a procedure MY_PKG.GET_DATA with parameters:
 *   P_DEPT_ID    IN  NUMBER
 *   P_STATUS     OUT INTEGER
 *   P_RESULTS    OUT SYS_REFCURSOR
 *
 * The module generates and executes:
 *   BEGIN MY_PKG.GET_DATA(:P_DEPT_ID, :P_STATUS, :P_RESULTS); END;
 *
 * XML input layout
 * ----------------
 *   <Procedure_Template>
 *     <operation>EXECUTE_PROCEDURE</operation>
 *     <procedure_name>MY_PKG.GET_DATA</procedure_name>
 *     <owner></owner>                    <!-- optional schema prefix  -->
 *     <parameters>
 *       <parameter>
 *         <param_name>P_DEPT_ID</param_name>
 *         <param_type>NUMBER</param_type>
 *         <param_direction>IN</param_direction>
 *         <param_value>10</param_value>
 *       </parameter>
 *       <parameter>
 *         <param_name>P_STATUS</param_name>
 *         <param_type>INTEGER</param_type>
 *         <param_direction>OUT</param_direction>
 *         <param_value></param_value>
 *       </parameter>
 *       <parameter>
 *         <param_name>P_RESULTS</param_name>
 *         <param_type>CURSOR</param_type>
 *         <param_direction>OUT</param_direction>
 *         <param_value></param_value>
 *       </parameter>
 *     </parameters>
 *   </Procedure_Template>
 *
 * Result XML layout (cfg->xml->OUTPUT_XML on success)
 * ----------------------------------------------------
 *   <?xml version="1.0" encoding="UTF-8"?>
 *   <Procedure_Result>
 *     <execution>
 *       <procedure_name>MY_PKG.GET_DATA</procedure_name>
 *       <execution_time>0.003412</execution_time>
 *       <out_parameters>
 *         <parameter>
 *           <param_name>P_STATUS</param_name>
 *           <param_type>INTEGER</param_type>
 *           <param_value>0</param_value>
 *         </parameter>
 *       </out_parameters>
 *     </execution>
 *     <resultset param_name="P_RESULTS">
 *       <row number="1">
 *         <field><field_name>EMP_ID</field_name>...</field>
 *       </row>
 *     </resultset>
 *   </Procedure_Result>
 *
 * Integration with Test_XML_Runner
 * ---------------------------------
 * Add to Test_XML_Runner.c:
 *
 *   #include "OCI_Execute_Procedure_Module.h"
 *
 *   static int dispatch_procedure(oci_context_t *ctx,
 *                                  const char    *filename,
 *                                  const char    *xml)
 *   {
 *       execute_config_t cfg;
 *       memset(&cfg, 0, sizeof(cfg));
 *       int rc = execute_procedure(ctx, xml, &cfg);
 *       if (rc == 0) { ... log PASS ... }
 *       else         { ... log FAIL ... }
 *       if (cfg.xml) { free(cfg.xml->OUTPUT_XML); free(cfg.xml); }
 *       return rc;
 *   }
 *
 *   // In process_xml_file dispatch block:
 *   else if (strcmp(operation, "EXECUTE_PROCEDURE") == 0)
 *       rc = dispatch_procedure(ctx, filename, xml);
 *
 * Compile additions
 * -----------------
 *   OCI_Execute_Procedure_Module.c
 */

#ifndef OCI_EXECUTE_PROCEDURE_MODULE_H
#define OCI_EXECUTE_PROCEDURE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/*
 * execute_procedure()
 *
 * Main entry point.
 *
 * Parameters
 *   ctx          - OCI context (connection + logger)
 *   template_xml - <Procedure_Template> XML string
 *   cfg          - execute_config_t; OUTPUT_XML set on success
 *
 * Returns
 *    0  success  - cfg->xml->OUTPUT_XML populated
 *   -1  error    - logged; no commit issued (procedures manage their
 *                  own transactions internally)
 */
int execute_procedure(oci_context_t    *ctx,
                      const char       *template_xml,
                      execute_config_t *cfg);

#endif /* OCI_EXECUTE_PROCEDURE_MODULE_H */
