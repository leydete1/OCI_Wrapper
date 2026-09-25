/*
 * OCI_DDL_Modules.h
 *
 * Merge of the six Independent DDL Module headers - see
 * OCI_DDL_Modules.c for the full merge rationale. Every type,
 * macro and function declaration below is verbatim from its
 * original header, under its own module's section, in original
 * order - nothing renamed, nothing changed. The original six
 * headers each carried their own #include "OCI_Connection.h" /
 * "XML_Helper.h" / "logger.h" - hoisted to the top here once,
 * same reasoning as the .c merge's shared-includes block.
 */

#ifndef OCI_DDL_MODULES_H
#define OCI_DDL_MODULES_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/* ==================================================================== */
/*  CREATE TABLE                                                              */
/*  (originally OCI_DDL_Create_Table_Module.h)                                            */
/* ==================================================================== */


/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define DDL_MAX_TABLE_COLUMNS        64
#define MAX_PRIMARY_KEY_COLUMNS  16
#define TABLE_IDENTIFIER_LEN     128
#define COLUMN_DATA_TYPE_LEN     16
#define COLUMN_DEFAULT_LEN       128

/* ------------------------------------------------------------------ */
/*  column_def_t                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char name          [TABLE_IDENTIFIER_LEN];
    char data_type     [COLUMN_DATA_TYPE_LEN];   /* VARCHAR2, CHAR,      *
                                                    * NUMBER, DATE,       *
                                                    * TIMESTAMP, CLOB,    *
                                                    * BLOB                */
    int  length;                                 /* VARCHAR2/CHAR - 0   *
                                                    * = not given         */
    int  precision;                               /* NUMBER - 0 = not   *
                                                    * given                */
    int  scale;                                   /* NUMBER - only      *
                                                    * meaningful when     *
                                                    * precision > 0       */
    int  nullable;                                /* 1 = NULL allowed   *
                                                    * (default), 0 = NOT  *
                                                    * NULL                */
    char default_value[COLUMN_DEFAULT_LEN];       /* "" = no DEFAULT    *
                                                    * clause               */
} column_def_t;

/* ------------------------------------------------------------------ */
/*  create_table_request_t                                              */
/*  Parsed from the <CREATE_TABLE> operation block.                     */
/* ------------------------------------------------------------------ */
typedef struct {
    char table_name [TABLE_IDENTIFIER_LEN];
    char owner       [TABLE_IDENTIFIER_LEN];      /* "" = auto-resolve, *
                                                    * same convention as  *
                                                    * grant_request_t     */

    int          column_count;
    column_def_t columns[DDL_MAX_TABLE_COLUMNS];

    int  primary_key_count;
    char primary_key_columns[MAX_PRIMARY_KEY_COLUMNS][TABLE_IDENTIFIER_LEN];
} create_table_request_t;

/* ------------------------------------------------------------------ */
/*  Validation result codes - same style as the other two DDL modules   */
/* ------------------------------------------------------------------ */
typedef enum {
    TABLE_FIELD_VALID = 0,
    TABLE_NAME_INVALID,
    TABLE_OWNER_INVALID,
    TABLE_NO_COLUMNS,
    TABLE_TOO_MANY_COLUMNS,
    TABLE_COLUMN_NAME_INVALID,
    TABLE_COLUMN_NAME_DUPLICATE,
    TABLE_COLUMN_TYPE_INVALID,           /* not a recognised data type   */
    TABLE_COLUMN_LENGTH_MISSING,         /* VARCHAR2/CHAR needs length   */
    TABLE_COLUMN_LENGTH_INVALID,         /* <= 0 or unreasonably large   */
    TABLE_COLUMN_PRECISION_INVALID,      /* NUMBER precision/scale       *
                                            * out of range                */
    TABLE_PRIMARY_KEY_TOO_MANY,
    TABLE_PRIMARY_KEY_UNKNOWN_COLUMN     /* PK references a column not   *
                                            * in the column list          */
} table_validation_result_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * parse_create_table_request()
 *
 * Parses a <CREATE_TABLE> operation block XML:
 *
 *   <operation type="CREATE_TABLE">
 *       <table_name>MIGRATION_LOG</table_name>
 *       <owner>HR</owner>
 *       <columns>
 *           <column>
 *               <name>LOG_ID</name>
 *               <data_type>NUMBER</data_type>
 *               <precision>10</precision>
 *               <nullable>0</nullable>
 *           </column>
 *           <column>
 *               <name>LOG_MESSAGE</name>
 *               <data_type>VARCHAR2</data_type>
 *               <length>4000</length>
 *               <nullable>1</nullable>
 *           </column>
 *       </columns>
 *       <primary_key>
 *           <column>LOG_ID</column>
 *       </primary_key>
 *   </operation>
 *
 * <table_name> and at least one <column> (with <name> and
 * <data_type>) are mandatory. <owner>, <primary_key>, <length>,
 * <precision>, <scale>, <nullable> (default 1), and <default_value>
 * are all optional. Returns 0 on success, -1 on parse error (logged
 * via ctx->ddl_logger). Semantic validity is NOT checked here - see
 * validate_create_table_request().
 */
