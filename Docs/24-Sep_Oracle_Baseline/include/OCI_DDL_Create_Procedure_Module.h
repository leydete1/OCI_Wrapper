/*
 * OCI_DDL_Create_Procedure_Module.h
 *
 * Independent DDL Module - Create Procedure (sixth operation)
 * -------------------------------------------------------------
 * Sixth and final concrete operation of the Independent DDL Module
 * proposal (03-Sep). Same three-stage split as the other five DDL
 * modules:
 *
 *   Definition  - parse_create_procedure_request()
 *   tgen        - get_create_procedure_template()
 *   Validation  - validate_create_procedure_request()
 *
 * Same scope decisions carried forward:
 *   - tgen/preview only - no execute module yet.
 *   - No live database lookups.
 *   - No PL/SQL parsing of <body> - same reasoning as
 *     OCI_DDL_Create_View_Module.h not parsing <query>: this codebase
 *     doesn't parse SQL/PL/SQL text anywhere, it passes it straight
 *     to OCI at execution time. validate_create_procedure_request()
 *     only checks that a body was given and that it's within a sane
 *     length - it is not a PL/SQL compiler.
 *
 * Parameter scope
 * ------------------
 *   Each parameter: name, data_type, mode (IN / OUT / IN OUT),
 *   optional default_value (IN parameters only - Oracle doesn't allow
 *   defaults on OUT or IN OUT parameters, checked in validation).
 *   Recognised data types for PL/SQL parameters (no length/precision
 *   - Oracle doesn't allow constrained types in a parameter list):
 *   VARCHAR2, NUMBER, DATE, TIMESTAMP, BOOLEAN, PLS_INTEGER, CLOB,
 *   BLOB.
 */

#ifndef OCI_DDL_CREATE_PROCEDURE_MODULE_H
#define OCI_DDL_CREATE_PROCEDURE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define MAX_PROCEDURE_PARAMETERS   32
#define PROCEDURE_IDENTIFIER_LEN   128
#define PARAMETER_DATA_TYPE_LEN    16
#define PARAMETER_MODE_LEN         8
#define PARAMETER_DEFAULT_LEN      128
#define PROCEDURE_BODY_LEN         8000  /* matches the LOG_MESSAGE-style *
                                            * text field ceiling used       *
                                            * elsewhere in this proposal's   *
                                            * own DDL fixtures                */

/* ------------------------------------------------------------------ */
/*  ddl_procedure_param_t                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    char name          [PROCEDURE_IDENTIFIER_LEN];
    char data_type     [PARAMETER_DATA_TYPE_LEN];  /* VARCHAR2, NUMBER,   *
                                                     * DATE, TIMESTAMP,     *
                                                     * BOOLEAN,             *
                                                     * PLS_INTEGER, CLOB,   *
                                                     * BLOB                  */
    char mode          [PARAMETER_MODE_LEN];        /* "IN" (default),    *
                                                     * "OUT", "IN OUT"      */
    char default_value [PARAMETER_DEFAULT_LEN];     /* "" = no DEFAULT -  *
                                                     * IN parameters only,  *
                                                     * checked in           *
                                                     * validation            */
} ddl_procedure_param_t;

/* ------------------------------------------------------------------ */
/*  create_procedure_request_t                                          */
/*  Parsed from the <CREATE_PROCEDURE> operation block.                 */
/* ------------------------------------------------------------------ */
typedef struct {
    char procedure_name [PROCEDURE_IDENTIFIER_LEN];
    char owner            [PROCEDURE_IDENTIFIER_LEN]; /* "" = auto-      *
                                                        * resolve, same    *
                                                        * convention as    *
                                                        * the other DDL    *
                                                        * modules           */
    int  replace;                                     /* 0/1 - OR REPLACE */

    int                parameter_count;
    ddl_procedure_param_t  parameters[MAX_PROCEDURE_PARAMETERS];

    char body[PROCEDURE_BODY_LEN];  /* the executable statements between *
                                      * BEGIN and END - see header doc     *
                                      * comment on why this isn't parsed   */
} create_procedure_request_t;

