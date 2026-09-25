/*
 * OCI_Clob_Utils.c
 *
 * See OCI_Clob_Utils.h for the relocation rationale. Both functions
 * below are copied verbatim (same snprintf format strings, same ini
 * keys, same precedence) from handle_clob_column_batch()'s "Build
 * output filename and path" / "Build URL for XML field" sections
 * (OCI_Execute_Query_Batch_Module.c) - a mechanical extraction, not a
 * rewrite, so existing on-disk output is unaffected.
 */

#include <stdio.h>
#include <string.h>

#include "Clob_Utils.h"

void build_clob_filename(const char *col_name, unsigned int abs_rownum,
                          int clob_index, char *output, size_t out_size,
                          oci_context_t *ctx)
{
    const char *ext = (ctx->ini->clob_default_extension[0] != '\0')
                      ? ctx->ini->clob_default_extension
                      : ".txt";

    snprintf(output, out_size, "%s_row%u_clob%d%s",
             col_name, abs_rownum, clob_index, ext);
}

void build_clob_url(const char *filename, const char *filepath,
                     char *output, size_t out_size, oci_context_t *ctx)
{
    if (ctx->ini->xml_share_CLOB_URL_path && ctx->ini->CLOB_URL_path[0])
    {
        snprintf(output, out_size, "%s/%s", ctx->ini->CLOB_URL_path, filename);
    }
    else
    {
        snprintf(output, out_size, "%s", filepath);
    }
}
