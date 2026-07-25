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
#include "cJSON.h"

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

char *response_write_json(oci_context_t *ctx, const resultset_t *rs)
{
    (void)ctx;   /* signature parity with response_write_xml() / future use */

    if (!rs) return NULL;

    cJSON *root      = cJSON_CreateObject();
    cJSON *resultset = cJSON_CreateArray();
    if (!root || !resultset)
    {
        cJSON_Delete(root);
        cJSON_Delete(resultset);
        return NULL;
    }
    cJSON_AddItemToObject(root, "resultset", resultset);

    for (int r = 0; r < rs->record_count; r++)
    {
        const resultset_row_t *row = &rs->records[r];

        cJSON *row_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(row_obj, "row_number", row->record_number);

        cJSON *fields = cJSON_CreateArray();
        cJSON_AddItemToObject(row_obj, "fields", fields);

        for (int f = 0; f < row->field_count; f++)
        {
            const resultset_field_t *fld = &row->fields[f];

            cJSON *field_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(field_obj, "field_name", fld->field_name);

            if (fld->is_blob)
            {
                /* Mirrors xml_add_blob_field_1() exactly: field_value is
                 * always empty for BLOBs, real data lives in the nested
                 * blob object. file_url is included only when set - same
                 * conditional as the XML path.                            */
                cJSON_AddStringToObject(field_obj, "field_type", "BLOB");
                cJSON_AddStringToObject(field_obj, "field_value", "");

                cJSON *blob_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(blob_obj, "file_name",
                    fld->blob_detail.file_name[0] ? fld->blob_detail.file_name : "N/A");
                cJSON_AddStringToObject(blob_obj, "file_path",
                    fld->blob_detail.file_path[0] ? fld->blob_detail.file_path : "N/A");

                if (fld->blob_detail.file_url[0])
                    cJSON_AddStringToObject(blob_obj, "file_url",
                        fld->blob_detail.file_url);

                /* file_size kept as a string, same as every other value -
                 * see the parity note in the header: no field in this
                 * output is a real JSON number yet, by deliberate choice. */
                char size_str[32];
                snprintf(size_str, sizeof(size_str), "%llu",
                         (unsigned long long)fld->blob_detail.file_size);
                cJSON_AddStringToObject(blob_obj, "file_size", size_str);

                cJSON_AddStringToObject(blob_obj, "mime_type",
                    fld->blob_detail.mime_type[0]
                        ? fld->blob_detail.mime_type
                        : "application/octet-stream");

                cJSON_AddItemToObject(field_obj, "blob", blob_obj);
            }
            else
            {
                cJSON_AddStringToObject(field_obj, "field_type", fld->field_type);
                cJSON_AddStringToObject(field_obj, "field_value", fld->value);
            }

            cJSON_AddItemToArray(fields, field_obj);
        }

        cJSON_AddItemToArray(resultset, row_obj);
    }

    char *result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    /* cJSON_PrintUnformatted() already returns a malloc'd buffer that the
     * caller frees directly - no strdup needed, unlike the xml_builder_t
     * path above which owns its own internal buffer.                     */
    return result;
}

int response_writer_cache_store(oci_context_t       *ctx,
                                 cache_t             *cache,
                                 const char          *normalised_key,
                                 const resultset_t   *rs,
                                 const char          *xml_output,
                                 uint64_t             row_count,
                                 cache_entry_opts_t  *opts,
                                 char               **out_json)
{
    if (out_json) *out_json = NULL;
    if (!rs || !out_json) return -1;

    char *json_str = response_write_json(ctx, rs);
    if (!json_str) return -1;

    if (cache && normalised_key && xml_output)
    {
        int rc = resultset_cache_store(cache, normalised_key,
                                        xml_output, json_str,
                                        row_count, opts);
        if (rc != 0 && ctx)
            logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                         "response_writer_cache_store: cache store "
                         "failed key='%.80s'", normalised_key);
    }

    *out_json = json_str;
    return 0;
}

/* rows_affected's wire tag/key name differs by operation - see this
 * pair's own doc comment in OCI_Response_Writer.h for why. Returns
 * NULL for OP_SELECT (not a supported case for this writer) or any
 * other unrecognised type, which the two functions below both treat
 * as "refuse to render" rather than guessing at a name.               */
static const char *rows_affected_tag_name(operation_type_t op_type)
{
    switch (op_type)
    {
        case OP_INSERT: return "rows_inserted";
        case OP_UPDATE: return "rows_updated";
        case OP_DELETE: return "rows_deleted";
        default:        return NULL;
    }
}

char *response_write_dml_xml(oci_context_t *ctx, operation_type_t op_type,
                              const dml_response_t *resp)
{
    (void)ctx;   /* signature parity with response_write_xml() / future use */

    if (!resp) return NULL;

    const char *rows_tag = rows_affected_tag_name(op_type);
    if (!rows_tag) return NULL;

    xml_builder_t *xml = xml_create(1024);
    if (!xml) return NULL;

    const char *op_name =
        op_type == OP_INSERT ? "INSERT" :
        op_type == OP_UPDATE ? "UPDATE" :
        op_type == OP_DELETE ? "DELETE" : "?";

    xml_append(xml, "<operation>%s</operation>\n", op_name);
    xml_append(xml, "<table_name>%s</table_name>\n", resp->table_name);
    xml_append(xml, "<owner>%s</owner>\n", resp->owner);
    xml_append(xml, "<%s>%d</%s>\n", rows_tag, resp->rows_affected, rows_tag);
    xml_append(xml, "<lobs_written>%d</lobs_written>\n", resp->lobs_written);
    xml_append(xml, "<execution_time>%.6f</execution_time>\n",
               resp->execution_time_seconds);

    char *result = xml->buffer ? strdup(xml->buffer) : NULL;
    xml_free(xml);

    return result;
}

char *response_write_dml_json(oci_context_t *ctx, operation_type_t op_type,
                               const dml_response_t *resp)
{
    (void)ctx;   /* signature parity with response_write_json() / future use */

    if (!resp) return NULL;

    const char *rows_key = rows_affected_tag_name(op_type);
    if (!rows_key) return NULL;

    const char *op_name =
        op_type == OP_INSERT ? "INSERT" :
        op_type == OP_UPDATE ? "UPDATE" :
        op_type == OP_DELETE ? "DELETE" : "?";

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "operation",   op_name);
    cJSON_AddStringToObject(root, "table_name",  resp->table_name);
    cJSON_AddStringToObject(root, "owner",       resp->owner);
    cJSON_AddNumberToObject(root, rows_key,      resp->rows_affected);
    cJSON_AddNumberToObject(root, "lobs_written", resp->lobs_written);
    cJSON_AddNumberToObject(root, "execution_time", resp->execution_time_seconds);

    char *result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return result;
}
