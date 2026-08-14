/*
 * OCI_Insert_Template_Module.c
 *
 * Stage 1 - Insert Template Builder
 * ----------------------------------
 * Queries ALL_TAB_COLUMNS for the requested table and returns a
 * fully-formed <Insert Template> XML with one <field> block per
 * column.  The <insert_value> element for each column is left empty
 * for the caller / Stage-2 to populate.
 *
 * Design notes
 * ------------
 *  - Green-field module; does NOT touch execute_query_batch().
 *  - Uses the same oci_context_t / logger_t / xml_builder_t types as
 *    the rest of the project.
 *  - ALL_TAB_COLUMNS is used (accessible for own-schema tables and any
 *    table on which SELECT privilege exists).  Switch to DBA_TAB_COLUMNS
 *    for DBA-level callers if needed.
 *  - Column rows are returned in COLUMN_ID order so the template
 *    matches the physical column order of the table.
 *  - DATA_DEFAULT can be NULL; we convert that to an empty string.
 *  - Precision / scale of -1 means "not applicable for this type".
 *
 * XML output layout (one <field> block per column)
 * -------------------------------------------------
 *  <Insert Template>
 *    <operation>INSERT</operation>
 *    <table_name>OCI_TEST_FIELDS</table_name>
 *    <row>
 *      <field>
 *        <field_name>NUMBER_COL</field_name>
 *        <field_type>NUMBER</field_type>
 *        <field_length>22</field_length>
 *        <field_precision>-1</field_precision>
 *        <field_scale>-1</field_scale>
 *        <field_nullable>Y</field_nullable>
 *        <field_default></field_default>
 *        <insert_value></insert_value>
 *      </field>
 *      ...
 *    </row>
 *  </Insert Template>
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "OCI_Connection.h"
#include "OCI_Table_Metadata_Module.h"
#include "metadata_cache.h"
#include "metadata_cache_meta.h"

#include "OCI_Insert_Template_Module.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Local OCI error macro (same pattern as the rest of the project)    */
/* ------------------------------------------------------------------ */
#define CHECK_OCI_TMPL(errhp, status, ctx, label)                       \
    do {                                                                 \
        if ((status) != OCI_SUCCESS &&                                  \
            (status) != OCI_SUCCESS_WITH_INFO)                          \
        {                                                                \
            text   _errbuf[512];                                         \
            sb4    _errcode = 0;                                         \
            OCIErrorGet((errhp), 1, NULL, &_errcode,                    \
                        _errbuf, sizeof(_errbuf), OCI_HTYPE_ERROR);     \
            logger_write((ctx)->logger, LOG_ERROR, __func__, 0,         \
                         "OCI Error %d: %s", _errcode,                  \
                         (char *)_errbuf);                               \
            rc = -1;                                                     \
            goto label;                                                  \
        }                                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define MAX_COLUMNS        1024
#define QUERY_BUF_SIZE     2048
#define XML_INITIAL_SIZE   32768

/* ------------------------------------------------------------------ */
/*  Static helper: trim leading / trailing whitespace in-place         */
/* ------------------------------------------------------------------ */
static void trim_inplace(char *s)
{
    if (!s) return;

    /* Leading */
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    /* Trailing */
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    {
        s[len - 1] = '\0';
        len--;
    }
}

/* ------------------------------------------------------------------ */
/*  Static helper: upper-case a string in-place                        */
/* ------------------------------------------------------------------ */
static void uppercase_inplace(char *s)
{
    if (!s) return;
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

/* ------------------------------------------------------------------ */
/*  Static helper: extract the text content of the first occurrence    */
/*  of a simple <tag>value</tag> pair from src into dest (max bytes).  */
/*  Returns 1 if found, 0 if not.                                      */
/* ------------------------------------------------------------------ */
static int extract_xml_tag(const char *src,
                            const char *tag,
                            char       *dest,
                            size_t      dest_max)
{
    if (!src || !tag || !dest) return 0;

    /* Build open / close tag strings */
    char open_tag [128];
    char close_tag[128];
    snprintf(open_tag,  sizeof(open_tag),  "<%s>",  tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(src, open_tag);
    if (!start) return 0;
    start += strlen(open_tag);

    const char *end = strstr(start, close_tag);
    if (!end) return 0;

    size_t len = (size_t)(end - start);
    if (len >= dest_max) len = dest_max - 1;

    memcpy(dest, start, len);
    dest[len] = '\0';
    trim_inplace(dest);
    return 1;
}

/* ==================================================================
 *  parse_template_request
 *  Parse <Template Request> XML into a template_request_t struct.
 * ================================================================== */
int parse_template_request(oci_context_t      *ctx,
                            const char         *input_xml,
                            template_request_t *req)
{
    int rc = 0;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Entering parse_template_request");

    if (!ctx || !input_xml || !req)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments: ctx, input_xml or req is NULL");
        return -1;
    }

    memset(req, 0, sizeof(*req));

    /* ---- Extract <operation> ---- */
    if (!extract_xml_tag(input_xml, "operation",
                         req->operation, sizeof(req->operation)))
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <operation> in input XML");
        rc = -1;
        goto Cleanup;
    }
    trim_inplace(req->operation);
    uppercase_inplace(req->operation);
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Parsed operation='%s'", req->operation);

    /* ---- Validate operation ---- */
    if (strcmp(req->operation, "INSERT") != 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Unsupported operation '%s'; expected INSERT",
                     req->operation);
        rc = -1;
        goto Cleanup;
    }

    /* ---- Extract <TABLE> ---- */
    if (!extract_xml_tag(input_xml, "TABLE",
                         req->table_name, sizeof(req->table_name)))
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <TABLE> in input XML");
        rc = -1;
        goto Cleanup;
    }
    trim_inplace(req->table_name);
    uppercase_inplace(req->table_name);
    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Parsed table_name='%s'", req->table_name);

    if (strlen(req->table_name) == 0)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Empty table name in <TABLE> element");
        rc = -1;
        goto Cleanup;
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "parse_template_request OK: operation=%s table=%s",
                 req->operation, req->table_name);

