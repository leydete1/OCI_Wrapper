/*
 * oci_cache.c
 *
 * General Purpose Cache Module - Implementation
 * ----------------------------------------------
 * See oci_cache.h for full design documentation.
 *
 * Internal structure
 * ------------------
 *   hash_fnv1a()          - FNV-1a 64-bit hash
 *   hash_djb2()           - DJB2 64-bit hash
 *   hash_murmur3()        - MurmurHash3 64-bit (lower 64 of 128)
 *   compute_hash()        - dispatch to configured algorithm
 *   entry_memory_size()   - calculate total bytes for one entry
 *   alloc_entry()         - allocate and populate a cache_entry_t
 *   free_entry()          - free all heap fields in one entry
 *   evict_lru_under_lock()- evict least recently used when over limit
 *   cache_create()        - allocate and initialise cache instance
 *   cache_destroy()       - free all entries and handle
 *   cache_insert()        - insert or replace entry
 *   cache_lookup()        - find entry by key, set in_use
 *   cache_release()       - clear in_use flag
 *   cache_expire_entry()  - immediately expire one entry by key
 *   cache_evict_expired() - sweep all buckets, remove expired entries
 *   cache_reinitialize()  - flush all non-pinned entries, reset stats
 *   cache_dump_report()   - write full state to logger
 *   cache_update_exec_stats() - update rolling execution statistics
 *   cache_hash_string()   - public hash helper
 *   cache_normalize_sql() - canonical SQL form for cache key
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>

#include "oci_cache.h"
#include "logger.h"

/* ================================================================== */
/*  Hash functions                                                      */
/* ================================================================== */

