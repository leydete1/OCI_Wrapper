/*
 * db_driver.c
 *
 * db_driver_get() - see db_driver.h for the contract.
 *
 * Not a real dispatch yet: db_type does not exist in config.ini /
 * app_config_t yet (order-of-attack step 3, not done). Oracle is also
 * the only driver that exists (order-of-attack step 2, this pass).
 * Both of those are true simultaneously right now, so this always
 * returns oracle_driver_get() regardless of ctx. ctx is accepted (not
 * ignored at the signature level) purely so every call site is already
 * written the way it will need to look once this becomes a real
 * ctx->ini->db_type dispatch - no caller will need to change again when
 * that lands.
 */

#include "db_driver.h"
#include "driver_oracle.h"

const db_driver_t *db_driver_get(const oci_context_t *ctx)
{
    (void)ctx;   /* unused until db_type dispatch exists - see above */

    return oracle_driver_get();
}
