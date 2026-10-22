/*
 * mssql_select.h   (driver_mssql Stage 3, 2026-10-19, points T3-T5;
 *                   Stage 5, 2026-10-21, points W1-W6;
 *                   Stage 6, 2026-10-21, points X3-X4)
 *
 * The SELECT cursor behind driver_mssql's select_open /
 * select_fetch_batch / select_close (db_driver.h has the contract).
 *
 *   select_open    one ODBC statement per SELECT on ctx->svchp (the
 *                  session's connection). query_timeout > 0 sets
 *                  SQL_ATTR_QUERY_TIMEOUT. Binds every value as text
 *                  (SQL_C_CHAR -> nvarchar, so nothing depends on the
 *                  database's code page; nvarchar(max) beyond 4000
 *                  UTF-16 units - W6); a NULL entry binds SQL NULL.
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
 * Long columns (Stage 5):
 *   CLOB  varchar(max), nvarchar(max), text, ntext, xml
 *   BLOB  varbinary(max), image
 * A result with one is fetched a row at a time with SQLGetData (the
 * batch is still the requested size), as driver_oracle does for its
 * LOBs: a BLOB is written to BLOB_output_dir and described in the field
 * (file name, path, URL, size, MIME type); a CLOB is written to
 * CLOB_output_dir with its URL in the field, or put in the field itself
 * with text_lobs_inline (a value longer than the field fails the
 * fetch). File names, URLs and ini keys are driver_oracle's (Blob_Utils,
 * Clob_Utils). out_stats counts them (CLOB bytes are UTF-8 bytes). A
 * BLOB file that cannot be written fails the fetch. A CLR type
 * (geometry, geography, hierarchyid) is refused by select_open.
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

/*
 * Stage 6 (2026-10-21, X3/X4) - a stored procedure's result sets, for
 * mssql_proc.c. ODBC gives a procedure's OUT values only after every
 * result set has been read, so each one is read in full while it is
 * current and held.
 *
 * mssql_select_buffer_result()
 *   Reads the current result set of stmt (an executed SQLHSTMT - not
 *   taken over, not advanced past this result) through the same code as
 *   a SELECT: batches of batch rows, long columns to files as Stage 5.
 *   More than max_rows rows (when > 0) fails the call - nothing is cut
 *   short. On return stmt has no bound columns and a row array of 1, so
 *   SQLMoreResults can follow. *out is a cursor ready for
 *   mssql_select_open_buffered(); free it with mssql_select_close() when
 *   it is not opened. what is logged with any failure. 0 or -1 (logged,
 *   mssql_select_last_error()).
 *
 * mssql_select_open_buffered()
 *   select_open_from_cursor() for such a cursor: the column list (caller
 *   frees) and batch size, exactly as select_open() gives them.
 *   mssql_select_fetch_batch() then returns the held batches in order
 *   and mssql_select_close() frees whatever is left. 0 or -1.
 *
 * mssql_select_buffered_rows()
 *   Rows held in such a cursor (for logging); -1 if not one.
 */
int  mssql_select_buffer_result(oci_context_t *ctx, void *stmt, const char *what,
                                int batch, int max_rows, db_select_cursor_t **out);
int  mssql_select_open_buffered(db_select_cursor_t *cursor, db_column_meta_t **out_columns,
                                int *out_column_count, int *out_batch_size);
int  mssql_select_buffered_rows(const db_select_cursor_t *cursor);

#ifdef __cplusplus
}
#endif

#endif /* MSSQL_SELECT_H */