/* ---- FNV-1a 64-bit ---- */
static uint64_t hash_fnv1a(const char *key)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char *p = (const unsigned char *)key;
    while (*p)
    {
        hash ^= (uint64_t)*p++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* ---- DJB2 64-bit ---- */
static uint64_t hash_djb2(const char *key)
{
    uint64_t hash = 5381;
    const unsigned char *p = (const unsigned char *)key;
    int c;
    while ((c = *p++))
        hash = ((hash << 5) + hash) + (uint64_t)c;
    return hash;
}

/* ---- MurmurHash3 - 64-bit finaliser on 64-bit seed ---- */
static uint64_t hash_murmur3(const char *key)
{
    const uint8_t *data  = (const uint8_t *)key;
    size_t         len   = strlen(key);
    uint64_t       seed  = UINT64_C(0xdeadbeefcafe1234);
    uint64_t       h     = seed ^ (uint64_t)len;
    const uint64_t c1    = UINT64_C(0x87c37b91114253d5);
    const uint64_t c2    = UINT64_C(0x4cf5ad432745937f);

    const uint64_t *blocks = (const uint64_t *)data;
    size_t nblocks = len / 8;

    for (size_t i = 0; i < nblocks; i++)
    {
        uint64_t k;
        memcpy(&k, blocks + i, sizeof(k));
        k *= c1;
        k = (k << 31) | (k >> 33);
        k *= c2;
        h ^= k;
        h = (h << 27) | (h >> 37);
        h = h * 5 + UINT64_C(0x52dce729);
    }

    /* Tail */
    const uint8_t *tail = data + nblocks * 8;
    uint64_t k1 = 0;
    switch (len & 7)
    {
        case 7: k1 ^= (uint64_t)tail[6] << 48; /* fall through */
        case 6: k1 ^= (uint64_t)tail[5] << 40; /* fall through */
        case 5: k1 ^= (uint64_t)tail[4] << 32; /* fall through */
        case 4: k1 ^= (uint64_t)tail[3] << 24; /* fall through */
        case 3: k1 ^= (uint64_t)tail[2] << 16; /* fall through */
        case 2: k1 ^= (uint64_t)tail[1] <<  8; /* fall through */
        case 1: k1 ^= (uint64_t)tail[0];
                k1 *= c1;
                k1  = (k1 << 31) | (k1 >> 33);
                k1 *= c2;
                h  ^= k1;
    }

    /* Finalise */
    h ^= (uint64_t)len;
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    h *= UINT64_C(0xc4ceb9fe1a85ec53);
    h ^= h >> 33;

    return h;
}

/* ---- Dispatch to configured algorithm ---- */
static uint64_t compute_hash(const cache_t *cache, const char *key)
{
    switch (cache->config.hash_algorithm)
    {
        case CACHE_HASH_DJB2:    return hash_djb2(key);
        case CACHE_HASH_MURMUR3: return hash_murmur3(key);
        case CACHE_HASH_FNV1A:
        default:                 return hash_fnv1a(key);
    }
}

/* ================================================================== */
/*  Memory accounting                                                   */
/* ================================================================== */
static size_t entry_memory_size(const cache_entry_t *e)
{
    size_t n = sizeof(cache_entry_t);

    if (e->normalized_sql)   n += strlen(e->normalized_sql)   + 1;
    if (e->output_document)  n += e->output_length             + 1;
    if (e->output_document_json) n += e->output_length_json    + 1;
    if (e->client_ip)        n += strlen(e->client_ip)         + 1;
    if (e->client_host)      n += strlen(e->client_host)       + 1;
    if (e->client_id)        n += strlen(e->client_id)         + 1;
    if (e->server_host)      n += strlen(e->server_host)       + 1;
    if (e->server_name)      n += strlen(e->server_name)       + 1;
    if (e->process_id)       n += strlen(e->process_id)        + 1;
    if (e->tx_trace)         n += strlen(e->tx_trace)          + 1;

    return n;
}

/* ================================================================== */
/*  alloc_entry                                                         */
/*  Allocate and populate a cache_entry_t from caller-supplied data.  */
/*  The cache takes ownership of output_doc.                           */
/* ================================================================== */
static cache_entry_t *alloc_entry(const char         *key,
                                   char               *output_doc,
                                   size_t              doc_len,
                                   uint64_t            hash_key,
                                   int                 ttl_seconds,
                                   cache_entry_opts_t *opts)
{
    cache_entry_t *e = calloc(1, sizeof(cache_entry_t));
    if (!e) return NULL;

    /* ---- Key ---- */
    e->normalized_sql = strdup(key);
    if (!e->normalized_sql) { free(e); return NULL; }

    /* ---- Payload (cache takes ownership) ---- */
    e->output_document = output_doc;
    e->output_length   = doc_len;
    e->row_count        = opts ? opts->row_count : 0;
    e->hash_key        = hash_key;

    /* ---- Optional secondary rendering (e.g. JSON alongside XML) ---- */
    if (opts && opts->output_document_json)
    {
        e->output_document_json = strdup(opts->output_document_json);
        e->output_length_json   = opts->output_length_json;
        if (!e->output_document_json)
        {
            /* Non-fatal: primary payload is still valid without it -
             * just log and continue with the entry XML/primary-only. */
            e->output_length_json = 0;
        }
    }

    /* ---- Timestamps ---- */
    time_t now       = time(NULL);
    e->created_ts    = now;
    e->last_access_ts = now;
    e->expiry_ts     = (ttl_seconds > 0) ? now + ttl_seconds : 0;

    /* ---- State ---- */
    e->in_use  = 0;
    e->dirty   = 0;
    e->pinned  = opts ? opts->pinned : 0;
    e->hit_count = 0;

    /* ---- Transaction tracing (optional) ---- */
    if (opts)
    {
        if (opts->client_ip)   e->client_ip   = strdup(opts->client_ip);
        if (opts->client_host) e->client_host = strdup(opts->client_host);
        if (opts->client_id)   e->client_id   = strdup(opts->client_id);
        if (opts->server_host) e->server_host = strdup(opts->server_host);
        if (opts->server_name) e->server_name = strdup(opts->server_name);
        if (opts->process_id)  e->process_id  = strdup(opts->process_id);
        if (opts->tx_trace)    e->tx_trace    = strdup(opts->tx_trace);
    }

    /* ---- Memory accounting ---- */
    e->entry_memory_bytes = entry_memory_size(e);

    return e;
}

/* ================================================================== */
/*  free_entry                                                          */
/*  Release all heap fields within one entry then free the entry.     */
/* ================================================================== */
static void free_entry(cache_entry_t *e)
{
    if (!e) return;

    free(e->normalized_sql);
    free(e->output_document);
    free(e->output_document_json);
    free(e->client_ip);
    free(e->client_host);
    free(e->client_id);
    free(e->server_host);
    free(e->server_name);
    free(e->process_id);
    free(e->tx_trace);

    free(e);
}

/* ================================================================== */
/*  evict_lru_under_lock                                                */
/*  Called when memory or entry count limits are exceeded.             */
/*  Finds the least recently used non-pinned, non-in-use entry across  */
/*  all buckets and frees it.  Caller must hold the write lock.        */
/*  Returns 1 if an entry was evicted, 0 if nothing was evictable.    */
/* ================================================================== */
static int evict_lru_under_lock(cache_t *cache)
{
    cache_entry_t *victim      = NULL;
    cache_entry_t *victim_prev = NULL;
    size_t         victim_bucket = 0;
    time_t         oldest_access = (time_t)UINT64_MAX;

    for (size_t b = 0; b < cache->bucket_count; b++)
    {
        cache_entry_t *prev = NULL;
        cache_entry_t *cur  = cache->buckets[b];

        while (cur)
        {
            if (!cur->pinned && !cur->in_use)
            {
                if (cur->last_access_ts < oldest_access)
                {
                    oldest_access  = cur->last_access_ts;
                    victim         = cur;
                    victim_prev    = prev;
                    victim_bucket  = b;
                }
            }
            prev = cur;
            cur  = cur->next;
        }
    }

    if (!victim) return 0;   /* nothing evictable */

    /* Unlink victim from its bucket chain */
    if (victim_prev)
        victim_prev->next = victim->next;
    else
        cache->buckets[victim_bucket] = victim->next;

    cache->current_memory_bytes -= victim->entry_memory_bytes;
    cache->entry_count--;
    cache->cache_evictions++;
    cache->stats.eviction_rate++;

    logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                 "[%s] LRU evict key='%.80s' last_access=%ld",
                 cache->config.cache_name,
                 victim->normalized_sql ? victim->normalized_sql : "",
                 (long)victim->last_access_ts);

    free_entry(victim);
    return 1;
}

