/*
 * OCI_DDL_Grant_Module.c
 *
 * Independent DDL Module - Grant (second operation)
 * -----------------------------------------------------
 * See OCI_DDL_Grant_Module.h for the full design note. Same
 * conventions as OCI_DDL_Create_User_Module.c: tag-extraction XML
 * parsing, fail-fast validation with a single error_buf message,
 * xml_builder_t for output, logging via ctx->ddl_logger.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "OCI_DDL_Grant_Module.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define XML_INITIAL_SIZE   4096

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as OCI_DDL_Create_User_Module.c        */
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

/* Extract every <privilege>...</privilege> strictly between the
 * <privileges>...</privileges> block. Absence of the block leaves
 * privilege_count at 0 - caller (validate_grant_request) rejects
 * that. */
static void extract_privileges(const char *src, grant_request_t *req)
{
    req->privilege_count = 0;

    const char *block_start = strstr(src, "<privileges>");
    if (!block_start) return;
    const char *block_end = strstr(block_start, "</privileges>");
    if (!block_end) return;

    const char *cursor = block_start;
    while (req->privilege_count < MAX_GRANT_PRIVILEGES)
    {
        const char *tag_start = strstr(cursor, "<privilege>");
        if (!tag_start || tag_start >= block_end) break;
        tag_start += strlen("<privilege>");

        const char *tag_end = strstr(tag_start, "</privilege>");
        if (!tag_end || tag_end > block_end) break;

        size_t len = (size_t)(tag_end - tag_start);
        char *dest = req->privileges[req->privilege_count];
        if (len >= GRANT_PRIVILEGE_LEN) len = GRANT_PRIVILEGE_LEN - 1;
        memcpy(dest, tag_start, len);
        dest[len] = '\0';
        trim_inplace(dest);
        uppercase_inplace(dest);

        if (strlen(dest) > 0)
            req->privilege_count++;

        cursor = tag_end + strlen("</privilege>");
    }
}

/* ==================================================================
 *  parse_grant_request  (Definition)
 * ================================================================== */
int parse_grant_request(oci_context_t   *ctx,
                         const char      *input_xml,
                         grant_request_t *req)
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
                 "Entering parse_grant_request");

    /* ---- Mandatory: grantee ---- */
    if (!extract_xml_tag(input_xml, "grantee",
                          req->grantee, sizeof(req->grantee)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <grantee> in input XML");
        return -1;
    }
    uppercase_inplace(req->grantee);

    /* ---- Mandatory: object_type ---- */
    if (!extract_xml_tag(input_xml, "object_type",
                          req->object_type, sizeof(req->object_type)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <object_type> in input XML "
                     "(grantee='%s')", req->grantee);
        return -1;
    }
    uppercase_inplace(req->object_type);

    /* ---- Mandatory: object_name ---- */
    if (!extract_xml_tag(input_xml, "object_name",
                          req->object_name, sizeof(req->object_name)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <object_name> in input XML "
                     "(grantee='%s')", req->grantee);
        return -1;
    }
    uppercase_inplace(req->object_name);

    /* ---- Optional: owner ---- */
    extract_xml_tag(input_xml, "owner", req->owner, sizeof(req->owner));
    uppercase_inplace(req->owner);

    /* ---- Optional: with_grant_option (default 0) ---- */
    char wgo_buf[16] = {0};
    if (extract_xml_tag(input_xml, "with_grant_option", wgo_buf, sizeof(wgo_buf)))
        req->with_grant_option = (atoi(wgo_buf) != 0);
    else
        req->with_grant_option = 0;

    /* ---- Mandatory (at least one): privileges ---- */
    extract_privileges(input_xml, req);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "parse_grant_request OK: grantee='%s' object_type='%s' "
                 "object_name='%s' owner='%s' privilege_count=%d "
                 "with_grant_option=%d",
                 req->grantee, req->object_type, req->object_name,
                 req->owner, req->privilege_count, req->with_grant_option);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Validation helpers                                                  */
/* ------------------------------------------------------------------ */

/* Legal Oracle unquoted identifier - same rule as
 * OCI_DDL_Create_User_Module.c's is_valid_identifier(). */
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

static int is_valid_object_type(const char *t)
{
    return (strcmp(t, "TABLE") == 0 || strcmp(t, "VIEW") == 0);
}

/* Object-level privileges Oracle recognises for TABLE/VIEW grants.
 * ALL / ALL PRIVILEGES accepted as-is (expands to every applicable
 * privilege at execution time - not expanded here). */
static int is_recognised_privilege(const char *p)
{
    static const char *valid[] = {
        "SELECT", "INSERT", "UPDATE", "DELETE",
        "ALTER", "INDEX", "REFERENCES",
        "ALL", "ALL PRIVILEGES",
        NULL
    };
    for (int i = 0; valid[i]; i++)
        if (strcmp(p, valid[i]) == 0) return 1;
    return 0;
}

/* ALTER/INDEX/REFERENCES are table-only in Oracle - invalid on a VIEW. */
static int is_valid_for_view(const char *p)
{
    return !(strcmp(p, "ALTER") == 0 ||
             strcmp(p, "INDEX") == 0 ||
             strcmp(p, "REFERENCES") == 0);
}

/* ==================================================================
 *  validate_grant_request  (Validation)
 * ================================================================== */
