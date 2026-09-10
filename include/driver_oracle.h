/*
 * driver_oracle.h
 *
 * Oracle implementation of db_driver_t (db_driver.h). PoC scope:
 * connect + disconnect only - see db_driver.h's own doc comment for why
 * execute_select is not wired up in this pass.
 *
 * oracle_driver_get() returns a pointer to a single, static, already-
 * populated db_driver_t - there is nothing to construct per-call, since
 * exactly one db_type is ever active per process (db_driver.h, section
 * on db_driver_t). db_driver_get() (db_driver.c) is the only intended
 * caller; everything else should go through the returned db_driver_t's
 * function pointers, not call OCI_Connect()/OCI_Disconnect() directly,
 * once db_driver_get() is wired into config.ini's db_type (order-of-
 * attack step 3, not done yet).
 */

#ifndef DRIVER_ORACLE_H
#define DRIVER_ORACLE_H

#include "db_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

const db_driver_t *oracle_driver_get(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_ORACLE_H */
