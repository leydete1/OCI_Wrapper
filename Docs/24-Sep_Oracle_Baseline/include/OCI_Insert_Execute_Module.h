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
#include "OCI_Request_Response_Types.h"   /* field_value_t - shared with UPDATE */

/* ------------------------------------------------------------------ */
/*  insert_request_t                                                    */
/*  Format-agnostic INSERT request payload - built by Level 1           */
/*  (build_payload_xml()/build_payload_json() in OCI_Level1_Parser.c)   */
/*  and consumed by Level 2 validation, then execute_insert_batch().    */
/*  Moved here from OCI_Request_Response_Types.h now that Insert is     */
/*  actually being refactored, per that header's own convention note.   */
/*                                                                       */
/*  Deliberately no row_number field on insert_row_t - the <row         */
/*  number="N"> attribute (XML) / row_number key (JSON) on the wire is  */
/*  purely for human readability; rows are processed in document/array  */
/*  order, same as the wire examples in                                 */
/*  Data_Manager_Request_Definitions.docx.                              */
/* ------------------------------------------------------------------ */
typedef struct {
    int            field_count;
    field_value_t *fields;      /* one row's worth of field_name/value */
} insert_row_t;

typedef struct {
    char table_name[128];
    char owner[128];
    int  row_count;
    insert_row_t *rows;         /* bulk insert - multiple rows per request.
                                  * row_count capped by ctx->ini->max_bulk_inserts
                                  * - Level 2's job to check and reject early,
                                  * not Level 1's.                              */
} insert_request_t;

/*
 * execute_insert_batch()
 *
 * Main Stage-3 entry point. Calls level2_validate_insert() internally
 * as its own first step (not just trusted to have already run in the
 * caller) - so both the client-facing business insert AND the internal
 * audit-trail insert (OCI_Audit_Trail_Manager.c calls this directly,
 * bypassing the client-facing dispatcher entirely since there's no
 * client request behind it) get the exact same validation for free,
 * with zero extra code needed in the audit module itself.
 *
 * Parameters
 *   ctx  - OCI context (connection + logger)
 *   req  - already-parsed insert_request_t - from Level 1 for a
 *          client-facing request, or built directly by
 *          OCI_Audit_Trail_Manager.c for the internal audit insert.
 *          Not modified - only read.
 *   cfg  - execute_config_t; cfg->xml->OUTPUT_XML always set on
 *          success; cfg->OUTPUT_JSON additionally set when
 *          cfg->ReturnFormat is "JSON" (NULL otherwise, per that
 *          field's own doc comment in OCI_Connection.h)
 *
 * Returns
 *    0  success  - all rows inserted, cfg->xml->OUTPUT_XML set (and
 *                  cfg->OUTPUT_JSON too, if requested)
 *   -1  error    - logged, no partial commit (rolled back)
 */
int execute_insert_batch(oci_context_t    *ctx,
                         insert_request_t *req,
                         execute_config_t *cfg);

#endif /* OCI_INSERT_EXECUTE_MODULE_H */
