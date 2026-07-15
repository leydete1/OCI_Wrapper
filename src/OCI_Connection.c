
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
        logger_write(ctx->connection_logger, LOG_DEBUG, __func__, 0, "Error : (status) != OCI_SUCCESS && (status) != OCI_SUCCESS_WITH_INFO"); \
        logger_write(ctx->connection_logger, LOG_DEBUG, __func__, 0, "OCI Error %d: %s\n", errcode, errbuf); \
     }





int OCI_Connect(oci_context_t *ctx)
{
    struct timespec ts_start, ts_end;
    int rc = 0;
    char entry_msg[512];

    const char *username = ctx->ini->username;
    const char *password = ctx->ini->password;
    const char *dbname   = ctx->ini->dbname;
    int         use_wallet = ctx->ini->use_wallet;

    snprintf(entry_msg, sizeof(entry_msg),
             "Entering function dbname=%s username=%s use_wallet=%d",
             dbname, username, use_wallet);

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, entry_msg);

    /* ----------------------------------------------------------------
     * Oracle Wallet: set TNS_ADMIN to the wallet directory so OCI
     * can locate cwallet.sso and sqlnet.ora at connection time.
     * This must be done before the first OCI call.
     * ---------------------------------------------------------------- */
    if (use_wallet)
    {
        const char *wallet_loc = ctx->ini->wallet_location;
        if (!wallet_loc || wallet_loc[0] == '\0')
        {
            logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0,
                         "use_wallet=1 but wallet_location is empty "
                         "in config.ini");
            return -1;
        }
        logger_write(ctx->connection_logger, LOG_INFO, __func__, 0,
                     "Setting TNS_ADMIN='%s' for wallet authentication",
                     wallet_loc);
        setenv("TNS_ADMIN", wallet_loc, 1);
    }
    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling gettime");
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    logger_write(ctx->connection_logger,
                 LOG_INFO,
                 __func__,
                 0,
                 "Starting OCI connection to %s",
                 dbname);

    sword status;

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIEnvCreate");
    status = OCIEnvCreate(&ctx->envhp, OCI_DEFAULT, NULL, NULL, NULL, NULL, 0, NULL);

    if (status != OCI_SUCCESS)
    {
        rc = -1;
        logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
        CHECK_OCI(ctx->errhp, status);
        goto Cleanup;
    }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleAlloc");
    status = OCIHandleAlloc(ctx->envhp, (void **)&ctx->errhp, OCI_HTYPE_ERROR, 0, NULL);

    if (status != OCI_SUCCESS)
    {
        rc = -1;
        logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
        CHECK_OCI(ctx->errhp, status);
        goto Cleanup;
    }


    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleAlloc");
    status = OCIHandleAlloc(ctx->envhp, (void **)&ctx->srvhp, OCI_HTYPE_SERVER, 0, NULL);
    if (status != OCI_SUCCESS)
    {
        rc = -1;
        logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
        CHECK_OCI(ctx->errhp, status);
        goto Cleanup;
    }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleAlloc");
    status = OCIHandleAlloc(ctx->envhp, (void **)&ctx->svchp, OCI_HTYPE_SVCCTX, 0, NULL);
    if (status != OCI_SUCCESS)
    {
        rc = -1;
        logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
        CHECK_OCI(ctx->errhp, status);
        goto Cleanup;
    }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIServerAttach");
    status = OCIServerAttach(ctx->srvhp,
                             ctx->errhp,
                             (text *)dbname,
                             (sb4)strlen(dbname),
                             OCI_DEFAULT);

    if (status != OCI_SUCCESS)
    {
        rc = -1;
        logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
        CHECK_OCI(ctx->errhp, status);
        goto Cleanup;
    }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIAttrSet");
    status = OCIAttrSet(ctx->svchp,
                        OCI_HTYPE_SVCCTX,
                        ctx->srvhp,
                        0,
                        OCI_ATTR_SERVER,
                        ctx->errhp);

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleAlloc");
    status = OCIHandleAlloc(ctx->envhp, (void **)&ctx->authp, OCI_HTYPE_SESSION, 0, NULL);

    if (status != OCI_SUCCESS)
    {
        rc = -1;
        logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
        CHECK_OCI(ctx->errhp, status);
        goto Cleanup;
    }

    if (use_wallet)
    {
        /* ---- Wallet mode: OCI_CRED_EXT - no username/password set ----
         * OCI reads credentials from cwallet.sso via TNS_ADMIN.
         * No OCIAttrSet for USERNAME or PASSWORD - wallet supplies both.
         * Recommended for production and GxP environments.            */
        logger_write(ctx->connection_logger, LOG_INFO, __func__, 0,
                     "Calling OCISessionBegin (OCI_CRED_EXT - wallet)");
        status = OCISessionBegin(ctx->svchp,
                                 ctx->errhp,
                                 ctx->authp,
                                 OCI_CRED_EXT,
                                 OCI_DEFAULT);
    }
    else
    {
        /* ---- Legacy mode: OCI_CRED_RDBMS - explicit credentials ----
         * Development / fallback only.  Not recommended for production.*/
        logger_write(ctx->connection_logger, LOG_INFO, __func__, 0,
                     "Calling OCIAttrSet OCI_ATTR_USERNAME (legacy mode)");
        status = OCIAttrSet(ctx->authp,
                            OCI_HTYPE_SESSION,
                            (void *)username,
                            (ub4)strlen(username),
                            OCI_ATTR_USERNAME,
                            ctx->errhp);
        if (status != OCI_SUCCESS)
        {
            rc = -1;
            logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0,
                         "Calling CHECK_OCI");
            CHECK_OCI(ctx->errhp, status);
            goto Cleanup;
        }

        logger_write(ctx->connection_logger, LOG_INFO, __func__, 0,
                     "Calling OCIAttrSet OCI_ATTR_PASSWORD (legacy mode)");
        status = OCIAttrSet(ctx->authp,
                            OCI_HTYPE_SESSION,
                            (void *)password,
                            (ub4)strlen(password),
                            OCI_ATTR_PASSWORD,
                            ctx->errhp);
        if (status != OCI_SUCCESS)
        {
            rc = -1;
            logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0,
                         "Calling CHECK_OCI");
            CHECK_OCI(ctx->errhp, status);
            goto Cleanup;
        }

        logger_write(ctx->connection_logger, LOG_INFO, __func__, 0,
                     "Calling OCISessionBegin (OCI_CRED_RDBMS - legacy)");
        status = OCISessionBegin(ctx->svchp,
                                 ctx->errhp,
                                 ctx->authp,
                                 OCI_CRED_RDBMS,
                                 OCI_DEFAULT);
    }

    if (status != OCI_SUCCESS)
    {
        rc = -1;
        logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
        CHECK_OCI(ctx->errhp, status);
        goto Cleanup;
    }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling CHECK_OCI");
    CHECK_OCI(ctx->errhp, status);

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIAttrSet");
   status = OCIAttrSet(ctx->svchp,
                        OCI_HTYPE_SVCCTX,
                        ctx->authp,
                        0,
                        OCI_ATTR_SESSION,
                        ctx->errhp);

    if (status != OCI_SUCCESS)
    {
        rc = -1;
        logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
        CHECK_OCI(ctx->errhp, status);
        goto Cleanup;
    }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling clock_gettime");
    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    double elapsed =
        (ts_end.tv_sec - ts_start.tv_sec) +
        (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

    logger_write(ctx->connection_logger,
    			LOG_INFO,
                 __func__,
                 0,
                 "OCI connection established (%.6f sec)",
                 elapsed);

Cleanup:
    return rc;
}







