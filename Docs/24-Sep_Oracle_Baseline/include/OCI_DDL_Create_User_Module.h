/*
 * OCI_DDL_Create_User_Module.h
 *
 * Independent DDL Module - Create User (first operation)
 * --------------------------------------------------------
 * First concrete operation of the Independent DDL Module proposal
 * (03-Sep). Follows the same three-stage split already used by
 * OCI_Insert_Template_Module.h / OCI_Insert_Validate_Module.h:
 *
 *   Definition  - parse_create_user_request()
 *                 Parses a <CREATE_USER> operation block (same
 *                 request-envelope shape as INSERT/UPDATE/DELETE, per
 *                 Data_Manager_Request_Definitions.docx) into a
 *                 create_user_request_t.
 *
 *   tgen        - get_create_user_template()
 *                 Builds the literal CREATE USER (+ optional GRANT)
 *                 DDL statement text and returns it wrapped in a
 *                 <Create_User_Template> XML via xml_builder_t, same
 *                 ownership convention as get_insert_template() -
 *                 heap-allocated, caller frees with xml_free().
 *                 This is a PREVIEW/GENERATION stage only - it does
 *                 not execute against the database. Execution is a
 *                 later stage (OCI_DDL_Create_User_Execute_Module),
 *                 not part of this pass.
 *
 *   Validation  - validate_create_user_request()
 *                 Validates every field before the DDL text is
 *                 generated/executed. Fail-fast, same convention as
 *                 validate_insert_template(): returns 0 on success,
 *                 -1 on first failure with a human-readable message
 *                 written to the caller's error_buf.
 *
 * Scope decisions locked in with Terry (03-Sep review)
 * -----------------------------------------------------
 *   - Full field set supported: default/temp tablespace, quota,
 *     profile, initial roles (not just username+password).
 *   - Password (identified_by) is taken as PLAIN TEXT in the request
 *     for this pass - no crypt_helper routing. Revisit if this module
 *     is ever exposed outside trusted internal callers.
 *
 * Not yet wired in this pass (flagged for the next stage)
 * ----------------------------------------------------------
 *   - OP_CREATE_USER is not yet added to operation_type_t in
 *     OCI_Request_Response_Types.h.
 *   - No dispatcher.c routing yet.
 *   - No Level 1 / Level 2 parser hook yet (mirrors the note in
 *     OCI_Request_Response_Types.h: each module's own request struct
 *     is added here first, then wired centrally once the module is
 *     actually plugged in).
 *   - No execute module yet - get_create_user_template() only
 *     generates and returns the DDL text for review.
 */

#ifndef OCI_DDL_CREATE_USER_MODULE_H
#define OCI_DDL_CREATE_USER_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define MAX_CREATE_USER_ROLES   16
#define DDL_IDENTIFIER_LEN      128

/* ------------------------------------------------------------------ */
/*  create_user_request_t                                               */
/*  Parsed from the <CREATE_USER> operation block. Every field except   */
/*  username/identified_by is optional - empty string means "omit that  */
/*  clause from the generated DDL".                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char username           [DDL_IDENTIFIER_LEN];
    char identified_by      [DDL_IDENTIFIER_LEN];  /* plain text - see
                                                      * header note above */
    char default_tablespace [DDL_IDENTIFIER_LEN];  /* "" = omit clause  */
    char temp_tablespace    [DDL_IDENTIFIER_LEN];  /* "" = omit clause  */
    char quota               [32];                 /* "" = omit clause;
                                                      * e.g. "UNLIMITED"
                                                      * or "500M"        */
    char quota_tablespace   [DDL_IDENTIFIER_LEN];  /* required if quota
                                                      * is set; defaults
                                                      * to
                                                      * default_tablespace
                                                      * when left empty  */
    char profile             [DDL_IDENTIFIER_LEN]; /* "" = omit clause  */

    int  role_count;
    char roles[MAX_CREATE_USER_ROLES][DDL_IDENTIFIER_LEN];
} create_user_request_t;

