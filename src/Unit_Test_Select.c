#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <logger.h>
#include <ini_reader.h>
#include <OCI_Connection.h>
#include <OCI_Execute_Query_Module.h>
#include <Oci_xml_tester.h>
#include <Unit_Test.h>
#include <oci.h>
#include <string.h>   // strlen, etc.
#include <strings.h>   // strlen, etc.
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <ctx_helper.h>





#define CHECK_OCI(errhp, status) \
    if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
        text errbuf[512]; \
        sb4 errcode = 0; \
        OCIErrorGet(errhp, 1, NULL, &errcode, errbuf, sizeof(errbuf), OCI_HTYPE_ERROR); \
        fprintf(stderr, "OCI Error %d: %s\n", errcode, errbuf); \
        exit(EXIT_FAILURE); \
    }




/* Helper macro to cast text* to const char* for strlen */
#define STRLEN(x) ((ub4)strlen((const char*)(x)))




int main_bak(int argc, char *argv[])
{
    int status = 0;

    if (argc < 2) {
        printf("Usage: %s <config_file>\n", argv[0]);
        return -1;
    }

    const char *config_file = argv[1];
    app_config_t config;
    oci_context_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    memset(&config, 0, sizeof(config));

     ctx.ini = &config;   // <-- Point ctx.ini to the single config struct

    status = load_ini(config_file, &config, &ctx);
    if (status != 0) {
        printf("Failed to load ini file\n");
        return -1;
    }


	/*ctx.ini->log_level_num = parse_log_level(ctx.ini->log_level);*/

	// Now access log file via ctx.ini
     printf("About to open log file %s \n", ctx.ini->log_file_name);

     printf("log_level pointer = %p\n", (void*)ctx.ini->log_level);
     printf("log_level string = %s\n", ctx.ini->log_level);  // should print INFO, DEBUG, etc.

     printf("ctx.ini->log_level= %s \n", ctx.ini->log_level);


    logger_t logger;
    if (logger_init(&logger,
                    ctx.ini->log_file_name,
                    ctx.ini->log_file_max_size,
                    ctx.ini->log_file_rotation_number,
                    ctx.ini->log_level_num) != 0)
    {
        printf("Failed to initialize logger\n");
        return -1;
    }
    printf("log_level string from INI = '%s'\n", ctx.ini->log_level);
    ctx.logger = &logger;
    ctx.NLS_DATE_FORMAT = "YYYY-MM-DD HH24:MI:SS";

    /*Dump log ctx contents loaded from ini*/
    logger_dump_ctx(&ctx);


    if (OCI_Connect(&ctx) != 0)
    {
        logger_write(&logger, LOG_ERROR, __func__, 0,
                     "Failed to connect to database");
        return -1;
    }

    logger_write(&logger, LOG_INFO, __func__, 0,
                 "Starting test run_all_xml_tests");

    run_all_xml_tests(&ctx, ctx.ini);

    logger_write(&logger, LOG_INFO , __func__, 0,
                 "Finished tests");

    return 0;
}


// Example function
int parse_log_level(const char *level) {
    if (!level) return LOG_DEBUG;  // default
    if (strcasecmp(level, "DEBUG") == 0) return LOG_DEBUG;
    if (strcasecmp(level, "INFO")  == 0) return LOG_INFO;
    if (strcasecmp(level, "WARN")  == 0) return LOG_WARN;
    if (strcasecmp(level, "ERROR") == 0) return LOG_ERROR;
    return LOG_DEBUG; // fallback
}








