/*
 * oci_cache.h
 *
 * General Purpose Cache Module
 * -----------------------------
 * A thread-safe, configurable hash-table cache suitable for storing
 * any string-keyed, string-valued payload.  Designed to back three
 * independent cache instances in the Data_Manager project:
 *
 *   resultset_cache  - caches XML output documents from SELECT queries
 *   metadata_cache   - caches Oracle column metadata per table
 *   statement_cache  - caches prepared OCI statement handles
 *
 * Each instance is created independently via cache_create() with its
 * own cache_config_t.  They share no global state.
 *
 * Design
 * ------
 *   Hash table with separate chaining (linked list per bucket).
 *   Collision chains are walked under a read or write lock depending
 *   on the operation.  pthread_rwlock_t allows multiple concurrent
 *   readers (cache_lookup) with exclusive access for writers
 *   (cache_insert, cache_expire_entry, cache_evict_expired).
 *
 * Entry lifecycle
 * ---------------
 *   cache_insert()        - hash key -> bucket -> prepend to chain
 *   cache_lookup()        - hash key -> bucket -> walk chain ->
 *                           set in_use=1, update last_access_ts,
 *                           increment hit_count
 *   cache_release()       - clear in_use flag after caller is done
 *   cache_expire_entry()  - mark one entry expired by key
 *   cache_evict_expired() - sweep all buckets, free expired/unpinned
 *
 * Eviction policy
 * ---------------
 *   Primary:   TTL - entry is expired when time(NULL) > expiry_ts
 *   Secondary: LRU - when memory exceeds max_memory_bytes the least
 *              recently used non-pinned, non-in-use entry is evicted
 *   Guard:     pinned=1 entries are never evicted automatically
 *   Guard:     in_use=1 entries are skipped during eviction sweep
 *              (caller must call cache_release() to allow eviction)
 *
 * Hash algorithms (configurable per instance)
 * -------------------------------------------
 *   CACHE_HASH_FNV1A   - FNV-1a 64-bit  (default, fast, good distribution)
 *   CACHE_HASH_DJB2    - DJB2 64-bit    (simple, well-known)
 *   CACHE_HASH_MURMUR3 - MurmurHash3    (strongest, slightly heavier)
 *
 * Thread safety
 * -------------
 *   All public functions are thread-safe via pthread_rwlock_t.
 *   cache_lookup() holds the read lock only for the chain walk and
 *   flag update; the caller reads the returned pointer under in_use=1
 *   protection and must call cache_release() when done.
 *   cache_create() and cache_destroy() are NOT thread-safe and must
 *   be called from a single thread during init / shutdown.
 *
 * Memory ownership
 * ----------------
 *   The cache owns all heap memory for entries and their string fields
 *   after cache_insert() returns.  The caller must not free any pointer
 *   passed to cache_insert().  The cache frees all entry memory on
 *   eviction or cache_destroy().
 *
 * config.ini keys (read by resultset_cache / metadata_cache wrappers)
 * --------------------------------------------------------------------
 *   <prefix>_enabled          = 1
 *   <prefix>_ttl_seconds      = 300
 *   <prefix>_max_entries      = 1000
 *   <prefix>_max_memory_mb    = 256
 *   <prefix>_bucket_count     = 2048
 *   <prefix>_hash_algorithm   = fnv1a | djb2 | murmur3
 */

#ifndef OCI_CACHE_H
#define OCI_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Hash algorithm identifiers                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    CACHE_HASH_FNV1A   = 0,   /* FNV-1a 64-bit  (default)            */
    CACHE_HASH_DJB2    = 1,   /* DJB2   64-bit                       */
    CACHE_HASH_MURMUR3 = 2    /* MurmurHash3 128-bit, lower 64 used  */
} cache_hash_algorithm_t;

/* ------------------------------------------------------------------ */
/*  Eviction policy identifiers                                         */
/* ------------------------------------------------------------------ */
typedef enum {
    CACHE_EVICT_TTL_LRU = 0,  /* TTL primary, LRU secondary (default) */
    CACHE_EVICT_LFU     = 1   /* Least Frequently Used                */
} cache_eviction_policy_t;

