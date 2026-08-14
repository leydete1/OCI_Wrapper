/*
 * Level2_Insert_Test.c
 *
 * Level 2 verification harness for level2_validate_insert(). Unlike
 * Level 1, this DOES need a real, live OCI connection -
 * metadata_cache_get_or_fetch() falls straight through to a real query
 * against ALL_TAB_COLUMNS on a cache miss, so there is no
 * connection-free way to exercise this stage (see the design note in
 * OCI_Level2_Parser.h on why SELECT's row-count guard and INSERT's
 * metadata resolution land on opposite sides of the "does this need a
 * connection" question).
 *
 * Loads config.ini for connection details (username/password/dbname/
 * wallet) and per-module log file paths, initialises only the loggers
 * actually touched by the code paths this harness exercises
 * (error/logger/connection/Metadata/insert - logger_write() is
 * NULL-safe, so every other logger is deliberately left NULL rather
 * than pulled in for no reason), connects once via OCI_Connect(), then
 * runs level1_parse() + level2_validate_insert() against every
 * .xml/.json fixture in INPUT_XML_DIR, disconnecting once at the end.
 *
 * What "good" looks like across the fixture set:
 *   Rounds 1-5  -> LEVEL2_OK
 *   Round 6     -> LEVEL2_ERR_FIELD_INVALID (VARCHAR2_COL value too
 *                  long - this time nothing in the request claims a
 *                  length at all; catching it means the real column
 *                  length was actually resolved from metadata_cache,
 *                  not trusted from the client)
 *   Round 7     -> LEVEL2_ERR_FIELD_INVALID (FIELD_UNKNOWN_COLUMN -
 *                  references a column that doesn't exist on the table)
 *   Round 8     -> LEVEL2_ERR_FIELD_INVALID (FIELD_MISSING_REQUIRED_COLUMN -
 *                  omits a NOT NULL column with no default entirely)
 *
 * Build (adjust -I/-L paths to match your OCI Instant Client + libxml2
 * + cJSON install):
 *
 *   gcc -o Level2_Insert_Test \
 *       Level2_Insert_Test.c \
 *       OCI_Level1_Parser.c \
 *       OCI_Level2_Parser.c \
 *       OCI_Insert_Validate_Module.c \
 *       OCI_Table_Metadata_Module.c \
 *       metadata_cache.c \
 *       oci_cache.c \
 *       OCI_Connection.c \
 *       XML_Helper.c \
 *       string_utils.c \
 *       ini_reader.c \
 *       logger.c \
 *       sql_dependency_extractor.c \
 *       $(pkg-config --cflags --libs libxml-2.0) \
 *       -I. -lclntsh -lcjson -lpthread -lm
 *
 * Run - no arguments needed, same as Level1_Insert_Test:
 *   ./Level2_Insert_Test
 *
 * Run - single file, bypassing INPUT_XML_DIR entirely:
 *   ./Level2_Insert_Test --file Unit_Test_Insert_Round_6.xml
 */

#define _POSIX_C_SOURCE 200809L

/* Same two hardcoded paths, same reasoning as Level1_Insert_Test.c -
 * update these and rebuild if either ever moves.                      */
#define INPUT_XML_DIR "/home/leyden100/eclipse-workspace/OCI_Wrapper/OCI_Tester/Input_XML"
#define CONFIG_INI    "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "OCI_Level1_Parser.h"
#include "OCI_Level2_Parser.h"
#include "OCI_Insert_Execute_Module.h"   /* insert_request_t, insert_row_t */
#include "OCI_Connection.h"
#include "ini_reader.h"
#include "logger.h"
#include "metadata_cache.h"

static char *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); return NULL; }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t read = fread(buf, 1, (size_t)size, fp);
    buf[read] = '\0';
    fclose(fp);

    *out_len = read;
    return buf;
}

static void print_insert_request(const insert_request_t *req)
{
    printf("    insert_request_t: table=%s.%s row_count=%d\n",
           req->owner, req->table_name, req->row_count);
    for (int r = 0; r < req->row_count; r++)
    {
        const insert_row_t *row = &req->rows[r];
        printf("      row[%d]:", r);
        for (int f = 0; f < row->field_count; f++)
            printf(" %s=\"%s\"", row->fields[f].field_name, row->fields[f].value);
        printf("\n");
    }
}

