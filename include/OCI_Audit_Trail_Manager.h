/*
 * OCI_Audit_Trail_Manager.h
 *
 * Audit Trail Manager Module - Stage 1
 * --------------------------------------
 * Provides a single public entry point that records one audit row per
 * field for any INSERT, UPDATE, or DELETE operation performed by the
 * Data_Manager application layer.
 *
 * Design overview
 * ---------------
 * The audit trail is written using execute_insert_batch() - the same
 * bulk-insert engine used by all other DML in the project.  This means
 * the audit rows share the caller's OCI session and, critically, the
 * caller's ACTIVE TRANSACTION (ctx->active_tx).  No separate commit is
 * issued here: the audit rows and the business rows commit or roll back
 * together atomically.
 *
 * Cycle-guard
 * -----------
 * execute_insert_batch() calls audit_trail_insert() after every
 * business INSERT.  If audit_trail_insert() itself called
 * execute_insert_batch() without a guard, the result would be infinite
 * mutual recursion.  The guard is a single thread-local flag:
 *
 *   audit_trail_in_progress (file-scope, zero-initialised)
 *
 * audit_trail_insert() sets this flag to 1 on entry and clears it on
 * exit.  execute_insert_batch() checks the flag before calling
 * audit_trail_insert() and skips the audit call when it is set.
 * Because Data_Manager is single-threaded per OCI context this is
 * sufficient; no mutex is required.
 *
 * AUDIT_TRAIL table columns populated
 * ------------------------------------
 *   TABLE_NAME        - source table being audited
 *   RECORD_ID         - stringified primary key of the business row
 *                       (caller supplies; typically NUMBER_COL or ROWID)
 *   FIELD_NAME        - column name being audited
 *   ACTION_TYPE       - 'INSERT' | 'UPDATE' | 'DELETE'
 *   OLD_VALUE         - NULL on INSERT; previous value on UPDATE/DELETE
 *   NEW_VALUE         - new value on INSERT/UPDATE; NULL on DELETE
 *   DATA_TYPE         - Oracle data type string e.g. "VARCHAR2", "NUMBER"
 *   CHANGED_BY        - application username from ctx->ini->username
 *   CHANGE_REASON     - caller-supplied free-text reason (mandatory)
 *   TRANSACTION_ID    - ctx->active_tx transaction UUID or "-"
 *   SESSION_ID        - ctx->active_tx session UUID or "-"
 *   CLIENT_IP         - "-" (populated by Session Manager in future)
 *   APPLICATION_NAME  - "Data_Manager"
 *   MODULE_NAME       - caller-supplied module name e.g. "OCI_Insert"
 *   ROW_HASH          - SHA-256 placeholder (hex string of key fields)
 *                       Full cryptographic hash added in Stage 2.
 *
 * Columns NOT populated by Stage 1 (left to Oracle defaults or NULL)
 * ------------------------------------------------------------------
 *   AUDIT_ID          - GENERATED ALWAYS AS IDENTITY (Oracle assigns)
 *   CHANGED_BY_DB_USER- DEFAULT SYS_CONTEXT (Oracle assigns)
 *   CHANGE_TIMESTAMP  - DEFAULT SYSTIMESTAMP (Oracle assigns)
 *   PARTITION_DATE    - VIRTUAL GENERATED ALWAYS (Oracle assigns)
 *   ESIG_ID / ESIG_MEANING / ESIG_TIMESTAMP / ESIG_REQUIRED
 *                     - Electronic signature fields; populated in
 *                       Stage 2 when the e-signature module is integrated.
 *
 * Integration in execute_insert_batch()
 * --------------------------------------
 *   // At the top of execute_insert_batch(), after headers:
 *   #include "OCI_Audit_Trail_Manager.h"
 *
 *   // After successful OCITransCommit / after the LOB write loop,
 *   // before the Stage 5 result XML block:
 *   if (!audit_trail_in_progress)
 *   {
 *       audit_trail_request_t atr;
 *       memset(&atr, 0, sizeof(atr));
 *       strncpy(atr.table_name,   ic->table_name, sizeof(atr.table_name)-1);
 *       strncpy(atr.action_type,  "INSERT",        sizeof(atr.action_type)-1);
 *       strncpy(atr.changed_by,   ctx->ini->username, sizeof(atr.changed_by)-1);
 *       strncpy(atr.change_reason,"Business insert", sizeof(atr.change_reason)-1);
 *       strncpy(atr.module_name,  "OCI_Insert",    sizeof(atr.module_name)-1);
 *       // record_id: use rowid_str obtained from OCI_ATTR_ROWID above
 *       strncpy(atr.record_id,    rowid_str,       sizeof(atr.record_id)-1);
 *       atr.fields      = ic->values;       // field_value_t flat array
 *       atr.col_names   = ic->col_names;    // column name array
 *       atr.col_types   = cols;             // col_metadata_t array
 *       atr.col_count   = ic->col_count;
 *       atr.row_count   = ic->row_count;
 *       audit_trail_insert(ctx, &atr);
 *   }
 *
 * Compile additions
 * -----------------
 *   OCI_Audit_Trail_Manager.c
 *
 * Dependencies
 * ------------
 *   OCI_Connection.h          - oci_context_t
 *   OCI_Insert_Execute_Module.h - execute_insert_batch()
 *   OCI_Table_Metadata_Module.h - col_metadata_t
 *   OCI_Transaction_Manager.h - tx_get_id()
 *   logger.h                  - logger_write()
 *   metrics.h                 - metrics_now_us() (timing only)
 */

