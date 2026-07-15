/*
 * OCI_Insert_Template_Module.h
 *
 * Stage 1 - Insert Template Builder
 * ----------------------------------
 * Accepts a parsed <Template Request> XML (table name + operation),
 * queries ALL_TAB_COLUMNS for that table, and returns a populated
 * <Insert Template> XML via the xml_builder_t typedef.
 *
 * The returned xml_builder_t* is heap-allocated and owned by the
 * caller; release with xml_free().
 *
 * Stage 2 (OCI_Insert_Validate_Module) consumes the same XML structure
 * once the caller has filled in the <insert_value> fields.
 */

#ifndef OCI_INSERT_TEMPLATE_MODULE_H
#define OCI_INSERT_TEMPLATE_MODULE_H

#include "OCI_Connection.h"
#include "OCI_Table_Metadata_Module.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  col_metadata_t is defined in OCI_Table_Metadata_Module.h          */
/*  Template request - parsed from input XML                           */
/* ------------------------------------------------------------------ */
typedef struct {
    char  operation[32];      /* e.g. "INSERT"                        */
    char  table_name[128];    /* target table, upper-cased            */
    char  owner[128];         /* schema owner, upper-cased            */
                              /* if empty, resolved automatically     */
                              /* via get_request_metadata()           */
} template_request_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * parse_template_request()
 *
 * Parse a <Template Request> XML string into template_request_t.
 * The XML is expected to follow the layout:
 *
 *   <Template Request>
 *       <operation>INSERT</operation>
 *       <TABLE>OCI_TEST_FIELDS</TABLE>
 *   </Template Request>
 *
 * Returns  0 on success, -1 on parse error (logged via ctx->logger).
 */
int parse_template_request(oci_context_t        *ctx,
                            const char           *input_xml,
                            template_request_t   *req);

/*
 * get_insert_template()
 *
 * Main Stage-1 entry point.
 *
 * Reads ALL_TAB_COLUMNS for req->table_name, builds and returns an
 * <Insert Template> XML as a heap-allocated xml_builder_t*.
 *
 * The caller writes the result to cfg->xml->OUTPUT_XML (or similar)
 * and later passes the XML string to validate_insert_template().
 *
 * Returns a valid xml_builder_t* on success, NULL on any error.
 * All errors are logged via ctx->logger.
 */
xml_builder_t *get_insert_template(oci_context_t            *ctx,
                                    const template_request_t *req);

#endif /* OCI_INSERT_TEMPLATE_MODULE_H */
