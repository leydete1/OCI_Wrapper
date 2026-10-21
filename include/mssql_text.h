/*
 * mssql_text.h   (driver_mssql Stage 5, Fix 1, 2026-10-21)
 *
 * Statement text to SQL Server as UTF-16. The narrow ODBC calls
 * (SQLPrepare / SQLExecDirect) do not carry UTF-8 statement text
 * through: on the first live run a literal N'<a>é</a>' arrived as
 * N'<a>Ã©</a>' (each byte read as one Latin-1 character). Bound values
 * are not affected - only the text of the statement itself. Every
 * statement whose text can hold client data (a SELECT built by core, an
 * INSERT/UPDATE/DELETE, a LOB write naming a table and column) goes
 * through these instead; the driver's own fixed ASCII queries need not.
 *
 * Both return the ODBC call's SQLRETURN, or SQL_ERROR (with no
 * diagnostic record) when the conversion cannot allocate. A byte
 * sequence that is not UTF-8 becomes U+FFFD.
 */

#ifndef MSSQL_TEXT_H
#define MSSQL_TEXT_H

#include <sql.h>
#include <sqlext.h>

#ifdef __cplusplus
extern "C" {
#endif

SQLRETURN mssql_prepare_utf8(SQLHSTMT stmt, const char *sql);
SQLRETURN mssql_exec_direct_utf8(SQLHSTMT stmt, const char *sql);

#ifdef __cplusplus
}
#endif

#endif /* MSSQL_TEXT_H */
