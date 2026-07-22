/*
 * OCI_Audit_Trail_Manager.c
 *
 * Audit Trail Manager Module - Stage 1
 * --------------------------------------
 * Implements audit_trail_insert() which records one AUDIT_TRAIL row for
 * every (business row, column) pair produced by an INSERT, UPDATE, or
 * DELETE operation executed through the Data_Manager application layer.
 *
 * Stage 1 scope
 * -------------
 *   - One AUDIT_TRAIL row per field per business row (bulk insert)
 *   - ACTION_TYPE = INSERT:  OLD_VALUE = NULL, NEW_VALUE = field value
 *   - ACTION_TYPE = UPDATE:  OLD_VALUE = old,  NEW_VALUE = new value
 *   - ACTION_TYPE = DELETE:  OLD_VALUE = field value, NEW_VALUE = NULL
 *   - ROW_HASH: FNV-1a 64-bit placeholder (full SHA-256 in Stage 2)
 *   - Cycle-guard: audit_trail_in_progress flag prevents re-entry
 *   - Same OCI session / same transaction as the business DML
 *   - Audit logger: ctx->audit_logger (all logging goes here)
 *
 * Stage 2 additions (not in this file)
 * -------------------------------------
 *   - SHA-256 ROW_HASH via OpenSSL EVP
 *   - E-signature fields (ESIG_ID, ESIG_MEANING, ESIG_TIMESTAMP)
 *   - CLIENT_IP populated from Session Manager
 *   - Async audit flush path for high-throughput scenarios
 *
 * Cycle-guard design
 * ------------------
 * audit_trail_insert() calls execute_insert_batch() to write the audit
 * rows.  execute_insert_batch() would normally call audit_trail_insert()
 * after a successful business insert.  Without a guard this creates
 * infinite mutual recursion.
 *
 * The guard is a plain file-scope int:
 *
 *   int audit_trail_in_progress = 0;   (defined here - extern in .h)
 *
 * Flow:
 *   execute_insert_batch()          <- business insert
 *     if (!audit_trail_in_progress)
 *       audit_trail_insert()        <- sets flag = 1
 *         execute_insert_batch()    <- audit insert
 *           audit_trail_in_progress == 1 -> SKIP audit call
 *         returns                   <- clears flag = 0
 *
 * Because Data_Manager uses one oci_context_t per thread the plain int
 * is sufficient.  If multi-threaded use is introduced later, replace
 * with __thread int (thread-local storage) or a per-context field.
 *
 * XML Template strategy
 * ---------------------
 * audit_trail_insert() builds a complete <Insert_Template> XML string
 * in memory (one row per field per business row) and passes it to
 * execute_insert_batch().  This re-uses all existing validation,
 * binding, LOB handling, and metrics infrastructure without duplicating
 * any OCI code.
 *
 * Memory note: the XML buffer is heap-allocated and sized dynamically
 * based on row_count * col_count.  Each audit row has a fixed set of
 * scalar columns plus two CLOB columns (OLD_VALUE, NEW_VALUE).
 * Maximum estimated size per audit row: ~4 KB.
 *
 * See OCI_Audit_Trail_Manager.h for the public API and integration guide.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>

#include "OCI_Audit_Trail_Manager.h"
#include "OCI_Insert_Execute_Module.h"
#include "OCI_Execute_Query_Batch_Module.h"
#include "OCI_Transaction_Manager.h"
#include "OCI_Table_Metadata_Module.h"   /* col_metadata_t, field_value_t */
#include "logger.h"
#include "metrics.h"

/* ------------------------------------------------------------------ */
/*  Internal: field_value_t mirror                                      */
/*  OCI_Insert_Execute_Module defines this internally but we need it   */
/*  here to cast the void* arrays in audit_trail_request_t.            */
/*  Keep in sync with the definition in OCI_Insert_Execute_Module.c.  */
/* ------------------------------------------------------------------ */
#define AUDIT_MAX_COL_VALUE  32768

typedef struct {
    char value[AUDIT_MAX_COL_VALUE];
    int  is_empty;
} audit_field_value_t;

/* ------------------------------------------------------------------ */
/*  Cycle-guard flag - defined here, declared extern in the header     */
/* ------------------------------------------------------------------ */
int audit_trail_in_progress = 0;

/* ------------------------------------------------------------------ */
/*  Internal limits                                                     */
/* ------------------------------------------------------------------ */
/* Estimated bytes per audit row in the XML template:
 *   ~2 KB for fixed scalar fields + up to AUDIT_MAX_COL_VALUE for
 *   OLD_VALUE and NEW_VALUE CLOBs.  We add a safety margin.          */
#define AUDIT_XML_BYTES_PER_ROW  (AUDIT_MAX_COL_VALUE * 2 + 4096)
#define AUDIT_XML_HEADER_BYTES   2048   /* template header + footer    */

/* ------------------------------------------------------------------ */
/*  Internal helpers - forward declarations                             */
/* ------------------------------------------------------------------ */
static uint64_t audit_fnv1a_64(const char *data, size_t len);
static void     audit_xml_escape(const char *src,
                                  char       *dest,
                                  size_t      dest_max);
static int      audit_append(char  **buf,
                              size_t *used,
                              size_t *capacity,
                              const char *fmt, ...);

/* ================================================================== */
/*  audit_trail_build_row_hash                                          */
/*  Stage 1: FNV-1a 64-bit hash of concatenated key fields.           */
/*  Stage 2: replace body with SHA-256 (keep signature).              */
/* ================================================================== */
char *audit_trail_build_row_hash(const char *table_name,
                                  const char *record_id,
                                  const char *field_name,
                                  const char *action_type,
                                  const char *new_value,
                                  const char *changed_by,
                                  const char *transaction_id,
                                  char       *dest,
                                  size_t      dest_max)
{
    if (!dest || dest_max < 17) return dest;

    /* Concatenate all key fields with a separator that cannot appear  */
    /* in any individual field value                                    */
    char concat[4096];
    snprintf(concat, sizeof(concat), "%s|%s|%s|%s|%s|%s|%s",
             table_name   ? table_name   : "",
             record_id    ? record_id    : "",
             field_name   ? field_name   : "",
             action_type  ? action_type  : "",
             new_value    ? new_value    : "",
             changed_by   ? changed_by   : "",
             transaction_id ? transaction_id : "");

    uint64_t hash = audit_fnv1a_64(concat, strlen(concat));

    snprintf(dest, dest_max, "%016llx", (unsigned long long)hash);
    return dest;
}