int parse_create_table_request(oci_context_t            *ctx,
                                const char               *input_xml,
                                create_table_request_t   *req);

/*
 * validate_create_table_request()
 *
 * Validates every field in req, including every column definition and
 * the primary key list. Fail-fast: returns 0 when every field passes,
 * -1 on the first failure with a human-readable description written
 * into error_buf. Logged via ctx->ddl_logger.
 */
int validate_create_table_request(oci_context_t                  *ctx,
                                   const create_table_request_t   *req,
                                   char                            *error_buf,
                                   size_t                           error_buf_size);

/*
 * get_create_table_template()
 *
 * tgen stage. Builds the literal CREATE TABLE statement text
 * (columns + optional PRIMARY KEY constraint) and returns it wrapped
 * in a <Create_Table_Template> XML via a heap-allocated xml_builder_t
 * - caller owns it, release with xml_free(). Does NOT validate req
 * and does NOT touch the database - call
 * validate_create_table_request() first. Returns NULL on allocation
 * failure only (logged via ctx->ddl_logger).
 */
xml_builder_t *get_create_table_template(oci_context_t                  *ctx,
                                          const create_table_request_t  *req);

/*
 * build_create_table_ddl_text()
 *
 * Exposed separately from get_create_table_template(), same reasoning
 * as build_create_user_ddl_text() / build_grant_ddl_text() -
 * dispatcher.c's JSON response path needs the raw DDL text without
 * parsing it back out of XML.
 */
void build_create_table_ddl_text(const create_table_request_t *req,
                                  char *out, size_t out_size);


/* ==================================================================== */
/*  CREATE VIEW                                                              */
/*  (originally OCI_DDL_Create_View_Module.h)                                            */
/* ==================================================================== */


/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define MAX_VIEW_COLUMNS        64
#define VIEW_IDENTIFIER_LEN     128
#define VIEW_QUERY_LEN          4000   /* matches VARCHAR2(4000) used   *
                                         * for LOG_MESSAGE-style text     *
                                         * fields elsewhere in this       *
                                         * codebase's own DDL fixtures    */

/* ------------------------------------------------------------------ */
/*  create_view_request_t                                               */
/*  Parsed from the <CREATE_VIEW> operation block.                      */
/* ------------------------------------------------------------------ */
typedef struct {
    char view_name  [VIEW_IDENTIFIER_LEN];
    char owner       [VIEW_IDENTIFIER_LEN];   /* "" = auto-resolve,     *
                                                * same convention as the *
                                                * other DDL modules       */
    int  replace;                             /* 0/1 - OR REPLACE       */
    int  force;                                /* 0/1 - FORCE (create   *
                                                * even if base tables    *
                                                * don't exist/aren't     *
                                                * visible yet)            */

    int  column_count;                         /* 0 = no explicit       *
                                                * column alias list -    *
                                                * columns inferred from   *
                                                * the query               */
    char columns[MAX_VIEW_COLUMNS][VIEW_IDENTIFIER_LEN];

    char query[VIEW_QUERY_LEN];                /* the SELECT statement  *
                                                * text - see header      *
                                                * doc comment for the    *
                                                * two safety checks      *
                                                * applied to it           */
} create_view_request_t;

