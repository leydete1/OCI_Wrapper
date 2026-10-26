/*
 * mssql_dml.h   (driver_mssql Stage 4, 2026-10-20, points U1-U5)
 *
 * driver_mssql's write side (db_driver.h has each hook's contract):
 *
 *   table_key         U1 - the single column that locates a row after
 *                     INSERT/UPDATE: the primary key, else a unique index
 *                     (no filter) on one NOT NULL column. Read from
 *                     sys.indexes once per table and kept for the life
 *                     of the process (DDL is out of scope, M8). A table
 *                     with no such key reports key_count 0: its INSERT
 *                     and UPDATE work, but it cannot take a LOB write
 *                     (5c, 5d).
 *   dml_execute       U3 - prepare, bind every value as text, execute,
 *                     then every result read: rows_affected is the LAST
 *                     row count - the statement's own, which comes after
 *                     any a trigger sends (C5). SQL_NO_DATA from execute
 *                     is a first count of 0, not the end. The error text
 *                     (M7) also goes to out_error_message.
 *   dml_execute_returning_rowids
 *                     U4 - as dml_execute, then reads the keys, one per
 *                     affected row, into the flat 24-byte-per-entry
 *                     buffer core reads (ROWID_BUF_SIZE). Since C5 the
 *                     dialect's clause is OUTPUT inserted.<key> INTO
 *                     @dm_k (k), and a statement carrying it runs as
 *                     DECLARE @dm_k TABLE (k nvarchar(4000)); <sql>;
 *                     SELECT k FROM @dm_k; - the keys are the rows of the
 *                     LAST result set (OUTPUT without INTO fails on a
 *                     table with a trigger, error 334). A key whose text
 *                     needs more than 23 characters fails the statement.
 *                     With no OUTPUT clause (no key) the entries are ""
 *                     and their number is the LAST row count.
 *                     row_count > 1 (a multi-row INSERT): the prepared
 *                     statement runs once per row, in row order, in the
 *                     caller's transaction.
 *   lob_write_by_rowid
 *                     U5 - UPDATE <table> SET <col> = ? WHERE <key> = ?,
 *                     the value sent in chunk_read_size pieces with
 *                     SQL_DATA_AT_EXEC / SQLPutData: a BLOB streamed
 *                     from its file, a text value from inline_text (cut
 *                     on UTF-8 character boundaries). On SQL Server core
 *                     only sends BLOB-kind columns here - text LOBs are
 *                     bound in the statement itself (U6). The row
 *                     count checked is the LAST one (C5).
 *
 * Z7 (2026-10-25): each write first switches the connection to
 * manual-commit, if it is not already; commit() / rollback() switch it
 * back to autocommit (MSSQL_Pool.h). Reads never open a transaction.
 *
 * Never includes an Oracle header.
 */

#ifndef MSSQL_DML_H
#define MSSQL_DML_H

#include "db_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MSSQL_ROWID_BUF_SIZE 24    /* = core's ROWID_BUF_SIZE */

int  mssql_table_key(oci_context_t *ctx, const char *owner, const char *table_name,
                     char *key_column, size_t key_column_size, int *key_count);

int  mssql_dml_execute(oci_context_t *ctx, logger_t *logger,
                       const db_dml_request_t *req, int *out_rows_affected,
                       char *out_error_message, size_t out_error_message_size);

int  mssql_dml_execute_returning_rowids(oci_context_t *ctx, logger_t *logger,
                                        const db_dml_returning_request_t *req,
                                        db_rowid_result_t *out_result);

void mssql_rowid_result_free(db_rowid_result_t *result);

int  mssql_lob_write_by_rowid(oci_context_t *ctx, logger_t *logger,
                              const db_lob_write_request_t *req,
                              uint64_t *out_bytes_written);

/* Forget every cached key (harnesses, after creating test tables). */
void mssql_table_key_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* MSSQL_DML_H */