/* ================================================================== */
/*  audit_trail_insert                                                  */
/*  Main public entry point.                                           */
/* ================================================================== */
int audit_trail_insert(oci_context_t         *ctx,
                       audit_trail_request_t *atr)
{
    int rc = 0;

    /* ----------------------------------------------------------------
     * Guard: already inside an audit insert - do nothing.
     * This is the key cycle-break that prevents infinite recursion
     * when execute_insert_batch() is called below.
     * ---------------------------------------------------------------- */
    if (audit_trail_in_progress)
    {
        logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                     "audit_trail_in_progress=1 - skipping recursive audit");
        return 0;
    }

    /* ---- Validate arguments ---- */
    if (!ctx || !atr)
    {
        if (ctx)
            logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                         "Invalid arguments: ctx or atr is NULL");
        return -1;
    }

    if (atr->col_count <= 0 || atr->row_count <= 0)
    {
        logger_write(ctx->audit_logger, LOG_WARN, __func__, 0,
                     "Nothing to audit: row_count=%d col_count=%d",
                     atr->row_count, atr->col_count);
        return 0;
    }

    if (atr->table_name[0] == '\0')
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "audit_trail_request_t.table_name is empty");
        return -1;
    }

    if (atr->change_reason[0] == '\0')
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "audit_trail_request_t.change_reason is empty "
                     "(mandatory field - AUDIT_TRAIL.CHANGE_REASON NOT NULL)");
        return -1;
    }

    /* ---- Warn if no active transaction ---- */
    if (!ctx->active_tx)
    {
        logger_write(ctx->audit_logger, LOG_WARN, __func__, 0,
                     "audit_trail_insert called with no active transaction - "
                     "audit rows may commit independently of business rows. "
                     "Wrap both in a tx_begin() / tx_commit() block.");
    }

    /* ================================================================
     *  INSERT always uses row-snapshot mode.
     *
     *  For INSERT there are no old values to record, so per-field rows
     *  add no information beyond what a single snapshot captures.
     *  One audit row per business row keeps volume proportional to the
     *  number of rows inserted regardless of column count, which is
     *  critical for bulk loads and migration jobs.
     *
     *  UPDATE and DELETE continue to fall through to the field-level
     *  path below because the before/after comparison per column is
     *  meaningful for those operations.
     * ================================================================ */
    if (strcasecmp(atr->action_type, "INSERT") == 0)
    {
        logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                     "INSERT detected - dispatching to snapshot path "
                     "(one audit row per business row) table='%s' rows=%d",
                     atr->table_name, atr->row_count);
        return audit_trail_insert_snapshot(ctx, atr);
    }

    /* ---- Resolve transaction / session IDs ---- */
    const char *tx_id      = ctx->active_tx
                             ? tx_get_id(ctx->active_tx)
                             : "-";
    const char *session_id = (atr->session_id[0] != '\0')
                             ? atr->session_id
                             : (ctx->active_tx &&
                                ctx->active_tx->session_id[0] != '\0'
                                ? ctx->active_tx->session_id
                                : "-");
    const char *client_ip  = (atr->client_ip[0] != '\0')
                             ? atr->client_ip
                             : "-";
    const char *changed_by = (atr->changed_by[0] != '\0')
                             ? atr->changed_by
                             : (ctx->ini ? ctx->ini->username : "Data_Manager");

    /* Total audit rows = row_count * col_count (UPDATE / DELETE path)  */
    int total_audit_rows = atr->row_count * atr->col_count;

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "Entering audit_trail_insert field-level path: "
                 "table='%s' action='%s' "
                 "business_rows=%d cols=%d total_audit_rows=%d "
                 "tx_id='%s'",
                 atr->table_name, atr->action_type,
                 atr->row_count, atr->col_count, total_audit_rows,
                 tx_id);

    /* ================================================================
     *  Build the <Insert_Template> XML for the AUDIT_TRAIL bulk insert.
     *
     *  One <row> element per (business_row, col) pair.
     *  Fixed columns (scalars): TABLE_NAME, RECORD_ID, FIELD_NAME,
     *    ACTION_TYPE, DATA_TYPE, CHANGED_BY, CHANGE_REASON,
     *    TRANSACTION_ID, SESSION_ID, CLIENT_IP, APPLICATION_NAME,
     *    MODULE_NAME, ROW_HASH.
     *  CLOB columns: OLD_VALUE, NEW_VALUE.
     *
     *  execute_insert_batch() handles CLOB columns via EMPTY_CLOB() +
     *  SELECT FOR UPDATE automatically when the field type is CLOB.
     *  We provide the value inline (it may be large but fits within
     *  MAX_COL_VALUE_SIZE = 32 KB per field).
     * ================================================================ */

    /* Estimate buffer size and allocate */
    size_t capacity = (size_t)total_audit_rows * AUDIT_XML_BYTES_PER_ROW
                    + AUDIT_XML_HEADER_BYTES;

    /* Minimum sensible buffer even for 0 rows (guards against tiny
     * edge-case allocations that still need the template skeleton)    */
    if (capacity < 8192) capacity = 8192;

    char  *xml_buf = malloc(capacity);
    if (!xml_buf)
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "malloc failed for audit XML buffer (%zu bytes)",
                     capacity);
        return -1;
    }

    size_t used = 0;

    /* ---- XML header ---- */
    rc = audit_append(&xml_buf, &used, &capacity,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Insert_Template>\n"
        "  <operation>INSERT</operation>\n"
        "  <table_name>%s</table_name>\n"
        "  <owner>%s</owner>\n",
        AUDIT_TABLE_NAME,
        AUDIT_OWNER);
    if (rc != 0) goto Cleanup;

    /* ---- One <row> per (business row * col) ---- */
    int xml_row_num = 0;

    /* Cast void* arrays to the internal field_value_t type            */
    const audit_field_value_t *new_vals =
        (const audit_field_value_t *)atr->new_values;
    const audit_field_value_t *old_vals =
        (const audit_field_value_t *)atr->old_values;

    for (int br = 0; br < atr->row_count; br++)
    {
        for (int c = 0; c < atr->col_count; c++)
        {
            xml_row_num++;

            /* ---- Resolve field name ---- */
            const char *field_name = (atr->col_names && atr->col_names[c][0])
                                     ? atr->col_names[c]
                                     : "UNKNOWN";

            /* ---- Resolve data type ---- */
            const char *data_type = "-";
            if (atr->col_types)
                data_type = atr->col_types[c].data_type[0]
                            ? atr->col_types[c].data_type
                            : "-";

            /* ---- Resolve OLD_VALUE and NEW_VALUE ---- */
            char old_escaped[AUDIT_MAX_COL_VALUE + 64];
            char new_escaped[AUDIT_MAX_COL_VALUE + 64];

            memset(old_escaped, 0, sizeof(old_escaped));
            memset(new_escaped, 0, sizeof(new_escaped));

            if (old_vals)
            {
                const audit_field_value_t *fv =
                    &old_vals[br * atr->col_count + c];
                if (!fv->is_empty && fv->value[0] != '\0')
                    audit_xml_escape(fv->value,
                                     old_escaped, sizeof(old_escaped));
            }

            if (new_vals)
            {
                const audit_field_value_t *fv =
                    &new_vals[br * atr->col_count + c];
                if (!fv->is_empty && fv->value[0] != '\0')
                    audit_xml_escape(fv->value,
                                     new_escaped, sizeof(new_escaped));
            }

            /* ---- Build ROW_HASH ---- */
            char row_hash[64] = {0};
            audit_trail_build_row_hash(
                atr->table_name,
                atr->record_id,
                field_name,
                atr->action_type,
                new_escaped[0] ? new_escaped : "",
                changed_by,
                tx_id,
                row_hash,
                sizeof(row_hash));

            logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                         "Audit row %d: table='%s' record='%s' "
                         "field='%s' action='%s' hash='%s'",
                         xml_row_num,
                         atr->table_name, atr->record_id,
                         field_name, atr->action_type, row_hash);

            /* ---- Emit <row> XML ---- */
            /*
             * Column order must match the AUDIT_TRAIL table definition.
             * Columns with Oracle defaults (AUDIT_ID, CHANGED_BY_DB_USER,
             * CHANGE_TIMESTAMP, PARTITION_DATE) are intentionally omitted
             * so Oracle assigns them automatically.
             * ESIG_* columns are also omitted (Stage 2).
             *
             * Each <field> block follows the Insert_Template schema
             * used throughout the project.
             */
            rc = audit_append(&xml_buf, &used, &capacity,
                "  <row number=\"%d\">\n"

                /* TABLE_NAME - VARCHAR2(128) NOT NULL */
                "    <field>\n"
                "      <field_number>1</field_number>\n"
                "      <field_name>TABLE_NAME</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>128</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>N</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n"

                /* RECORD_ID - VARCHAR2(255) NOT NULL */
                "    <field>\n"
                "      <field_number>2</field_number>\n"
                "      <field_name>RECORD_ID</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>255</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>N</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n"

                /* FIELD_NAME - VARCHAR2(128) NOT NULL */
                "    <field>\n"
                "      <field_number>3</field_number>\n"
                "      <field_name>FIELD_NAME</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>128</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>N</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n"

                /* ACTION_TYPE - VARCHAR2(10) NOT NULL */
                "    <field>\n"
                "      <field_number>4</field_number>\n"
                "      <field_name>ACTION_TYPE</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>10</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>N</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                xml_row_num,
                atr->table_name,
                atr->record_id[0] ? atr->record_id : "-",
                field_name,
                atr->action_type);
            if (rc != 0) goto Cleanup;

            /* OLD_VALUE - CLOB (nullable) */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>5</field_number>\n"
                "      <field_name>OLD_VALUE</field_name>\n"
                "      <field_type>CLOB</field_type>\n"
                "      <field_length>4000</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>Y</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                old_escaped);
            if (rc != 0) goto Cleanup;

            /* NEW_VALUE - CLOB (nullable) */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>6</field_number>\n"
                "      <field_name>NEW_VALUE</field_name>\n"
                "      <field_type>CLOB</field_type>\n"
                "      <field_length>4000</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>Y</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                new_escaped);
            if (rc != 0) goto Cleanup;

            /* DATA_TYPE - VARCHAR2(50) nullable */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>7</field_number>\n"
                "      <field_name>DATA_TYPE</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>50</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>Y</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                data_type);
            if (rc != 0) goto Cleanup;

            /* CHANGED_BY - VARCHAR2(100) NOT NULL */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>8</field_number>\n"
                "      <field_name>CHANGED_BY</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>100</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>N</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                changed_by);
            if (rc != 0) goto Cleanup;

            /* CHANGE_REASON - VARCHAR2(500) NOT NULL */
            char reason_escaped[1024];
            memset(reason_escaped, 0, sizeof(reason_escaped));
            audit_xml_escape(atr->change_reason,
                             reason_escaped, sizeof(reason_escaped));

            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>9</field_number>\n"
                "      <field_name>CHANGE_REASON</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>500</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>N</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                reason_escaped);
            if (rc != 0) goto Cleanup;

            /* TRANSACTION_ID - VARCHAR2(100) NOT NULL */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>10</field_number>\n"
                "      <field_name>TRANSACTION_ID</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>100</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>N</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                tx_id);
            if (rc != 0) goto Cleanup;

            /* SESSION_ID - VARCHAR2(100) nullable */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>11</field_number>\n"
                "      <field_name>SESSION_ID</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>100</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>Y</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                session_id);
            if (rc != 0) goto Cleanup;

            /* CLIENT_IP - VARCHAR2(45) nullable */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>12</field_number>\n"
                "      <field_name>CLIENT_IP</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>45</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>Y</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                client_ip);
            if (rc != 0) goto Cleanup;

            /* APPLICATION_NAME - VARCHAR2(100) nullable */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>13</field_number>\n"
                "      <field_name>APPLICATION_NAME</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>100</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>Y</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                AUDIT_APPLICATION_NAME);
            if (rc != 0) goto Cleanup;

            /* MODULE_NAME - VARCHAR2(100) nullable */
            char module_escaped[256];
            memset(module_escaped, 0, sizeof(module_escaped));
            audit_xml_escape(atr->module_name[0]
                             ? atr->module_name : "Data_Manager",
                             module_escaped, sizeof(module_escaped));

            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>14</field_number>\n"
                "      <field_name>MODULE_NAME</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>100</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>Y</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n",
                module_escaped);
            if (rc != 0) goto Cleanup;

            /* ROW_HASH - VARCHAR2(64) NOT NULL */
            rc = audit_append(&xml_buf, &used, &capacity,
                "    <field>\n"
                "      <field_number>15</field_number>\n"
                "      <field_name>ROW_HASH</field_name>\n"
                "      <field_type>VARCHAR2</field_type>\n"
                "      <field_length>64</field_length>\n"
                "      <field_precision>-1</field_precision>\n"
                "      <field_scale>-1</field_scale>\n"
                "      <field_nullable>N</field_nullable>\n"
                "      <field_default></field_default>\n"
                "      <insert_value>%s</insert_value>\n"
                "    </field>\n"
                "  </row>\n",
                row_hash);
            if (rc != 0) goto Cleanup;

        } /* end for each column */
    } /* end for each business row */

    /* ---- XML footer ---- */
    rc = audit_append(&xml_buf, &used, &capacity,
        "  <column_count>15</column_count>\n"
        "</Insert_Template>\n");
    if (rc != 0) goto Cleanup;

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "Audit XML built: %zu bytes for %d audit row(s)",
                 used, xml_row_num);
    logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                 "Audit XML:\n%s", xml_buf);

    /* ================================================================
     *  Execute the bulk audit insert via execute_insert_batch().
     *  The cycle-guard flag is set here and cleared in Cleanup so
     *  execute_insert_batch() will not re-enter audit_trail_insert().
     * ================================================================ */
    audit_trail_in_progress = 1;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)"AUDIT_TRAIL_MANAGER";

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "Calling execute_insert_batch for AUDIT_TRAIL "
                 "(%d row(s)) tx_id='%s'",
                 xml_row_num, tx_id);

    rc = execute_insert_batch(ctx, xml_buf, &cfg);

    if (rc == 0)
    {
        logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                     "Audit insert OK: %d row(s) written to AUDIT_TRAIL",
                     xml_row_num);
    }
    else
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "Audit insert FAILED rc=%d for table='%s' action='%s' "
                     "tx_id='%s' - audit trail is incomplete",
                     rc, atr->table_name, atr->action_type, tx_id);
    }

    /* Free execute_config OUTPUT_XML - we do not need the result XML  */
    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
        cfg.xml = NULL;
    }

