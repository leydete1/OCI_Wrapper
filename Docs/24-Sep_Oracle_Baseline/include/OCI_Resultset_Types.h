/*
 * OCI_Resultset_Types.h
 *
 * Format-agnostic resultset representation
 * -------------------------------------------
 * This is what execute_query_batch()'s fetch loop populates in parallel
 * with the existing XML output (2026-07-18 refactor, Stage 1 of 3 - see
 * Data_Manager_Request_Definitions.docx / OCI_Request_Response_Types.h
 * for the wider design). Once the CRUD layer only ever produces this
 * struct - and the existing inline XML-building code is removed in
 * Stage 3 - execute_query_batch() genuinely knows nothing about XML or
 * JSON; a Response layer renders this into whichever wire format the
 * original request came in as (dml_response_t.resultset_xml_fragment
 * today, a JSON equivalent later, both built from the same struct).
 *
 * Sizing
 * ------
 * record_count and fields_per_row are both known before the fetch loop
 * starts - record_count from Stage 1's row-count guard, fields_per_row
 * from Stage 2's DESCRIBE - so this is allocated exactly once, upfront,
 * by resultset_create(). No growth/realloc logic - fields are SET into
 * fixed slots by index (the same column loop index already used in
 * build_row_xml_batch), not appended. Matches how BLOB_list and the
 * column buffers are already pre-sized in this codebase.
 *
 * BLOB fields
 * -----------
 * Deliberately NOT split into a separate array (2026-07-18 decision) -
 * see the maintainability discussion in project notes: column-order
 * fidelity against the existing XML output (a hard requirement for the
 * Stage 3 comparison) would require synchronizing two arrays back into
 * one sequence, and there is no consumer that benefits from BLOBs being
 * grouped separately - all BLOB processing (LOB read, file write, mime
 * type, url) already happens synchronously per-field during the fetch
 * loop, before this struct is even touched. One ordered array with a
 * per-entry is_blob flag is simpler and safer to maintain.
 *
 * is_blob mirrors xml_add_blob_field_1()'s actual behaviour exactly:
 * that function always emits an empty <field_value/> for BLOB columns
 * and carries the real data in a separate nested <blob> block (file_name/
 * file_path/file_url/file_size/mime_type) - value[] is meaningless when
 * is_blob is set; blob_detail is what's populated instead.
 */

#ifndef OCI_RESULTSET_TYPES_H
#define OCI_RESULTSET_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     file_name[512];
    char     file_path[768];
    char     file_url[768];     /* "" if not set - matches how
                                  * xml_add_blob_field_1() only emits
                                  * <file_url> conditionally             */
    uint64_t file_size;
    char     mime_type[128];
} resultset_blob_detail_t;

typedef struct {
    char  field_name[128];
    char  field_type[32];       /* NUMBER, VARCHAR2, DATE, STRING,
                                  * TIMESTAMP, BLOB, CLOB, UNKNOWN - same
                                  * type_str values build_row_xml_batch
                                  * already computes                     */
    char  value[4096];          /* scalar value, or CLOB's file URL/path.
                                  * Meaningless when is_blob is set.      */
    int   is_blob;              /* 1 = blob_detail populated instead of
                                  * value, matching xml_add_blob_field_1's
                                  * always-empty <field_value/>          */
    resultset_blob_detail_t blob_detail;
} resultset_field_t;

typedef struct {
    int                 record_number;   /* 1-based, matches abs_rownum */
    int                 field_count;     /* == fields_per_row passed to
                                           * resultset_create()          */
    resultset_field_t  *fields;
} resultset_row_t;

typedef struct {
    int              record_count;      /* == record_count passed to
                                          * resultset_create()           */
    resultset_row_t *records;
} resultset_t;

#ifdef __cplusplus
}
#endif

#endif /* OCI_RESULTSET_TYPES_H */