#ifndef OCI_AUDIT_TRAIL_MANAGER_H
#define OCI_AUDIT_TRAIL_MANAGER_H

#include "OCI_Connection.h"
#include "OCI_Table_Metadata_Module.h"
#include "OCI_Execute_Query_Batch_Module.h"
#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define AUDIT_MAX_COLS          1024   /* max columns per audited row  */
#define AUDIT_TABLE_NAME        "AUDIT_TRAIL"
#define AUDIT_OWNER             "DATA_MANAGER"
#define AUDIT_APPLICATION_NAME  "Data_Manager"

/* ------------------------------------------------------------------ */
/*  Audit mode constants                                                */
/*                                                                      */
/*  AUDIT_MODE_FIELD (default)                                          */
/*    One AUDIT_TRAIL row per (business row × column).                  */
/*    Used for normal application DML where field-level change history  */
/*    is required.                                                       */
/*                                                                       */
/*  AUDIT_MODE_ROW_SNAPSHOT                                              */
/*    One AUDIT_TRAIL row per business row.                              */
/*    All column values are serialised into NEW_VALUE as a single CLOB  */
/*    in "COL=VALUE|COL=VALUE|..." format.                               */
/*    FIELD_NAME is set to the sentinel AUDIT_SNAPSHOT_SENTINEL.        */
/*    Used for bulk loads and migration jobs where per-field audit rows  */
/*    would generate prohibitive volume.                                 */
/*                                                                       */
/*  Set audit_trail_request_t.audit_mode to one of these values.       */
/*  Defaults to AUDIT_MODE_FIELD when audit_mode == 0.                 */
/* ------------------------------------------------------------------ */
#define AUDIT_MODE_FIELD         0    /* one row per field (default)  */
#define AUDIT_MODE_ROW_SNAPSHOT  1    /* one row per business row     */

/* Sentinel stored in FIELD_NAME for row-snapshot audit records.
 * Callers querying AUDIT_TRAIL for field-level changes should add
 * WHERE FIELD_NAME != AUDIT_SNAPSHOT_SENTINEL to exclude snapshots. */
#define AUDIT_SNAPSHOT_SENTINEL  "__ROW_SNAPSHOT__"

/* ------------------------------------------------------------------ */
/*  Cycle-guard flag                                                    */
/*                                                                      */
/*  Declared extern here; defined once in OCI_Audit_Trail_Manager.c.  */
/*  execute_insert_batch() checks this before calling                   */
/*  audit_trail_insert() to prevent infinite recursion.                */
/*                                                                      */
/*  Usage in execute_insert_batch.c (add near top of file):            */
/*    #include "OCI_Audit_Trail_Manager.h"                             */
/*    extern int audit_trail_in_progress;                              */
/*                                                                      */
/*  Then guard the audit call:                                          */
/*    if (!audit_trail_in_progress) { audit_trail_insert(...); }       */
/* ------------------------------------------------------------------ */
extern int audit_trail_in_progress;