Cleanup:
    /* Always clear the cycle-guard regardless of success or failure   */
    audit_trail_in_progress = 0;

    if (xml_buf)
    {
        free(xml_buf);
        xml_buf = NULL;
    }

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "audit_trail_insert complete rc=%d", rc);
    return rc;
}

/* ================================================================== */
/*  Internal: audit_fnv1a_64                                            */
/*  FNV-1a 64-bit hash.  Used for Stage 1 ROW_HASH placeholder.       */
/*  Replace with SHA-256 in Stage 2.                                   */
/* ================================================================== */
static uint64_t audit_fnv1a_64(const char *data, size_t len)
{
    uint64_t hash   = 14695981039346656037ULL; /* FNV offset basis     */
    uint64_t prime  = 1099511628211ULL;        /* FNV prime            */

    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= (uint64_t)p[i];
        hash *= prime;
    }
    return hash;
}

/* ================================================================== */
/*  Internal: audit_xml_escape                                          */
/*  Escape a string for safe embedding inside an XML element value.    */
/*  Replaces: & < > " '                                               */
/*  Newlines and carriage returns are replaced with a space to keep    */
/*  the Insert_Template XML parseable by extract_tag_ins().            */
/* ================================================================== */
static void audit_xml_escape(const char *src,
                               char       *dest,
                               size_t      dest_max)
{
    if (!src || !dest || dest_max == 0) return;

    size_t wi = 0;

    for (const char *p = src; *p && wi < dest_max - 1; p++)
    {
        switch (*p)
        {
            case '&':
                if (wi + 5 >= dest_max) goto Done;
                memcpy(dest + wi, "&amp;", 5); wi += 5;
                break;
            case '<':
                if (wi + 4 >= dest_max) goto Done;
                memcpy(dest + wi, "&lt;",  4); wi += 4;
                break;
            case '>':
                if (wi + 4 >= dest_max) goto Done;
                memcpy(dest + wi, "&gt;",  4); wi += 4;
                break;
            case '"':
                if (wi + 6 >= dest_max) goto Done;
                memcpy(dest + wi, "&quot;",6); wi += 6;
                break;
            case '\'':
                if (wi + 6 >= dest_max) goto Done;
                memcpy(dest + wi, "&apos;",6); wi += 6;
                break;
            case '\n':
            case '\r':
                /* Replace with space - keeps template XML single-line  */
                if (wi + 1 >= dest_max) goto Done;
                dest[wi++] = ' ';
                break;
            default:
                dest[wi++] = *p;
                break;
        }
    }

Done:
    dest[wi] = '\0';
}

