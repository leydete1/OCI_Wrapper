
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include "XML_Helper.h"
#include "OCI_Connection.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include "metrics.h"

#define CHECK_OCI(errhp, status) \
    if ((status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO) { \
        text errbuf[512]; sb4 errcode = 0; \
        OCIErrorGet(errhp, 1, NULL, &errcode, errbuf, sizeof(errbuf), OCI_HTYPE_ERROR); \
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Error : (status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO"); \
        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "OCI Error %d: %s\n", errcode, errbuf); \
     }



int get_row_count(oci_context_t *ctx,
                  execute_config_t *cfg,
                  unsigned long long *row_count)
{
	sword status;
	int rc=0;
	char entry_msg[512];
    snprintf(entry_msg, sizeof(entry_msg), "Entering function sql=%s", cfg->SQL );
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, entry_msg);


    OCIStmt *stmt = NULL;
    OCIDefine *def = NULL;

    char count_sql[4096];
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "SELECT COUNT(*) FROM (%s)", cfg->SQL);


   logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIStmtPrepare2");
   CHECK_OCI(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp,
                        &stmt,
                        ctx->errhp,
                        (text *)count_sql,
                        strlen(count_sql),
                        NULL, 0,
                        OCI_NTV_SYNTAX,
                        OCI_DEFAULT));

   logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIStmtExecute");
   CHECK_OCI(ctx->errhp,
        OCIStmtExecute(ctx->svchp,
                       stmt,
                       ctx->errhp,
                       0,
                       0,
                       NULL,
                       NULL,
                       OCI_DEFAULT));

   logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIDefineByPos");
   CHECK_OCI(ctx->errhp,
        OCIDefineByPos(stmt,
                       &def,
                       ctx->errhp,
                       1,
                       row_count,
                       sizeof(unsigned long long),
                       SQLT_UIN,
                       NULL,
                       NULL,
                       NULL,
                       OCI_DEFAULT));

   logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIStmtFetch2");
   status = OCIStmtFetch2(stmt,
                                 ctx->errhp,
                                 1,
                                 OCI_FETCH_NEXT,
                                 0,
                                 OCI_DEFAULT);
    if (status != OCI_SUCCESS){
         rc=-1;
         logger_write(ctx->select_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
         CHECK_OCI(ctx->errhp, status);
         goto Cleanup;
     }

   logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIStmtRelease");
   status=OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
   if (status != OCI_SUCCESS){
          rc=-1;
          logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Calling CHECK_OCI");
          CHECK_OCI(ctx->errhp, status);
          goto Cleanup;
      }


    logger_write(ctx->select_logger,
    			LOG_INFO,
                 __func__,
                 0,
                 "get_row_count completed, rows=%llu",*row_count);

    Cleanup:
    	return rc;
}





int execute_query(oci_context_t *ctx, execute_config_t *cfg)
{
	int rc=0;
    char entry_msg[512];
 	char      *clob_buf = NULL;
	unsigned char *blob_buf = NULL;
	lob_item_t *BLOB_list = NULL;

	/*Mar-19 Perform pre-check new code*/
			int record_count = 0;
			OCIStmt *stmt_count = NULL;
			OCIDefine *defn_count = NULL;

			char query_count[4096];

			/* Build count query */
			snprintf(query_count, sizeof(query_count),
					 "SELECT COUNT(*) FROM (%s)", cfg->SQL);

			logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
						 "Validate query : Expected record Count query: %s", query_count);

			logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
						 "Validate query : Calling OCIStmtPrepare2");
			/* Prepare */
			CHECK_OCI(ctx->errhp,
				OCIStmtPrepare2(ctx->svchp,
								&stmt_count,
								ctx->errhp,
								(text *)query_count,
								(ub4)strlen(query_count),
								NULL,
								0,
								OCI_NTV_SYNTAX,
								OCI_DEFAULT));

			/* Define output */
			logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
						 "Validate query : Calling OCIDefineByPos");
			CHECK_OCI(ctx->errhp,
				OCIDefineByPos(stmt_count,
							   &defn_count,
							   ctx->errhp,
							   1,
							   &record_count,
							   sizeof(record_count),
							   SQLT_INT,
							   NULL,
							   NULL,
							   NULL,
							   OCI_DEFAULT));

			/* Execute */
			logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
						 "Validate query : Calling OCIStmtExecute");
			CHECK_OCI(ctx->errhp,
				OCIStmtExecute(ctx->svchp,
							   stmt_count,
							   ctx->errhp,
							   1,   /* only 1 row */
							   0,
							   NULL,
							   NULL,
							   OCI_DEFAULT));

			logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
						 "Validate query : Query expected Resulting record count = %d", record_count);

			/*This one check prevents massive queries running.   Stop right here*/
			logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
						 "Validate query : Checking %d > %d ", record_count , ctx->ini->query_max_record_count);

			if (record_count > ctx->ini->query_max_record_count|| record_count < 1)
			{
			    logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
			        "Validate query : Query aborted: record_count=%d exceeds max=%d",
			        record_count,
			        ctx->ini->query_max_record_count);

			    goto Cleanup;
			}

			//*Allocating lobs*/
			logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
						 "Validate query : Calculating max_lobs = record_count (%d) * ctx->ini->max_BLOBS_per_record (%d) = %d ", record_count , ctx->ini->max_BLOBS_per_record ,( record_count * ctx->ini->max_BLOBS_per_record));
			int max_lobs = record_count * ctx->ini->max_BLOBS_per_record;
			int BLOB_index =0;

			BLOB_list = calloc(max_lobs, sizeof(lob_item_t));

			if (!BLOB_list)
			{
			    logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
			                 "Validate query : Failed to allocate lob_list");
			    goto Cleanup;
			}else{

			    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
			                 "Validate query : %d lob_item_t items allocated.  Allocation byte size=%d" , max_lobs,(max_lobs*sizeof(lob_item_t)));
			}



	/*End of new code Mar 19*/

    /*Step 1 Allocate locators*/
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Allocating lob locators");
    OCILobLocator *blob_loc = NULL;
    OCILobLocator *clob_loc = NULL;
 	ub4       lob_len;


    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Allocating locator blob_loc");
    OCIDescriptorAlloc(ctx->envhp, (void**)&blob_loc, OCI_DTYPE_LOB, 0, NULL);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Allocating locator clob_loc");
    OCIDescriptorAlloc(ctx->envhp, (void**)&clob_loc, OCI_DTYPE_LOB, 0, NULL);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Finished Allocating locators");
    /*End Step 1 Allocate locators*/



    snprintf(entry_msg, sizeof(entry_msg), "Entering function sql=%s", cfg->SQL );
	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, entry_msg);

    if (!ctx || !cfg || !cfg->SQL){
         rc=-1;
         logger_write(ctx->select_logger, LOG_ERROR, __func__, 0, "!ctx || !cfg || !cfg->SQL");
         goto Cleanup;
     }

    /*This trim is required to remove white space from sql*/
	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Cleaning SQL %s",cfg->SQL);
	trim_sql_inplace(cfg->SQL,ctx);
	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Cleaned SQL %s",cfg->SQL);


	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling clock_gettime");
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    OCIStmt *stmt = NULL;

	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "preparing statement: %s", cfg->SQL);
    CHECK_OCI(ctx->errhp,
        OCIStmtPrepare2(ctx->svchp, &stmt, ctx->errhp,
                        (text *)cfg->SQL, strlen(cfg->SQL),
                        NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT));

  logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIStmtExecute");
   CHECK_OCI(ctx->errhp,
        OCIStmtExecute(ctx->svchp, stmt, ctx->errhp,
                       0, 0, NULL, NULL, OCI_DEFAULT));

    ub4 col_count = 0;
	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIAttrGet");
  CHECK_OCI(ctx->errhp,
        OCIAttrGet(stmt, OCI_HTYPE_STMT,
                   &col_count, 0,
                   OCI_ATTR_PARAM_COUNT,
                   ctx->errhp));



	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "column count = %u", col_count);

    if (col_count == 0)
    {
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0, "No columns returned");
        logger_write(ctx->select_logger, LOG_WARN, __func__, 0, "Calling OCIStmtRelease");
        OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
        rc=-1;
        goto Cleanup;
    }


    /* ---- Allocate arrays on heap (SAFE) ---- */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Allocating Memory");

    OCIDefine **def        = calloc(col_count, sizeof(OCIDefine *));
    char       **buffers   = calloc(col_count, sizeof(char *));
    sb2        *indicators = calloc(col_count, sizeof(sb2));
    ub2        *data_types = calloc(col_count, sizeof(ub2));
    ub4        *data_sizes = calloc(col_count, sizeof(ub4));
    ub1        *precision  = calloc(col_count, sizeof(ub1));
    sb1        *scale      = calloc(col_count, sizeof(sb1));
    OCILobLocator **col_blob_locs = calloc(col_count, sizeof(OCILobLocator *));


    /*Lets check everything is allocated ok*/
    if (!data_sizes || !precision || !scale  )
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Memory allocation failed (metadata)");
        rc=-1;
        goto Cleanup;
    }


    char      (*col_names)[256] = calloc(col_count, sizeof(*col_names));

    if (!def || !buffers || !indicators || !data_types || !col_names)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0, "Memory allocation failed");
        rc=-1;
        goto Cleanup;
    }

    /* ---- XML builder ---- */
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling Building XML");
    xml_builder_t *xml = xml_create(16384);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_start_document");
    xml_start_document(xml);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_start_execution");
    xml_start_execution(xml);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_append");
    xml_append(xml, "<sql_query>%s</sql_query>\n", cfg->SQL);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_end_execution");
    xml_end_execution(xml);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_start_resultset");
   xml_start_resultset(xml);

    /* ---- Column metadata ---- */

    OCIParam *param = NULL;

    for (ub4 i = 1; i <= col_count; i++)
    {
       logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Processing column %d", i );
       logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIParamGet");
       CHECK_OCI(ctx->errhp,
            OCIParamGet(stmt, OCI_HTYPE_STMT,
                        ctx->errhp,
                        (void **)&param,
                        i));

        text *tmp_name = NULL;
        ub4 tmp_len = 0;

        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIAttrGet OCI_ATTR_NAME");
        CHECK_OCI(ctx->errhp,
            OCIAttrGet(param,
                       OCI_DTYPE_PARAM,
                       &tmp_name,
                       &tmp_len,
                       OCI_ATTR_NAME,
                       ctx->errhp));

        if (tmp_len >= 255)
            tmp_len = 255;

        memcpy(col_names[i-1], tmp_name, tmp_len);
        col_names[i-1][tmp_len] = '\0';

        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIAttrGet OCI_ATTR_DATA_TYP");
        CHECK_OCI(ctx->errhp,
            OCIAttrGet(param,
                       OCI_DTYPE_PARAM,
                       &data_types[i-1],
                       0,
                       OCI_ATTR_DATA_TYPE,
                       ctx->errhp));

        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIAttrGet OCI_ATTR_DATA_SIZE");
      CHECK_OCI(ctx->errhp,
            OCIAttrGet(param,
                       OCI_DTYPE_PARAM,
                       &data_sizes[i-1],
                       0,
                       OCI_ATTR_DATA_SIZE,
                       ctx->errhp));

      logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIAttrGet OCI_ATTR_PRECISION");
        CHECK_OCI(ctx->errhp,
            OCIAttrGet(param,
                       OCI_DTYPE_PARAM,
                       &precision[i-1],
                       0,
                       OCI_ATTR_PRECISION,
                       ctx->errhp));

        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIAttrGet OCI_ATTR_SCALE");
       CHECK_OCI(ctx->errhp,
            OCIAttrGet(param,
                       OCI_DTYPE_PARAM,
                       &scale[i-1],
                       0,
                       OCI_ATTR_SCALE,
                       ctx->errhp));


       ub4 buf_size = data_sizes[i-1] + 32;


       if (buf_size < 64)
           buf_size = 64;




       logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
           "Allocating buffer for column %s size=%u",
           col_names[i-1],
           buf_size);
       buffers[i-1] = calloc(1, buf_size);

       if (data_types[i-1] == SQLT_BLOB)
       {
           logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
               "Defining BLOB column %s", col_names[i-1]);

           /*New code Mar-21-2006 to allow for multiple blobs*/


           // BEFORE fetch loop
           logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                 "Defining BLOB column %s", col_names[i-1]);

             // Allocate ONE locator per column
           logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                  "Calling OCIDescriptorAlloc for column %s", col_names[i-1]);
             CHECK_OCI(ctx->errhp,
                 OCIDescriptorAlloc(ctx->envhp,
                                    (void**)&col_blob_locs[i-1],
                                    OCI_DTYPE_LOB, 0, NULL));

             // Define using THAT locator
             logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                   "Calling OCIDefineByPos for column %s", col_names[i-1]);
             CHECK_OCI(ctx->errhp,
                  OCIDefineByPos(stmt,
                                &def[i-1],
                                ctx->errhp,
                                i,
                                (dvoid *)&col_blob_locs[i-1],
                                -1,
                                SQLT_BLOB,
                                &indicators[i-1],
                                NULL,
                                NULL,
                                OCI_DEFAULT));
         /*End New code Mar-21-2006 to allow for multiple blobs*/


       }
       else if (data_types[i-1] == SQLT_CLOB)
       {
           logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
               "Defining CLOB column %s", col_names[i-1]);

          CHECK_OCI(ctx->errhp,
               OCIDefineByPos(stmt,
                              &def[i-1],
                              ctx->errhp,
                              i,
                              (dvoid *)&clob_loc,
                              -1,
                              SQLT_CLOB,
                              &indicators[i-1],
                              NULL,
                              NULL,
                              OCI_DEFAULT));

       }
       else
       {
           logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
               "Defining normal column %s", col_names[i-1]);

           CHECK_OCI(ctx->errhp,
               OCIDefineByPos(stmt,
                              &def[i-1],
                              ctx->errhp,
                              i,
                              buffers[i-1],
                              4000,
                              SQLT_STR,
                              &indicators[i-1],
                              NULL,
                              NULL,
                              OCI_DEFAULT));
       }







        logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
            "Column %d name=%s type=%u size=%u precision=%u scale=%d",
            i,
            col_names[i-1],
            data_types[i-1],
            data_sizes[i-1],
            precision[i-1],
            scale[i-1]);

    }

    /* ---- Fetch loop ---- */

    unsigned int rownum = 0;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Starting fetch loop");

    while (OCIStmtFetch2(stmt, ctx->errhp,
                         1, OCI_FETCH_NEXT, 0,
                         OCI_DEFAULT) == OCI_SUCCESS)
    {

        rownum++;

        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "rownum=%d",rownum);
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_add_row_start");
        xml_add_row_start(xml, rownum);

        for (ub4 i = 0; i < col_count; i++)
        {

            if (data_types[i] == SQLT_CLOB)
            {
                CHECK_OCI(ctx->errhp,
                    OCILobGetLength(ctx->svchp, ctx->errhp, clob_loc, &lob_len));
            }
            else if (data_types[i] == SQLT_BLOB)
            	{
            		logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                                   "Processing BLOB column %s  BLOB_index=%d", col_names[i],BLOB_index);
    				if (BLOB_index >= max_lobs)
					{
						logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
									 "BLOB overflow index=%d max=%d", BLOB_index, max_lobs);
						goto Cleanup;
					}

					lob_item_t *item = &BLOB_list[BLOB_index];


					// Allocate locator for THIS row
	                 logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
	                                   "Calling OCIDescriptorAlloc");
	 					CHECK_OCI(ctx->errhp,
						OCIDescriptorAlloc(ctx->envhp,
										   (void**)&item->lob_loc,
										   OCI_DTYPE_LOB, 0, NULL));

					// COPY from column locator → row locator
		            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
		                                   "Calling OCILobLocatorAssign");
					CHECK_OCI(ctx->errhp,
						OCILobLocatorAssign(ctx->svchp,
											ctx->errhp,
											col_blob_locs[i],   // 👈 key fix
											&item->lob_loc));

					// Get size
		            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
		                                   "Calling OCILobGetLength");
					CHECK_OCI(ctx->errhp,
						OCILobGetLength(ctx->svchp,
										ctx->errhp,
										item->lob_loc,
										&item->blob_size));
		            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
		                                   "BlOB Length item->blob_size=%d",item->blob_size);

					item->is_null = (indicators[i] == -1);

					logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
								 "Row=%d Col=%d BLOB size=%u index=%d",
								 rownum, i, item->blob_size, BLOB_index);

					/*xxxxxxxxxxxxxxxxxx22-Mar New code required*/

					if (!item->is_null && item->blob_size > 0)
					{
					    ub4 total_size = item->blob_size;
					    ub4 offset = 1;  // OCI is 1-based

					    // Allocate full buffer
			            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
			                                   "Allocating total size for item->blob_data=%d",item->blob_size);
					    item->blob_data = malloc(total_size);
					    if (!item->blob_data)
					    {
					        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
					                     "Memory allocation failed for BLOB size=%u", total_size);
					        goto Cleanup;
					    }

					    ub4 bytes_remaining = total_size;
					    ub1 *write_ptr = item->blob_data;

					    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
					                 "Starting chunked BLOB read total_size=%u chunk_size=%u",
					                 total_size, ctx->ini->chunk_read_size);

					    while (bytes_remaining > 0)
					    {
					        ub4 chunk = ctx->ini->chunk_read_size;

					        // Adjust last chunk
					        if (chunk > bytes_remaining)
					            chunk = bytes_remaining;

					        ub4 amount = chunk;

					        logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
					                     "OCILobRead offset=%u chunk=%u remaining=%u",
					                     offset, chunk, bytes_remaining);

					        CHECK_OCI(ctx->errhp,
					            OCILobRead(ctx->svchp,
					                       ctx->errhp,
					                       item->lob_loc,
					                       &amount,
					                       offset,
					                       write_ptr,
					                       chunk,
					                       NULL,   // ctxp
					                       NULL,   // callback
					                       0,
					                       SQLCS_IMPLICIT));

					        // Move forward
					        write_ptr      += amount;
					        offset         += amount;
					        bytes_remaining -= amount;

					        // Safety: OCI can return less than requested
					        if (amount == 0)
					        {
					            logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
					                         "OCILobRead returned 0 bytes unexpectedly");
					            goto Cleanup;
					        }
					    }

					    logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
					                 "Completed BLOB read total=%u", total_size);

					}
					else
					{
					    item->blob_data = NULL;
					}


					/*xxxxxxxxxxxxxxxxxxxxxxEnd 22-Mar New code required*/
					/* ===== ADD: filename + write + XML ===== */

				   logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
					                 "Adding BLOB attributes such as timestamp, file_name,  BLOB size and BLOB index" );
				if (!item->is_null && item->blob_size > 0)
					{
					    const char *search_col = ctx->ini->BLOB_default_file_name_col;

					    switch (BLOB_index)
					    {
					        case 1: search_col = ctx->ini->BLOB_default_file_name_col_1; break;
					        case 2: search_col = ctx->ini->BLOB_default_file_name_col_2; break;
					        case 3: search_col = ctx->ini->BLOB_default_file_name_col_3; break;
					        case 4: search_col = ctx->ini->BLOB_default_file_name_col_4; break;
					        case 5: search_col = ctx->ini->BLOB_default_file_name_col_5; break;
					    }

					    int idx = lookup_blob_index(col_names, col_count, search_col,ctx);

					    if (idx >= 0 && indicators[idx] != -1)
					    {
					        char final_name[512];

					        if (ctx->ini->BLOB_append_file_timestamp == 1)
					        {
								logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
									                 "Calling build_filename_with_timestamp " );
								build_filename_with_timestamp(
										buffers[idx],
										final_name,
										sizeof(final_name),
										BLOB_index,
										ctx
									);
								}
					        else
					        {
					            snprintf(final_name, sizeof(final_name), "%s", buffers[idx]);
					        }
							logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
								                 "final_name=%s",final_name );

					        item->column_name = col_names[i];
					        item->file_name = strdup(final_name);
					        item->mime_type = strdup(get_mime_type(item->file_name));
					        item->column_index = BLOB_index;
					        if(ctx->ini->xml_share_BLOB_URL_path)
					        	item->output_file_url = ctx->ini->BLOB_URL_path;
					        else
					        	item->output_file_url = strdup("N/A");;
					        if(ctx->ini->xml_share_BLOB_host_path)
					        	item->output_file_destination = ctx->ini->BLOB_output_dir;
					        else
					        	item->output_file_destination = strdup("N/A");;

					    }
					    else
					    {
					        item->file_name = strdup("blob_unknown.dat");
					    }

					    /* Write file */
						logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
							                 "Calling write_blob_to_file");
				         write_blob_to_file(item, ctx->ini->BLOB_output_dir,ctx);

					    /* Add XML inline (THIS fixes your placement issue) */
						logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
											 "Calling write_blob_to_file");
						xml_add_blob_field_1(xml, item,ctx);
				}
				BLOB_index++;



            	}


        		const char *value = buffers[i];

				if (indicators[i] == -1)
				{
					value = "";
				}


				const char *type_str = "STRING";

				switch (data_types[i])
				{
				    case SQLT_NUM:
				        type_str = "NUMBER";
				        break;

				    case SQLT_DAT:
				        type_str = "DATE";
				        break;

				    case SQLT_CHR:
				    case SQLT_AFC:
				    case SQLT_STR:
				        type_str = "STRING";
				        break;

				    case SQLT_TIMESTAMP:
					        type_str = "TIMESTAMP";
					        break;
				    case SQLT_BLOB:
					        type_str = "BLOB";
					        break;
				    case SQLT_CLOB:
					        type_str = "CLOB";
					        break;

				    default:
				        type_str = "UNKNOWN";
				}



				logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
				    "Column %s mapped type %s",
				    col_names[i],
				    type_str);

				if (data_types[i] != SQLT_BLOB){
				   logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_add_field %s", col_names[i]);
				   xml_add_field(xml,
								  col_names[i],
								  type_str,
								  value);
				}
        }


        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_add_row_end");
      xml_add_row_end(xml);
    }
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "fetch loop finished, rows=%u", rownum );

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_end_resultset");
    xml_end_resultset(xml);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "calling clock_gettime");
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed =
        (ts_end.tv_sec - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_start_execution");
    xml_start_execution(xml);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_append");
    xml_append(xml, "<num_rows>%u</num_rows>\n", rownum);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_append");
    xml_append(xml, "<execution_time_total>%.6f</execution_time_total>\n", elapsed);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_end_execution");
    xml_end_execution(xml);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_finalize");
    xml_finalize(xml);

    /* ---- Ensure cfg->xml exists ---- */

    if (!cfg->xml)
        cfg->xml = calloc(1, sizeof(*cfg->xml));

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Setting cfg->xml->OUTPUT_XML");
    cfg->xml->OUTPUT_XML = strdup(xml->buffer);

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "exit execute_query");

