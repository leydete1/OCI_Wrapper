
/*
 * OCI_Table_Metadata_Module.h
 *
 * Shared Table Metadata Module
 * -----------------------------
 * Provides two public metadata functions:
 *
 *   get_request_metadata()
 *   ----------------------
 *   Queries ALL_TAB_COLUMNS for a SINGLE named table and populates a
 *   caller-supplied col_metadata_t array.  Used by INSERT, UPDATE, and
 *   any other module that targets exactly one table.  When the metadata
 *   cache is introduced, this function will serve cached results for
 *   single-table callers with no changes required in the callers.
 *
 *   get_multi_metadata()
 *   --------------------
 *   Populates the OCI define/buffer/indicator arrays that
 *   execute_query_batch needs to describe and fetch a result set.
 *   It works entirely from OCI descriptor metadata (OCIParamGet /
 *   OCIAttrGet) rather than ALL_TAB_COLUMNS, making it correct for:
 *
 *     - Multi-table JOIN queries  (columns from many tables merged
 *       into one result set - ALL_TAB_COLUMNS cannot be queried per
 *       column without knowing which table each column belongs to)
 *
 *     - Views  (older Oracle versions do not expose view columns in
 *       ALL_TAB_COLUMNS; descriptor metadata always works)
 *
 *     - Synonyms and remote DB-link tables (same reason as views)
 *
 *   For the common single-table SELECT case the two functions produce
 *   equivalent results.  The distinction matters only when the cache
 *   arrives: get_request_metadata() will serve cached data;
 *   get_multi_metadata() will continue to use live OCI descriptors
 *   because column-to-table attribution is unavailable for joins.
 *   This boundary is intentional and permanent - both functions are
 *   maintained here so metadata logic never leaks into execute modules.
 *
 * Consumers
 * ---------
 *   get_request_metadata()  : OCI_Insert_Template_Module
 *                             OCI_Insert_Execute_Module
 *                             OCI_Update_Execute_Module
 *
 *   get_multi_metadata()    : OCI_Execute_Query_Batch_Module
 *                             OCI_Execute_Procedure_Module (cursor fetch)
 */

#ifndef OCI_TABLE_METADATA_MODULE_H
#define OCI_TABLE_METADATA_MODULE_H

#include "logger.h"
#include <oci.h>
#include <OCI_Connection.h>
#include "sql_dependency_extractor.h"

/* ------------------------------------------------------------------ */
/*  Maximum columns supported per table / result set                  */
/* ------------------------------------------------------------------ */
#define MAX_TABLE_COLUMNS  1024

/* ------------------------------------------------------------------ */
/*  col_metadata_t                                                     */
/*  One entry per column - canonical type shared by all modules.      */
/*  source_table is populated by get_multi_metadata() for diagnostic  */
/*  logging; left empty by get_request_metadata() single-table path.  */
/* ------------------------------------------------------------------ */
typedef struct {
    char  col_name    [128];  /* COLUMN_NAME / OCI descriptor name     */
    char  data_type   [128];  /* DATA_TYPE  e.g. "NUMBER","VARCHAR2"   */
    int   data_length;        /* DATA_LENGTH  (bytes / chars)          */
    int   data_precision;     /* DATA_PRECISION  (-1 = not applicable) */
    int   data_scale;         /* DATA_SCALE     (-1 = not applicable)  */
    char  nullable    [4];    /* "Y" or "N"                            */
    char  data_default[512];  /* DATA_DEFAULT   (empty string if none) */
    char  source_table[128];  /* set by get_multi_metadata() only      */
} col_metadata_t;

/* ------------------------------------------------------------------ */
/*  metadata_request_t                                                 */
/*  Input descriptor for get_request_metadata() single-table path.    */
/* ------------------------------------------------------------------ */
typedef struct {
    char  table_name[128];    /* target table, upper-cased             */
    char  owner     [128];    /* schema owner, upper-cased             */
                              /* if empty, resolved automatically      */
                              /* from ALL_TABLES at runtime            */
} metadata_request_t;