/* ------------------------------------------------------------------ */
/*  audit_trail_request_t                                               */
/*  All information needed to write one set of audit rows for one      */
/*  business DML operation.  Populated by the caller (execute module)  */
/*  and passed to audit_trail_insert().                                */
/*                                                                      */
/*  For INSERT: old_values[] is NULL (all OLD_VALUE written as NULL).  */
/*  For UPDATE: old_values[] contains the pre-update values.           */
/*  For DELETE: new_values[] is NULL (all NEW_VALUE written as NULL).  */
/* ------------------------------------------------------------------ */
typedef struct
{
    /* ---- What was changed ---- */
    char            table_name  [128];   /* audited business table       */
    char            record_id   [256];   /* PK / ROWID of the row        */
    char            action_type [10];    /* "INSERT" | "UPDATE" | "DELETE"*/

    /* ---- Field arrays (parallel, indexed 0 .. col_count-1) ---- */
    /*
     * col_names  - array of col_count C-strings, each [128] bytes.
     *              Matches ic->col_names[][] from the insert context.
     *
     * col_types  - pointer to a col_metadata_t array (col_count entries).
     *              Provides DATA_TYPE string for the AUDIT_TRAIL.DATA_TYPE
     *              column.  Pass NULL to record "-" for all types.
     *
     * new_values - flat field_value_t array [row_count * col_count].
     *              Use macro AUDIT_FV(new_values, row, col, col_count)
     *              to index.  Pass NULL on DELETE.
     *
     * old_values - flat field_value_t array [row_count * col_count].
     *              Pass NULL on INSERT (OLD_VALUE will be written as NULL).
     *
     * row_count  - number of business rows processed.
     * col_count  - number of columns per row.
     *
     * Note: audit_trail_insert() iterates all rows × all columns and
     * writes one AUDIT_TRAIL row per (business row, column) pair.
     * For a 2-row INSERT with 3 columns, 6 audit rows are produced.
     */
    char          (*col_names) [128];    /* [col_count][128]             */
    col_metadata_t *col_types;           /* [col_count] or NULL          */
    void           *new_values;          /* field_value_t* or NULL       */
    void           *old_values;          /* field_value_t* or NULL       */
    int             row_count;
    int             col_count;

    /* ---- Who / Why ---- */
    char            changed_by    [100]; /* app username                 */
    char            change_reason [500]; /* mandatory free-text          */
    char            module_name   [100]; /* e.g. "OCI_Insert_Execute"    */

    /* ---- Optional (populated by Session Manager in future) ---- */
    char            client_ip     [46];  /* IPv4/IPv6 or "-"             */
    char            session_id    [64];  /* session UUID or "-"          */

    /* ---- Audit mode ---- */
    /*  AUDIT_MODE_FIELD (0, default) : one row per field                */
    /*  AUDIT_MODE_ROW_SNAPSHOT (1)   : one row per business row         */
    /*  Leave as 0 for normal application DML.                           */
    /*  Set to AUDIT_MODE_ROW_SNAPSHOT for bulk loads / migration jobs.  */
    int             audit_mode;

} audit_trail_request_t;

/* ------------------------------------------------------------------ */
/*  Convenience macro: index into flat field_value_t array             */
/*                                                                      */
/*  field_value_t arrays from OCI_Insert_Execute_Module are stored as  */
/*  ic->values[row * col_count + col].                                 */
/*  Cast the void* pointers in audit_trail_request_t before use:       */
/*                                                                      */
/*  field_value_t *nv = (field_value_t *)atr->new_values;             */
/*  const char *val = AUDIT_FV(nv, row, col, atr->col_count).value;   */
/* ------------------------------------------------------------------ */
#define AUDIT_FV(arr, row, col, ncols) ((arr)[(row) * (ncols) + (col)])

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