Cleanup:    /* ---- Cleanup ---- */



    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Starting cleanup");

	/*New code 19-Mar*/
	if (stmt_count)
	{
	    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIStmtRelease for stmt_count");
		OCIStmtRelease(stmt_count,
					   ctx->errhp,
					   NULL,
					   0,
					   OCI_DEFAULT);
	}
	/*End New code 19-Mar*/
	/*New code Mar 29 Free all blobs resources*/
	  /* ---- Free per-row BLOB data ---- */
	    if (BLOB_list)
	    {
	        for (int i = 0; i < BLOB_index; i++)
	        {
	            if (BLOB_list[i].lob_loc)
	            {
	    	    	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIDescriptorFree for BLOB_list[%d].lob_loc",i);
	                OCIDescriptorFree(BLOB_list[i].lob_loc, OCI_DTYPE_LOB);
	                BLOB_list[i].lob_loc = NULL;
	            }

	            if (BLOB_list[i].blob_data)
	            {
	    	    	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(BLOB_list[%d].blob_data",i);
	                free(BLOB_list[i].blob_data);
	                BLOB_list[i].blob_data = NULL;
	            }

	            if (BLOB_list[i].file_name)
	            {
	    	    	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(BLOB_list[%d].file_name",i);
	                free(BLOB_list[i].file_name);
	                BLOB_list[i].file_name = NULL;
	            }
	        }

	    	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling  free(BLOB_list)");
	        free(BLOB_list);
	        BLOB_list = NULL;
	    }

	    for (ub4 i = 0; i < col_count; i++)
	    {
	        if (col_blob_locs && col_blob_locs[i])
	        {
	            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
	                "Calling OCIDescriptorFree(col_blob_locs[%u])", i);

	            OCIDescriptorFree(col_blob_locs[i], OCI_DTYPE_LOB);
	            col_blob_locs[i] = NULL;   // ✅ important
	    	    col_blob_locs = NULL;
	        }
	    }


	    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling  free(col_blob_locs)");
	    free(col_blob_locs);
/*End New code Mar 29 Free all blobs resources*/

	if(buffers){
		logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(buffers array");
		for (ub4 i = 0; i < col_count; i++){
			logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "free(buffers[%d] ", i);
			free(buffers[i]);
			buffers[i]=NULL;
		}
		logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "free(buffers) ");
		free(buffers);
		buffers=NULL;
	}

    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(def)");
   free(def);
   logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(buffers)");
    free(buffers);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(indicators)");
  free(indicators);
  logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(data_types)");
    free(data_types);
    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(col_names)");
   free(col_names);

   logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling xml_free(xml)");
    xml_free(xml);

    if (stmt_count){
    	logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIStmtRelease");
    	OCIStmtRelease(stmt, ctx->errhp, NULL, 0, OCI_DEFAULT);
    }

     logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(data_sizes)");
     free(data_sizes);

      logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(precision)");
      free(precision);

       logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(scale)");
       free(scale);


       logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIDescriptorFree for blob_loc");
       OCIDescriptorFree(blob_loc, OCI_DTYPE_LOB);
       logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling OCIDescriptorFree for clob_loc");
        OCIDescriptorFree(clob_loc, OCI_DTYPE_LOB);
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(blob_buf)");
        free(blob_buf);
        logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Calling free(clob_buf)");
       free(clob_buf);



    logger_write(ctx->select_logger, LOG_INFO, __func__, 0, "Ending cleanup");

    return rc;
}