/* ------------------------------------------------------------------ */
/*  Per-entry options passed to cache_insert()                          */
/* ------------------------------------------------------------------ */
typedef struct {
    int     ttl_seconds;      /* 0 = use cache default TTL            */
    uint8_t pinned;           /* 1 = never auto-evict this entry      */

    /* Row count for this cached payload, e.g. resultset row count.
     * 0 = not applicable (e.g. metadata_cache / session_cache entries
     * that aren't caching a query resultset).                        */
    uint64_t row_count;

    /* Optional secondary rendering of the same payload, e.g. a JSON
     * rendering alongside output_doc's XML, so a cache entry can serve
     * either format without re-rendering. NULL = not applicable.
     * Used only by resultset_cache; metadata_cache / session_cache
     * leave this NULL. Not owned - copied (strdup'd) internally.     */
    const char *output_document_json;
    size_t      output_length_json;

    /* Transaction tracing - all optional, may be NULL               */
    const char *client_ip;
    const char *client_host;
    const char *client_id;
    const char *server_host;
    const char *server_name;
    const char *process_id;
    const char *tx_trace;

    /* Table-level invalidation (closure item 5 follow-up, 2026-08-12) -
     * comma-separated list of every table this cached entry's own
     * query depends on (resultset_cache only - metadata_cache/
     * session_cache leave this NULL, matching output_document_json's
     * own convention above). Populated from the same table dependency
     * list extract_sql_dependencies() already produces for every
     * SELECT (sql_dependency_extractor.h) - no new parsing needed, just
     * threading what's already computed through to the cache entry
     * itself. Used by cache_invalidate_by_tag() to find every cached
     * entry that needs expiring when a write modifies a given table.
     * NULL = no table dependency tracked for this entry (never
     * invalidated by table, only by TTL or explicit key). Not owned -
     * copied (strdup'd) internally, matching output_document_json.   */
    const char *table_dependency_tag;
} cache_entry_opts_t;

/* ------------------------------------------------------------------ */
/*  Cache entry                                                         */
/*  The cache owns all heap fields after insert.                       */
/* ------------------------------------------------------------------ */
typedef struct cache_entry_t {

    /* ---- Lookup ---- */
    uint64_t    hash_key;           /* computed from normalized_sql   */
    char       *normalized_sql;     /* canonical key string           */

    /* ---- Transaction tracing (all heap-allocated, may be NULL) --- */
    char       *client_ip;
    char       *client_host;
    char       *client_id;
    char       *server_host;
    char       *server_name;
    char       *process_id;
    char       *tx_trace;

    /* ---- Cached payload ---- */
    char       *output_document;    /* heap-allocated XML / JSON / etc */
    size_t      output_length;      /* byte length of output_document  */
    uint64_t    row_count;          /* rows in the cached resultset,   *
                                      * 0 for non-resultset caches      */

    /* ---- Optional secondary rendering (resultset_cache only) ---- */
    char       *output_document_json; /* e.g. JSON alongside XML above,
                                        * NULL if not stored            */
    size_t      output_length_json;

    /* ---- Table-level invalidation (resultset_cache only) ----
     * See cache_entry_opts_t's own doc comment on the field this is
     * copied from at store time (closure item 5 follow-up, 2026-08-12).
     * NULL for metadata_cache/session_cache entries.                  */
    char       *table_dependency_tag;

    /* ---- Timestamps ---- */
    time_t      created_ts;
    time_t      last_access_ts;
    time_t      expiry_ts;          /* 0 = never expires               */

    /* ---- Per-entry statistics ---- */
    uint64_t    hit_count;

    /* ---- State flags ---- */
    uint8_t     in_use;             /* 1 = borrowed by a worker        */
    uint8_t     pinned;             /* 1 = immune to auto-eviction     */
    uint8_t     dirty;              /* 1 = needs writeback (future)    */

    /* ---- Memory accounting ---- */
    size_t      entry_memory_bytes; /* total bytes charged to this entry */

    /* ---- Collision chain ---- */
    struct cache_entry_t *next;

} cache_entry_t;

/* ------------------------------------------------------------------ */
/*  Per-entry execution statistics                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t    execution_count;
    uint64_t    success_count;
    uint64_t    failure_count;
    uint64_t    rows_returned;
    uint64_t    cache_hits;
    uint64_t    cache_misses;

    double      total_execution_ms;
    double      avg_execution_ms;
    double      total_fetch_ms;
    double      avg_fetch_ms;
    double      min_execution_ms;
    double      max_execution_ms;

    time_t      last_execution_ts;

    double      memory_pressure_pct;  /* % of max_memory_bytes used    */
    uint64_t    eviction_rate;         /* evictions since last report   */
    double      compression_ratio;     /* reserved for future use       */
    uint64_t    avg_lookup_us;         /* average lookup microseconds   */
    uint64_t    peak_entry_size;       /* largest single entry seen     */
} query_execution_stats_t;