/*
 * audit_trail_insert()
 *
 * Write one AUDIT_TRAIL row for every (row, column) pair described in
 * atr.  Uses execute_insert_batch() internally with the cycle-guard
 * flag set to prevent recursion.
 *
 * The audit rows are written on the caller's OCI session within the
 * caller's active transaction (ctx->active_tx).  No commit is issued
 * here - the caller owns the transaction boundary.
 *
 * If ctx->active_tx is NULL the function still writes the audit rows
 * but logs a warning because the rows may be committed independently
 * of the business rows if the caller issues its own commit.
 *
 * Parameters
 *   ctx  - OCI context (connection + loggers).  Must be the SAME
 *          context used for the business DML so both operations share
 *          the same Oracle session and transaction.
 *   atr  - fully populated audit_trail_request_t.
 *
 * Returns
 *    0   all audit rows written successfully
 *   -1   error (logged to ctx->audit_logger; business DML is unaffected
 *         - the caller decides whether to abort on audit failure)
 *
 * Thread safety
 * -------------
 * audit_trail_in_progress is a plain int (not atomic).  This is safe
 * because each oci_context_t is used by exactly one thread at a time.
 * Do NOT share a single ctx across threads without external locking.
 */
int audit_trail_insert(oci_context_t         *ctx,
                       audit_trail_request_t *atr);

/*
 * audit_trail_build_row_hash()
 *
 * Build a deterministic hex string from the key audit fields:
 *   table_name | record_id | field_name | action_type |
 *   new_value  | changed_by | transaction_id
 *
 * Stage 1: produces a simple FNV-1a 64-bit hex string as a placeholder.
 * Stage 2: replace with SHA-256 using OpenSSL or similar.
 *
 * dest must be at least 17 bytes (16 hex chars + NUL).
 * Returns dest for convenience.
 */
char *audit_trail_build_row_hash(const char *table_name,
                                  const char *record_id,
                                  const char *field_name,
                                  const char *action_type,
                                  const char *new_value,
                                  const char *changed_by,
                                  const char *transaction_id,
                                  char       *dest,
                                  size_t      dest_max);

/*
 * audit_trail_insert_snapshot()
 *
 * Write ONE AUDIT_TRAIL row per business row, serialising all column
 * values into the NEW_VALUE CLOB as a pipe-delimited snapshot string:
 *
 *   COL1=value1|COL2=value2|COL3=value3|...
 *
 * OLD_VALUE is written as NULL (INSERT snapshot) or as a matching
 * snapshot string of the pre-change values (UPDATE/DELETE snapshot).
 * FIELD_NAME is set to the sentinel "__ROW_SNAPSHOT__" so queries can
 * distinguish snapshot rows from field-level rows:
 *
 *   SELECT * FROM AUDIT_TRAIL
 *   WHERE TABLE_NAME = 'MY_TABLE'
 *   AND   FIELD_NAME != '__ROW_SNAPSHOT__';   -- exclude snapshots
 *
 * Intended use
 * ------------
 *   Migration jobs, bulk loads, or any high-volume operation where
 *   AUDIT_MODE_FIELD would generate prohibitive row volume.
 *   The caller sets atr->audit_mode = AUDIT_MODE_ROW_SNAPSHOT and
 *   calls audit_trail_insert() - which delegates here automatically -
 *   OR calls audit_trail_insert_snapshot() directly.
 *
 * Parameters
 *   ctx  - OCI context (same as business DML context)
 *   atr  - populated audit_trail_request_t; audit_mode is ignored here
 *          since this function always uses snapshot mode.
 *
 * Returns
 *    0   all snapshot rows written successfully (one per business row)
 *   -1   error (logged to ctx->audit_logger)
 *
 * Volume comparison vs audit_trail_insert()
 * ------------------------------------------
 *   500 rows × 20 columns:
 *     AUDIT_MODE_FIELD        -> 10,000 AUDIT_TRAIL rows
 *     AUDIT_MODE_ROW_SNAPSHOT ->    500 AUDIT_TRAIL rows
 */
int audit_trail_insert_snapshot(oci_context_t         *ctx,
                                 audit_trail_request_t *atr);

