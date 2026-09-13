
#define _GNU_SOURCE
#include "XML_Helper.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "OCI_Connection.h"   /* oci_context_t, lob_item_t - needed by xml_add_blob_field_1() */
#include "logger.h"

static void ensure_capacity(xml_builder_t *xml, size_t extra)
{
    while (xml->size + extra + 1 > xml->capacity)
    {
        xml->capacity *= 2;
        xml->buffer = realloc(xml->buffer, xml->capacity);
    }
}

xml_builder_t* xml_create(size_t initial_size)
{
    xml_builder_t *xml = malloc(sizeof(xml_builder_t));
    xml->capacity = initial_size;
    xml->size = 0;
    xml->buffer = malloc(initial_size);
    xml->buffer[0] = '\0';
    return xml;
}

void xml_free(xml_builder_t *xml)
{
    if (!xml) return;
    free(xml->buffer);
    free(xml);
}

void xml_append(xml_builder_t *xml, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    char temp[8192];
    int written = vsnprintf(temp, sizeof(temp), fmt, args);
    va_end(args);

    if (written <= 0) return;

    ensure_capacity(xml, written);
    memcpy(xml->buffer + xml->size, temp, written);
    xml->size += written;
    xml->buffer[xml->size] = '\0';
}

/*
 * xml_append_raw()
 *
 * Appends an already-built string verbatim, with no printf-style
 * formatting. xml_append() formats into a fixed 8192-byte stack
 * buffer, then trusts vsnprintf's *would-have-been* return length
 * when copying out of it - safe for the small per-field/per-tag calls
 * it was designed for, but a stack over-read if a caller ever passes
 * a single string longer than that buffer via "%s" (as splicing in a
 * whole multi-row response_write_xml() fragment does for any
 * resultset over ~8KB). Use this instead whenever the string being
 * appended is already fully formatted and could be arbitrarily long.
 */
void xml_append_raw(xml_builder_t *xml, const char *str)
{
    if (!str) return;

    size_t len = strlen(str);
    if (len == 0) return;

    ensure_capacity(xml, len);
    memcpy(xml->buffer + xml->size, str, len);
    xml->size += len;
    xml->buffer[xml->size] = '\0';
}

char* xml_escape(const char *input)
{
    if (!input) return strdup("");

    xml_builder_t *tmp = xml_create(256);

    for (const char *p = input; *p; p++)
    {
        switch (*p)
        {
            case '&':  xml_append(tmp, "&amp;"); break;
            case '<':  xml_append(tmp, "&lt;"); break;
            case '>':  xml_append(tmp, "&gt;"); break;
            case '"':  xml_append(tmp, "&quot;"); break;
            case '\'': xml_append(tmp, "&apos;"); break;
            default:   xml_append(tmp, "%c", *p);
        }
    }

    char *escaped = strdup(tmp->buffer);
    xml_free(tmp);
    return escaped;
}

void xml_start_document(xml_builder_t *xml)
{
    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<output_xml>\n");
}

void xml_finalize(xml_builder_t *xml)
{
    xml_append(xml, "</output_xml>\n");
}

void xml_start_execution(xml_builder_t *xml)
{
    xml_append(xml, "<execution_envelope>\n");
}

void xml_end_execution(xml_builder_t *xml)
{
    xml_append(xml, "</execution_envelope>\n");
}

void xml_start_resultset(xml_builder_t *xml)
{
    xml_append(xml, "<resultset>\n");
}

void xml_end_resultset(xml_builder_t *xml)
{
    xml_append(xml, "</resultset>\n");
}

void xml_add_row_start(xml_builder_t *xml, unsigned int rownum)
{
    xml_append(xml, "<row number=\"%u\">\n", rownum);
}

void xml_add_row_end(xml_builder_t *xml)
{
    xml_append(xml, "</row>\n");
}

void xml_add_field(xml_builder_t *xml,
                   const char *name,
                   const char *type,
                   const char *value)
{
    char *e_name  = xml_escape(name);
    char *e_type  = xml_escape(type);
    char *e_value = xml_escape(value);

    xml_append(xml,
        "<field>"
        "<field_name>%s</field_name>"
        "<field_type>%s</field_type>"
        "<field_value>%s</field_value>"
        "</field>\n",
        e_name, e_type, e_value);

    free(e_name);
    free(e_type);
    free(e_value);
}


