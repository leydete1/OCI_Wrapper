/*
 * OCI_Resultset_Builder.h
 *
 * Small, standalone helper API for populating a resultset_t. Deliberately
 * minimal - this module only ever builds the struct; it has no idea
 * what will eventually read it (an XML writer, a JSON writer, nothing
 * yet) and no idea where the data came from (OCI, or in principle any
 * other database driver). Matches the project convention of keeping
 * concerns in small, independent modules (see OCI_Session_Manager.h/.c
 * for the same pattern applied elsewhere).
 *
 * Every function here is a straightforward, allocation-checked setter -
 * no OCI types, no XML, no format awareness at all. See OCI_Resultset_
 * Types.h for the struct definitions and the design reasoning behind
 * them (why BLOB isn't a separate array, why sizing is fixed upfront).
 */

#ifndef OCI_RESULTSET_BUILDER_H
#define OCI_RESULTSET_BUILDER_H

#include "OCI_Resultset_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * resultset_create()
 *
 * Allocates a resultset_t sized exactly for record_count rows, each
 * with fields_per_row field slots - both dimensions already known
 * before the fetch loop starts (see OCI_Resultset_Types.h). Every
 * row's record_number is pre-set to its 1-based position (1..record_count)
 * and every field slot starts zeroed (empty strings, is_blob=0).
 *
 * Returns NULL on allocation failure - caller must check before using.
 */
resultset_t *resultset_create(int record_count, int fields_per_row);

/*
 * resultset_get_row()
 *
 * Returns a pointer to the row for record_number (1-based, matching
 * abs_rownum in execute_query_batch's fetch loop), or NULL if
 * record_number is out of range. Does not allocate - rs must already
 * have been sized large enough by resultset_create().
 */
resultset_row_t *resultset_get_row(resultset_t *rs, int record_number);

/*
 * resultset_set_field()
 *
 * Sets a scalar/CLOB field at field_index (0-based, the same column
 * loop index already used in build_row_xml_batch) within row. value is
 * copied as-is - for CLOB, pass exactly the same string already being
 * passed to xml_add_field() (the file URL/path), so the two outputs
 * stay identical during the Stage 3 comparison.
 *
 * Returns 0 on success, -1 if row is NULL or field_index is out of
 * range for the field_count resultset_create() was given.
 */
int resultset_set_field(resultset_row_t *row,
                         int              field_index,
                         const char      *field_name,
                         const char      *field_type,
                         const char      *value);

/*
 * resultset_set_blob_field()
 *
 * Sets a BLOB field at field_index. Mirrors exactly what
 * xml_add_blob_field_1() writes - pass the same file_name/file_path/
 * file_url/file_size/mime_type values already going into that call, so
 * the two outputs stay identical. file_url may be NULL/empty (matches
 * xml_add_blob_field_1() only emitting <file_url> conditionally).
 *
 * Returns 0 on success, -1 if row is NULL or field_index is out of range.
 */
int resultset_set_blob_field(resultset_row_t *row,
                              int              field_index,
                              const char      *field_name,
                              const char      *file_name,
                              const char      *file_path,
                              const char      *file_url,
                              uint64_t         file_size,
                              const char      *mime_type);

/*
 * resultset_free()
 *
 * Frees rs and every row's fields array. Safe to call with NULL.
 */
void resultset_free(resultset_t *rs);

#ifdef __cplusplus
}
#endif

#endif /* OCI_RESULTSET_BUILDER_H */
