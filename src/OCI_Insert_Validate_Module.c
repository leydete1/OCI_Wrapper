
/*
 * OCI_Insert_Validate_Module.c
 *
 * Stage 2 - Insert Template Validator
 * -------------------------------------
 * Parses every <field> block in the <Insert Template> XML produced by
 * Stage 1 and validates the <insert_value> against the declared Oracle
 * column metadata.
 *
 * Validation rules per Oracle data type are documented in the header.
 * This module is completely self-contained; it does NOT make any OCI
 * calls at runtime (all metadata it needs is embedded in the XML).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#include "OCI_Insert_Validate_Module.h"
#include "logger.h"

/* parsed_field_t is now declared in OCI_Insert_Validate_Module.h -
 * exposed there so level2_validate_insert() can build one from
 * insert_request_t + metadata_cache and reuse validate_field()
 * directly, rather than this being a private detail of this file. */

/* ------------------------------------------------------------------ */
/*  Static helpers                                                      */
/* ------------------------------------------------------------------ */

static void trim_inplace_v(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
    { s[len - 1] = '\0'; len--; }
}

/* Extract text between <tag> and </tag>; returns 1 on success */
static int extract_tag(const char *src, const char *tag,
                       char *dest, size_t dest_max)
{
    if (!src || !tag || !dest) return 0;
    char open [132], close[132];
    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *s = strstr(src, open);
    if (!s) return 0;
    s += strlen(open);
    const char *e = strstr(s, close);
    if (!e) return 0;
    size_t len = (size_t)(e - s);
    if (len >= dest_max) len = dest_max - 1;
    memcpy(dest, s, len);
    dest[len] = '\0';
    trim_inplace_v(dest);
    return 1;
}