/* ================================================================== */
/*  Internal: audit_append                                              */
/*  Printf into a dynamically grown heap buffer.                       */
/*  Doubles capacity when the buffer is too small.                     */
/*  Sets *buf = NULL and returns -1 on allocation failure.            */
/* ================================================================== */
#include <stdarg.h>

static int audit_append(char  **buf,
                         size_t *used,
                         size_t *capacity,
                         const char *fmt, ...)
{
    if (!buf || !*buf || !used || !capacity) return -1;

    for (;;)
    {
        size_t remaining = *capacity - *used;

        va_list args;
        va_start(args, fmt);
        int needed = vsnprintf(*buf + *used, remaining, fmt, args);
        va_end(args);

        if (needed < 0)
            return -1;  /* encoding error */

        if ((size_t)needed < remaining)
        {
            *used += (size_t)needed;
            return 0;   /* success */
        }

        /* Buffer too small - grow to at least needed + current used   */
        size_t new_cap = *capacity * 2;
        size_t min_cap = *used + (size_t)needed + 1;
        if (new_cap < min_cap) new_cap = min_cap;

        char *new_buf = realloc(*buf, new_cap);
        if (!new_buf)
        {
            free(*buf);
            *buf = NULL;
            return -1;
        }
        *buf      = new_buf;
        *capacity = new_cap;
        /* retry vsnprintf with the larger buffer */
    }
}

/* ================================================================== */
/*  audit_trail_serialise_row                                           */
/*  Serialise one business row into "COL=VALUE|COL=VALUE|..." format.  */
/*  NULL / empty fields are written as COL=<NULL>.                     */
/*  Values containing '|' or '=' are double-quoted.                    */
/* ================================================================== */
char *audit_trail_serialise_row(char          (*col_names)[128],
                                 const void    *values,
                                 int            col_count,
                                 int            row_idx,
                                 char          *dest,
                                 size_t         dest_max)
{
    if (!col_names || !dest || dest_max < 2) return NULL;

    const audit_field_value_t *fv_arr =
        (const audit_field_value_t *)values;

    size_t wi = 0;
    dest[0]   = '\0';

    for (int c = 0; c < col_count; c++)
    {
        /* Separator between fields */
        if (c > 0)
        {
            if (wi + 1 >= dest_max) break;
            dest[wi++] = '|';
        }

        /* Column name */
        const char *col = col_names[c][0] ? col_names[c] : "UNKNOWN";
        size_t clen = strlen(col);
        if (wi + clen + 1 >= dest_max) break;
        memcpy(dest + wi, col, clen);
        wi += clen;
        dest[wi++] = '=';

        /* Value */
        const char *val    = "<NULL>";
        int         is_null = 1;

        if (fv_arr)
        {
            const audit_field_value_t *fv =
                &fv_arr[row_idx * col_count + c];
            if (!fv->is_empty && fv->value[0] != '\0')
            {
                val     = fv->value;
                is_null = 0;
            }
        }

        if (is_null)
        {
            /* Write <NULL> literal */
            if (wi + 6 >= dest_max) break;
            memcpy(dest + wi, "<NULL>", 6);
            wi += 6;
        }
        else
        {
            /* Quote value if it contains pipe or equals characters
             * so the serialised string can be parsed unambiguously.  */
            int needs_quote = (strchr(val, '|') || strchr(val, '='));

            if (needs_quote)
            {
                if (wi + 1 >= dest_max) break;
                dest[wi++] = '"';
            }

            /* Copy value with XML escaping for safe CLOB storage     */
            char escaped[AUDIT_MAX_COL_VALUE + 64];
            audit_xml_escape(val, escaped, sizeof(escaped));

            size_t elen = strlen(escaped);
            if (wi + elen >= dest_max)
                elen = dest_max - wi - (needs_quote ? 2 : 1);
            memcpy(dest + wi, escaped, elen);
            wi += elen;

            if (needs_quote)
            {
                if (wi + 1 >= dest_max) { dest[wi] = '\0'; return dest; }
                dest[wi++] = '"';
            }
        }
    }

    dest[wi] = '\0';
    return dest;
}