Cleanup:
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Static helper: resolve a type-default string from ini config.      */
/*  Returns a pointer to the relevant config string (never NULL).      */
/*  If insert_table_defaults=0 or no matching type, returns "".        */
/* ------------------------------------------------------------------ */
static const char *resolve_insert_default(oci_context_t           *ctx,
                                           const col_metadata_t *col)
{
    if (!ctx->ini->insert_table_defaults)
        return "";

    const char *t = col->data_type;

    /* If the Oracle column already carries a DATA_DEFAULT, prefer it  */
    if (strlen(col->data_default) > 0)
        return col->data_default;

    /* Map Oracle type string to the matching ini field                */
    if (strncmp(t, "NUMBER",  6) == 0 ||
        strcmp (t, "INTEGER")    == 0 ||
        strcmp (t, "INT")        == 0 ||
        strcmp (t, "SMALLINT")   == 0 ||
        strcmp (t, "DECIMAL")    == 0 ||
        strcmp (t, "NUMERIC")    == 0)
        return ctx->ini->insert_default_number;

    if (strncmp(t, "FLOAT", 5) == 0 ||
        strcmp (t, "REAL")      == 0 ||
        strcmp (t, "DOUBLE PRECISION") == 0)
        return ctx->ini->insert_default_float;

    if (strcmp(t, "BINARY_FLOAT")  == 0)
        return ctx->ini->insert_default_binary_float;

    if (strcmp(t, "BINARY_DOUBLE") == 0)
        return ctx->ini->insert_default_binary_double;

    if (strcmp(t, "CHAR")    == 0)  return ctx->ini->insert_default_char;
    if (strcmp(t, "VARCHAR2")== 0)  return ctx->ini->insert_default_varchar2;
    if (strcmp(t, "NCHAR")   == 0)  return ctx->ini->insert_default_nchar;
    if (strcmp(t, "NVARCHAR2")== 0) return ctx->ini->insert_default_nvarchar2;

    if (strcmp(t, "DATE")    == 0)  return ctx->ini->insert_default_date;

    if (strncmp(t, "TIMESTAMP", 9) == 0)
        return ctx->ini->insert_default_timestamp;

    if (strstr(t, "INTERVAL") && strstr(t, "MONTH"))
        return ctx->ini->insert_default_interval_ym;

    if (strstr(t, "INTERVAL") && strstr(t, "SECOND"))
        return ctx->ini->insert_default_interval_ds;

    if (strcmp(t, "RAW")     == 0)  return ctx->ini->insert_default_raw;
    if (strcmp(t, "CLOB")    == 0)  return ctx->ini->insert_default_clob;
    if (strcmp(t, "NCLOB")   == 0)  return ctx->ini->insert_default_nclob;
    if (strcmp(t, "BLOB")    == 0)  return ctx->ini->insert_default_blob;
    if (strcmp(t, "ROWID")   == 0)  return ctx->ini->insert_default_rowid;
    if (strcmp(t, "UROWID")  == 0)  return ctx->ini->insert_default_urowid;

    return "";  /* unrecognised type - leave value empty */
}