static const char *level2_err_name(int rc)
{
    switch (rc)
    {
        case LEVEL2_OK:                     return "LEVEL2_OK";
        case LEVEL2_ERR_INVALID_ARG:         return "LEVEL2_ERR_INVALID_ARG";
        case LEVEL2_ERR_EMPTY_SQL:           return "LEVEL2_ERR_EMPTY_SQL";
        case LEVEL2_ERR_SQL_INVALID:         return "LEVEL2_ERR_SQL_INVALID";
        case LEVEL2_ERR_NOT_IMPLEMENTED:     return "LEVEL2_ERR_NOT_IMPLEMENTED";
        case LEVEL2_ERR_VALIDATION_FAILED:   return "LEVEL2_ERR_VALIDATION_FAILED";
        case LEVEL2_ERR_ROW_COUNT_EXCEEDED:  return "LEVEL2_ERR_ROW_COUNT_EXCEEDED";
        case LEVEL2_ERR_METADATA_LOOKUP:     return "LEVEL2_ERR_METADATA_LOOKUP";
        case LEVEL2_ERR_FIELD_INVALID:       return "LEVEL2_ERR_FIELD_INVALID";
        default:                             return "?";
    }
}

/*
 * run_one_file()
 *
 * Runs level1_parse() then, for any OP_INSERT operation found,
 * level2_validate_insert(). Prints the result either way. Returns 0 if
 * level1_parse() succeeded (regardless of what level2_validate_insert()
 * found) - "Level 2 correctly rejected Round 6" is a PASS for this
 * harness, not a failure; the tally at the end is about whether the
 * harness itself ran cleanly against every file, not whether every
 * fixture was valid data. Check the printed LEVEL2_* result against
 * the "what good looks like" table in the header comment by eye.
 */
static int run_one_file(oci_context_t *ctx, const char *filepath)
{
    printf("  reading file...\n");
    size_t len = 0;
    char *buf = read_file(filepath, &len);
    if (!buf)
    {
        printf("  FAIL: could not read %s\n", filepath);
        return -1;
    }
    printf("  read %zu bytes\n", len);

    input_c_request_t request;
    operation_status_t error_detail;
    memset(&request, 0, sizeof(request));
    memset(&error_detail, 0, sizeof(error_detail));

    printf("  calling level1_parse()...\n");
    int rc = level1_parse(ctx, buf, len, &request, &error_detail);
    free(buf);

    if (rc != LEVEL1_OK)
    {
        printf("  FAIL: level1_parse failed - error_code=%s error_text=%s\n",
               error_detail.error_code, error_detail.error_text);
        return -1;
    }

    for (int i = 0; i < request.operation_count; i++)
    {
        input_c_operation_t *op = &request.operations[i];

        if (op->type != OP_INSERT)
        {
            printf("    operation[%d]: type=%d (not OP_INSERT - skipping Level 2)\n", i, op->type);
            continue;
        }

        print_insert_request((const insert_request_t *)op->payload);

        operation_status_t l2_status;
        memset(&l2_status, 0, sizeof(l2_status));

        printf("    calling level2_validate_insert()...\n");
        int l2_rc = level2_validate_insert(ctx, op, &l2_status);
        printf("    level2_validate_insert() returned %d (%s)\n", l2_rc, level2_err_name(l2_rc));
        printf("      status_code=%d error_code=%s\n", l2_status.status_code, l2_status.error_code);
        printf("      error_text=%s\n", l2_status.error_text);
    }

    level1_free_request(&request);
    return 0;
}

static int run_directory(oci_context_t *ctx, const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        fprintf(stderr, "Failed to open directory: %s\n", dir_path);
        return -1;
    }

    int total = 0, ran_ok = 0, ran_fail = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        const char *name = entry->d_name;
        size_t      nlen = strlen(name);

        int is_xml  = (nlen >= 5 && strcasecmp(name + nlen - 4, ".xml")  == 0);
        int is_json = (nlen >= 6 && strcasecmp(name + nlen - 5, ".json") == 0);

        if (!is_xml && !is_json)
            continue;

        char filepath[1024];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, name);

        struct stat st;
        if (stat(filepath, &st) != 0 || S_ISDIR(st.st_mode))
            continue;

        total++;
        printf("------------------------------------------------\n");
        printf("[%d] %s\n", total, name);

        if (run_one_file(ctx, filepath) == 0)
            ran_ok++;
        else
            ran_fail++;

        printf("------------------------------------------------\n\n");
    }

    closedir(dir);

    printf("==================================================\n");
    printf("Total: %d   Ran OK: %d   Harness failures: %d\n", total, ran_ok, ran_fail);
    printf("(check each file's printed LEVEL2_* result by eye against\n");
    printf(" the \"what good looks like\" table in this file's header -\n");
    printf(" a LEVEL2_ERR_* result is not a harness failure if that's\n");
    printf(" what the fixture is designed to trigger)\n");
    printf("==================================================\n");

    return ran_fail == 0 ? 0 : -1;
}

