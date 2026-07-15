/*
 * sql_dependency_extractor.h
 *
 * SQL Dependency Extractor
 * ------------------------
 * Parses a SELECT statement and extracts every table/view reference
 * and field reference into an OCI_DEPENDENCY_LIST.
 *
 * SQL format rules
 * ----------------
 * Strict rules (always enforced):
 *   Rule 4 - Subqueries, UNION, INTERSECT, EXCEPT are not supported
 *   Rule 5 - Functions in SELECT are not supported
 *
 * Relaxed rules (expansion required - see needs_expansion flag):
 *   Rule 1 - Fields should be qualified as TABLE.COLUMN.
 *            Unqualified fields are accepted when there is exactly
 *            one FROM-clause object (source table is unambiguous).
 *            The source table is inferred and table_ref is populated.
 *            needs_expansion is set on the field and on the deps struct.
 *
 *   Rule 3 - SELECT * and TABLE.* are accepted.
 *            When * is detected, deps->needs_expansion = 1 and
 *            field_count = 0.  The caller (execute_query_batch Phase B)
 *            must expand the field list from the OCI descriptor names
 *            after OCIStmtExecute OCI_DESCRIBE_ONLY completes.
 *            TABLE.* per-table wildcard stores field_name = "*" with
 *            the correct table_ref so Phase B expands per-table.
 *
 * Rule 2 - FROM clause table format (unchanged - always enforced)
 * Rule 4 - Subqueries / UNION / INTERSECT / EXCEPT (unchanged)
 * Rule 5 - Functions in SELECT (unchanged)
 *
 * Supported SELECT forms
 * ----------------------
 *   SELECT t.col1, t.col2 FROM my_table t WHERE ...
 *   SELECT a.id, b.name FROM table_a a, table_b b WHERE a.id = b.id
 *   SELECT t.col AS label FROM owner.my_table t
 *   SELECT t.col1, t.col2 FROM my_table t INNER JOIN other_table o ...
 *   SELECT * FROM my_table t                  (needs_expansion=1)
 *   SELECT t.* FROM my_table t                (needs_expansion=1)
 *   SELECT col1, col2 FROM my_table t         (needs_expansion=1, single table)
 *   SELECT t.col1, col2 FROM my_table t       (needs_expansion=1, single table)
 */

#ifndef SQL_DEPENDENCY_EXTRACTOR_H
#define SQL_DEPENDENCY_EXTRACTOR_H

#include "OCI_Connection.h"   /* oci_context_t - for logger pointers  */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Limits                                                              */
/* ------------------------------------------------------------------ */
#define SQL_DEP_MAX_OBJECTS   64    /* max tables/views per query      */
#define SQL_DEP_MAX_FIELDS    256   /* max fields in SELECT clause     */
#define SQL_DEP_MAX_ERR       512   /* error message buffer size       */

/* ------------------------------------------------------------------ */
/*  Object type codes                                                   */
/* ------------------------------------------------------------------ */
#define SQL_OBJ_TYPE_UNKNOWN  0
#define SQL_OBJ_TYPE_TABLE    1
#define SQL_OBJ_TYPE_VIEW     2     /* set by caller after resolution  */
#define SQL_OBJ_TYPE_ALIAS    3     /* alias entry in object list      */

/* ------------------------------------------------------------------ */
/*  Field expansion type codes                                          */
/*  Set on OCI_FIELD_REF entries that require Phase B expansion.       */
/* ------------------------------------------------------------------ */
#define SQL_FIELD_NORMAL        0   /* fully qualified TABLE.COLUMN     */
#define SQL_FIELD_WILDCARD_ALL  1   /* SELECT * - expand all columns    */
#define SQL_FIELD_WILDCARD_TBL  2   /* TABLE.* - expand one table's cols*/
#define SQL_FIELD_UNQUALIFIED   3   /* inferred single-table field      */

