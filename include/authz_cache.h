/*
 * authz_cache.h
 *
 * Permission Cache - Public Interface (Security Module Stage 5)
 * ---------------------------------------------------------------
 * See authz_cache.c for the full design description. Declared in its
 * own header (unlike session_cache.c, whose prototypes live in
 * OCI_Session_Manager.h) because this cache has two separate
 * consumers with no single natural "owner" module:
 *   - OCI_Auth_Manager.c writes to it (authz_cache_store()), right
 *     after a successful session_create().
 *   - OCI_Authz_Manager.c reads from it (authz_cache_lookup()), on
 *     every OP_CHECK_PERMISSION request.
 */

#ifndef AUTHZ_CACHE_H
#define AUTHZ_CACHE_H

#include "oci_cache.h"    /* cache_t, cache_entry_t */
#include "ini_reader.h"   /* app_config_t */
#include "logger.h"       /* logger_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * authz_cache_init()
 *
 * Reads authz_cache_* keys from config.ini (ini_reader.h) and calls
 * cache_create(). Returns NULL if authz_cache_enabled=0 or on failure
 * - callers must handle NULL the same way session_cache's own callers
 * do (a disabled/failed cache means every authz_has_permission() call
 * denies, exactly like an expired/missing session would).
 */
cache_t *authz_cache_init(const app_config_t *ini, logger_t *logger);

/*
 * authz_cache_lookup() / authz_cache_release()
 *
 * Delegates to cache_lookup()/cache_release() (oci_cache.h). Caller
 * must authz_cache_release() any non-NULL entry returned, same
 * borrow/release contract as every other cache in this project.
 */
cache_entry_t *authz_cache_lookup(cache_t *cache, const char *session_id);
void           authz_cache_release(cache_t *cache, cache_entry_t *entry);

/*
 * authz_cache_store()
 *
 * Stores permission_codes_csv (a comma-separated list of PERMISSION_
 * CODE values, e.g. "CUSTOMER.READ,CUSTOMER.WRITE,ORDER.READ" - pass
 * NULL or "" for a user with no granted permissions, a valid state,
 * not an error) keyed by session_id. TTL defaults to
 * authz_cache_ttl_seconds from config.ini.
 *
 * Returns 0 on success, -1 on failure (logged internally).
 */
int authz_cache_store(cache_t *cache, const char *session_id,
                       const char *permission_codes_csv);

/*
 * authz_cache_invalidate() / evict() / report() / destroy()
 *
 * Delegates to cache_expire_entry() / cache_evict_expired() /
 * cache_dump_report() / cache_destroy() respectively.
 */
int  authz_cache_invalidate(cache_t *cache, const char *session_id);
int  authz_cache_evict(cache_t *cache);
void authz_cache_report(cache_t *cache);
void authz_cache_destroy(cache_t *cache);

/*
 * authz_cache_encode() / authz_cache_decode()
 *
 * Pipe-delimited, versioned encoding - see authz_cache.c's own doc
 * comment on the format. encode() returns a heap-allocated string
 * (caller frees) or NULL on failure. decode() writes the comma-
 * separated permission list into out_csv (empty string if the user
 * has no permissions) and returns 0, or -1 if encoded is malformed/
 * a stale encoding version.
 */
char *authz_cache_encode(const char *permission_codes_csv);
int   authz_cache_decode(const char *encoded, char *out_csv, size_t out_csv_size);

#ifdef __cplusplus
}
#endif

#endif /* AUTHZ_CACHE_H */
