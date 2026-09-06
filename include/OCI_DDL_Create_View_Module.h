/*
 * OCI_DDL_Create_View_Module.h
 *
 * Independent DDL Module - Create View (fifth operation)
 * --------------------------------------------------------
 * Fifth concrete operation of the Independent DDL Module proposal
 * (03-Sep). Same three-stage split as the other four DDL modules:
 *
 *   Definition  - parse_create_view_request()
 *   tgen        - get_create_view_template()
 *   Validation  - validate_create_view_request()
 *
 * Same scope decisions carried forward:
 *   - tgen/preview only - no execute module yet.
 *   - No live database lookups - view_name/owner/column aliases are
 *     validated as well-formed Oracle identifiers only. The
 *     underlying query text is NOT parsed as SQL (this codebase
 *     doesn't parse SELECT text anywhere else either - see
 *     OCI_Execute_Query_Batch_Module.c, which passes SQL straight to
 *     OCI). validate_create_view_request() only does the light,
 *     injection-conscious checks described below - it is not a SQL
 *     parser or an optimiser.
 *
 * Query text safety scope
 * --------------------------
 *   The <query> field becomes the literal AS <query> clause of the
 *   generated CREATE VIEW statement, so two shallow checks apply
 *   before it's ever used:
 *     1. Must start with SELECT (case-insensitive) - a sanity check
 *        that this is actually a query, not some other statement.
 *     2. Must not contain a semicolon - a CREATE VIEW ... AS clause
 *        takes exactly one statement; a semicolon would let a second,
 *        unrelated statement be smuggled in behind it.
 *   Everything else about the query (column references, joins,
 *   correctness) is left to the database at execution time, same as
 *   every other SQL text already flowing through this codebase.
 */

#ifndef OCI_DDL_CREATE_VIEW_MODULE_H
#define OCI_DDL_CREATE_VIEW_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

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

#endif /* OCI_DDL_CREATE_VIEW_MODULE_H */