void xml_add_blob_field_old(
    xml_builder_t *xml,
    const char *name,
    const char *file_name,
    const char *file_path,
    ub8 file_size
)
{
    char *e_name = xml_escape(name);
    char *e_file = xml_escape(file_name);
    char *e_path = xml_escape(file_path);

    xml_append(xml,
        "<field>"
            "<field_name>%s</field_name>"
            "<field_type>BLOB</field_type>"
            "<field_value/>"
            "<blob>"
                "<file_name>%s</file_name>"
                "<file_path>%s</file_path>"
                "<file_size>%llu</file_size>"
            "</blob>"
        "</field>\n",
        e_name,
        e_file,
        e_path,
        (unsigned long long)file_size
    );

    free(e_name);
    free(e_file);
    free(e_path);
}


void xml_add_blob_field(
    xml_builder_t *xml,
    const char *name,
    const char *file_name,
    const char *file_path,
    ub8 file_size
)
{
    char *e_name = xml_escape(name);

    xml_append(xml,
        "<field>"
        "<field_name>%s</field_name>"
        "<field_type>BLOB</field_type>"
        "<field_value/>",
        e_name);

    if (file_name)
    {
        char *e_file = xml_escape(file_name);
        char *e_path = xml_escape(file_path);

        xml_append(xml,
            "<blob>"
            "<file_name>%s</file_name>"
            "<file_path>%s</file_path>"
            "<file_size>%llu</file_size>"
            "</blob>",
            e_file, e_path, (unsigned long long)file_size);

        free(e_file);
        free(e_path);
    }

    xml_append(xml, "</field>\n");

    free(e_name);
}



/*
 * xml_add_blob_field_1()
 *
 * Relocated from OCI_Execute_Query_Module.c when that module was removed
 * (execute_query() was dead, but this helper is still called by
 * execute_query_batch(), OCI_Execute_Procedure_Module.c, and
 * OCI_Response_Writer.c's response_write_xml()). Behaviour unchanged,
 * including its logger call - takes ctx for that reason alone, same as
 * before.
 */
void xml_add_blob_field_1(xml_builder_t *xml, const lob_item_t *item, oci_context_t *ctx)
{
    /* Use the column name or fallback to "UNKNOWN" */
    char *e_name = xml_escape(item->column_name ? item->column_name : "UNKNOWN");
    char *e_file = xml_escape(item->file_name ? item->file_name : "N/A");
    char *e_path = xml_escape(item->output_file_destination ? item->output_file_destination : "N/A");

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "In xml_add_blob_field_1");
    /* Only escape and add URL if it's set */
    char *e_url = item->output_file_url ? xml_escape(item->output_file_url) : NULL;

    xml_append(xml,
        "<field>"
        "<field_name>%s</field_name>"
        "<field_type>BLOB</field_type>"
        "<field_value/>"
        "<blob>"
        "<file_name>%s</file_name>"
        "<file_path>%s</file_path>",
        e_name, e_file, e_path
    );

    if (e_url) {
        xml_append(xml, "<file_url>%s</file_url>", e_url);
        free(e_url);
    }

    xml_append(xml,
        "<file_size>%llu</file_size>"
        "<mime_type>%s</mime_type>"
        "</blob>"
        "</field>\n",
        (unsigned long long)item->blob_size,
        item->mime_type ? item->mime_type : "application/octet-stream"
    );

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Freeing escaped strings");
    free(e_name);
    free(e_file);
    free(e_path);
}


const char* get_mime_type(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";

    ext++; // skip the dot

    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, "png") == 0)
        return "image/png";
    if (strcasecmp(ext, "gif") == 0)
        return "image/gif";
    if (strcasecmp(ext, "bmp") == 0)
        return "image/bmp";
    if (strcasecmp(ext, "pdf") == 0)
        return "application/pdf";

    return "application/octet-stream";
}


