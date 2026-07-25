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