/* ------------------------------------------------------------------ */
/*  Validation result codes - same style as the other DDL modules       */
/* ------------------------------------------------------------------ */
typedef enum {
    VIEW_FIELD_VALID = 0,
    VIEW_NAME_INVALID,
    VIEW_OWNER_INVALID,
    VIEW_COLUMN_ALIAS_INVALID,
    VIEW_TOO_MANY_COLUMNS,
    VIEW_QUERY_MISSING,
    VIEW_QUERY_NOT_SELECT,       /* doesn't start with SELECT           */
    VIEW_QUERY_CONTAINS_SEMICOLON /* possible statement-stacking attempt */
} view_validation_result_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * parse_create_view_request()
 *
 * Parses a <CREATE_VIEW> operation block XML:
 *
 *   <operation type="CREATE_VIEW">
 *       <view_name>ACTIVE_EMPLOYEES</view_name>
 *       <owner>HR</owner>
 *       <replace>1</replace>
 *       <force>0</force>
 *       <columns>
 *           <column>EMP_ID</column>
 *           <column>EMP_NAME</column>
 *       </columns>
 *       <query>SELECT EMPLOYEE_ID, FULL_NAME FROM HR.EMPLOYEES WHERE STATUS = 'ACTIVE'</query>
 *   </operation>
 *
 * <view_name> and <query> are mandatory. <owner>, <replace>, <force>,
 * and <columns> are all optional ("" / 0 / empty list when absent).
 * Returns 0 on success, -1 on parse error (logged via
 * ctx->ddl_logger). Semantic validity is NOT checked here - see
 * validate_create_view_request().
 */
int parse_create_view_request(oci_context_t            *ctx,
                               const char               *input_xml,
                               create_view_request_t    *req);

/*
 * validate_create_view_request()
 *
 * Validates every field in req, including the two query-text safety
 * checks described in the header doc comment. Fail-fast: returns 0
 * when every field passes, -1 on the first failure with a
 * human-readable description written into error_buf. Logged via
 * ctx->ddl_logger.
 */
int validate_create_view_request(oci_context_t                 *ctx,
                                  const create_view_request_t   *req,
                                  char                           *error_buf,
                                  size_t                          error_buf_size);

/*
 * get_create_view_template()
 *
 * tgen stage. Builds the literal CREATE [OR REPLACE] [FORCE] VIEW
 * statement text and returns it wrapped in a <Create_View_Template>
 * XML via a heap-allocated xml_builder_t - caller owns it, release
 * with xml_free(). Does NOT validate req and does NOT touch the
 * database - call validate_create_view_request() first. Returns NULL
 * on allocation failure only (logged via ctx->ddl_logger).
 */
xml_builder_t *get_create_view_template(oci_context_t                 *ctx,
                                         const create_view_request_t  *req);

/*
 * build_create_view_ddl_text()
 *
 * Exposed separately from get_create_view_template(), same reasoning
 * as the sibling build_*_ddl_text() functions - dispatcher.c's JSON
 * response path needs the raw DDL text without parsing it back out
 * of XML.
 */
void build_create_view_ddl_text(const create_view_request_t *req,
                                 char *out, size_t out_size);


/* ==================================================================== */
/*  CREATE USER                                                              */
/*  (originally OCI_DDL_Create_User_Module.h)                                            */
/* ==================================================================== */


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


/* ==================================================================== */
/*  CREATE PROCEDURE                                                              */
/*  (originally OCI_DDL_Create_Procedure_Module.h)                                            */
/* ==================================================================== */


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


/* ==================================================================== */
/*  DROP TABLE                                                              */
/*  (originally OCI_DDL_Drop_Table_Module.h)                                            */
/* ==================================================================== */


/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define DROP_TABLE_IDENTIFIER_LEN   128

