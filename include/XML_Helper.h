
#ifndef XML_HELPER_H
#define XML_HELPER_H

#include <stddef.h>
#include <Connection.h>
#include <Execute_Query_Batch_Module.h>

typedef struct xml_builder_t{
    char *buffer;
    size_t size;
    size_t capacity;
} xml_builder_t;

xml_builder_t* xml_create(size_t initial_size);
void xml_free(xml_builder_t *xml);

void xml_start_document(xml_builder_t *xml);
void xml_finalize(xml_builder_t *xml);

void xml_start_execution(xml_builder_t *xml);
void xml_end_execution(xml_builder_t *xml);

void xml_start_resultset(xml_builder_t *xml);
void xml_end_resultset(xml_builder_t *xml);

void xml_add_row_start(xml_builder_t *xml, unsigned int rownum);
void xml_add_row_end(xml_builder_t *xml);

void xml_add_field(xml_builder_t *xml,
                   const char *name,
                   const char *type,
                   const char *value);

void xml_append(xml_builder_t *xml, const char *fmt, ...);
void xml_append_raw(xml_builder_t *xml, const char *str);
char* xml_escape(const char *input);

/* xml_add_blob_field() removed in Stage 6 (2026-10-09) - no callers;
 * BLOB fields are written by xml_add_blob_field_1() and the response
 * writer. It was the only use of Oracle's ub8 type in this header, and
 * this header no longer includes oci.h directly (Connection.h, above,
 * still brings in what oci_context_t needs). */

const char* get_mime_type(const char *filename);
void xml_add_blob_field_1(xml_builder_t *xml, const lob_item_t *item, oci_context_t *ctx);
void xml_add_blob_field_1(xml_builder_t *xml,
                                   const lob_item_t *item,
                                   oci_context_t *ctx);
   
#endif

