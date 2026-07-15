
#ifndef XML_HELPER_H
#define XML_HELPER_H

#include <oci.h>
#include <stddef.h>
#include <OCI_Connection.h>
#include <OCI_Execute_Query_Module.h>

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
char* xml_escape(const char *input);

void xml_add_blob_field(
    xml_builder_t *xml,
    const char *name,
    const char *file_name,
    const char *file_path,
    ub8 file_size
);

const char* get_mime_type(const char *filename);
void xml_add_blob_field_1(xml_builder_t *xml, const lob_item_t *item, oci_context_t *ctx);

#endif

