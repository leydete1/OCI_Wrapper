/*
 * OCI_Execute_Procedure_Module.h
 *
 * Stored Procedure Execution Module
 * -----------------------------------
 * Executes an Oracle stored procedure or function via an anonymous
 * PL/SQL block and returns a result via cfg->xml->OUTPUT_XML /
 * cfg->OUTPUT_JSON.
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
 *               OUT only; triggers full batch fetch producing a
 *               resultset fragment identical to execute_query_batch's
 *               own output shape
 *
 * A procedure may have any combination of the above. Multiple CURSOR
 * OUT parameters are supported - each produces its own resultset in
 * the response, identified by parameter name.
 *
 * 2026-07-29 refactor - new request-envelope architecture
 * ---------------------------------------------------------
 * Moved onto the same insert_request_t-style architecture as every
 * other CRUD module: level2_validate_procedure() is called internally
 * as this function's own first step (same reasoning as
 * execute_insert_batch(), see its own doc comment in
 * OCI_Insert_Execute_Module.h), and execute_procedure() now takes an
 * already-parsed execute_procedure_request_t rather than a raw XML
 * string.
 *
 * Validation here is deliberately lighter than INSERT/UPDATE/DELETE's -
 * there is no metadata_cache equivalent for a procedure's own parameter
 * signature (Oracle doesn't expose that the way it exposes table
 * columns, and this project has no lookup for it), so
 * level2_validate_procedure() only checks structural correctness:
 * procedure_name non-empty, param_count in range, every param_type is
 * one of the recognised set, every direction is valid, and CURSOR is
 * always OUT (never IN or IN_OUT - a cursor can't be passed in).
 *
 * No audit trail integration, by design (2026-07-29 decision): a
 * procedure is a black box from Data_Manager's perspective - it has no
 * visibility into what the procedure's own PL/SQL actually does
 * internally, so there is nothing meaningful to hold Data_Manager
 * accountable for beyond the fact that the call was made (which the
 * general request/response logging already captures). This is
 * different from INSERT/UPDATE/DELETE, where Data_Manager itself
 * builds and executes the actual DML and so bears direct
 * responsibility for what changed.
 *
 * The bind/execute/cursor-fetch machinery below (build_plsql_block(),
 * bind_parameters(), fetch_cursor_to_xml()) is unchanged by this
 * refactor - already fully decoupled from the XML/JSON parsing layer,
 * exactly like INSERT's own bind/execute machinery was.
 *
 * New request-envelope XML shape (see
 * Data_Manager_Request_Definitions.docx for the full spec):
 *   <request version="1.0">
 *     <transaction required="1">
 *       <operation type="EXECUTE_PROCEDURE">
 *         <procedure_name>MY_PKG.GET_DATA</procedure_name>
 *         <owner></owner>
 *         <parameters>
 *           <parameter>
 *             <param_name>P_DEPT_ID</param_name>
 *             <param_type>NUMBER</param_type>
 *             <param_direction>IN</param_direction>
 *             <param_value>10</param_value>
 *           </parameter>
 *           <parameter>
 *             <param_name>P_RESULTS</param_name>
 *             <param_type>CURSOR</param_type>
 *             <param_direction>OUT</param_direction>
 *             <param_value></param_value>
 *           </parameter>
 *         </parameters>
 *       </operation>
 *     </transaction>
 *   </request>
 */

#ifndef OCI_EXECUTE_PROCEDURE_MODULE_H
#define OCI_EXECUTE_PROCEDURE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  MAX_PROC_PARAMS                                                     */
/*  Public - shared between the public execute_procedure_request_t      */
/*  below and level2_validate_procedure()'s own range check             */
/*  (OCI_Level2_Parser.c), as well as this module's own internal         */
/*  defense-in-depth check.                                              */
/* ------------------------------------------------------------------ */
#define MAX_PROC_PARAMS 64