/* Extract integer between <tag> and </tag>; returns 1 on success */
static int extract_tag_int(const char *src, const char *tag, int *out)
{
    char buf[32] = {0};
    if (!extract_tag(src, tag, buf, sizeof(buf))) return 0;
    *out = atoi(buf);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Type-family helpers                                                 */
/* ------------------------------------------------------------------ */

/* Is the type a numeric family? */
static int is_numeric_type(const char *t)
{
    return (strncmp(t, "NUMBER",        6)  == 0 ||
            strncmp(t, "FLOAT",         5)  == 0 ||
            strcmp (t, "BINARY_FLOAT")      == 0 ||
            strcmp (t, "BINARY_DOUBLE")     == 0 ||
            strcmp (t, "INTEGER")           == 0 ||
            strcmp (t, "INT")               == 0 ||
            strcmp (t, "SMALLINT")          == 0 ||
            strcmp (t, "DECIMAL")           == 0 ||
            strcmp (t, "NUMERIC")           == 0 ||
            strcmp (t, "REAL")              == 0 ||
            strcmp (t, "DOUBLE PRECISION")  == 0);
}

static int is_string_type(const char *t)
{
    return (strcmp(t, "CHAR")      == 0 ||
            strcmp(t, "VARCHAR2")  == 0 ||
            strcmp(t, "NCHAR")     == 0 ||
            strcmp(t, "NVARCHAR2") == 0);
}

static int is_lob_type(const char *t)
{
    return (strcmp(t, "CLOB")  == 0 ||
            strcmp(t, "NCLOB") == 0 ||
            strcmp(t, "BLOB")  == 0);
}

static int is_date_type(const char *t)
{
    return strcmp(t, "DATE") == 0;
}

static int is_timestamp_type(const char *t)
{
    return (strncmp(t, "TIMESTAMP", 9) == 0);
}

static int is_interval_ym_type(const char *t)
{
    return (strstr(t, "INTERVAL") != NULL &&
            strstr(t, "MONTH")    != NULL);
}

static int is_interval_ds_type(const char *t)
{
    return (strstr(t, "INTERVAL") != NULL &&
            strstr(t, "SECOND")   != NULL);
}

static int is_raw_type(const char *t)
{
    return strcmp(t, "RAW") == 0;
}

static int is_rowid_type(const char *t)
{
    return (strcmp(t, "ROWID")  == 0 ||
            strcmp(t, "UROWID") == 0);
}

/* ------------------------------------------------------------------ */
/*  Individual type validators                                          */
/*  All return field_validation_result_t and write a message on error  */
/* ------------------------------------------------------------------ */

/* ---- Numeric ---- */
static field_validation_result_t
validate_numeric(const parsed_field_t *f, char *msg, size_t msg_max)
{
    const char *v = f->insert_value;

    /* Allow optional leading +/- */
    const char *p = v;
    if (*p == '+' || *p == '-') p++;

    if (*p == '\0')
    {
        snprintf(msg, msg_max,
                 "Field '%s': empty value for numeric type '%s'",
                 f->field_name, f->field_type);
        return FIELD_TYPE_MISMATCH;
    }

    /* Walk integer digits */
    int int_digits = 0;
    while (*p && *p != '.' && *p != 'e' && *p != 'E')
    {
        if (!isdigit((unsigned char)*p))
        {
            snprintf(msg, msg_max,
                     "Field '%s': invalid character '%c' in numeric value",
                     f->field_name, *p);
            return FIELD_TYPE_MISMATCH;
        }
        int_digits++;
        p++;
    }

    /* Fractional part */
    int frac_digits = 0;
    if (*p == '.')
    {
        p++;
        while (*p && *p != 'e' && *p != 'E')
        {
            if (!isdigit((unsigned char)*p))
            {
                snprintf(msg, msg_max,
                         "Field '%s': invalid character '%c' in "
                         "fractional part",
                         f->field_name, *p);
                return FIELD_TYPE_MISMATCH;
            }
            frac_digits++;
            p++;
        }
    }

    /* Optional exponent */
    if (*p == 'e' || *p == 'E')
    {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p))
        {
            snprintf(msg, msg_max,
                     "Field '%s': malformed exponent in numeric value",
                     f->field_name);
            return FIELD_TYPE_MISMATCH;
        }
        while (*p && isdigit((unsigned char)*p)) p++;
    }

    if (*p != '\0')
    {
        snprintf(msg, msg_max,
                 "Field '%s': trailing garbage '%s' in numeric value",
                 f->field_name, p);
        return FIELD_TYPE_MISMATCH;
    }

    /* Precision / scale check for NUMBER(p,s) */
    int prec  = f->field_precision;
    int scale = f->field_scale;

    if (prec > 0 && scale >= 0)
    {
        int max_int_digits = prec - scale;
        if (int_digits > max_int_digits)
        {
            snprintf(msg, msg_max,
                     "Field '%s': integer part (%d digits) exceeds "
                     "NUMBER(%d,%d) precision",
                     f->field_name, int_digits, prec, scale);
            return FIELD_PRECISION_EXCEEDED;
        }
        if (frac_digits > scale)
        {
            snprintf(msg, msg_max,
                     "Field '%s': fractional part (%d digits) exceeds "
                     "scale %d of NUMBER(%d,%d)",
                     f->field_name, frac_digits, scale, prec, scale);
            return FIELD_SCALE_EXCEEDED;
        }
    }

    return FIELD_VALID;
}

/* ---- String / CHAR / VARCHAR2 ---- */
static field_validation_result_t
validate_string(const parsed_field_t *f, char *msg, size_t msg_max)
{
    int len = (int)strlen(f->insert_value);
    if (f->field_length > 0 && len > f->field_length)
    {
        snprintf(msg, msg_max,
                 "Field '%s': value length %d exceeds "
                 "%s(%d)",
                 f->field_name, len,
                 f->field_type, f->field_length);
        return FIELD_LENGTH_EXCEEDED;
    }
    return FIELD_VALID;
}

/* ---- DATE ----
 * No longer does its own format checking here (2026-07-28) - used to
 * be a hardcoded sscanf("%d-%d-%d %d:%d:%d", ...) pattern, completely
 * disconnected from ctx->ini->nls_date_format and from the
 * <client_date_format> mechanism added 2026-07-27. That hardcoded
 * pattern is exactly what the "remove all hardcoding of date format"
 * decision was about - and it's now genuinely redundant, not just
 * hardcoded: OCI_Level2_Parser.c's normalize_client_date_value() has
 * already validated (and normalized) this value via a real Oracle
 * round-trip against the actual configured nls_date_format (or the
 * client's own declared format) before validate_field() is ever
 * called at all. This function stays as a real dispatch target rather
 * than being removed, so nothing upstream needs to change, but it has
 * nothing left to check.                                              */