/* ================================================================== */
/*  cache_create                                                        */
/* ================================================================== */
cache_t *cache_create(const cache_config_t *cfg, logger_t *logger)
{
    if (!cfg || !logger)
    {
        if (logger)
            logger_write(logger, LOG_ERROR, __func__, 0,
                         "cache_create: cfg or logger is NULL");
        return NULL;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Creating cache '%s' buckets=%zu max_entries=%zu "
                 "max_memory_mb=%zu ttl=%ds hash=%d evict=%d enabled=%d",
                 cfg->cache_name,
                 cfg->bucket_count,
                 cfg->max_entries,
                 cfg->max_memory_bytes / (1024 * 1024),
                 cfg->ttl_seconds,
                 cfg->hash_algorithm,
                 cfg->eviction_policy,
                 cfg->enabled);

    /* ---- Validate bucket count - force power of two ---- */
    size_t buckets = cfg->bucket_count;
    if (buckets == 0) buckets = 1024;
    /* Round up to next power of two for fast modulo via bitmask */
    size_t pw2 = 1;
    while (pw2 < buckets) pw2 <<= 1;
    buckets = pw2;

    cache_t *cache = calloc(1, sizeof(cache_t));
    if (!cache)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "calloc failed for cache_t");
        return NULL;
    }

    cache->buckets = calloc(buckets, sizeof(cache_entry_t *));
    if (!cache->buckets)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "calloc failed for buckets array size=%zu", buckets);
        free(cache);
        return NULL;
    }

    /* ---- Copy configuration ---- */
    cache->config              = *cfg;
    cache->config.bucket_count = buckets;   /* store rounded value   */
    cache->bucket_count        = buckets;
    cache->max_entries         = cfg->max_entries  > 0 ? cfg->max_entries  : 10000;
    cache->max_memory_bytes    = cfg->max_memory_bytes > 0
                                 ? cfg->max_memory_bytes
                                 : 256UL * 1024 * 1024;  /* 256 MB default */
    cache->ttl_seconds         = cfg->ttl_seconds  > 0 ? cfg->ttl_seconds  : 300;
    cache->enabled             = cfg->enabled;
    cache->logger              = logger;

    /* ---- Initialise execution stats ---- */
    cache->stats.min_execution_ms = 1e18;   /* will be overwritten    */

    /* ---- Initialise rwlock ---- */
    if (pthread_rwlock_init(&cache->lock, NULL) != 0)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "pthread_rwlock_init failed");
        free(cache->buckets);
        free(cache);
        return NULL;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Cache '%s' created OK buckets=%zu (rounded to power of 2)",
                 cfg->cache_name, buckets);

    return cache;
}

