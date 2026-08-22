/*
 * OCI_Execute_Query_Batch_Module.c
 *
 * Array-fetch batch SELECT module.
 * Produces identical XML output to execute_query() but fetches
 * ctx->ini->query_fetch_batch_size rows per round-trip instead of one,
 * dramatically reducing network overhead on larger result sets.
 *
 * Structure
 * ---------
 *   allocate_batch_buffers()     - heap allocate column buffers & indicators
 *   get_multi_metadata()         - OCIParamGet metadata + OCIDefineByPos
 *                                  + OCIDefineArrayOfStruct per column
 *                                  NOW IN OCI_Table_Metadata_Module.c
 *   handle_clob_column_batch()   - read one CLOB cell, write to disk, emit XML
 *   handle_blob_column_batch()   - read one BLOB cell, write to disk, emit XML
 *   build_row_xml_batch()        - iterate columns for one logical row
 *   execute_query_batch()        - orchestrate: validate -> prepare ->
 *                                  describe -> execute -> fetch loop -> XML
 *
 * Metadata change
 * ---------------
 * define_columns_batch() has been removed from this file.  Its logic
 * now lives in get_multi_metadata() inside OCI_Table_Metadata_Module.c.
 * This means all metadata code for the project lives in one place.
 * When the metadata cache is introduced, single-table SELECTs can be
 * served from cache via get_request_metadata() with no changes here.
 * Multi-table JOINs and views continue to use OCI descriptor metadata
 * via get_multi_metadata() - the correct approach for those cases.
 * See OCI_Table_Metadata_Module.h for the full design rationale.
 *
 * Changes from previous version
 * ------------------------------
 *   1. COUNT query uses SELECT 1 wrapper to avoid ORA-00932 with CLOB columns
 *   2. BLOB NULL/empty guard: indicator checked BEFORE OCILobGetLength
 *   3. CLOB handler writes to CLOB_output_dir with clob_default_extension
 *   4. Multiple CLOBs per record tracked via CLOB_index (mirrors BLOB pattern)
 *   5. define_columns_batch() moved to OCI_Table_Metadata_Module.c as
 *      get_multi_metadata()
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>
#include <inttypes.h>                    /* PRIu64 for row_count logging  */

#include "XML_Helper.h"
#include "OCI_Connection.h"
#include "OCI_Execute_Query_Batch_Module.h"
#include <string_utils.h>               /* trim_sql_inplace()                         */
#include "OCI_Blob_Utils.h"              /* lookup_blob_index(), write_blob_to_file(),
                                            build_filename_with_timestamp() - relocated
                                            from the now-removed OCI_Execute_Query_Module */
#include "OCI_Table_Metadata_Module.h"   /* get_multi_metadata() get_select_metadata() */
#include "sql_dependency_extractor.h"    /* extract_sql_dependencies()                 */
#include "logger.h"
#include "resultset_cache.h"
#include "OCI_Transaction_Manager.h"
#include "metrics.h"
#include "metrics_writer.h"   /* metrics_finalise_and_enqueue() - closure item 5, Stage 2 */
#include "OCI_Resultset_Builder.h"
#include "OCI_Response_Writer.h";
#include "cJSON.h"                       /* Stage 3c JSON verification only */

/* ------------------------------------------------------------------ */
/*  Local OCI error macro - mirrors execute_query style                */
/* ------------------------------------------------------------------ */
#define CHECK_OCI(errhp, status) \
    if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
        text errbuf[512]; sb4 errcode = 0; \
        OCIErrorGet((errhp), 1, NULL, &errcode, errbuf, \
                    sizeof(errbuf), OCI_HTYPE_ERROR); \
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0, \
                     "OCI Error %d: %s", errcode, (char *)errbuf); \
    }

/* ------------------------------------------------------------------ */
/*  Internal batch context - groups all per-column arrays together     */
/* ------------------------------------------------------------------ */
typedef struct {
    ub4              col_count;
    ub4              fetch_count;     /* rows per fetch batch           */

    OCIDefine      **def;             /* [col]                          */
    char           **buffers;         /* [col] flat [fetch_count*bsz]   */
    ub4             *buf_sizes;       /* [col] individual buffer width   */
    sb2            **indicators;      /* [col] -> [fetch_count]          */
    ub2             *data_types;      /* [col]                           */
    ub4             *data_sizes;      /* [col] from OCI metadata         */
    char           (*col_names)[256]; /* [col]                           */

    /*
     * BLOB locators: flat array per column, one slot per row in the batch.
     * col_blob_locs[col] points to a contiguous block of fetch_count
     * OCILobLocator* handles. OCI strides through this block using
     * sizeof(OCILobLocator*) as the value_skip in OCIDefineArrayOfStruct.
     * Access row r of column c as: col_blob_locs[c][r]
     */
    OCILobLocator ***col_blob_locs;   /* [col] -> flat [fetch_count]     */

    /* Single CLOB locator reused per row (CLOBs not array-fetchable)   */
    OCILobLocator   *clob_loc;
} batch_ctx_t;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static int  allocate_batch_buffers(oci_context_t *ctx, batch_ctx_t *bc);

static int handle_clob_column_batch(oci_context_t *ctx,
        batch_ctx_t   *bc,
        ub4            col_idx,
        unsigned int   abs_rownum,
        int           *CLOB_index_ptr,
        int            max_clobs,
        uint64_t      *clob_bytes_acc,
        xml_builder_t *xml,
        resultset_row_t *rs_row,
        int              field_index);

static int handle_blob_column_batch(oci_context_t *ctx,
                                         batch_ctx_t   *bc,
                                         ub4            row_in_batch,
                                         ub4            col_idx,
                                         unsigned int   abs_rownum,
                                         lob_item_t    *BLOB_list,
                                         int           *BLOB_index_ptr,
                                         int            max_lobs,
                                         xml_builder_t *xml,
                                         resultset_row_t *rs_row,
                                         int              field_index);


static int  build_row_xml_batch(oci_context_t *ctx,
										batch_ctx_t   *bc,
										ub4            row_in_batch,
										unsigned int   abs_rownum,
										lob_item_t    *BLOB_list,
										int           *BLOB_index_ptr,
										int            max_lobs,
										int           *CLOB_index_ptr,
										int            max_clobs,
										uint64_t      *clob_bytes_acc,
										xml_builder_t *xml,
										resultset_t   *rs);
static void free_batch_ctx          (oci_context_t *ctx, batch_ctx_t *bc);


/* ================================================================== */
/*  1.  allocate_batch_buffers                                         */
/*      Heap-allocate all per-column arrays sized for fetch_count rows */
/* ================================================================== */
static int allocate_batch_buffers(oci_context_t *ctx, batch_ctx_t *bc)
{
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering: col_count=%u fetch_count=%u",
                 bc->col_count, bc->fetch_count);

    bc->def           = calloc(bc->col_count, sizeof(OCIDefine *));
    bc->buffers       = calloc(bc->col_count, sizeof(char *));
    bc->buf_sizes     = calloc(bc->col_count, sizeof(ub4));
    bc->indicators    = calloc(bc->col_count, sizeof(sb2 *));
    bc->data_types    = calloc(bc->col_count, sizeof(ub2));
    bc->data_sizes    = calloc(bc->col_count, sizeof(ub4));
    bc->col_names     = calloc(bc->col_count, sizeof(*bc->col_names));
    bc->col_blob_locs = calloc(bc->col_count, sizeof(OCILobLocator **));

    if (!bc->def        || !bc->buffers     || !bc->buf_sizes   ||
        !bc->indicators || !bc->data_types  || !bc->data_sizes  ||
        !bc->col_names  || !bc->col_blob_locs)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Top-level calloc failed");
        return -1;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Top-level arrays allocated OK");
    return 0;
}

/*
 * NOTE: define_columns_batch() has been removed from this file.
 * Its logic now lives in get_multi_metadata() inside
 * OCI_Table_Metadata_Module.c.  The call site below passes all
 * bc array pointers via multi_meta_request_t.  The fetch loop,
 * free_batch_ctx(), and all other code in this file are unchanged.
 */