void OCI_Disconnect(oci_context_t *ctx)
{
	sword status;
	char entry_msg[512];
    snprintf(entry_msg, sizeof(entry_msg), "Entering function");
    logger_write(ctx->connection_logger, LOG_DEBUG, __func__, 0, entry_msg);


    logger_write(ctx->connection_logger,
    			LOG_INFO,
                 __func__,
                 0,
                 "Starting cleanup");

    logger_write(ctx->connection_logger, LOG_DEBUG, __func__, 0, "Calling OCISessionEnd");
    if (ctx->svchp && ctx->authp){
        status=OCISessionEnd(ctx->svchp, ctx->errhp, ctx->authp, OCI_DEFAULT);
        if (status != OCI_SUCCESS){
             logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
             CHECK_OCI(ctx->errhp, status);
             goto Finish_Cleanup;
         }
   }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIServerDetach");
    if (ctx->srvhp){
    	status=OCIServerDetach(ctx->srvhp, ctx->errhp, OCI_DEFAULT);
        if (status != OCI_SUCCESS){
               logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
               CHECK_OCI(ctx->errhp, status);
               goto Finish_Cleanup;
           }
    }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleFree");
    if (ctx->authp){
    	status=OCIHandleFree(ctx->authp, OCI_HTYPE_SESSION);
        if (status != OCI_SUCCESS){
               logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
               CHECK_OCI(ctx->errhp, status);
               goto Finish_Cleanup;
           }
    }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleFree");
    if (ctx->svchp){
    	status=OCIHandleFree(ctx->svchp, OCI_HTYPE_SVCCTX);
        if (status != OCI_SUCCESS){
               logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
               CHECK_OCI(ctx->errhp, status);
               goto Finish_Cleanup;
           }
   }

    logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleFree");
   if (ctx->srvhp){
        status=OCIHandleFree(ctx->srvhp, OCI_HTYPE_SERVER);
        if (status != OCI_SUCCESS){
               logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
               CHECK_OCI(ctx->errhp, status);
               goto Finish_Cleanup;
           }
}


   logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleFree");
   if (ctx->errhp){
        status=OCIHandleFree(ctx->errhp, OCI_HTYPE_ERROR);
        if (status != OCI_SUCCESS){
                logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
                CHECK_OCI(ctx->errhp, status);
                goto Finish_Cleanup;
            }
   }

   logger_write(ctx->connection_logger, LOG_INFO, __func__, 0, "Calling OCIHandleFree");
   if (ctx->envhp){
        status=OCIHandleFree(ctx->envhp, OCI_HTYPE_ENV);
        if (status != OCI_SUCCESS){
                logger_write(ctx->connection_logger, LOG_ERROR, __func__, 0, "Calling CHECK_OCI");
                CHECK_OCI(ctx->errhp, status);
                goto Finish_Cleanup;
            }
   }

   Finish_Cleanup:

    logger_write(ctx->connection_logger,
    			LOG_INFO,
                 __func__,
                 0,
                 "Finished cleanup");
}