/* ================================================================== */
/*  cache_destroy                                                       */
/* ================================================================== */
void cache_destroy(cache_t *cache)
{
    if (!cache) return;

    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "Destroying cache '%s' entries=%zu",
                 cache->config.cache_name, cache->entry_count);

    pthread_rwlock_wrlock(&cache->lock);

    for (size_t b = 0; b < cache->bucket_count; b++)
    {
        cache_entry_t *cur = cache->buckets[b];
        while (cur)
        {
            cache_entry_t *next = cur->next;
            free_entry(cur);
            cur = next;
        }
        cache->buckets[b] = NULL;
    }

    free(cache->buckets);
    cache->buckets = NULL;

    pthread_rwlock_unlock(&cache->lock);
    pthread_rwlock_destroy(&cache->lock);

    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "Cache '%s' destroyed", cache->config.cache_name);

    free(cache);
}

/* ================================================================== */
/*  cache_insert                                                        */
/* ================================================================== */
int cache_insert(cache_t            *cache,
                 const char         *key,
                 char               *output_doc,
                 size_t              doc_len,
                 cache_entry_opts_t *opts)
{
    if (!cache || !key || !output_doc)
    {
        if (cache)
            logger_write(cache->logger, LOG_ERROR, __func__, 0,
                         "[%s] cache_insert: invalid arguments",
                         cache->config.cache_name);
        return -1;
    }

    if (!cache->enabled)
    {
        /* Cache disabled - free the document the caller passed in    */
        free(output_doc);
        return 0;
    }

    uint64_t hash   = compute_hash(cache, key);
    size_t   bucket = (size_t)(hash & (cache->bucket_count - 1));

    /* Determine effective TTL */
    int ttl = (opts && opts->ttl_seconds > 0)
              ? opts->ttl_seconds
              : cache->ttl_seconds;

    logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                 "[%s] insert key='%.80s' hash=%"PRIu64" bucket=%zu "
                 "doc_len=%zu ttl=%ds",
                 cache->config.cache_name, key, hash, bucket,
                 doc_len, ttl);

    pthread_rwlock_wrlock(&cache->lock);

    /* ---- Expire existing entry with same key if present ---- */
    cache_entry_t *prev = NULL;
    cache_entry_t *cur  = cache->buckets[bucket];
    while (cur)
    {
        if (cur->hash_key == hash &&
            cur->normalized_sql &&
            strcmp(cur->normalized_sql, key) == 0)
        {
            logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                         "[%s] replacing existing entry for key='%.80s'",
                         cache->config.cache_name, key);

            /* Unlink */
            if (prev) prev->next = cur->next;
            else      cache->buckets[bucket] = cur->next;

            cache->current_memory_bytes -= cur->entry_memory_bytes;
            cache->entry_count--;
            free_entry(cur);
            break;
        }
        prev = cur;
        cur  = cur->next;
    }

    /* ---- Enforce entry count limit via LRU eviction ---- */
    while (cache->entry_count >= cache->max_entries)
    {
        logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                     "[%s] entry_count=%zu >= max=%zu - LRU evict",
                     cache->config.cache_name,
                     cache->entry_count, cache->max_entries);
        if (!evict_lru_under_lock(cache)) break;
    }

    /* ---- Enforce memory limit via LRU eviction ---- */
    size_t estimated = sizeof(cache_entry_t) + strlen(key) + 1 + doc_len + 1;
    while (cache->current_memory_bytes + estimated > cache->max_memory_bytes)
    {
        logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                     "[%s] memory=%zu + new=%zu > max=%zu - LRU evict",
                     cache->config.cache_name,
                     cache->current_memory_bytes,
                     estimated, cache->max_memory_bytes);
        if (!evict_lru_under_lock(cache)) break;
    }

    /* ---- Allocate new entry ---- */
    cache_entry_t *entry = alloc_entry(key, output_doc, doc_len,
                                        hash, ttl, opts);
    if (!entry)
    {
        logger_write(cache->logger, LOG_ERROR, __func__, 0,
                     "[%s] alloc_entry failed", cache->config.cache_name);
        pthread_rwlock_unlock(&cache->lock);
        return -1;
    }

    /* ---- Track collision ---- */
    if (cache->buckets[bucket] != NULL)
        cache->collision_count++;

    /* ---- Prepend to bucket chain ---- */
    entry->next           = cache->buckets[bucket];
    cache->buckets[bucket] = entry;

    cache->entry_count++;
    cache->current_memory_bytes += entry->entry_memory_bytes;
    cache->cache_inserts++;

    /* ---- Update peak entry size stat ---- */
    if (entry->entry_memory_bytes > cache->stats.peak_entry_size)
        cache->stats.peak_entry_size = entry->entry_memory_bytes;

    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "[%s] insert OK entries=%zu memory=%zu bytes",
                 cache->config.cache_name,
                 cache->entry_count,
                 cache->current_memory_bytes);

    pthread_rwlock_unlock(&cache->lock);
    return 0;
}

