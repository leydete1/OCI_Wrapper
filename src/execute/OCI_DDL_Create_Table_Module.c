/*
 * OCI_DDL_Create_Table_Module.c
 *
 * Independent DDL Module - Create Table (third operation)
 * -------------------------------------------------------
 * See OCI_DDL_Create_Table_Module.h for the full design note. Same
 * conventions as OCI_DDL_Create_User_Module.c / OCI_DDL_Grant_Module.c:
 * tag-extraction XML parsing, fail-fast validation with a single
 * error_buf message, xml_builder_t for output, logging via
 * ctx->ddl_logger.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "OCI_DDL_Create_Table_Module.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define XML_INITIAL_SIZE   8192
#define MAX_COLUMN_LENGTH  32767   /* VARCHAR2 max in-row/CLOB-adjacent  *
                                     * limit - matches Oracle's own       *
                                     * ceiling for this data type          */
#define MAX_NUMBER_PRECISION 38    /* Oracle NUMBER precision ceiling    */

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as the other two DDL modules            */
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
 * Parse every <column>...</column> block strictly between the outer
 * <columns>...</columns> block. Each block is parsed independently
 * via extract_xml_tag() against just that slice, same nested-block
 * approach extract_roles()/extract_privileges() use in the sibling
 * DDL modules.
 */
static void extract_columns(const char *src, create_table_request_t *req)
{
    req->column_count = 0;

    const char *columns_start = strstr(src, "<columns>");
    if (!columns_start) return;
    const char *columns_end = strstr(columns_start, "</columns>");
    if (!columns_end) return;

    const char *cursor = columns_start;
    while (req->column_count < DDL_MAX_TABLE_COLUMNS)
    {
        const char *col_start = strstr(cursor, "<column>");
        if (!col_start || col_start >= columns_end) break;
        col_start += strlen("<column>");

        const char *col_end = strstr(col_start, "</column>");
        if (!col_end || col_end > columns_end) break;

        size_t block_len = (size_t)(col_end - col_start);
        char *block = malloc(block_len + 1);
        if (!block) break;
        memcpy(block, col_start, block_len);
        block[block_len] = '\0';

        column_def_t *col = &req->columns[req->column_count];
        memset(col, 0, sizeof(*col));
        col->nullable = 1;   /* default: NULL allowed unless overridden */

        extract_xml_tag(block, "name", col->name, sizeof(col->name));
        uppercase_inplace(col->name);

        extract_xml_tag(block, "data_type", col->data_type, sizeof(col->data_type));
        uppercase_inplace(col->data_type);

        char num_buf[16] = {0};
        if (extract_xml_tag(block, "length", num_buf, sizeof(num_buf)))
            col->length = atoi(num_buf);

        num_buf[0] = '\0';
        if (extract_xml_tag(block, "precision", num_buf, sizeof(num_buf)))
            col->precision = atoi(num_buf);

        num_buf[0] = '\0';
        if (extract_xml_tag(block, "scale", num_buf, sizeof(num_buf)))
            col->scale = atoi(num_buf);

        num_buf[0] = '\0';
        if (extract_xml_tag(block, "nullable", num_buf, sizeof(num_buf)))
            col->nullable = (atoi(num_buf) != 0);

        extract_xml_tag(block, "default_value", col->default_value,
                         sizeof(col->default_value));

        free(block);

        if (strlen(col->name) > 0)
            req->column_count++;

        cursor = col_end + strlen("</column>");
    }
}

/* Extract every <column>...</column> found strictly between the
 * <primary_key>...</primary_key> block (if present). */