/* ------------------------------------------------------------------ */
/*  Validation result codes - mirrors field_validation_result_t's       */
/*  style in OCI_Insert_Validate_Module.h                               */
/* ------------------------------------------------------------------ */
typedef enum {
    DDL_FIELD_VALID = 0,
    DDL_USERNAME_MISSING,
    DDL_USERNAME_INVALID,        /* not a legal Oracle identifier      */
    DDL_PASSWORD_MISSING,
    DDL_PASSWORD_UNSAFE,         /* contains quote / semicolon / etc.  */
    DDL_TABLESPACE_INVALID,
    DDL_QUOTA_INVALID,           /* not UNLIMITED and not <n>[K|M|G]   */
    DDL_QUOTA_TABLESPACE_MISSING,/* quota given, no tablespace to      *
                                   * apply it to (and no default either)*/
    DDL_PROFILE_INVALID,
    DDL_ROLE_INVALID,
    DDL_TOO_MANY_ROLES
} ddl_validation_result_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * parse_create_user_request()
 *
 * Parses a <CREATE_USER> operation block XML:
 *
 *   <operation type="CREATE_USER">
 *       <username>MIGRATOR_USER</username>
 *       <identified_by>SomePlainTextPwd1</identified_by>
 *       <default_tablespace>USERS</default_tablespace>
 *       <temp_tablespace>TEMP</temp_tablespace>
 *       <quota>UNLIMITED</quota>
 *       <quota_tablespace>USERS</quota_tablespace>
 *       <profile>DEFAULT</profile>
 *       <roles>
 *           <role>CONNECT</role>
 *           <role>RESOURCE</role>
 *       </roles>
 *   </operation>
 *
 * Returns 0 on success, -1 on parse error (logged via ctx->ddl_logger).
 * Only <username> and <identified_by> are mandatory to find in the
 * XML; every other tag is optional and left as an empty string /
 * zero role_count when absent. Semantic validity is NOT checked here
 * - see validate_create_user_request().
 */
int parse_create_user_request(oci_context_t          *ctx,
                               const char             *input_xml,
                               create_user_request_t  *req);

/*
 * validate_create_user_request()
 *
 * Validates every field in req. Fail-fast: returns 0 when every field
 * passes, -1 on the first failure with a human-readable description
 * written into error_buf. All results (pass and fail) are logged via
 * ctx->ddl_logger, same convention as validate_insert_template().
 */
int validate_create_user_request(oci_context_t                *ctx,
                                  const create_user_request_t  *req,
                                  char                          *error_buf,
                                  size_t                         error_buf_size);

/*
 * get_create_user_template()
 *
 * tgen stage. Builds the literal CREATE USER statement (plus a
 * trailing GRANT statement when req->role_count > 0) and returns it
 * wrapped in a <Create_User_Template> XML via a heap-allocated
 * xml_builder_t - caller owns it, release with xml_free().
 *
 * This function does NOT validate req and does NOT touch the
 * database - call validate_create_user_request() first. Returns NULL
 * on allocation failure only (logged via ctx->ddl_logger).
 */
xml_builder_t *get_create_user_template(oci_context_t                *ctx,
                                         const create_user_request_t *req);

/*
 * build_create_user_ddl_text()
 *
 * Exposed separately from get_create_user_template() so callers that
 * need just the DDL text - e.g. dispatcher.c's JSON response path,
 * which builds its own small JSON envelope by hand rather than going
 * through xml_builder_t - don't have to parse it back out of XML.
 * Writes a NUL-terminated string into out (truncated to out_size if
 * needed); no validation performed, same contract as
 * get_create_user_template().
 */
void build_create_user_ddl_text(const create_user_request_t *req,
                                 char *out, size_t out_size);

#endif /* OCI_DDL_CREATE_USER_MODULE_H */