void trim_sql_inplace(char *str , oci_context_t *ctx )
{

	if (!str) return;

	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "In trim_sql_inplace str=%s" , str);

    unsigned char *s = (unsigned char*)str;
    // Skip leading whitespace + NBSP
    while (*s && ((*s <= 0x20) || (*s == 0xC2 && *(s+1) == 0xA0))) {
        if (*s == 0xC2 && *(s+1) == 0xA0) s++;  // skip extra byte
        s++;
    }

    unsigned char *start = s;
    unsigned char *end = (unsigned char*)str + strlen(str) - 1;

    while (end >= start) {
        if (*end <= 0x20) {
            end--;
        } else if (end > start && *(end-1) == 0xC2 && *end == 0xA0) {
            end -= 2;
        } else if (*end == 0xA0) {  // fallback
            end--;
        } else break;
    }

    size_t len = end - start + 1;
    memmove(str, start, len);
    str[len] = '\0';
	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Leaving trim_sql_inplace str=%s" , str);

}




 /*New helper function 25-Mar*/


int lookup_blob_index(char (*col_names)[256], int col_count, const char *col_name, oci_context_t *ctx)
{
	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "In lookup_blob_index for %s",col_name);

	for (int i = 0; i < col_count; i++)
    {
        if (strcasecmp(col_names[i], col_name) == 0)
        {
          	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Found match at i=%d",i);
            return i;
        }
    }
 	logger_write(ctx->select_logger, LOG_WARN, __func__, 0, "No match found");
   return -1;
}

  /* end New  helper function 25-Mar*/