/* ================================================================== */
/*  2.  handle_clob_column_batch                                       */
/*      Read one CLOB cell (already in bc->clob_loc), write to disk,  */
/*      emit XML field with file URL - mirrors BLOB handler pattern.   */
/*      NULL and empty CLOB both emit empty XML field safely.          */
/* ================================================================== */
static int handle_clob_column_batch(oci_context_t *ctx,
                                         batch_ctx_t   *bc,
                                         ub4            col_idx,
                                         unsigned int   abs_rownum,
                                         int           *CLOB_index_ptr,
                                         int            max_clobs,
                                         uint64_t      *clob_bytes_acc,
                                         xml_builder_t *xml,
                                         resultset_row_t *rs_row,
                                         int              field_index)
{
    int CLOB_index = *CLOB_index_ptr;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering col=%u name=%s abs_rownum=%u CLOB_index=%d",
                 col_idx, bc->col_names[col_idx], abs_rownum, CLOB_index);

    if (CLOB_index >= max_clobs)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "CLOB overflow: index=%d max=%d", CLOB_index, max_clobs);
        return -1;
    }

    /* ---- NULL check BEFORE OCILobGetLength ---- */
    if (bc->indicators[col_idx][0] == -1)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "CLOB col=%u is NULL, emitting empty field", col_idx);
        /* xml_add_field(xml, bc->col_names[col_idx], "CLOB", ""); */ /* Unused: XML now built from response_write_xml(ctx, rs) via new parsing layer */
        resultset_set_field(rs_row, field_index, bc->col_names[col_idx], "CLOB", "");   /* ADD */
        (*CLOB_index_ptr)++;
        return 0;
    }

    /* ---- Get CLOB length ---- */
    ub4 lob_len = 0;
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCILobGetLength col=%u", col_idx);
    CHECK_OCI(ctx->errhp,
        OCILobGetLength(ctx->svchp, ctx->errhp,
                        bc->clob_loc, &lob_len));

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "CLOB col=%u lob_len=%u CLOB_index=%d",
                 col_idx, lob_len, CLOB_index);

    /* ---- Empty CLOB check ---- */
    if (lob_len == 0)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "CLOB col=%u is empty, emitting empty field", col_idx);
        /* xml_add_field(xml, bc->col_names[col_idx], "CLOB", ""); */ /* Unused: XML now built from response_write_xml(ctx, rs) via new parsing layer */
        resultset_set_field(rs_row, field_index, bc->col_names[col_idx], "CLOB", "");   /* ADD */
        (*CLOB_index_ptr)++;
        return 0;
    }

    /* ---- Allocate read buffer ---- */
    char *clob_buf = calloc(1, lob_len + 1);
    if (!clob_buf)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "calloc failed for CLOB buffer size=%u", lob_len + 1);
        return -1;
    }

    /* ---- Chunked read ---- */
    ub4   offset          = 1;
    ub4   bytes_remaining = lob_len;
    char *write_ptr       = clob_buf;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Starting chunked CLOB read total=%u chunk_size=%u",
                 lob_len, ctx->ini->chunk_read_size);

    while (bytes_remaining > 0)
    {
        ub4 chunk  = (ub4)ctx->ini->chunk_read_size;
        if (chunk > bytes_remaining) chunk = bytes_remaining;
        ub4 amount = chunk;

        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                     "OCILobRead offset=%u chunk=%u remaining=%u",
                     offset, chunk, bytes_remaining);

        /* Bug fix (2026-08-06): previously passed the OCILobRead(...)
         * call directly into CHECK_OCI, which only LOGS a failure - it
         * never breaks control flow. Since 'amount' is pre-set to
         * 'chunk' above and OCI typically leaves an output parameter
         * unchanged on failure, a failed read wasn't reliably caught by
         * the "if (amount == 0)" check below (amount would still equal
         * the requested chunk size, not 0) - the loop would silently
         * treat a failed read as a successful one, advance past it
         * with zero-filled (uninitialised-content) buffer space, and
         * eventually exhaust bytes_remaining normally: no infinite
         * loop, but a genuinely corrupted CLOB result with no error
         * ever surfaced to the caller. Capturing the real return code
         * and checking it explicitly, rather than relying on
         * CHECK_OCI's logging-only behaviour, fixes that.             */
        sword lob_read_rc = OCILobRead(ctx->svchp, ctx->errhp,
                       bc->clob_loc,
                       &amount, offset,
                       write_ptr, chunk,
                       NULL, NULL, 0, SQLCS_IMPLICIT);
        CHECK_OCI(ctx->errhp, lob_read_rc);

        if (lob_read_rc != OCI_SUCCESS && lob_read_rc != OCI_SUCCESS_WITH_INFO)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "OCILobRead failed (rc=%d) at offset=%u - "
                         "aborting this CLOB read rather than silently "
                         "treating the failure as success",
                         (int)lob_read_rc, offset);
            free(clob_buf);
            return -1;
        }

        if (amount == 0)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "OCILobRead returned 0 bytes unexpectedly");
            free(clob_buf);
            return -1;
        }

        write_ptr       += amount;
        offset          += amount;
        bytes_remaining -= amount;
    }

    clob_buf[lob_len] = '\0';

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "CLOB read complete col=%u total=%u", col_idx, lob_len);

    if (clob_bytes_acc) *clob_bytes_acc += (uint64_t)lob_len;

    /* ---- Build output filename and path ---- */
    const char *ext = (ctx->ini->clob_default_extension[0] != '\0')
                      ? ctx->ini->clob_default_extension
                      : ".txt";

    char clob_filename[512];
    char clob_filepath[768];

    snprintf(clob_filename, sizeof(clob_filename),
             "%s_row%u_clob%d%s",
             bc->col_names[col_idx],
             abs_rownum,
             CLOB_index,
             ext);

    snprintf(clob_filepath, sizeof(clob_filepath),
             "%s/%s",
             ctx->ini->CLOB_output_dir,
             clob_filename);

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Writing CLOB to file: %s", clob_filepath);

    /* ---- Write to disk ---- */
    FILE *fp = fopen(clob_filepath, "w");
    if (!fp)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Failed to open CLOB output file: %s - "
                     "emitting inline content", clob_filepath);
        /* xml_add_field(xml, bc->col_names[col_idx], "CLOB", clob_buf); */ /* Unused: XML now built from response_write_xml(ctx, rs) via new parsing layer */
        resultset_set_field(rs_row, field_index, bc->col_names[col_idx], "CLOB", clob_buf);   /* ADD */
        free(clob_buf);
        (*CLOB_index_ptr)++;
        return 0;
    }

    size_t written = fwrite(clob_buf, 1, lob_len, fp);
    fclose(fp);
    free(clob_buf);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "CLOB written to disk: %s bytes=%zu",
                 clob_filepath, written);

    /* ---- Build URL for XML field ---- */
    char clob_url[768];
    if (ctx->ini->xml_share_CLOB_URL_path && ctx->ini->CLOB_URL_path[0])
    {
        snprintf(clob_url, sizeof(clob_url),
                 "%s/%s",
                 ctx->ini->CLOB_URL_path,
                 clob_filename);
    }
    else
    {
        snprintf(clob_url, sizeof(clob_url), "%s", clob_filepath);
    }

    /* Emit field: value is the URL/path to the written file */
    /* xml_add_field(xml, bc->col_names[col_idx], "CLOB", clob_url); */ /* Unused: XML now built from response_write_xml(ctx, rs) via new parsing layer */
    resultset_set_field(rs_row, field_index, bc->col_names[col_idx], "CLOB", clob_url);   /* ADD */

    (*CLOB_index_ptr)++;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "handle_clob_column_batch complete CLOB_index=%d",
                 *CLOB_index_ptr);
    return 0;
}


/* ================================================================== */
/*  3.  handle_blob_column_batch                                       */
/*      Read BLOB at col_blob_locs[col_idx][row_in_batch],            */
/*      write to disk, emit XML.                                       */
/*      NULL/empty guard BEFORE OCILobGetLength - prevents segfault   */
/*      on NULL locator and emits consistent empty XML field.          */
/* ================================================================== */
static int handle_blob_column_batch(oci_context_t *ctx,
                                     batch_ctx_t   *bc,
                                     ub4            row_in_batch,
                                     ub4            col_idx,
                                     unsigned int   abs_rownum,
                                     lob_item_t    *BLOB_list,
                                     int           *BLOB_index_ptr,
                                     int            max_lobs,
                                     xml_builder_t *xml,
                                     resultset_row_t *rs_row,
                                     int              field_index)
{
    int BLOB_index = *BLOB_index_ptr;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering col=%u row_in_batch=%u abs_rownum=%u BLOB_index=%d",
                 col_idx, row_in_batch, abs_rownum, BLOB_index);

    if (BLOB_index >= max_lobs)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "BLOB overflow: index=%d max=%d", BLOB_index, max_lobs);
        return -1;
    }

    lob_item_t *item = &BLOB_list[BLOB_index];

    /* ---- NULL check BEFORE any OCI LOB calls ---- */
    item->is_null = (bc->indicators[col_idx][row_in_batch] == -1);

    if (item->is_null)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "BLOB col=%u row=%u is NULL, emitting empty field",
                     col_idx, row_in_batch);
        /* xml_add_field(xml, bc->col_names[col_idx], "BLOB", ""); */ /* Unused: XML now built from response_write_xml(ctx, rs) via new parsing layer */
        resultset_set_field(rs_row, field_index, bc->col_names[col_idx], "BLOB", "");
     (*BLOB_index_ptr)++;
        return 0;
    }

    /* ---- Allocate locator and assign from column array ---- */
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCIDescriptorAlloc for item->lob_loc");
    CHECK_OCI(ctx->errhp,
        OCIDescriptorAlloc(ctx->envhp,
                           (void **)&item->lob_loc,
                           OCI_DTYPE_LOB, 0, NULL));

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCILobLocatorAssign col=%u row=%u",
                 col_idx, row_in_batch);
    CHECK_OCI(ctx->errhp,
        OCILobLocatorAssign(ctx->svchp, ctx->errhp,
                            bc->col_blob_locs[col_idx][row_in_batch],
                            &item->lob_loc));

    /* ---- Get BLOB size (safe - not NULL) ---- */
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCILobGetLength");
    CHECK_OCI(ctx->errhp,
        OCILobGetLength(ctx->svchp, ctx->errhp,
                        item->lob_loc, &item->blob_size));

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "BLOB col=%u row=%u size=%u index=%d",
                 col_idx, row_in_batch, item->blob_size, BLOB_index);

    /* ---- Empty BLOB check ---- */
    if (item->blob_size == 0)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "BLOB col=%u row=%u is empty, emitting empty field",
                     col_idx, row_in_batch);
        /* xml_add_field(xml, bc->col_names[col_idx], "BLOB", ""); */ /* Unused: XML now built from response_write_xml(ctx, rs) via new parsing layer */
        resultset_set_field(rs_row, field_index, bc->col_names[col_idx], "BLOB", "");   /* ADD */
        (*BLOB_index_ptr)++;
        return 0;
    }

    /* ---- Read BLOB data in chunks ---- */
    ub4  total_size = item->blob_size;
    ub4  offset     = 1;

    item->blob_data = malloc(total_size);
    if (!item->blob_data)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "malloc failed for BLOB data size=%u", total_size);
        return -1;
    }

    ub4  bytes_remaining = total_size;
    ub1 *write_ptr       = item->blob_data;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Starting chunked BLOB read total=%u chunk_size=%u",
                 total_size, ctx->ini->chunk_read_size);

    while (bytes_remaining > 0)
    {
        ub4 chunk  = (ub4)ctx->ini->chunk_read_size;
        if (chunk > bytes_remaining) chunk = bytes_remaining;
        ub4 amount = chunk;

        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                     "OCILobRead offset=%u chunk=%u remaining=%u",
                     offset, chunk, bytes_remaining);

        CHECK_OCI(ctx->errhp,
            OCILobRead(ctx->svchp, ctx->errhp,
                       item->lob_loc,
                       &amount, offset,
                       write_ptr, chunk,
                       NULL, NULL, 0, SQLCS_IMPLICIT));

        if (amount == 0)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "OCILobRead returned 0 bytes unexpectedly");
            return -1;
        }

        write_ptr       += amount;
        offset          += amount;
        bytes_remaining -= amount;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "BLOB read complete total=%u", total_size);

    /* ---- Filename, MIME type and metadata ---- */
    const char *search_col = ctx->ini->BLOB_default_file_name_col;

    switch (BLOB_index)
    {
        case 1: search_col = ctx->ini->BLOB_default_file_name_col_1; break;
        case 2: search_col = ctx->ini->BLOB_default_file_name_col_2; break;
        case 3: search_col = ctx->ini->BLOB_default_file_name_col_3; break;
        case 4: search_col = ctx->ini->BLOB_default_file_name_col_4; break;
        case 5: search_col = ctx->ini->BLOB_default_file_name_col_5; break;
    }

    int name_col_idx = lookup_blob_index(bc->col_names,
                                         (int)bc->col_count,
                                         search_col, ctx);

    char final_name[512];

    if (name_col_idx >= 0 &&
        bc->indicators[name_col_idx][row_in_batch] != -1 &&
        bc->buffers[name_col_idx] != NULL)
    {
        const char *name_val =
            bc->buffers[name_col_idx] +
            ((size_t)row_in_batch * bc->buf_sizes[name_col_idx]);

        if (ctx->ini->BLOB_append_file_timestamp == 1)
        {
            logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                         "Calling build_filename_with_timestamp");
            build_filename_with_timestamp(name_val,
                                          final_name,
                                          sizeof(final_name),
                                          BLOB_index, ctx);
        }
        else
        {
            snprintf(final_name, sizeof(final_name), "%s", name_val);
        }
    }
    else
    {
        snprintf(final_name, sizeof(final_name),
                 "%s_%d",
                 ctx->ini->BLOB_default_file_name,
                 BLOB_index);
    }

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "final_name=%s", final_name);

    item->column_name  = bc->col_names[col_idx];
    item->file_name    = strdup(final_name);
    item->mime_type    = strdup(get_mime_type(item->file_name));
    item->column_index = BLOB_index;

    item->output_file_url =
        ctx->ini->xml_share_BLOB_URL_path ?
        ctx->ini->BLOB_URL_path : strdup("N/A");

    item->output_file_destination =
        ctx->ini->xml_share_BLOB_host_path ?
        ctx->ini->BLOB_output_dir : strdup("N/A");

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Calling write_blob_to_file");
    write_blob_to_file(item, ctx->ini->BLOB_output_dir, ctx);

    /* xml_add_blob_field_1(xml, item, ctx); */ /* Unused: XML now built from response_write_xml(ctx, rs) via new parsing layer */
    resultset_set_blob_field(rs_row, field_index, item->column_name,
                              item->file_name,
                              item->output_file_destination,
                              item->output_file_url,
                              item->blob_size,
                              item->mime_type);

    (*BLOB_index_ptr)++;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "handle_blob_column_batch complete BLOB_index=%d",
                 *BLOB_index_ptr);
    return 0;
}