/* ------------------------------------------------------------------ */
/*  Cache configuration - one per cache instance                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char                    cache_name    [64];  /* for logging        */
    size_t                  bucket_count;        /* hash table width   */
    size_t                  max_entries;         /* hard entry cap     */
    size_t                  max_memory_bytes;    /* memory limit       */
    int                     ttl_seconds;         /* default entry TTL  */
    cache_hash_algorithm_t  hash_algorithm;      /* FNV1A / DJB2 / M3 */
    cache_eviction_policy_t eviction_policy;     /* TTL_LRU / LFU     */
    int                     enabled;             /* 0 = bypass cache   */
} cache_config_t;

/* ------------------------------------------------------------------ */
/*  Cache handle                                                        */
/* ------------------------------------------------------------------ */
typedef struct {

    /* ---- Hash table ---- */
    cache_entry_t         **buckets;       /* [bucket_count]           */
    size_t                  bucket_count;
    size_t                  entry_count;

    /* ---- Global statistics ---- */
    uint64_t                cache_hits;
    uint64_t                cache_misses;
    uint64_t                cache_evictions;
    uint64_t                cache_inserts;
    uint64_t                collision_count;

    /* ---- Limits (from cache_config_t) ---- */
    size_t                  max_entries;
    size_t                  max_memory_bytes;
    int                     ttl_seconds;
    int                     enabled;

    /* ---- Memory usage ---- */
    size_t                  current_memory_bytes;

    /* ---- Configuration snapshot ---- */
    cache_config_t          config;

    /* ---- Execution statistics ---- */
    query_execution_stats_t stats;

    /* ---- Thread safety ---- */
    pthread_rwlock_t        lock;

    /* ---- Logger ---- */
    logger_t               *logger;

} cache_t;

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

/*
 * cache_create()
 *
 * Allocate and initialise a new cache instance.
 * Must be called once from a single thread before any other operation.
 *
 * Parameters
 *   cfg    - configuration (copied internally; caller may free after call)
 *   logger - shared logger (not owned by the cache)
 *
 * Returns  heap-allocated cache_t*  on success
 *          NULL on failure (logged)
 */
cache_t *cache_create(const cache_config_t *cfg, logger_t *logger);

/*
 * cache_destroy()
 *
 * Free all entries and the cache handle itself.
 * Must be called from a single thread during shutdown.
 * All in_use and pinned entries are freed unconditionally.
 */
void cache_destroy(cache_t *cache);

/*
 * cache_insert()
 *
 * Insert a new entry.  If an entry with the same key already exists
 * it is expired and replaced.
 *
 * Parameters
 *   cache      - cache instance
 *   key        - lookup key (SQL string, table name, etc.)
 *   output_doc - heap-allocated payload (cache takes ownership)
 *   doc_len    - byte length of output_doc
 *   opts       - per-entry options (may be NULL for defaults)
 *
 * Returns  0  success
 *         -1  error (cache full, memory limit reached, or disabled)
 */
int cache_insert(cache_t            *cache,
                 const char         *key,
                 char               *output_doc,
                 size_t              doc_len,
                 cache_entry_opts_t *opts);

/*
 * cache_lookup()
 *
 * Look up an entry by key.
 * On hit: sets in_use=1, updates last_access_ts and hit_count.
 * Caller MUST call cache_release() when done with the entry.
 * Expired entries are not returned (treated as misses).
 *
 * Returns  pointer to cache_entry_t on hit (in_use=1)
 *          NULL on miss or expired
 */
cache_entry_t *cache_lookup(cache_t *cache, const char *key);

/*
 * cache_release()
 *
 * Clear the in_use flag on a borrowed entry.
 * Must be called after every successful cache_lookup().
 * Safe to call with a NULL entry pointer.
 */
void cache_release(cache_t *cache, cache_entry_t *entry);

