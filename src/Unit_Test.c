
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>      // Fix implicit isspace() declaration
#include "logger.h"
#include "Unit_Test.h"
#include <OCI_Connection.h>
#include <OCI_Execute_Query_Module.h>

#define MAX_SQL_LINE 2048

static void trim_newline(char *str) {
    size_t len = strlen(str);
    if (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r'))
        str[len - 1] = '\0';
}

/**
 * Run unit tests from a SQL file.
 * Assumes ctx->logger and ctx->log_level are valid.
 * @param ctx Pointer to OCI context
 * @param test_sql_file_name Path to SQL test file
 * @return number of failures
 */
int run_unit_tests_Back_10_Mar(oci_context_t *ctx, const char *test_sql_file_name) {
    FILE *fp;
    char line[MAX_SQL_LINE];
    int total_tests = 0;
    int failures = 0;

    logger_write(ctx->logger, LOG_DEBUG, __func__, 0, "Entered run_unit_tests");

    if (!ctx || !ctx->logger) {
        logger_write(ctx->logger, LOG_DEBUG, __func__, 0, "Invalid OCI context or logger");
        failures=-1;
        goto Cleanup;
      }

    logger_write(ctx->logger,
                 LOG_INFO,
                 __func__,
                 0,
                 "Opening test SQL file: %s",
                 test_sql_file_name);

    fp = fopen(test_sql_file_name, "r");
    if (!fp) {
        logger_write(ctx->logger,
                     LOG_ERROR,
                     __func__,
                     0,
                     "Failed to open test SQL file");
        failures=-1;
         goto Cleanup;
    }

    logger_write(ctx->logger,
    		LOG_INFO,
                 __func__,
                 0,
                 "Successfully opened test SQL file");

    while (fgets(line, sizeof(line), fp)) {
          trim_newline(line);
          logger_write(ctx->logger,  LOG_INFO,__func__, 0, "Reading line %s", line);

        // Skip empty lines or comments
        char *ptr = line;
        while (*ptr && isspace((unsigned char)*ptr)) ptr++;
        if (*ptr == '\0' || strncmp(ptr, "--", 2) == 0)
            continue;

        total_tests++;

        logger_write(ctx->logger,
        		LOG_INFO,
                     __func__,
                     0,
                     "Read line (trimmed): '%s'",
                     line);

        execute_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.SQL = line;
        cfg.log_execute_stats = 1;

        logger_write(ctx->logger,
        		LOG_INFO,
                     __func__,
                     0,
                     "Executing test %d: %s",
                     total_tests,
                     line);

        logger_write(ctx->logger, LOG_INFO, __func__, 0, "Calling execute_query");
        int rc = execute_query(ctx, &cfg);
        if (rc != 0) {
            failures++;
            logger_write(ctx->logger,
            		LOG_INFO,
                        __func__,
                         0,
                         "Test %d FAILED",
                         total_tests);
        } else {
            logger_write(ctx->logger,
            		LOG_INFO,
                         __func__,
                         0,
                         "Test %d PASSED",
                         total_tests);

            // Optional: print XML for debugging
            if (cfg.xml && cfg.xml->OUTPUT_XML) {
                logger_write(ctx->logger,
                		LOG_INFO,
                                __func__,
                             0,
                             "Generated XML for test %d:\n%s",
                             total_tests,
                             cfg.xml->OUTPUT_XML);
            }
        }
    }
    logger_write(ctx->logger, LOG_INFO, __func__, 0, "Closing Test file");

    fclose(fp);
    logger_write(ctx->logger,
      		LOG_INFO,
                      __func__,
                   0,
                   "Generated XML for test \n");



 Cleanup:

    return failures;
}