/* ================================================================== */
/*  4.  build_row_xml_batch                                            */
/*      Iterate all columns for one logical row within a batch.        */
/*      Dispatches to BLOB, CLOB or scalar handler per column type.    */
/* ================================================================== */
static int build_row_xml_batch(oci_context_t *ctx,
								batch_ctx_t   *bc,
								ub4            row_in_batch,
								unsigned int   abs_rownum,
								lob_item_t    *BLOB_list,
								int           *BLOB_index_ptr,
								int            max_lobs,
								int           *CLOB_index_ptr,
								int            max_clobs,
								uint64_t      *clob_bytes_acc,
								xml_builder_t *xml,
								resultset_t   *rs)
{
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering row_in_batch=%u abs_rownum=%u",
                 row_in_batch, abs_rownum);

    /* xml_add_row_start(xml, abs_rownum); */ /* Unused: row wrapper now built by response_write_xml(ctx, rs) via new parsing layer */
    resultset_row_t *rs_row = resultset_get_row(rs, abs_rownum);



    for (ub4 i = 0; i < bc->col_count; i++)
    {
        const char *type_str = "STRING";

        switch (bc->data_types[i])
        {
            case SQLT_NUM:       type_str = "NUMBER";    break;
            case SQLT_DAT:       type_str = "DATE";      break;
            case SQLT_CHR:
            case SQLT_AFC:
            case SQLT_STR:       type_str = "STRING";    break;
            case SQLT_TIMESTAMP: type_str = "TIMESTAMP"; break;
            case SQLT_BLOB:      type_str = "BLOB";      break;
            case SQLT_CLOB:      type_str = "CLOB";      break;
            default:             type_str = "UNKNOWN";   break;
        }

        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "col=%u name=%s type=%s",
                     i, bc->col_names[i], type_str);

        if (bc->data_types[i] == SQLT_BLOB)
        {

            /*New code 18-JUL */
            int rc = handle_blob_column_batch(ctx, bc,
                                                row_in_batch, i,
                                                abs_rownum,
                                                BLOB_list, BLOB_index_ptr,
                                                max_lobs, xml,
                                                rs_row, (int)i);          /* ADD */

            if (rc != 0)
            {
                logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                             "handle_blob_column_batch failed col=%u", i);
                return rc;
            }


        }
        else if (bc->data_types[i] == SQLT_CLOB)
        {
            /*New code 18-JUL */
            int rc = handle_clob_column_batch(ctx, bc,
                                               i,
                                               abs_rownum,
                                               CLOB_index_ptr,
                                               max_clobs,
                                               clob_bytes_acc,
                                               xml,
                                               rs_row,
											   (int)i);          /* ADD */
          if (rc != 0)
            {
                logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                             "handle_clob_column_batch failed col=%u", i);
                return rc;
            }
        }
        else
        {
            /* Scalar: stride into flat buffer for this row's value */
            const char *value =
                bc->buffers[i] +
                ((size_t)row_in_batch * bc->buf_sizes[i]);

            if (bc->indicators[i][row_in_batch] == -1)
                value = "";

            /* xml_add_field(xml, bc->col_names[i], type_str, value); */ /* Unused: XML now built from response_write_xml(ctx, rs) via new parsing layer */

            resultset_set_field(rs_row, (int)i, bc->col_names[i], type_str, value);   /* ADD THIS LINE */

        }
    }

    /* xml_add_row_end(xml); */ /* Unused: row wrapper now built by response_write_xml(ctx, rs) via new parsing layer */

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "build_row_xml_batch complete abs_rownum=%u", abs_rownum);
    return 0;
}


/* ================================================================== */
/*  5.  free_batch_ctx                                                 */
/*      Release all memory and LOB locators in a batch_ctx_t          */
/* ================================================================== */
static void free_batch_ctx(oci_context_t *ctx, batch_ctx_t *bc)
{
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering free_batch_ctx");

    if (!bc) return;

    if (bc->col_blob_locs)
    {
        for (ub4 i = 0; i < bc->col_count; i++)
        {
            if (bc->col_blob_locs[i])
            {
                for (ub4 r = 0; r < bc->fetch_count; r++)
                {
                    if (bc->col_blob_locs[i][r])
                    {
                        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                                     "OCIDescriptorFree col_blob_locs[%u][%u]",
                                     i, r);
                        OCIDescriptorFree(bc->col_blob_locs[i][r],
                                          OCI_DTYPE_LOB);
                    }
                }
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "free(col_blob_locs[%u])", i);
                free(bc->col_blob_locs[i]);
                bc->col_blob_locs[i] = NULL;
            }
        }
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                     "free(col_blob_locs)");
        free(bc->col_blob_locs);
        bc->col_blob_locs = NULL;
    }

    if (bc->buffers)
    {
        for (ub4 i = 0; i < bc->col_count; i++)
        {
            if (bc->buffers[i])
            {
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "free(buffers[%u])", i);
                free(bc->buffers[i]);
                bc->buffers[i] = NULL;
            }
        }
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "free(buffers)");
        free(bc->buffers);
        bc->buffers = NULL;
    }

    if (bc->indicators)
    {
        for (ub4 i = 0; i < bc->col_count; i++)
        {
            if (bc->indicators[i])
            {
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "free(indicators[%u])", i);
                free(bc->indicators[i]);
                bc->indicators[i] = NULL;
            }
        }
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "free(indicators)");
        free(bc->indicators);
        bc->indicators = NULL;
    }

    if (bc->clob_loc)
    {
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                     "OCIDescriptorFree clob_loc");
        OCIDescriptorFree(bc->clob_loc, OCI_DTYPE_LOB);
        bc->clob_loc = NULL;
    }

    if (bc->def)        { free(bc->def);        bc->def        = NULL; }
    if (bc->buf_sizes)  { free(bc->buf_sizes);  bc->buf_sizes  = NULL; }
    if (bc->data_types) { free(bc->data_types); bc->data_types = NULL; }
    if (bc->data_sizes) { free(bc->data_sizes); bc->data_sizes = NULL; }
    if (bc->col_names)  { free(bc->col_names);  bc->col_names  = NULL; }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "free_batch_ctx complete");
}


