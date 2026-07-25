/*
 * OCI_Response_Writer.h
 *
 * response_write_xml()
 * ---------------------
 * Stage 2 of the 2026-07-18 refactor plan (see Resultset_Population_
 * Patch.txt for Stage 1). Renders a resultset_t back into exactly the
 * same <resultset>...</resultset> XML shape execute_query_batch()'s
 * inline code already produces today - same tag names, same order,
 * same escaping - so that Stage 3 (comparing the two outputs) is a
 * genuine, meaningful check rather than "are these two documents
 * semantically equivalent but textually different."
 *
 * Deliberately reuses the real, existing helper functions rather than
 * reproducing their formatting by hand:
 *   - xml_start_resultset() / xml_add_row_start() / xml_add_row_end() /
 *     xml_end_resultset() - unchanged, from XML_Helper.h
 *   - xml_add_field()      - unchanged, from XML_Helper.h - used for
 *                             every scalar/CLOB field
 *   - xml_add_blob_field_1() - unchanged, from OCI_Execute_Query_
 *                             Module.h - used for every BLOB field, via
 *                             a locally-constructed, zero-initialised
 *                             lob_item_t populated with only the 6
 *                             fields that function actually reads
 *                             (column_name, file_name,
 *                             output_file_destination, output_file_url,
 *                             blob_size, mime_type - verified against
 *                             its current body, not assumed)
 *
 * This is why Stage 3 should be a clean pass on the first try rather
 * than a source of subtle formatting mismatches to chase down - the
 * actual byte-producing code is the same code in both the old and new
 * path, just called from two different places for now.
 */

#ifndef OCI_RESPONSE_WRITER_H
#define OCI_RESPONSE_WRITER_H

#include "OCI_Connection.h"
#include "OCI_Resultset_Types.h"
#include "resultset_cache.h"
#include "OCI_Request_Response_Types.h"   /* dml_response_t, operation_type_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * response_write_xml()
 *
 * Renders rs into a heap-allocated <resultset>...</resultset> string,
 * identical in shape to what execute_query_batch()'s inline XML code
 * already produces for the same data.
 *
 * ctx is needed only because xml_add_blob_field_1() takes it (for its
 * own internal DEBUG logging via ctx->select_logger) - no other use.
 *
 * Returns a malloc'd string the caller must free(), or NULL on
 * allocation failure or if rs is NULL.
 */
char *response_write_xml(oci_context_t *ctx, const resultset_t *rs);

/*
 * response_write_json()
 *
 * Renders rs into a heap-allocated JSON document, structurally
 * equivalent to response_write_xml()'s output for the same data:
 *
 *   { "resultset": [
 *       { "row_number": N, "fields": [
 *           { "field_name": .., "field_type": .., "field_value": .. },
 *           ...
 *           { "field_name": .., "field_type": "BLOB", "field_value": "",
 *             "blob": { "file_name": .., "file_path": ..,
 *                       ["file_url": ..,] "file_size": .., "mime_type": .. } }
 *       ]},
 *       ...
 *   ]}
 *
 * All scalar values - including NUMBER and DATE typed fields - are
 * emitted as JSON strings, matching resultset_field_t.value being a
 * char[4096] regardless of field_type. This is a deliberate parity
 * choice with response_write_xml(), not an oversight: real JSON typing
 * (numbers as numbers) is a separate future enhancement, tracked
 * separately so it can be evaluated - and tested - on its own.
 *
 * file_url is included only when blob_detail.file_url[0] is set,
 * mirroring xml_add_blob_field_1()'s identical conditional exactly.
 *
 * ctx is accepted for signature parity with response_write_xml() and
 * for future logging use - not currently read.
 *
 * Returns a malloc'd string (via cJSON_PrintUnformatted) the caller
 * must free(), or NULL on allocation failure or if rs is NULL.
 */
char *response_write_json(oci_context_t *ctx, const resultset_t *rs);