/* ------------------------------------------------------------------ */
/*  multi_meta_request_t                                               */
/*  Input/output descriptor for get_multi_metadata().                 */
/*  All pointer fields point into the batch_ctx_t arrays that         */
/*  execute_query_batch has already allocated via                      */
/*  allocate_batch_buffers().  get_multi_metadata() fills them in     */
/*  exactly as define_columns_batch() previously did, so the batch    */
/*  module's fetch loop and free_batch_ctx() are completely unchanged. */
/* ------------------------------------------------------------------ */
typedef struct {

    /* ---- Input: OCI handles ---- */
    oci_context_t      *ctx;           /* connection + logger               */
    OCIStmt            *stmt;          /* described / executed stmt handle  */

    /* ---- Input: SQL dependency list (optional) ---- */
    /*  When non-NULL, get_select_metadata() uses this to call              */
    /*  get_table_metadata() for every FROM-clause object and log rich      */
    /*  table-level metadata alongside the OCI column descriptors.          */
    /*  get_multi_metadata() ignores this field completely.                 */
    OCI_DEPENDENCY_LIST *deps;         /* from extract_sql_dependencies()   */

    /* ---- Input: sizing ---- */
    ub4             col_count;     /* from OCI_ATTR_PARAM_COUNT         */
    ub4             fetch_count;   /* rows per OCIStmtFetch2 call       */

    /* ---- Output: pointers into batch_ctx_t arrays ---- */
    /*  All arrays are allocated by the caller (allocate_batch_buffers) */
    /*  before calling get_multi_metadata().  This function fills them. */
    OCIDefine     **def;           /* [col_count]                       */
    char          **buffers;       /* [col_count] scalar flat buffers   */
    ub4            *buf_sizes;     /* [col_count] bytes per scalar slot */
    sb2           **indicators;    /* [col_count] -> [fetch_count]      */
    ub2            *data_types;    /* [col_count] OCI type codes        */
    ub4            *data_sizes;    /* [col_count] OCI reported sizes    */
    char          (*col_names)[256]; /* [col_count]                     */
    OCILobLocator ***col_blob_locs;  /* [col_count] -> [fetch_count]    */
    OCILobLocator  *clob_loc;      /* single shared CLOB locator        */

} multi_meta_request_t;

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */

/*
 * get_request_metadata()
 *
 * Single-table path.  Queries ALL_TAB_COLUMNS for req->table_name /
 * req->owner and populates the caller-supplied cols[] array in
 * COLUMN_ID order.
 *
 * If req->owner is empty it is resolved automatically from ALL_TABLES
 * and written back into req->owner.
 *
 * Parameters
 *   ctx       - OCI context (connection + logger)
 *   req       - table name and optional owner (modified in place)
 *   cols      - caller-allocated array of at least max_cols entries
 *   col_count - set to the number of columns found on success
 *   max_cols  - size of the cols[] array (suggest MAX_TABLE_COLUMNS)
 *
 * Returns
 *    0  success
 *   -1  error (logged)
 */
int get_request_metadata(oci_context_t      *ctx,
                         metadata_request_t *req,
                         col_metadata_t     *cols,
                         int                *col_count,
                         int                 max_cols);

/*
 * get_multi_metadata()
 *
 * Multi-table / view / join path.
 * Describes every column in the result set of mmr->stmt via OCI
 * descriptor metadata (OCIParamGet / OCIAttrGet), allocates per-column
 * buffers and LOB locators, and registers OCIDefineByPos +
 * OCIDefineArrayOfStruct so the caller's fetch loop can proceed
 * immediately after this call returns.
 *
 * Parameters
 *   mmr - fully populated multi_meta_request_t (see typedef above)
 *         All pointer fields must point to already-allocated arrays
 *         (via allocate_batch_buffers or equivalent).
 *
 * Returns
 *    0  success - all arrays populated, defines registered
 *   -1  error   - logged via mmr->ctx->logger
 */
int get_multi_metadata(multi_meta_request_t *mmr);