/*
 * cache_expire_entry()
 *
 * Mark the entry matching key as immediately expired - no longer
 * servable as a cache hit by any subsequent cache_lookup(). No-op if
 * the key is not found.
 *
 * Fixed (2026-08-12) - this used to unlink and free the entry directly
 * and unconditionally, with no check on whether another thread
 * currently had it checked out (cur->in_use) - a genuine use-after-
 * free risk under concurrent access, the same class of bug fixed in
 * cache_release() on 2026-08-09. Now defers the actual free entirely
 * to cache_evict_expired() below, which already correctly checks
 * !in_use before ever freeing anything - this function only ever marks
 * expiry_ts in the past, never touches the entry's memory directly.
 * Functionally equivalent from any caller's point of view (the key is
 * unusable as a cache hit the instant this returns), just safe under
 * concurrent access now.
 *
 * Returns  0  entry found and marked expired
 *          1  entry not found
 *         -1  error
 */
int cache_expire_entry(cache_t *cache, const char *key);

/*
 * cache_invalidate_by_tag()
 *
 * Table-level resultset cache invalidation (closure item 5 follow-up,
 * 2026-08-12) - marks every entry whose own table_dependency_tag
 * contains tag as immediately expired (same safe, defer-the-actual-
 * free-to-evict mechanism as cache_expire_entry() above - this
 * function is genuinely just a bulk version of that one). tag is
 * matched as a substring against each entry's comma-separated
 * dependency list, so "OCI_FIELD_TEST" correctly matches an entry
 * whose own tag is e.g. "OCI_FIELD_TEST,AUDIT_TRAIL" - deliberately
 * simple substring matching rather than a real tokenised comparison,
 * since table names in this project don't collide as substrings of
 * each other in practice (see resultset_cache.h's own note on this
 * being the deliberately simplest correct design, not row-level
 * tracking). Entries with a NULL table_dependency_tag (metadata_cache/
 * session_cache, or a resultset_cache entry stored without one) are
 * never matched.
 *
 * Returns the number of entries marked expired (>= 0), or -1 on error.
 */
int cache_invalidate_by_tag(cache_t *cache, const char *tag);

/*
 * cache_evict_expired()
 *
 * Sweep all buckets and free every entry where:
 *   - expiry_ts > 0 && time(NULL) >= expiry_ts  (TTL expired), OR
 *   - memory pressure exceeds max_memory_bytes  (LRU secondary)
 * Pinned and in_use entries are skipped.
 *
 * Returns  number of entries evicted (>= 0), or -1 on error
 */
int cache_evict_expired(cache_t *cache);

/*
 * cache_reinitialize()
 *
 * Evict all non-pinned entries and reset all statistics.
 * Equivalent to a soft flush - the cache structure stays intact.
 *
 * Returns  0  success
 *         -1  error
 */
int cache_reinitialize(cache_t *cache);

/*
 * cache_dump_report()
 *
 * Write a full cache state report to the logger at LOG_INFO level.
 * Includes: configuration, hit/miss/eviction counts, memory usage,
 * per-bucket collision depth, and execution statistics.
 */
void cache_dump_report(cache_t *cache);

/*
 * cache_update_exec_stats()
 *
 * Update execution statistics after a query completes.
 * Called by resultset_cache wrapper after each execute_query_batch.
 *
 * Parameters
 *   cache          - cache instance
 *   execution_ms   - wall time of OCI execute phase
 *   fetch_ms       - wall time of OCI fetch phase
 *   rows_returned  - rows in result set
 *   was_cache_hit  - 1 if result was served from cache, 0 if from OCI
 *   success        - 1 if query succeeded, 0 if it failed
 */
void cache_update_exec_stats(cache_t *cache,
                              double   execution_ms,
                              double   fetch_ms,
                              uint64_t rows_returned,
                              int      was_cache_hit,
                              int      success);

/*
 * cache_hash_string()
 *
 * Compute the hash of a key string using the algorithm configured
 * for this cache instance.  Exposed so wrapper modules can pre-compute
 * keys without calling cache_lookup().
 */
uint64_t cache_hash_string(const cache_t *cache, const char *key);

/*
 * cache_normalize_sql()
 *
 * Produce a canonical form of a SQL string for use as a cache key:
 *   - collapse all whitespace runs to a single space
 *   - strip leading and trailing whitespace
 *   - uppercase all characters
 *
 * The caller supplies dest and dest_max.
 * Returns dest on success, NULL if dest_max is too small.
 */
char *cache_normalize_sql(const char *sql, char *dest, size_t dest_max);

#ifdef __cplusplus
}
#endif

#endif /* OCI_CACHE_H */
