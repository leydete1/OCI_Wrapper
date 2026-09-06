/*
 * OCI_DDL_Create_Table_Module.h
 *
 * Independent DDL Module - Create Table (third operation)
 * -------------------------------------------------------
 * Third concrete operation of the Independent DDL Module proposal
 * (03-Sep). Built now specifically so DROP has something real to
 * target in testing (Terry, 05-Sep). Same three-stage split as
 * OCI_DDL_Create_User_Module.h / OCI_DDL_Grant_Module.h:
 *
 *   Definition  - parse_create_table_request()
 *   tgen        - get_create_table_template()
 *   Validation  - validate_create_table_request()
 *
 * Same scope decisions carried forward from CREATE USER / GRANT:
 *   - tgen/preview only - no execute module yet. Response is the
 *     generated CREATE TABLE text, not a side effect that already
 *     happened.
 *   - No live database lookups - table/column/owner names are
 *     validated as well-formed Oracle identifiers only, and column
 *     data types are checked against a fixed recognised set. Whether
 *     the table already exists, or a NUMBER precision is sane for the
 *     target tablespace, is left to the database at execution time.
 *
 * Column scope
 * --------------
 *   Each column: name, data_type, optional length (VARCHAR2/CHAR),
 *   optional precision/scale (NUMBER), nullable flag, optional
 *   default_value. Recognised data types: VARCHAR2, CHAR, NUMBER,
 *   DATE, TIMESTAMP, CLOB, BLOB - the common set used elsewhere in
 *   this codebase's own test fixtures (see Unit_Test_Insert_Round_*
 *   for the column shapes already in play). Optional PRIMARY KEY
 *   constraint over one or more of the declared columns.
 *
 * Not yet wired in this pass (same flags as CREATE USER / GRANT)
 * -----------------------------------------------------------------
 *   - No execute module - get_create_table_template() only generates
 *     and returns the DDL text for review.
 */

#ifndef OCI_DDL_CREATE_TABLE_MODULE_H
#define OCI_DDL_CREATE_TABLE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

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

#endif /* OCI_DDL_CREATE_TABLE_MODULE_H */