/*
 * get_select_metadata()
 *
 * Combined metadata path for SELECT statements where SQL dependency
 * information is available.
 *
 * This function:
 *   1. Calls get_object_metadata() (queries ALL_OBJECTS) for every
 *      FROM-clause object in mmr->deps.  ALL_OBJECTS is used instead
 *      of ALL_TABLES so that views, synonyms, and materialised views
 *      are resolved correctly.  If any object is NOT found in
 *      ALL_OBJECTS the function returns -1 immediately (fail fast).
 *
 *   1b. For objects whose type is TABLE, validates every SELECT-clause
 *      field that references the table against ALL_TAB_COLUMNS via
 *      get_request_metadata().  Any column name not found in the table
 *      causes an immediate -1 return before any OCI execution
 *      round-trips are made.  Non-TABLE objects (VIEW, SYNONYM, etc.)
 *      are skipped - Oracle validates their columns at prepare time.
 *
 *   2. Uses mmr->deps->fields[] to cross-reference each SELECT-clause
 *      field to its source table and logs the mapping to Metadata_logger
 *      for diagnostics.  No OCI state is changed in this step.
 *   3. Delegates to get_multi_metadata() to perform the actual OCI
 *      OCIParamGet / OCIDefineByPos / OCIDefineArrayOfStruct work.
 *      The fetch loop and free_batch_ctx() in the batch module are
 *      completely unchanged.
 *
 * Parameters
 *   mmr - fully populated multi_meta_request_t.
 *         mmr->deps must be non-NULL and point to a successfully
 *         populated OCI_DEPENDENCY_LIST from extract_sql_dependencies().
 *         All other pointer fields must be pre-allocated exactly as
 *         for get_multi_metadata().
 *
 * Returns
 *    0  success - all bc arrays populated, defines registered
 *   -1  error   - logged via mmr->ctx->select_logger /
 *                 mmr->ctx->Metadata_logger
 *
 * Note: if mmr->deps is NULL the function falls through to
 * get_multi_metadata() transparently so existing callers are safe.
 */
int get_select_metadata(multi_meta_request_t *mmr);