/*
 * audit_trail_serialise_row()
 *
 * Serialise one business row's field values into a pipe-delimited
 * "COL=VALUE|COL=VALUE|..." string suitable for storage in NEW_VALUE
 * or OLD_VALUE.
 *
 * Values containing '|' or '=' are surrounded by double-quotes.
 * NULL / empty values are written as COL=<NULL>.
 *
 * Parameters
 *   col_names  - array of column name strings [col_count][128]
 *   values     - flat audit_field_value_t array, row br at
 *                values[br * col_count .. br * col_count + col_count-1]
 *   col_count  - number of columns
 *   row_idx    - which business row to serialise (0-based)
 *   dest       - caller-supplied output buffer
 *   dest_max   - size of dest in bytes
 *
 * Returns dest on success, NULL if dest_max is too small.
 * Exposed publicly so callers can build snapshot strings independently
 * (e.g. for logging or comparison).
 */
char *audit_trail_serialise_row(char          (*col_names)[128],
                                 const void    *values,
                                 int            col_count,
                                 int            row_idx,
                                 char          *dest,
                                 size_t         dest_max);

/* ================================================================== */
/*  Stage 2 - UPDATE audit support                                      */
/* ================================================================== */

/*
 * audit_old_value_t
 *
 * Holds the before-image value for one column of one business row.
 * Populated by audit_trail_fetch_before_image() via a SELECT against
 * the business table before the UPDATE executes.
 *
 * is_null = 1  : column was NULL in the database (OLD_VALUE = NULL)
 * is_null = 0  : value[] holds the current column value as a string
 */
typedef struct {
    char value[32768];   /* matches MAX_COL_VALUE_SIZE in update module */
    int  is_null;
} audit_old_value_t;

/*
 * audit_trail_fetch_before_image()
 *
 * Execute a SELECT against the business table to capture the current
 * (pre-update, or pre-delete) values of the target columns.
 *
 * Builds and executes:
 *   SELECT col1, col2, ... FROM owner.table
 *   WHERE  key1 = 'val1' AND key2 = 'val2' ...
 * (or, for a DATE/TIMESTAMP/INTERVAL-typed key - see key_data_types
 * below - key1 = TO_DATE('val1','YYYY-MM-DD HH24:MI:SS') and so on)
 *
 * Uses OCI_Execute_Query_Batch_Module internally.  Results are stored
 * in old_values[row][col] indexed as old_values[r * col_count + c].
 *
 * Parameters
 *   ctx        - OCI context (same session as the UPDATE/DELETE)
 *   table_name - business table name
 *   owner      - schema owner (may be empty)
 *   col_names  - columns being captured [col_count][128]
 *   col_count  - number of columns being captured
 *   key_names  - WHERE key column names [key_count][128]
 *   key_values - WHERE key values [key_count][32768]
 *   key_data_types - WHERE key columns' REAL Oracle data types
 *                (e.g. "DATE", "TIMESTAMP(6)", "NUMBER", "VARCHAR2")
 *                [key_count][128], resolved by the caller via
 *                metadata_cache - never trust a client-supplied type,
 *                same reasoning as every other type resolution in this
 *                project. May be NULL (every key then embeds as a
 *                plain string literal, the original pre-2026-07-26
 *                behaviour) - but a caller that has already resolved
 *                real column metadata for its own SQL building (which
 *                every caller of this function does, since it's always
 *                called from inside execute_update_batch()/
 *                execute_delete_batch()) should always pass the real
 *                types instead.
 *                Found and fixed 2026-07-26: without this, a DATE (or
 *                TIMESTAMP/INTERVAL) key value was always embedded as
 *                a bare string literal ('2026-05-20 00:00:00'),
 *                relying on Oracle's implicit string-to-DATE
 *                conversion succeeding - which isn't guaranteed
 *                (depends on session NLS_DATE_FORMAT actually matching
 *                the literal's format), and failed outright the first
 *                time any WHERE-key audit path was ever exercised with
 *                a DATE-typed key (DELETE's own Round 3 test fixture -
 *                UPDATE's fixtures all happened to key on NUMBER_COL,
 *                so this was a dormant, pre-existing gap in this
 *                function itself, not something DELETE introduced).
 *   key_count  - number of WHERE key columns
 *   old_values - output: flat array [row_count * col_count]
 *                caller must free() this pointer
 *   row_count  - output: number of rows fetched
 *
 * Returns
 *    0   before-image fetched successfully
 *   -1   error (SELECT failed or no rows found)
 */
