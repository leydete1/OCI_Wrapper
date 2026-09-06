/*
 * OCI_Level1_Parser.c
 *
 * See OCI_Level1_Parser.h for the full design description.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "OCI_Level1_Parser.h"
#include "OCI_Insert_Execute_Module.h"   /* insert_request_t, insert_row_t */
#include "OCI_Update_Execute_Module.h"   /* update_request_t - where_key_t/
                                            field_value_t come via
                                            OCI_Request_Response_Types.h,
                                            already pulled in above       */
#include "OCI_Delete_Execute_Module.h"   /* delete_request_t */
#include "OCI_Execute_Procedure_Module.h" /* execute_procedure_request_t,
                                              procedure_param_t,
                                              param_direction_t          */
#include "OCI_Auth_Manager.h"             /* authenticate_request_t - new,
                                            * Security Module Stage 2 */
#include "OCI_DDL_Create_User_Module.h"   /* create_user_request_t - new,
                                              Independent DDL Module
                                              proposal (03-Sep) */
#include "OCI_DDL_Grant_Module.h"         /* grant_request_t - new,
                                              Independent DDL Module
                                              proposal (03-Sep), second
                                              operation */
#include "OCI_DDL_Create_Table_Module.h"  /* create_table_request_t - new,
                                              Independent DDL Module
                                              proposal (03-Sep), third
                                              operation */
#include "OCI_DDL_Drop_Table_Module.h"    /* drop_table_request_t - new,
                                              Independent DDL Module
                                              proposal (03-Sep), fourth
                                              operation */
#include "OCI_DDL_Create_View_Module.h"   /* create_view_request_t - new,
                                              Independent DDL Module
                                              proposal (03-Sep), fifth
                                              operation */
#include "OCI_DDL_Create_Procedure_Module.h" /* create_procedure_request_t -
                                              new, Independent DDL Module
                                              proposal (03-Sep), sixth
                                              operation */
#include "OCI_Authz_Manager.h"            /* check_permission_request_t -
                                            * new, Security Module Stage 5 */
#include "logger.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include "cJSON.h"

/* ------------------------------------------------------------------ */
/*  level1_detect_format                                                */
/* ------------------------------------------------------------------ */
input_format_t level1_detect_format(const char *buf, size_t len)
{
    size_t i = 0;
    while (i < len && isspace((unsigned char)buf[i])) i++;
    if (i >= len) return INPUT_FORMAT_UNKNOWN;

    if (buf[i] == '<') return INPUT_FORMAT_XML;
    if (buf[i] == '{' || buf[i] == '[') return INPUT_FORMAT_JSON;
    return INPUT_FORMAT_UNKNOWN;
}

/* See this function's own doc comment in OCI_Level1_Parser.h - moved
 * here verbatim from Test_XML_Runner.c (2026-08-01), only renamed.    */
