/*
 * OCI_DDL_Drop_Table_Module.c
 *
 * Independent DDL Module - Drop Table (fourth operation)
 * --------------------------------------------------------
 * See OCI_DDL_Drop_Table_Module.h for the full design note. Same
 * conventions as the other three DDL modules: tag-extraction XML
 * parsing, fail-fast validation with a single error_buf message,
 * xml_builder_t for output, logging via ctx->ddl_logger.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "OCI_DDL_Drop_Table_Module.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define XML_INITIAL_SIZE   2048

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as the other DDL modules               */
/* ------------------------------------------------------------------ */
static void trim_inplace(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    { s[len - 1] = '\0'; len--; }
}

static void uppercase_inplace(char *s)
{
    if (!s) return;
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

static int extract_xml_tag(const char *src, const char *tag,
                            char *dest, size_t dest_max)
{
    if (!src || !tag || !dest) return 0;

    char open_tag [136];
    char close_tag[136];
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
 *  parse_drop_table_request  (Definition)
 * ================================================================== */
int parse_drop_table_request(oci_context_t          *ctx,
                              const char             *input_xml,
                              drop_table_request_t   *req)
{
    if (!ctx || !input_xml || !req)
    {
        if (ctx)
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx, input_xml or req is NULL");
        return -1;
    }

    memset(req, 0, sizeof(*req));

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering parse_drop_table_request");

    /* ---- Mandatory: table_name ---- */
    if (!extract_xml_tag(input_xml, "table_name",
                          req->table_name, sizeof(req->table_name)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <table_name> in input XML");
        return -1;
    }
    uppercase_inplace(req->table_name);

    /* ---- Optional: owner ---- */
    extract_xml_tag(input_xml, "owner", req->owner, sizeof(req->owner));
    uppercase_inplace(req->owner);

    /* ---- Optional: cascade_constraints (default 0) ---- */
    char buf[16] = {0};
    if (extract_xml_tag(input_xml, "cascade_constraints", buf, sizeof(buf)))
        req->cascade_constraints = (atoi(buf) != 0);

    /* ---- Optional: purge (default 0) ---- */
    buf[0] = '\0';
    if (extract_xml_tag(input_xml, "purge", buf, sizeof(buf)))
        req->purge = (atoi(buf) != 0);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "parse_drop_table_request OK: table_name='%s' owner='%s' "
                 "cascade_constraints=%d purge=%d",
                 req->table_name, req->owner, req->cascade_constraints,
                 req->purge);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Validation helpers                                                  */
/* ------------------------------------------------------------------ */
static int is_valid_identifier(const char *s)
{
    if (!s || strlen(s) == 0) return 0;
    if (!isalpha((unsigned char)s[0])) return 0;
    for (const char *p = s; *p; p++)
    {
        if (!isalnum((unsigned char)*p) &&
            *p != '_' && *p != '$' && *p != '#')
            return 0;
    }
    return 1;
}

/* ==================================================================
 *  validate_drop_table_request  (Validation)
 * ================================================================== */
int validate_drop_table_request(oci_context_t                *ctx,
                                 const drop_table_request_t   *req,
                                 char                          *error_buf,
                                 size_t                         error_buf_size)
{
    if (!ctx || !req || !error_buf || error_buf_size == 0)
        return -1;

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering validate_drop_table_request table_name='%s' "
                 "owner='%s'", req->table_name, req->owner);

    if (!is_valid_identifier(req->table_name))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid table_name '%s': must start with a letter and "
                 "contain only letters, digits, '_', '$' or '#'",
                 req->table_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strlen(req->owner) > 0 && !is_valid_identifier(req->owner))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid owner '%s' for table '%s'",
                 req->owner, req->table_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "validate_drop_table_request OK: table_name='%s' owner='%s'",
                 req->table_name, req->owner);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  build_drop_table_ddl_text()                                         */
/* ------------------------------------------------------------------ */
void build_drop_table_ddl_text(const drop_table_request_t *req,
                                char *out, size_t out_size)
{
    size_t used = 0;

    if (strlen(req->owner) > 0)
        used += (size_t)snprintf(out + used, out_size - used,
                                  "DROP TABLE %s.%s",
                                  req->owner, req->table_name);
    else
        used += (size_t)snprintf(out + used, out_size - used,
                                  "DROP TABLE %s", req->table_name);

    if (req->cascade_constraints && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  " CASCADE CONSTRAINTS");

    if (req->purge && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, " PURGE");

    if (used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, ";");
}

/* ==================================================================
 *  get_drop_table_template  (tgen)
 * ================================================================== */
xml_builder_t *get_drop_table_template(oci_context_t                *ctx,
                                        const drop_table_request_t  *req)
{
    if (!ctx || !req)
    {
        if (ctx)
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or req is NULL");
        return NULL;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering get_drop_table_template table_name='%s' owner='%s'",
                 req->table_name, req->owner);

    xml_builder_t *xml = xml_create(XML_INITIAL_SIZE);
    if (!xml)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "xml_create failed");
        return NULL;
    }

    char ddl_text[512] = {0};
    build_drop_table_ddl_text(req, ddl_text, sizeof(ddl_text));

    char *e_name  = xml_escape(req->table_name);
    char *e_owner = xml_escape(req->owner);
    char *e_ddl   = xml_escape(ddl_text);

    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<Drop_Table_Template>\n");
    xml_append(xml, "  <operation>DROP_TABLE</operation>\n");
    xml_append(xml, "  <table_name>%s</table_name>\n", e_name);
    xml_append(xml, "  <owner>%s</owner>\n",           e_owner);
    xml_append(xml, "  <cascade_constraints>%d</cascade_constraints>\n",
               req->cascade_constraints);
    xml_append(xml, "  <purge>%d</purge>\n", req->purge);
    xml_append(xml, "  <generated_ddl>%s</generated_ddl>\n", e_ddl);
    xml_append(xml, "</Drop_Table_Template>\n");

    free(e_name);
    free(e_owner);
    free(e_ddl);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "get_drop_table_template OK: table_name='%s' owner='%s' "
                 "ddl_len=%zu",
                 req->table_name, req->owner, strlen(ddl_text));

    return xml;
}
