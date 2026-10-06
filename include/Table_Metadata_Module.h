/*
 * Table_Metadata_Module.h
 *
 * Table metadata - core entry point
 * ----------------------------------
 * Stage 4a (Oracle dialect extraction, 2026-10-06). This module used to
 * hold the Oracle data-dictionary code itself (ALL_TAB_COLUMNS,
 * ALL_TABLES, ALL_OBJECTS, OCIParamGet/OCIDefineByPos). That code now
 * lives in the driver:
 *
 *   get_request_metadata()  ->  db_driver_t.describe_table
 *                               (driver_oracle.c: oracle_describe_table)
 *   get_multi_metadata()    ->  private to driver_oracle.c
 *                               (oracle_define_columns, used by
 *                               select_open / select_open_from_cursor)
 *
 * Deleted outright - no production caller:
 *   get_select_metadata(), get_table_metadata(), get_object_metadata(),
 *   free_table_metadata(), free_object_metadata(), multi_meta_request_t,
 *   table_metadata_alltabs_t, object_metadata_allobjs_t.
 *
 * get_request_metadata() stays as the one function core calls, so its
 * callers (metadata_cache.c, Driver_Metadata_Test.c,
 * Driver_Level2Parser_Test.c) did not change. It is now a thin wrapper
 * that dispatches to the active driver.
 *
 * Normal callers should go through metadata_cache_get_or_fetch()
 * (metadata_cache_meta.h), which serves cached results and only calls
 * get_request_metadata() on a miss.
 */

#ifndef OCI_TABLE_METADATA_MODULE_H
#define OCI_TABLE_METADATA_MODULE_H

#include "db_metadata.h"               /* col_metadata_t, metadata_request_t,
                                           MAX_TABLE_COLUMNS                */
#include "Connection.h"                /* oci_context_t                    */
#include "sql_dependency_extractor.h"  /* kept: some includers of this
                                           header still pick up
                                           OCI_DEPENDENCY_LIST through it   */

/*
 * get_request_metadata()
 *
 * Describe one table: its columns in column order, with type name,
 * length, precision, scale, nullability and default.
 *
 * If req->owner is empty the driver resolves it and writes it back into
 * req->owner. Both req fields are upper-cased in place.
 *
 * Parameters
 *   ctx       - connection context; ctx->Metadata_logger must be set
 *   req       - table name and optional owner (modified in place)
 *   cols      - caller-allocated array of at least max_cols entries
 *   col_count - set to the number of columns found on success
 *   max_cols  - size of cols[] (use MAX_TABLE_COLUMNS)
 *
 * Returns
 *    0  success
 *   -1  error, or table not found (logged to ctx->Metadata_logger)
 */
int get_request_metadata(oci_context_t      *ctx,
                         metadata_request_t *req,
                         col_metadata_t     *cols,
                         int                *col_count,
                         int                 max_cols);

#endif /* OCI_TABLE_METADATA_MODULE_H */