/* ================================================================== */
/*  audit_trail_insert_snapshot                                         */
/*  One AUDIT_TRAIL row per business row.                              */
/*  All field values serialised into NEW_VALUE CLOB.                  */
/*  FIELD_NAME = "__ROW_SNAPSHOT__" sentinel.                          */
/* ================================================================== */
int audit_trail_insert_snapshot(oci_context_t         *ctx,
                                 audit_trail_request_t *atr)
{
    int rc = 0;

    /* Cycle-guard - same flag as audit_trail_insert()                 */
    if (audit_trail_in_progress)
    {
        logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                     "audit_trail_in_progress=1 - skipping recursive "
                     "snapshot audit");
        return 0;
    }

    if (!ctx || !atr)
    {
        if (ctx)
            logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                         "audit_trail_insert_snapshot: ctx or atr is NULL");
        return -1;
    }

    /* ---- Resolve IDs ---- */
    const char *tx_id = ctx->active_tx
                        ? tx_get_id(ctx->active_tx) : "-";

    const char *session_id = (atr->session_id[0] != '\0')
                             ? atr->session_id
                             : (ctx->active_tx &&
                                ctx->active_tx->session_id[0] != '\0'
                                ? ctx->active_tx->session_id : "-");

    const char *client_ip  = (atr->client_ip[0] != '\0')
                             ? atr->client_ip : "-";

    const char *changed_by = (atr->changed_by[0] != '\0')
                             ? atr->changed_by
                             : (ctx->ini ? ctx->ini->username
                                         : "Data_Manager");

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "audit_trail_insert_snapshot: table='%s' action='%s' "
                 "rows=%d cols=%d tx_id='%s'",
                 atr->table_name, atr->action_type,
                 atr->row_count, atr->col_count, tx_id);

    /* ================================================================
     *  Allocate snapshot serialisation buffer.
     *  Worst case: col_count * (128 name + 1 = + AUDIT_MAX_COL_VALUE)
     *  plus separators.  We use a generous fixed cap per row.
     * ================================================================ */
    size_t snap_cap = (size_t)atr->col_count *
                      (128 + 1 + AUDIT_MAX_COL_VALUE + 8) + 64;

    char *snap_buf = malloc(snap_cap);
    if (!snap_buf)
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "malloc failed for snapshot buffer (%zu bytes)",
                     snap_cap);
        return -1;
    }

    /* ================================================================
     *  Allocate XML template buffer.
     *  One <row> per business row; each row has 15 fields.
     *  Per-row estimate: snapshot CLOB size + 4 KB overhead.
     * ================================================================ */
    size_t capacity = (size_t)atr->row_count *
                      (snap_cap + 4096) + AUDIT_XML_HEADER_BYTES;
    if (capacity < 8192) capacity = 8192;

    char  *xml_buf = malloc(capacity);
    if (!xml_buf)
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "malloc failed for snapshot XML buffer (%zu bytes)",
                     capacity);
        free(snap_buf);
        return -1;
    }

    size_t used = 0;

    /* ---- XML header ---- */
    rc = audit_append(&xml_buf, &used, &capacity,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Insert_Template>\n"
        "  <operation>INSERT</operation>\n"
        "  <table_name>%s</table_name>\n"
        "  <owner>%s</owner>\n",
        AUDIT_TABLE_NAME,
        AUDIT_OWNER);
    if (rc != 0) goto Cleanup;

    /* ---- One <row> per business row ---- */
    for (int br = 0; br < atr->row_count; br++)
    {
        /* Serialise all columns for this row into snap_buf */
        memset(snap_buf, 0, snap_cap);
        if (!audit_trail_serialise_row(atr->col_names,
                                        atr->new_values,
                                        atr->col_count,
                                        br,
                                        snap_buf,
                                        snap_cap))
        {
            logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                         "audit_trail_serialise_row failed row=%d", br);
            rc = -1;
            goto Cleanup;
        }

        /* Build ROW_HASH across the entire snapshot string            */
        char row_hash[64] = {0};
        audit_trail_build_row_hash(
            atr->table_name,
            atr->record_id,
            AUDIT_SNAPSHOT_SENTINEL,
            atr->action_type,
            snap_buf,
            changed_by,
            tx_id,
            row_hash,
            sizeof(row_hash));

        logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                     "Snapshot row %d: table='%s' record='%s' "
                     "snap_len=%zu hash='%s'",
                     br + 1, atr->table_name, atr->record_id,
                     strlen(snap_buf), row_hash);

        /* Escape change_reason for XML */
        char reason_escaped[1024];
        memset(reason_escaped, 0, sizeof(reason_escaped));
        audit_xml_escape(atr->change_reason,
                         reason_escaped, sizeof(reason_escaped));

        /* Escape module_name for XML */
        char module_escaped[256];
        memset(module_escaped, 0, sizeof(module_escaped));
        audit_xml_escape(atr->module_name[0]
                         ? atr->module_name : "Data_Manager",
                         module_escaped, sizeof(module_escaped));

        /* ---- Emit <row> ---- */
        rc = audit_append(&xml_buf, &used, &capacity,
            "  <row number=\"%d\">\n"

            /* 1 TABLE_NAME */
            "    <field>\n"
            "      <field_number>1</field_number>\n"
            "      <field_name>TABLE_NAME</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>128</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>N</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n"

            /* 2 RECORD_ID */
            "    <field>\n"
            "      <field_number>2</field_number>\n"
            "      <field_name>RECORD_ID</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>255</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>N</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n"

            /* 3 FIELD_NAME - sentinel for snapshot rows */
            "    <field>\n"
            "      <field_number>3</field_number>\n"
            "      <field_name>FIELD_NAME</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>128</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>N</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n"

            /* 4 ACTION_TYPE */
            "    <field>\n"
            "      <field_number>4</field_number>\n"
            "      <field_name>ACTION_TYPE</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>10</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>N</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            br + 1,
            atr->table_name,
            atr->record_id[0] ? atr->record_id : "-",
            AUDIT_SNAPSHOT_SENTINEL,
            atr->action_type);
        if (rc != 0) goto Cleanup;

        /* 5 OLD_VALUE CLOB - always NULL for INSERT snapshot           */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>5</field_number>\n"
            "      <field_name>OLD_VALUE</field_name>\n"
            "      <field_type>CLOB</field_type>\n"
            "      <field_length>4000</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>Y</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value></insert_value>\n"
            "    </field>\n");
        if (rc != 0) goto Cleanup;

        /* 6 NEW_VALUE CLOB - the full row snapshot                     */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>6</field_number>\n"
            "      <field_name>NEW_VALUE</field_name>\n"
            "      <field_type>CLOB</field_type>\n"
            "      <field_length>4000</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>Y</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            snap_buf);
        if (rc != 0) goto Cleanup;

        /* 7 DATA_TYPE - "SNAPSHOT" to identify the record type clearly */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>7</field_number>\n"
            "      <field_name>DATA_TYPE</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>50</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>Y</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>SNAPSHOT</insert_value>\n"
            "    </field>\n");
        if (rc != 0) goto Cleanup;

        /* 8 CHANGED_BY */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>8</field_number>\n"
            "      <field_name>CHANGED_BY</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>100</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>N</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            changed_by);
        if (rc != 0) goto Cleanup;

        /* 9 CHANGE_REASON */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>9</field_number>\n"
            "      <field_name>CHANGE_REASON</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>500</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>N</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            reason_escaped);
        if (rc != 0) goto Cleanup;

        /* 10 TRANSACTION_ID */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>10</field_number>\n"
            "      <field_name>TRANSACTION_ID</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>100</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>N</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            tx_id);
        if (rc != 0) goto Cleanup;

        /* 11 SESSION_ID */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>11</field_number>\n"
            "      <field_name>SESSION_ID</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>100</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>Y</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            session_id);
        if (rc != 0) goto Cleanup;

        /* 12 CLIENT_IP */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>12</field_number>\n"
            "      <field_name>CLIENT_IP</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>45</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>Y</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            client_ip);
        if (rc != 0) goto Cleanup;

        /* 13 APPLICATION_NAME */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>13</field_number>\n"
            "      <field_name>APPLICATION_NAME</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>100</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>Y</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            AUDIT_APPLICATION_NAME);
        if (rc != 0) goto Cleanup;

        /* 14 MODULE_NAME */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>14</field_number>\n"
            "      <field_name>MODULE_NAME</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>100</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>Y</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n",
            module_escaped);
        if (rc != 0) goto Cleanup;

        /* 15 ROW_HASH */
        rc = audit_append(&xml_buf, &used, &capacity,
            "    <field>\n"
            "      <field_number>15</field_number>\n"
            "      <field_name>ROW_HASH</field_name>\n"
            "      <field_type>VARCHAR2</field_type>\n"
            "      <field_length>64</field_length>\n"
            "      <field_precision>-1</field_precision>\n"
            "      <field_scale>-1</field_scale>\n"
            "      <field_nullable>N</field_nullable>\n"
            "      <field_default></field_default>\n"
            "      <insert_value>%s</insert_value>\n"
            "    </field>\n"
            "  </row>\n",
            row_hash);
        if (rc != 0) goto Cleanup;

    } /* end for each business row */

    /* ---- XML footer ---- */
    rc = audit_append(&xml_buf, &used, &capacity,
        "  <column_count>15</column_count>\n"
        "</Insert_Template>\n");
    if (rc != 0) goto Cleanup;

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "Snapshot XML built: %zu bytes for %d audit row(s)",
                 used, atr->row_count);

    /* ---- Execute via execute_insert_batch() with cycle-guard ---- */
    audit_trail_in_progress = 1;

    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.input_file_name = (char *)"AUDIT_TRAIL_SNAPSHOT";

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "Calling execute_insert_batch for AUDIT_TRAIL snapshot "
                 "(%d row(s)) tx_id='%s'",
                 atr->row_count, tx_id);

    rc = execute_insert_batch(ctx, xml_buf, &cfg);

    if (rc == 0)
        logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                     "Snapshot audit insert OK: %d row(s) written "
                     "to AUDIT_TRAIL  table='%s'",
                     atr->row_count, atr->table_name);
    else
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "Snapshot audit insert FAILED rc=%d  table='%s' "
                     "tx_id='%s'",
                     rc, atr->table_name, tx_id);

    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }

