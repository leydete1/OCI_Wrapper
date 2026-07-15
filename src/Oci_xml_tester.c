
#define _POSIX_C_SOURCE 200809L

#include "Oci_xml_tester.h"
#include "logger.h"
#include <string_utils.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void build_timestamp(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    strftime(buf, size, "%d_%m_%Y_%H_%M_%S", t);
}


static int extract_sql_from_xml(oci_context_t *ctx, const char *filename, char *sql, size_t max)
{
	int status=0;

	logger_write(ctx->logger, LOG_INFO, __func__, 0,
	                 "Opening file %s for reading.", filename);
	FILE *fp = fopen(filename, "r");
    if (!fp){
    	logger_write(ctx->logger, LOG_ERROR, __func__, 0,
    	                 "Failed opening file %s for reading.", filename);
		status = -1;
		goto Clean_Up;
    }

    char line[2048];
    int in_sql = 0;

    sql[0] = '\0';
	logger_write(ctx->logger, LOG_INFO, __func__, 0,
	                 "Starting reading file %s.", filename);
    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "<sql>"))
        {
            in_sql = 1;
            continue;
        }

        if (strstr(line, "</sql>"))
        {
            break;
        }

        if (in_sql)
        {
            if (strlen(sql) + strlen(line) < max)
                strcat(sql, line);
        }
    }
	logger_write(ctx->logger, LOG_INFO, __func__, 0,
	                 "Finished reading file %s.", filename);


Clean_Up:
    fclose(fp);
    return (strlen(sql) > 0) ? 0 : status;
}


int list_available_tests(oci_context_t *ctx , const char *input_dir)
{
    DIR *d;
    struct dirent *entry;
	int status=0;

	logger_write(ctx->logger, LOG_INFO, __func__, 0,
	                 "Opening directory %s for reading.", input_dir);
    d = opendir(input_dir);
    if (!d)
    {
    	logger_write(ctx->logger, LOG_ERROR, __func__, 0,
    	                 "Failed Opening directory %s for reading.", input_dir);
        goto Clean_Up;
    }

	logger_write(ctx->logger, LOG_INFO, __func__, 0,
	                 "Available XML Tests in %s ", input_dir);
	logger_write(ctx->logger, LOG_INFO, __func__, 0,
	                 "A-------------------");
     while ((entry = readdir(d)) != NULL)
    {
        if (strstr(entry->d_name, ".xml")){
           	logger_write(ctx->logger, LOG_INFO, __func__, 0,
            	                 "%s", entry->d_name);
               }
    }

 Clean_Up:
    closedir(d);
    return status;
}

int execute_xml_test(oci_context_t *ctx,
                     app_config_t *config,
                     const char *filename)
{
    char sql[4096];
    int status=0;

    if (extract_sql_from_xml(ctx, filename, sql, sizeof(sql)) != 0)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Failed toc extract SQL from %s", filename);
        status = -1;
        goto Clean_Up;
    }

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Cleaning SQL from %s", filename);

    /* Clean SQL extracted from XML */
    sanitize_sql(sql);
    execute_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.SQL = sql;
    cfg.log_execute_stats = 1;

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Executing SQL from %s", filename);
    /*status = execute_query(ctx, &cfg);*/
    status = execute_query_batch(ctx, &cfg);

    if (status != 0){
        status = -1;
        goto Clean_Up;
    }

    /* Build output filename */

    char timestamp[64];
    char output_file[512];

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Building Timestamp for %s ", filename);
   build_timestamp(timestamp, sizeof(timestamp));

    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    snprintf(output_file,
             sizeof(output_file),
             "%s/%s_completed_%s.xml",
             config->xml_output_dir,
             base,
             timestamp);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                  "Opening  %s for write", output_file);
   FILE *out = fopen(output_file, "w");

    if (!out){
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                      "Failed Opening  %s for write", output_file);
        status = -1;
         goto Clean_Up;
    }

    if (cfg.xml && cfg.xml->OUTPUT_XML)
        fprintf(out, "%s", cfg.xml->OUTPUT_XML);

    fclose(out);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Output written to %s", output_file);
Clean_Up:
    return status;
}

int run_all_xml_tests(oci_context_t *ctx,
                      app_config_t *config)
{
    DIR *d;
    struct dirent *entry;
    int status = 0;

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "Opening directory %s to get listing.",
                 config->xml_input_dir);
   d = opendir(config->xml_input_dir);

    if (!d)
    {
        logger_write(ctx->logger, LOG_ERROR, __func__, 0,
                     "Cannot open input directory %s",
                     config->xml_input_dir);
        status = -1;
        goto Clean_Up;
    }

    int failures = 0;
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                  "Reading input directory %s",
                  config->xml_input_dir);

    while ((entry = readdir(d)) != NULL)
    {
        if (!strstr(entry->d_name, ".xml"))
            continue;

        char path[512];

        snprintf(path, sizeof(path),
                 "%s/%s",
                 config->xml_input_dir,
                 entry->d_name);

        logger_write(ctx->logger, LOG_INFO, __func__, 0,
                       "Calling execute_xml_test");
        status = execute_xml_test(ctx, config, path);
        if (status != 0)
            failures++;
    }
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                   "Releasing d");
    closedir(d);
    status = failures;

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
                 "XML tests complete. Failures=%d",
                 status);
Clean_Up:
    return status;
}
