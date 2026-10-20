/*
 * mssql_select.h   (driver_mssql Stage 3, 2026-10-19, points T3-T5)
 *
 * The scalar SELECT cursor behind driver_mssql's select_open /
 * select_fetch_batch / select_close (db_driver.h has the contract).
 *
 *   select_open    one ODBC statement per SELECT on ctx->svchp (the
 *                  session's connection). query_timeout > 0 sets
 *                  SQL_ATTR_QUERY_TIMEOUT. Binds every value as text
 *                  (SQL_C_CHAR -> nvarchar, so nothing depends on the
 *                  database's code page); a NULL entry binds SQL NULL.
 *                  Prepares, executes, describes the columns and binds
 *                  a column-wise array of req->fetch_array_size rows.
 *   fetch_batch    one SQLFetch per call - up to the batch size rows -
 *                  into a new resultset_t, every value as text in SQL
 *                  Server's own form (M8): datetime2 "2030-01-02
 *                  08:30:00.123456", binary as hex, NULL as "".
 *   close          frees the statement (which discards any rows not
 *                  read) and the buffers.
 *
 * max_rows and max_memory_bytes are not applied here - as with
 * driver_oracle, core bounds the query itself (count_rows_sql /
 * row_limit_sql) before it opens the cursor.
 *
 * Not yet (T4): a result with a long column - varchar(max),
 * nvarchar(max), varbinary(max), text, ntext, image or xml - is refused
 * by select_open with "driver_mssql: long column <name> (<type>) in
 * SELECT not implemented yet (stage 5)". Stage 5 reads those with
 * SQLGetData.
 *
 * D6 (T5): when SQL Server rejects a wrapped query because a column
 * has no name (error 8155) or a duplicate one (8156), the error text
 * gets " - on SQL Server every selected column needs a distinct name -
 * add an alias (AS name)" added.
 *
 * Every failure is logged on ctx->select_logger with the SQL and
 * SQLSTATE text, and also kept for the calling thread - see
 * mssql_select_last_error().
 */

#ifndef MSSQL_SELECT_H
#define MSSQL_SELECT_H

#include "db_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSSQL_D6_HINT "on SQL Server every selected column needs a distinct name - add an alias (AS name)"

int  mssql_select_open(oci_context_t              *ctx,
                       const db_select_request_t  *req,
                       db_select_cursor_t        **out_cursor,
                       db_column_meta_t          **out_columns,
                       int                        *out_column_count,
                       int                        *out_batch_size);

int  mssql_select_fetch_batch(db_select_cursor_t      *cursor,
                              resultset_t            **out_rs,
                              int                     *out_rows_fetched,
                              db_fetch_batch_stats_t  *out_stats);

void mssql_select_close(db_select_cursor_t *cursor);

/*
 * The text of the last select_open / select_fetch_batch failure on the
 * calling thread ("" if none since the thread started), the same text
 * that was logged. For the harnesses, and for Stage 4's
 * out_error_message.
 */
const char *mssql_select_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* MSSQL_SELECT_H */