/* ------------------------------------------------------------------ */
/*  Static helper: emit one <field> block into the xml_builder_t       */
/* ------------------------------------------------------------------ */
static void emit_field_xml(xml_builder_t          *xml,
                            oci_context_t          *ctx,
                            const col_metadata_t *col,
                            int                     field_num)
{
    const char *ins_val = resolve_insert_default(ctx, col);

    char *e_name    = xml_escape(col->col_name);
    char *e_type    = xml_escape(col->data_type);
    char *e_null    = xml_escape(col->nullable);
    char *e_default = xml_escape(col->data_default);
    char *e_insval  = xml_escape(ins_val);

    xml_append(xml,
        "      <field>\n"
        "        <field_number>%d</field_number>\n"
        "        <field_name>%s</field_name>\n"
        "        <field_type>%s</field_type>\n"
        "        <field_length>%d</field_length>\n"
        "        <field_precision>%d</field_precision>\n"
        "        <field_scale>%d</field_scale>\n"
        "        <field_nullable>%s</field_nullable>\n"
        "        <field_default>%s</field_default>\n"
        "        <insert_value>%s</insert_value>\n"
        "      </field>\n",
        field_num,
        e_name,
        e_type,
        col->data_length,
        col->data_precision,
        col->data_scale,
        e_null,
        e_default,
        e_insval);

    free(e_name);
    free(e_type);
    free(e_null);
    free(e_default);
    free(e_insval);
}

/* ==================================================================

/* ==================================================================
 *  get_insert_template
 *  Main Stage-1 entry point.
 *
 *  Calls get_request_metadata() to populate column metadata, then
 *  builds the <Insert_Template> XML from the results.
 *  All OCI metadata work is now owned by OCI_Table_Metadata_Module.
 * ================================================================== */
xml_builder_t *get_insert_template(oci_context_t            *ctx,
                                    const template_request_t *req)
{
    int            rc  = 0;
    xml_builder_t *xml = NULL;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Entering get_insert_template table='%s'",
                 req ? req->table_name : "(null)");

    if (!ctx || !req)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments: ctx or req is NULL");
        return NULL;
    }

    /* ----------------------------------------------------------------
     *  Step 1: Populate column metadata via shared metadata module
     * ---------------------------------------------------------------- */
    col_metadata_t     cols[MAX_TABLE_COLUMNS];
    int                col_count = 0;
    metadata_request_t meta_req;

    memset(&meta_req, 0, sizeof(meta_req));
    strncpy(meta_req.table_name, req->table_name,
            sizeof(meta_req.table_name) - 1);
    strncpy(meta_req.owner, req->owner,
            sizeof(meta_req.owner) - 1);

    if (metadata_cache_get_or_fetch(ctx->metadata_cache,
                                        ctx,
                                        &meta_req,
                                        cols,
                                        &col_count,
                                        MAX_TABLE_COLUMNS,
										NULL) != 0)
       {
           logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                        "metadata_cache_get_or_fetch failed "
                        "table='%s'", req->table_name);
           return NULL;
       }


    /* Copy resolved owner back so XML can include it */
    char resolved_owner[128] = {0};
    strncpy(resolved_owner, meta_req.owner, sizeof(resolved_owner) - 1);

    /* ----------------------------------------------------------------
     *  Step 2: Build XML template from metadata
     * ---------------------------------------------------------------- */
    xml = xml_create(XML_INITIAL_SIZE);
    if (!xml)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "xml_create failed");
        return NULL;
    }

    char *e_table = xml_escape(req->table_name);
    char *e_op    = xml_escape(req->operation);
    char *e_owner = xml_escape(resolved_owner);

    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<Insert_Template>\n");
    xml_append(xml, "  <operation>%s</operation>\n",   e_op);
    xml_append(xml, "  <table_name>%s</table_name>\n", e_table);
    xml_append(xml, "  <owner>%s</owner>\n",           e_owner);
    xml_append(xml, "  <row number=\"1\">\n");

    free(e_table);
    free(e_op);
    free(e_owner);

    for (int i = 0; i < col_count; i++)
        emit_field_xml(xml, ctx, &cols[i], i + 1);

    xml_append(xml, "  </row>\n");
    xml_append(xml, "  <!-- Add further <row number=\"N\"> blocks for "
                    "bulk insert (max %d rows) -->\n",
                    ctx->ini->max_bulk_inserts);
    xml_append(xml, "  <column_count>%d</column_count>\n", col_count);
    xml_append(xml, "</Insert_Template>\n");

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "get_insert_template OK: table='%s' owner='%s' "
                 "columns=%d",
                 req->table_name, resolved_owner, col_count);

    return xml;
}