static void extract_primary_key(const char *src, create_table_request_t *req)
{
    req->primary_key_count = 0;

    const char *pk_start = strstr(src, "<primary_key>");
    if (!pk_start) return;
    const char *pk_end = strstr(pk_start, "</primary_key>");
    if (!pk_end) return;

    const char *cursor = pk_start;
    while (req->primary_key_count < MAX_PRIMARY_KEY_COLUMNS)
    {
        const char *tag_start = strstr(cursor, "<column>");
        if (!tag_start || tag_start >= pk_end) break;
        tag_start += strlen("<column>");

        const char *tag_end = strstr(tag_start, "</column>");
        if (!tag_end || tag_end > pk_end) break;

        size_t len = (size_t)(tag_end - tag_start);
        char *dest = req->primary_key_columns[req->primary_key_count];
        if (len >= TABLE_IDENTIFIER_LEN) len = TABLE_IDENTIFIER_LEN - 1;
        memcpy(dest, tag_start, len);
        dest[len] = '\0';
        trim_inplace(dest);
        uppercase_inplace(dest);

        if (strlen(dest) > 0)
            req->primary_key_count++;

        cursor = tag_end + strlen("</column>");
    }
}

/* ==================================================================
 *  parse_create_table_request  (Definition)
 * ================================================================== */
int parse_create_table_request(oci_context_t            *ctx,
                                const char               *input_xml,
                                create_table_request_t   *req)
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
                 "Entering parse_create_table_request");

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

    /* ---- Mandatory (at least one): columns ---- */
    extract_columns(input_xml, req);
    if (req->column_count == 0)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "No <column> entries found for table_name='%s'",
                     req->table_name);
        return -1;
    }

    /* ---- Optional: primary_key ---- */
    extract_primary_key(input_xml, req);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "parse_create_table_request OK: table_name='%s' owner='%s' "
                 "column_count=%d primary_key_count=%d",
                 req->table_name, req->owner, req->column_count,
                 req->primary_key_count);

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

static int is_recognised_data_type(const char *t)
{
    static const char *valid[] = {
        "VARCHAR2", "CHAR", "NUMBER", "DATE", "TIMESTAMP", "CLOB", "BLOB",
        NULL
    };
    for (int i = 0; valid[i]; i++)
        if (strcmp(t, valid[i]) == 0) return 1;
    return 0;
}

static int type_requires_length(const char *t)
{
    return (strcmp(t, "VARCHAR2") == 0 || strcmp(t, "CHAR") == 0);
}

/* ==================================================================
 *  validate_create_table_request  (Validation)
 * ================================================================== */