/* ------------------------------------------------------------------ */
/*  Stage 3c: verify response_write_json() output (temporary -         */
/*  verification only)                                                 */
/*                                                                      */
/*  Unlike Stage 3's XML check, there's no pre-existing JSON buffer to  */
/*  diff against - response_write_json() is brand new. So instead this  */
/*  parses its own output back with cJSON and compares every field      */
/*  against rs directly, the same struct both writers render from.      */
/*  This is a stricter check than comparing two rendered strings by     */
/*  eye: it catches the writer disagreeing with its own source data,    */
/*  not just disagreeing with the XML writer.                           */
/*                                                                      */
/*  Returns 1 on full match, 0 on any mismatch (with a printf/logger    */
/*  line identifying exactly what didn't match, same MATCH/MISMATCH     */
/*  pattern as Stage 3).                                                */
/* ------------------------------------------------------------------ */
static int verify_response_json_against_resultset(oci_context_t *ctx,
                                                    const resultset_t *rs,
                                                    const char *json_str)
{
    if (!rs || !json_str) return 0;

    cJSON *root = cJSON_Parse(json_str);
    if (!root)
    {
        printf("\n[STAGE3c] JSON did not parse: %s\n", cJSON_GetErrorPtr());
        return 0;
    }

    cJSON *resultset = cJSON_GetObjectItemCaseSensitive(root, "resultset");
    if (!cJSON_IsArray(resultset))
    {
        printf("\n[STAGE3c] MISMATCH - no \"resultset\" array in JSON\n");
        cJSON_Delete(root);
        return 0;
    }

    int json_row_count = cJSON_GetArraySize(resultset);
    if (json_row_count != rs->record_count)
    {
        printf("\n[STAGE3c] MISMATCH - row count: struct=%d json=%d\n",
               rs->record_count, json_row_count);
        cJSON_Delete(root);
        return 0;
    }

    int ok = 1;

    for (int r = 0; r < rs->record_count && ok; r++)
    {
        const resultset_row_t *row      = &rs->records[r];
        cJSON                 *row_obj  = cJSON_GetArrayItem(resultset, r);
        cJSON *row_number = cJSON_GetObjectItemCaseSensitive(row_obj, "row_number");
        cJSON *fields     = cJSON_GetObjectItemCaseSensitive(row_obj, "fields");

        if (!cJSON_IsNumber(row_number) || row_number->valueint != row->record_number)
        {
            printf("\n[STAGE3c] MISMATCH - row %d: row_number struct=%d json=%s\n",
                   r, row->record_number,
                   row_number ? cJSON_Print(row_number) : "(missing)");
            ok = 0;
            break;
        }

        if (!cJSON_IsArray(fields) || cJSON_GetArraySize(fields) != row->field_count)
        {
            printf("\n[STAGE3c] MISMATCH - row %d: field_count struct=%d json=%d\n",
                   r, row->field_count,
                   cJSON_IsArray(fields) ? cJSON_GetArraySize(fields) : -1);
            ok = 0;
            break;
        }

        for (int f = 0; f < row->field_count; f++)
        {
            const resultset_field_t *fld       = &row->fields[f];
            cJSON                   *field_obj = cJSON_GetArrayItem(fields, f);

            cJSON *fn = cJSON_GetObjectItemCaseSensitive(field_obj, "field_name");
            cJSON *ft = cJSON_GetObjectItemCaseSensitive(field_obj, "field_type");

            if (!cJSON_IsString(fn) || strcmp(fn->valuestring, fld->field_name) != 0)
            {
                printf("\n[STAGE3c] MISMATCH - row %d field %d: field_name struct=%s json=%s\n",
                       r, f, fld->field_name,
                       cJSON_IsString(fn) ? fn->valuestring : "(missing)");
                ok = 0;
                break;
            }

            if (!cJSON_IsString(ft) ||
                strcmp(ft->valuestring, fld->is_blob ? "BLOB" : fld->field_type) != 0)
            {
                printf("\n[STAGE3c] MISMATCH - row %d field %d (%s): field_type struct=%s json=%s\n",
                       r, f, fld->field_name,
                       fld->is_blob ? "BLOB" : fld->field_type,
                       cJSON_IsString(ft) ? ft->valuestring : "(missing)");
                ok = 0;
                break;
            }

            if (fld->is_blob)
            {
                cJSON *blob = cJSON_GetObjectItemCaseSensitive(field_obj, "blob");
                cJSON *bn   = blob ? cJSON_GetObjectItemCaseSensitive(blob, "file_name") : NULL;
                cJSON *bp   = blob ? cJSON_GetObjectItemCaseSensitive(blob, "file_path") : NULL;
                cJSON *bs   = blob ? cJSON_GetObjectItemCaseSensitive(blob, "file_size") : NULL;
                cJSON *bm   = blob ? cJSON_GetObjectItemCaseSensitive(blob, "mime_type") : NULL;

                char size_str[32];
                snprintf(size_str, sizeof(size_str), "%llu",
                         (unsigned long long)fld->blob_detail.file_size);

                const char *exp_name = fld->blob_detail.file_name[0] ? fld->blob_detail.file_name : "N/A";
                const char *exp_path = fld->blob_detail.file_path[0] ? fld->blob_detail.file_path : "N/A";
                const char *exp_mime = fld->blob_detail.mime_type[0] ? fld->blob_detail.mime_type : "application/octet-stream";

                if (!blob ||
                    !cJSON_IsString(bn) || strcmp(bn->valuestring, exp_name) != 0 ||
                    !cJSON_IsString(bp) || strcmp(bp->valuestring, exp_path) != 0 ||
                    !cJSON_IsString(bs) || strcmp(bs->valuestring, size_str) != 0 ||
                    !cJSON_IsString(bm) || strcmp(bm->valuestring, exp_mime) != 0)
                {
                    printf("\n[STAGE3c] MISMATCH - row %d field %d (%s): blob object differs\n",
                           r, f, fld->field_name);
                    ok = 0;
                    break;
                }

                /* file_url only expected when set - same conditional as
                 * the writer itself and Stage 3c stays in sync with it. */
                if (fld->blob_detail.file_url[0])
                {
                    cJSON *bu = cJSON_GetObjectItemCaseSensitive(blob, "file_url");
                    if (!cJSON_IsString(bu) || strcmp(bu->valuestring, fld->blob_detail.file_url) != 0)
                    {
                        printf("\n[STAGE3c] MISMATCH - row %d field %d (%s): file_url differs\n",
                               r, f, fld->field_name);
                        ok = 0;
                        break;
                    }
                }
            }
            else
            {
                cJSON *fv = cJSON_GetObjectItemCaseSensitive(field_obj, "field_value");
                if (!cJSON_IsString(fv) || strcmp(fv->valuestring, fld->value) != 0)
                {
                    printf("\n[STAGE3c] MISMATCH - row %d field %d (%s): field_value struct=%s json=%s\n",
                           r, f, fld->field_name, fld->value,
                           cJSON_IsString(fv) ? fv->valuestring : "(missing)");
                    ok = 0;
                    break;
                }
            }
        }
    }

    cJSON_Delete(root);

    if (ok)
        printf("\n[STAGE3c] MATCH - JSON matches source resultset struct field-for-field (%d rows)\n",
               rs->record_count);

    logger_write(ctx->select_logger, ok ? LOG_INFO : LOG_WARN, __func__, 0,
                 ok ? "STAGE3c MATCH - JSON matches resultset struct"
                    : "STAGE3c MISMATCH - JSON differs from resultset struct");

    return ok;
}


