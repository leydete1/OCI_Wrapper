/*
 * OCI_DDL_Create_Procedure_Module.c
 *
 * Independent DDL Module - Create Procedure (sixth operation)
 * -------------------------------------------------------------
 * See OCI_DDL_Create_Procedure_Module.h for the full design note.
 * Same conventions as the other five DDL modules: tag-extraction XML
 * parsing, fail-fast validation with a single error_buf message,
 * xml_builder_t for output, logging via ctx->ddl_logger.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "OCI_DDL_Create_Procedure_Module.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define XML_INITIAL_SIZE   8192

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

/*
 * Parse every <parameter>...</parameter> block strictly between the
 * outer <parameters>...</parameters> block. Same nested-block slice
 * approach extract_columns() uses in OCI_DDL_Create_Table_Module.c.
 */
static void extract_parameters(const char *src, create_procedure_request_t *req)
{
    req->parameter_count = 0;

    const char *params_start = strstr(src, "<parameters>");
    if (!params_start) return;
    const char *params_end = strstr(params_start, "</parameters>");
    if (!params_end) return;

    const char *cursor = params_start;
    while (req->parameter_count < MAX_PROCEDURE_PARAMETERS)
    {
        const char *p_start = strstr(cursor, "<parameter>");
        if (!p_start || p_start >= params_end) break;
        p_start += strlen("<parameter>");

        const char *p_end = strstr(p_start, "</parameter>");
        if (!p_end || p_end > params_end) break;

        size_t block_len = (size_t)(p_end - p_start);
        char *block = malloc(block_len + 1);
        if (!block) break;
        memcpy(block, p_start, block_len);
        block[block_len] = '\0';

        ddl_procedure_param_t *param = &req->parameters[req->parameter_count];
        memset(param, 0, sizeof(*param));

        extract_xml_tag(block, "name", param->name, sizeof(param->name));
        uppercase_inplace(param->name);

        extract_xml_tag(block, "data_type", param->data_type, sizeof(param->data_type));
        uppercase_inplace(param->data_type);

        if (extract_xml_tag(block, "mode", param->mode, sizeof(param->mode)))
            uppercase_inplace(param->mode);
        else
            strncpy(param->mode, "IN", sizeof(param->mode) - 1); /* default */

        extract_xml_tag(block, "default_value", param->default_value,
                         sizeof(param->default_value));

        free(block);

        if (strlen(param->name) > 0)
            req->parameter_count++;

        cursor = p_end + strlen("</parameter>");
    }
}

/* ==================================================================
 *  parse_create_procedure_request  (Definition)
 * ================================================================== */
int parse_create_procedure_request(oci_context_t                 *ctx,
                                    const char                    *input_xml,
                                    create_procedure_request_t    *req)
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
                 "Entering parse_create_procedure_request");

    /* ---- Mandatory: procedure_name ---- */
    if (!extract_xml_tag(input_xml, "procedure_name",
                          req->procedure_name, sizeof(req->procedure_name)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <procedure_name> in input XML");
        return -1;
    }
    uppercase_inplace(req->procedure_name);

    /* ---- Mandatory: body ---- */
    if (!extract_xml_tag(input_xml, "body", req->body, sizeof(req->body)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <body> in input XML "
                     "(procedure_name='%s')", req->procedure_name);
        return -1;
    }
    /* NOT uppercased - PL/SQL body case (string literals, identifiers)
     * must be preserved as given. */

    /* ---- Optional: owner ---- */
    extract_xml_tag(input_xml, "owner", req->owner, sizeof(req->owner));
    uppercase_inplace(req->owner);

    /* ---- Optional: replace (default 0) ---- */
    char buf[16] = {0};
    if (extract_xml_tag(input_xml, "replace", buf, sizeof(buf)))
        req->replace = (atoi(buf) != 0);

    /* ---- Optional: parameters ---- */
    extract_parameters(input_xml, req);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "parse_create_procedure_request OK: procedure_name='%s' "
                 "owner='%s' replace=%d parameter_count=%d body_len=%zu",
                 req->procedure_name, req->owner, req->replace,
                 req->parameter_count, strlen(req->body));

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