int audit_trail_fetch_before_image(oci_context_t  *ctx,
                                    const char     *table_name,
                                    const char     *owner,
                                    char          (*col_names)[128],
                                    int             col_count,
                                    char          (*key_names)[128],
                                    char          (*key_values)[32768],
                                    char          (*key_data_types)[128],
                                    int             key_count,
                                    audit_old_value_t **old_values,
                                    int            *row_count);

/*
 * audit_trail_insert_update()
 *
 * Write one AUDIT_TRAIL row per (updated row × updated column).
 *
 * For each column being updated, compares old_values[r][c] with
 * new_values[r][c].  If the values differ, writes one audit row with:
 *   OLD_VALUE = old_values[r][c]
 *   NEW_VALUE = new_values[r][c]
 *   FIELD_NAME = col_names[c]
 *   ACTION_TYPE = "UPDATE"
 *
 * Columns where old and new values are identical are skipped —
 * no audit row is written for unchanged columns.  This keeps the
 * audit trail clean and proportional to actual changes, not just
 * the columns listed in the UPDATE statement.
 *
 * Parameters
 *   ctx        - OCI context (same session as the UPDATE)
 *   atr        - populated audit_trail_request_t with:
 *                  table_name, action_type="UPDATE", changed_by,
 *                  change_reason, module_name, record_id,
 *                  col_names, col_types, col_count, row_count
 *                  new_values = upd_field_value_t* (UPDATE SET values)
 *   old_values - before-image from audit_trail_fetch_before_image()
 *
 * Returns
 *    0   all audit rows written successfully
 *   -1   error (logged to ctx->audit_logger)
 */
int audit_trail_insert_update(oci_context_t         *ctx,
                               audit_trail_request_t *atr,
                               audit_old_value_t     *old_values);

/*
 * audit_trail_insert_delete()
 *
 * Write one AUDIT_TRAIL row per (matched row x WHERE-key column) -
 * unconditional, unlike audit_trail_insert_update()'s diff-based skip:
 * there is no "new" value to compare against for a DELETE, the old
 * value going away IS the change by definition:
 *   OLD_VALUE = old_values[r][c]
 *   NEW_VALUE = NULL
 *   FIELD_NAME = col_names[c]
 *   ACTION_TYPE = "DELETE"
 *
 * Scoped to the WHERE-key columns only (2026-07-26 design decision),
 * not every column on the table - col_names/old_values here already
 * only ever cover that same set, since audit_trail_fetch_before_image()
 * is called with the WHERE keys themselves as the columns to capture.
 *
 * Called BEFORE the actual DELETE executes (from
 * execute_delete_batch()), so the attempt is captured in AUDIT_TRAIL
 * even if Oracle itself then rejects the DELETE for lack of privilege
 * - a GxP-relevant distinction: in many regulated environments,
 * database records aren't deleted at all (a status change to
 * "CLOSED"/similar is used instead), so the executing account may have
 * no DELETE privilege whatsoever, and the record of the attempt still
 * needs to exist regardless of whether the DELETE itself succeeds.
 *
 * Parameters
 *   ctx        - OCI context (same session as the DELETE)
 *   atr        - populated audit_trail_request_t with:
 *                  table_name, action_type="DELETE", changed_by,
 *                  change_reason, module_name, record_id,
 *                  col_names, col_types, col_count, row_count
 *                  new_values is ignored (always NULL on the
 *                  resulting per-row request this function builds)
 *   old_values - before-image from audit_trail_fetch_before_image()
 *
 * Returns
 *    0   all audit rows written successfully
 *   -1   error (logged to ctx->audit_logger)
 */
int audit_trail_insert_delete(oci_context_t         *ctx,
                               audit_trail_request_t *atr,
                               audit_old_value_t     *old_values);

#ifdef __cplusplus
}
#endif

#endif /* OCI_AUDIT_TRAIL_MANAGER_H */
