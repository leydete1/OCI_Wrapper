/*
 * OCI_Blob_Utils.c
 *
 * See OCI_Blob_Utils.h for the relocation rationale.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "OCI_Blob_Utils.h"
#include "logger.h"

int lookup_blob_index(char (*col_names)[256], int col_count,
                       const char *col_name, oci_context_t *ctx)
{
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "In lookup_blob_index for %s", col_name);

    for (int i = 0; i < col_count; i++)
    {
        if (strcasecmp(col_names[i], col_name) == 0)
        {
            logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                         "Found match at i=%d", i);
            return i;
        }
    }
    logger_write(ctx->select_logger, LOG_WARN, __func__, 0, "No match found");
    return -1;
}

int write_blob_to_file(lob_item_t *item, const char *output_dir, oci_context_t *ctx)
{
    if (!item || !item->blob_data || item->blob_size == 0)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0, "Invalid BLOB");
        return -1;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", output_dir, item->file_name);

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Opening file %s", path);
    FILE *fp = fopen(path, "wb");
    if (!fp)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Error opening file %s", path);
        return -1;
    }

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Writing file %s", path);
    fwrite(item->blob_data, 1, item->blob_size, fp);
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Closing file %s", path);
    fclose(fp);

    return 0;
}

void generate_timestamp(char *buffer, size_t size, oci_context_t *ctx)
{
    (void)ctx;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    strftime(buffer, size, "%Y%m%d_%H%M%S", t);
}

void build_filename_with_timestamp(const char *original, char *output,
                                    size_t out_size, int idx, oci_context_t *ctx)
{
    char name[256] = {0};
    char ext[64] = {0};
    char timestamp[32];

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "In build_filename_with_timestamp for idx=%d", idx);
    generate_timestamp(timestamp, sizeof(timestamp), ctx);

    const char *dot = strrchr(original, '.');

    if (dot)
    {
        size_t name_len = dot - original;
        strncpy(name, original, name_len);
        name[name_len] = '\0';

        strncpy(ext, dot, sizeof(ext) - 1);
    }
    else
    {
        strncpy(name, original, sizeof(name) - 1);
        ext[0] = '\0';
    }

    snprintf(output, out_size, "%s_%s_%d%s", name, timestamp, idx, ext);
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Leaving build_filename_with_timestamp output=%s", output);
}