/* ================================================================== */
/*  cache_lookup                                                        */
/* ================================================================== */
cache_entry_t *cache_lookup(cache_t *cache, const char *key)
{
    if (!cache || !key) return NULL;
    if (!cache->enabled) return NULL;

    uint64_t hash   = compute_hash(cache, key);
    size_t   bucket = (size_t)(hash & (cache->bucket_count - 1));

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    pthread_rwlock_rdlock(&cache->lock);

    cache_entry_t *cur = cache->buckets[bucket];
    while (cur)
    {
        if (cur->hash_key == hash &&
            cur->normalized_sql &&
            strcmp(cur->normalized_sql, key) == 0)
        {
            /* ---- Check TTL expiry ---- */
            time_t now = time(NULL);
            if (cur->expiry_ts > 0 && now >= cur->expiry_ts)
            {
                logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                             "[%s] lookup HIT but EXPIRED key='%.80s'",
                             cache->config.cache_name, key);
                pthread_rwlock_unlock(&cache->lock);
                cache->cache_misses++;
                cache->stats.cache_misses++;
                return NULL;
            }

            /* ---- Live hit ---- */
            cur->in_use        = 1;
            cur->last_access_ts = now;
            cur->hit_count++;

            cache->cache_hits++;
            cache->stats.cache_hits++;

            clock_gettime(CLOCK_MONOTONIC, &ts_end);
            uint64_t lookup_us =
                (uint64_t)((ts_end.tv_sec  - ts_start.tv_sec)  * 1000000 +
                           (ts_end.tv_nsec - ts_start.tv_nsec) / 1000);

            /* Rolling average lookup time */
            uint64_t n = cache->stats.cache_hits;
            cache->stats.avg_lookup_us =
                (cache->stats.avg_lookup_us * (n - 1) + lookup_us) / n;

            logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                         "[%s] HIT key='%.80s' hit_count=%"PRIu64
                         " lookup_us=%"PRIu64,
                         cache->config.cache_name, key,
                         cur->hit_count, lookup_us);

            pthread_rwlock_unlock(&cache->lock);
            return cur;
        }
        cur = cur->next;
    }

    /* Miss */
    cache->cache_misses++;
    cache->stats.cache_misses++;

    logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                 "[%s] MISS key='%.80s'",
                 cache->config.cache_name, key);

    pthread_rwlock_unlock(&cache->lock);
    return NULL;
}

