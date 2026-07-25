/*
 * Level1_Insert_Test.c
 *
 * Standalone Level 1 verification harness for the new OP_INSERT payload
 * builder. Does NOT connect to Oracle - level1_parse() never touches
 * ctx->envhp/svchp/etc, only ctx->logger - so this only needs a logger
 * initialised, nothing else from oci_context_t.
 *
 * Batch mode: scans INPUT_XML_DIR below (hardcoded, taken straight from
 * a real run's xml_input_dir - see the #define) for .xml/.json files
 * and runs level1_parse() against every one, printing a per-file result
 * and a final tally. No config.ini/load_ini() dependency at all now -
 * one less moving part while chasing down anything unexpected, and one
 * less library (ini_reader.c) this harness needs linked in.
 *
 * If your input_xml directory ever moves, update INPUT_XML_DIR below
 * and rebuild - that's the only place it's specified.
 *
 * What it checks per fixture:
 *   1. level1_parse() returns LEVEL1_OK
 *   2. external_audit_id / session_id / operation_count are right
 *   3. operations[0].type == OP_INSERT and payload is non-NULL
 *   4. insert_request_t contents (table_name, owner, row_count, and
 *      every row's field_count/field_name/value) match the fixture
 *   5. level1_free_request() runs clean - run this whole binary under
 *      valgrind to confirm no leaks from the new insert_request_t
 *      free path in level1_free_request()
 *
 * Build (adjust -I/-L paths to match your OCI Instant Client + libxml2
 * install, same as Test_XML_Runner.c's documented build line):
 *
 *   gcc -o Level1_Insert_Test \
 *       Level1_Insert_Test.c \
 *       OCI_Level1_Parser.c \
 *       logger.c \
 *       $(pkg-config --cflags --libs libxml-2.0) \
 *       -I. -lclntsh -lpthread -lm
 *
 * Run - no arguments needed, always scans INPUT_XML_DIR:
 *   ./Level1_Insert_Test
 *
 * Run - single file, bypassing INPUT_XML_DIR entirely:
 *   ./Level1_Insert_Test --file Unit_Test_Insert_Round_1.xml
 *
 * Leak check:
 *   valgrind --leak-check=full ./Level1_Insert_Test --file Unit_Test_Insert_Round_5.json
 *   (Round 5's JSON fixture is the current priority - the last batch
 *   run showed a "stack smashing detected" message right on this file
 *   before printing its result - needs isolating to confirm whether
 *   it's real corruption in build_payload_json's OP_INSERT path or
 *   console cross-talk from a stale process.)
 */

#define _POSIX_C_SOURCE 200809L

/* Hardcoded from the last real run's logged xml_input_dir - update this
 * and rebuild if the directory ever moves. Removed the load_ini()
 * dependency entirely per 2026-07-23 decision - one less moving part. */
#define INPUT_XML_DIR "/home/leyden100/eclipse-workspace/OCI_Wrapper/OCI_Tester/Input_XML"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>     /* getcwd() - debug print only */
#include <dirent.h>
#include <sys/stat.h>

#include "OCI_Level1_Parser.h"
#include "OCI_Insert_Execute_Module.h"   /* insert_request_t, insert_row_t */
#include "logger.h"

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
    printf("    insert_request_t:\n");
    printf("      table_name = \"%s\"\n", req->table_name);
    printf("      owner      = \"%s\"\n", req->owner);
    printf("      row_count  = %d\n", req->row_count);

    for (int r = 0; r < req->row_count; r++)
    {
        const insert_row_t *row = &req->rows[r];
        printf("      row[%d]: field_count = %d\n", r, row->field_count);
        for (int f = 0; f < row->field_count; f++)
        {
            printf("        field[%d]: field_name = \"%s\", value = \"%s\"\n",
                   f, row->fields[f].field_name, row->fields[f].value);
        }
    }
}

