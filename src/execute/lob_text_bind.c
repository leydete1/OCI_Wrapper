/*
 * lob_text_bind.c   (driver_mssql Stage 4, 2026-10-20, point U6)
 *
 * See lob_text_bind.h. The file:// handling is the same as the
 * post-execute CLOB write in Insert_Execute_Module.c and
 * Update_Execute_Module.c (open, read whole, NUL-terminate).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lob_text_bind.h"

#define LOB_TEXT_FILE_PREFIX     "file://"
#define LOB_TEXT_FILE_PREFIX_LEN 7

int lob_text_resolve(const char *raw, const char **out_value, char **out_heap,
                     size_t *out_len, logger_t *logger)
{
    if (!raw || !out_value || !out_heap || !out_len) return -1;
    *out_heap  = NULL;
    *out_value = raw;
    *out_len   = 0;

    if (strncmp(raw, LOB_TEXT_FILE_PREFIX, LOB_TEXT_FILE_PREFIX_LEN) != 0)
    {
        *out_len = strlen(raw);
        return 0;
    }

    const char *file_path = raw + LOB_TEXT_FILE_PREFIX_LEN;
    FILE *fp = fopen(file_path, "r");
    if (!fp)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "Failed to open CLOB file: %s", file_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize < 0) fsize = 0;

    char *buf = malloc((size_t)fsize + 1);
    if (!buf)
    {
        fclose(fp);
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "malloc failed for CLOB file %s (%ld bytes)", file_path, fsize);
        return -1;
    }
    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    fclose(fp);
    buf[nread] = '\0';

    *out_heap  = buf;
    *out_value = buf;
    *out_len   = nread;
    return 0;
}