/* ================================================================== */
/*  cache_release                                                       */
/* ================================================================== */
void cache_release(cache_t *cache, cache_entry_t *entry)
{
    if (!cache || !entry) return;

    pthread_rwlock_wrlock(&cache->lock);
    entry->in_use = 0;
    pthread_rwlock_unlock(&cache->lock);

    logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                 "[%s] released entry key='%.80s'",
                 cache->config.cache_name,
                 entry->normalized_sql ? entry->normalized_sql : "");
}

/* ================================================================== */
/*  cache_expire_entry                                                  */
/* ================================================================== */
int cache_expire_entry(cache_t *cache, const char *key)
{
    if (!cache || !key) return -1;

    uint64_t hash   = compute_hash(cache, key);
    size_t   bucket = (size_t)(hash & (cache->bucket_count - 1));

    pthread_rwlock_wrlock(&cache->lock);

    cache_entry_t *prev = NULL;
    cache_entry_t *cur  = cache->buckets[bucket];

    while (cur)
    {
        if (cur->hash_key == hash &&
            cur->normalized_sql &&
            strcmp(cur->normalized_sql, key) == 0)
        {
            /* Unlink */
            if (prev) prev->next = cur->next;
            else      cache->buckets[bucket] = cur->next;

            cache->current_memory_bytes -= cur->entry_memory_bytes;
            cache->entry_count--;
            cache->cache_evictions++;
            cache->stats.eviction_rate++;

            logger_write(cache->logger, LOG_INFO, __func__, 0,
                         "[%s] expired entry key='%.80s'",
                         cache->config.cache_name, key);

            free_entry(cur);

            pthread_rwlock_unlock(&cache->lock);
            return 0;   /* found and expired */
        }
        prev = cur;
        cur  = cur->next;
    }

    pthread_rwlock_unlock(&cache->lock);

    logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                 "[%s] expire_entry: key not found '%.80s'",
                 cache->config.cache_name, key);
    return 1;   /* not found */
}

/* ================================================================== */
/*  cache_evict_expired                                                 */
/* ================================================================== */
int cache_evict_expired(cache_t *cache)
{
    if (!cache) return -1;

    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "[%s] Starting evict_expired sweep entries=%zu",
                 cache->config.cache_name, cache->entry_count);

    int    evicted = 0;
    time_t now     = time(NULL);

    pthread_rwlock_wrlock(&cache->lock);

    for (size_t b = 0; b < cache->bucket_count; b++)
    {
        cache_entry_t *prev = NULL;
        cache_entry_t *cur  = cache->buckets[b];

        while (cur)
        {
            cache_entry_t *next = cur->next;
            int expired = (cur->expiry_ts > 0 && now >= cur->expiry_ts);

            if (expired && !cur->pinned && !cur->in_use)
            {
                /* Unlink */
                if (prev) prev->next = next;
                else      cache->buckets[b] = next;

                cache->current_memory_bytes -= cur->entry_memory_bytes;
                cache->entry_count--;
                cache->cache_evictions++;
                cache->stats.eviction_rate++;
                evicted++;

                logger_write(cache->logger, LOG_DEBUG, __func__, 0,
                             "[%s] evicting key='%.80s' age=%lds",
                             cache->config.cache_name,
                             cur->normalized_sql ? cur->normalized_sql : "",
                             (long)(now - cur->created_ts));

                free_entry(cur);
                cur = next;
                continue;
            }

            prev = cur;
            cur  = next;
        }
    }

    /* ---- Secondary LRU pass if still over memory limit ---- */
    int lru_evicted = 0;
    while (cache->current_memory_bytes > cache->max_memory_bytes)
    {
        if (!evict_lru_under_lock(cache)) break;
        lru_evicted++;
    }

    pthread_rwlock_unlock(&cache->lock);

    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "[%s] evict_expired complete: ttl_evicted=%d "
                 "lru_evicted=%d remaining=%zu memory=%zu",
                 cache->config.cache_name,
                 evicted, lru_evicted,
                 cache->entry_count,
                 cache->current_memory_bytes);

    return evicted + lru_evicted;
}

