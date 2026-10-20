/*
 * mssql_metadata.h   (driver_mssql Stage 3, 2026-10-19, point T7)
 *
 * driver_mssql's describe_table (db_driver.h has the contract): one
 * table's columns, in column order, from INFORMATION_SCHEMA.COLUMNS
 * joined to sys.columns, with defaults from sys.default_constraints.
 *
 *   data_type       upper-case base type: NVARCHAR, DATETIME2, DECIMAL ...
 *                   "(MAX)" added for varchar/nvarchar/varbinary(max).
 *                   rowversion (sys.types calls it "timestamp") is
 *                   reported as ROWVERSION, so it is never mistaken for
 *                   a date class.
 *   data_length     declared length n (characters for char types, bytes
 *                   for binary); 0 for (MAX) and the old text/ntext/image
 *                   types - no length check; otherwise the storage size
 *                   in bytes (sys.columns.max_length)
 *   data_precision  numeric precision, -1 when not applicable
 *   data_scale      numeric scale; fractional-second digits for
 *                   datetime2 / datetimeoffset / time; -1 otherwise
 *   nullable        "Y" / "N"
 *   data_default    the default's definition, e.g. "(sysdatetime())".
 *                   Columns SQL Server fills itself report a default too,
 *                   as Oracle's identity columns do, so Level 2 does not
 *                   demand a value for them: "IDENTITY(seed,increment)",
 *                   "AS <expression>" (computed), "(rowversion)".
 *
 * owner is the schema. When req->owner is empty it is filled in: the
 * caller's default schema (SCHEMA_NAME()) if the table is there,
 * otherwise the first schema, by name, holding a table of that name.
 * table_name and owner are upper-cased in place.
 *
 * Logs to ctx->Metadata_logger. Returns 0, or -1 on any error -
 * including a table that does not exist or shows no columns.
 */

#ifndef MSSQL_METADATA_H
#define MSSQL_METADATA_H

#include "db_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

int mssql_describe_table(oci_context_t      *ctx,
                         metadata_request_t *req,
                         col_metadata_t     *cols,
                         int                *col_count,
                         int                 max_cols);

#ifdef __cplusplus
}
#endif

#endif /* MSSQL_METADATA_H */
