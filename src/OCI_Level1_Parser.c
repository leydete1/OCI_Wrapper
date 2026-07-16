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

/* ------------------------------------------------------------------ */
/*  XML path                                                             */
/* ------------------------------------------------------------------ */
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
                out->operations[idx].payload = NULL;  /* Level 2's job */
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
            out->operations[i].payload = NULL;   /* Level 2's job */
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
    free(request->operations);
    request->operations = NULL;
    request->operation_count = 0;
}