/* ------------------------------------------------------------------ */
/*  param_direction_t                                                    */
/*  Public - shared between execute_procedure_request_t's own           */
/*  procedure_param_t (below) and the private, internal proc_param_t    */
/*  in OCI_Execute_Procedure_Module.c, avoiding two parallel enums for   */
/*  the same three values.                                               */
/* ------------------------------------------------------------------ */
typedef enum {
    PARAM_DIR_IN     = 0,
    PARAM_DIR_OUT    = 1,
    PARAM_DIR_IN_OUT = 2
} param_direction_t;

/* ------------------------------------------------------------------ */
/*  procedure_param_t                                                   */
/*  One request-side parameter - param_value carries the IN value (or   */
/*  stays empty for a pure OUT param going in, including CURSOR, which   */
/*  is always OUT and never has a meaningful param_value on the way in). */
/* ------------------------------------------------------------------ */
typedef struct {
    char               param_name [128];
    char               param_type [32];    /* NUMBER, INTEGER, VARCHAR2,
                                             * DATE, TIMESTAMP, CURSOR    */
    param_direction_t  direction;
    char               param_value[4096];
} procedure_param_t;

/* ------------------------------------------------------------------ */
/*  execute_procedure_request_t                                         */
/*  Format-agnostic EXECUTE_PROCEDURE request payload - built by        */
/*  Level 1 (build_payload_xml()/build_payload_json() in                */
/*  OCI_Level1_Parser.c) and consumed by Level 2 validation, then       */
/*  execute_procedure().                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    char               procedure_name[128];
    char               owner[128];
    int                param_count;
    procedure_param_t *parameters;
} execute_procedure_request_t;

/* ------------------------------------------------------------------ */
/*  procedure_resultset_t                                               */
/*  One CURSOR OUT parameter's fetched result - resultset_xml_fragment  */
/*  is the same reused shape as dml_response_t's own field of the same   */
/*  name (see OCI_Request_Response_Types.h): a <resultset>...</resultset>
 *  fragment identical to what response_write_xml() produces for a      */
/*  plain SELECT, ready to splice into the envelope the same way.       */
/* ------------------------------------------------------------------ */
typedef struct {
    char  param_name[128];
    char *resultset_xml_fragment;
} procedure_resultset_t;

/* ------------------------------------------------------------------ */
/*  execute_procedure_response_t                                        */
/*  out_parameters holds every OUT/IN_OUT scalar param's post-execute    */
/*  value (CURSOR params are never included here - they produce a       */
/*  resultset instead, via resultsets[] below). resultset_count is the   */
/*  number of CURSOR OUT params that actually produced a resultset -     */
/*  zero, one, or several.                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    char                    procedure_name[128];
    double                  execution_time_seconds;
    int                     out_param_count;
    procedure_param_t      *out_parameters;
    int                     resultset_count;
    procedure_resultset_t  *resultsets;
} execute_procedure_response_t;

/*
 * execute_procedure()
 *
 * Main entry point. Calls level2_validate_procedure() internally as
 * its own first step - see this file's own top comment.
 *
 * Parameters
 *   ctx  - OCI context (connection + logger)
 *   req  - already-parsed execute_procedure_request_t - from Level 1
 *          for a client-facing request. Not modified - only read.
 *   cfg  - execute_config_t; cfg->xml->OUTPUT_XML always set on
 *          success; cfg->OUTPUT_JSON additionally set when
 *          cfg->ReturnFormat is "JSON" (NULL otherwise, per that
 *          field's own doc comment in OCI_Connection.h)
 *
 * Returns
 *    0  success  - cfg->xml->OUTPUT_XML populated (and
 *                  cfg->OUTPUT_JSON too, if requested)
 *   -1  error    - logged; no commit issued (procedures manage their
 *                  own transactions internally)
 */
int execute_procedure(oci_context_t                *ctx,
                       execute_procedure_request_t  *req,
                       execute_config_t             *cfg);

#endif /* OCI_EXECUTE_PROCEDURE_MODULE_H */
