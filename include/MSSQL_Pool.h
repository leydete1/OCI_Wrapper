/*
 * MSSQL_Pool.h
 *
 * SQL Server connections for driver_mssql (driver_mssql Stage 2,
 * points P4/P5/P7, 2026-10-18).
 *
 * The same contract as Connection_Pool.h, over ODBC instead of OCI:
 *
 *   MSSQL_Connect_pool()        open pool_min_size connections
 *   MSSQL_Disconnect_pool()     close them all
 *   MSSQL_Pool_get_session()    lend one (waits up to
 *                               pool_connection_timeout, grows up to
 *                               pool_max_size)
 *   MSSQL_Pool_release_session() give it back (rollback first when
 *                               rollback_on_return_to_pool = 1, then back
 *                               to autocommit - Z7)
 *   MSSQL_Pool_session_is_alive() SQL_ATTR_CONNECTION_DEAD, no round trip
 *   MSSQL_Pool_reconnect_session() replace the caller's connection with a
 *                               NEW one (the old one is closed, not
 *                               lent out again)
 *   MSSQL_Pool_health_check()   SELECT 1 on every free slot; recycle
 *                               expired, reopen dead
 *
 *   MSSQL_Connect_standalone() / MSSQL_Disconnect_standalone()
 *                               one connection on ctx itself
 *                               (use_connection_pool = 0)
 *
 * Handles in oci_context_t (M1): envhp = SQLHENV, svchp = SQLHDBC.
 * errhp, srvhp and authp stay NULL - ODBC reports errors on the handle
 * that failed.
 *
 * Every slot is its own SQLHDBC - its own physical connection (the
 * 2026-08-06 ORA-03114 lesson from Connection_Pool.c).
 *
 * Transactions (Z7, 2026-10-25). With autocommit_mode = 0 every
 * connection sits in AUTOCOMMIT and switches to manual-commit only for a
 * write: mssql_tx_begin_write() before the first write of a transaction,
 * mssql_tx_end_write() after the SQLEndTran that ends it. Reads therefore
 * never hold a transaction open on an idle connection - as on Oracle,
 * where a SELECT starts no transaction. (In manual-commit mode ODBC
 * Driver 18 keeps a transaction open at all times: it begins one when
 * manual-commit is set and again after every commit or rollback - probe
 * P1/P6, 25 Oct.) autocommit_mode = 1: every statement commits on its
 * own, and neither function changes anything.
 *
 * Settings (app_config_t): dbname = database, username, password,
 * mssql_server, mssql_port, mssql_trust_server_certificate,
 * mssql_login_timeout (SQL_ATTR_LOGIN_TIMEOUT),
 * network_read_write_timeout (SQL_ATTR_CONNECTION_TIMEOUT),
 * autocommit_mode, and the pool_* / session_* keys exactly as
 * Connection_Pool.c reads them. use_wallet and nls_* do not apply and
 * are ignored (logged once).
 */

#ifndef MSSQL_POOL_H
#define MSSQL_POOL_H

#include <stddef.h>
#include "Connection.h"

/* ---- Connections ---------------------------------------------------- */
int  MSSQL_Connect_standalone   (oci_context_t *ctx);
void MSSQL_Disconnect_standalone(oci_context_t *ctx);

int  MSSQL_Connect_pool           (oci_context_t *ctx);
void MSSQL_Disconnect_pool        (oci_context_t *ctx);
int  MSSQL_Pool_get_session       (oci_context_t *ctx, oci_context_t *worker_ctx);
void MSSQL_Pool_release_session   (oci_context_t *ctx, oci_context_t *worker_ctx);
int  MSSQL_Pool_session_is_alive  (oci_context_t *ctx);
int  MSSQL_Pool_reconnect_session (oci_context_t *base_ctx, oci_context_t *ctx);
int  MSSQL_Pool_health_check      (oci_context_t *ctx);

/* ---- Helpers shared with driver_mssql.c and the harnesses ----------- */

/*
 * mssql_diag() - M7. Every diagnostic record on (handle_type, handle),
 * as "SQLSTATE 23000 [native 2627]: text", records joined by " | ".
 * The "[Microsoft][ODBC Driver 18 for SQL Server][SQL Server]" prefix
 * is removed. out is always terminated; "" if there is no record.
 * handle_type is an SQLSMALLINT (SQL_HANDLE_ENV/DBC/STMT), passed as
 * int so this header needs no ODBC include.
 */
void mssql_diag(int handle_type, void *handle, char *out, size_t out_size);

/*
 * mssql_connection_string() - the ODBC connection string for ini.
 * With mask_password = 1 the password is written as *** (for logs).
 * Returns 0, or -1 if it does not fit.
 */
int mssql_connection_string(const app_config_t *ini, int mask_password,
                            char *out, size_t out_size);

/*
 * mssql_env_acquire() / mssql_env_release() - P4. One SQLHENV for the
 * whole process (ODBC 3.8, ODBC connection pooling off), shared by every
 * pool and standalone connection; reference counted, freed when the
 * last user releases it. Returns the SQLHENV, or NULL on failure.
 */
void *mssql_env_acquire(logger_t *logger);
void  mssql_env_release(void);

/*
 * mssql_open_connection() / mssql_close_connection() - one connected
 * SQLHDBC with the P4 attributes (autocommit per autocommit_mode, login
 * and connection timeouts). The connection string is logged with the
 * password masked. Returns the SQLHDBC, or NULL (error logged).
 */
void *mssql_open_connection (void *henv, const app_config_t *ini, logger_t *logger);
void  mssql_close_connection(void *hdbc, logger_t *logger);

/*
 * mssql_ping() - runs SELECT 1 on hdbc. 1 = answered, 0 = not.
 */
int mssql_ping(void *hdbc, logger_t *logger);

/*
 * mssql_end_tran() - commit (commit = 1) or rollback (0) on hdbc.
 * 0 = done, -1 = failed (error logged on logger).
 */
int mssql_end_tran(void *hdbc, int commit, logger_t *logger, const char *caller);

/*
 * Z7 (2026-10-25) - manual-commit only for writes; see the header note.
 *
 * mssql_tx_begin_write()
 *   Called by every write hook before it runs anything. Switches hdbc to
 *   manual-commit unless it already is (SQLGetConnectAttr - no round
 *   trip). 0 = ready, -1 = the switch failed (logged at ERROR on logger):
 *   the caller must fail without running the write, so a write never
 *   runs in autocommit by accident. ini may be NULL (= autocommit_mode 0).
 *
 * mssql_tx_end_write()
 *   Called only after an SQLEndTran (commit or rollback) that SUCCEEDED:
 *   switches hdbc back to autocommit unless it already is. Never call it
 *   with a write pending - switching autocommit on commits whatever is
 *   open (probe P7). 0 = done, -1 = the switch failed (logged at WARN):
 *   the connection stays in manual-commit, as before Z7, and the switch
 *   is tried again at the next commit or rollback.
 *
 * mssql_tx_in_manual()
 *   1 if hdbc is in manual-commit, 0 if in autocommit, -1 if it cannot
 *   be read.
 */
int mssql_tx_begin_write(void *hdbc, const app_config_t *ini, logger_t *logger,
                         const char *caller);
int mssql_tx_end_write  (void *hdbc, const app_config_t *ini, logger_t *logger,
                         const char *caller);
int mssql_tx_in_manual  (void *hdbc);

#endif /* MSSQL_POOL_H */
