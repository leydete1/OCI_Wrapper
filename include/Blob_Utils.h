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
#include "Connection.h"   /* oci_context_t, lob_item_t */

/* Find the index of col_name within a fixed-width [][256] column name
 * array. Returns -1 if not found. */
int lookup_blob_index(char (*col_names)[256], int col_count,
                       const char *col_name, oci_context_t *ctx);

/* Write item->blob_data (item->blob_size bytes) to output_dir/item->file_name.
 * 2026-10-14 (point a3): written as <file_name>.part and renamed into
 * place, so a reader sees the whole file or none of it.
 * Returns 0 on success, -1 on invalid input, open, write, close or
 * rename failure (the .part file is removed). */
int write_blob_to_file(lob_item_t *item, const char *output_dir,
                        oci_context_t *ctx);

/* Format the current local time as YYYYMMDD_HHMMSS into buffer.
 * ctx is not used and may be NULL. */
void generate_timestamp(char *buffer, size_t size, oci_context_t *ctx);

/* 2026-10-14 (point a1) - one tag per SELECT that writes LOB files:
 * "YYYYMMDD_HHMMSS_NNNNNN" (local time plus a counter shared by every
 * thread of the process, 000001-999999 then round again). Two SELECTs
 * never get the same tag unless a million SELECTs start within one
 * second, so file names built from it never collide. out_size should
 * be at least LOB_OUTPUT_TAG_SIZE. */
#define LOB_OUTPUT_TAG_SIZE 32
void lob_output_tag(char *out, size_t out_size);

/* Build output = "<name>_<tag>_<idx><ext>" from original's name/ext.
 * 2026-10-14 (point a2): tag is the SELECT's lob_output_tag(); it used
 * to be a bare YYYYMMDD_HHMMSS taken here, which two requests in the
 * same second shared. */
void build_filename_with_timestamp(const char *original, const char *tag,
                                    char *output, size_t out_size, int idx,
                                    oci_context_t *ctx);

#endif /* OCI_BLOB_UTILS_H */
