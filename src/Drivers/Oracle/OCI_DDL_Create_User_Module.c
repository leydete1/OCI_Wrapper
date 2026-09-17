/*
 * OCI_DDL_Create_User_Module.c
 *
 * Independent DDL Module - Create User (first operation)
 * --------------------------------------------------------
 * See OCI_DDL_Create_User_Module.h for the full design note. This
 * file implements all three stages for CREATE USER:
 *
 *   parse_create_user_request()    - Definition
 *   validate_create_user_request() - Validation
 *   get_create_user_template()     - tgen (DDL text generation)
 *
 * Follows the same conventions as OCI_Insert_Template_Module.c /
 * OCI_Insert_Validate_Module.c: simple tag-extraction XML parsing
 * (no third-party XML lib), fail-fast validation with a single
 * error_buf message, xml_builder_t for output construction, and
 * logging via the module's own ctx logger (ctx->ddl_logger here,
 * already present on oci_context_t).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "OCI_DDL_Create_User_Module.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define XML_INITIAL_SIZE   4096

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as OCI_Insert_Template_Module.c        */
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

/* Extract text between <tag> and </tag>; returns 1 if found, 0 if not */
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
 * Extract every <role>...</role> found strictly between the
 * <roles>...</roles> block (if present). Populates req->roles /
 * req->role_count. Absence of a <roles> block is not an error - it
 * just means role_count stays 0.
 */
static void extract_roles(const char *src, create_user_request_t *req)
{
    req->role_count = 0;

    const char *roles_start = strstr(src, "<roles>");
    if (!roles_start) return;
    const char *roles_end = strstr(roles_start, "</roles>");
    if (!roles_end) return;

    const char *cursor = roles_start;
    while (req->role_count < MAX_CREATE_USER_ROLES)
    {
        const char *tag_start = strstr(cursor, "<role>");
        if (!tag_start || tag_start >= roles_end) break;
        tag_start += strlen("<role>");

        const char *tag_end = strstr(tag_start, "</role>");
        if (!tag_end || tag_end > roles_end) break;

        size_t len = (size_t)(tag_end - tag_start);
        char *dest = req->roles[req->role_count];
        if (len >= DDL_IDENTIFIER_LEN) len = DDL_IDENTIFIER_LEN - 1;
        memcpy(dest, tag_start, len);
        dest[len] = '\0';
        trim_inplace(dest);
        uppercase_inplace(dest);

        if (strlen(dest) > 0)
            req->role_count++;

        cursor = tag_end + strlen("</role>");
    }
}

/* ==================================================================
 *  parse_create_user_request  (Definition)
 * ================================================================== */
int parse_create_user_request(oci_context_t          *ctx,
                               const char             *input_xml,
                               create_user_request_t  *req)
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
                 "Entering parse_create_user_request");

    /* ---- Mandatory: username ---- */
    if (!extract_xml_tag(input_xml, "username",
                          req->username, sizeof(req->username)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <username> in input XML");
        return -1;
    }
    uppercase_inplace(req->username);

    /* ---- Mandatory: identified_by (plain text password) ---- */
    if (!extract_xml_tag(input_xml, "identified_by",
                          req->identified_by, sizeof(req->identified_by)))
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "Failed to find <identified_by> in input XML "
                     "(username='%s')", req->username);
        return -1;
    }

    /* ---- Optional fields ---- */
    extract_xml_tag(input_xml, "default_tablespace",
                     req->default_tablespace, sizeof(req->default_tablespace));
    uppercase_inplace(req->default_tablespace);

    extract_xml_tag(input_xml, "temp_tablespace",
                     req->temp_tablespace, sizeof(req->temp_tablespace));
    uppercase_inplace(req->temp_tablespace);

    extract_xml_tag(input_xml, "quota", req->quota, sizeof(req->quota));
    uppercase_inplace(req->quota);

    extract_xml_tag(input_xml, "quota_tablespace",
                     req->quota_tablespace, sizeof(req->quota_tablespace));
    uppercase_inplace(req->quota_tablespace);

    /* quota given but no explicit tablespace -> default to
     * default_tablespace, matching the header's documented behaviour */
    if (strlen(req->quota) > 0 && strlen(req->quota_tablespace) == 0)
        strncpy(req->quota_tablespace, req->default_tablespace,
                sizeof(req->quota_tablespace) - 1);

    extract_xml_tag(input_xml, "profile", req->profile, sizeof(req->profile));
    uppercase_inplace(req->profile);

    extract_roles(input_xml, req);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "parse_create_user_request OK: username='%s' "
                 "default_tablespace='%s' temp_tablespace='%s' "
                 "quota='%s' quota_tablespace='%s' profile='%s' "
                 "role_count=%d",
                 req->username, req->default_tablespace,
                 req->temp_tablespace, req->quota, req->quota_tablespace,
                 req->profile, req->role_count);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Validation helpers                                                  */
