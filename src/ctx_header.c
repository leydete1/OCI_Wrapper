#include "ctx_helper.h"


void logger_dump_ctx(oci_context_t *ctx)
{
    if (!ctx || !ctx->ini || !ctx->logger)
        return;

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "------ OCI CONTEXT DUMP START ------");

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "Oracle handles:");

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  envhp  = %p", ctx->envhp);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  errhp  = %p", ctx->errhp);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  srvhp  = %p", ctx->srvhp);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  svchp  = %p", ctx->svchp);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  authp  = %p", ctx->authp);


    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "Configuration values:");

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  username                 = %s", ctx->ini->username);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  dbname                   = %s", ctx->ini->dbname);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  log_file_name            = %s", ctx->ini->log_file_name);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  log_file_max_size        = %d", ctx->ini->log_file_max_size);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  log_file_rotation_number = %d", ctx->ini->log_file_rotation_number);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  log_level                = %s", ctx->ini->log_level);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  log_level_num            = %d", ctx->ini->log_level_num);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  TEST_SQL_FILE_NAME       = %s", ctx->ini->TEST_SQL_FILE_NAME);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  xml_input_dir            = %s", ctx->ini->xml_input_dir);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  xml_output_dir           = %s", ctx->ini->xml_output_dir);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  xml_error_dir            = %s", ctx->ini->xml_error_dir);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  BLOB_host_path           = %s", ctx->ini->BLOB_host_path);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  CLOB_URL_path            = %s", ctx->ini->CLOB_URL_path);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  BLOB_output_dir          = %s", ctx->ini->BLOB_output_dir);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  CLOB_output_dir          = %s", ctx->ini->CLOB_output_dir);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  xml_share_BLOB_output_dir = %d", ctx->ini->xml_share_BLOB_output_dir);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  xml_share_CLOB_output_dir = %d", ctx->ini->xml_share_CLOB_output_dir);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  xml_share_BLOB_host_path  = %d", ctx->ini->xml_share_BLOB_host_path);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "  xml_share_CLOB_URL_path   = %d", ctx->ini->xml_share_CLOB_URL_path);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
         "  max_BLOBS_per_record     = %d", ctx->ini->max_BLOBS_per_record);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
         "  max_CLOBS_per_record     = %d", ctx->ini->max_CLOBS_per_record);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
          "  chunk_read_size     = %d", ctx->ini->chunk_read_size);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
           "  query_max_record_count     = %d", ctx->ini->query_max_record_count);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
           "  BLOB_default_file_name_col    = %s", ctx->ini->BLOB_default_file_name_col);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_file_name_col_1    = %s", ctx->ini->BLOB_default_file_name_col_1);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_file_name_col_2    = %s", ctx->ini->BLOB_default_file_name_col_2);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_file_name_col_3    = %s", ctx->ini->BLOB_default_file_name_col_3);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_file_name_col_4    = %s", ctx->ini->BLOB_default_file_name_col_5);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_file_name_col_5    = %s", ctx->ini->BLOB_default_file_name_col_5);


    logger_write(ctx->logger, LOG_INFO, __func__, 0,
           "  BLOB_default_MIME_col    = %s", ctx->ini->BLOB_default_MIME_col);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_MIME_TYPE_col_1    = %s", ctx->ini->BLOB_default_MIME_TYPE_col_1);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_MIME_TYPE_col_2   = %s", ctx->ini->BLOB_default_MIME_TYPE_col_2);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_MIME_TYPE_col_3    = %s", ctx->ini->BLOB_default_MIME_TYPE_col_3);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_MIME_TYPE_col_4   = %s", ctx->ini->BLOB_default_MIME_TYPE_col_4);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
            "  BLOB_default_MIME_TYPE_col_5   = %s", ctx->ini->BLOB_default_MIME_TYPE_col_5);


    logger_write(ctx->logger, LOG_INFO, __func__, 0,
             "  BLOB_default_MIME_TYPE   = %s", ctx->ini->BLOB_default_MIME_TYPE);

    logger_write(ctx->logger, LOG_INFO, __func__, 0,
             "  BLOB_default_file_name   = %s", ctx->ini->BLOB_default_file_name);


    logger_write(ctx->logger, LOG_INFO, __func__, 0,
         "  BLOB_append_file_timestamp   = %d", ctx->ini->BLOB_append_file_timestamp);

    /*
BBLOB_append_file_timestamp=1

     */
    logger_write(ctx->logger, LOG_INFO, __func__, 0,
        "------ OCI CONTEXT DUMP END ------");
}