/* ================================================================== */
/*  6.  execute_query_batch                                            */
/*      Main entry point - orchestrates the full batch fetch cycle     */
/* ================================================================== */
int execute_query_batch(oci_context_t *ctx, execute_config_t *cfg)
{
    int        rc         = 0;
    OCIStmt   *stmt       = NULL;
    OCIStmt   *stmt_count = NULL;
    lob_item_t *BLOB_list = NULL;
    resultset_t *rs = NULL;
    xml_builder_t *xml    = NULL;
    char parse_msg[256];

    OCI_DEPENDENCY_LIST deps;
    memset(&deps, 0, sizeof(deps));
    memset(&parse_msg, 0, sizeof(parse_msg));

    batch_ctx_t bc;
    memset(&bc, 0, sizeof(bc));

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Entering execute_query_batch sql=%s", cfg->SQL);

    if (!ctx || !cfg || !cfg->SQL)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Invalid arguments: ctx, cfg or cfg->SQL is NULL");
        rc = -1;
        goto Cleanup;
    }

    /* ---- Resolve fetch batch size ---- */
    bc.fetch_count = (ub4)ctx->ini->query_fetch_batch_size;
    if (bc.fetch_count < 1)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "query_fetch_batch_size=%d < 1, defaulting to 1",
                     ctx->ini->query_fetch_batch_size);
        bc.fetch_count = 1;
    }
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "fetch_count=%u", bc.fetch_count);

    /* ---- Clean SQL ---- */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling trim_sql_inplace");
    trim_sql_inplace(cfg->SQL, ctx);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Cleaned SQL: %s", cfg->SQL);

    /* ============Metrics INIT BLOCK -           */
    /* ================================================================== */

        metrics_record_t metrics;
        metrics_init(&metrics);
        metrics_set_context(&metrics, ctx);

        metrics.start_time_us = metrics_now_us();

        strncpy(metrics.operation,   "SELECT",  sizeof(metrics.operation)   - 1);
        /* object_name: filled after sql dependency extraction when available */
        /* For now use the first 127 chars of the SQL as a fallback          */
        strncpy(metrics.object_name, cfg->SQL,  sizeof(metrics.object_name) - 1);

        /* Set transaction_id immediately so every write path carries it  */
            if (ctx->active_tx)
                strncpy(metrics.transaction_id,
                        tx_get_id(ctx->active_tx),
                        sizeof(metrics.transaction_id) - 1);
            else
                strncpy(metrics.transaction_id, "-",
                        sizeof(metrics.transaction_id) - 1);
            /* Same source as transaction_id above, just the name -
             * closure item 5 follow-up (2026-08-10).                  */
            strncpy(metrics.transaction_name,
                    ctx->active_tx ? ctx->active_tx->tx_name : "-",
                    sizeof(metrics.transaction_name) - 1);


    /* ================================================================
     *  TL:6-June - Stage 0: Parse SQL dependencies
     *  Extract every table/view and field reference from the cleaned
     *  SQL.  On failure return -1 immediately with a descriptive error
     *  already written to sql_parser_logger by the extractor.
     *  On success deps is fully populated and passed to
     *  get_select_metadata() later so it can call get_table_metadata()
     *  per source table before delegating to get_multi_metadata().
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 0: Parsing SQL dependencies");

    uint64_t sql_parse_start = metrics_now_us();
    int sql_parse_rc = extract_sql_dependencies(cfg->SQL, &deps, ctx);
    metrics.sql_parse_us = metrics_now_us() - sql_parse_start;

    if (sql_parse_rc != 0)
    {

        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "extract_sql_dependencies failed for SQL: %s",
                     cfg->SQL);

        rc = -1;
        goto Cleanup;
    }


    strncpy(metrics.object_name, deps.objects[0].object_name ,  sizeof(metrics.object_name) - 1);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 0 OK: objects=%d fields=%d",
                 deps.object_count, deps.field_count);







    /* ================================================================
     * BLOCK A - Cache lookup
     * Place this immediately after trim_sql_inplace() call,
     * before the Stage 1 count query.
     * ================================================================ */

        /* ---- Resultset cache lookup ---- */
       logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Checking Resultsetcache setup");


       char cache_key[8192] = {0};
        int  served_from_cache = 0;

        if (ctx->resultset_cache &&
            resultset_cache_make_key(cfg->SQL, cache_key, sizeof(cache_key)))
        {
            /* Compute both hashes as soon as the key is available     */
            metrics.sql_hash       = cache_hash_string(ctx->resultset_cache,
                                                        cfg->SQL);
            metrics.cache_key_hash = cache_hash_string(ctx->resultset_cache,
                                                        cache_key);

            uint64_t lookup_start = metrics_now_us();
            cache_entry_t *hit = resultset_cache_lookup(ctx->resultset_cache,
                                                         cache_key);
            metrics.cache_lookup_us = metrics_now_us() - lookup_start;

            if (hit)
            {
                int want_json = (cfg->ReturnFormat &&
                                 strcasecmp(cfg->ReturnFormat, "JSON") == 0);

                if (want_json && !hit->output_document_json)
                {
                    /* This entry predates JSON caching (or JSON
                     * rendering failed at store time) - do not serve
                     * XML to a JSON request. Treat as a miss and fall
                     * through to normal execution below rather than
                     * goto Cleanup, so this becomes a genuine store
                     * with JSON included.                              */
                    logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                                 "CACHE HIT key='%.80s' but no cached JSON "
                                 "for a JSON request - treating as miss",
                                 cache_key);
                    metrics.cache_hit = 0;
                    resultset_cache_release(ctx->resultset_cache, hit);
                }
                else
                {
                    metrics.cache_hit = 1;

                    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                 "CACHE HIT key='%.80s' doc_len=%zu rows=%"PRIu64
                                 " format=%s",
                                 cache_key, hit->output_length, hit->row_count,
                                 want_json ? "JSON" : "XML");

                    /* Return cached content directly - no OCI work needed */
                    if (want_json)
                    {
                        cfg->OUTPUT_JSON = strdup(hit->output_document_json);
                    }
                    else
                    {
                        if (!cfg->xml)
                            cfg->xml = calloc(1, sizeof(*cfg->xml));

                        if (cfg->xml)
                            cfg->xml->OUTPUT_XML = strdup(hit->output_document);
                    }

                    metrics.rows_affected = hit->row_count;

                    uint64_t hit_row_count = hit->row_count;

                    resultset_cache_release(ctx->resultset_cache, hit);

                    cache_update_exec_stats(ctx->resultset_cache,
                                            0.0, 0.0,
                                            hit_row_count,  /* rows from cache entry */
                                            1,     /* was_cache_hit = 1        */
                                            1);    /* success                  */
                    served_from_cache = 1;
                    rc = 0;
                    goto Cleanup;
                }
            }
            else
            {
                metrics.cache_hit = 0;
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "CACHE MISS key='%.80s'", cache_key);
            }
        }





    /* ================================================================
     *  Stage 1 - Validate: row count guard
     *
     *  Use SELECT COUNT(*) FROM (SELECT 1 FROM (original_sql)) to avoid
     *  ORA-00932 which Oracle raises when a CLOB column appears in a
     *  COUNT(*) subquery. The SELECT 1 strips all column types.
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 1: Validate record count");

    int        record_count = 0;
    OCIDefine *defn_count   = NULL;
    char       query_count[4096];

    snprintf(query_count, sizeof(query_count),
             "SELECT COUNT(*) FROM (SELECT 1 FROM (%s))", cfg->SQL);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Count query: %s", query_count);

    CHECK_OCI(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt_count, ctx->errhp,
                        (text *)query_count, (ub4)strlen(query_count),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));

    CHECK_OCI(ctx->errhp,
        OCIDefineByPos(stmt_count, &defn_count, ctx->errhp,
                       1, &record_count, sizeof(record_count),
                       SQLT_INT, NULL, NULL, NULL, OCI_DEFAULT));

    CHECK_OCI(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt_count, ctx->errhp,
                       1, 0, NULL, NULL, OCI_DEFAULT));

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "record_count=%d max=%d",
                 record_count, ctx->ini->query_max_record_count);

    /* Closure item 4 (2026-08-09) - this guard used to treat "zero
     * rows" and "exceeds max" identically, aborting the whole request
     * with a generic error for both. Neither is actually an error:
     *
     *  - Zero rows is a completely normal, valid outcome for a SELECT
     *    whose WHERE clause simply matches nothing - it isn't a
     *    failure to be reported as one. Confirmed as a real, repeated
     *    point of confusion during 2026-07 testing (this exact log
     *    line being mistaken for a genuine problem - see
     *    OCI_Session_Manager.c's own reconcile_orphans() comment on
     *    the same issue) and worth fixing at the source rather than
     *    re-explaining every time it comes up.
     *
     *  - Exceeding max shouldn't block the caller outright either -
     *    return what's allowed and say so, rather than forcing a
     *    ticket/retry cycle for something the caller can already see
     *    and adjust for themselves (their own request's row count is
     *    knowable in advance).
     *
     * IMPORTANT SAFETY NOTE for the "exceeds max" branch: capping only
     * the record_count VARIABLE here would NOT actually bound how many
     * rows get fetched below - Stage 5's fetch loop runs unbounded
     * until OCIStmtFetch2 itself reports rows_fetched=0 (cursor
     * exhausted), with no check against record_count inside the loop
     * at all. If the underlying query would still return more rows
     * than the cap, capping just this variable would leave
     * resultset_create() allocating arrays sized for the CAPPED count,
     * while the fetch loop kept writing past that bound - a genuine
     * buffer overflow. The safe fix constrains the actual SQL executed
     * below (see fetch_sql construction a few lines down), not just
     * this count.                                                      */
    int truncated = 0;

    if (record_count < 1)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "record_count=0 - no rows matched. This is a valid, "
                     "normal outcome, not an error - proceeding with an "
                     "empty result set.");
        /* record_count stays 0 - resultset_create(0, ...) and the
         * max_lobs/max_clobs calculations below all handle this
         * correctly (0 * anything = 0), and Stage 5's fetch loop will
         * simply see rows_fetched=0 on its first call and exit
         * immediately with nothing processed.                         */
    }
    else if (record_count > ctx->ini->query_max_record_count)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "record_count=%d exceeds max=%d - returning the "
                     "first %d row(s) rather than aborting. Caller "
                     "should check the <truncated> flag on the response "
                     "and narrow their own query if the full result set "
                     "is actually needed.",
                     record_count, ctx->ini->query_max_record_count,
                     ctx->ini->query_max_record_count);
        record_count = ctx->ini->query_max_record_count;
        truncated = 1;
    }

    /* ---- Allocate BLOB and CLOB tracking lists ---- */
    int max_lobs  = record_count * ctx->ini->max_BLOBS_per_record;
    int max_clobs = record_count * ctx->ini->max_CLOBS_per_record;
    int BLOB_index = 0;
    int CLOB_index = 0;
    uint64_t lob_bytes  = 0;   /* total BLOB bytes read               */
    uint64_t clob_bytes = 0;   /* total CLOB bytes read               */

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "max_lobs=%d max_clobs=%d", max_lobs, max_clobs);

    /* Closure item 4 (2026-08-09) - when truncated, the statement
     * actually fetched below must itself be bounded to record_count
     * rows (now the capped value) - not just the variable. A separate
     * buffer, not an in-place rewrite of cfg->SQL: that pointer is
     * also used later for extract_sql_dependencies() (already run,
     * above this point, on the real original query - unaffected
     * either way) and for the <sql_query> tag in the response itself,
     * which should show the caller's actual query, not a wrapper this
     * code injected around it. Same ROWNUM-subquery idiom already used
     * a few lines up for the COUNT(*) guard query, for consistency
     * within this file.                                                */
    char  fetch_sql_buf[4096];
    const char *fetch_sql = cfg->SQL;

    if (truncated)
    {
        snprintf(fetch_sql_buf, sizeof(fetch_sql_buf),
                 "SELECT * FROM (%s) WHERE ROWNUM <= %d",
                 cfg->SQL, record_count);
        fetch_sql = fetch_sql_buf;
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Truncated fetch SQL: %s", fetch_sql);
    }

    if (max_lobs > 0)
    {
        BLOB_list = calloc(max_lobs, sizeof(lob_item_t));
        if (!BLOB_list)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "calloc failed for BLOB_list");
            rc = -1;
            goto Cleanup;
        }
    }

    /* ================================================================
     *  Stage 2 - Prepare, Describe, Allocate, Define
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 2: Prepare and describe statement");

    CHECK_OCI(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)fetch_sql, (ub4)strlen(fetch_sql),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Calling OCIStmtExecute OCI_DESCRIBE_ONLY");
    CHECK_OCI(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DESCRIBE_ONLY));

    CHECK_OCI(ctx->errhp,
        OCIAttrGet(stmt, OCI_HTYPE_STMT,
                   &bc.col_count, 0,
                   OCI_ATTR_PARAM_COUNT, ctx->errhp));

    if (bc.col_count == 0)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0, "No columns returned");
        rc = -1;
        goto Cleanup;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "col_count=%u", bc.col_count);

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Calling OCIDescriptorAlloc for clob_loc");
    CHECK_OCI(ctx->errhp,
        OCIDescriptorAlloc(ctx->envhp,
                           (void **)&bc.clob_loc,
                           OCI_DTYPE_LOB, 0, NULL));

    if (allocate_batch_buffers(ctx, &bc) != 0) { rc = -1; goto Cleanup; }

    /* ================================================================
     *  Phase B - Expand wildcard and unqualified field entries
     *
     *  When deps.needs_expansion == 1 the parser found SELECT *,
     *  TABLE.*, or unqualified column names and could not fully
     *  populate deps.fields[].  Now that OCI_DESCRIBE_ONLY has run
     *  we have the exact column names and count from Oracle in
     *  bc.col_count.  We read them via OCIParamGet here and rebuild
     *  deps.fields[] with fully populated OCI_FIELD_REF entries so
     *  get_select_metadata() can cross-reference correctly.
     *
     *  Three cases handled:
     *    SQL_FIELD_WILDCARD_ALL (*):
     *      Replace the single * entry with one entry per OCI column.
     *      table_ref is set to the alias/name of the sole FROM object.
     *      Only valid for single-table queries (parser enforces this).
     *
     *    SQL_FIELD_WILDCARD_TBL (TABLE.*):
     *      Each TABLE.* entry is replaced with OCI columns in order.
     *      For a single table this is all columns.
     *      For mixed queries (t.col1, u.*) the wildcard slots are
     *      expanded and normal fields are kept in position order.
     *      Since OCI returns columns in SELECT list order this is
     *      straightforward: re-read all OCI names and match.
     *
     *    SQL_FIELD_UNQUALIFIED:
     *      table_ref was already inferred by the parser.
     *      Just update field_name from the OCI descriptor to ensure
     *      exact Oracle casing and no alias confusion.
     *
     *  After expansion deps.needs_expansion is cleared.
     *  If no expansion is needed this entire block is skipped.
     * ================================================================ */
    if (deps.needs_expansion)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Phase B: expanding wildcard/unqualified fields "
                     "from OCI descriptor (col_count=%u)", bc.col_count);

        /* Read all OCI column names from the describe result */
        char oci_col_names[SQL_DEP_MAX_FIELDS][256];
        memset(oci_col_names, 0, sizeof(oci_col_names));

        for (ub4 ci = 0; ci < bc.col_count && ci < SQL_DEP_MAX_FIELDS; ci++)
        {
            OCIParam *param  = NULL;
            text     *tname  = NULL;
            ub4       tlen   = 0;

            if (OCIParamGet(stmt, OCI_HTYPE_STMT, ctx->errhp,
                            (void **)&param, ci + 1) == OCI_SUCCESS)
            {
                if (OCIAttrGet(param, OCI_DTYPE_PARAM,
                               &tname, &tlen,
                               OCI_ATTR_NAME,
                               ctx->errhp) == OCI_SUCCESS && tname)
                {
                    if (tlen > 255) tlen = 255;
                    memcpy(oci_col_names[ci], tname, tlen);
                    oci_col_names[ci][tlen] = '\0';
                }
            }
            logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                         "Phase B: OCI col[%u] = '%s'",
                         ci, oci_col_names[ci]);
        }

        /* Determine the single_table_ref for SELECT * expansion */
        const char *single_ref =
            (deps.object_count == 1)
            ? (deps.objects[0].alias[0]
               ? deps.objects[0].alias
               : deps.objects[0].object_name)
            : NULL;

        /* Rebuild deps.fields[] from OCI column names */
        OCI_FIELD_REF new_fields[SQL_DEP_MAX_FIELDS];
        int           new_count = 0;
        memset(new_fields, 0, sizeof(new_fields));

        /*
         * Strategy: walk the original deps.fields[] entries.
         * Normal and unqualified entries map 1:1 to OCI columns
         * (in the order they appear in the SELECT list).
         * Wildcard entries expand to one entry per remaining OCI column.
         * We use an OCI column cursor (oci_idx) that advances through
         * bc.col_count in lock-step with the expanded output.
         *
         * For TABLE.* wildcards: the number of OCI columns the wildcard
         * should consume equals bc.col_count minus the total number of
         * non-wildcard fields in deps.fields[].  This prevents the
         * wildcard from greedily consuming aliased columns that appear
         * explicitly in the SELECT list after the wildcard, e.g.:
         *
         *   SELECT OCI_LOB_TEST.*, PHOTO AS Second_Blob FROM OCI_LOB_TEST
         *
         * Here OCI_LOB_TEST.* expands to 4 columns (ID, DESCRIPTION,
         * FILE_NAME, PHOTO), then Second_Blob is a normal field that
         * must be handled separately.  Without the count guard the
         * wildcard loop consumed all 5 OCI columns including SECOND_BLOB,
         * then get_select_metadata() failed trying to look up
         * OCI_LOB_TEST.SECOND_BLOB in ALL_TAB_COLUMNS.
         */
        /* Pre-compute: how many non-wildcard fields are in deps.fields[] */
        int non_wildcard_count = 0;
        for (int f = 0; f < deps.field_count; f++)
        {
            if (deps.fields[f].expansion_type != SQL_FIELD_WILDCARD_ALL &&
                deps.fields[f].expansion_type != SQL_FIELD_WILDCARD_TBL)
                non_wildcard_count++;
        }
        /* Wildcard columns to expand = total OCI columns - explicit fields */
        int wildcard_oci_budget = (int)bc.col_count - non_wildcard_count;
        if (wildcard_oci_budget < 0) wildcard_oci_budget = 0;

        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                     "Phase B: col_count=%u non_wildcard=%d wildcard_budget=%d",
                     bc.col_count, non_wildcard_count, wildcard_oci_budget);

        int oci_idx          = 0;
        int wildcard_emitted = 0;  /* how many wildcard OCI cols consumed */

        for (int f = 0; f < deps.field_count && oci_idx < (int)bc.col_count; f++)
        {
            OCI_FIELD_REF *src = &deps.fields[f];

            if (src->expansion_type == SQL_FIELD_WILDCARD_ALL)
            {
                /* Expand * into wildcard_budget OCI columns.
                 * Never consume columns that belong to explicit fields. */
                while (oci_idx < (int)bc.col_count &&
                       new_count < SQL_DEP_MAX_FIELDS &&
                       wildcard_emitted < wildcard_oci_budget)
                {
                    OCI_FIELD_REF *dst = &new_fields[new_count];
                    memset(dst, 0, sizeof(*dst));
                    strncpy(dst->table_ref,  single_ref ? single_ref : "",
                            sizeof(dst->table_ref)  - 1);
                    strncpy(dst->field_name, oci_col_names[oci_idx],
                            sizeof(dst->field_name) - 1);
                    dst->field_pos      = new_count + 1;
                    dst->expansion_type = SQL_FIELD_NORMAL;
                    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                 "Phase B: * -> [%d] %s.%s",
                                 dst->field_pos,
                                 dst->table_ref, dst->field_name);
                    new_count++;
                    oci_idx++;
                    wildcard_emitted++;
                }
            }
            else if (src->expansion_type == SQL_FIELD_WILDCARD_TBL)
            {
                /* Expand TABLE.* — consume exactly wildcard_budget OCI
                 * columns, leaving the remainder for explicit fields.
                 * For queries like:
                 *   SELECT OCI_LOB_TEST.*, PHOTO AS Second_Blob ...
                 * the wildcard consumes 4 columns (the real table cols)
                 * and SECOND_BLOB is handled as a normal field below.  */
                while (oci_idx < (int)bc.col_count &&
                       new_count < SQL_DEP_MAX_FIELDS &&
                       wildcard_emitted < wildcard_oci_budget)
                {
                    OCI_FIELD_REF *dst = &new_fields[new_count];
                    memset(dst, 0, sizeof(*dst));
                    strncpy(dst->table_ref,  src->table_ref,
                            sizeof(dst->table_ref)  - 1);
                    strncpy(dst->field_name, oci_col_names[oci_idx],
                            sizeof(dst->field_name) - 1);
                    dst->field_pos      = new_count + 1;
                    dst->expansion_type = SQL_FIELD_NORMAL;
                    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                 "Phase B: %s.* -> [%d] %s.%s",
                                 src->table_ref,
                                 dst->field_pos,
                                 dst->table_ref, dst->field_name);
                    new_count++;
                    oci_idx++;
                    wildcard_emitted++;
                }
            }
            else
            {
                /* Normal or unqualified field - explicit in the SELECT list.
                 *
                 * If the field has an alias (e.g. PHOTO AS Second_Blob)
                 * the OCI descriptor returns the ALIAS as the column name
                 * (SECOND_BLOB).  For metadata lookup we need the real
                 * underlying column name (PHOTO from src->field_name) not
                 * the alias.  We store the alias separately so the result
                 * XML can use it as the element name.
                 *
                 * src->field_name was populated by the SQL parser from the
                 * actual column expression (PHOTO), so we keep it and only
                 * update the alias from the OCI descriptor.              */
                OCI_FIELD_REF *dst = &new_fields[new_count];
                *dst = *src;

                /* Store OCI name as alias if it differs from field_name
                 * (i.e. an AS alias was used in the SQL).               */
                if (strcasecmp(oci_col_names[oci_idx],
                               src->field_name) != 0)
                {
                    strncpy(dst->field_alias, oci_col_names[oci_idx],
                            sizeof(dst->field_alias) - 1);
                    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                 "Phase B: [%d] %s.%s AS %s (alias detected)",
                                 new_count + 1,
                                 dst->table_ref,
                                 dst->field_name,
                                 dst->field_alias);
                }
                else
                {
                    /* No alias - update field_name from OCI for exact casing */
                    strncpy(dst->field_name, oci_col_names[oci_idx],
                            sizeof(dst->field_name) - 1);
                    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                 "Phase B: [%d] %s.%s confirmed from OCI",
                                 new_count + 1,
                                 dst->table_ref, dst->field_name);
                }

                dst->field_pos      = new_count + 1;
                dst->expansion_type = SQL_FIELD_NORMAL;
                new_count++;
                oci_idx++;
            }
        }

        /* Replace deps.fields[] with the expanded set */
        memcpy(deps.fields, new_fields,
               (size_t)new_count * sizeof(OCI_FIELD_REF));
        deps.field_count     = new_count;
        deps.needs_expansion = 0;

        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Phase B complete: field_count=%d", deps.field_count);
    }





    /* ----------------------------------------------------------------
     * TL:6-June - Call get_select_metadata() in place of get_multi_metadata().
     * get_select_metadata() enriches the metadata step with:
     *   a) get_table_metadata() per FROM-clause object (warms cache,
     *      logs ALL_TABLES statistics to Metadata_logger)
     *   b) Cross-reference of SELECT-clause fields to source tables
     *      (diagnostic logging in Metadata_logger in column order)
     *   c) Delegation to get_multi_metadata() for the actual OCI
     *      OCIParamGet / OCIDefineByPos / OCIDefineArrayOfStruct work.
     * The fetch loop and free_batch_ctx() below are completely unchanged.
     * get_multi_metadata() remains in place for callers that do not
     * have dependency information available.
     * ---------------------------------------------------------------- */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Calling get_select_metadata");

    multi_meta_request_t mmr;
    memset(&mmr, 0, sizeof(mmr));
    mmr.ctx           = ctx;
    mmr.stmt          = stmt;
    mmr.col_count     = bc.col_count;
    mmr.fetch_count   = bc.fetch_count;
    mmr.def           = bc.def;
    mmr.buffers       = bc.buffers;
    mmr.buf_sizes     = bc.buf_sizes;
    mmr.indicators    = bc.indicators;
    mmr.data_types    = bc.data_types;
    mmr.data_sizes    = bc.data_sizes;
    mmr.col_names     = bc.col_names;
    mmr.col_blob_locs = bc.col_blob_locs;
    mmr.clob_loc      = bc.clob_loc;
    mmr.deps          = &deps;          /* populated by Stage 0 above   */

    if (get_select_metadata(&mmr) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "get_select_metadata failed");
        rc = -1;
        goto Cleanup;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "get_select_metadata OK");

    /*
     * CLOB ARRAY FETCH RESTRICTION - OCI QUIRK - DO NOT REMOVE THIS BLOCK
     * ---------------------------------------------------------------------
     * OCI does not support array fetch (OCIStmtFetch2 with nrows > 1) when
     * a CLOB column is present in the select list. The single clob_loc
     * locator registered in OCIDefineByPos cannot be strided like scalar
     * or BLOB columns via OCIDefineArrayOfStruct. Attempting to fetch
     * multiple rows at once with a CLOB defined causes a silent process
     * crash with no OCI error logged - making it extremely hard to diagnose.
     *
     * Detection: scan column types after describe. If any CLOB is found,
     * force fetch_count to 1 for the entire fetch loop. All other logic
     * (batch context, buffers, XML output) remains unchanged - only the
     * number of rows requested per OCIStmtFetch2 call is reduced to 1.
     *
     * This restriction applies to CLOB only. BLOB array fetch works
     * correctly because each BLOB locator slot is individually allocated
     * and strided via OCIDefineArrayOfStruct with sizeof(OCILobLocator*).
     */
    for (ub4 i = 0; i < bc.col_count; i++)
    {
        if (bc.data_types[i] == SQLT_CLOB)
        {
            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                         "CLOB column detected at col=%u - "
                         "forcing fetch_count=1 (OCI array fetch restriction)",
                         i);
            bc.fetch_count = 1;
            break;
        }
    }

    /* ================================================================
     *  Stage 3 - Execute (real run, cursor open)
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 3: Execute statement (real run)");

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Calling OCIStmtExecute OCI_DEFAULT (iters=0)");
    CHECK_OCI(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT));

    uint64_t exec_start_us = metrics_now_us();

    /* ================================================================
     *  Stage 4 - Build XML document header
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 4: Build XML document");

    /*18-JUL*/
    rs = resultset_create(record_count, (int)bc.col_count);
     if (!rs)
     {
         logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                      "resultset_create failed - record_count=%d fields_per_row=%u",
                      record_count, bc.col_count);
         rc = -1;
         goto Cleanup;
     }

    xml = xml_create(16384);
    xml_start_document(xml);
    xml_start_execution(xml);
    xml_append(xml, "<sql_query>%s</sql_query>\n", cfg->SQL);
    /* Closure item 4 (2026-08-09) - explicit flag, not just a log
     * warning, so a caller doing reconciliation or a completeness
     * check has a real way to know they received a partial result
     * rather than silently trusting a row count that's actually
     * short of the true total.                                        */
    xml_append(xml, "<truncated>%s</truncated>\n", truncated ? "true" : "false");
    xml_end_execution(xml);
    /* xml_start_resultset(xml); */ /* Unused: resultset body now built by response_write_xml(ctx, rs) via new parsing layer, after the fetch loop populates rs */

    /* ================================================================
     *  Stage 5 - Batch fetch loop
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 5: Batch fetch loop fetch_count=%u", bc.fetch_count);

    unsigned int abs_rownum = 0;
    sword        fetch_status;

    for (;;)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Calling OCIStmtFetch2 fetch_count=%u", bc.fetch_count);

        fetch_status = OCIStmtFetch2(stmt, ctx->errhp,
                                     bc.fetch_count,
                                     OCI_FETCH_NEXT, 0,
                                     OCI_DEFAULT);

        if (fetch_status != OCI_SUCCESS         &&
            fetch_status != OCI_SUCCESS_WITH_INFO &&
            fetch_status != OCI_NO_DATA)
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "OCIStmtFetch2 unexpected status=%d", fetch_status);
            CHECK_OCI(ctx->errhp, fetch_status);
            rc = -1;
            goto Cleanup;
        }

        ub4 rows_fetched = 0;
        OCIAttrGet(stmt, OCI_HTYPE_STMT,
                   &rows_fetched, 0,
                   OCI_ATTR_ROWS_FETCHED, ctx->errhp);

        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "rows_fetched=%u fetch_status=%d",
                     rows_fetched, fetch_status);

        if (rows_fetched == 0)
            break;

        for (ub4 r = 0; r < rows_fetched; r++)
        {
            abs_rownum++;
            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                         "Processing r=%u abs_rownum=%u", r, abs_rownum);

            int row_rc = build_row_xml_batch(ctx, &bc,
                                              r, abs_rownum,
                                              BLOB_list, &BLOB_index,
                                              max_lobs,
                                              &CLOB_index,
                                              max_clobs,
                                              &clob_bytes,
                                              xml,
											  rs);
            if (row_rc != 0)
            {
                logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                             "build_row_xml_batch failed r=%u", r);
                rc = -1;
                goto Cleanup;
            }
        }

        if (fetch_status == OCI_NO_DATA)
            break;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Fetch loop complete. Total rows=%u BLOBS=%d CLOBS=%d",
                 abs_rownum, BLOB_index, CLOB_index);


    metrics.execution_us  = metrics_now_us() - exec_start_us;
    metrics.rows_affected = (uint64_t)abs_rownum;

    /* ================================================================
     *  Stage 6 - Finalise XML
     * ================================================================ */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Stage 6: Finalise XML");

    /* xml_end_resultset(xml); */ /* Unused: replaced below - resultset body now comes from response_write_xml(ctx, rs), the new parsing-layer writer that supports both XML and JSON from the same resultset_t */
    {
        char *resultset_xml = response_write_xml(ctx, rs);
        if (resultset_xml)
        {
            xml_append_raw(xml, resultset_xml);
            free(resultset_xml);
        }
        else
        {
            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                         "response_write_xml returned NULL - resultset "
                         "section will be missing from OUTPUT_XML");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed =
        (ts_end.tv_sec  - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    xml_start_execution(xml);
    xml_append(xml, "<num_rows>%u</num_rows>\n",               abs_rownum);
    xml_append(xml, "<execution_time_total>%.6f</execution_time_total>\n",
               elapsed);
    xml_append(xml, "<fetch_batch_size>%u</fetch_batch_size>\n",
               bc.fetch_count);
    xml_append(xml, "<blobs_extracted>%d</blobs_extracted>\n", BLOB_index);
    xml_append(xml, "<clobs_extracted>%d</clobs_extracted>\n", CLOB_index);
    xml_end_execution(xml);
    xml_finalize(xml);


    /* ---- Stage 3: compare old and new resultset XML (temporary - verification only) ---- */
     /*   char *new_response_xml = response_write_xml(ctx, rs);

        if (new_response_xml)
        {
            printf("\n===== OLD (existing xml->buffer) =====\n%s\n", xml->buffer);
            printf("\n===== NEW (response_write_xml) =====\n%s\n", new_response_xml);

            if (strcmp(xml->buffer, new_response_xml) == 0)
            {
                printf("\n[STAGE3] MATCH - old and new resultset XML are identical\n");
                logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                             "STAGE3 MATCH - old and new resultset XML identical");
            }
            else
            {
                printf("\n[STAGE3] MISMATCH - old and new resultset XML differ\n");
                logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                             "STAGE3 MISMATCH - old and new resultset XML differ");
            }

            free(new_response_xml);
        }
        else
        {
            printf("\n[STAGE3] response_write_xml returned NULL\n");
        }

	*/


    /* ---- Stage 3: compare old and new resultset XML (temporary - verification only) ----
     * Unused/dead: this block existed to verify response_write_xml(ctx, rs)
     * produced the same <resultset> fragment as the old manually-built
     * xml->buffer. Now that xml->buffer's resultset section IS
     * response_write_xml(ctx, rs)'s output (spliced in above), there is no
     * separate "old" implementation left to diff against - this would only
     * ever compare the new output to itself. */
#if 0
    char *new_response_xml = response_write_xml(ctx, rs);

    if (new_response_xml)
    {
        /* Extract just <resultset>...</resultset> from the old buffer -
         * response_write_xml() only ever produces that fragment, not the
         * surrounding <output_xml>/<execution_envelope> wrapper, so
         * comparing against the whole old buffer was never a fair
         * like-for-like check.                                          */
        const char *old_start = strstr(xml->buffer, "<resultset>");
        const char *old_end   = old_start ? strstr(old_start, "</resultset>") : NULL;

        if (old_start && old_end)
        {
            old_end += strlen("</resultset>");
            size_t old_fragment_len = (size_t)(old_end - old_start);

            char *old_fragment = malloc(old_fragment_len + 1);
            if (old_fragment)
            {
                memcpy(old_fragment, old_start, old_fragment_len);
                old_fragment[old_fragment_len] = '\0';


                /* Trim trailing whitespace/newline from both before comparing -
                 * xml_end_resultset() appends "</resultset>\n" with the
                 * newline as part of that one write, which response_write_xml()'s
                 * strdup(xml->buffer) naturally picks up but this substring
                 * extraction does not - a trivial difference, not a real one. */
                size_t ol = strlen(old_fragment);
                while (ol > 0 && (old_fragment[ol-1] == '\n' || old_fragment[ol-1] == '\r' || old_fragment[ol-1] == ' '))
                    old_fragment[--ol] = '\0';

                size_t nl = strlen(new_response_xml);
                while (nl > 0 && (new_response_xml[nl-1] == '\n' || new_response_xml[nl-1] == '\r' || new_response_xml[nl-1] == ' '))
                    new_response_xml[--nl] = '\0';


                printf("\n===== OLD resultset fragment only =====\n%s\n", old_fragment);
                printf("\n===== NEW (response_write_xml) =====\n%s\n", new_response_xml);

                if (strcmp(old_fragment, new_response_xml) == 0)
                {
                    printf("\n[STAGE3] MATCH - old and new resultset XML are identical\n");
                    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                 "STAGE3 MATCH - old and new resultset XML identical");
                }
                else
                {
                    printf("\n[STAGE3] MISMATCH - old and new resultset XML differ\n");
                    logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                                 "STAGE3 MISMATCH - old and new resultset XML differ");
                }

                free(old_fragment);
            }
        }
        else
        {
            printf("\n[STAGE3] Could not find <resultset> in old buffer\n");
        }

        free(new_response_xml);
    }
    else
    {
        printf("\n[STAGE3] response_write_xml returned NULL\n");
    }
