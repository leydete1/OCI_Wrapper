/*
 * OCI_Blob_Utils.h
 *
 * BLOB file-writing helpers.
 *
 * Relocated from OCI_Execute_Query_Module.c/.h when that module was
 * removed (execute_query(), get_row_count(), and lookup_blob_index_1()
 * were dead - superseded by execute_query_batch() and its own count
 * query - but these four functions were still called by
 * OCI_Execute_Query_Batch_Module.c and OCI_Execute_Procedure_Module.c,
 * so they needed a home that survives the removal.
 */

#ifndef OCI_BLOB_UTILS_H
#define OCI_BLOB_UTILS_H

#include <stddef.h>
#include "OCI_Connection.h"   /* oci_context_t, lob_item_t */

/* Find the index of col_name within a fixed-width [][256] column name
 * array. Returns -1 if not found. */
int lookup_blob_index(char (*col_names)[256], int col_count,
                       const char *col_name, oci_context_t *ctx);

/* Write item->blob_data (item->blob_size bytes) to output_dir/item->file_name.
 * Returns 0 on success, -1 on invalid input or file open failure. */
int write_blob_to_file(lob_item_t *item, const char *output_dir,
                        oci_context_t *ctx);

/* Format the current local time as YYYYMMDD_HHMMSS into buffer. */
void generate_timestamp(char *buffer, size_t size, oci_context_t *ctx);

/* Build output = "<name>_<timestamp>_<idx><ext>" from original's name/ext,
 * used to disambiguate multiple BLOB output files per row. */
void build_filename_with_timestamp(const char *original, char *output,
                                    size_t out_size, int idx,
                                    oci_context_t *ctx);

#endif /* OCI_BLOB_UTILS_H */
