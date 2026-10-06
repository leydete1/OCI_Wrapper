/*
 * Table_Metadata_Module.c
 *
 * Stage 4a (Oracle dialect extraction, 2026-10-06): see
 * Table_Metadata_Module.h. Everything Oracle-specific that used to be
 * here moved into driver_oracle.c; what remains is the core entry point
 * that dispatches to the active driver.
 */

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>

#include "Table_Metadata_Module.h"
#include "db_driver.h"
#include "logger.h"

int get_request_metadata(oci_context_t      *ctx,
                         metadata_request_t *req,
                         col_metadata_t     *cols,
                         int                *col_count,
                         int                 max_cols)
{
    if (!ctx)
        return -1;

    const db_driver_t *driver = db_driver_get(ctx);
    if (!driver || !driver->describe_table)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "Active driver has no describe_table");
        return -1;
    }

    return driver->describe_table(ctx, req, cols, col_count, max_cols);
}