/* ================================================================== */
/*  table_metadata_alltabs_t                                           */
/*                                                                     */
/*  Holds one row from ALL_TABLES.  Every VARCHAR2 column is stored   */
/*  as a NUL-terminated C string sized to the Oracle column width+1.  */
/*  Every NUMBER column is stored as a double (OCI fetches it as      */
/*  SQLT_FLT; -1.0 means the column was NULL).                        */
/*  LAST_ANALYZED (DATE) is stored as a formatted string              */
/*  "YYYY-MM-DD HH24:MI:SS" or empty string if NULL.                  */
/*                                                                     */
/*  Heap-allocated by get_table_metadata(); caller must call          */
/*  free_table_metadata() when done.                                   */
/* ================================================================== */
typedef struct {

    /* ---- Identity ---- */
    char  owner          [129];   /* NOT NULL VARCHAR2(128)            */
    char  table_name     [129];   /* NOT NULL VARCHAR2(128)            */

    /* ---- Storage ---- */
    char  tablespace_name[31];
    char  cluster_name   [129];
    char  iot_name       [129];
    char  status         [9];

    /* ---- Numeric storage parameters ---- */
    double pct_free;
    double pct_used;
    double ini_trans;
    double max_trans;
    double initial_extent;
    double next_extent;
    double min_extents;
    double max_extents;
    double pct_increase;
    double freelists;
    double freelist_groups;

    /* ---- Flags / small strings ---- */
    char   logging        [4];
    char   backed_up      [2];

    /* ---- Statistics ---- */
    double num_rows;
    double blocks;
    double empty_blocks;
    double avg_space;
    double chain_cnt;
    double avg_row_len;
    double avg_space_freelist_blocks;
    double num_freelist_blocks;

    /* ---- Parallelism ---- */
    char   degree         [11];
    char   instances      [11];

    /* ---- Misc flags ---- */
    char   cache          [6];
    char   table_lock     [9];
    double sample_size;
    char   last_analyzed  [32];   /* formatted DATE or ""              */
    char   partitioned    [4];
    char   iot_type       [13];
    char   temporary      [2];
    char   secondary      [2];
    char   nested         [4];
    char   buffer_pool    [8];
    char   flash_cache    [8];
    char   cell_flash_cache[8];
    char   row_movement   [9];
    char   global_stats   [4];
    char   user_stats     [4];
    char   duration       [16];
    char   skip_corrupt   [9];
    char   monitoring     [4];
    char   cluster_owner  [129];
    char   dependencies   [9];
    char   compression    [9];
    char   compress_for   [31];
    char   dropped        [4];
    char   read_only      [4];
    char   segment_created[4];
    char   result_cache   [8];
    char   clustering     [4];
    char   activity_tracking[24];
    char   dml_timestamp  [26];
    char   has_identity   [4];
    char   container_data [4];

    /* ---- In-Memory ---- */
    char   inmemory             [9];
    char   inmemory_priority    [9];
    char   inmemory_distribute  [16];
    char   inmemory_compression [18];
    char   inmemory_duplicate   [14];

    /* ---- Collation / sharding / misc ---- */
    char   default_collation        [101];
    char   duplicated               [2];
    char   synchronous_duplicated   [2];
    char   sharded                  [2];
    char   externally_sharded       [2];
    char   externally_duplicated    [2];
    char   external                 [4];
    char   hybrid                   [4];
    char   cellmemory               [25];
    char   containers_default       [4];
    char   container_map            [4];
    char   extended_data_link       [4];
    char   extended_data_link_map   [4];
    char   inmemory_service         [13];
    char   inmemory_service_name    [1001];
    char   container_map_object     [4];
    char   memoptimize_read         [9];
    char   memoptimize_write        [9];
    char   has_sensitive_column     [4];
    char   admit_null               [4];
    char   data_link_dml_enabled    [4];
    char   logical_replication      [9];
    char   staging                  [4];
    char   row_change_tracking      [4];
    char   has_reservable_column    [4];
    char   vector_index_type        [29];

} table_metadata_alltabs_t;

/* ================================================================== */
/*  get_table_metadata()                                               */
/*                                                                     */
/*  Query ALL_TABLES for a single object and return its full metadata. */
/*                                                                     */
/*  Parameters                                                         */
/*    ctx          - OCI context (connection + loggers)                */
/*    object_owner - schema owner, case-insensitive.                   */
/*                   Pass NULL or "" to use ctx->ini->username.        */
/*    object_name  - table / view name, case-insensitive.             */
/*                                                                     */
/*  Returns                                                            */
/*    Heap-allocated table_metadata_alltabs_t* on success.            */
/*    Caller must call free_table_metadata() when done.               */
/*    NULL on error or if the table is not found (error is logged).   */
/*                                                                     */
/*  Logging                                                            */
/*    All activity goes to ctx->Metadata_logger.                      */
/*    On success the full struct is dumped at DEBUG level.            */
/* ================================================================== */
table_metadata_alltabs_t *get_table_metadata(oci_context_t *ctx,
                                              const char    *object_owner,
                                              const char    *object_name);

void free_table_metadata(table_metadata_alltabs_t *meta);