static field_validation_result_t
validate_date(const parsed_field_t *f, char *msg, size_t msg_max)
{
    (void)f; (void)msg; (void)msg_max;
    return FIELD_VALID;
}

/* ---- TIMESTAMP (all variants) ----
 * Same reasoning as validate_date() above - already validated
 * authoritatively by OCI_Level2_Parser.c's normalize_client_date_
 * value() before this ever runs.                                      */
static field_validation_result_t
validate_timestamp(const parsed_field_t *f, char *msg, size_t msg_max)
{
    (void)f; (void)msg; (void)msg_max;
    return FIELD_VALID;
}

/* ---- INTERVAL YEAR TO MONTH: [+/-]YY-MM ---- */
static field_validation_result_t
validate_interval_ym(const parsed_field_t *f, char *msg, size_t msg_max)
{
    const char *v = f->insert_value;
    if (*v == '+' || *v == '-') v++;

    int yy, mm;
    if (sscanf(v, "%d-%d", &yy, &mm) != 2 || mm < 0 || mm > 11)
    {
        snprintf(msg, msg_max,
                 "Field '%s': INTERVAL YEAR TO MONTH value '%s' "
                 "does not match [+/-]YY-MM (months 0-11)",
                 f->field_name, f->insert_value);
        return FIELD_FORMAT_INVALID;
    }
    return FIELD_VALID;
}

/* ---- INTERVAL DAY TO SECOND: [+/-]DD HH:MI:SS[.ffffff] ---- */
static field_validation_result_t
validate_interval_ds(const parsed_field_t *f, char *msg, size_t msg_max)
{
    const char *v = f->insert_value;
    if (*v == '+' || *v == '-') v++;

    int dd, hh, mi, ss;
    if (sscanf(v, "%d %d:%d:%d", &dd, &hh, &mi, &ss) != 4 ||
        hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 59)
    {
        snprintf(msg, msg_max,
                 "Field '%s': INTERVAL DAY TO SECOND value '%s' "
                 "does not match [+/-]DD HH:MI:SS[.ffffff]",
                 f->field_name, f->insert_value);
        return FIELD_FORMAT_INVALID;
    }
    return FIELD_VALID;
}

/* ---- RAW: must be hex string, byte length <= field_length ---- */
static field_validation_result_t
validate_raw(const parsed_field_t *f, char *msg, size_t msg_max)
{
    const char *v = f->insert_value;
    int len = (int)strlen(v);

    for (int i = 0; i < len; i++)
    {
        if (!isxdigit((unsigned char)v[i]))
        {
            snprintf(msg, msg_max,
                     "Field '%s': RAW value contains non-hex character "
                     "'%c' at position %d",
                     f->field_name, v[i], i + 1);
            return FIELD_HEX_INVALID;
        }
    }
    /* Hex string length in chars = bytes * 2 */
    if (f->field_length > 0 && len > f->field_length * 2)
    {
        snprintf(msg, msg_max,
                 "Field '%s': RAW hex string (%d hex chars = %d bytes) "
                 "exceeds RAW(%d BYTE)",
                 f->field_name, len, len / 2, f->field_length);
        return FIELD_LENGTH_EXCEEDED;
    }
    return FIELD_VALID;
}

/* ---- ROWID / UROWID: 18-char Oracle extended ROWID base-64 charset ---- */
/* Oracle base-64 uses A-Z a-z 0-9 + / */
static int is_oracle_b64_char(char c)
{
    return (isalpha((unsigned char)c) ||
            isdigit((unsigned char)c) ||
            c == '+' || c == '/');
}

static field_validation_result_t
validate_rowid(const parsed_field_t *f, char *msg, size_t msg_max)
{
    const char *v   = f->insert_value;
    int         len = (int)strlen(v);

    /* Extended ROWID is exactly 18 chars; UROWID may vary but must
     * consist only of valid Oracle base-64 characters               */
    if (strcmp(f->field_type, "ROWID") == 0 && len != 18)
    {
        snprintf(msg, msg_max,
                 "Field '%s': ROWID must be exactly 18 characters "
                 "(got %d)",
                 f->field_name, len);
        return FIELD_ROWID_INVALID;
    }
    for (int i = 0; i < len; i++)
    {
        if (!is_oracle_b64_char(v[i]))
        {
            snprintf(msg, msg_max,
                     "Field '%s': ROWID/UROWID value contains invalid "
                     "character '%c' at position %d",
                     f->field_name, v[i], i + 1);
            return FIELD_ROWID_INVALID;
        }
    }
    return FIELD_VALID;
}