int level1_looks_like_new_format(const char *buf, size_t len)
{
    if (!buf) return 0;

    input_format_t fmt = level1_detect_format(buf, len);

    if (fmt == INPUT_FORMAT_JSON)
        return 1;   /* old-format files are never JSON - safe to assume new format */

    if (fmt == INPUT_FORMAT_XML)
    {
        const char *p = buf;

        for (;;)
        {
            while (*p && isspace((unsigned char)*p)) p++;

            if (strncmp(p, "<?xml", 5) == 0)
            {
                const char *decl_end = strstr(p, "?>");
                if (!decl_end) break;
                p = decl_end + 2;
                continue;
            }

            if (strncmp(p, "<!--", 4) == 0)
            {
                const char *comment_end = strstr(p, "-->");
                if (!comment_end) break;
                p = comment_end + 3;
                continue;
            }

            break;
        }

        return (strncmp(p, "<request", 8) == 0);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  set_error / set_ok - fill error_detail consistently                 */
/* ------------------------------------------------------------------ */
static void set_error(operation_status_t *error_detail, int code,
                       const char *err_code, const char *err_text)
{
    if (!error_detail) return;
    error_detail->status_code = code;
    strncpy(error_detail->error_code, err_code, sizeof(error_detail->error_code) - 1);
    error_detail->error_code[sizeof(error_detail->error_code) - 1] = '\0';
    strncpy(error_detail->error_text, err_text, sizeof(error_detail->error_text) - 1);
    error_detail->error_text[sizeof(error_detail->error_text) - 1] = '\0';
}

static void set_ok(operation_status_t *error_detail)
{
    if (!error_detail) return;
    error_detail->status_code = 0;
    strncpy(error_detail->error_code, "-", sizeof(error_detail->error_code) - 1);
    error_detail->error_code[sizeof(error_detail->error_code) - 1] = '\0';
    strncpy(error_detail->error_text, "-", sizeof(error_detail->error_text) - 1);
    error_detail->error_text[sizeof(error_detail->error_text) - 1] = '\0';
}

/* Map <operation type="..."> / "type":"..." string to operation_type_t */
static operation_type_t map_operation_type(const char *s)
{
    if (!s) return OP_UNKNOWN;
    if (strcasecmp(s, "GET_TEMPLATE") == 0)       return OP_GET_TEMPLATE;
    if (strcasecmp(s, "SELECT") == 0)             return OP_SELECT;
    if (strcasecmp(s, "INSERT") == 0)             return OP_INSERT;
    if (strcasecmp(s, "UPDATE") == 0)             return OP_UPDATE;
    if (strcasecmp(s, "DELETE") == 0)             return OP_DELETE;
    if (strcasecmp(s, "EXECUTE_PROCEDURE") == 0)  return OP_EXECUTE_PROCEDURE;
    if (strcasecmp(s, "CREATE_SESSION") == 0)     return OP_CREATE_SESSION;
    if (strcasecmp(s, "END_SESSION") == 0)        return OP_END_SESSION;
    if (strcasecmp(s, "AUTHENTICATE") == 0)       return OP_AUTHENTICATE;
    if (strcasecmp(s, "CHECK_PERMISSION") == 0)   return OP_CHECK_PERMISSION;
    if (strcasecmp(s, "CREATE_USER") == 0)        return OP_CREATE_USER;
    if (strcasecmp(s, "GRANT") == 0)              return OP_GRANT;
    if (strcasecmp(s, "CREATE_TABLE") == 0)       return OP_CREATE_TABLE;
    if (strcasecmp(s, "DROP_TABLE") == 0)         return OP_DROP_TABLE;
    if (strcasecmp(s, "CREATE_VIEW") == 0)        return OP_CREATE_VIEW;
    if (strcasecmp(s, "CREATE_PROCEDURE") == 0)   return OP_CREATE_PROCEDURE;
    return OP_UNKNOWN;
}

/*
 * trim_inplace()
 * XML element content (e.g. <sql>\n    SELECT ...\n</sql>) carries
 * the surrounding indentation/newlines as part of its text content -
 * xmlNodeGetContent() does not trim this. Strip it so select_request_t.
 * sql holds just the query, matching how the pre-refactor XML runner's
 * own extract_tag()+trim() always did the same thing.
 */
static void trim_inplace(char *s)
{
    if (!s) return;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) { s[len - 1] = '\0'; len--; }
}

/*
 * set_field_value()
 *
 * Sets fv->value (and fv->large_value if needed) from a NUL-terminated
 * source string of any length - see field_value_t's own doc comment in
 * OCI_Request_Response_Types.h for why this exists: CLOB values,
 * client-supplied or otherwise, routinely exceed value[]'s 4096 bytes.
 * src should already be trimmed by the caller if trimming applies
 * (XML text nodes; JSON string values need no such trimming).
 */
static void set_field_value(field_value_t *fv, const char *src)
{
    size_t len = strlen(src);

    if (len < sizeof(fv->value))
    {
        strncpy(fv->value, src, sizeof(fv->value) - 1);
        fv->value[sizeof(fv->value) - 1] = '\0';
        fv->large_value = NULL;
        return;
    }

    fv->large_value = malloc(len + 1);
    if (fv->large_value)
    {
        memcpy(fv->large_value, src, len + 1);
    }
    /* value[] holds a truncated preview either way - the real value
     * for anything downstream is field_value_get(fv), which falls
     * back to value[] if the malloc above failed rather than losing
     * the field entirely.                                              */
    strncpy(fv->value, src, sizeof(fv->value) - 1);
    fv->value[sizeof(fv->value) - 1] = '\0';
}

/*
 * parse_direction_l1()
 *
 * Maps a <param_direction>/"param_direction" string ("IN"/"OUT"/
 * "IN_OUT") to param_direction_t during Level 1 parsing - a local copy
 * of the same logic OCI_Execute_Procedure_Module.c's own
 * parse_direction() used to have before this refactor (see that file's
 * own removal note), matching the project convention of each parser/
 * builder module staying independent rather than sharing this kind of
 * small helper.
 */
static param_direction_t parse_direction_l1(const char *s)
{
    if (!s) return PARAM_DIR_IN;
    if (strcasecmp(s, "OUT")    == 0) return PARAM_DIR_OUT;
    if (strcasecmp(s, "IN_OUT") == 0) return PARAM_DIR_IN_OUT;
    return PARAM_DIR_IN;   /* default */
}

/*
 * build_payload_xml() / build_payload_json()
 *
 * Builds the concrete per-operation payload struct for operation types
 * Level 1 knows how to extract today. OP_SELECT and OP_INSERT are
 * implemented; everything else returns NULL (payload stays unset)
 * until Level 1 is extended for that operation type.
 *
 * max_rows/fetch_batch_size are left 0 deliberately - select_request_t's
 * own doc comment says 0 means "use ctx->ini->query_max_record_count /
 * query_fetch_batch_size", so Level 2 (or execute_query_batch() itself)
 * resolves the real default rather than Level 1 guessing at one.
 *
 * insert_request_t.row_count is intentionally NOT capped against
 * ctx->ini->max_bulk_inserts here - Level 1 only extracts what's on the
 * wire; the cap is Level 2's job to check and reject early, per
 * Data_Manager_Request_Definitions.docx.
 */
static void *build_payload_xml(xmlNodePtr op_node, operation_type_t type)
{
    switch (type)
    {
        case OP_SELECT:
        {
            select_request_t *req = calloc(1, sizeof(select_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(child->name, (const xmlChar *)"sql") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->sql, (const char *)content, sizeof(req->sql) - 1);
                        trim_inplace(req->sql);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"execute_async") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                        req->execute_async = (atoi((const char *)content) != 0);
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"async_call_back_url") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->async_call_back_url, (const char *)content,
                                sizeof(req->async_call_back_url) - 1);
                        trim_inplace(req->async_call_back_url);
                    }
                    xmlFree(content);
                }
            }

            req->max_rows              = 0;
            req->fetch_batch_size      = 0;
            req->include_column_names  = 1;
            return req;
        }
        case OP_INSERT:
        {
            insert_request_t *req = calloc(1, sizeof(insert_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"table_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->table_name, (const char *)content, sizeof(req->table_name) - 1);
                        trim_inplace(req->table_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
            }

            /* Count <row> elements first so we allocate exactly once -
             * same two-pass approach used for <transaction>'s <operation>
             * count above.                                                */
            int row_count = 0;
            for (xmlNodePtr child = op_node->children; child; child = child->next)
                if (child->type == XML_ELEMENT_NODE &&
                    xmlStrcmp(child->name, (const xmlChar *)"row") == 0)
                    row_count++;

            if (row_count > 0)
            {
                req->rows = calloc((size_t)row_count, sizeof(insert_row_t));
                if (!req->rows) { free(req); return NULL; }
            }
            req->row_count = row_count;

            int row_idx = 0;
            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE ||
                    xmlStrcmp(child->name, (const xmlChar *)"row") != 0)
                    continue;

                insert_row_t *row = &req->rows[row_idx++];

                /* row number="N" attribute is human-readable only - rows
                 * are processed in document order, matching insert_row_t's
                 * own doc comment.                                        */

                int field_count = 0;
                for (xmlNodePtr f = child->children; f; f = f->next)
                    if (f->type == XML_ELEMENT_NODE &&
                        xmlStrcmp(f->name, (const xmlChar *)"field") == 0)
                        field_count++;

                if (field_count > 0)
                {
                    row->fields = calloc((size_t)field_count, sizeof(field_value_t));
                    if (!row->fields) continue;   /* leaves this row empty; Level 2 will reject */
                }
                row->field_count = field_count;

                int field_idx = 0;
                for (xmlNodePtr f = child->children; f; f = f->next)
                {
                    if (f->type != XML_ELEMENT_NODE ||
                        xmlStrcmp(f->name, (const xmlChar *)"field") != 0)
                        continue;

                    field_value_t *fv = &row->fields[field_idx++];

                    for (xmlNodePtr fc = f->children; fc; fc = fc->next)
                    {
                        if (fc->type != XML_ELEMENT_NODE) continue;

                        if (xmlStrcmp(fc->name, (const xmlChar *)"field_name") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(fc);
                            if (content)
                            {
                                strncpy(fv->field_name, (const char *)content, sizeof(fv->field_name) - 1);
                                trim_inplace(fv->field_name);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(fc->name, (const xmlChar *)"value") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(fc);
                            if (content)
                            {
                                trim_inplace((char *)content);
                                set_field_value(fv, (const char *)content);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(fc->name, (const xmlChar *)"client_date_format") == 0)
                        {
                            /* Optional - see level2_validate_insert()'s
                             * own doc comment in OCI_Level2_Parser.h
                             * for the full 2026-07-27 date-handling
                             * design this is part of. Empty means
                             * "already in nls_date_format".            */
                            xmlChar *content = xmlNodeGetContent(fc);
                            if (content)
                            {
                                strncpy(fv->client_date_format, (const char *)content,
                                        sizeof(fv->client_date_format) - 1);
                                trim_inplace(fv->client_date_format);
                            }
                            xmlFree(content);
                        }
                    }
                }
            }

            return req;
        }
        case OP_UPDATE:
        {
            update_request_t *req = calloc(1, sizeof(update_request_t));
            if (!req) return NULL;

            xmlNodePtr where_node = NULL;
            xmlNodePtr set_node   = NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"table_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->table_name, (const char *)content, sizeof(req->table_name) - 1);
                        trim_inplace(req->table_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"where") == 0)
                    where_node = child;
                else if (xmlStrcmp(child->name, (const xmlChar *)"set") == 0)
                    set_node = child;
            }

            /* ---- <where><key>...</key></where> - AND'd together ---- */
            if (where_node)
            {
                int key_count = 0;
                for (xmlNodePtr k = where_node->children; k; k = k->next)
                    if (k->type == XML_ELEMENT_NODE &&
                        xmlStrcmp(k->name, (const xmlChar *)"key") == 0)
                        key_count++;

                if (key_count > 0)
                {
                    req->keys = calloc((size_t)key_count, sizeof(where_key_t));
                    if (!req->keys) { free(req); return NULL; }
                }
                req->key_count = key_count;

                int key_idx = 0;
                for (xmlNodePtr k = where_node->children; k; k = k->next)
                {
                    if (k->type != XML_ELEMENT_NODE ||
                        xmlStrcmp(k->name, (const xmlChar *)"key") != 0)
                        continue;

                    where_key_t *wk = &req->keys[key_idx++];

                    for (xmlNodePtr kc = k->children; kc; kc = kc->next)
                    {
                        if (kc->type != XML_ELEMENT_NODE) continue;

                        if (xmlStrcmp(kc->name, (const xmlChar *)"field_name") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(kc);
                            if (content)
                            {
                                strncpy(wk->field_name, (const char *)content, sizeof(wk->field_name) - 1);
                                trim_inplace(wk->field_name);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(kc->name, (const xmlChar *)"key_value") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(kc);
                            if (content)
                            {
                                strncpy(wk->key_value, (const char *)content, sizeof(wk->key_value) - 1);
                                trim_inplace(wk->key_value);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(kc->name, (const xmlChar *)"client_date_format") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(kc);
                            if (content)
                            {
                                strncpy(wk->client_date_format, (const char *)content,
                                        sizeof(wk->client_date_format) - 1);
                                trim_inplace(wk->client_date_format);
                            }
                            xmlFree(content);
                        }
                    }
                }
            }

            /* ---- <set><field>...</field></set> - no per-row concept,
             * a single flat SET list applied to however many rows the
             * WHERE clause matches. Same <field><field_name>/<value>
             * shape as INSERT's rows[].fields[], reusing field_value_t
             * directly - see update_request_t's own doc comment in
             * OCI_Update_Execute_Module.h.                              */
            if (set_node)
            {
                int field_count = 0;
                for (xmlNodePtr f = set_node->children; f; f = f->next)
                    if (f->type == XML_ELEMENT_NODE &&
                        xmlStrcmp(f->name, (const xmlChar *)"field") == 0)
                        field_count++;

                if (field_count > 0)
                {
                    req->fields = calloc((size_t)field_count, sizeof(field_value_t));
                    if (!req->fields) { free(req->keys); free(req); return NULL; }
                }
                req->field_count = field_count;

                int field_idx = 0;
                for (xmlNodePtr f = set_node->children; f; f = f->next)
                {
                    if (f->type != XML_ELEMENT_NODE ||
                        xmlStrcmp(f->name, (const xmlChar *)"field") != 0)
                        continue;

                    field_value_t *fv = &req->fields[field_idx++];

                    for (xmlNodePtr fc = f->children; fc; fc = fc->next)
                    {
                        if (fc->type != XML_ELEMENT_NODE) continue;

                        if (xmlStrcmp(fc->name, (const xmlChar *)"field_name") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(fc);
                            if (content)
                            {
                                strncpy(fv->field_name, (const char *)content, sizeof(fv->field_name) - 1);
                                trim_inplace(fv->field_name);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(fc->name, (const xmlChar *)"value") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(fc);
                            if (content)
                            {
                                trim_inplace((char *)content);
                                set_field_value(fv, (const char *)content);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(fc->name, (const xmlChar *)"client_date_format") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(fc);
                            if (content)
                            {
                                strncpy(fv->client_date_format, (const char *)content,
                                        sizeof(fv->client_date_format) - 1);
                                trim_inplace(fv->client_date_format);
                            }
                            xmlFree(content);
                        }
                    }
                }
            }

            return req;
        }
        case OP_DELETE:
        {
            delete_request_t *req = calloc(1, sizeof(delete_request_t));
            if (!req) return NULL;

            xmlNodePtr where_node = NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"table_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->table_name, (const char *)content, sizeof(req->table_name) - 1);
                        trim_inplace(req->table_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"where") == 0)
                    where_node = child;
                /* No <set> at all - DELETE has nothing else to carry,
                 * unlike UPDATE. See delete_request_t's own doc comment
                 * in OCI_Delete_Execute_Module.h.                       */
            }

            /* ---- <where><key>...</key></where> - AND'd together -
             * identical shape and parsing to UPDATE's own WHERE clause.  */
            if (where_node)
            {
                int key_count = 0;
                for (xmlNodePtr k = where_node->children; k; k = k->next)
                    if (k->type == XML_ELEMENT_NODE &&
                        xmlStrcmp(k->name, (const xmlChar *)"key") == 0)
                        key_count++;

                if (key_count > 0)
                {
                    req->keys = calloc((size_t)key_count, sizeof(where_key_t));
                    if (!req->keys) { free(req); return NULL; }
                }
                req->key_count = key_count;

                int key_idx = 0;
                for (xmlNodePtr k = where_node->children; k; k = k->next)
                {
                    if (k->type != XML_ELEMENT_NODE ||
                        xmlStrcmp(k->name, (const xmlChar *)"key") != 0)
                        continue;

                    where_key_t *wk = &req->keys[key_idx++];

                    for (xmlNodePtr kc = k->children; kc; kc = kc->next)
                    {
                        if (kc->type != XML_ELEMENT_NODE) continue;

                        if (xmlStrcmp(kc->name, (const xmlChar *)"field_name") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(kc);
                            if (content)
                            {
                                strncpy(wk->field_name, (const char *)content, sizeof(wk->field_name) - 1);
                                trim_inplace(wk->field_name);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(kc->name, (const xmlChar *)"key_value") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(kc);
                            if (content)
                            {
                                strncpy(wk->key_value, (const char *)content, sizeof(wk->key_value) - 1);
                                trim_inplace(wk->key_value);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(kc->name, (const xmlChar *)"client_date_format") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(kc);
                            if (content)
                            {
                                strncpy(wk->client_date_format, (const char *)content,
                                        sizeof(wk->client_date_format) - 1);
                                trim_inplace(wk->client_date_format);
                            }
                            xmlFree(content);
                        }
                    }
                }
            }

            return req;
        }
        case OP_EXECUTE_PROCEDURE:
        {
            execute_procedure_request_t *req =
                calloc(1, sizeof(execute_procedure_request_t));
            if (!req) return NULL;

            xmlNodePtr params_node = NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"procedure_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->procedure_name, (const char *)content,
                                sizeof(req->procedure_name) - 1);
                        trim_inplace(req->procedure_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"parameters") == 0)
                    params_node = child;
            }

            /* ---- <parameters><parameter>...</parameter></parameters> -
             * one entry per IN/OUT/IN_OUT scalar or CURSOR OUT param.    */
            if (params_node)
            {
                int param_count = 0;
                for (xmlNodePtr p = params_node->children; p; p = p->next)
                    if (p->type == XML_ELEMENT_NODE &&
                        xmlStrcmp(p->name, (const xmlChar *)"parameter") == 0)
                        param_count++;

                if (param_count > 0)
                {
                    req->parameters = calloc((size_t)param_count, sizeof(procedure_param_t));
                    if (!req->parameters) { free(req); return NULL; }
                }
                req->param_count = param_count;

                int param_idx = 0;
                for (xmlNodePtr p = params_node->children; p; p = p->next)
                {
                    if (p->type != XML_ELEMENT_NODE ||
                        xmlStrcmp(p->name, (const xmlChar *)"parameter") != 0)
                        continue;

                    procedure_param_t *pp = &req->parameters[param_idx++];

                    for (xmlNodePtr pc = p->children; pc; pc = pc->next)
                    {
                        if (pc->type != XML_ELEMENT_NODE) continue;

                        if (xmlStrcmp(pc->name, (const xmlChar *)"param_name") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(pc);
                            if (content)
                            {
                                strncpy(pp->param_name, (const char *)content,
                                        sizeof(pp->param_name) - 1);
                                trim_inplace(pp->param_name);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(pc->name, (const xmlChar *)"param_type") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(pc);
                            if (content)
                            {
                                strncpy(pp->param_type, (const char *)content,
                                        sizeof(pp->param_type) - 1);
                                trim_inplace(pp->param_type);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(pc->name, (const xmlChar *)"param_direction") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(pc);
                            if (content)
                            {
                                char dir_str[32] = {0};
                                strncpy(dir_str, (const char *)content, sizeof(dir_str) - 1);
                                trim_inplace(dir_str);
                                pp->direction = parse_direction_l1(dir_str);
                            }
                            xmlFree(content);
                        }
                        else if (xmlStrcmp(pc->name, (const xmlChar *)"param_value") == 0)
                        {
                            xmlChar *content = xmlNodeGetContent(pc);
                            if (content)
                            {
                                strncpy(pp->param_value, (const char *)content,
                                        sizeof(pp->param_value) - 1);
                                trim_inplace(pp->param_value);
                            }
                            xmlFree(content);
                        }
                    }
                }
            }

            return req;
        }
        case OP_AUTHENTICATE:
        {
            /* Security Module Stage 2 - deliberately minimal: two plain
             * string fields, no nested structure, matching the request
             * shape in Security_Module_Design_Specification.docx
             * Section 8.1. Empty/missing elements are left as empty
             * strings - Level 2's level2_validate_authenticate() is
             * where "username/credential required" is actually
             * enforced, same division of responsibility as every other
             * operation type here.                                    */
            authenticate_request_t *req = calloc(1, sizeof(authenticate_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"username") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->username, (const char *)content,
                                sizeof(req->username) - 1);
                        trim_inplace(req->username);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"credential") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->credential, (const char *)content,
                                sizeof(req->credential) - 1);
                        trim_inplace(req->credential);
                    }
                    xmlFree(content);
                }
            }

            return req;
        }
        case OP_CHECK_PERMISSION:
        {
            /* Security Module Stage 5 - a single plain string field.
             * session_id is deliberately NOT parsed here - it comes
             * from the envelope's own session_id (already parsed
             * elsewhere into input_c_request_t.session_id), matching
             * every other operation type's existing session handling
             * rather than duplicating it per-operation (see
             * OCI_Authz_Manager.h's own doc comment on check_
             * permission_request_t).                                  */
            check_permission_request_t *req =
                calloc(1, sizeof(check_permission_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"permission_code") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->permission_code, (const char *)content,
                                sizeof(req->permission_code) - 1);
                        trim_inplace(req->permission_code);
                    }
                    xmlFree(content);
                }
            }

            return req;
        }
        case OP_CREATE_USER:
        {
            create_user_request_t *req = calloc(1, sizeof(create_user_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"username") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->username, (const char *)content, sizeof(req->username) - 1);
                        trim_inplace(req->username);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"identified_by") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->identified_by, (const char *)content, sizeof(req->identified_by) - 1);
                        trim_inplace(req->identified_by);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"default_tablespace") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->default_tablespace, (const char *)content, sizeof(req->default_tablespace) - 1);
                        trim_inplace(req->default_tablespace);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"temp_tablespace") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->temp_tablespace, (const char *)content, sizeof(req->temp_tablespace) - 1);
                        trim_inplace(req->temp_tablespace);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"quota") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->quota, (const char *)content, sizeof(req->quota) - 1);
                        trim_inplace(req->quota);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"quota_tablespace") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->quota_tablespace, (const char *)content, sizeof(req->quota_tablespace) - 1);
                        trim_inplace(req->quota_tablespace);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"profile") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->profile, (const char *)content, sizeof(req->profile) - 1);
                        trim_inplace(req->profile);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"roles") == 0)
                {
                    for (xmlNodePtr role_node = child->children; role_node; role_node = role_node->next)
                    {
                        if (role_node->type != XML_ELEMENT_NODE) continue;
                        if (xmlStrcmp(role_node->name, (const xmlChar *)"role") != 0) continue;
                        if (req->role_count >= MAX_CREATE_USER_ROLES) break;

                        xmlChar *content = xmlNodeGetContent(role_node);
                        if (content)
                        {
                            char *dest = req->roles[req->role_count];
                            strncpy(dest, (const char *)content, DDL_IDENTIFIER_LEN - 1);
                            trim_inplace(dest);
                            if (strlen(dest) > 0)
                                req->role_count++;
                        }
                        xmlFree(content);
                    }
                }
            }

            /* quota given but no explicit tablespace -> default to
             * default_tablespace, same fallback
             * parse_create_user_request() applies when the module is
             * exercised standalone (see OCI_DDL_Create_User_Module.c). */
            if (strlen(req->quota) > 0 && strlen(req->quota_tablespace) == 0)
                strncpy(req->quota_tablespace, req->default_tablespace,
                        sizeof(req->quota_tablespace) - 1);

            return req;
        }
        case OP_GRANT:
        {
            grant_request_t *req = calloc(1, sizeof(grant_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"grantee") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->grantee, (const char *)content, sizeof(req->grantee) - 1);
                        trim_inplace(req->grantee);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"object_type") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->object_type, (const char *)content, sizeof(req->object_type) - 1);
                        trim_inplace(req->object_type);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"object_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->object_name, (const char *)content, sizeof(req->object_name) - 1);
                        trim_inplace(req->object_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"with_grant_option") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                        req->with_grant_option = (atoi((const char *)content) != 0);
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"privileges") == 0)
                {
                    for (xmlNodePtr priv_node = child->children; priv_node; priv_node = priv_node->next)
                    {
                        if (priv_node->type != XML_ELEMENT_NODE) continue;
                        if (xmlStrcmp(priv_node->name, (const xmlChar *)"privilege") != 0) continue;
                        if (req->privilege_count >= MAX_GRANT_PRIVILEGES) break;

                        xmlChar *content = xmlNodeGetContent(priv_node);
                        if (content)
                        {
                            char *dest = req->privileges[req->privilege_count];
                            strncpy(dest, (const char *)content, GRANT_PRIVILEGE_LEN - 1);
                            trim_inplace(dest);
                            if (strlen(dest) > 0)
                                req->privilege_count++;
                        }
                        xmlFree(content);
                    }
                }
            }

            return req;
        }
        case OP_CREATE_TABLE:
        {
            create_table_request_t *req = calloc(1, sizeof(create_table_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"table_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->table_name, (const char *)content, sizeof(req->table_name) - 1);
                        trim_inplace(req->table_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"columns") == 0)
                {
                    for (xmlNodePtr col_node = child->children; col_node; col_node = col_node->next)
                    {
                        if (col_node->type != XML_ELEMENT_NODE) continue;
                        if (xmlStrcmp(col_node->name, (const xmlChar *)"column") != 0) continue;
                        if (req->column_count >= DDL_MAX_TABLE_COLUMNS) break;

                        column_def_t *col = &req->columns[req->column_count];
                        memset(col, 0, sizeof(*col));
                        col->nullable = 1;   /* default: NULL allowed */

                        for (xmlNodePtr field = col_node->children; field; field = field->next)
                        {
                            if (field->type != XML_ELEMENT_NODE) continue;
                            xmlChar *fc = xmlNodeGetContent(field);
                            if (!fc) continue;

                            if (xmlStrcmp(field->name, (const xmlChar *)"name") == 0)
                            {
                                strncpy(col->name, (const char *)fc, sizeof(col->name) - 1);
                                trim_inplace(col->name);
                            }
                            else if (xmlStrcmp(field->name, (const xmlChar *)"data_type") == 0)
                            {
                                strncpy(col->data_type, (const char *)fc, sizeof(col->data_type) - 1);
                                trim_inplace(col->data_type);
                            }
                            else if (xmlStrcmp(field->name, (const xmlChar *)"length") == 0)
                                col->length = atoi((const char *)fc);
                            else if (xmlStrcmp(field->name, (const xmlChar *)"precision") == 0)
                                col->precision = atoi((const char *)fc);
                            else if (xmlStrcmp(field->name, (const xmlChar *)"scale") == 0)
                                col->scale = atoi((const char *)fc);
                            else if (xmlStrcmp(field->name, (const xmlChar *)"nullable") == 0)
                                col->nullable = (atoi((const char *)fc) != 0);
                            else if (xmlStrcmp(field->name, (const xmlChar *)"default_value") == 0)
                            {
                                strncpy(col->default_value, (const char *)fc, sizeof(col->default_value) - 1);
                                trim_inplace(col->default_value);
                            }

                            xmlFree(fc);
                        }

                        if (strlen(col->name) > 0)
                            req->column_count++;
                    }
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"primary_key") == 0)
                {
                    for (xmlNodePtr pk_node = child->children; pk_node; pk_node = pk_node->next)
                    {
                        if (pk_node->type != XML_ELEMENT_NODE) continue;
                        if (xmlStrcmp(pk_node->name, (const xmlChar *)"column") != 0) continue;
                        if (req->primary_key_count >= MAX_PRIMARY_KEY_COLUMNS) break;

                        xmlChar *content = xmlNodeGetContent(pk_node);
                        if (content)
                        {
                            char *dest = req->primary_key_columns[req->primary_key_count];
                            strncpy(dest, (const char *)content, TABLE_IDENTIFIER_LEN - 1);
                            trim_inplace(dest);
                            if (strlen(dest) > 0)
                                req->primary_key_count++;
                        }
                        xmlFree(content);
                    }
                }
            }

            return req;
        }
        case OP_DROP_TABLE:
        {
            drop_table_request_t *req = calloc(1, sizeof(drop_table_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"table_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->table_name, (const char *)content, sizeof(req->table_name) - 1);
                        trim_inplace(req->table_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"cascade_constraints") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                        req->cascade_constraints = (atoi((const char *)content) != 0);
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"purge") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                        req->purge = (atoi((const char *)content) != 0);
                    xmlFree(content);
                }
            }

            return req;
        }
        case OP_CREATE_VIEW:
        {
            create_view_request_t *req = calloc(1, sizeof(create_view_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"view_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->view_name, (const char *)content, sizeof(req->view_name) - 1);
                        trim_inplace(req->view_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"replace") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                        req->replace = (atoi((const char *)content) != 0);
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"force") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                        req->force = (atoi((const char *)content) != 0);
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"query") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->query, (const char *)content, sizeof(req->query) - 1);
                        trim_inplace(req->query);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"columns") == 0)
                {
                    for (xmlNodePtr col_node = child->children; col_node; col_node = col_node->next)
                    {
                        if (col_node->type != XML_ELEMENT_NODE) continue;
                        if (xmlStrcmp(col_node->name, (const xmlChar *)"column") != 0) continue;
                        if (req->column_count >= MAX_VIEW_COLUMNS) break;

                        xmlChar *content = xmlNodeGetContent(col_node);
                        if (content)
                        {
                            char *dest = req->columns[req->column_count];
                            strncpy(dest, (const char *)content, VIEW_IDENTIFIER_LEN - 1);
                            trim_inplace(dest);
                            if (strlen(dest) > 0)
                                req->column_count++;
                        }
                        xmlFree(content);
                    }
                }
            }

            return req;
        }
        case OP_CREATE_PROCEDURE:
        {
            create_procedure_request_t *req = calloc(1, sizeof(create_procedure_request_t));
            if (!req) return NULL;

            for (xmlNodePtr child = op_node->children; child; child = child->next)
            {
                if (child->type != XML_ELEMENT_NODE) continue;

                if (xmlStrcmp(child->name, (const xmlChar *)"procedure_name") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->procedure_name, (const char *)content, sizeof(req->procedure_name) - 1);
                        trim_inplace(req->procedure_name);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"owner") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->owner, (const char *)content, sizeof(req->owner) - 1);
                        trim_inplace(req->owner);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"replace") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                        req->replace = (atoi((const char *)content) != 0);
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"body") == 0)
                {
                    xmlChar *content = xmlNodeGetContent(child);
                    if (content)
                    {
                        strncpy(req->body, (const char *)content, sizeof(req->body) - 1);
                        trim_inplace(req->body);
                    }
                    xmlFree(content);
                }
                else if (xmlStrcmp(child->name, (const xmlChar *)"parameters") == 0)
                {
                    for (xmlNodePtr param_node = child->children; param_node; param_node = param_node->next)
                    {
                        if (param_node->type != XML_ELEMENT_NODE) continue;
                        if (xmlStrcmp(param_node->name, (const xmlChar *)"parameter") != 0) continue;
                        if (req->parameter_count >= MAX_PROCEDURE_PARAMETERS) break;

                        ddl_procedure_param_t *param = &req->parameters[req->parameter_count];
                        memset(param, 0, sizeof(*param));
                        strncpy(param->mode, "IN", sizeof(param->mode) - 1); /* default */

                        for (xmlNodePtr field = param_node->children; field; field = field->next)
                        {
                            if (field->type != XML_ELEMENT_NODE) continue;
                            xmlChar *fc = xmlNodeGetContent(field);
                            if (!fc) continue;

                            if (xmlStrcmp(field->name, (const xmlChar *)"name") == 0)
                            {
                                strncpy(param->name, (const char *)fc, sizeof(param->name) - 1);
                                trim_inplace(param->name);
                            }
                            else if (xmlStrcmp(field->name, (const xmlChar *)"data_type") == 0)
                            {
                                strncpy(param->data_type, (const char *)fc, sizeof(param->data_type) - 1);
                                trim_inplace(param->data_type);
                            }
                            else if (xmlStrcmp(field->name, (const xmlChar *)"mode") == 0)
                            {
                                strncpy(param->mode, (const char *)fc, sizeof(param->mode) - 1);
                                trim_inplace(param->mode);
                            }
                            else if (xmlStrcmp(field->name, (const xmlChar *)"default_value") == 0)
                            {
                                strncpy(param->default_value, (const char *)fc, sizeof(param->default_value) - 1);
                                trim_inplace(param->default_value);
                            }

                            xmlFree(fc);
                        }

                        if (strlen(param->name) > 0)
                            req->parameter_count++;
                    }
                }
            }

            return req;
        }
        default:
            return NULL;
    }
}

