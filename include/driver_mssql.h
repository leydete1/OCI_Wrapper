/*
 * driver_mssql.h
 *
 * SQL Server implementation of db_driver_t (db_driver.h), over ODBC
 * (ODBC Driver 18 for SQL Server, unixODBC). See
 * driver_mssql_proposal_v1_0_18-oct.docx.
 *
 * mssql_driver_get() returns a pointer to one static, already-populated
 * db_driver_t. db_driver_get() (db_driver.c) returns it when db_type =
 * mssql; everything else goes through the returned function pointers.
 *
 * Every hook and every dialect hook is implemented (Stages 2-6,
 * 18-22 Oct 2026; see driver_mssql.c for which file holds each one).
 *
 * Transactions (Z7, 2026-10-25): connections sit in autocommit; each
 * write transaction runs in manual-commit from its first write until
 * commit() or rollback() - MSSQL_Pool.h.
 */

#ifndef DRIVER_MSSQL_H
#define DRIVER_MSSQL_H

#include "db_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

const db_driver_t *mssql_driver_get(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_MSSQL_H */
