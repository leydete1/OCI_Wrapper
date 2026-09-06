/*
 * OCI_DDL_Create_View_Module.c
 *
 * Independent DDL Module - Create View (fifth operation)
 * --------------------------------------------------------
 * See OCI_DDL_Create_View_Module.h for the full design note. Same
 * conventions as the other four DDL modules: tag-extraction XML
 * parsing, fail-fast validation with a single error_buf message,
 * xml_builder_t for output, logging via ctx->ddl_logger.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>   /* strncasecmp */

#include "OCI_DDL_Create_View_Module.h"
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

/* Extract every <column>...</column> found strictly between the
 * <columns>...</columns> block (if present). Same pattern as
 * extract_roles()/extract_privileges() in the sibling DDL modules. */
static void extract_view_columns(const char *src, create_view_request_t *req)
{
    req->column_count = 0;

    const char *block_start = strstr(src, "<columns>");
    if (!block_start) return;
    const char *block_end = strstr(block_start, "</columns>");
    if (!block_end) return;

    const char *cursor = block_start;
    while (req->column_count < MAX_VIEW_COLUMNS)
    {
        const char *tag_start = strstr(cursor, "<column>");
        if (!tag_start || tag_start >= block_end) break;
        tag_start += strlen("<column>");

        const char *tag_end = strstr(tag_start, "</column>");
        if (!tag_end || tag_end > block_end) break;

        size_t len = (size_t)(tag_end - tag_start);
        char *dest = req->columns[req->column_count];
        if (len >= VIEW_IDENTIFIER_LEN) len = VIEW_IDENTIFIER_LEN - 1;
        memcpy(dest, tag_start, len);
        dest[len] = '\0';
        trim_inplace(dest);
        uppercase_inplace(dest);

        if (strlen(dest) > 0)
            req->column_count++;

        cursor = tag_end + strlen("</column>");
    }
}

/* ==================================================================
 *  parse_create_view_request  (Definition)
 * ================================================================== */
int parse_create_view_request(oci_context_t            *ctx,
                               const char               *input_xml,
                               create_view_request_t    *req)
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
                 "Entering parse_create_view_request");

    /* ---- Mandatory: view_name ---- */
    if (!extract_xml_tag(input_xml, "view_name",
                          req->view_name, sizeof(req->view_name)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <view_name> in input XML");
        return -1;
    }
    uppercase_inplace(req->view_name);

    /* ---- Mandatory: query ---- */
    if (!extract_xml_tag(input_xml, "query", req->query, sizeof(req->query)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <query> in input XML (view_name='%s')",
                     req->view_name);
        return -1;
    }
    /* NOT uppercased - query text case (string literals, mixed-case
     * aliases) must be preserved as given. */

    /* ---- Optional: owner ---- */
    extract_xml_tag(input_xml, "owner", req->owner, sizeof(req->owner));
    uppercase_inplace(req->owner);

    /* ---- Optional: replace (default 0) ---- */
    char buf[16] = {0};
    if (extract_xml_tag(input_xml, "replace", buf, sizeof(buf)))
        req->replace = (atoi(buf) != 0);

    /* ---- Optional: force (default 0) ---- */
    buf[0] = '\0';
    if (extract_xml_tag(input_xml, "force", buf, sizeof(buf)))
        req->force = (atoi(buf) != 0);

    /* ---- Optional: columns ---- */
    extract_view_columns(input_xml, req);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "parse_create_view_request OK: view_name='%s' owner='%s' "
                 "replace=%d force=%d column_count=%d query_len=%zu",
                 req->view_name, req->owner, req->replace, req->force,
                 req->column_count, strlen(req->query));

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
 *  validate_create_view_request  (Validation)
 * ================================================================== */
