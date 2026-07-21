/*
 * resultset_cache.c
 *
 * Result Set Cache - Implementation
 * -----------------------------------
 * Thin wrapper around oci_cache.  All heavy lifting (hash table,
 * thread safety, eviction, statistics) is in oci_cache.c.
 * This module provides:
 *
 *   - resultset_cache_init()    reads ini, calls cache_create()
 *   - resultset_cache_make_key() normalises SQL for consistent keys
 *   - resultset_cache_lookup()   delegates to cache_lookup()
 *   - resultset_cache_release()  delegates to cache_release()
 *   - resultset_cache_store()    strdup's xml, calls cache_insert()
 *   - resultset_cache_invalidate() delegates to cache_expire_entry()
 *   - resultset_cache_evict()    delegates to cache_evict_expired()
 *   - resultset_cache_report()   delegates to cache_dump_report()
 *   - resultset_cache_destroy()  delegates to cache_destroy()
 *
 * Hash algorithm mapping from config.ini string
 * ---------------------------------------------
 *   "fnv1a"   -> CACHE_HASH_FNV1A   (default)
 *   "djb2"    -> CACHE_HASH_DJB2
 *   "murmur3" -> CACHE_HASH_MURMUR3
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "resultset_cache.h"
#include "oci_cache.h"
#include "ini_reader.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Internal: map config.ini hash algorithm string to enum             */
/* ------------------------------------------------------------------ */
static cache_hash_algorithm_t parse_hash_algorithm(const char *s)
{
    if (!s) return CACHE_HASH_FNV1A;
    if (strcasecmp(s, "djb2")    == 0) return CACHE_HASH_DJB2;
    if (strcasecmp(s, "murmur3") == 0) return CACHE_HASH_MURMUR3;
    return CACHE_HASH_FNV1A;   /* default */
}

/* ================================================================== */
/*  resultset_cache_init                                                */
/* ================================================================== */
cache_t *resultset_cache_init(const app_config_t *ini, logger_t *logger)
{
    if (!ini || !logger) return NULL;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Initialising resultset cache from ini");

    cache_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.cache_name, "resultset_cache", sizeof(cfg.cache_name) - 1);

    cfg.enabled        = ini->resultset_cache_enabled;
    cfg.ttl_seconds    = ini->resultset_cache_ttl_seconds    > 0
                         ? ini->resultset_cache_ttl_seconds  : 300;
    cfg.max_entries    = (size_t)(ini->resultset_cache_max_entries > 0
                         ? ini->resultset_cache_max_entries  : 1000);
    cfg.max_memory_bytes = (size_t)(ini->resultset_cache_max_memory_mb > 0
                           ? ini->resultset_cache_max_memory_mb : 256)
                           * 1024UL * 1024UL;
    cfg.bucket_count   = (size_t)(ini->resultset_cache_bucket_count > 0
                         ? ini->resultset_cache_bucket_count : 2048);
    cfg.hash_algorithm = parse_hash_algorithm(
                             ini->resultset_cache_hash_algorithm);
    cfg.eviction_policy = CACHE_EVICT_TTL_LRU;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "resultset_cache config: enabled=%d ttl=%d "
                 "max_entries=%zu max_memory_mb=%zu buckets=%zu "
                 "hash_algorithm=%s",
                 cfg.enabled,
                 cfg.ttl_seconds,
                 cfg.max_entries,
                 cfg.max_memory_bytes / (1024 * 1024),
                 cfg.bucket_count,
                 ini->resultset_cache_hash_algorithm[0]
                     ? ini->resultset_cache_hash_algorithm : "fnv1a");

    if (!cfg.enabled)
    {
        logger_write(logger, LOG_INFO, __func__, 0,
                     "resultset_cache is disabled in config.ini");
        return NULL;
    }

    cache_t *cache = cache_create(&cfg, logger);
    if (!cache)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "cache_create failed for resultset_cache");
        return NULL;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "resultset_cache initialised OK");
    return cache;
}

/* ================================================================== */
/*  resultset_cache_make_key                                            */
/* ================================================================== */
char *resultset_cache_make_key(const char *sql,
                                char       *dest,
                                size_t      dest_max)
{
    if (!sql || !dest || dest_max < 2) return NULL;
    return cache_normalize_sql(sql, dest, dest_max);
}

/* ================================================================== */
/*  resultset_cache_lookup                                              */
/* ================================================================== */
cache_entry_t *resultset_cache_lookup(cache_t    *cache,
                                       const char *normalised_key)
{
    if (!cache || !normalised_key) return NULL;
    return cache_lookup(cache, normalised_key);
}

/* ================================================================== */
/*  resultset_cache_release                                             */
/* ================================================================== */
void resultset_cache_release(cache_t       *cache,
                              cache_entry_t *entry)
{
    cache_release(cache, entry);
}

/* ================================================================== */
/*  resultset_cache_store                                               */
/* ================================================================== */
int resultset_cache_store(cache_t            *cache,
                           const char         *normalised_key,
                           const char         *output_xml,
                           const char         *output_json,
                           uint64_t            row_count,
                           cache_entry_opts_t *opts)
{
    if (!cache || !normalised_key || !output_xml) return -1;

    /* strdup so the cache owns its copy and the caller keeps theirs  */
    char *doc = strdup(output_xml);
    if (!doc)
    {
        logger_write(cache->logger, LOG_ERROR, __func__, 0,
                     "[resultset_cache] strdup failed for output_xml");
        return -1;
    }

    /* Copy caller's opts (if any) into a local struct so row_count and
     * output_json can be set without mutating memory the caller owns. */
    cache_entry_opts_t local_opts;
    if (opts) local_opts = *opts;
    else      memset(&local_opts, 0, sizeof(local_opts));
    local_opts.row_count = row_count;

    if (output_json)
    {
        local_opts.output_document_json = output_json;
        local_opts.output_length_json   = strlen(output_json);
    }

    int rc = cache_insert(cache, normalised_key, doc,
                          strlen(doc), &local_opts);
    if (rc != 0)
    {
        /* cache_insert frees doc on failure when cache disabled;
         * on actual error it does not - free it here               */
        free(doc);
    }
    return rc;
}

/* ================================================================== */
/*  resultset_cache_invalidate                                          */
/* ================================================================== */
int resultset_cache_invalidate(cache_t    *cache,
                                const char *sql)
{
    if (!cache || !sql) return -1;

    char key[8192];
    if (!cache_normalize_sql(sql, key, sizeof(key)))
    {
        logger_write(cache->logger, LOG_ERROR, __func__, 0,
                     "[resultset_cache] normalize failed for invalidate");
        return -1;
    }

    return cache_expire_entry(cache, key);
}

/* ================================================================== */
/*  resultset_cache_evict                                               */
/* ================================================================== */
int resultset_cache_evict(cache_t *cache)
{
    if (!cache) return -1;
    return cache_evict_expired(cache);
}

/* ================================================================== */
/*  resultset_cache_report                                              */
/* ================================================================== */
void resultset_cache_report(cache_t *cache)
{
    if (!cache) return;
    cache_dump_report(cache);
}

/* ================================================================== */
/*  resultset_cache_destroy                                             */
/* ================================================================== */
void resultset_cache_destroy(cache_t *cache)
{
    if (!cache) return;
    cache_destroy(cache);
}