/*
 * run_one_file()
 *
 * Runs level1_parse()/level1_free_request() against one file and
 * prints the result. Returns 0 on LEVEL1_OK, non-zero otherwise -
 * caller tallies pass/fail, same convention as Test_XML_Runner.c's
 * process_xml_file() return value.
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
    printf("  level1_parse() returned %d\n", rc);
    free(buf);

    if (rc != LEVEL1_OK)
    {
        printf("  FAIL: level1_parse failed\n");
        printf("    error_code = %s\n", error_detail.error_code);
        printf("    error_text = %s\n", error_detail.error_text);
        return -1;
    }

    printf("  OK: level1_parse succeeded\n");
    printf("    external_audit_id = \"%s\"\n", request.external_audit_id);
    printf("    session_id        = \"%s\"\n", request.session_id);
    printf("    transaction_required = %d\n", request.transaction_required);
    printf("    operation_count    = %d\n", request.operation_count);

    int any_missing_payload = 0;

    for (int i = 0; i < request.operation_count; i++)
    {
        input_c_operation_t *op = &request.operations[i];
        printf("    operation[%d]: type = %d (%s)\n", i, op->type,
               op->type == OP_INSERT ? "OP_INSERT" :
               op->type == OP_SELECT ? "OP_SELECT" : "other");

        if (op->type == OP_INSERT)
        {
            if (!op->payload)
            {
                printf("    FAIL: payload is NULL for OP_INSERT\n");
                any_missing_payload = 1;
                continue;
            }
            print_insert_request((const insert_request_t *)op->payload);
        }
    }

    printf("  calling level1_free_request()...\n");
    level1_free_request(&request);
    printf("  level1_free_request() completed\n");

    return any_missing_payload ? -1 : 0;
}

/*
 * run_directory()
 *
 * Scans dir_path for .xml/.json files (same suffix-matching convention
 * as Test_XML_Runner.c's own input_xml scan) and runs run_one_file()
 * against each, in whatever order readdir() returns them - matching
 * every other batch runner in this project, this does not sort or
 * otherwise impose an order.
 */
static int run_directory(oci_context_t *ctx, const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
    {
        fprintf(stderr, "Failed to open directory: %s\n", dir_path);
        return -1;
    }

    int total = 0, passed = 0, failed = 0;
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
            passed++;
        else
            failed++;

        printf("------------------------------------------------\n\n");
    }

    closedir(dir);

    printf("==================================================\n");
    printf("Total: %d   Passed: %d   Failed: %d\n", total, passed, failed);
    printf("==================================================\n");

    return failed == 0 ? 0 : -1;
}

int main_JUL24(int argc, char **argv)
{
    /* Force unbuffered stdout - see header comment for why. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Only --file <path> changes behaviour - everything else (no
     * arguments, or a stale/leftover argument from an old Eclipse Run
     * Configuration) is ignored and falls through to the hardcoded
     * INPUT_XML_DIR scan. The path being hardcoded means there's
     * nothing else a stray argument could usefully mean here.          */
    int file_mode = (argc == 3 && strcmp(argv[1], "--file") == 0);

    oci_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    logger_t log;
    if (logger_init(&log, "Level1_Insert_Test.log", 1024 * 1024, 3, LOG_DEBUG) != 0)
    {
        fprintf(stderr, "logger_init failed\n");
        return 1;
    }
    ctx.logger = &log;

    if (file_mode)
        return run_one_file(&ctx, argv[2]) == 0 ? 0 : 1;

    if (argc > 1)
        printf("(ignoring argument(s) - not --file <path>, defaulting to directory scan)\n");

    char *cwd = getcwd(NULL, 0);
    printf("logger_init() OK - writing to Level1_Insert_Test.log in %s\n", cwd ? cwd : "?");
    free(cwd);

    printf("INPUT_XML_DIR (hardcoded) = %s\n", INPUT_XML_DIR);

    int rc = run_directory(&ctx, INPUT_XML_DIR);
    return rc == 0 ? 0 : 1;
}