/*
 * response_writer_cache_store()
 *
 * Renders rs to JSON (via response_write_json()) and stores it in the
 * resultset cache alongside xml_output - the XML rendering the caller
 * already has for this same resultset - under one cache entry. A
 * later cache hit, in either format, can then be served directly with
 * no re-render and no re-execution of the query.
 *
 * xml_output is NOT re-rendered here; this project currently produces
 * its XML via the legacy inline xml_builder buffer in
 * OCI_Execute_Query_Batch_Module.c, proven byte-identical to
 * response_write_xml()'s output by the Stage 3 check there - so the
 * caller's existing string is reused rather than rendering XML twice.
 *
 * Parameters
 *   ctx            - OCI context, passed through to response_write_json()
 *   cache          - resultset cache instance; pass NULL to render the
 *                    JSON without storing anything (e.g. caching is
 *                    disabled, or this was already served from cache,
 *                    but a JSON response is still needed for this call)
 *   normalised_key - cache key from resultset_cache_make_key(); ignored
 *                    if cache is NULL
 *   rs             - source resultset struct to render to JSON
 *   xml_output     - the caller's already-rendered XML string for this
 *                    resultset; ignored if cache is NULL
 *   row_count      - row count to record on the cache entry
 *   opts           - optional per-entry cache options (may be NULL)
 *   out_json       - required out param; receives a malloc'd JSON
 *                    string the caller owns and must free(). Set to
 *                    NULL if rendering failed.
 *
 * Returns  0  JSON rendered successfully (a cache store failure, when
 *             cache is non-NULL, is logged but treated as non-fatal -
 *             the rendered JSON is still valid and returned)
 *         -1  rendering failed (rs was NULL, or response_write_json()
 *             returned NULL, or out_json was NULL) - *out_json is NULL
 */
int response_writer_cache_store(oci_context_t       *ctx,
                                 cache_t             *cache,
                                 const char          *normalised_key,
                                 const resultset_t   *rs,
                                 const char          *xml_output,
                                 uint64_t             row_count,
                                 cache_entry_opts_t  *opts,
                                 char               **out_json);

/*
 * response_write_dml_xml() / response_write_dml_json()
 *
 * Renders a dml_response_t (see OCI_Request_Response_Types.h) into a
 * heap-allocated XML/JSON string - the first response writers for
 * anything other than a SELECT resultset, added as part of Stage 3's
 * execute_insert_batch() refactor. UPDATE/DELETE will use the exact
 * same two functions once their own Stage 3 work reaches this point -
 * that's the whole reason dml_response_t is one shared struct rather
 * than three near-duplicates (see its own doc comment).
 *
 * op_type selects the tag/key name used for resp->rows_affected, per
 * dml_response_t's own doc comment: the field is generic, but the
 * wire name differs by operation, matching what execute_query_batch()
 * /execute_insert_batch() have always emitted (rows_inserted/
 * rows_updated/rows_deleted) rather than introducing a new generic
 * name that would break compatibility with existing consumers of
 * these XML documents:
 *   OP_SELECT -> not applicable (SELECT's response is its own
 *                <resultset> shape via response_write_xml/json above,
 *                not dml_response_t - passing OP_SELECT here is a
 *                caller error, not a supported case)
 *   OP_INSERT -> rows_inserted
 *   OP_UPDATE -> rows_updated
 *   OP_DELETE -> rows_deleted
 *
 * resp->sql_query / resp->resultset_xml_fragment are ignored - those
 * two fields are SELECT-only per dml_response_t's doc comment, and
 * SELECT doesn't render through this writer at all (see above).
 *
 * Output shape (XML):
 *   <operation>INSERT</operation>
 *   <table_name>...</table_name>
 *   <owner>...</owner>
 *   <rows_inserted>N</rows_inserted>
 *   <lobs_written>N</lobs_written>
 *   <execution_time>%.6f</execution_time>
 *
 * Output shape (JSON) - same fields, snake_case keys, rows_affected/
 * lobs_written/execution_time as real JSON numbers (unlike resultset
 * field values, these are operation metadata/counts, not opaque
 * database column values needing exact-string preservation, so there's
 * no equivalent reason to keep them as strings):
 *   { "operation": "INSERT", "table_name": "...", "owner": "...",
 *     "rows_inserted": N, "lobs_written": N, "execution_time": %.6f }
 *
 * Deliberately does NOT include execute_batch_size (an old, execute-
 * insert-batch-internal implementation detail present in the old
 * inline XML) - not part of dml_response_t, and not part of the
 * client-facing contract going forward.
 *
 * Returns a malloc'd string the caller must free(), or NULL if resp is
 * NULL or op_type is OP_SELECT.
 */
char *response_write_dml_xml (oci_context_t *ctx, operation_type_t op_type,
                               const dml_response_t *resp);
char *response_write_dml_json(oci_context_t *ctx, operation_type_t op_type,
                               const dml_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* OCI_RESPONSE_WRITER_H */
