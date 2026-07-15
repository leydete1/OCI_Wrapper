/*
 * OCI_Insert_Execute_Module.h
 *
 * Stage 3 - Insert Execute Module
 * --------------------------------
 * Accepts a fully validated <Insert_Template> XML (produced by Stage 1
 * and validated by Stage 2), executes the INSERT against Oracle, and
 * returns a result XML via cfg->xml->OUTPUT_XML.
 *
 * Design mirrors execute_query_batch in reverse:
 *
 *   Stage 1 - Validate    parse XML, validate all rows via Stage 2
 *                         any row fails -> return failure immediately
 *   Stage 2 - Prepare     build INSERT SQL, OCIStmtPrepare2
 *                         detect CLOB/NCLOB -> force execute_count=1
 *   Stage 3 - Bind        setup bind variables per column per type
 *                         scalar: OCIBindArrayOfStruct up to execute_count
 *                         BLOB:   read from file path, OCILobWrite chunked
 *                         CLOB:   inline text or file path, OCILobWrite
 *                         EMPTY_BLOB()/EMPTY_CLOB() for empty values
 *   Stage 4 - Execute     OCIStmtExecute per batch, OCITransCommit
 *   Stage 5 - Result XML  rows inserted, timing, LOB counts
 *   Stage 6 - Cleanup     reverse allocation order, all guards
 *
 * CLOB Array Insert Restriction (mirrors fetch-side guard)
 * ---------------------------------------------------------
 * OCI does not support OCIBindArrayOfStruct for CLOB columns.
 * If any CLOB/NCLOB column is present, execute_count is forced to 1.
 * BLOB array insert works correctly as each locator is individually
 * allocated and strided.
 *
 * BLOB/CLOB input
 * ---------------
 *   BLOB: insert_value = full file path, or empty -> EMPTY_BLOB()
 *   CLOB: insert_value = inline text, or "file://<path>" for file,
 *         or empty -> EMPTY_CLOB()
 */

#ifndef OCI_INSERT_EXECUTE_MODULE_H
#define OCI_INSERT_EXECUTE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/*
 * execute_insert_batch()
 *
 * Main Stage-3 entry point.
 *
 * Parameters
 *   ctx          - OCI context (connection + logger)
 *   template_xml - validated <Insert_Template> XML string
 *   cfg          - execute_config_t; OUTPUT_XML set on success
 *
 * Returns
 *    0  success  - all rows inserted, cfg->xml->OUTPUT_XML set
 *   -1  error    - logged, no partial commit (rolled back)
 */
int execute_insert_batch(oci_context_t    *ctx,
                         const char       *template_xml,
                         execute_config_t *cfg);

#endif /* OCI_INSERT_EXECUTE_MODULE_H */
