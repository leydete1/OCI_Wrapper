/*
 * OCI_DDL_Modules.c
 *
 * Merge of the six Independent DDL Modules (follow-up proposal,
 * 2026-09-20, item 2 continuation - connection consolidation and this
 * merge were the two items flagged alongside the three
 * standalone-harness passes). Each of the six was pure request-
 * parsing/validation/tgen logic with zero OCI calls (confirmed
 * during the original proposal survey - see the DDL row of that
 * survey), so unlike Table_Metadata/Transaction_Manager/Level2_Parser
 * this merge needed no standalone-harness pass of its own: nothing
 * here touches a live connection, so nothing here could exhibit the
 * class of bug that harness discipline exists to catch.
 *
 * All content below is verbatim from the six original files - no
 * logic was changed, only reorganised - with one exception:
 * trim_inplace(), uppercase_inplace(), extract_xml_tag() and
 * is_valid_identifier() were byte-for-byte identical (confirmed by
 * md5sum) copy-pasted into all six original files independently.
 * They appear exactly ONCE below, immediately after the includes,
 * shared by every module's section the same way they always
 * functionally were, just without six duplicate copies of the same
 * four function bodies. Everything else - every module-specific
 * static helper, every one of the four public functions per module
 * (parse_*, validate_*, get_*_template, build_*_ddl_text) - is kept
 * under its own module's original file, in its original file's
 * original order, exactly as it read before.
 *
 * Source files superseded by this merge (safe to remove from the
 * build once this file and OCI_DDL_Modules.h are wired in and a
 * full rebuild + regression pass - same signature-comparison method
 * used for the connection consolidation - comes back clean):
 *   OCI_DDL_Create_Table_Module.c/.h
 *   OCI_DDL_Create_View_Module.c/.h
 *   OCI_DDL_Create_User_Module.c/.h
 *   OCI_DDL_Create_Procedure_Module.c/.h
 *   OCI_DDL_Drop_Table_Module.c/.h
 *   OCI_DDL_Grant_Module.c/.h
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>    /* size_t - added defensively after a build-config
                          issue surfaced this needing to resolve before
                          anything else in the chain (Oracle SDK headers,
                          pthread.h) got a chance to pull it in themselves;
                          see the build troubleshooting note dated
                          2026-09-22 if this is still here and the real
                          cause turned out to be something else.          */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>   /* strncasecmp - only the former Create View
                         section below actually uses this           */

#include "DDL_Modules.h"
#include "XML_Helper.h"
#include "logger.h"

/* ==================================================================== */
/*  SHARED HELPERS                                                       */
/*  Byte-for-byte identical (md5sum-verified) across all six original   */
/*  modules before this merge - de-duplicated to one copy here.         */
/* ==================================================================== */

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

/* ==================================================================== */
/*  CREATE TABLE                                                              */
/*  (originally OCI_DDL_Create_Table_Module.c)                                            */
/* ==================================================================== */

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


/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define CREATE_TABLE_XML_INITIAL_SIZE   8192
#define MAX_COLUMN_LENGTH  32767   /* VARCHAR2 max in-row/CLOB-adjacent  *
                                     * limit - matches Oracle's own       *
                                     * ceiling for this data type          */
#define MAX_NUMBER_PRECISION 38    /* Oracle NUMBER precision ceiling    */

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as the other two DDL modules            */
/* ------------------------------------------------------------------ */



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

    xml_builder_t *xml = xml_create(CREATE_TABLE_XML_INITIAL_SIZE);
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


/* ==================================================================== */
/*  CREATE VIEW                                                              */
/*  (originally OCI_DDL_Create_View_Module.c)                                            */
/* ==================================================================== */

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


/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define CREATE_VIEW_XML_INITIAL_SIZE   8192

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as the other DDL modules               */
/* ------------------------------------------------------------------ */



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

    xml_builder_t *xml = xml_create(CREATE_VIEW_XML_INITIAL_SIZE);
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


/* ==================================================================== */
/*  CREATE USER                                                              */
/*  (originally OCI_DDL_Create_User_Module.c)                                            */
/* ==================================================================== */

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


/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define CREATE_USER_XML_INITIAL_SIZE   4096

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as OCI_Insert_Template_Module.c        */
/* ------------------------------------------------------------------ */


/* Extract text between <tag> and </tag>; returns 1 if found, 0 if not */

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

    xml_builder_t *xml = xml_create(CREATE_USER_XML_INITIAL_SIZE);
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


/* ==================================================================== */
/*  CREATE PROCEDURE                                                              */
/*  (originally OCI_DDL_Create_Procedure_Module.c)                                            */
/* ==================================================================== */

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


/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define CREATE_PROCEDURE_XML_INITIAL_SIZE   8192

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as the other DDL modules               */
/* ------------------------------------------------------------------ */



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

    xml_builder_t *xml = xml_create(CREATE_PROCEDURE_XML_INITIAL_SIZE);
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


/* ==================================================================== */
/*  DROP TABLE                                                              */
/*  (originally OCI_DDL_Drop_Table_Module.c)                                            */
/* ==================================================================== */

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


/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define DROP_TABLE_XML_INITIAL_SIZE   2048

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as the other DDL modules               */
/* ------------------------------------------------------------------ */



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

    /* ---- Optional at parse time, mandatory (must be 1) at execute
     * time - see header doc comment and dispatch_drop_table_new()
     * (dispatcher.c) ---- */
    buf[0] = '\0';
    if (extract_xml_tag(input_xml, "confirm", buf, sizeof(buf)))
        req->confirm = (atoi(buf) != 0);

    logger_write(ctx->ddl_logger, LOG_INFO, __func__, 0,
                 "parse_drop_table_request OK: table_name='%s' owner='%s' "
                 "cascade_constraints=%d purge=%d confirm=%d",
                 req->table_name, req->owner, req->cascade_constraints,
                 req->purge, req->confirm);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Validation helpers                                                  */
/* ------------------------------------------------------------------ */

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

    xml_builder_t *xml = xml_create(DROP_TABLE_XML_INITIAL_SIZE);
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


/* ==================================================================== */
/*  GRANT                                                              */
/*  (originally OCI_DDL_Grant_Module.c)                                            */
/* ==================================================================== */

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


/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
#define GRANT_XML_INITIAL_SIZE   4096

/* ------------------------------------------------------------------ */
/*  Static helpers - same style as OCI_DDL_Create_User_Module.c        */
/* ------------------------------------------------------------------ */



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

    xml_builder_t *xml = xml_create(GRANT_XML_INITIAL_SIZE);
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


