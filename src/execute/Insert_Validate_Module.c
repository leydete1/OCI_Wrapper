
/*
 * Insert_Validate_Module.c
 *
 * validate_field(): checks one value against its column's metadata
 * (type, length, precision/scale, nullability). Level 2 calls it for
 * every INSERT and UPDATE field (level2_validate_insert/_update, with
 * metadata from metadata_cache). The rules are listed in the header.
 * Makes no database calls.
 *
 * 2026-10-13 (fix-22 point 18): the Insert Template feature this file
 * started in (validate_insert_template() and its XML helpers) had no
 * callers and was removed, with Insert_Template_Module.c/.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#include "Insert_Validate_Module.h"
#include "db_type_class.h"   /* Stage 4b - type classes, not type names */
#include "logger.h"

/* parsed_field_t is now declared in OCI_Insert_Validate_Module.h -
 * exposed there so level2_validate_insert() can build one from
 * insert_request_t + metadata_cache and reuse validate_field()
 * directly, rather than this being a private detail of this file. */

/* ------------------------------------------------------------------ */
/*  Type-family helpers                                                 */
/*                                                                      */
/*  Stage 4b (Oracle dialect extraction, 2026-10-06): these used to     */
/*  compare Oracle type names directly (strcmp(t, "VARCHAR2") ...).     */
/*  They now ask db_type_class() / db_type_lob_kind(), so the same      */
/*  rules apply to any driver's type names. Names and call sites are    */
/*  unchanged; for every type name Oracle reports, each helper gives    */
/*  the same answer as before (proved by the Stage 4b differential      */
/*  test, proof/validate_diff.c).                                       */
/*                                                                      */
/*  Two deliberate details:                                             */
/*   - is_string_type() no longer matches ROWID/UROWID by accident:     */
/*     they have their own class (ROWID) and is_rowid_type() catches    */
/*     them, exactly as before.                                         */
/*   - is_lob_type() uses db_type_lob_kind(), not the LOB class, so     */
/*     BFILE stays out of it (BLOB, CLOB, NCLOB only - as before).      */
/* ------------------------------------------------------------------ */

static int is_numeric_type(const char *t)
{
    return db_type_class(t) == DB_TYPE_CLASS_NUMERIC;
}

static int is_string_type(const char *t)
{
    return db_type_class(t) == DB_TYPE_CLASS_STRING;
}

static int is_lob_type(const char *t)
{
    return db_type_lob_kind(t) != DB_LOB_NONE;
}

static int is_date_type(const char *t)
{
    return db_type_class(t) == DB_TYPE_CLASS_DATE;
}

static int is_timestamp_type(const char *t)
{
    return db_type_class(t) == DB_TYPE_CLASS_TIMESTAMP;
}

static int is_interval_ym_type(const char *t)
{
    return db_type_class(t) == DB_TYPE_CLASS_INTERVAL_YM;
}

static int is_interval_ds_type(const char *t)
{
    return db_type_class(t) == DB_TYPE_CLASS_INTERVAL_DS;
}

static int is_raw_type(const char *t)
{
    return db_type_class(t) == DB_TYPE_CLASS_BINARY;
}

static int is_rowid_type(const char *t)
{
    return db_type_class(t) == DB_TYPE_CLASS_ROWID;
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
     * consist only of valid Oracle base-64 characters.
     * Stage 4b note: the "ROWID" name test below stays - it separates
     * Oracle's two row-address types inside the ROWID class, and this
     * function is only reached for that class.                       */
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

