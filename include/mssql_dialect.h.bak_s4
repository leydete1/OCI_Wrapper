/*
 * mssql_dialect.h   (driver_mssql Stage 3, 2026-10-19, points T1/T2)
 *
 * SQL Server's db_dialect_t - the SQL fragments core asks the driver
 * for (db_driver.h, DIALECT). Every form is the one in section M5 of
 * driver_mssql_proposal_v1_0_18-oct.docx:
 *
 *   bind_placeholder     ?
 *   value_expr           CAST(? AS datetime2(7))  datetime2 / datetime /
 *                                                  smalldatetime
 *                        CAST(? AS date)           date
 *                        CAST(? AS datetimeoffset(7))
 *                        CAST(? AS time(7))
 *                        operand unchanged         anything else
 *   lob_placeholder      0x   varbinary(max) / image
 *                        ''   varchar(max) / text
 *                        N''  nvarchar(max) / ntext
 *                        NULL anything else (xml binds as text)
 *   row_locator_clauses  stage 4 (needs the key lookup) - returns -1
 *   row_limit_sql        SELECT TOP (n) * FROM (<sql>) AS dm_t
 *                        or, with a top-level ORDER BY (5a),
 *                        <sql> OFFSET 0 ROWS FETCH NEXT n ROWS ONLY
 *   count_rows_sql       SELECT COUNT(*) FROM (SELECT 1 AS dm_c FROM
 *                        (<sql>) AS dm_a) AS dm_b
 *                        with " OFFSET 0 ROWS" after <sql> when it has a
 *                        top-level ORDER BY (5a)
 *   now_expr             SYSDATETIME()
 *   add_seconds_expr     DATEADD(second, CAST(<secs> AS int), <base>)
 *                        (DATEADD refuses a bigint number - error 8116)
 *   procedure_call_sql   {CALL proc(?, ?)}   /   {CALL proc}
 *
 * Never includes an Oracle header (m2_compile_proof.py, Part 1).
 */

#ifndef MSSQL_DIALECT_H
#define MSSQL_DIALECT_H

#include "db_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The dialect object itself (driver_mssql.c points db_driver_t.dialect
 * at it) and a getter for code that prefers one. */
extern const db_dialect_t mssql_dialect;
const db_dialect_t *mssql_dialect_get(void);

/*
 * mssql_sql_shape() - the 5a scanner, exposed for
 * MSSQL_Driver_Dialect_Test.c.
 *
 * Looks at the TOP LEVEL of sql only: text inside brackets, 'strings'
 * (including N'...'), [identifiers], "identifiers", -- comments and
 * (nested) block comments is skipped. Each output may be NULL.
 *
 *   order_by       1 if a top-level ORDER BY is present
 *   offset         1 if the ORDER BY is followed by OFFSET ... ROW(S)
 *                  (OFFSET/FETCH) - a column called OFFSET is not one
 *   starts_top     1 if the statement is SELECT [ALL|DISTINCT] TOP ...
 *   ends_in_line_comment
 *                  1 if sql ends inside a -- comment, so text appended
 *                  after it must start on a new line
 *
 * Returns 0, or -1 if sql is NULL.
 */
int mssql_sql_shape(const char *sql, int *order_by, int *offset,
                    int *starts_top, int *ends_in_line_comment);

#ifdef __cplusplus
}
#endif

#endif /* MSSQL_DIALECT_H */