/* ------------------------------------------------------------------ */
/*  Dispatch: validate one parsed_field_t                              */
/*  No longer static - declared in OCI_Insert_Validate_Module.h so      */
/*  level2_validate_insert() (OCI_Level2_Parser.c) can call this        */
/*  directly and reuse the exact same rules.                            */
/* ------------------------------------------------------------------ */
field_validation_result_t
validate_field(oci_context_t        *ctx,
               const parsed_field_t *f,
               char                 *msg,
               size_t                msg_max)
{
    const char *v    = f->insert_value;
    int         empty = (strlen(v) == 0);

    /* ---- Nullable check ---- */
    if (empty)
    {
        /* If NOT NULL and no default Oracle won't accept it */
        if (f->field_nullable[0] == 'N' && strlen(f->field_default) == 0)
        {
            snprintf(msg, msg_max,
                     "Field '%s' (%s): NOT NULL column has empty "
                     "insert_value and no column default",
                     f->field_name, f->field_type);
            return FIELD_NULL_VIOLATION;
        }
        /* Otherwise empty is allowed (NULL or default applies) */
        logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                     "Field '%s': empty value accepted (nullable=%s "
                     "default='%s')",
                     f->field_name, f->field_nullable, f->field_default);
        return FIELD_VALID;
    }

    /* ---- Type dispatch ---- */
    if (is_numeric_type(f->field_type))
        return validate_numeric(f, msg, msg_max);

    if (is_string_type(f->field_type))
        return validate_string(f, msg, msg_max);

    if (is_date_type(f->field_type))
        return validate_date(f, msg, msg_max);

    if (is_timestamp_type(f->field_type))
        return validate_timestamp(f, msg, msg_max);

    if (is_interval_ym_type(f->field_type))
        return validate_interval_ym(f, msg, msg_max);

    if (is_interval_ds_type(f->field_type))
        return validate_interval_ds(f, msg, msg_max);

    if (is_raw_type(f->field_type))
        return validate_raw(f, msg, msg_max);

    if (is_rowid_type(f->field_type))
        return validate_rowid(f, msg, msg_max);

    if (is_lob_type(f->field_type))
    {
        /* CLOB / NCLOB: any text OK at template level */
        /* BLOB: any non-empty string treated as file path; Stage 3
         * will verify the file exists when executing the INSERT       */
        logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                     "Field '%s' (%s): LOB value accepted at "
                     "template level",
                     f->field_name, f->field_type);
        return FIELD_VALID;
    }

    /* Unknown type: log a warning but do not fail */
    logger_write(ctx->insert_logger, LOG_WARN, __func__, 0,
                 "Field '%s': unrecognised type '%s', skipping "
                 "type-specific validation",
                 f->field_name, f->field_type);
    return FIELD_VALID;
}

/* ------------------------------------------------------------------ */
/*  Parse one <field> block into parsed_field_t                        */
/*  Returns 1 on success, 0 on parse failure                           */
/* ------------------------------------------------------------------ */
static int parse_field_block(const char    *block,
                              parsed_field_t *out)
{
    memset(out, 0, sizeof(*out));

    char tmp[32] = {0};

    if (!extract_tag(block, "field_number",   tmp,                 sizeof(tmp)))
        return 0;
    out->field_number = atoi(tmp);

    if (!extract_tag(block, "field_name",     out->field_name,     sizeof(out->field_name)))
        return 0;
    if (!extract_tag(block, "field_type",     out->field_type,     sizeof(out->field_type)))
        return 0;

    memset(tmp, 0, sizeof(tmp));
    if (!extract_tag(block, "field_length",   tmp,                 sizeof(tmp)))
        return 0;
    out->field_length = atoi(tmp);

    memset(tmp, 0, sizeof(tmp));
    if (!extract_tag(block, "field_precision",tmp,                 sizeof(tmp)))
        return 0;
    out->field_precision = atoi(tmp);

    memset(tmp, 0, sizeof(tmp));
    if (!extract_tag(block, "field_scale",    tmp,                 sizeof(tmp)))
        return 0;
    out->field_scale = atoi(tmp);

    if (!extract_tag(block, "field_nullable", out->field_nullable, sizeof(out->field_nullable)))
        return 0;

    /* field_default may be absent / empty - that is fine */
    extract_tag(block, "field_default",   out->field_default,  sizeof(out->field_default));

    /* insert_value may be absent / empty */
    extract_tag(block, "insert_value",    out->insert_value,   sizeof(out->insert_value));

    return 1;
}