int write_blob_to_file(lob_item_t *item, const char *output_dir, oci_context_t *ctx)
  {
      if (!item || !item->blob_data || item->blob_size == 0){
    	 logger_write(ctx->select_logger, LOG_WARN, __func__, 0, "Invalid BLOB");
          return -1;
      }


      char path[1024];
      snprintf(path, sizeof(path), "%s/%s", output_dir, item->file_name);

      logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Opening file %s", path);
      FILE *fp = fopen(path, "wb");
      if (!fp){
    	  logger_write(ctx->select_logger, LOG_ERROR, __func__, 0, "Error opening file %s", path);
    	  return -1;
      }

      logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Writing file %s", path);
      fwrite(item->blob_data, 1, item->blob_size, fp);
      logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Closing file %s", path);
      fclose(fp);

      return 0;
  }


void generate_timestamp(char *buffer, size_t size,oci_context_t *ctx)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    strftime(buffer, size, "%Y%m%d_%H%M%S", t);
}


void build_filename_with_timestamp(const char *original,  char *output,  size_t out_size, int idx, oci_context_t *ctx)
{

    char name[256] = {0};
    char ext[64] = {0};
    char timestamp[32];

	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "In build_filename_with_timestamp for idx=%d",idx);
    generate_timestamp(timestamp, sizeof(timestamp),ctx);

    const char *dot = strrchr(original, '.');

    if (dot)
    {
        size_t name_len = dot - original;
        strncpy(name, original, name_len);
        name[name_len] = '\0';

        strncpy(ext, dot, sizeof(ext)-1);
    }
    else
    {
        strncpy(name, original, sizeof(name)-1);
        ext[0] = '\0';
    }

    snprintf(output, out_size, "%s_%s_%d%s", name, timestamp, idx , ext);
	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Leaving build_filename_with_timestamp output=%s",output);
}



