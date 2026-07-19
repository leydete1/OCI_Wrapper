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

#ifdef __cplusplus
}
#endif

#endif /* OCI_RESPONSE_WRITER_H */
