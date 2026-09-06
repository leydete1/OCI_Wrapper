/*
 * OCI_DDL_Grant_Module.h
 *
 * Independent DDL Module - Grant (second operation)
 * -----------------------------------------------------
 * Second concrete operation of the Independent DDL Module proposal
 * (03-Sep), per Terry's clarification: "Grants are like user grants
 * on a table or view." Same three-stage split as
 * OCI_DDL_Create_User_Module.h:
 *
 *   Definition  - parse_grant_request()
 *   tgen        - get_grant_template()
 *   Validation  - validate_grant_request()
 *
 * Same scope decisions as CREATE USER, carried forward:
 *   - tgen/preview only - no execute module yet. Response is the
 *     generated GRANT statement text, not a side effect that already
 *     happened.
 *   - No live database lookups - grantee/object_name/owner are
 *     validated as well-formed Oracle identifiers only. Whether the
 *     object actually exists, or the grantee is a real user/role, is
 *     left to the database to reject at execution time once an
 *     execute module exists - same "not yet wired" boundary
 *     documented in OCI_DDL_Create_User_Module.h.
 *
 * Object type scope
 * -------------------
 *   object_type is "TABLE" or "VIEW" (case-insensitive on input,
 *   normalised to uppercase). Oracle rejects ALTER/INDEX/REFERENCES
 *   on a VIEW - validate_grant_request() checks this so a bad request
 *   fails here rather than only at execution time.
 *
 * Not yet wired in this pass (same flags as CREATE USER)
 * -----------------------------------------------------------
 *   - No execute module - get_grant_template() only generates and
 *     returns the DDL text for review.
 */

#ifndef OCI_DDL_GRANT_MODULE_H
#define OCI_DDL_GRANT_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define MAX_GRANT_PRIVILEGES    16
#define GRANT_IDENTIFIER_LEN    128
#define GRANT_PRIVILEGE_LEN     32
#define GRANT_OBJECT_TYPE_LEN   16

/* ------------------------------------------------------------------ */
/*  grant_request_t                                                     */
/*  Parsed from the <GRANT> operation block.                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char grantee      [GRANT_IDENTIFIER_LEN];   /* user or role         */
    char object_type  [GRANT_OBJECT_TYPE_LEN];  /* "TABLE" or "VIEW"    */
    char object_name  [GRANT_IDENTIFIER_LEN];
    char owner        [GRANT_IDENTIFIER_LEN];   /* "" = auto-resolve,   *
                                                   * same convention as  *
                                                   * template_request_t  */

    int  privilege_count;
    char privileges[MAX_GRANT_PRIVILEGES][GRANT_PRIVILEGE_LEN];
                                                 /* e.g. "SELECT",       *
                                                   * "INSERT", "ALL"     */

    int  with_grant_option;                     /* 0/1 - WITH GRANT     *
                                                   * OPTION clause       */
} grant_request_t;

/* ------------------------------------------------------------------ */
/*  Validation result codes - same style as ddl_validation_result_t     */
/*  in OCI_DDL_Create_User_Module.h                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    GRANT_FIELD_VALID = 0,
    GRANT_GRANTEE_MISSING,
    GRANT_GRANTEE_INVALID,
    GRANT_OBJECT_TYPE_INVALID,     /* not TABLE or VIEW                 */
    GRANT_OBJECT_NAME_INVALID,
    GRANT_OWNER_INVALID,
    GRANT_NO_PRIVILEGES,           /* privilege_count == 0              */
    GRANT_TOO_MANY_PRIVILEGES,
    GRANT_PRIVILEGE_INVALID,       /* not a recognised privilege        */
    GRANT_PRIVILEGE_NOT_VALID_FOR_VIEW /* e.g. ALTER/INDEX/REFERENCES   *
                                         * on object_type=VIEW          */
} grant_validation_result_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * parse_grant_request()
 *
 * Parses a <GRANT> operation block XML:
 *
 *   <operation type="GRANT">
 *       <grantee>MIGRATOR_USER</grantee>
 *       <object_type>TABLE</object_type>
 *       <object_name>EMPLOYEES</object_name>
 *       <owner>HR</owner>
 *       <privileges>
 *           <privilege>SELECT</privilege>
 *           <privilege>INSERT</privilege>
 *       </privileges>
 *       <with_grant_option>0</with_grant_option>
 *   </operation>
 *
 * <grantee>, <object_type>, <object_name>, and at least one
 * <privilege> are mandatory. <owner> and <with_grant_option> are
 * optional ("" / 0 when absent). Returns 0 on success, -1 on parse
 * error (logged via ctx->ddl_logger). Semantic validity is NOT
 * checked here - see validate_grant_request().
 */
int parse_grant_request(oci_context_t   *ctx,
                         const char      *input_xml,
                         grant_request_t *req);

/*
 * validate_grant_request()
 *
 * Validates every field in req. Fail-fast: returns 0 when every field
 * passes, -1 on the first failure with a human-readable description
 * written into error_buf. Logged via ctx->ddl_logger.
 */
int validate_grant_request(oci_context_t          *ctx,
                            const grant_request_t  *req,
                            char                    *error_buf,
                            size_t                   error_buf_size);

/*
 * get_grant_template()
 *
 * tgen stage. Builds the literal GRANT statement text and returns it
 * wrapped in a <Grant_Template> XML via a heap-allocated
 * xml_builder_t - caller owns it, release with xml_free(). Does NOT
 * validate req and does NOT touch the database - call
 * validate_grant_request() first. Returns NULL on allocation failure
 * only (logged via ctx->ddl_logger).
 */
xml_builder_t *get_grant_template(oci_context_t          *ctx,
                                   const grant_request_t  *req);

/*
 * build_grant_ddl_text()
 *
 * Exposed separately from get_grant_template(), same reasoning as
 * build_create_user_ddl_text() in OCI_DDL_Create_User_Module.h -
 * dispatcher.c's JSON response path needs the raw DDL text without
 * parsing it back out of XML.
 */
void build_grant_ddl_text(const grant_request_t *req,
                           char *out, size_t out_size);

#endif /* OCI_DDL_GRANT_MODULE_H */