int lookup_blob_index_1(const char **col_names, int col_count, const char *col_name, oci_context_t *ctx)
{
	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "In lookup_blob_index_1 for %s",col_name);
    for (int i = 0; i < col_count; i++)
    {
        if (col_names[i] && strcmp(col_names[i], col_name) == 0)
         	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Found match at i=%d",i);
            return i;
    }
    return -1;
}


void xml_add_blob_field_1(xml_builder_t *xml, const lob_item_t *item, oci_context_t *ctx)
{
    // Use the column name or fallback to "UNKNOWN"
    char *e_name = xml_escape(item->column_name ? item->column_name : "UNKNOWN");
    char *e_file = xml_escape(item->file_name ? item->file_name : "N/A");
    char *e_path = xml_escape(item->output_file_destination ? item->output_file_destination : "N/A");

	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "In xml_add_blob_field_1");
    // Only escape and add URL if it's set
    char *e_url = item->output_file_url ? xml_escape(item->output_file_url) : NULL;

    xml_append(xml,
        "<field>"
        "<field_name>%s</field_name>"
        "<field_type>BLOB</field_type>"
        "<field_value/>"
        "<blob>"
        "<file_name>%s</file_name>"
        "<file_path>%s</file_path>",
        e_name, e_file, e_path
    );

    if (e_url) {
        xml_append(xml, "<file_url>%s</file_url>", e_url);
        free(e_url);
    }

    xml_append(xml,
        "<file_size>%llu</file_size>"
        "<mime_type>%s</mime_type>"
        "</blob>"
        "</field>\n",
        (unsigned long long)item->blob_size,
        item->mime_type ? item->mime_type : "application/octet-stream"
    );

    // Free escaped strings
	logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Freeing escaped strings");
    free(e_name);
    free(e_file);
    free(e_path);
}