/* ================================================================== */
/*  object_metadata_allobjs_t                                          */
/*                                                                     */
/*  Holds one row from ALL_OBJECTS.  Covers every object type that     */
/*  can appear in a SELECT FROM clause: TABLE, VIEW, SYNONYM,          */
/*  MATERIALIZED VIEW, and remote DB-link objects.                     */
/*                                                                     */
/*  This is the correct metadata source for get_select_metadata()      */
/*  because SELECT queries in the Data_Manager project are typically   */
/*  issued against views rather than raw tables.  ALL_TABLES would     */
/*  return nothing for a view; ALL_OBJECTS covers everything.          */
/*                                                                     */
/*  LAST_DDL_TIME is stored as a formatted string                      */
/*  "YYYY-MM-DD HH24:MI:SS" and is suitable for metadata cache         */
/*  invalidation: if LAST_DDL_TIME > cache entry creation time the     */
/*  cached metadata is stale and should be refreshed.                  */
/*                                                                     */
/*  Heap-allocated by get_object_metadata(); caller must call          */
/*  free_object_metadata() when done.                                  */
/* ================================================================== */
typedef struct {

    /* ---- Identity ---- */
    char  owner          [129];   /* NOT NULL VARCHAR2(128)            */
    char  object_name    [129];   /* NOT NULL VARCHAR2(128)            */
    char  subobject_name [129];   /* partition / subpartition name     */
    char  object_type    [24];    /* TABLE, VIEW, SYNONYM, etc.        */

    /* ---- Object identifiers ---- */
    char  object_id      [20];    /* stored as string from TO_CHAR()   */
    char  data_object_id [20];    /* stored as string from TO_CHAR()   */

    /* ---- Status and timestamps ---- */
    char  status         [9];     /* VALID / INVALID / N/A             */
    char  created        [32];    /* TO_CHAR(CREATED,'YYYY-MM-DD HH24:MI:SS')      */
    char  last_ddl_time  [32];    /* TO_CHAR(LAST_DDL_TIME,'YYYY-MM-DD HH24:MI:SS')*/
    char  timestamp      [32];    /* spec / compile timestamp (VARCHAR2) */

    /* ---- Flags ---- */
    char  temporary      [2];     /* Y / N                             */
    char  generated      [2];     /* Y / N  (system-generated name)    */
    char  secondary      [2];     /* Y / N  (secondary object)         */
    char  namespace_      [20];   /* namespace number as string        */
    char  edition_name   [129];   /* edition (may be empty)            */
    char  sharing        [24];    /* METADATA LINK / DATA LINK / NONE  */
    char  editionable    [2];     /* Y / N                             */
    char  oracle_maintained [2];  /* Y / N                             */
    char  application    [2];     /* Y / N                             */
    char  default_collation[101]; /* default collation                 */
    char  duplicated     [2];     /* Y / N                             */
    char  sharded        [2];     /* Y / N                             */
    char  created_appid  [20];    /* application id as string          */
    char  created_vsnid  [20];    /* version id as string              */
    char  modified_appid [20];    /* application id as string          */
    char  modified_vsnid [20];    /* version id as string              */

} object_metadata_allobjs_t;

/* ================================================================== */
/*  get_object_metadata()                                              */
/*                                                                     */
/*  Query ALL_OBJECTS for a single named object and return its         */
/*  metadata.  Unlike get_table_metadata() (which queries ALL_TABLES   */
/*  and returns nothing for views), this function correctly handles:   */
/*                                                                     */
/*    - Tables                                                         */
/*    - Views                                                          */
/*    - Synonyms                                                       */
/*    - Materialized views                                             */
/*    - DB-link remote objects (as seen in ALL_OBJECTS)               */
/*                                                                     */
/*  This is the correct function to call from get_select_metadata()    */
/*  since SELECT queries are commonly issued against views.            */
/*                                                                     */
/*  Parameters                                                         */
/*    ctx          - OCI context (connection + loggers)                */
/*    object_owner - schema owner, case-insensitive.                   */
/*                   Pass NULL or "" to use ctx->ini->username.        */
/*    object_name  - object name, case-insensitive.                   */
/*                                                                     */
/*  Returns                                                            */
/*    Heap-allocated object_metadata_allobjs_t* on success.           */
/*    Caller must call free_object_metadata() when done.              */
/*    NULL on error or if the object is not found (error is logged).  */
/*                                                                     */
/*  Logging                                                            */
/*    All activity goes to ctx->Metadata_logger.                      */
/*    On success the full struct is dumped at DEBUG level.            */
/* ================================================================== */
object_metadata_allobjs_t *get_object_metadata(oci_context_t *ctx,
                                                const char    *object_owner,
                                                const char    *object_name);

void free_object_metadata(object_metadata_allobjs_t *meta);

#endif /* OCI_TABLE_METADATA_MODULE_H */

