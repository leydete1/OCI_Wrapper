/*
 * OCI_Insert_Validate_Module.h
 *
 * Stage 2 - Insert Template Validator
 * -------------------------------------
 * Accepts a fully-formed <Insert Template> XML (produced by Stage 1
 * and then populated by the caller with <insert_value> entries) and
 * validates every value against its declared Oracle column metadata.
 *
 * Rules enforced per Oracle data type
 * ------------------------------------
 *   NUMBER / FLOAT / BINARY_FLOAT / BINARY_DOUBLE
 *       - Must be a valid numeric literal.
 *       - For NUMBER(p,s): integer part <= p-s digits,
 *         fractional part <= s digits.
 *
 *   CHAR / VARCHAR2 / NCHAR / NVARCHAR2
 *       - Length must not exceed field_length.
 *
 *   DATE
 *       - Must match YYYY-MM-DD or the NLS_DATE_FORMAT stored in ctx.
 *
 *   TIMESTAMP / TIMESTAMP WITH TIME ZONE / TIMESTAMP WITH LOCAL TIME ZONE
 *       - Must match YYYY-MM-DD HH24:MI:SS[.ffffff][timezone].
 *
 *   INTERVAL YEAR TO MONTH
 *       - Format: [+/-]YY-MM  (leading precision flexible).
 *
 *   INTERVAL DAY TO SECOND
 *       - Format: [+/-]DD HH:MI:SS[.ffffff].
 *
 *   RAW
 *       - Must be a hex string; byte count <= field_length.
 *
 *   CLOB / NCLOB
 *       - Any text accepted (no length cap at template level).
 *
 *   BLOB
 *       - Must be a valid file path string (Stage 3 reads the file).
 *
 *   ROWID / UROWID
 *       - Extended ROWID format validated (18-char base-64 Oracle set).
 *
 * Nullable check
 *   - If field_nullable == 'N' and insert_value is empty, an error is
 *     raised UNLESS field_default is non-empty (Oracle will supply it).
 *
 * Return value
 *   validate_insert_template() returns 0 on full success.
 *   On the first validation failure it returns -1 and writes a
 *   human-readable error description into the caller-supplied
 *   error_buf / error_buf_size.  All events are also logged.
 */

#ifndef OCI_INSERT_VALIDATE_MODULE_H
#define OCI_INSERT_VALIDATE_MODULE_H

#include "OCI_Connection.h"
#include "XML_Helper.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Per-field validation result (used internally and exposed for       */
/*  callers that want a full report rather than fail-fast behaviour)   */
/* ------------------------------------------------------------------ */
typedef enum {
    FIELD_VALID   = 0,
    FIELD_NULL_VIOLATION,       /* NOT NULL column, empty value, no default */
    FIELD_TYPE_MISMATCH,        /* Value cannot be coerced to declared type */
    FIELD_LENGTH_EXCEEDED,      /* String / RAW value too long              */
    FIELD_PRECISION_EXCEEDED,   /* Numeric integer-part overflow            */
    FIELD_SCALE_EXCEEDED,       /* Numeric fractional-part overflow         */
    FIELD_FORMAT_INVALID,       /* Date / Timestamp / Interval bad format   */
    FIELD_ROWID_INVALID,        /* ROWID / UROWID format check failed       */
    FIELD_HEX_INVALID,          /* RAW value is not valid hexadecimal       */

    /* ---- Added for Level 2 (level2_validate_insert() in
     * OCI_Level2_Parser.c) - these two situations could never arise
     * under the old model, where the client always echoed back
     * field_type/field_length/field_nullable/etc. for every column
     * itself. Now that field_value_t is just field_name+value and the
     * real metadata comes from metadata_cache instead, two new
     * failure modes are possible that validate_field() itself has no
     * way to detect (it only ever sees fields the caller already
     * matched to a real column) - the caller building parsed_field_t
     * from metadata_cache results is responsible for catching these
     * before ever calling validate_field():
     *   - the client named a column that doesn't exist on the table
     *   - the client omitted a NOT NULL column with no default        */
    FIELD_UNKNOWN_COLUMN,       /* field_name matches no real column        */
    FIELD_MISSING_REQUIRED_COLUMN /* NOT NULL column, no default, omitted   */
} field_validation_result_t;

/* ------------------------------------------------------------------ */
/*  parsed_field_t                                                      */
/*  One field's value plus the declared Oracle column metadata it's     */
/*  validated against. Originally private to this .c file, built only   */
/*  by parse_field_block() from the old <Insert Template> XML - now      */
/*  exposed here so level2_validate_insert() (OCI_Level2_Parser.c) can   */
/*  build one directly from insert_request_t's field_value_t plus a      */
/*  metadata_cache lookup, and call validate_field() with it, reusing    */
/*  the exact same type-validation rules documented above rather than    */
/*  re-implementing them against the new source of metadata. Update      */
/*  will do the same once its own Level 2 validator is built.            */
/* ------------------------------------------------------------------ */
typedef struct {
    int  field_number;
    char field_name    [128];
    char field_type    [128];
    int  field_length;
    int  field_precision;
    int  field_scale;
    char field_nullable[4];
    char field_default [512];
    char insert_value  [1024];
} parsed_field_t;

/*
 * validate_field()
 *
 * Dispatches to the correct type-specific validator (validate_numeric,
 * validate_string, validate_date, etc.) based on f->field_type, after
 * the nullable check. Same rules documented in this header's own
 * top-level comment block - this is the single place both
 * validate_insert_template() (below, XML-string path) and
 * level2_validate_insert() (OCI_Level2_Parser.c, insert_request_t
 * path) ultimately call.
 *
 * Does NOT check for an unknown column name or a missing required
 * column - see FIELD_UNKNOWN_COLUMN/FIELD_MISSING_REQUIRED_COLUMN
 * above. Callers building parsed_field_t from a source where those are
 * possible (i.e. metadata_cache, not client-echoed metadata) must
 * check for both themselves before calling this.
 */
field_validation_result_t validate_field(oci_context_t        *ctx,
                                          const parsed_field_t *f,
                                          char                 *msg,
                                          size_t                msg_max);

typedef struct {
    int                       field_number;           /* 1-based          */
    char                      field_name[128];
    char                      field_type[128];
    char                      insert_value[1024];
    field_validation_result_t result;
    char                      message[256];           /* human description*/
} field_report_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/*
 * validate_insert_template()
 *
 * Parse the <Insert Template> XML produced by get_insert_template()
 * (and populated with <insert_value> entries by the caller), then
 * validate every field value against its declared Oracle metadata.
 *
 * Parameters
 *   ctx           - OCI context (for logger)
 *   template_xml  - NULL-terminated XML string to validate
 *   error_buf     - caller-supplied buffer for first error message
 *   error_buf_size- size of error_buf in bytes
 *
 * Returns
 *   0             - all fields valid
 *  -1             - one or more validation failures
 *                   (first failure written to error_buf)
 *
 * All field results (pass and fail) are written to the log at
 * LOG_DEBUG; failures are also written at LOG_ERROR.
 */
int validate_insert_template(oci_context_t *ctx,
                              const char    *template_xml,
                              char          *error_buf,
                              size_t         error_buf_size);

#endif /* OCI_INSERT_VALIDATE_MODULE_H */