/* ------------------------------------------------------------------ */
/*  OCI_OBJECT_REF                                                      */
/*  Describes one table/view reference or one field reference.         */
/* ------------------------------------------------------------------ */
typedef struct
{
    char owner      [64];   /* schema owner (may be empty if not      */
                            /* specified; caller resolves from cache) */
    char object_name[128];  /* table or view name                     */
    char alias      [64];   /* alias used in the SQL (may be empty)   */
    int  object_type;       /* SQL_OBJ_TYPE_* constant                */
    int  field_pos;         /* 1-based position in SELECT clause      */
                            /* 0 for FROM-clause (table) entries      */
} OCI_OBJECT_REF;

/* ------------------------------------------------------------------ */
/*  OCI_FIELD_REF                                                       */
/*  Describes one field in the SELECT clause.                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    char table_ref    [64];   /* the OBJECT_NAME or alias before the . */
    char field_name   [128];  /* column name; "*" for wildcard entries  */
    char field_alias  [128];  /* alias after AS (may be empty)          */
    int  field_pos;           /* 1-based position in SELECT clause      */
    int  expansion_type;      /* SQL_FIELD_* constant                   */
                              /* 0 = normal; non-zero = needs Phase B   */
} OCI_FIELD_REF;

/* ------------------------------------------------------------------ */
/*  OCI_DEPENDENCY_LIST                                                 */
/*  Complete dependency information extracted from one SELECT.         */
/* ------------------------------------------------------------------ */
typedef struct
{
    /* ---- FROM-clause objects (tables / views) ---- */
    OCI_OBJECT_REF objects[SQL_DEP_MAX_OBJECTS];
    int            object_count;

    /* ---- SELECT-clause fields ---- */
    OCI_FIELD_REF  fields[SQL_DEP_MAX_FIELDS];
    int            field_count;

    /* ---- Expansion flag ---- */
    /*  Set to 1 when the field list contains wildcard (*) or           */
    /*  unqualified entries that must be resolved after OCI describe.   */
    /*  The caller (execute_query_batch Phase B) reads this flag and    */
    /*  populates fields[] from bc.col_names[] before calling           */
    /*  get_select_metadata().  When 0 the field list is complete.      */
    int            needs_expansion;

} OCI_DEPENDENCY_LIST;

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

/*
 * extract_sql_dependencies()
 *
 * Parse sql and populate deps with all table/view and field references.
 *
 * Parameters
 *   sql    - the SELECT statement to parse (read-only)
 *   deps   - caller-allocated OCI_DEPENDENCY_LIST to populate
 *   ctx    - OCI context used only for logger pointers
 *            (ctx->select_logger receives all log output)
 *            May be NULL; logging is skipped if so.
 *
 * Returns
 *    0   success - deps is fully populated
 *   -1   parse error - deps is undefined; check err_msg
 *
 * On failure the reason is written into deps->err_msg if the caller
 * needs it programmatically, and logged to ctx->select_logger.
 *
 * Usage example (see sql_dependency_extractor_usage.c for full test):
 *
 *   OCI_DEPENDENCY_LIST deps;
 *   memset(&deps, 0, sizeof(deps));
 *
 *   if (extract_sql_dependencies(sql, &deps, ctx) != 0)
 *   {
 *       logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
 *                    "SQL dependency extraction failed");
 *       goto Cleanup;
 *   }
 *
 *   for (int i = 0; i < deps.object_count; i++)
 *   {
 *       get_request_metadata(ctx,
 *                             deps.objects[i].owner,
 *                             deps.objects[i].object_name, ...);
 *   }
 */
int extract_sql_dependencies(const char         *sql,
                               OCI_DEPENDENCY_LIST *deps,
                               oci_context_t       *ctx);

/*
 * sql_dep_dump()
 *
 * Write the full contents of deps to ctx->select_logger at LOG_INFO.
 * Useful for debugging and test validation.
 * Safe to call with a NULL ctx (output goes to stdout instead).
 */
void sql_dep_dump(const OCI_DEPENDENCY_LIST *deps,
                  oci_context_t             *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SQL_DEPENDENCY_EXTRACTOR_H */