static void *build_payload_json(cJSON *op_json, operation_type_t type)
{
    switch (type)
    {
        case OP_SELECT:
        {
            select_request_t *req = calloc(1, sizeof(select_request_t));
            if (!req) return NULL;

            cJSON *sql = cJSON_GetObjectItemCaseSensitive(op_json, "sql");
            if (cJSON_IsString(sql) && sql->valuestring)
            {
                strncpy(req->sql, sql->valuestring, sizeof(req->sql) - 1);
                trim_inplace(req->sql);
            }

            cJSON *async = cJSON_GetObjectItemCaseSensitive(op_json, "execute_async");
            if (cJSON_IsNumber(async))
                req->execute_async = (async->valueint != 0);
            else if (cJSON_IsBool(async))
                req->execute_async = cJSON_IsTrue(async) ? 1 : 0;

            cJSON *cb_url = cJSON_GetObjectItemCaseSensitive(op_json, "async_call_back_url");
            if (cJSON_IsString(cb_url) && cb_url->valuestring)
            {
                strncpy(req->async_call_back_url, cb_url->valuestring,
                        sizeof(req->async_call_back_url) - 1);
                trim_inplace(req->async_call_back_url);
            }

            req->max_rows              = 0;
            req->fetch_batch_size      = 0;
            req->include_column_names  = 1;
            return req;
        }
        case OP_INSERT:
        {
            insert_request_t *req = calloc(1, sizeof(insert_request_t));
            if (!req) return NULL;

            cJSON *table_name = cJSON_GetObjectItemCaseSensitive(op_json, "table_name");
            if (cJSON_IsString(table_name) && table_name->valuestring)
                strncpy(req->table_name, table_name->valuestring, sizeof(req->table_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            cJSON *rows = cJSON_GetObjectItemCaseSensitive(op_json, "rows");
            int row_count = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;

            if (row_count > 0)
            {
                req->rows = calloc((size_t)row_count, sizeof(insert_row_t));
                if (!req->rows) { free(req); return NULL; }
            }
            req->row_count = row_count;

            for (int r = 0; r < row_count; r++)
            {
                cJSON *row_json = cJSON_GetArrayItem(rows, r);
                insert_row_t *row = &req->rows[r];

                /* row_number key (if present) is human-readable only,
                 * same as the XML "number" attribute - rows are processed
                 * in array order.                                        */

                cJSON *fields = cJSON_GetObjectItemCaseSensitive(row_json, "fields");
                int field_count = cJSON_IsArray(fields) ? cJSON_GetArraySize(fields) : 0;

                if (field_count > 0)
                {
                    row->fields = calloc((size_t)field_count, sizeof(field_value_t));
                    if (!row->fields) continue;   /* leaves this row empty; Level 2 will reject */
                }
                row->field_count = field_count;

                for (int f = 0; f < field_count; f++)
                {
                    cJSON *field_json = cJSON_GetArrayItem(fields, f);
                    field_value_t *fv = &row->fields[f];

                    cJSON *fname = cJSON_GetObjectItemCaseSensitive(field_json, "field_name");
                    if (cJSON_IsString(fname) && fname->valuestring)
                        strncpy(fv->field_name, fname->valuestring, sizeof(fv->field_name) - 1);

                    cJSON *fvalue = cJSON_GetObjectItemCaseSensitive(field_json, "value");
                    if (cJSON_IsString(fvalue) && fvalue->valuestring)
                        set_field_value(fv, fvalue->valuestring);

                    cJSON *fdatefmt = cJSON_GetObjectItemCaseSensitive(field_json, "client_date_format");
                    if (cJSON_IsString(fdatefmt) && fdatefmt->valuestring)
                        strncpy(fv->client_date_format, fdatefmt->valuestring,
                                sizeof(fv->client_date_format) - 1);
                }
            }

            return req;
        }
        case OP_UPDATE:
        {
            update_request_t *req = calloc(1, sizeof(update_request_t));
            if (!req) return NULL;

            cJSON *table_name = cJSON_GetObjectItemCaseSensitive(op_json, "table_name");
            if (cJSON_IsString(table_name) && table_name->valuestring)
                strncpy(req->table_name, table_name->valuestring, sizeof(req->table_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            /* ---- "where": [ {field_name, key_value}, ... ] ---- */
            cJSON *where = cJSON_GetObjectItemCaseSensitive(op_json, "where");
            int key_count = cJSON_IsArray(where) ? cJSON_GetArraySize(where) : 0;

            if (key_count > 0)
            {
                req->keys = calloc((size_t)key_count, sizeof(where_key_t));
                if (!req->keys) { free(req); return NULL; }
            }
            req->key_count = key_count;

            for (int k = 0; k < key_count; k++)
            {
                cJSON *key_json = cJSON_GetArrayItem(where, k);
                where_key_t *wk = &req->keys[k];

                cJSON *fname = cJSON_GetObjectItemCaseSensitive(key_json, "field_name");
                if (cJSON_IsString(fname) && fname->valuestring)
                    strncpy(wk->field_name, fname->valuestring, sizeof(wk->field_name) - 1);

                cJSON *kvalue = cJSON_GetObjectItemCaseSensitive(key_json, "key_value");
                if (cJSON_IsString(kvalue) && kvalue->valuestring)
                    strncpy(wk->key_value, kvalue->valuestring, sizeof(wk->key_value) - 1);

                cJSON *kdatefmt = cJSON_GetObjectItemCaseSensitive(key_json, "client_date_format");
                if (cJSON_IsString(kdatefmt) && kdatefmt->valuestring)
                    strncpy(wk->client_date_format, kdatefmt->valuestring,
                            sizeof(wk->client_date_format) - 1);
            }

            /* ---- "set": [ {field_name, value}, ... ] - no per-row
             * concept, same field_value_t shape as INSERT's fields[].  */
            cJSON *set = cJSON_GetObjectItemCaseSensitive(op_json, "set");
            int field_count = cJSON_IsArray(set) ? cJSON_GetArraySize(set) : 0;

            if (field_count > 0)
            {
                req->fields = calloc((size_t)field_count, sizeof(field_value_t));
                if (!req->fields) { free(req->keys); free(req); return NULL; }
            }
            req->field_count = field_count;

            for (int f = 0; f < field_count; f++)
            {
                cJSON *field_json = cJSON_GetArrayItem(set, f);
                field_value_t *fv = &req->fields[f];

                cJSON *fname = cJSON_GetObjectItemCaseSensitive(field_json, "field_name");
                if (cJSON_IsString(fname) && fname->valuestring)
                    strncpy(fv->field_name, fname->valuestring, sizeof(fv->field_name) - 1);

                cJSON *fvalue = cJSON_GetObjectItemCaseSensitive(field_json, "value");
                if (cJSON_IsString(fvalue) && fvalue->valuestring)
                    set_field_value(fv, fvalue->valuestring);

                cJSON *fdatefmt = cJSON_GetObjectItemCaseSensitive(field_json, "client_date_format");
                if (cJSON_IsString(fdatefmt) && fdatefmt->valuestring)
                    strncpy(fv->client_date_format, fdatefmt->valuestring,
                            sizeof(fv->client_date_format) - 1);
            }

            return req;
        }
        case OP_DELETE:
        {
            delete_request_t *req = calloc(1, sizeof(delete_request_t));
            if (!req) return NULL;

            cJSON *table_name = cJSON_GetObjectItemCaseSensitive(op_json, "table_name");
            if (cJSON_IsString(table_name) && table_name->valuestring)
                strncpy(req->table_name, table_name->valuestring, sizeof(req->table_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            /* ---- "where": [ {field_name, key_value}, ... ] - identical
             * shape and parsing to UPDATE's own WHERE clause. No "set"
             * at all - DELETE has nothing else to carry.                */
            cJSON *where = cJSON_GetObjectItemCaseSensitive(op_json, "where");
            int key_count = cJSON_IsArray(where) ? cJSON_GetArraySize(where) : 0;

            if (key_count > 0)
            {
                req->keys = calloc((size_t)key_count, sizeof(where_key_t));
                if (!req->keys) { free(req); return NULL; }
            }
            req->key_count = key_count;

            for (int k = 0; k < key_count; k++)
            {
                cJSON *key_json = cJSON_GetArrayItem(where, k);
                where_key_t *wk = &req->keys[k];

                cJSON *fname = cJSON_GetObjectItemCaseSensitive(key_json, "field_name");
                if (cJSON_IsString(fname) && fname->valuestring)
                    strncpy(wk->field_name, fname->valuestring, sizeof(wk->field_name) - 1);

                cJSON *kvalue = cJSON_GetObjectItemCaseSensitive(key_json, "key_value");
                if (cJSON_IsString(kvalue) && kvalue->valuestring)
                    strncpy(wk->key_value, kvalue->valuestring, sizeof(wk->key_value) - 1);

                cJSON *kdatefmt = cJSON_GetObjectItemCaseSensitive(key_json, "client_date_format");
                if (cJSON_IsString(kdatefmt) && kdatefmt->valuestring)
                    strncpy(wk->client_date_format, kdatefmt->valuestring,
                            sizeof(wk->client_date_format) - 1);
            }

            return req;
        }
        case OP_EXECUTE_PROCEDURE:
        {
            execute_procedure_request_t *req =
                calloc(1, sizeof(execute_procedure_request_t));
            if (!req) return NULL;

            cJSON *proc_name = cJSON_GetObjectItemCaseSensitive(op_json, "procedure_name");
            if (cJSON_IsString(proc_name) && proc_name->valuestring)
                strncpy(req->procedure_name, proc_name->valuestring,
                        sizeof(req->procedure_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            /* ---- "parameters": [ {param_name, param_type,
             * param_direction, param_value}, ... ]                      */
            cJSON *params = cJSON_GetObjectItemCaseSensitive(op_json, "parameters");
            int param_count = cJSON_IsArray(params) ? cJSON_GetArraySize(params) : 0;

            if (param_count > 0)
            {
                req->parameters = calloc((size_t)param_count, sizeof(procedure_param_t));
                if (!req->parameters) { free(req); return NULL; }
            }
            req->param_count = param_count;

            for (int i = 0; i < param_count; i++)
            {
                cJSON *param_json = cJSON_GetArrayItem(params, i);
                procedure_param_t *pp = &req->parameters[i];

                cJSON *pname = cJSON_GetObjectItemCaseSensitive(param_json, "param_name");
                if (cJSON_IsString(pname) && pname->valuestring)
                    strncpy(pp->param_name, pname->valuestring, sizeof(pp->param_name) - 1);

                cJSON *ptype = cJSON_GetObjectItemCaseSensitive(param_json, "param_type");
                if (cJSON_IsString(ptype) && ptype->valuestring)
                    strncpy(pp->param_type, ptype->valuestring, sizeof(pp->param_type) - 1);

                cJSON *pdir = cJSON_GetObjectItemCaseSensitive(param_json, "param_direction");
                if (cJSON_IsString(pdir) && pdir->valuestring)
                    pp->direction = parse_direction_l1(pdir->valuestring);

                cJSON *pvalue = cJSON_GetObjectItemCaseSensitive(param_json, "param_value");
                if (cJSON_IsString(pvalue) && pvalue->valuestring)
                    strncpy(pp->param_value, pvalue->valuestring, sizeof(pp->param_value) - 1);
            }

            return req;
        }
        case OP_AUTHENTICATE:
        {
            /* Same shape as the XML case above - see Security_Module_
             * Design_Specification.docx Section 8.1 for the JSON
             * example this mirrors.                                   */
            authenticate_request_t *req = calloc(1, sizeof(authenticate_request_t));
            if (!req) return NULL;

            cJSON *username = cJSON_GetObjectItemCaseSensitive(op_json, "username");
            if (cJSON_IsString(username) && username->valuestring)
                strncpy(req->username, username->valuestring, sizeof(req->username) - 1);

            cJSON *credential = cJSON_GetObjectItemCaseSensitive(op_json, "credential");
            if (cJSON_IsString(credential) && credential->valuestring)
                strncpy(req->credential, credential->valuestring, sizeof(req->credential) - 1);

            return req;
        }
        case OP_CHECK_PERMISSION:
        {
            /* Same shape as the XML case in build_payload_xml() - see
             * Security_Module_Design_Specification.docx Section 8.2. */
            check_permission_request_t *req =
                calloc(1, sizeof(check_permission_request_t));
            if (!req) return NULL;

            cJSON *permission_code =
                cJSON_GetObjectItemCaseSensitive(op_json, "permission_code");
            if (cJSON_IsString(permission_code) && permission_code->valuestring)
                strncpy(req->permission_code, permission_code->valuestring,
                        sizeof(req->permission_code) - 1);

            return req;
        }
        case OP_CREATE_USER:
        {
            /* Same field set as the XML case in build_payload_xml() -
             * see OCI_DDL_Create_User_Module.h.                         */
            create_user_request_t *req = calloc(1, sizeof(create_user_request_t));
            if (!req) return NULL;

            cJSON *username = cJSON_GetObjectItemCaseSensitive(op_json, "username");
            if (cJSON_IsString(username) && username->valuestring)
                strncpy(req->username, username->valuestring, sizeof(req->username) - 1);

            cJSON *identified_by = cJSON_GetObjectItemCaseSensitive(op_json, "identified_by");
            if (cJSON_IsString(identified_by) && identified_by->valuestring)
                strncpy(req->identified_by, identified_by->valuestring, sizeof(req->identified_by) - 1);

            cJSON *default_ts = cJSON_GetObjectItemCaseSensitive(op_json, "default_tablespace");
            if (cJSON_IsString(default_ts) && default_ts->valuestring)
                strncpy(req->default_tablespace, default_ts->valuestring, sizeof(req->default_tablespace) - 1);

            cJSON *temp_ts = cJSON_GetObjectItemCaseSensitive(op_json, "temp_tablespace");
            if (cJSON_IsString(temp_ts) && temp_ts->valuestring)
                strncpy(req->temp_tablespace, temp_ts->valuestring, sizeof(req->temp_tablespace) - 1);

            cJSON *quota = cJSON_GetObjectItemCaseSensitive(op_json, "quota");
            if (cJSON_IsString(quota) && quota->valuestring)
                strncpy(req->quota, quota->valuestring, sizeof(req->quota) - 1);

            cJSON *quota_ts = cJSON_GetObjectItemCaseSensitive(op_json, "quota_tablespace");
            if (cJSON_IsString(quota_ts) && quota_ts->valuestring)
                strncpy(req->quota_tablespace, quota_ts->valuestring, sizeof(req->quota_tablespace) - 1);

            cJSON *profile = cJSON_GetObjectItemCaseSensitive(op_json, "profile");
            if (cJSON_IsString(profile) && profile->valuestring)
                strncpy(req->profile, profile->valuestring, sizeof(req->profile) - 1);

            cJSON *roles = cJSON_GetObjectItemCaseSensitive(op_json, "roles");
            int role_count = cJSON_IsArray(roles) ? cJSON_GetArraySize(roles) : 0;
            for (int r = 0; r < role_count && req->role_count < MAX_CREATE_USER_ROLES; r++)
            {
                cJSON *role_item = cJSON_GetArrayItem(roles, r);
                if (cJSON_IsString(role_item) && role_item->valuestring &&
                    strlen(role_item->valuestring) > 0)
                {
                    strncpy(req->roles[req->role_count], role_item->valuestring,
                            DDL_IDENTIFIER_LEN - 1);
                    req->role_count++;
                }
            }

            /* Same quota_tablespace fallback as the XML case. */
            if (strlen(req->quota) > 0 && strlen(req->quota_tablespace) == 0)
                strncpy(req->quota_tablespace, req->default_tablespace,
                        sizeof(req->quota_tablespace) - 1);

            return req;
        }
        case OP_GRANT:
        {
            /* Same field set as the XML case in build_payload_xml() -
             * see OCI_DDL_Grant_Module.h.                               */
            grant_request_t *req = calloc(1, sizeof(grant_request_t));
            if (!req) return NULL;

            cJSON *grantee = cJSON_GetObjectItemCaseSensitive(op_json, "grantee");
            if (cJSON_IsString(grantee) && grantee->valuestring)
                strncpy(req->grantee, grantee->valuestring, sizeof(req->grantee) - 1);

            cJSON *object_type = cJSON_GetObjectItemCaseSensitive(op_json, "object_type");
            if (cJSON_IsString(object_type) && object_type->valuestring)
                strncpy(req->object_type, object_type->valuestring, sizeof(req->object_type) - 1);

            cJSON *object_name = cJSON_GetObjectItemCaseSensitive(op_json, "object_name");
            if (cJSON_IsString(object_name) && object_name->valuestring)
                strncpy(req->object_name, object_name->valuestring, sizeof(req->object_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            cJSON *wgo = cJSON_GetObjectItemCaseSensitive(op_json, "with_grant_option");
            if (cJSON_IsNumber(wgo))
                req->with_grant_option = (wgo->valueint != 0);
            else if (cJSON_IsBool(wgo))
                req->with_grant_option = cJSON_IsTrue(wgo) ? 1 : 0;

            cJSON *privileges = cJSON_GetObjectItemCaseSensitive(op_json, "privileges");
            int priv_count = cJSON_IsArray(privileges) ? cJSON_GetArraySize(privileges) : 0;
            for (int p = 0; p < priv_count && req->privilege_count < MAX_GRANT_PRIVILEGES; p++)
            {
                cJSON *priv_item = cJSON_GetArrayItem(privileges, p);
                if (cJSON_IsString(priv_item) && priv_item->valuestring &&
                    strlen(priv_item->valuestring) > 0)
                {
                    strncpy(req->privileges[req->privilege_count], priv_item->valuestring,
                            GRANT_PRIVILEGE_LEN - 1);
                    req->privilege_count++;
                }
            }

            return req;
        }
        case OP_CREATE_TABLE:
        {
            /* Same field set as the XML case in build_payload_xml() -
             * see OCI_DDL_Create_Table_Module.h.                        */
            create_table_request_t *req = calloc(1, sizeof(create_table_request_t));
            if (!req) return NULL;

            cJSON *table_name = cJSON_GetObjectItemCaseSensitive(op_json, "table_name");
            if (cJSON_IsString(table_name) && table_name->valuestring)
                strncpy(req->table_name, table_name->valuestring, sizeof(req->table_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            cJSON *columns = cJSON_GetObjectItemCaseSensitive(op_json, "columns");
            int col_count = cJSON_IsArray(columns) ? cJSON_GetArraySize(columns) : 0;
            for (int c = 0; c < col_count && req->column_count < DDL_MAX_TABLE_COLUMNS; c++)
            {
                cJSON *col_json = cJSON_GetArrayItem(columns, c);
                if (!cJSON_IsObject(col_json)) continue;

                column_def_t *col = &req->columns[req->column_count];
                memset(col, 0, sizeof(*col));
                col->nullable = 1;

                cJSON *name = cJSON_GetObjectItemCaseSensitive(col_json, "name");
                if (cJSON_IsString(name) && name->valuestring)
                    strncpy(col->name, name->valuestring, sizeof(col->name) - 1);

                cJSON *data_type = cJSON_GetObjectItemCaseSensitive(col_json, "data_type");
                if (cJSON_IsString(data_type) && data_type->valuestring)
                    strncpy(col->data_type, data_type->valuestring, sizeof(col->data_type) - 1);

                cJSON *length = cJSON_GetObjectItemCaseSensitive(col_json, "length");
                if (cJSON_IsNumber(length))
                    col->length = length->valueint;

                cJSON *precision = cJSON_GetObjectItemCaseSensitive(col_json, "precision");
                if (cJSON_IsNumber(precision))
                    col->precision = precision->valueint;

                cJSON *scale = cJSON_GetObjectItemCaseSensitive(col_json, "scale");
                if (cJSON_IsNumber(scale))
                    col->scale = scale->valueint;

                cJSON *nullable = cJSON_GetObjectItemCaseSensitive(col_json, "nullable");
                if (cJSON_IsNumber(nullable))
                    col->nullable = (nullable->valueint != 0);
                else if (cJSON_IsBool(nullable))
                    col->nullable = cJSON_IsTrue(nullable) ? 1 : 0;

                cJSON *default_value = cJSON_GetObjectItemCaseSensitive(col_json, "default_value");
                if (cJSON_IsString(default_value) && default_value->valuestring)
                    strncpy(col->default_value, default_value->valuestring, sizeof(col->default_value) - 1);

                if (strlen(col->name) > 0)
                    req->column_count++;
            }

            cJSON *primary_key = cJSON_GetObjectItemCaseSensitive(op_json, "primary_key");
            int pk_count = cJSON_IsArray(primary_key) ? cJSON_GetArraySize(primary_key) : 0;
            for (int p = 0; p < pk_count && req->primary_key_count < MAX_PRIMARY_KEY_COLUMNS; p++)
            {
                cJSON *pk_item = cJSON_GetArrayItem(primary_key, p);
                if (cJSON_IsString(pk_item) && pk_item->valuestring &&
                    strlen(pk_item->valuestring) > 0)
                {
                    strncpy(req->primary_key_columns[req->primary_key_count], pk_item->valuestring,
                            TABLE_IDENTIFIER_LEN - 1);
                    req->primary_key_count++;
                }
            }

            return req;
        }
        case OP_DROP_TABLE:
        {
            /* Same field set as the XML case in build_payload_xml() -
             * see OCI_DDL_Drop_Table_Module.h.                          */
            drop_table_request_t *req = calloc(1, sizeof(drop_table_request_t));
            if (!req) return NULL;

            cJSON *table_name = cJSON_GetObjectItemCaseSensitive(op_json, "table_name");
            if (cJSON_IsString(table_name) && table_name->valuestring)
                strncpy(req->table_name, table_name->valuestring, sizeof(req->table_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            cJSON *cascade = cJSON_GetObjectItemCaseSensitive(op_json, "cascade_constraints");
            if (cJSON_IsNumber(cascade))
                req->cascade_constraints = (cascade->valueint != 0);
            else if (cJSON_IsBool(cascade))
                req->cascade_constraints = cJSON_IsTrue(cascade) ? 1 : 0;

            cJSON *purge = cJSON_GetObjectItemCaseSensitive(op_json, "purge");
            if (cJSON_IsNumber(purge))
                req->purge = (purge->valueint != 0);
            else if (cJSON_IsBool(purge))
                req->purge = cJSON_IsTrue(purge) ? 1 : 0;

            return req;
        }
        case OP_CREATE_VIEW:
        {
            /* Same field set as the XML case in build_payload_xml() -
             * see OCI_DDL_Create_View_Module.h.                         */
            create_view_request_t *req = calloc(1, sizeof(create_view_request_t));
            if (!req) return NULL;

            cJSON *view_name = cJSON_GetObjectItemCaseSensitive(op_json, "view_name");
            if (cJSON_IsString(view_name) && view_name->valuestring)
                strncpy(req->view_name, view_name->valuestring, sizeof(req->view_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            cJSON *replace = cJSON_GetObjectItemCaseSensitive(op_json, "replace");
            if (cJSON_IsNumber(replace))
                req->replace = (replace->valueint != 0);
            else if (cJSON_IsBool(replace))
                req->replace = cJSON_IsTrue(replace) ? 1 : 0;

            cJSON *force = cJSON_GetObjectItemCaseSensitive(op_json, "force");
            if (cJSON_IsNumber(force))
                req->force = (force->valueint != 0);
            else if (cJSON_IsBool(force))
                req->force = cJSON_IsTrue(force) ? 1 : 0;

            cJSON *query = cJSON_GetObjectItemCaseSensitive(op_json, "query");
            if (cJSON_IsString(query) && query->valuestring)
                strncpy(req->query, query->valuestring, sizeof(req->query) - 1);

            cJSON *columns = cJSON_GetObjectItemCaseSensitive(op_json, "columns");
            int col_count = cJSON_IsArray(columns) ? cJSON_GetArraySize(columns) : 0;
            for (int c = 0; c < col_count && req->column_count < MAX_VIEW_COLUMNS; c++)
            {
                cJSON *col_item = cJSON_GetArrayItem(columns, c);
                if (cJSON_IsString(col_item) && col_item->valuestring &&
                    strlen(col_item->valuestring) > 0)
                {
                    strncpy(req->columns[req->column_count], col_item->valuestring,
                            VIEW_IDENTIFIER_LEN - 1);
                    req->column_count++;
                }
            }

            return req;
        }
        case OP_CREATE_PROCEDURE:
        {
            /* Same field set as the XML case in build_payload_xml() -
             * see OCI_DDL_Create_Procedure_Module.h.                    */
            create_procedure_request_t *req = calloc(1, sizeof(create_procedure_request_t));
            if (!req) return NULL;

            cJSON *procedure_name = cJSON_GetObjectItemCaseSensitive(op_json, "procedure_name");
            if (cJSON_IsString(procedure_name) && procedure_name->valuestring)
                strncpy(req->procedure_name, procedure_name->valuestring, sizeof(req->procedure_name) - 1);

            cJSON *owner = cJSON_GetObjectItemCaseSensitive(op_json, "owner");
            if (cJSON_IsString(owner) && owner->valuestring)
                strncpy(req->owner, owner->valuestring, sizeof(req->owner) - 1);

            cJSON *replace = cJSON_GetObjectItemCaseSensitive(op_json, "replace");
            if (cJSON_IsNumber(replace))
                req->replace = (replace->valueint != 0);
            else if (cJSON_IsBool(replace))
                req->replace = cJSON_IsTrue(replace) ? 1 : 0;

            cJSON *body = cJSON_GetObjectItemCaseSensitive(op_json, "body");
            if (cJSON_IsString(body) && body->valuestring)
                strncpy(req->body, body->valuestring, sizeof(req->body) - 1);

            cJSON *parameters = cJSON_GetObjectItemCaseSensitive(op_json, "parameters");
            int param_count = cJSON_IsArray(parameters) ? cJSON_GetArraySize(parameters) : 0;
            for (int p = 0; p < param_count && req->parameter_count < MAX_PROCEDURE_PARAMETERS; p++)
            {
                cJSON *param_json = cJSON_GetArrayItem(parameters, p);
                if (!cJSON_IsObject(param_json)) continue;

                ddl_procedure_param_t *param = &req->parameters[req->parameter_count];
                memset(param, 0, sizeof(*param));
                strncpy(param->mode, "IN", sizeof(param->mode) - 1); /* default */

                cJSON *name = cJSON_GetObjectItemCaseSensitive(param_json, "name");
                if (cJSON_IsString(name) && name->valuestring)
                    strncpy(param->name, name->valuestring, sizeof(param->name) - 1);

                cJSON *data_type = cJSON_GetObjectItemCaseSensitive(param_json, "data_type");
                if (cJSON_IsString(data_type) && data_type->valuestring)
                    strncpy(param->data_type, data_type->valuestring, sizeof(param->data_type) - 1);

                cJSON *mode = cJSON_GetObjectItemCaseSensitive(param_json, "mode");
                if (cJSON_IsString(mode) && mode->valuestring)
                    strncpy(param->mode, mode->valuestring, sizeof(param->mode) - 1);

                cJSON *default_value = cJSON_GetObjectItemCaseSensitive(param_json, "default_value");
                if (cJSON_IsString(default_value) && default_value->valuestring)
                    strncpy(param->default_value, default_value->valuestring, sizeof(param->default_value) - 1);

                if (strlen(param->name) > 0)
                    req->parameter_count++;
            }

            return req;
        }
        default:
            return NULL;
    }
}
static int parse_xml(oci_context_t *ctx, const char *buf, size_t len,
                      input_c_request_t *out, operation_status_t *error_detail)
{
    xmlDocPtr doc = xmlReadMemory(buf, (int)len, "request.xml", NULL,
                                  XML_PARSE_NOBLANKS | XML_PARSE_NONET);
    if (!doc)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 1: XML not well-formed");
        set_error(error_detail, LEVEL1_ERR_MALFORMED,
                  "LEVEL1_MALFORMED", "Request aborted. Level 1 parse failed - XML not well-formed.");
        return LEVEL1_ERR_MALFORMED;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || xmlStrcmp(root->name, (const xmlChar *)"request") != 0)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 1: root element is not <request>");
        set_error(error_detail, LEVEL1_ERR_MALFORMED,
                  "LEVEL1_MALFORMED", "Request aborted. Level 1 parse failed - missing <request> root.");
        xmlFreeDoc(doc);
        return LEVEL1_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    out->source_format = INPUT_FORMAT_XML;
    strncpy(out->transaction_name, "No Name Specified",
            sizeof(out->transaction_name) - 1);

    int have_audit_id = 0, have_session_id = 0;

    for (xmlNodePtr node = root->children; node; node = node->next)
    {
        if (node->type != XML_ELEMENT_NODE) continue;

        if (xmlStrcmp(node->name, (const xmlChar *)"external_audit_id") == 0)
        {
            xmlChar *content = xmlNodeGetContent(node);
            if (content && content[0] != '\0')
            {
                strncpy(out->external_audit_id, (const char *)content,
                        sizeof(out->external_audit_id) - 1);
                have_audit_id = 1;
            }
            xmlFree(content);
        }
        else if (xmlStrcmp(node->name, (const xmlChar *)"session_id") == 0)
        {
            xmlChar *content = xmlNodeGetContent(node);
            if (content)
            {
                strncpy(out->session_id, (const char *)content,
                        sizeof(out->session_id) - 1);
                have_session_id = 1;   /* present, even if "-" */
            }
            xmlFree(content);
        }
        else if (xmlStrcmp(node->name, (const xmlChar *)"transaction") == 0)
        {
            xmlChar *req_attr = xmlGetProp(node, (const xmlChar *)"required");
            out->transaction_required = (req_attr && xmlStrcmp(req_attr, (const xmlChar *)"1") == 0) ? 1 : 0;
            xmlFree(req_attr);

            /* transaction_name is optional - business label only, e.g.
             * name="Save Booking". Absent, or present-but-empty, both
             * fall back to the same placeholder so nothing downstream
             * (metrics, audit trail CHANGE_REASON) ever sees "". */
            xmlChar *name_attr = xmlGetProp(node, (const xmlChar *)"name");
            if (name_attr && name_attr[0] != '\0')
                strncpy(out->transaction_name, (const char *)name_attr,
                        sizeof(out->transaction_name) - 1);
            else
                strncpy(out->transaction_name, "No Name Specified",
                        sizeof(out->transaction_name) - 1);
            out->transaction_name[sizeof(out->transaction_name) - 1] = '\0';
            xmlFree(name_attr);

            /* Count operations first so we can allocate exactly once */
            int op_count = 0;
            for (xmlNodePtr op = node->children; op; op = op->next)
                if (op->type == XML_ELEMENT_NODE &&
                    xmlStrcmp(op->name, (const xmlChar *)"operation") == 0)
                    op_count++;

            if (op_count == 0)
            {
                logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                             "Level 1: <transaction> has no <operation> elements");
                set_error(error_detail, LEVEL1_ERR_MISSING_FIELD,
                          "LEVEL1_MISSING_FIELD",
                          "Request aborted. Level 1 parse failed - at least one operation is required.");
                xmlFreeDoc(doc);
                return LEVEL1_ERR_MISSING_FIELD;
            }

            out->operations = calloc((size_t)op_count, sizeof(input_c_operation_t));
            if (!out->operations)
            {
                set_error(error_detail, LEVEL1_ERR_ALLOC, "LEVEL1_ALLOC",
                          "Request aborted. Level 1 parse failed - allocation failure.");
                xmlFreeDoc(doc);
                return LEVEL1_ERR_ALLOC;
            }
            out->operation_count = op_count;

            int idx = 0;
            for (xmlNodePtr op = node->children; op; op = op->next)
            {
                if (op->type != XML_ELEMENT_NODE ||
                    xmlStrcmp(op->name, (const xmlChar *)"operation") != 0)
                    continue;

                xmlChar *type_attr = xmlGetProp(op, (const xmlChar *)"type");
                out->operations[idx].type = map_operation_type((const char *)type_attr);
                xmlFree(type_attr);
                out->operations[idx].payload = build_payload_xml(op, out->operations[idx].type);
                idx++;
            }
        }
    }

    xmlFreeDoc(doc);

    /* ---- Step 3: mandatory field check ----
     * session_id must be PRESENT (even as "-"), not necessarily a real
     * value - every non-CREATE_SESSION test fixture uses "-" as the
     * standard stub, same convention used throughout the rest of the
     * project's testing to date. Whether "-" is an acceptable value to
     * actually EXECUTE an operation against belongs to real session
     * validation at CRUD dispatch time, once that's wired in - not to
     * Level 1, which only checks structural presence.
     */

    if (!have_audit_id)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 1: external_audit_id missing or empty");
        set_error(error_detail, LEVEL1_ERR_MISSING_FIELD, "LEVEL1_MISSING_FIELD",
                  "Request aborted. Level 1 parse failed - external_audit_id is mandatory.");
        free(out->operations);
        memset(out, 0, sizeof(*out));
        return LEVEL1_ERR_MISSING_FIELD;
    }

    if (!have_session_id)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 1: session_id element missing entirely");
        set_error(error_detail, LEVEL1_ERR_MISSING_FIELD, "LEVEL1_MISSING_FIELD",
                  "Request aborted. Level 1 parse failed - session_id is mandatory (use \"-\" if none, e.g. for CREATE_SESSION).");
        free(out->operations);
        memset(out, 0, sizeof(*out));
        return LEVEL1_ERR_MISSING_FIELD;
    }

    strncpy(out->version, "1.0", sizeof(out->version) - 1);
    set_ok(error_detail);
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Level 1: XML request parsed OK - audit_id=%s session_id=%s operations=%d",
                 out->external_audit_id, out->session_id, out->operation_count);
    return LEVEL1_OK;
}

/* ------------------------------------------------------------------ */
/*  JSON path                                                            */
/* ------------------------------------------------------------------ */
static int parse_json(oci_context_t *ctx, const char *buf, size_t len,
                       input_c_request_t *out, operation_status_t *error_detail)
{
    cJSON *root = cJSON_ParseWithLength(buf, len);
    if (!root)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 1: JSON not well-formed");
        set_error(error_detail, LEVEL1_ERR_MALFORMED, "LEVEL1_MALFORMED",
                  "Request aborted. Level 1 parse failed - JSON not well-formed.");
        return LEVEL1_ERR_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    out->source_format = INPUT_FORMAT_JSON;
    strncpy(out->transaction_name, "No Name Specified",
            sizeof(out->transaction_name) - 1);

    cJSON *audit_id = cJSON_GetObjectItemCaseSensitive(root, "external_audit_id");
    cJSON *session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    cJSON *transaction = cJSON_GetObjectItemCaseSensitive(root, "transaction");

    int have_audit_id = cJSON_IsString(audit_id) && audit_id->valuestring[0] != '\0';
    if (have_audit_id)
        strncpy(out->external_audit_id, audit_id->valuestring,
                sizeof(out->external_audit_id) - 1);

    int have_session_id = cJSON_IsString(session_id);
    if (have_session_id)
        strncpy(out->session_id, session_id->valuestring,
                sizeof(out->session_id) - 1);

    if (cJSON_IsObject(transaction))
    {
        cJSON *required = cJSON_GetObjectItemCaseSensitive(transaction, "required");
        out->transaction_required = (cJSON_IsNumber(required) && required->valueint == 1) ? 1 : 0;

        /* transaction_name is optional - same placeholder fallback as
         * the XML path (parse_xml's own "name" attribute handling). */
        cJSON *tx_name = cJSON_GetObjectItemCaseSensitive(transaction, "name");
        if (cJSON_IsString(tx_name) && tx_name->valuestring[0] != '\0')
        {
            strncpy(out->transaction_name, tx_name->valuestring,
                    sizeof(out->transaction_name) - 1);
            out->transaction_name[sizeof(out->transaction_name) - 1] = '\0';
        }

        cJSON *ops = cJSON_GetObjectItemCaseSensitive(transaction, "operations");
        int op_count = cJSON_IsArray(ops) ? cJSON_GetArraySize(ops) : 0;

        if (op_count == 0)
        {
            logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                         "Level 1: transaction.operations is missing or empty");
            set_error(error_detail, LEVEL1_ERR_MISSING_FIELD, "LEVEL1_MISSING_FIELD",
                      "Request aborted. Level 1 parse failed - at least one operation is required.");
            cJSON_Delete(root);
            return LEVEL1_ERR_MISSING_FIELD;
        }

        out->operations = calloc((size_t)op_count, sizeof(input_c_operation_t));
        if (!out->operations)
        {
            set_error(error_detail, LEVEL1_ERR_ALLOC, "LEVEL1_ALLOC",
                      "Request aborted. Level 1 parse failed - allocation failure.");
            cJSON_Delete(root);
            return LEVEL1_ERR_ALLOC;
        }
        out->operation_count = op_count;

        for (int i = 0; i < op_count; i++)
        {
            cJSON *op = cJSON_GetArrayItem(ops, i);
            cJSON *type = cJSON_GetObjectItemCaseSensitive(op, "type");
            out->operations[i].type = map_operation_type(
                cJSON_IsString(type) ? type->valuestring : NULL);
            out->operations[i].payload = build_payload_json(op, out->operations[i].type);
        }
    }

    cJSON_Delete(root);

    if (!have_audit_id)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 1: external_audit_id missing or empty");
        set_error(error_detail, LEVEL1_ERR_MISSING_FIELD, "LEVEL1_MISSING_FIELD",
                  "Request aborted. Level 1 parse failed - external_audit_id is mandatory.");
        free(out->operations);
        memset(out, 0, sizeof(*out));
        return LEVEL1_ERR_MISSING_FIELD;
    }

    if (!have_session_id)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 1: session_id field missing entirely");
        set_error(error_detail, LEVEL1_ERR_MISSING_FIELD, "LEVEL1_MISSING_FIELD",
                  "Request aborted. Level 1 parse failed - session_id is mandatory (use \"-\" if none, e.g. for CREATE_SESSION).");
        free(out->operations);
        memset(out, 0, sizeof(*out));
        return LEVEL1_ERR_MISSING_FIELD;
    }

    strncpy(out->version, "1.0", sizeof(out->version) - 1);
    set_ok(error_detail);
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Level 1: JSON request parsed OK - audit_id=%s session_id=%s operations=%d",
                 out->external_audit_id, out->session_id, out->operation_count);
    return LEVEL1_OK;
}

/* ------------------------------------------------------------------ */
/*  Dispatcher                                                           */
/* ------------------------------------------------------------------ */
int level1_parse(oci_context_t *ctx, const char *buf, size_t len,
                  input_c_request_t *out_request, operation_status_t *error_detail)
{
    if (!ctx || !buf || !out_request)
    {
        set_error(error_detail, LEVEL1_ERR_EMPTY_INPUT, "LEVEL1_EMPTY_INPUT",
                  "Request aborted. Level 1 parse failed - no input provided.");
        return LEVEL1_ERR_EMPTY_INPUT;
    }

    if (len == 0)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Level 1: empty request body");
        set_error(error_detail, LEVEL1_ERR_EMPTY_INPUT, "LEVEL1_EMPTY_INPUT",
                  "Request aborted. Level 1 parse failed - empty request body.");
        return LEVEL1_ERR_EMPTY_INPUT;
    }

    input_format_t fmt = level1_detect_format(buf, len);

    switch (fmt)
    {
        case INPUT_FORMAT_XML:
            logger_write(ctx->logger, LOG_DEBUG, __func__, 0,
                         "Level 1: detected XML");
            return parse_xml(ctx, buf, len, out_request, error_detail);

        case INPUT_FORMAT_JSON:
            logger_write(ctx->logger, LOG_DEBUG, __func__, 0,
                         "Level 1: detected JSON");
            return parse_json(ctx, buf, len, out_request, error_detail);

        default:
            logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                         "Level 1: could not determine document type "
                         "(first non-whitespace byte is not '<', '{', or '[')");
            set_error(error_detail, LEVEL1_ERR_UNKNOWN_FORMAT, "LEVEL1_UNKNOWN_FORMAT",
                      "Request aborted. Level 1 parse failed - unrecognised document type.");
            return LEVEL1_ERR_UNKNOWN_FORMAT;
    }
}

void level1_free_request(input_c_request_t *request)
{
    if (!request) return;

    for (int i = 0; i < request->operation_count; i++)
    {
        input_c_operation_t *op = &request->operations[i];

        switch (op->type)
        {
            case OP_INSERT:
            {
                insert_request_t *req = (insert_request_t *)op->payload;
                if (req)
                {
                    for (int r = 0; r < req->row_count; r++)
                    {
                        /* Free each field's large_value overflow, if
                         * any, before freeing the fields array itself -
                         * see field_value_t's own doc comment in
                         * OCI_Request_Response_Types.h.                 */
                        for (int f = 0; f < req->rows[r].field_count; f++)
                            free(req->rows[r].fields[f].large_value);
                        free(req->rows[r].fields);
                    }
                    free(req->rows);
                }
                free(req);
                break;
            }

            case OP_UPDATE:
            {
                update_request_t *req = (update_request_t *)op->payload;
                if (req)
                {
                    /* fields[] is a flat SET list (no per-row nesting,
                     * unlike INSERT's rows[].fields[]) - same large_value
                     * overflow freeing, one level shallower.             */
                    for (int f = 0; f < req->field_count; f++)
                        free(req->fields[f].large_value);
                    free(req->fields);
                    free(req->keys);
                }
                free(req);
                break;
            }

            case OP_DELETE:
            {
                /* No fields[], no large_value at all - DELETE has no SET
                 * clause, so nothing here can ever have overflowed into
                 * a heap allocation the way a field_value_t can.         */
                delete_request_t *req = (delete_request_t *)op->payload;
                if (req)
                    free(req->keys);
                free(req);
                break;
            }

            case OP_EXECUTE_PROCEDURE:
            {
                /* parameters[] is a flat array - procedure_param_t has
                 * no nested heap allocations at all (param_value is a
                 * plain fixed char[4096], no large_value overflow
                 * mechanism the way field_value_t has), so this is
                 * just two frees.                                       */
                execute_procedure_request_t *req =
                    (execute_procedure_request_t *)op->payload;
                if (req)
                    free(req->parameters);
                free(req);
                break;
            }

            default:
                /* select_request_t, and any other type with no nested
                 * allocations, is correctly freed by a flat free().        */
                free(op->payload);
                break;
        }
    }

    free(request->operations);
    request->operations = NULL;
    request->operation_count = 0;
}