static int is_recognised_param_type(const char *t)
{
    static const char *valid[] = {
        "VARCHAR2", "NUMBER", "DATE", "TIMESTAMP", "BOOLEAN",
        "PLS_INTEGER", "CLOB", "BLOB",
        NULL
    };
    for (int i = 0; valid[i]; i++)
        if (strcmp(t, valid[i]) == 0) return 1;
    return 0;
}

static int is_valid_param_mode(const char *m)
{
    return (strcmp(m, "IN") == 0 || strcmp(m, "OUT") == 0 ||
            strcmp(m, "IN OUT") == 0);
}

/* ==================================================================
 *  validate_create_procedure_request  (Validation)
 * ================================================================== */
int validate_create_procedure_request(oci_context_t                      *ctx,
                                       const create_procedure_request_t   *req,
                                       char                                *error_buf,
                                       size_t                               error_buf_size)
{
    if (!ctx || !req || !error_buf || error_buf_size == 0)
        return -1;

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering validate_create_procedure_request "
                 "procedure_name='%s'", req->procedure_name);

    if (!is_valid_identifier(req->procedure_name))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid procedure_name '%s': must start with a letter "
                 "and contain only letters, digits, '_', '$' or '#'",
                 req->procedure_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strlen(req->owner) > 0 && !is_valid_identifier(req->owner))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid owner '%s' for procedure '%s'",
                 req->owner, req->procedure_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (req->parameter_count > MAX_PROCEDURE_PARAMETERS)
    {
        snprintf(error_buf, error_buf_size,
                 "Too many parameters for procedure '%s': %d given, max %d",
                 req->procedure_name, req->parameter_count,
                 MAX_PROCEDURE_PARAMETERS);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    for (int i = 0; i < req->parameter_count; i++)
    {
        const ddl_procedure_param_t *param = &req->parameters[i];

        if (!is_valid_identifier(param->name))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid parameter name '%s' (position %d) for "
                     "procedure '%s'",
                     param->name, i + 1, req->procedure_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }

        for (int j = 0; j < i; j++)
        {
            if (strcmp(req->parameters[j].name, param->name) == 0)
            {
                snprintf(error_buf, error_buf_size,
                         "Duplicate parameter name '%s' (positions %d and "
                         "%d) for procedure '%s'",
                         param->name, j + 1, i + 1, req->procedure_name);
                logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
                return -1;
            }
        }

        if (!is_recognised_param_type(param->data_type))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid data_type '%s' for parameter '%s' in "
                     "procedure '%s': expected one of VARCHAR2, NUMBER, "
                     "DATE, TIMESTAMP, BOOLEAN, PLS_INTEGER, CLOB, BLOB",
                     param->data_type, param->name, req->procedure_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }

        if (!is_valid_param_mode(param->mode))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid mode '%s' for parameter '%s' in procedure "
                     "'%s': expected IN, OUT, or IN OUT",
                     param->mode, param->name, req->procedure_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }

        if (strlen(param->default_value) > 0 && strcmp(param->mode, "IN") != 0)
        {
            snprintf(error_buf, error_buf_size,
                     "Parameter '%s' in procedure '%s' has a default_value "
                     "but mode is '%s' - DEFAULT is only allowed on IN "
                     "parameters",
                     param->name, req->procedure_name, param->mode);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }
    }

    if (strlen(req->body) == 0)
    {
        snprintf(error_buf, error_buf_size,
                 "No <body> given for procedure '%s'", req->procedure_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strlen(req->body) >= PROCEDURE_BODY_LEN - 1)
    {
        snprintf(error_buf, error_buf_size,
                 "<body> for procedure '%s' is at or over the %d "
                 "character limit",
                 req->procedure_name, PROCEDURE_BODY_LEN);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "validate_create_procedure_request OK: procedure_name='%s' "
                 "parameter_count=%d body_len=%zu",
                 req->procedure_name, req->parameter_count, strlen(req->body));
    return 0;
}