Cleanup:
    audit_trail_in_progress = 0;

    if (snap_buf) { free(snap_buf); snap_buf = NULL; }
    if (xml_buf)  { free(xml_buf);  xml_buf  = NULL; }

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "audit_trail_insert_snapshot complete rc=%d", rc);
    return rc;
}

/* ================================================================== */
/*  Stage 2 - audit_trail_fetch_before_image                           */
/*  SELECT the current column values before the UPDATE executes.      */
/*  Uses execute_query_batch() to re-use all existing query           */
/*  infrastructure: metadata cache, LOB handling, metrics.            */
/* ================================================================== */
int audit_trail_fetch_before_image(oci_context_t  *ctx,
                                    const char     *table_name,
                                    const char     *owner,
                                    char          (*col_names)[128],
                                    int             col_count,
                                    char          (*key_names)[128],
                                    char          (*key_values)[32768],
                                    int             key_count,
                                    audit_old_value_t **old_values_out,
                                    int            *row_count_out)
{
    if (!ctx || !table_name || !col_names || !key_names ||
        !key_values || !old_values_out || !row_count_out)
    {
        logger_write(ctx ? ctx->audit_logger : NULL,
                     LOG_ERROR, __func__, 0,
                     "audit_trail_fetch_before_image: NULL argument");
        return -1;
    }

    *old_values_out = NULL;
    *row_count_out  = 0;

    /* ================================================================
     *  Build SELECT SQL
     *  SELECT col1, col2, ... FROM [owner.]table
     *  WHERE  key1 = 'val1' AND key2 = 'val2' ...
     *
     *  Values are embedded as literals (single-quoted, escaped) rather
     *  than bind variables because execute_query_batch() takes a plain
     *  SQL string.  The values come from the application layer and have
     *  already been validated by OCI_Insert_Validate_Module before
     *  reaching this point, so SQL injection risk is minimal in this
     *  controlled internal context.  Stage 3 can introduce bind
     *  variables if this path is exposed more broadly.
     * ================================================================ */
    char sql_buf[65536] = {0};
    size_t sql_used = 0;

    /* SELECT clause */
    sql_used += (size_t)snprintf(sql_buf + sql_used,
                                  sizeof(sql_buf) - sql_used,
                                  "SELECT ");

    for (int c = 0; c < col_count; c++)
    {
        if (c > 0)
            sql_used += (size_t)snprintf(sql_buf + sql_used,
                                          sizeof(sql_buf) - sql_used, ", ");
        sql_used += (size_t)snprintf(sql_buf + sql_used,
                                      sizeof(sql_buf) - sql_used,
                                      "%s", col_names[c]);
    }

    /* FROM clause */
    if (owner && owner[0])
        sql_used += (size_t)snprintf(sql_buf + sql_used,
                                      sizeof(sql_buf) - sql_used,
                                      " FROM %s.%s", owner, table_name);
    else
        sql_used += (size_t)snprintf(sql_buf + sql_used,
                                      sizeof(sql_buf) - sql_used,
                                      " FROM %s", table_name);

    /* WHERE clause */
    for (int k = 0; k < key_count; k++)
    {
        /* Escape single quotes in the key value */
        char escaped_val[65536] = {0};
        const char *src = key_values[k];
        size_t ei = 0;
        for (; *src && ei < sizeof(escaped_val) - 2; src++)
        {
            if (*src == '\'')
                escaped_val[ei++] = '\'';   /* double the quote */
            escaped_val[ei++] = *src;
        }
        escaped_val[ei] = '\0';

        sql_used += (size_t)snprintf(sql_buf + sql_used,
                                      sizeof(sql_buf) - sql_used,
                                      " %s %s = '%s'",
                                      k == 0 ? "WHERE" : "AND",
                                      key_names[k],
                                      escaped_val);
    }

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "Before-image SELECT: %s", sql_buf);

    /* ================================================================
     *  Execute SELECT via execute_query_batch()
     * ================================================================ */
    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.SQL              = sql_buf;
    cfg.input_file_name  = (char *)"AUDIT_BEFORE_IMAGE";
    cfg.fetch_array_size       = 100;

    int rc = execute_query_batch(ctx, &cfg);
    if (rc != 0)
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "Before-image SELECT failed rc=%d table='%s'",
                     rc, table_name);
        if (cfg.xml) { if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
                       free(cfg.xml); }
        return -1;
    }

    /* ================================================================
     *  Parse the OUTPUT_XML result set into old_values[]
     *
     *  execute_query_batch() returns XML in the form:
     *    <resultset>
     *      <row number="1">
     *        <COL1>value</COL1>
     *        <COL2>value</COL2>
     *      </row>
     *    </resultset>
     *
     *  We parse each <row> block and extract each column value.
     * ================================================================ */
    if (!cfg.xml || !cfg.xml->OUTPUT_XML)
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "Before-image SELECT returned no OUTPUT_XML");
        if (cfg.xml) free(cfg.xml);
        return -1;
    }

    const char *xml = cfg.xml->OUTPUT_XML;

    /* Count rows */
    int fetched_rows = 0;
    const char *rp = xml;
    while ((rp = strstr(rp, "<row ")) != NULL)
    { fetched_rows++; rp++; }

    if (fetched_rows == 0)
    {
        logger_write(ctx->audit_logger, LOG_WARN, __func__, 0,
                     "Before-image SELECT returned 0 rows for table='%s' "
                     "- row may have been deleted concurrently",
                     table_name);
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
        return -1;
    }

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "Before-image SELECT: fetched %d row(s) for %d column(s)",
                 fetched_rows, col_count);

    /* Allocate output array: [fetched_rows * col_count]               */
    audit_old_value_t *old_vals =
        calloc((size_t)(fetched_rows * col_count), sizeof(audit_old_value_t));
    if (!old_vals)
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for old_values[%d * %d]",
                     fetched_rows, col_count);
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
        return -1;
    }

    /* Parse each <row> block */
    int row_idx = 0;
    const char *row_ptr = xml;

    while ((row_ptr = strstr(row_ptr, "<row ")) != NULL && row_idx < fetched_rows)
    {
        const char *row_end = strstr(row_ptr, "</row>");
        if (!row_end) break;

        size_t  row_len = (size_t)(row_end - row_ptr) + 6;
        char   *row_buf = malloc(row_len + 1);
        if (!row_buf) { free(old_vals); rc = -1; goto FetchCleanup; }
        memcpy(row_buf, row_ptr, row_len);
        row_buf[row_len] = '\0';

        for (int c = 0; c < col_count; c++)
        {
            audit_old_value_t *ov = &old_vals[row_idx * col_count + c];
            ov->is_null = 1;   /* default to NULL */

            /* Build open/close tags for this column */
            char open_tag [160], close_tag[160];
            snprintf(open_tag,  sizeof(open_tag),  "<%s>",  col_names[c]);
            snprintf(close_tag, sizeof(close_tag), "</%s>", col_names[c]);

            const char *vs = strstr(row_buf, open_tag);
            if (vs)
            {
                vs += strlen(open_tag);
                const char *ve = strstr(vs, close_tag);
                if (ve)
                {
                    size_t vlen = (size_t)(ve - vs);
                    if (vlen >= sizeof(ov->value))
                        vlen = sizeof(ov->value) - 1;
                    memcpy(ov->value, vs, vlen);
                    ov->value[vlen] = '\0';
                    ov->is_null = (vlen == 0) ? 1 : 0;

                    logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                                 "Before-image row=%d col='%s' value='%.80s'%s",
                                 row_idx, col_names[c], ov->value,
                                 vlen > 80 ? "..." : "");
                }
            }
            else
            {
                logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                             "Before-image row=%d col='%s' tag not found "
                             "(NULL or column not in result)",
                             row_idx, col_names[c]);
            }
        }

        free(row_buf);
        row_idx++;
        row_ptr = row_end + 6;
    }

    *old_values_out = old_vals;
    *row_count_out  = row_idx;

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "audit_trail_fetch_before_image complete: "
                 "%d row(s) captured for table='%s'",
                 row_idx, table_name);

