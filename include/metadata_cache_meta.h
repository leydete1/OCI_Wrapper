/*
 * metadata_cache_meta.h
 *
 * Metadata Cache - col_metadata_t API
 * -------------------------------------
 * Declares the functions that accept col_metadata_t and
 * metadata_request_t parameters.  These types are anonymous typedef
 * structs defined in OCI_Table_Metadata_Module.h and cannot be
 * forward-declared, so this header must be included AFTER both:
 *
 *   #include "OCI_Connection.h"
 *   #include "OCI_Table_Metadata_Module.h"
 *   #include "metadata_cache.h"
 *   #include "metadata_cache_meta.h"   <-- last, after all of the above
 *
 * This header is included ONLY by the .c modules that call these
 * functions (Insert Execute, Insert Template, Update Execute).
 *
 * It is NEVER included by:
 *   - OCI_Connection.h
 *   - OCI_Table_Metadata_Module.h
 *   - metadata_cache.h
 *
 * That exclusion is what breaks the circular dependency.
 */

#ifndef METADATA_CACHE_META_H
#define METADATA_CACHE_META_H

/*
 * These includes must already have been done by the including .c file
 * before this header is reached.  They are NOT repeated here to avoid
 * re-introducing the circular dependency.
 *
 * Required before including this header:
 *   #include "oci_cache.h"
 *   #include "logger.h"
 *   #include "OCI_Connection.h"
 *   #include "OCI_Table_Metadata_Module.h"
 *   #include "metadata_cache.h"
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Cache result struct - populated by metadata_cache_get_or_fetch     */
/* ================================================================== */

/*
 * metadata_cache_result_t
 *
 * Filled by metadata_cache_get_or_fetch() so callers can wire accurate
 * cache metrics without touching the cache a second time.
 *
 *   was_cache_hit    1 = served from cache,  0 = fetched from OCI
 *   cache_lookup_us  microseconds spent inside the cache lookup
 *   cache_key_hash   FNV1a hash of the OWNER.TABLE_NAME key
 *   cache_key        the canonical key string used for the lookup
 */
typedef struct {
    int      was_cache_hit;
    uint64_t cache_lookup_us;
    uint64_t cache_key_hash;
    char     cache_key[260];
} metadata_cache_result_t;

/* ================================================================== */
/*  Primary API - used by INSERT / UPDATE / DELETE callers             */
/* ================================================================== */

/*
 * metadata_cache_get_or_fetch()
 *
 * Drop-in replacement for get_request_metadata() in DML modules.
 *
 * Behaviour
 * ---------
 *   HIT  -> deserialise cols[] from cache entry, return 0.
 *            OCI connection is NOT touched.
 *   MISS -> call get_request_metadata(), store result in cache,
 *            return 0.
 *   NULL cache -> falls straight through to get_request_metadata()
 *                 with no overhead.
 *
 * Parameters
 *   cache     - from metadata_cache_init() (may be NULL if disabled)
 *   ctx       - OCI context (connection + logger)
 *   req       - table / owner descriptor
 *   cols      - caller-allocated array (at least max_cols entries)
 *   col_count - set to the number of columns on success
 *   max_cols  - size of cols[] (use MAX_TABLE_COLUMNS)
 *   result    - optional output: hit/miss flag, timing, hash, key.
 *               Pass NULL if caller does not need metrics detail.
 *
 * Returns  0  success
 *         -1  error (logged to ctx->Metadata_logger)
 */
int metadata_cache_get_or_fetch(cache_t                  *cache,
                                 oci_context_t            *ctx,
                                 metadata_request_t       *req,
                                 col_metadata_t           *cols,
                                 int                      *col_count,
                                 int                       max_cols,
                                 metadata_cache_result_t  *result);

/* ================================================================== */
/*  Serialisation helpers (available if callers need direct control)   */
/* ================================================================== */

/*
 * metadata_cache_store()
 *
 * Serialise cols[0..col_count-1] and insert into the cache under key.
 * The payload is heap-allocated internally; caller's array is unchanged.
 *
 * Returns  0  stored successfully
 *         -1  error or cache disabled
 */
int metadata_cache_store(cache_t              *cache,
                          logger_t             *logger,
                          const char           *key,
                          const col_metadata_t *cols,
                          int                   col_count);

/*
 * metadata_cache_deserialise()
 *
 * Decode a cache entry payload into a caller-supplied cols[] array.
 * entry must be a valid in_use pointer from metadata_cache_lookup().
 *
 * Returns  0  success
 *         -1  payload corrupt or max_cols too small
 */
int metadata_cache_deserialise(const cache_entry_t *entry,
                                col_metadata_t      *cols,
                                int                 *col_count,
                                int                  max_cols);

#ifdef __cplusplus
}
#endif

#endif /* METADATA_CACHE_META_H */
