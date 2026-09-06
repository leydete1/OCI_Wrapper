/*
 * OCI_DDL_Drop_Table_Module.h
 *
 * Independent DDL Module - Drop Table (fourth operation)
 * --------------------------------------------------------
 * Fourth concrete operation of the Independent DDL Module proposal
 * (03-Sep), per Terry's clarification: "Drop would be in the returned
 * dml for dropping a pre-existing table." Same three-stage split as
 * the other three DDL modules:
 *
 *   Definition  - parse_drop_table_request()
 *   tgen        - get_drop_table_template()
 *   Validation  - validate_drop_table_request()
 *
 * Scope decided with Terry (06-Sep): generate the DROP TABLE text
 * only (optionally CASCADE CONSTRAINTS / PURGE) - no companion
 * "describe existing DDL before dropping" step in this pass. That
 * closer-to-the-original-proposal Describe capability is a separate,
 * larger piece if wanted later.
 *
 * Same scope decisions carried forward from CREATE USER / GRANT /
 * CREATE TABLE:
 *   - tgen/preview only - no execute module yet. Response is the
 *     generated DROP TABLE text, not a side effect that already
 *     happened. This matters more here than anywhere else in the
 *     proposal so far - DROP is destructive and irreversible once
 *     executed, so the preview-only boundary is especially
 *     deliberate.
 *   - No live database lookups - table_name/owner are validated as
 *     well-formed Oracle identifiers only. Whether the table actually
 *     exists is left to the database to reject at execution time once
 *     an execute module exists.
 *
 * Intended test target: HR.MIGRATION_LOG, built by
 * OCI_DDL_Create_Table_Module.h's own Round 1 test.
 */

#ifndef OCI_DDL_DROP_TABLE_MODULE_H
#define OCI_DDL_DROP_TABLE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

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
 *   </operation>
 *
 * <table_name> is mandatory. <owner>, <cascade_constraints>, and
 * <purge> are all optional ("" / 0 when absent). Returns 0 on
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

#endif /* OCI_DDL_DROP_TABLE_MODULE_H */