/* ------------------------------------------------------------------ */
/*  Validation result codes - same style as the other DDL modules       */
/* ------------------------------------------------------------------ */
typedef enum {
    PROCEDURE_FIELD_VALID = 0,
    PROCEDURE_NAME_INVALID,
    PROCEDURE_OWNER_INVALID,
    PROCEDURE_TOO_MANY_PARAMETERS,
    PROCEDURE_PARAM_NAME_INVALID,
    PROCEDURE_PARAM_NAME_DUPLICATE,
    PROCEDURE_PARAM_TYPE_INVALID,
    PROCEDURE_PARAM_MODE_INVALID,
    PROCEDURE_PARAM_DEFAULT_NOT_ALLOWED,  /* DEFAULT on an OUT/IN OUT      *
                                            * parameter                     */
    PROCEDURE_BODY_MISSING,
    PROCEDURE_BODY_TOO_LONG
} procedure_validation_result_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * parse_create_procedure_request()
 *
 * Parses a <CREATE_PROCEDURE> operation block XML:
 *
 *   <operation type="CREATE_PROCEDURE">
 *       <procedure_name>LOG_MIGRATION_EVENT</procedure_name>
 *       <owner>HR</owner>
 *       <replace>1</replace>
 *       <parameters>
 *           <parameter>
 *               <name>P_MESSAGE</name>
 *               <data_type>VARCHAR2</data_type>
 *               <mode>IN</mode>
 *           </parameter>
 *           <parameter>
 *               <name>P_LOG_ID</name>
 *               <data_type>NUMBER</data_type>
 *               <mode>OUT</mode>
 *           </parameter>
 *       </parameters>
 *       <body>
 *           INSERT INTO HR.MIGRATION_LOG (LOG_ID, LOG_MESSAGE)
 *           VALUES (MIGRATION_LOG_SEQ.NEXTVAL, P_MESSAGE)
 *           RETURNING LOG_ID INTO P_LOG_ID;
 *       </body>
 *   </operation>
 *
 * <procedure_name> and <body> are mandatory. <owner>, <replace>, and
 * <parameters> are all optional ("" / 0 / empty list when absent).
 * Each <parameter> needs <name> and <data_type>; <mode> defaults to
 * "IN" when absent. Returns 0 on success, -1 on parse error (logged
 * via ctx->ddl_logger). Semantic validity is NOT checked here - see
 * validate_create_procedure_request().
 */
int parse_create_procedure_request(oci_context_t                 *ctx,
                                    const char                    *input_xml,
                                    create_procedure_request_t    *req);

/*
 * validate_create_procedure_request()
 *
 * Validates every field in req, including every parameter definition.
 * Fail-fast: returns 0 when every field passes, -1 on the first
 * failure with a human-readable description written into error_buf.
 * Logged via ctx->ddl_logger.
 */
int validate_create_procedure_request(oci_context_t                      *ctx,
                                       const create_procedure_request_t   *req,
                                       char                                *error_buf,
                                       size_t                               error_buf_size);

/*
 * get_create_procedure_template()
 *
 * tgen stage. Builds the literal CREATE [OR REPLACE] PROCEDURE
 * statement text and returns it wrapped in a
 * <Create_Procedure_Template> XML via a heap-allocated xml_builder_t
 * - caller owns it, release with xml_free(). Does NOT validate req
 * and does NOT touch the database - call
 * validate_create_procedure_request() first. Returns NULL on
 * allocation failure only (logged via ctx->ddl_logger).
 */
xml_builder_t *get_create_procedure_template(oci_context_t                      *ctx,
                                              const create_procedure_request_t  *req);

/*
 * build_create_procedure_ddl_text()
 *
 * Exposed separately from get_create_procedure_template(), same
 * reasoning as the sibling build_*_ddl_text() functions -
 * dispatcher.c's JSON response path needs the raw DDL text without
 * parsing it back out of XML.
 */
void build_create_procedure_ddl_text(const create_procedure_request_t *req,
                                      char *out, size_t out_size);

#endif /* OCI_DDL_CREATE_PROCEDURE_MODULE_H */
