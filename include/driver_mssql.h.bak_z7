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
 * Stage 2 (2026-10-18): connect / disconnect / pool / session_is_alive /
 * reconnect_session / health_check / commit / rollback are real. Every
 * other hook - and every dialect hook - is present but fails with
 * "driver_mssql: <hook> not implemented yet (stage N)" until that stage
 * fills it in.
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