FetchCleanup:
    if (cfg.xml)
    {
        if (cfg.xml->OUTPUT_XML) free(cfg.xml->OUTPUT_XML);
        free(cfg.xml);
    }
    return rc;
}

/* ================================================================== */
/*  Stage 2 - audit_trail_insert_update                                */
/*  Write one AUDIT_TRAIL row per (updated row × changed column).     */
/*  Only columns where OLD_VALUE != NEW_VALUE produce an audit row.   */
/*  Uses the existing field-level path in audit_trail_insert() which  */
/*  is already wired to execute_insert_batch() with the cycle guard.  */
/* ================================================================== */
int audit_trail_insert_update(oci_context_t         *ctx,
                               audit_trail_request_t *atr,
                               audit_old_value_t     *old_values)
{
    if (!ctx || !atr || !old_values)
    {
        logger_write(ctx ? ctx->audit_logger : NULL,
                     LOG_ERROR, __func__, 0,
                     "audit_trail_insert_update: NULL argument");
        return -1;
    }

    /* Cast new_values to the internal field type */
    typedef struct { char value[32768]; int is_empty; } upd_fv_t;
    const upd_fv_t *new_vals = (const upd_fv_t *)atr->new_values;

    if (!new_vals)
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "audit_trail_insert_update: new_values is NULL");
        return -1;
    }

    const char *tx_id = ctx->active_tx
                        ? tx_get_id(ctx->active_tx) : "-";

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "audit_trail_insert_update: table='%s' rows=%d cols=%d "
                 "tx_id='%s'",
                 atr->table_name, atr->row_count,
                 atr->col_count, tx_id);

    /*
     * For each (row, col) pair compare old and new values.
     * Build a synthetic audit_trail_request_t containing only the
     * changed columns, then call audit_trail_insert() which routes
     * to the field-level XML builder for UPDATE/DELETE.
     *
     * We collect changed columns into parallel arrays per row and
     * call audit_trail_insert() once per row so each row's changed
     * columns are grouped by record in the audit trail.
     *
     * The field-level path in audit_trail_insert() expects:
     *   atr->col_names  [changed_count][128]
     *   atr->old_values : flat array [row_count=1 * changed_count]
     *                     of audit_old_value_t (which matches the
     *                     internal audit_field_value_t layout)
     *   atr->new_values : flat array [row_count=1 * changed_count]
     *                     of upd_field_value_t
     *   atr->col_count  = changed_count
     *   atr->row_count  = 1 (one row per call)
     */

    int total_rc       = 0;
    int total_audited  = 0;
    int total_skipped  = 0;

    /* Temp arrays for one row's changed columns */
    char changed_col_names [AUDIT_MAX_COLS][128];

    /* old/new value buffers matching the internal field_value_t layout */
    typedef struct { char value[32768]; int is_empty; } audit_fv_t;
    audit_fv_t *old_fv_buf =
        calloc((size_t)atr->col_count, sizeof(audit_fv_t));
    audit_fv_t *new_fv_buf =
        calloc((size_t)atr->col_count, sizeof(audit_fv_t));
    col_metadata_t *changed_types =
        calloc((size_t)atr->col_count, sizeof(col_metadata_t));

    if (!old_fv_buf || !new_fv_buf || !changed_types)
    {
        logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for update audit buffers");
        free(old_fv_buf);
        free(new_fv_buf);
        free(changed_types);
        return -1;
    }

    for (int br = 0; br < atr->row_count; br++)
    {
        int changed_count = 0;

        for (int c = 0; c < atr->col_count; c++)
        {
            const audit_old_value_t *ov = &old_values[br * atr->col_count + c];
            const upd_fv_t          *nv = &new_vals[br * atr->col_count + c];

            /* Resolve old value string */
            const char *old_str = ov->is_null  ? "" : ov->value;
            const char *new_str = nv->is_empty ? "" : nv->value;

            /* Skip unchanged columns */
            if (strcmp(old_str, new_str) == 0)
            {
                logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                             "row=%d col='%s' unchanged - skipping audit",
                             br, atr->col_names[c]);
                total_skipped++;
                continue;
            }

            logger_write(ctx->audit_logger, LOG_DEBUG, __func__, 0,
                         "row=%d col='%s' CHANGED: "
                         "old='%.40s' new='%.40s'",
                         br, atr->col_names[c],
                         old_str[0] ? old_str : "<NULL>",
                         new_str[0] ? new_str : "<NULL>");

            /* Add to changed set */
            strncpy(changed_col_names[changed_count], atr->col_names[c],
                    127);
            changed_col_names[changed_count][127] = '\0';

            /* old_fv_buf */
            memset(&old_fv_buf[changed_count], 0, sizeof(audit_fv_t));
            if (!ov->is_null && ov->value[0])
                strncpy(old_fv_buf[changed_count].value, ov->value,
                        sizeof(old_fv_buf[0].value) - 1);
            old_fv_buf[changed_count].is_empty = ov->is_null;

            /* new_fv_buf */
            memset(&new_fv_buf[changed_count], 0, sizeof(audit_fv_t));
            if (!nv->is_empty && nv->value[0])
                strncpy(new_fv_buf[changed_count].value, nv->value,
                        sizeof(new_fv_buf[0].value) - 1);
            new_fv_buf[changed_count].is_empty = nv->is_empty;

            /* Column type */
            if (atr->col_types)
                changed_types[changed_count] = atr->col_types[c];
            else
                memset(&changed_types[changed_count], 0,
                       sizeof(col_metadata_t));

            changed_count++;
        }

        if (changed_count == 0)
        {
            logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                         "row=%d: no column changes detected - "
                         "no audit rows written", br);
            continue;
        }

        logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                     "row=%d: %d changed column(s) to audit",
                     br, changed_count);

        /* Build a single-row audit request for this business row */
        audit_trail_request_t row_atr;
        memset(&row_atr, 0, sizeof(row_atr));

        strncpy(row_atr.table_name,   atr->table_name,
                sizeof(row_atr.table_name)   - 1);
        strncpy(row_atr.action_type,  "UPDATE",
                sizeof(row_atr.action_type)  - 1);
        strncpy(row_atr.record_id,    atr->record_id,
                sizeof(row_atr.record_id)    - 1);
        strncpy(row_atr.changed_by,   atr->changed_by,
                sizeof(row_atr.changed_by)   - 1);
        strncpy(row_atr.change_reason, atr->change_reason,
                sizeof(row_atr.change_reason) - 1);
        strncpy(row_atr.module_name,  atr->module_name,
                sizeof(row_atr.module_name)  - 1);
        strncpy(row_atr.session_id,   atr->session_id,
                sizeof(row_atr.session_id)   - 1);
        strncpy(row_atr.client_ip,    atr->client_ip,
                sizeof(row_atr.client_ip)    - 1);

        row_atr.col_names  = changed_col_names;
        row_atr.col_types  = changed_types;
        row_atr.old_values = old_fv_buf;    /* before-image values     */
        row_atr.new_values = new_fv_buf;    /* update SET values       */
        row_atr.col_count  = changed_count;
        row_atr.row_count  = 1;
        row_atr.audit_mode = AUDIT_MODE_FIELD;  /* field-level for UPDATE */

        int row_rc = audit_trail_insert(ctx, &row_atr);
        if (row_rc != 0)
        {
            logger_write(ctx->audit_logger, LOG_ERROR, __func__, 0,
                         "audit_trail_insert failed for UPDATE row=%d "
                         "table='%s' rc=%d",
                         br, atr->table_name, row_rc);
            total_rc = -1;
            /* Continue to next row - partial audit is better than none */
        }
        else
        {
            total_audited += changed_count;
        }
    }

    free(old_fv_buf);
    free(new_fv_buf);
    free(changed_types);

    logger_write(ctx->audit_logger, LOG_INFO, __func__, 0,
                 "audit_trail_insert_update complete: "
                 "audited=%d skipped(unchanged)=%d rc=%d "
                 "table='%s' tx_id='%s'",
                 total_audited, total_skipped, total_rc,
                 atr->table_name, tx_id);

    return total_rc;
}