/* ------------------------------------------------------------------ */
/*  drop_table_request_t                                                */
/*  Parsed from the <DROP_TABLE> operation block.                       */
/* ------------------------------------------------------------------ */
typedef struct {
    char table_name          [DROP_TABLE_IDENTIFIER_LEN];
    char owner                [DROP_TABLE_IDENTIFIER_LEN]; /* "" = auto- *
                                                              * resolve,  *
                                                              * same      *
                                                              * convention*
                                                              * as the    *
                                                              * other DDL *
                                                              * modules   */
    int  cascade_constraints;                              /* 0/1 -     *
                                                              * CASCADE   *
                                                              * CONSTRAINTS*/
    int  purge;                                             /* 0/1 -     *
                                                              * PURGE     *
                                                              * (skip     *
                                                              * recycle   *
                                                              * bin)      */
    int  confirm;   /* 0/1 - MUST be 1 for the execute stage to actually
                      * run this DROP (Terry, 07-Sep). Checked by
                      * dispatch_drop_table_new() (dispatcher.c), NOT
                      * here - this struct only carries the value. */
} drop_table_request_t;

/* ------------------------------------------------------------------ */
/*  Validation result codes - same style as the other DDL modules       */
/* ------------------------------------------------------------------ */
typedef enum {
    DROP_TABLE_FIELD_VALID = 0,
    DROP_TABLE_NAME_INVALID,
    DROP_TABLE_OWNER_INVALID
} drop_table_validation_result_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * parse_drop_table_request()
 *
 * Parses a <DROP_TABLE> operation block XML:
 *
 *   <operation type="DROP_TABLE">
 *       <table_name>MIGRATION_LOG</table_name>
 *       <owner>HR</owner>
 *       <cascade_constraints>0</cascade_constraints>
 *       <purge>0</purge>
 *       <confirm>1</confirm>
 *   </operation>
 *
 * <table_name> is mandatory. <owner>, <cascade_constraints>,
 * <purge>, and <confirm> are all optional ("" / 0 when absent) as far
 * as parsing/validation are concerned - <confirm> only becomes
 * mandatory (must be 1) at the execute stage, checked by
 * dispatch_drop_table_new() (dispatcher.c), not here. Returns 0 on
 * success, -1 on parse error (logged via ctx->ddl_logger). Semantic
 * validity is NOT checked here - see validate_drop_table_request().
 */
int parse_drop_table_request(oci_context_t          *ctx,
                              const char             *input_xml,
                              drop_table_request_t   *req);

/*
 * validate_drop_table_request()
 *
 * Validates table_name/owner as well-formed identifiers. Fail-fast:
 * returns 0 on success, -1 on first failure with a human-readable
 * description written into error_buf. Logged via ctx->ddl_logger.
 */
int validate_drop_table_request(oci_context_t                *ctx,
                                 const drop_table_request_t   *req,
                                 char                          *error_buf,
                                 size_t                         error_buf_size);

/*
 * get_drop_table_template()
 *
 * tgen stage. Builds the literal DROP TABLE statement text and
 * returns it wrapped in a <Drop_Table_Template> XML via a
 * heap-allocated xml_builder_t - caller owns it, release with
 * xml_free(). Does NOT validate req and does NOT touch the database -
 * call validate_drop_table_request() first. Returns NULL on
 * allocation failure only (logged via ctx->ddl_logger).
 */
xml_builder_t *get_drop_table_template(oci_context_t                *ctx,
                                        const drop_table_request_t  *req);

/*
 * build_drop_table_ddl_text()
 *
 * Exposed separately from get_drop_table_template(), same reasoning
 * as the sibling build_*_ddl_text() functions - dispatcher.c's JSON
 * response path needs the raw DDL text without parsing it back out
 * of XML.
 */
void build_drop_table_ddl_text(const drop_table_request_t *req,
                                char *out, size_t out_size);


/* ==================================================================== */
/*  GRANT                                                              */
/*  (originally OCI_DDL_Grant_Module.h)                                            */
/* ==================================================================== */


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


#endif /* OCI_DDL_MODULES_H */
