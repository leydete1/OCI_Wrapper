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
#include <errno.h>     /* write_blob_to_file() - 2026-10-14, point a3 */
#include <pthread.h>   /* lob_output_tag() counter - point a1          */

#include "Blob_Utils.h"
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

    /* 2026-10-14 (point a3): written as <path>.part, then renamed into
     * place - rename() replaces the name in one step, so anything
     * reading the output folder sees the whole file or none of it. A
     * short write or a failed close used to go unnoticed. */
    char path[1024];
    char part[1040];
    snprintf(path, sizeof(path), "%s/%s", output_dir, item->file_name);
    snprintf(part, sizeof(part), "%s.part", path);

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Opening file %s", part);
    FILE *fp = fopen(part, "wb");
    if (!fp)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Error opening file %s: %s", part, strerror(errno));
        return -1;
    }

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Writing file %s", part);
    size_t written  = fwrite(item->blob_data, 1, item->blob_size, fp);
    int    close_rc = fclose(fp);
    if (written != (size_t)item->blob_size || close_rc != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Error writing file %s: %zu of %zu bytes written, "
                     "close rc=%d", part, written, (size_t)item->blob_size,
                     close_rc);
        remove(part);
        return -1;
    }

    if (rename(part, path) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Error renaming %s to %s: %s", part, path,
                     strerror(errno));
        remove(part);
        return -1;
    }
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Wrote file %s", path);

    return 0;
}

void generate_timestamp(char *buffer, size_t size, oci_context_t *ctx)
{
    (void)ctx;
    time_t now = time(NULL);
    struct tm t;
    /* 2026-10-14: localtime_r - localtime() returns a buffer shared by
     * every thread, so two workers could read each other's time. */
    localtime_r(&now, &t);

    strftime(buffer, size, "%Y%m%d_%H%M%S", &t);
}

/* 2026-10-14 (point a1) - see Blob_Utils.h. */
static pthread_mutex_t g_lob_tag_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long   g_lob_tag_seq   = 0;

void lob_output_tag(char *out, size_t out_size)
{
    char ts[32];
    generate_timestamp(ts, sizeof(ts), NULL);

    pthread_mutex_lock(&g_lob_tag_mutex);
    g_lob_tag_seq = (g_lob_tag_seq % 999999UL) + 1;   /* 1..999999 */
    unsigned long seq = g_lob_tag_seq;
    pthread_mutex_unlock(&g_lob_tag_mutex);

    snprintf(out, out_size, "%s_%06lu", ts, seq);
}

/* 2026-10-14 (point a2): the time part is now the caller's tag (one per
 * SELECT, from lob_output_tag()) instead of a timestamp taken here. */
void build_filename_with_timestamp(const char *original, const char *tag,
                                    char *output, size_t out_size, int idx,
                                    oci_context_t *ctx)
{
    char name[256] = {0};
    char ext[64] = {0};

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "In build_filename_with_timestamp for idx=%d", idx);

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

    snprintf(output, out_size, "%s_%s_%d%s", name, tag, idx, ext);
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "Leaving build_filename_with_timestamp output=%s", output);
}