/* ================================================================== */
/*  cache_reinitialize                                                  */
/* ================================================================== */
int cache_reinitialize(cache_t *cache)
{
    if (!cache) return -1;

    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "[%s] Reinitializing cache entries=%zu",
                 cache->config.cache_name, cache->entry_count);

    pthread_rwlock_wrlock(&cache->lock);

    int freed = 0;
    for (size_t b = 0; b < cache->bucket_count; b++)
    {
        cache_entry_t *cur = cache->buckets[b];
        cache_entry_t *kept_head = NULL;
        cache_entry_t *kept_tail = NULL;

        while (cur)
        {
            cache_entry_t *next = cur->next;

            if (!cur->pinned)
            {
                cache->current_memory_bytes -= cur->entry_memory_bytes;
                cache->entry_count--;
                freed++;
                free_entry(cur);
            }
            else
            {
                /* Preserve pinned entries */
                cur->next = NULL;
                if (!kept_head) kept_head = cur;
                else            kept_tail->next = cur;
                kept_tail = cur;
            }
            cur = next;
        }
        cache->buckets[b] = kept_head;
    }

    /* ---- Reset statistics (preserve entry_count which reflects
     *      remaining pinned entries) ---- */
    cache->cache_hits      = 0;
    cache->cache_misses    = 0;
    cache->cache_evictions = 0;
    cache->cache_inserts   = 0;
    cache->collision_count = 0;
    memset(&cache->stats, 0, sizeof(cache->stats));
    cache->stats.min_execution_ms = 1e18;

    pthread_rwlock_unlock(&cache->lock);

    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "[%s] Reinitialized: freed=%d pinned_kept=%zu",
                 cache->config.cache_name,
                 freed, cache->entry_count);
    return 0;
}

/* ================================================================== */
/*  cache_dump_report                                                   */
/* ================================================================== */
void cache_dump_report(cache_t *cache)
{
    if (!cache) return;

    pthread_rwlock_rdlock(&cache->lock);

    /* ---- Compute memory pressure ---- */
    double mem_pct = 0.0;
    if (cache->max_memory_bytes > 0)
        mem_pct = (double)cache->current_memory_bytes /
                  (double)cache->max_memory_bytes * 100.0;

    /* ---- Compute hit rate ---- */
    uint64_t total_lookups = cache->cache_hits + cache->cache_misses;
    double   hit_rate = total_lookups > 0
                        ? (double)cache->cache_hits /
                          (double)total_lookups * 100.0
                        : 0.0;

    /* ---- Compute max collision depth ---- */
    size_t max_depth = 0;
    size_t used_buckets = 0;
    for (size_t b = 0; b < cache->bucket_count; b++)
    {
        size_t depth = 0;
        cache_entry_t *cur = cache->buckets[b];
        while (cur) { depth++; cur = cur->next; }
        if (depth > max_depth) max_depth = depth;
        if (depth > 0) used_buckets++;
    }

    /* ---- Update memory pressure in stats ---- */
    cache->stats.memory_pressure_pct = mem_pct;

    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "======================================");
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  Cache Report: %s", cache->config.cache_name);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "======================================");
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  enabled              = %d", cache->enabled);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  hash_algorithm       = %d", cache->config.hash_algorithm);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  eviction_policy      = %d", cache->config.eviction_policy);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  bucket_count         = %zu", cache->bucket_count);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  used_buckets         = %zu", used_buckets);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  max_collision_depth  = %zu", max_depth);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  entry_count          = %zu", cache->entry_count);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  max_entries          = %zu", cache->max_entries);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  ttl_seconds          = %d",  cache->ttl_seconds);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  current_memory_bytes = %zu", cache->current_memory_bytes);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  max_memory_bytes     = %zu", cache->max_memory_bytes);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  memory_pressure_pct  = %.1f%%", mem_pct);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  cache_hits           = %"PRIu64, cache->cache_hits);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  cache_misses         = %"PRIu64, cache->cache_misses);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  hit_rate             = %.1f%%", hit_rate);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  cache_inserts        = %"PRIu64, cache->cache_inserts);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  cache_evictions      = %"PRIu64, cache->cache_evictions);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  collision_count      = %"PRIu64, cache->collision_count);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "--- Execution Stats ---");
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  execution_count      = %"PRIu64,
                 cache->stats.execution_count);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  success_count        = %"PRIu64,
                 cache->stats.success_count);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  failure_count        = %"PRIu64,
                 cache->stats.failure_count);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  rows_returned        = %"PRIu64,
                 cache->stats.rows_returned);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  avg_execution_ms     = %.3f",
                 cache->stats.avg_execution_ms);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  min_execution_ms     = %.3f",
                 cache->stats.min_execution_ms >= 1e17
                 ? 0.0 : cache->stats.min_execution_ms);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  max_execution_ms     = %.3f",
                 cache->stats.max_execution_ms);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  avg_fetch_ms         = %.3f",
                 cache->stats.avg_fetch_ms);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  avg_lookup_us        = %"PRIu64,
                 cache->stats.avg_lookup_us);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  peak_entry_size      = %"PRIu64" bytes",
                 cache->stats.peak_entry_size);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "  eviction_rate        = %"PRIu64" (since last report)",
                 cache->stats.eviction_rate);
    logger_write(cache->logger, LOG_INFO, __func__, 0,
                 "======================================");

    /* Reset eviction_rate counter after report */
    cache->stats.eviction_rate = 0;

    pthread_rwlock_unlock(&cache->lock);
}

