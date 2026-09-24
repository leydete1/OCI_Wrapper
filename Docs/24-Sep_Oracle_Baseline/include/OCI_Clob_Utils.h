/*
 * OCI_Clob_Utils.h
 *
 * CLOB filename/URL helpers - extracted from handle_clob_column_batch()
 * (OCI_Execute_Query_Batch_Module.c) as part of the driver-extraction
 * v2 work (2026-09-13). CLOB's file-naming/URL-building logic was
 * previously inline, mixed in with the actual OCILobRead calls - unlike
 * BLOB, which already had this split out into OCI_Blob_Utils.c. This
 * file brings CLOB to the same shape: zero OCI dependency, callable
 * from anywhere (core or a driver), same as OCI_Blob_Utils.c already
 * is.
 *
 * Deliberately NOT a write_clob_to_file() alongside these - checked
 * OCI_Blob_Utils.c's write_blob_to_file() first: it's already fully
 * byte-agnostic (fopen/fwrite/fclose on item->blob_data/item->blob_size,
 * nothing BLOB-specific in it at all), so it already serves CLOB bytes
 * unchanged. Duplicating it here would just be the same function twice.
 * Callers building a CLOB lob_item_t (file_name from
 * build_clob_filename() below, blob_data/blob_size from the CLOB read)
 * call write_blob_to_file() directly - see OCI_Blob_Utils.h.
 */

#ifndef OCI_CLOB_UTILS_H
#define OCI_CLOB_UTILS_H

#include <stddef.h>
#include "OCI_Connection.h"   /* oci_context_t */

/*
 * build_clob_filename()
 *
 * Preserves handle_clob_column_batch()'s exact original pattern -
 * "<col_name>_row<abs_rownum>_clob<clob_index><ext>" - byte-for-byte
 * unchanged, so existing output (file names already on disk, anything
 * that parses them) does not shift under this extraction. ext comes
 * from ctx->ini->clob_default_extension, defaulting to ".txt" when
 * that's unset - same default the inline code already used.
 */
void build_clob_filename(const char *col_name, unsigned int abs_rownum,
                          int clob_index, char *output, size_t out_size,
                          oci_context_t *ctx);

/*
 * build_clob_url()
 *
 * Preserves handle_clob_column_batch()'s exact original policy: if
 * ctx->ini->xml_share_CLOB_URL_path is set AND CLOB_URL_path is
 * non-empty, build "<CLOB_URL_path>/<filename>"; otherwise fall back to
 * the plain filepath as-is. Same two ini keys, same precedence, same
 * fallback - a mechanical extraction, not a policy change.
 */
void build_clob_url(const char *filename, const char *filepath,
                     char *output, size_t out_size, oci_context_t *ctx);

#endif /* OCI_CLOB_UTILS_H */