int main_25JUL(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int file_mode = (argc == 3 && strcmp(argv[1], "--file") == 0);

    oci_context_t ctx;
    app_config_t  config;
    memset(&ctx,    0, sizeof(ctx));
    memset(&config, 0, sizeof(config));
    ctx.ini             = &config;
    ctx.pool_slot_index = -1;

    printf("loading config: %s\n", CONFIG_INI);
    if (load_ini(CONFIG_INI, &config, &ctx) != 0)
    {
        fprintf(stderr, "Failed to load ini file: %s\n", CONFIG_INI);
        return 1;
    }

    /* ---- Only the loggers actually touched by OCI_Connect(),
     * metadata_cache_get_or_fetch()/get_request_metadata(),
     * level1_parse() (uses ctx->logger, not ctx->select_logger -
     * confirmed by grep), and level2_validate_insert()/validate_field() -
     * error, logger, connection, Metadata, insert. Every other logger
     * in oci_context_t is deliberately left NULL: logger_write() is
     * NULL-safe (see logger.c), so nothing downstream crashes, it just
     * means we're not paying for or polluting log files this harness
     * never needed. See this file's header comment.                    */
    logger_t error_logger, main_logger, connection_logger, metadata_logger, insert_logger;

    if (logger_init_str(&error_logger, config.error_log_file_name,
                         config.error_log_file_max_size,
                         config.error_log_file_rotation_number,
                         config.error_log_level) != 0)
    {
        fprintf(stderr, "Failed to init error_logger\n");
        return 1;
    }
    ctx.error_logger = &error_logger;

    if (logger_init_str2(&main_logger, config.log_file_name,
                          config.log_file_max_size,
                          config.log_file_rotation_number,
                          config.log_level, ctx.error_logger) != 0)
    {
        fprintf(stderr, "Failed to init logger (main)\n");
        return 1;
    }
    ctx.logger = &main_logger;

    if (logger_init_str2(&connection_logger, config.connection_log_file_name,
                          config.connection_log_file_max_size,
                          config.connection_log_file_rotation_number,
                          config.connection_log_level, ctx.error_logger) != 0)
    {
        fprintf(stderr, "Failed to init connection_logger\n");
        return 1;
    }
    ctx.connection_logger = &connection_logger;

    if (logger_init_str2(&metadata_logger, config.Metadata_log_file_name,
                          config.Metadata_log_file_max_size,
                          config.Metadata_log_file_rotation_number,
                          config.Metadata_log_level, ctx.error_logger) != 0)
    {
        fprintf(stderr, "Failed to init Metadata_logger\n");
        return 1;
    }
    ctx.Metadata_logger = &metadata_logger;

    if (logger_init_str2(&insert_logger, config.insert_log_file_name,
                          config.insert_log_file_max_size,
                          config.insert_log_file_rotation_number,
                          config.insert_log_level, ctx.error_logger) != 0)
    {
        fprintf(stderr, "Failed to init insert_logger\n");
        return 1;
    }
    ctx.insert_logger = &insert_logger;

    printf("loggers initialised (error/logger/connection/Metadata/insert only)\n");

    ctx.metadata_cache = metadata_cache_init(ctx.ini, ctx.Metadata_logger);
    printf("metadata_cache_init() -> %s\n", ctx.metadata_cache ? "enabled" : "disabled/NULL");

    printf("calling OCI_Connect()...\n");
    if (OCI_Connect(&ctx) != 0)
    {
        fprintf(stderr, "OCI_Connect failed - check %s\n", config.connection_log_file_name);
        return 1;
    }
    printf("OCI_Connect() OK\n");

    int rc;
    if (file_mode)
    {
        printf("------------------------------------------------\n");
        printf("%s\n", argv[2]);
        rc = run_one_file(&ctx, argv[2]);
        printf("------------------------------------------------\n");
    }
    else
    {
        if (argc > 1)
            printf("(ignoring argument(s) - not --file <path>, defaulting to directory scan)\n");
        printf("INPUT_XML_DIR (hardcoded) = %s\n", INPUT_XML_DIR);
        rc = run_directory(&ctx, INPUT_XML_DIR);
    }

    printf("calling OCI_Disconnect()...\n");
    OCI_Disconnect(&ctx);
    printf("OCI_Disconnect() done\n");

    /* ---- Cleanup - closes the two real leaks LeakSanitizer found.
     * The rest of what LSan reports (OCIEnvCreate/kpeDbgInitDBGC/
     * nlstdggo/OPENSSL_cleanup/___pthread_once, all inside
     * libclntsh.so/libclntshcore.so/libnnz.so) is Oracle Instant
     * Client's own one-time process-lifetime global init - not
     * anything this project allocates, and not freed until true
     * process teardown via its own atexit hooks, often after LSan has
     * already taken its snapshot. Nothing to do about that here.      */
    printf("calling metadata_cache_destroy()...\n");
    metadata_cache_destroy(ctx.metadata_cache);

    printf("calling logger_close() x5...\n");
    logger_close(&insert_logger);
    logger_close(&metadata_logger);
    logger_close(&connection_logger);
    logger_close(&main_logger);
    logger_close(&error_logger);

    return rc == 0 ? 0 : 1;
}
