/*
 * OCI_Response_Writer.c
 *
 * See OCI_Response_Writer.h for the full design description.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "OCI_Response_Writer.h"
#include "XML_Helper.h"
#include "OCI_Execute_Query_Module.h"   /* xml_add_blob_field_1() lives here */

char *response_write_xml(oci_context_t *ctx, const resultset_t *rs)
{
    if (!rs) return NULL;

    xml_builder_t *xml = xml_create(16384);
    if (!xml) return NULL;

    xml_start_resultset(xml);

    for (int r = 0; r < rs->record_count; r++)
    {
        const resultset_row_t *row = &rs->records[r];

        xml_add_row_start(xml, (unsigned int)row->record_number);

        for (int f = 0; f < row->field_count; f++)
        {
            const resultset_field_t *fld = &row->fields[f];

            if (fld->is_blob)
            {
                /* Zero-initialised, and only the 6 fields
                 * xml_add_blob_field_1() actually reads are populated -
                 * verified directly against its current body, not
                 * assumed. Every other lob_item_t field (is_null,
                 * lob_loc, etc.) stays zero and is never touched by
                 * that function.                                        */
                lob_item_t item;
                memset(&item, 0, sizeof(item));

                item.column_name             = (char *)fld->field_name;
                item.file_name                = (char *)fld->blob_detail.file_name;
                item.output_file_destination  = (char *)fld->blob_detail.file_path;
                item.output_file_url          = fld->blob_detail.file_url[0]
                                                 ? (char *)fld->blob_detail.file_url
                                                 : NULL;
                item.blob_size                = (ub4)fld->blob_detail.file_size;
                item.mime_type                = (char *)fld->blob_detail.mime_type;

                xml_add_blob_field_1(xml, &item, ctx);
            }
            else
            {
                xml_add_field(xml, fld->field_name, fld->field_type, fld->value);
            }
        }

        xml_add_row_end(xml);
    }

    xml_end_resultset(xml);

    char *result = xml->buffer ? strdup(xml->buffer) : NULL;
    xml_free(xml);

    return result;
}
