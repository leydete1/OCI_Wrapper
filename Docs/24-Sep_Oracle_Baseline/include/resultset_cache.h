/*
 * resultset_cache.h
 *
 * Result Set Cache
 * -----------------
 * Thin wrapper around oci_cache that provides a dedicated cache
 * instance for SELECT query XML output documents.
 *
 * The cache key is the normalised SQL string (uppercased, whitespace
 * collapsed).  The cached value is the complete OUTPUT_XML document
 * that execute_query_batch would have produced.
 *
 * Integration point
 * -----------------
 * In execute_query_batch (or its caller):
 *
 *   1. Normalise the SQL key via resultset_cache_make_key()
 *   2. Call resultset_cache_lookup() - on HIT return cached XML,
 *      skip all OCI work, call resultset_cache_release()
 *   3. On MISS run execute_query_batch as normal
 *   4. On success call resultset_cache_store() with the OUTPUT_XML
 *
 * config.ini keys
 * ---------------
 *   resultset_cache_enabled        = 1
 *   resultset_cache_ttl_seconds    = 300
 *   resultset_cache_max_entries    = 1000
 *   resultset_cache_max_memory_mb  = 256
 *   resultset_cache_bucket_count   = 2048
 *   resultset_cache_hash_algorithm = fnv1a
 */

#ifndef RESULTSET_CACHE_H
#define RESULTSET_CACHE_H

#include "oci_cache.h"
#include "ini_reader.h"
#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * resultset_cache_init()
 *
 * Create and return the resultset cache instance, reading all
 * configuration from ini.  Returns NULL if disabled or on error.
 */
cache_t *resultset_cache_init(const app_config_t *ini, logger_t *logger);

/*
 * resultset_cache_make_key()
 *
 * Normalise sql into dest for use as a cache lookup key.
 * Wrapper around cache_normalize_sql().
 * Returns dest on success, NULL on error.
 */
char *resultset_cache_make_key(const char *sql,
                                char       *dest,
                                size_t      dest_max);

/*
 * resultset_cache_lookup()
 *
 * Look up a normalised SQL key.
 * Returns a borrowed cache_entry_t* on HIT (caller must call
 * resultset_cache_release() when done), NULL on MISS.
 */
cache_entry_t *resultset_cache_lookup(cache_t    *cache,
                                       const char *normalised_key);

/*
 * resultset_cache_release()
 *
 * Release a borrowed entry back to the cache.
 * Must be called after every successful resultset_cache_lookup().
 */
void resultset_cache_release(cache_t       *cache,
                              cache_entry_t *entry);

/*
 * resultset_cache_store()
 *
 * Store a query result in the cache.
 * output_xml is strdup'd internally - the caller retains its copy.
 *
 * Parameters
 *   cache          - cache instance
 *   normalised_key - key from resultset_cache_make_key()
 *   output_xml     - OUTPUT_XML string to cache (will be strdup'd)
 *   output_json    - optional JSON rendering of the same resultset
 *                     (will be strdup'd); pass NULL if not available.
 *                     Stored alongside output_xml on the same entry so
 *                     a later hit can serve either format without
 *                     re-rendering.
 *   row_count      - number of rows in this resultset, stored on the
 *                     entry so a later cache hit can report an accurate
 *                     row count instead of 0
 *   opts           - optional per-entry options (may be NULL). If
 *                     provided, row_count and output_json above take
 *                     precedence over any matching fields set on opts.
 *
 * Returns  0  stored successfully
 *         -1  error or cache disabled
 */
int resultset_cache_store(cache_t            *cache,
                           const char         *normalised_key,
                           const char         *output_xml,
                           const char         *output_json,
                           uint64_t            row_count,
                           cache_entry_opts_t *opts);

/*
 * resultset_cache_invalidate()
 *
 * Immediately expire the entry for the given SQL string.
 * sql is normalised internally before lookup.
 */
int resultset_cache_invalidate(cache_t    *cache,
                                const char *sql);

/*
 * resultset_cache_invalidate_by_table()
 *
 * Closure item 5 follow-up (2026-08-12) - expires every cached SELECT
 * result whose own table dependency list (set at store time via
 * cache_entry_opts_t's table_dependency_tag, populated from
 * extract_sql_dependencies()'s own output - see execute_query_batch)
 * includes table_name. Thin wrapper around cache_invalidate_by_tag()
 * (oci_cache.h) - see that function's own doc comment for the full
 * design, including why substring matching is the deliberately
 * simplest correct choice here rather than row-level tracking.
 *
 * Intended caller: execute_insert_batch()/execute_update_batch()/
 * execute_delete_batch(), after a successful write, with the table
 * they just modified - so any resultset previously cached from a
 * SELECT touching that same table is never served stale after this.
 *
 * Returns the number of entries invalidated (>= 0), or -1 on error.
 */
int resultset_cache_invalidate_by_table(cache_t *cache, const char *table_name);

/*
 * resultset_cache_evict()
 *
 * Sweep expired entries.  Call periodically from a heartbeat thread.
 * Returns number of entries evicted.
 */
int resultset_cache_evict(cache_t *cache);

/*
 * resultset_cache_report()
 *
 * Write cache statistics to the logger.
 */
void resultset_cache_report(cache_t *cache);

/*
 * resultset_cache_destroy()
 *
 * Free all entries and the cache handle.
 * Call once at application shutdown.
 */
void resultset_cache_destroy(cache_t *cache);

#ifdef __cplusplus
}
#endif

#endif /* RESULTSET_CACHE_H */
