/*
 * Level1_Test_Runner.c
 *
 * Standalone test harness for the Level 1 parser only. Deliberately
 * lean, per 2026-07-16 decision - no connection pool, no transaction,
 * no CRUD dispatch. Scans xml_input_dir, reads every file, runs it
 * through level1_parse(), and reports pass/fail per file plus a
 * summary. Will eventually be replaced by HTTP-sourced input - this
 * exists purely to prove the parsing layer out against real files
 * first.
 *
 * Uses the main logger only for now (matching 2026-07-16 decision) -
 * not the full initialise_loggers() from Test_XML_Runner.c, which is
 * static to that file and pulls in ~20 loggers this harness doesn't
 * need yet.
 *
 * Build: add to the same makefile/target as OCI_Level1_Parser.c, or
 * build as its own small executable - it only depends on ini_reader.c,
 * logger.c, and OCI_Level1_Parser.c, not the rest of the CRUD modules.
 *
 * Usage: ./Level1_Test_Runner <config.ini>
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "OCI_Connection.h"
#include "ini_reader.h"
#include "logger.h"
#include "OCI_Level1_Parser.h"

/* ------------------------------------------------------------------ */
/*  read_file - whole-file read into a heap buffer                     */
/* ------------------------------------------------------------------ */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t read_bytes = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);

    buf[read_bytes] = '\0';
    if (out_len) *out_len = read_bytes;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  is_regular_file                                                     */
/* ------------------------------------------------------------------ */
static int is_regular_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

/* ------------------------------------------------------------------ */
/*  main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2)
    {
        printf("Usage: %s <config.ini>\n", argv[0]);
        return -1;
    }

    app_config_t  config;
    oci_context_t ctx;
    logger_t      logger;

    memset(&ctx, 0, sizeof(ctx));
    memset(&config, 0, sizeof(config));
    ctx.ini             = &config;
    ctx.pool_slot_index = -1;

    if (load_ini(argv[1], &config, &ctx) != 0)
    {
        printf("Failed to load ini file: %s\n", argv[1]);
        return -1;
    }

    if (logger_init_str(&logger, config.log_file_name,
                         config.log_file_max_size,
                         config.log_file_rotation_number,
                         config.log_level) != 0)
    {
        printf("Failed to open main logger: %s\n", config.log_file_name);
        return -1;
    }
    ctx.logger = &logger;

    logger_write(&logger, LOG_INFO, __func__, 0,
                 "================================================");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "Level 1 Parser Test Runner");
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  config.ini      : %s", argv[1]);
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "  input directory : %s", config.xml_input_dir);
    logger_write(&logger, LOG_INFO, __func__, 0,
                 "================================================");

    DIR *dir = opendir(config.xml_input_dir);
    if (!dir)
    {
        logger_write(&logger, LOG_ERROR, __func__, 0,
                     "Cannot open input directory: %s", config.xml_input_dir);
        printf("Cannot open input directory: %s\n", config.xml_input_dir);
        logger_close(&logger);
        return -1;
    }

    int total = 0, passed = 0, failed = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.') continue;   /* skip . / .. / hidden */

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 config.xml_input_dir, entry->d_name);

        if (!is_regular_file(full_path)) continue;

        total++;

        size_t len = 0;
        char *buf = read_file(full_path, &len);
        if (!buf)
        {
            logger_write(&logger, LOG_ERROR, __func__, 0,
                         "[%s] Could not read file", entry->d_name);
            printf("[FAIL] %-45s could not read file\n", entry->d_name);
            failed++;
            continue;
        }

        input_c_request_t request;
        operation_status_t error_detail;
        memset(&request, 0, sizeof(request));
        memset(&error_detail, 0, sizeof(error_detail));

        int rc = level1_parse(&ctx, buf, len, &request, &error_detail);

        if (rc == LEVEL1_OK)
        {
            printf("[PASS] %-45s format=%s audit_id=%s session_id=%s operations=%d\n",
                   entry->d_name,
                   request.source_format == INPUT_FORMAT_XML ? "XML" : "JSON",
                   request.external_audit_id,
                   request.session_id,
                   request.operation_count);
            passed++;
            level1_free_request(&request);
        }
        else
        {
            printf("[FAIL] %-45s rc=%d error_code=%s error_text=%s\n",
                   entry->d_name, rc, error_detail.error_code, error_detail.error_text);
            failed++;
        }

        free(buf);
    }

    closedir(dir);

    printf("\n================================================\n");
    printf("Level 1 Parser Test Runner complete\n");
    printf("  total  : %d\n", total);
    printf("  passed : %d\n", passed);
    printf("  failed : %d\n", failed);
    printf("================================================\n");

    logger_write(&logger, LOG_INFO, __func__, 0,
                 "Level 1 Parser Test Runner complete: total=%d passed=%d failed=%d",
                 total, passed, failed);

    logger_close(&logger);
    return (failed == 0) ? 0 : -1;
}