/* ------------------------------------------------------------------ */
/*  build_create_procedure_ddl_text()                                   */
/* ------------------------------------------------------------------ */
void build_create_procedure_ddl_text(const create_procedure_request_t *req,
                                      char *out, size_t out_size)
{
    size_t used = 0;

    used += (size_t)snprintf(out + used, out_size - used, "CREATE ");

    if (req->replace && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, "OR REPLACE ");

    if (used < out_size)
    {
        if (strlen(req->owner) > 0)
            used += (size_t)snprintf(out + used, out_size - used,
                                      "PROCEDURE %s.%s",
                                      req->owner, req->procedure_name);
        else
            used += (size_t)snprintf(out + used, out_size - used,
                                      "PROCEDURE %s", req->procedure_name);
    }

    if (used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, " (\n");

    for (int i = 0; i < req->parameter_count && used < out_size; i++)
    {
        const ddl_procedure_param_t *param = &req->parameters[i];

        used += (size_t)snprintf(out + used, out_size - used,
                                  "  %s %s %s", param->name, param->mode,
                                  param->data_type);

        if (strlen(param->default_value) > 0 && used < out_size)
            used += (size_t)snprintf(out + used, out_size - used,
                                      " DEFAULT %s", param->default_value);

        if (used < out_size)
            used += (size_t)snprintf(out + used, out_size - used,
                                      "%s\n",
                                      (i < req->parameter_count - 1) ? "," : "");
    }

    if (used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  ")\nAS\nBEGIN\n%s\nEND %s;",
                                  req->body, req->procedure_name);
}

/* ==================================================================
 *  get_create_procedure_template  (tgen)
 * ================================================================== */
xml_builder_t *get_create_procedure_template(oci_context_t                      *ctx,
                                              const create_procedure_request_t  *req)
{
    if (!ctx || !req)
    {
        if (ctx)
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or req is NULL");
        return NULL;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering get_create_procedure_template procedure_name='%s'",
                 req->procedure_name);

    xml_builder_t *xml = xml_create(XML_INITIAL_SIZE);
    if (!xml)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "xml_create failed");
        return NULL;
    }

    char ddl_text[8192 + PROCEDURE_BODY_LEN] = {0};
    build_create_procedure_ddl_text(req, ddl_text, sizeof(ddl_text));

    char *e_name = xml_escape(req->procedure_name);
    char *e_owner = xml_escape(req->owner);
    char *e_body = xml_escape(req->body);
    char *e_ddl  = xml_escape(ddl_text);

    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<Create_Procedure_Template>\n");
    xml_append(xml, "  <operation>CREATE_PROCEDURE</operation>\n");
    xml_append(xml, "  <procedure_name>%s</procedure_name>\n", e_name);
    xml_append(xml, "  <owner>%s</owner>\n",                   e_owner);
    xml_append(xml, "  <replace>%d</replace>\n",               req->replace);

    xml_append(xml, "  <parameters count=\"%d\">\n", req->parameter_count);
    for (int i = 0; i < req->parameter_count; i++)
    {
        const ddl_procedure_param_t *param = &req->parameters[i];
        char *e_p_name = xml_escape(param->name);
        char *e_p_type = xml_escape(param->data_type);
        char *e_p_mode = xml_escape(param->mode);
        char *e_p_default = xml_escape(param->default_value);

        xml_append(xml, "    <parameter>\n");
        xml_append(xml, "      <name>%s</name>\n", e_p_name);
        xml_append(xml, "      <data_type>%s</data_type>\n", e_p_type);
        xml_append(xml, "      <mode>%s</mode>\n", e_p_mode);
        if (strlen(param->default_value) > 0)
            xml_append(xml, "      <default_value>%s</default_value>\n", e_p_default);
        xml_append(xml, "    </parameter>\n");

        free(e_p_name);
        free(e_p_type);
        free(e_p_mode);
        free(e_p_default);
    }
    xml_append(xml, "  </parameters>\n");

    xml_append(xml, "  <body>%s</body>\n", e_body);
    xml_append(xml, "  <generated_ddl>%s</generated_ddl>\n", e_ddl);
    xml_append(xml, "</Create_Procedure_Template>\n");

    free(e_name);
    free(e_owner);
    free(e_body);
    free(e_ddl);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "get_create_procedure_template OK: procedure_name='%s' "
                 "parameter_count=%d ddl_len=%zu",
                 req->procedure_name, req->parameter_count, strlen(ddl_text));

    return xml;
}
