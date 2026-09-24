/*
 * metadata_cache.h
 *
 * Metadata Cache
 * ---------------
 * Thin wrapper around oci_cache providing a dedicated cache instance
 * for Oracle table column metadata (col_metadata_t arrays).
 *
 * Include strategy - how the circular dependency is broken
 * --------------------------------------------------------
 * The problem:
 *   OCI_Connection.h  includes  metadata_cache.h
 *   metadata_cache.h  included  OCI_Table_Metadata_Module.h   <- loop
 *
 * col_metadata_t and metadata_request_t are anonymous typedef structs
 * so they CANNOT be forward-declared with "typedef struct tag name".
 *
 * Solution:
 *   This header declares ONLY the functions that do NOT reference
 *   col_metadata_t or metadata_request_t.  Those are:
 *     - metadata_cache_init()
 *     - metadata_cache_destroy()
 *     - metadata_cache_make_key()
 *     - metadata_cache_lookup()
 *     - metadata_cache_release()
 *     - metadata_cache_invalidate()
 *     - metadata_cache_evict()
 *     - metadata_cache_report()
 *
 *   The functions that DO reference col_metadata_t / metadata_request_t
 *   are declared in a SECOND header:
 *
 *     metadata_cache_meta.h
 *
 *   That second header includes OCI_Table_Metadata_Module.h explicitly
 *   and is included ONLY by .c files that already have the full type
 *   definitions in scope (Insert/Update/Template modules).
 *   It is NEVER included by OCI_Connection.h or OCI_Table_Metadata_Module.h.
 *
 * config.ini keys (already present in app_config_t / ini_reader)
 * ---------------------------------------------------------------
 *   metadata_cache_enabled        = 1
 *   metadata_cache_ttl_seconds    = 3600
 *   metadata_cache_max_entries    = 500
 *   metadata_cache_max_memory_mb  = 64
 *   metadata_cache_bucket_count   = 512
 *   metadata_cache_hash_algorithm = fnv1a | djb2 | murmur3
 */

#ifndef METADATA_CACHE_H
#define METADATA_CACHE_H

#include "oci_cache.h"
#include "ini_reader.h"
#include "logger.h"

/* oci_context_t is defined in OCI_Connection.h which is already in
 * the include chain before this header is reached.  Declare it as an
 * incomplete type so this header compiles stand-alone too.            */
#ifndef OCI_CONNECTION_H
typedef struct oci_context_t oci_context_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Lifecycle                                                           */
/* ================================================================== */

/*
 * metadata_cache_init()
 *
 * Create and return the metadata cache instance.
 * Returns NULL if disabled (metadata_cache_enabled=0) or on error.
 * Store the returned pointer in ctx->metadata_cache.
 */
cache_t *metadata_cache_init(const app_config_t *ini, logger_t *logger);

/*
 * metadata_cache_destroy()
 *
 * Free all entries and the cache handle.
 * Call once at application shutdown.
 */
void metadata_cache_destroy(cache_t *cache);

/* ================================================================== */
/*  Key utilities                                                       */
/* ================================================================== */

/*
 * metadata_cache_make_key()
 *
 * Build the canonical "OWNER.TABLE_NAME" lookup key into dest.
 * Both strings are uppercased and dot-joined.
 * If owner is empty the key is just TABLE_NAME.
 *
 * Returns dest on success, NULL if dest_max is too small.
 */
char *metadata_cache_make_key(const char *owner,
                               const char *table_name,
                               char       *dest,
                               size_t      dest_max);

/* ================================================================== */
/*  Low-level cache operations (type-agnostic)                         */
/* ================================================================== */

/*
 * metadata_cache_lookup()
 *
 * Look up a pre-built key.
 * Returns borrowed cache_entry_t* on HIT (in_use=1).
 * Caller MUST call metadata_cache_release() when done.
 * Returns NULL on MISS.
 */
cache_entry_t *metadata_cache_lookup(cache_t    *cache,
                                      const char *key);

/*
 * metadata_cache_release()
 *
 * Release a borrowed entry. Safe to call with NULL.
 */
void metadata_cache_release(cache_t       *cache,
                             cache_entry_t *entry);

/*
 * metadata_cache_invalidate()
 *
 * Immediately expire the entry for owner.table_name.
 * Returns 0=expired, 1=not found, -1=error.
 */
int metadata_cache_invalidate(cache_t    *cache,
                               const char *owner,
                               const char *table_name);

/*
 * metadata_cache_evict()
 *
 * Sweep expired entries. Call from heartbeat thread.
 * Returns number evicted, -1 on error.
 */
int metadata_cache_evict(cache_t *cache);

/*
 * metadata_cache_report()
 *
 * Write statistics to the logger at LOG_INFO level.
 */
void metadata_cache_report(cache_t *cache);

#ifdef __cplusplus
}
#endif

#endif /* METADATA_CACHE_H */