/* ------------------------------------------------------------------ */

/* Legal Oracle unquoted identifier: starts with a letter, then
 * letters/digits/_/$/# only, max 128 bytes (DDL_IDENTIFIER_LEN). */
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

/* Plain-text password: reject characters that would break out of the
 * generated DDL string (quote, semicolon, backslash) or contain
 * whitespace/control characters. Not a password-strength policy. */
static int is_safe_password(const char *s)
{
    if (!s || strlen(s) == 0) return 0;
    for (const char *p = s; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c == '\'' || c == '"' || c == ';' || c == '\\' ||
            iscntrl(c) || isspace(c))
            return 0;
    }
    return 1;
}

/* "UNLIMITED" or <digits>[K|M|G], e.g. "500M", "10G", "2048K" */
static int is_valid_quota(const char *s)
{
    if (!s || strlen(s) == 0) return 0;
    if (strcmp(s, "UNLIMITED") == 0) return 1;

    const char *p = s;
    int digit_count = 0;
    while (isdigit((unsigned char)*p)) { p++; digit_count++; }
    if (digit_count == 0) return 0;

    if (*p != 'K' && *p != 'M' && *p != 'G') return 0;
    p++;
    return (*p == '\0');
}

/* ==================================================================
 *  validate_create_user_request  (Validation)
 * ================================================================== */