#endif


    /* ---- Stage 3b: verify JSON writer (temporary - verification only) ---- */
    char *new_response_json = response_write_json(ctx, rs);

    if (new_response_json)
    {
        printf("\n===== NEW (response_write_json) =====\n%s\n", new_response_json);
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "STAGE3b response_write_json produced output len=%zu",
                     strlen(new_response_json));

        verify_response_json_against_resultset(ctx, rs, new_response_json);

        free(new_response_json);
    }
    else
    {
        printf("\n[STAGE3b] response_write_json returned NULL\n");
    }






    if (!cfg->xml)
        cfg->xml = calloc(1, sizeof(*cfg->xml));

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Setting cfg->xml->OUTPUT_XML");
    cfg->xml->OUTPUT_XML = strdup(xml->buffer);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "execute_query_batch complete rows=%u elapsed=%.6f",
                 abs_rownum, elapsed);

    /* ---- Render JSON once, store both formats in cache ----
     * response_writer_cache_store() renders JSON from rs and, when
     * caching is enabled, stores it alongside the XML string above on
     * one cache entry - so a later hit, in either format, is served
     * directly with no re-render and no re-execution.
     *
     * This is also the actual fix for JSON requests never receiving a
     * JSON response: previously nothing populated a JSON output field
     * at all, cached or not, regardless of ReturnFormat.               */

    /* Table-level cache invalidation (closure item 5 follow-up,
     * 2026-08-12) - build the comma-separated table dependency tag
     * from deps (Stage 0's own output, already computed above - no
     * new parsing) and pass it through opts, so this entry can later
     * be found and expired by resultset_cache_invalidate_by_table()
     * when a write modifies one of these same tables. Regression fix
     * for UT-SEL-004 (a post-UPDATE SELECT could serve a stale cached
     * result up to resultset_cache_ttl_seconds after the write, since
     * nothing previously invalidated anything on write at all).       */
    char table_tag[512];
    table_tag[0] = '\0';
    for (int di = 0; di < deps.object_count; di++)
    {
        if (!deps.objects[di].object_name[0]) continue;
        size_t used = strlen(table_tag);
        if (used > 0 && used < sizeof(table_tag) - 1)
        {
            table_tag[used] = ',';
            table_tag[used + 1] = '\0';
        }
        strncat(table_tag, deps.objects[di].object_name,
                sizeof(table_tag) - strlen(table_tag) - 1);
    }

    cache_entry_opts_t cache_opts;
    memset(&cache_opts, 0, sizeof(cache_opts));
    if (table_tag[0]) cache_opts.table_dependency_tag = table_tag;

    char *json_output = NULL;
    int   json_rc = response_writer_cache_store(
                        ctx,
                        (ctx->resultset_cache && !served_from_cache && cache_key[0])
                            ? ctx->resultset_cache : NULL,
                        cache_key,
                        rs,
                        cfg->xml->OUTPUT_XML,
                        (uint64_t)abs_rownum,
                        table_tag[0] ? &cache_opts : NULL,
                        &json_output);

    if (json_rc == 0 && json_output)
    {
        if (cfg->ReturnFormat && strcasecmp(cfg->ReturnFormat, "JSON") == 0)
            cfg->OUTPUT_JSON = json_output;   /* ownership transferred to cfg */
        else
            free(json_output);                /* not requested this call     */
    }
    else
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                     "JSON rendering failed key='%.80s' - a JSON request "
                     "this call would not receive a response body",
                     cache_key);
    }

    /* ---- Update cache exec stats on success ----
     * The actual cache_insert (both formats) already happened above via
     * response_writer_cache_store(); this block only records execution
     * stats, matching the miss-path timing previously recorded here.  */
    if (ctx->resultset_cache && !served_from_cache &&
        cfg->xml && cfg->xml->OUTPUT_XML && cache_key[0])
    {
        if (json_rc == 0)
            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                         "Stored result in cache key='%.80s' rows=%u "
                         "(xml+json)", cache_key, abs_rownum);
        else
            logger_write(ctx->select_logger, LOG_WARN, __func__, 0,
                         "Cache store incomplete (JSON render failed, "
                         "non-fatal) key='%.80s'", cache_key);

        cache_update_exec_stats(ctx->resultset_cache,
                                elapsed * 1000.0,  /* execution_ms    */
                                0.0,               /* fetch_ms        */
                                (uint64_t)abs_rownum,
                                0,                 /* was_cache_hit   */
                                1);                /* success         */
    }

     /* ================================================================== */
     /*  WRITE BLOCK - place at the end of Stage 6 (after xml_finalize)    */
     /*  and also in the Cleanup label for the error path                   */
     /* ================================================================== */

         /* Success path - after xml_finalize() */
         metrics.end_time_us  = metrics_now_us();
         metrics.status_code  = 0;
         strncpy(metrics.error_code, "-",  sizeof(metrics.error_code) - 1);
         strncpy(metrics.error_text, "-",  sizeof(metrics.error_text) - 1);
         metrics.rows_affected    = abs_rownum;
         metrics.output_xml_bytes = (xml && xml->buffer)
                                    ? (uint64_t)strlen(xml->buffer) : 0;

         /* Accumulate BLOB bytes from BLOB_list                       */
         for (int _bi = 0; _bi < BLOB_index; _bi++)
             lob_bytes += (uint64_t)BLOB_list[_bi].blob_size;
         metrics.lob_bytes  = lob_bytes;
         metrics.clob_bytes = clob_bytes;
         /* transaction_id already set at init time                    */