int validate_create_view_request(oci_context_t                 *ctx,
                                  const create_view_request_t   *req,
                                  char                           *error_buf,
                                  size_t                          error_buf_size)
{
    if (!ctx || !req || !error_buf || error_buf_size == 0)
        return -1;

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering validate_create_view_request view_name='%s'",
                 req->view_name);

    if (!is_valid_identifier(req->view_name))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid view_name '%s': must start with a letter and "
                 "contain only letters, digits, '_', '$' or '#'",
                 req->view_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strlen(req->owner) > 0 && !is_valid_identifier(req->owner))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid owner '%s' for view '%s'",
                 req->owner, req->view_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (req->column_count > MAX_VIEW_COLUMNS)
    {
        snprintf(error_buf, error_buf_size,
                 "Too many column aliases for view '%s': %d given, max %d",
                 req->view_name, req->column_count, MAX_VIEW_COLUMNS);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    for (int i = 0; i < req->column_count; i++)
    {
        if (!is_valid_identifier(req->columns[i]))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid column alias '%s' (position %d) for view '%s'",
                     req->columns[i], i + 1, req->view_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }
    }

    if (strlen(req->query) == 0)
    {
        snprintf(error_buf, error_buf_size,
                 "No <query> given for view '%s'", req->view_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    /* Skip leading whitespace before checking the SELECT prefix, same
     * tolerance a human-authored query might have. */
    const char *q = req->query;
    while (*q && isspace((unsigned char)*q)) q++;

    if (strncasecmp(q, "SELECT", 6) != 0)
    {
        snprintf(error_buf, error_buf_size,
                 "query for view '%s' must start with SELECT",
                 req->view_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strchr(req->query, ';') != NULL)
    {
        snprintf(error_buf, error_buf_size,
                 "query for view '%s' must not contain ';' - a CREATE "
                 "VIEW ... AS clause takes exactly one statement",
                 req->view_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "validate_create_view_request OK: view_name='%s' "
                 "column_count=%d query_len=%zu",
                 req->view_name, req->column_count, strlen(req->query));
    return 0;
}

/* ------------------------------------------------------------------ */
/*  build_create_view_ddl_text()                                        */
/* ------------------------------------------------------------------ */
void build_create_view_ddl_text(const create_view_request_t *req,
                                 char *out, size_t out_size)
{
    size_t used = 0;

    used += (size_t)snprintf(out + used, out_size - used, "CREATE ");

    if (req->replace && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, "OR REPLACE ");

    if (req->force && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, "FORCE ");

    if (used < out_size)
    {
        if (strlen(req->owner) > 0)
            used += (size_t)snprintf(out + used, out_size - used,
                                      "VIEW %s.%s", req->owner, req->view_name);
        else
            used += (size_t)snprintf(out + used, out_size - used,
                                      "VIEW %s", req->view_name);
    }

    if (req->column_count > 0 && used < out_size)
    {
        used += (size_t)snprintf(out + used, out_size - used, " (");
        for (int i = 0; i < req->column_count && used < out_size; i++)
        {
            used += (size_t)snprintf(out + used, out_size - used,
                                      "%s%s", req->columns[i],
                                      (i < req->column_count - 1) ? ", " : "");
        }
        if (used < out_size)
            used += (size_t)snprintf(out + used, out_size - used, ")");
    }

    if (used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  "\nAS\n%s;", req->query);
}

/* ==================================================================
 *  get_create_view_template  (tgen)
 * ================================================================== */
xml_builder_t *get_create_view_template(oci_context_t                 *ctx,
                                         const create_view_request_t  *req)
{
    if (!ctx || !req)
    {
        if (ctx)
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or req is NULL");
        return NULL;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering get_create_view_template view_name='%s'",
                 req->view_name);

    xml_builder_t *xml = xml_create(XML_INITIAL_SIZE);
    if (!xml)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "xml_create failed");
        return NULL;
    }

    char ddl_text[8192] = {0};
    build_create_view_ddl_text(req, ddl_text, sizeof(ddl_text));

    char *e_name  = xml_escape(req->view_name);
    char *e_owner = xml_escape(req->owner);
    char *e_query = xml_escape(req->query);
    char *e_ddl   = xml_escape(ddl_text);

    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<Create_View_Template>\n");
    xml_append(xml, "  <operation>CREATE_VIEW</operation>\n");
    xml_append(xml, "  <view_name>%s</view_name>\n", e_name);
    xml_append(xml, "  <owner>%s</owner>\n",         e_owner);
    xml_append(xml, "  <replace>%d</replace>\n",     req->replace);
    xml_append(xml, "  <force>%d</force>\n",         req->force);

    xml_append(xml, "  <columns count=\"%d\">\n", req->column_count);
    for (int i = 0; i < req->column_count; i++)
    {
        char *e_col = xml_escape(req->columns[i]);
        xml_append(xml, "    <column>%s</column>\n", e_col);
        free(e_col);
    }
    xml_append(xml, "  </columns>\n");

    xml_append(xml, "  <query>%s</query>\n", e_query);
    xml_append(xml, "  <generated_ddl>%s</generated_ddl>\n", e_ddl);
    xml_append(xml, "</Create_View_Template>\n");

    free(e_name);
    free(e_owner);
    free(e_query);
    free(e_ddl);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "get_create_view_template OK: view_name='%s' "
                 "column_count=%d ddl_len=%zu",
                 req->view_name, req->column_count, strlen(ddl_text));

    return xml;
}