/* ==================================================================
 *  validate_insert_template - public entry point
 * ================================================================== */
int validate_insert_template(oci_context_t *ctx,
                              const char    *template_xml,
                              char          *error_buf,
                              size_t         error_buf_size)
{
    int failures    = 0;
    int field_count = 0;

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "Entering validate_insert_template");

    if (!ctx || !template_xml)
    {
        logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments: ctx or template_xml is NULL");
        return -1;
    }

    if (error_buf && error_buf_size > 0)
        error_buf[0] = '\0';

    /* ---- Walk every <field> block in the XML ---- */
    const char *cursor = template_xml;
    const char *OPEN  = "<field>";
    const char *CLOSE = "</field>";
    size_t      open_len  = strlen(OPEN);
    size_t      close_len = strlen(CLOSE);

    while ((cursor = strstr(cursor, OPEN)) != NULL)
    {
        const char *block_start = cursor;
        const char *block_end   = strstr(cursor + open_len, CLOSE);

        if (!block_end)
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "Malformed XML: <field> without </field> "
                         "near offset %td",
                         (ptrdiff_t)(cursor - template_xml));
            if (error_buf && error_buf_size > 0)
                snprintf(error_buf, error_buf_size,
                         "Malformed XML: unclosed <field> tag");
            return -1;
        }

        /* Extract the block content (including the tags) */
        size_t block_len = (size_t)(block_end - block_start) + close_len;
        char  *block     = malloc(block_len + 1);
        if (!block)
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "malloc failed for field block");
            return -1;
        }
        memcpy(block, block_start, block_len);
        block[block_len] = '\0';

        /* Parse the block */
        parsed_field_t pf;
        if (!parse_field_block(block, &pf))
        {
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "Failed to parse <field> block #%d",
                         field_count + 1);
            free(block);
            if (error_buf && error_buf_size > 0)
                snprintf(error_buf, error_buf_size,
                         "Failed to parse <field> block #%d",
                         field_count + 1);
            return -1;
        }
        free(block);

        field_count++;

        logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                     "Validating field %d: name='%s' type='%s' "
                     "len=%d prec=%d scale=%d null='%s' value='%s'",
                     pf.field_number,
                     pf.field_name,
                     pf.field_type,
                     pf.field_length,
                     pf.field_precision,
                     pf.field_scale,
                     pf.field_nullable,
                     pf.insert_value);

        char field_msg[512] = {0};
        field_validation_result_t result =
            validate_field(ctx, &pf, field_msg, sizeof(field_msg));

        if (result == FIELD_VALID)
        {
            logger_write(ctx->insert_logger, LOG_DEBUG, __func__, 0,
                         "Field '%s' VALID", pf.field_name);
        }
        else
        {
            failures++;
            logger_write(ctx->insert_logger, LOG_ERROR, __func__, 0,
                         "Field '%s' FAILED (result=%d): %s",
                         pf.field_name, result, field_msg);

            /* Capture first failure into caller buffer */
            if (failures == 1 && error_buf && error_buf_size > 0)
                snprintf(error_buf, error_buf_size, "%s", field_msg);
        }

        /* Advance cursor past this block */
        cursor = block_end + close_len;
    }

    if (field_count == 0)
    {
        logger_write(ctx->insert_logger, LOG_WARN, __func__, 0,
                     "No <field> blocks found in template XML - "
                     "nothing to validate");
    }

    logger_write(ctx->insert_logger, LOG_INFO, __func__, 0,
                 "validate_insert_template complete: "
                 "fields=%d failures=%d",
                 field_count, failures);

    return (failures > 0) ? -1 : 0;
}