Cleanup:

	resultset_free(rs);   /* ADD THIS LINE - not yet consumed, just avoiding a leak */

    /* ================================================================
     *  Stage 7 - Cleanup: reverse allocation order, guard all frees
     * ================================================================ */
	metrics.end_time_us = metrics_now_us();
	metrics.status_code = rc;

	/* Error path - in Cleanup label when rc != 0 */
	if (rc!=0){
		    strncpy(metrics.error_code,
		            logger_last_error.error_code,
		            sizeof(metrics.error_code) - 1);
		    strncpy(metrics.error_text,
		            logger_last_error.error_text,
		            sizeof(metrics.error_text) - 1);
	}
	if(ctx->active_tx)
		strncpy(metrics.transaction_id , tx_get_id(ctx->active_tx),sizeof(metrics.transaction_id)-1);
	else
		strncpy(metrics.transaction_id , "-",sizeof(metrics.transaction_id)-1);
	strncpy(metrics.transaction_name , ctx->active_tx ? ctx->active_tx->tx_name : "-", sizeof(metrics.transaction_name)-1);
	metrics.connection_wait_us    = ctx->connection_wait_us;
	metrics.connection_create_us  = ctx->connection_create_us;
	metrics.connection_acquire_us = ctx->connection_acquire_us;

	//Process final 3 metrics
	//printf("DEBUG : cfg->input_file_name=%s\n",cfg->input_file_name);
	if (ctx->ini && ctx->ini->metrics_display_input_file_name && cfg->input_file_name)
	    metrics.input_file_name = flatten_for_csv(cfg->input_file_name);

	if (ctx->ini && ctx->ini->metrics_display_input_request && ctx->INPUT_XML)
	    metrics.input_request = flatten_for_csv3(ctx->INPUT_XML);


	if (ctx->ini && ctx->ini->metrics_display_output_response)
	{
	    /* Serve whichever format was actually returned to the caller -
	     * this used to always read cfg->xml->OUTPUT_XML regardless of
	     * ReturnFormat, so a JSON request's metrics row showed the XML
	     * rendering instead of the JSON it actually got back.          */
	    int is_json = (cfg->ReturnFormat &&
	                   strcasecmp(cfg->ReturnFormat, "JSON") == 0);

	    if (is_json && cfg->OUTPUT_JSON)
	        metrics.output_response = flatten_for_csv3(cfg->OUTPUT_JSON);
	    else if (cfg->xml && cfg->xml->OUTPUT_XML)
	        metrics.output_response = flatten_for_csv3(cfg->xml->OUTPUT_XML);
	}

	metrics_finalise_and_enqueue(ctx->metrics_writer, ctx->metrics_writer_logger, &metrics);
    logger_clear_last_error();   // reset for next operation


    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Stage 7: Cleanup");

    if (stmt_count)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Calling OCIStmtRelease for stmt_count");
        OCIStmtRelease(stmt_count, ctx->errhp, NULL, 0, OCI_DEFAULT);
        stmt_count = NULL;
    }

    if (BLOB_list)
    {
        for (int i = 0; i < BLOB_index; i++)
        {
            if (BLOB_list[i].lob_loc)
            {
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "OCIDescriptorFree BLOB_list[%d].lob_loc", i);
                OCIDescriptorFree(BLOB_list[i].lob_loc, OCI_DTYPE_LOB);
                BLOB_list[i].lob_loc = NULL;
            }
            if (BLOB_list[i].blob_data)
            {
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "free(BLOB_list[%d].blob_data)", i);
                free(BLOB_list[i].blob_data);
                BLOB_list[i].blob_data = NULL;
            }
            if (BLOB_list[i].file_name)
            {
                logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                             "free(BLOB_list[%d].file_name)", i);
                free(BLOB_list[i].file_name);
                BLOB_list[i].file_name = NULL;
            }
        }
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "free(BLOB_list)");
        free(BLOB_list);
        BLOB_list = NULL;
    }

    free_batch_ctx(ctx, &bc);

    if (xml)
    {
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "xml_free(xml)");
        xml_free(xml);
        xml = NULL;
    }

    if (stmt)
    {
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                     "Calling OCIStmtRelease for stmt");
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        stmt = NULL;
    }

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Cleanup complete. rc=%d", rc);
    return rc;
}