int validate_create_user_request(oci_context_t                *ctx,
                                  const create_user_request_t  *req,
                                  char                          *error_buf,
                                  size_t                         error_buf_size)
{
    if (!ctx || !req || !error_buf || error_buf_size == 0)
        return -1;

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering validate_create_user_request username='%s'",
                 req->username);

    if (!is_valid_identifier(req->username))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid username '%s': must start with a letter and "
                 "contain only letters, digits, '_', '$' or '#'",
                 req->username);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (!is_safe_password(req->identified_by))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid password for user '%s': must be non-empty and "
                 "must not contain quotes, ';', '\\', or whitespace",
                 req->username);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strlen(req->default_tablespace) > 0 &&
        !is_valid_identifier(req->default_tablespace))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid default_tablespace '%s' for user '%s'",
                 req->default_tablespace, req->username);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strlen(req->temp_tablespace) > 0 &&
        !is_valid_identifier(req->temp_tablespace))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid temp_tablespace '%s' for user '%s'",
                 req->temp_tablespace, req->username);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (strlen(req->quota) > 0)
    {
        if (!is_valid_quota(req->quota))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid quota '%s' for user '%s': expected "
                     "UNLIMITED or <digits>[K|M|G]",
                     req->quota, req->username);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }

        if (strlen(req->quota_tablespace) == 0)
        {
            snprintf(error_buf, error_buf_size,
                     "quota '%s' given for user '%s' but no "
                     "quota_tablespace (and no default_tablespace to "
                     "fall back to)",
                     req->quota, req->username);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }

        if (!is_valid_identifier(req->quota_tablespace))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid quota_tablespace '%s' for user '%s'",
                     req->quota_tablespace, req->username);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }
    }

    if (strlen(req->profile) > 0 && !is_valid_identifier(req->profile))
    {
        snprintf(error_buf, error_buf_size,
                 "Invalid profile '%s' for user '%s'",
                 req->profile, req->username);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (req->role_count > MAX_CREATE_USER_ROLES)
    {
        snprintf(error_buf, error_buf_size,
                 "Too many roles for user '%s': %d given, max %d",
                 req->username, req->role_count, MAX_CREATE_USER_ROLES);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    for (int i = 0; i < req->role_count; i++)
    {
        if (!is_valid_identifier(req->roles[i]))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid role '%s' (position %d) for user '%s'",
                     req->roles[i], i + 1, req->username);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "validate_create_user_request OK: username='%s'",
                 req->username);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  build_create_user_ddl_text()                                        */
/*  Build the literal CREATE USER (+ GRANT) DDL text. Public - see       */
/*  header doc comment for why (dispatcher.c's JSON response path).      */
/* ------------------------------------------------------------------ */
void build_create_user_ddl_text(const create_user_request_t *req,
                                 char *out, size_t out_size)
{
    size_t used = 0;
    used += (size_t)snprintf(out + used, out_size - used,
                              "CREATE USER %s IDENTIFIED BY %s",
                              req->username, req->identified_by);

    if (strlen(req->default_tablespace) > 0 && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  "\n  DEFAULT TABLESPACE %s",
                                  req->default_tablespace);

    if (strlen(req->temp_tablespace) > 0 && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  "\n  TEMPORARY TABLESPACE %s",
                                  req->temp_tablespace);

    if (strlen(req->quota) > 0 && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  "\n  QUOTA %s ON %s",
                                  req->quota, req->quota_tablespace);

    if (strlen(req->profile) > 0 && used < out_size)
        used += (size_t)snprintf(out + used, out_size - used,
                                  "\n  PROFILE %s",
                                  req->profile);

    if (used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, ";");

    if (req->role_count > 0 && used < out_size)
    {
        used += (size_t)snprintf(out + used, out_size - used,
                                  "\nGRANT ");
        for (int i = 0; i < req->role_count && used < out_size; i++)
        {
            used += (size_t)snprintf(out + used, out_size - used,
                                      "%s%s", req->roles[i],
                                      (i < req->role_count - 1) ? ", " : "");
        }
        if (used < out_size)
            used += (size_t)snprintf(out + used, out_size - used,
                                      " TO %s;", req->username);
    }
}

/* ==================================================================
 *  get_create_user_template  (tgen)
 * ================================================================== */
xml_builder_t *get_create_user_template(oci_context_t                *ctx,
                                         const create_user_request_t *req)
{
    if (!ctx || !req)
    {
        if (ctx)
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or req is NULL");
        return NULL;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering get_create_user_template username='%s'",
                 req->username);

    xml_builder_t *xml = xml_create(XML_INITIAL_SIZE);
    if (!xml)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "xml_create failed");
        return NULL;
    }

    char ddl_text[2048] = {0};
    build_create_user_ddl_text(req, ddl_text, sizeof(ddl_text));

    char *e_username  = xml_escape(req->username);
    char *e_def_ts    = xml_escape(req->default_tablespace);
    char *e_temp_ts   = xml_escape(req->temp_tablespace);
    char *e_quota     = xml_escape(req->quota);
    char *e_quota_ts  = xml_escape(req->quota_tablespace);
    char *e_profile   = xml_escape(req->profile);
    char *e_ddl       = xml_escape(ddl_text);

    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<Create_User_Template>\n");
    xml_append(xml, "  <operation>CREATE_USER</operation>\n");
    xml_append(xml, "  <username>%s</username>\n",           e_username);
    xml_append(xml, "  <default_tablespace>%s</default_tablespace>\n", e_def_ts);
    xml_append(xml, "  <temp_tablespace>%s</temp_tablespace>\n",       e_temp_ts);
    xml_append(xml, "  <quota>%s</quota>\n",                 e_quota);
    xml_append(xml, "  <quota_tablespace>%s</quota_tablespace>\n",     e_quota_ts);
    xml_append(xml, "  <profile>%s</profile>\n",             e_profile);

    xml_append(xml, "  <roles count=\"%d\">\n", req->role_count);
    for (int i = 0; i < req->role_count; i++)
    {
        char *e_role = xml_escape(req->roles[i]);
        xml_append(xml, "    <role>%s</role>\n", e_role);
        free(e_role);
    }
    xml_append(xml, "  </roles>\n");

    xml_append(xml, "  <generated_ddl>%s</generated_ddl>\n", e_ddl);
    xml_append(xml, "</Create_User_Template>\n");

    free(e_username);
    free(e_def_ts);
    free(e_temp_ts);
    free(e_quota);
    free(e_quota_ts);
    free(e_profile);
    free(e_ddl);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "get_create_user_template OK: username='%s' "
                 "role_count=%d ddl_len=%zu",
                 req->username, req->role_count, strlen(ddl_text));

    return xml;
}