int validate_grant_request(oci_context_t          *ctx,
                            const grant_request_t  *req,
                            char                    *error_buf,
                            size_t                   error_buf_size)
{
    if (!ctx || !req || !error_buf || error_buf_size == 0)
        return -1;

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering validate_grant_request grantee='%s' "
                 "object='%s.%s'",
                 req->grantee, req->owner, req->object_name);

    if (!is_valid_identifier(req->grantee))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid grantee '%s': must start with a letter and "
                 "contain only letters, digits, '_', '$' or '#'",
                 req->grantee);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (!is_valid_object_type(req->object_type))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid object_type '%s' for grantee '%s': expected "
                 "TABLE or VIEW",
                 req->object_type, req->grantee);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (!is_valid_identifier(req->object_name))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid object_name '%s' for grantee '%s'",
                 req->object_name, req->grantee);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strlen(req->owner) > 0 && !is_valid_identifier(req->owner))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid owner '%s' for object '%s'",
                 req->owner, req->object_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (req->privilege_count == 0)
    {
        snprintf(error_buf, error_buf_size,
                 "No privileges given for grantee '%s' on '%s' - at "
                 "least one <privilege> is required",
                 req->grantee, req->object_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (req->privilege_count > MAX_GRANT_PRIVILEGES)
    {
        snprintf(error_buf, error_buf_size,
                 "Too many privileges for grantee '%s': %d given, max %d",
                 req->grantee, req->privilege_count, MAX_GRANT_PRIVILEGES);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    for (int i = 0; i < req->privilege_count; i++)
    {
        if (!is_recognised_privilege(req->privileges[i]))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid privilege '%s' (position %d) for grantee "
                     "'%s' on '%s'",
                     req->privileges[i], i + 1, req->grantee, req->object_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }

        if (strcmp(req->object_type, "VIEW") == 0 &&
            !is_valid_for_view(req->privileges[i]))
        {
            snprintf(error_buf, error_buf_size,
                     "Privilege '%s' is not valid on a VIEW (object "
                     "'%s') - ALTER/INDEX/REFERENCES are table-only",
                     req->privileges[i], req->object_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "validate_grant_request OK: grantee='%s' object='%s.%s'",
                 req->grantee, req->owner, req->object_name);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  build_grant_ddl_text()                                              */
/*  Build the literal GRANT statement text. Public - see header doc     */
/*  comment for why (dispatcher.c's JSON response path).                */
/* ------------------------------------------------------------------ */
void build_grant_ddl_text(const grant_request_t *req,
                           char *out, size_t out_size)
{
    size_t used = 0;

    used += (size_t)snprintf(out + used, out_size - used, "GRANT ");

    for (int i = 0; i < req->privilege_count && used < out_size; i++)
    {
        used += (size_t)snprintf(out + used, out_size - used,
                                  "%s%s", req->privileges[i],
                                  (i < req->privilege_count - 1) ? ", " : "");
    }

    if (used < out_size)
    {
        if (strlen(req->owner) > 0)
            used += (size_t)snprintf(out + used, out_size - used,
                                      " ON %s.%s", req->owner, req->object_name);
        else
            used += (size_t)snprintf(out + used, out_size - used,
                                      " ON %s", req->object_name);
    }

    if (used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  " TO %s", req->grantee);

    if (req->with_grant_option && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  " WITH GRANT OPTION");

    if (used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, ";");
}

/* ==================================================================
 *  get_grant_template  (tgen)
 * ================================================================== */
xml_builder_t *get_grant_template(oci_context_t          *ctx,
                                   const grant_request_t  *req)
{
    if (!ctx || !req)
    {
        if (ctx)
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or req is NULL");
        return NULL;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering get_grant_template grantee='%s' object='%s.%s'",
                 req->grantee, req->owner, req->object_name);

    xml_builder_t *xml = xml_create(XML_INITIAL_SIZE);
    if (!xml)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "xml_create failed");
        return NULL;
    }

    char ddl_text[2048] = {0};
    build_grant_ddl_text(req, ddl_text, sizeof(ddl_text));

    char *e_grantee = xml_escape(req->grantee);
    char *e_type    = xml_escape(req->object_type);
    char *e_name    = xml_escape(req->object_name);
    char *e_owner   = xml_escape(req->owner);
    char *e_ddl     = xml_escape(ddl_text);

    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<Grant_Template>\n");
    xml_append(xml, "  <operation>GRANT</operation>\n");
    xml_append(xml, "  <grantee>%s</grantee>\n",           e_grantee);
    xml_append(xml, "  <object_type>%s</object_type>\n",   e_type);
    xml_append(xml, "  <object_name>%s</object_name>\n",   e_name);
    xml_append(xml, "  <owner>%s</owner>\n",               e_owner);
    xml_append(xml, "  <with_grant_option>%d</with_grant_option>\n",
               req->with_grant_option);

    xml_append(xml, "  <privileges count=\"%d\">\n", req->privilege_count);
    for (int i = 0; i < req->privilege_count; i++)
    {
        char *e_priv = xml_escape(req->privileges[i]);
        xml_append(xml, "    <privilege>%s</privilege>\n", e_priv);
        free(e_priv);
    }
    xml_append(xml, "  </privileges>\n");

    xml_append(xml, "  <generated_ddl>%s</generated_ddl>\n", e_ddl);
    xml_append(xml, "</Grant_Template>\n");

    free(e_grantee);
    free(e_type);
    free(e_name);
    free(e_owner);
    free(e_ddl);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "get_grant_template OK: grantee='%s' object='%s.%s' "
                 "privilege_count=%d ddl_len=%zu",
                 req->grantee, req->owner, req->object_name,
                 req->privilege_count, strlen(ddl_text));

    return xml;
}