/* ================================================================== */
/*  cache_update_exec_stats                                             */
/* ================================================================== */
void cache_update_exec_stats(cache_t *cache,
                              double   execution_ms,
                              double   fetch_ms,
                              uint64_t rows_returned,
                              int      was_cache_hit,
                              int      success)
{
    if (!cache) return;

    pthread_rwlock_wrlock(&cache->lock);

    query_execution_stats_t *s = &cache->stats;

    s->execution_count++;
    s->rows_returned += rows_returned;
    s->last_execution_ts = time(NULL);

    if (success)
        s->success_count++;
    else
        s->failure_count++;

    if (was_cache_hit)
        s->cache_hits++;
    else
        s->cache_misses++;

    /* ---- Rolling execution time stats ---- */
    s->total_execution_ms += execution_ms;
    s->avg_execution_ms    = s->total_execution_ms / (double)s->execution_count;

    if (execution_ms < s->min_execution_ms)
        s->min_execution_ms = execution_ms;
    if (execution_ms > s->max_execution_ms)
        s->max_execution_ms = execution_ms;

    /* ---- Rolling fetch time stats ---- */
    s->total_fetch_ms += fetch_ms;
    s->avg_fetch_ms    = s->total_fetch_ms / (double)s->execution_count;

    /* ---- Memory pressure ---- */
    if (cache->max_memory_bytes > 0)
        s->memory_pressure_pct =
            (double)cache->current_memory_bytes /
            (double)cache->max_memory_bytes * 100.0;

    pthread_rwlock_unlock(&cache->lock);
}

/* ================================================================== */
/*  cache_hash_string                                                   */
/* ================================================================== */
uint64_t cache_hash_string(const cache_t *cache, const char *key)
{
    if (!cache || !key) return 0;
    return compute_hash(cache, key);
}

/* ================================================================== */
/*  cache_normalize_sql                                                 */
/*  Canonical SQL: uppercase, collapse whitespace, trim.               */
/* ================================================================== */
char *cache_normalize_sql(const char *sql, char *dest, size_t dest_max)
{
    if (!sql || !dest || dest_max < 2) return NULL;

    const char *s   = sql;
    char       *d   = dest;
    size_t      len = 0;
    int         in_ws = 1;   /* treat leading whitespace as consumed */

    while (*s && len < dest_max - 1)
    {
        unsigned char c = (unsigned char)*s++;

        if (isspace(c))
        {
            if (!in_ws && len < dest_max - 2)
            {
                *d++ = ' ';
                len++;
                in_ws = 1;
            }
        }
        else
        {
            *d++  = (char)toupper(c);
            len++;
            in_ws = 0;
        }
    }

    /* Trim trailing space */
    if (len > 0 && dest[len - 1] == ' ')
    {
        len--;
        d--;
    }

    *d = '\0';
    return dest;
}