int validate_create_table_request(oci_context_t                  *ctx,
                                   const create_table_request_t   *req,
                                   char                            *error_buf,
                                   size_t                           error_buf_size)
{
    if (!ctx || !req || !error_buf || error_buf_size == 0)
        return -1;

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering validate_create_table_request table_name='%s'",
                 req->table_name);

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

    if (req->column_count == 0)
    {
        snprintf(error_buf, error_buf_size,
                 "No columns given for table '%s' - at least one <column> "
                 "is required",
                 req->table_name);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    if (req->column_count > DDL_MAX_TABLE_COLUMNS)
    {
        snprintf(error_buf, error_buf_size,
                 "Too many columns for table '%s': %d given, max %d",
                 req->table_name, req->column_count, DDL_MAX_TABLE_COLUMNS);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    for (int i = 0; i < req->column_count; i++)
    {
        const column_def_t *col = &req->columns[i];

        if (!is_valid_identifier(col->name))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid column name '%s' (position %d) for table '%s'",
                     col->name, i + 1, req->table_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }

        for (int j = 0; j < i; j++)
        {
            if (strcmp(req->columns[j].name, col->name) == 0)
            {
                snprintf(error_buf, error_buf_size,
                         "Duplicate column name '%s' (positions %d and %d) "
                         "for table '%s'",
                         col->name, j + 1, i + 1, req->table_name);
                logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
                return -1;
            }
        }

        if (!is_recognised_data_type(col->data_type))
        {
            snprintf(error_buf, error_buf_size,
                     "Invalid data_type '%s' for column '%s' in table '%s': "
                     "expected one of VARCHAR2, CHAR, NUMBER, DATE, "
                     "TIMESTAMP, CLOB, BLOB",
                     col->data_type, col->name, req->table_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }

        if (type_requires_length(col->data_type))
        {
            if (col->length <= 0)
            {
                snprintf(error_buf, error_buf_size,
                         "Column '%s' (%s) in table '%s' requires a "
                         "positive <length>",
                         col->name, col->data_type, req->table_name);
                logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
                return -1;
            }
            if (col->length > MAX_COLUMN_LENGTH)
            {
                snprintf(error_buf, error_buf_size,
                         "Column '%s' (%s) in table '%s' has length %d, "
                         "exceeding the %d limit",
                         col->name, col->data_type, req->table_name,
                         col->length, MAX_COLUMN_LENGTH);
                logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
                return -1;
            }
        }

        if (strcmp(col->data_type, "NUMBER") == 0 && col->precision > 0)
        {
            if (col->precision > MAX_NUMBER_PRECISION)
            {
                snprintf(error_buf, error_buf_size,
                         "Column '%s' (NUMBER) in table '%s' has precision "
                         "%d, exceeding the %d limit",
                         col->name, req->table_name, col->precision,
                         MAX_NUMBER_PRECISION);
                logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
                return -1;
            }
            if (col->scale < 0 || col->scale > col->precision)
            {
                snprintf(error_buf, error_buf_size,
                         "Column '%s' (NUMBER) in table '%s' has scale %d "
                         "incompatible with precision %d",
                         col->name, req->table_name, col->scale, col->precision);
                logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
                return -1;
            }
        }
    }

    if (req->primary_key_count > MAX_PRIMARY_KEY_COLUMNS)
    {
        snprintf(error_buf, error_buf_size,
                 "Too many primary key columns for table '%s': %d given, "
                 "max %d",
                 req->table_name, req->primary_key_count, MAX_PRIMARY_KEY_COLUMNS);
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
        return -1;
    }

    for (int i = 0; i < req->primary_key_count; i++)
    {
        int found = 0;
        for (int j = 0; j < req->column_count; j++)
        {
            if (strcmp(req->primary_key_columns[i], req->columns[j].name) == 0)
            {
                found = 1;
                break;
            }
        }
        if (!found)
        {
            snprintf(error_buf, error_buf_size,
                     "Primary key references unknown column '%s' in "
                     "table '%s'",
                     req->primary_key_columns[i], req->table_name);
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0, "%s", error_buf);
            return -1;
        }
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "validate_create_table_request OK: table_name='%s' "
                 "column_count=%d primary_key_count=%d",
                 req->table_name, req->column_count, req->primary_key_count);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  build_create_table_ddl_text()                                       */
/* ------------------------------------------------------------------ */
void build_create_table_ddl_text(const create_table_request_t *req,
                                  char *out, size_t out_size)
{
    size_t used = 0;

    if (strlen(req->owner) > 0)
        used += (size_t)snprintf(out + used, out_size - used,
                                  "CREATE TABLE %s.%s (\n",
                                  req->owner, req->table_name);
    else
        used += (size_t)snprintf(out + used, out_size - used,
                                  "CREATE TABLE %s (\n", req->table_name);

    for (int i = 0; i < req->column_count && used < out_size; i++)
    {
        const column_def_t *col = &req->columns[i];

        used += (size_t)snprintf(out + used, out_size - used,
                                  "  %s %s", col->name, col->data_type);

        if (type_requires_length(col->data_type) && col->length > 0 && used < out_size)
            used += (size_t)snprintf(out + used, out_size - used,
                                      "(%d)", col->length);
        else if (strcmp(col->data_type, "NUMBER") == 0 && col->precision > 0 && used < out_size)
        {
            if (col->scale > 0)
                used += (size_t)snprintf(out + used, out_size - used,
                                          "(%d,%d)", col->precision, col->scale);
            else
                used += (size_t)snprintf(out + used, out_size - used,
                                          "(%d)", col->precision);
        }

        if (strlen(col->default_value) > 0 && used < out_size)
            used += (size_t)snprintf(out + used, out_size - used,
                                      " DEFAULT %s", col->default_value);

        if (!col->nullable && used < out_size)
            used += (size_t)snprintf(out + used, out_size - used, " NOT NULL");

        if (used < out_size)
            used += (size_t)snprintf(out + used, out_size - used,
                                      "%s\n",
                                      (i < req->column_count - 1 || req->primary_key_count > 0) ? "," : "");
    }

    if (req->primary_key_count > 0 && used < out_size)
    {
        used += (size_t)snprintf(out + used, out_size - used,
                                  "  CONSTRAINT %s_PK PRIMARY KEY (",
                                  req->table_name);
        for (int i = 0; i < req->primary_key_count && used < out_size; i++)
        {
            used += (size_t)snprintf(out + used, out_size - used,
                                      "%s%s", req->primary_key_columns[i],
                                      (i < req->primary_key_count - 1) ? ", " : "");
        }
        if (used < out_size)
            used += (size_t)snprintf(out + used, out_size - used, ")\n");
    }

    if (used < out_size)
        used += (size_t)snprintf(out + used, out_size - used, ");");
}

/* ==================================================================
 *  get_create_table_template  (tgen)
 * ================================================================== */
xml_builder_t *get_create_table_template(oci_context_t                  *ctx,
                                          const create_table_request_t  *req)
{
    if (!ctx || !req)
    {
        if (ctx)
            logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or req is NULL");
        return NULL;
    }

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "Entering get_create_table_template table_name='%s'",
                 req->table_name);

    xml_builder_t *xml = xml_create(XML_INITIAL_SIZE);
    if (!xml)
    {
        logger_write(ctx->ddl_logger, LOG_ERROR, __func__, 0,
                     "xml_create failed");
        return NULL;
    }

    char ddl_text[8192] = {0};
    build_create_table_ddl_text(req, ddl_text, sizeof(ddl_text));

    char *e_name  = xml_escape(req->table_name);
    char *e_owner = xml_escape(req->owner);
    char *e_ddl   = xml_escape(ddl_text);

    xml_append(xml, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_append(xml, "<Create_Table_Template>\n");
    xml_append(xml, "  <operation>CREATE_TABLE</operation>\n");
    xml_append(xml, "  <table_name>%s</table_name>\n", e_name);
    xml_append(xml, "  <owner>%s</owner>\n",           e_owner);

    xml_append(xml, "  <columns count=\"%d\">\n", req->column_count);
    for (int i = 0; i < req->column_count; i++)
    {
        const column_def_t *col = &req->columns[i];
        char *e_col_name = xml_escape(col->name);
        char *e_col_type = xml_escape(col->data_type);
        char *e_col_default = xml_escape(col->default_value);

        xml_append(xml, "    <column>\n");
        xml_append(xml, "      <name>%s</name>\n", e_col_name);
        xml_append(xml, "      <data_type>%s</data_type>\n", e_col_type);
        if (col->length > 0)
            xml_append(xml, "      <length>%d</length>\n", col->length);
        if (col->precision > 0)
            xml_append(xml, "      <precision>%d</precision>\n", col->precision);
        if (col->scale > 0)
            xml_append(xml, "      <scale>%d</scale>\n", col->scale);
        xml_append(xml, "      <nullable>%d</nullable>\n", col->nullable);
        if (strlen(col->default_value) > 0)
            xml_append(xml, "      <default_value>%s</default_value>\n", e_col_default);
        xml_append(xml, "    </column>\n");

        free(e_col_name);
        free(e_col_type);
        free(e_col_default);
    }
    xml_append(xml, "  </columns>\n");

    xml_append(xml, "  <primary_key count=\"%d\">\n", req->primary_key_count);
    for (int i = 0; i < req->primary_key_count; i++)
    {
        char *e_pk = xml_escape(req->primary_key_columns[i]);
        xml_append(xml, "    <column>%s</column>\n", e_pk);
        free(e_pk);
    }
    xml_append(xml, "  </primary_key>\n");

    xml_append(xml, "  <generated_ddl>%s</generated_ddl>\n", e_ddl);
    xml_append(xml, "</Create_Table_Template>\n");

    free(e_name);
    free(e_owner);
    free(e_ddl);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "get_create_table_template OK: table_name='%s' "
                 "column_count=%d ddl_len=%zu",
                 req->table_name, req->column_count, strlen(ddl_text));

    return xml;
}
