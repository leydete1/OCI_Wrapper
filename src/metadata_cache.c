/*
 * metadata_cache.c
 *
 * Metadata Cache - Implementation
 * ---------------------------------
 * Implements both metadata_cache.h and metadata_cache_meta.h.
 *
 * Include order (mandatory - do not reorder)
 * ------------------------------------------
 * 1. OCI_Connection.h            -> defines oci_context_t (full struct)
 * 2. OCI_Table_Metadata_Module.h -> defines col_metadata_t,
 *                                   metadata_request_t (anonymous structs)
 * 3. metadata_cache.h            -> cache lifecycle / key / low-level API
 * 4. metadata_cache_meta.h       -> col_metadata_t API declarations
 *
 * This order means all types are fully defined before any function
 * body that uses them is compiled.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "OCI_Connection.h"
#include "OCI_Table_Metadata_Module.h"
#include "metadata_cache.h"
#include "metadata_cache_meta.h"
#include "metrics.h"

/* ------------------------------------------------------------------ */
/*  Serialisation constants                                             */
/* ------------------------------------------------------------------ */
#define META_PAYLOAD_HDR_SIZE   sizeof(int)   /* col_count prefix      */

/* ------------------------------------------------------------------ */
/*  Internal: map config.ini hash algorithm string to enum             */
/* ------------------------------------------------------------------ */
static cache_hash_algorithm_t meta_parse_hash_algorithm(const char *s)
{
    if (!s) return CACHE_HASH_FNV1A;
    if (strcasecmp(s, "djb2")    == 0) return CACHE_HASH_DJB2;
    if (strcasecmp(s, "murmur3") == 0) return CACHE_HASH_MURMUR3;
    return CACHE_HASH_FNV1A;
}

/* ------------------------------------------------------------------ */
/*  Internal: uppercase string into a fixed-size buffer                */
/* ------------------------------------------------------------------ */
static void meta_upper_copy(const char *src, char *dst, size_t dst_max)
{
    if (!src || !dst || dst_max == 0) return;
    size_t i = 0;
    for (; src[i] && i < dst_max - 1; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

/* ================================================================== */
/*  metadata_cache_init                                                 */
/* ================================================================== */
cache_t *metadata_cache_init(const app_config_t *ini, logger_t *logger)
{
    if (!ini || !logger) return NULL;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Initialising metadata cache from ini");

    cache_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.cache_name, "metadata_cache", sizeof(cfg.cache_name) - 1);

    cfg.enabled        = ini->metadata_cache_enabled;
    cfg.ttl_seconds    = ini->metadata_cache_ttl_seconds    > 0
                         ? ini->metadata_cache_ttl_seconds  : 3600;
    cfg.max_entries    = (size_t)(ini->metadata_cache_max_entries > 0
                         ? ini->metadata_cache_max_entries  : 500);
    cfg.max_memory_bytes = (size_t)(ini->metadata_cache_max_memory_mb > 0
                           ? ini->metadata_cache_max_memory_mb : 64)
                           * 1024UL * 1024UL;
    cfg.bucket_count   = (size_t)(ini->metadata_cache_bucket_count > 0
                         ? ini->metadata_cache_bucket_count : 512);
    cfg.hash_algorithm = meta_parse_hash_algorithm(
                             ini->metadata_cache_hash_algorithm);
    cfg.eviction_policy = CACHE_EVICT_TTL_LRU;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "metadata_cache config: enabled=%d ttl=%d "
                 "max_entries=%zu max_memory_mb=%zu buckets=%zu "
                 "hash_algorithm=%s",
                 cfg.enabled,
                 cfg.ttl_seconds,
                 cfg.max_entries,
                 cfg.max_memory_bytes / (1024 * 1024),
                 cfg.bucket_count,
                 ini->metadata_cache_hash_algorithm[0]
                     ? ini->metadata_cache_hash_algorithm : "fnv1a");

    if (!cfg.enabled)
    {
        logger_write(logger, LOG_INFO, __func__, 0,
                     "metadata_cache is disabled in config.ini");
        return NULL;
    }

    cache_t *cache = cache_create(&cfg, logger);
    if (!cache)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "cache_create failed for metadata_cache");
        return NULL;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "metadata_cache initialised OK");
    return cache;
}

/* ================================================================== */
/*  metadata_cache_destroy                                              */
/* ================================================================== */
void metadata_cache_destroy(cache_t *cache)
{
    if (!cache) return;
    cache_destroy(cache);
}

/* ================================================================== */
/*  metadata_cache_make_key                                             */
/* ================================================================== */
char *metadata_cache_make_key(const char *owner,
                               const char *table_name,
                               char       *dest,
                               size_t      dest_max)
{
    if (!table_name || !dest || dest_max < 4) return NULL;

    char u_owner[128] = {0};
    char u_table[128] = {0};

    meta_upper_copy(table_name, u_table, sizeof(u_table));

    if (owner && owner[0] != '\0')
    {
        meta_upper_copy(owner, u_owner, sizeof(u_owner));
        snprintf(dest, dest_max, "%s.%s", u_owner, u_table);
    }
    else
    {
        snprintf(dest, dest_max, "%s", u_table);
    }

    return dest;
}

/* ================================================================== */
/*  metadata_cache_lookup                                               */
/* ================================================================== */
cache_entry_t *metadata_cache_lookup(cache_t    *cache,
                                      const char *key)
{
    if (!cache || !key) return NULL;
    return cache_lookup(cache, key);
}

/* ================================================================== */
/*  metadata_cache_release                                              */
/* ================================================================== */
void metadata_cache_release(cache_t       *cache,
                             cache_entry_t *entry)
{
    cache_release(cache, entry);
}

/* ================================================================== */
/*  metadata_cache_invalidate                                           */
/* ================================================================== */
int metadata_cache_invalidate(cache_t    *cache,
                               const char *owner,
                               const char *table_name)
{
    if (!cache || !table_name) return -1;

    char key[260] = {0};
    if (!metadata_cache_make_key(owner, table_name, key, sizeof(key)))
        return -1;

    return cache_expire_entry(cache, key);
}

/* ================================================================== */
/*  metadata_cache_evict                                                */
/* ================================================================== */
int metadata_cache_evict(cache_t *cache)
{
    if (!cache) return -1;
    return cache_evict_expired(cache);
}

/* ================================================================== */
/*  metadata_cache_report                                               */
/* ================================================================== */
void metadata_cache_report(cache_t *cache)
{
    if (!cache) return;
    cache_dump_report(cache);
}

/* ================================================================== */
/*  metadata_cache_store                                                */
/* ================================================================== */
int metadata_cache_store(cache_t              *cache,
                          logger_t             *logger,
                          const char           *key,
                          const col_metadata_t *cols,
                          int                   col_count)
{
    if (!cache || !key || !cols || col_count <= 0) return -1;

    size_t payload_size = META_PAYLOAD_HDR_SIZE +
                          (size_t)col_count * sizeof(col_metadata_t);

    char *payload = malloc(payload_size);
    if (!payload)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "[metadata_cache] malloc failed size=%zu key='%s'",
                     payload_size, key);
        return -1;
    }

    memcpy(payload, &col_count, META_PAYLOAD_HDR_SIZE);
    memcpy(payload + META_PAYLOAD_HDR_SIZE,
           cols,
           (size_t)col_count * sizeof(col_metadata_t));

    logger_write(logger, LOG_DEBUG, __func__, 0,
                 "[metadata_cache] Storing key='%s' col_count=%d "
                 "payload_size=%zu", key, col_count, payload_size);

    int rc = cache_insert(cache, key, payload, payload_size, NULL);
    if (rc != 0)
    {
        free(payload);
        logger_write(logger, LOG_WARN, __func__, 0,
                     "[metadata_cache] cache_insert failed (non-fatal) "
                     "key='%s'", key);
    }
    else
    {
        logger_write(logger, LOG_INFO, __func__, 0,
                     "[metadata_cache] Stored key='%s' col_count=%d",
                     key, col_count);
    }

    return rc;
}

/* ================================================================== */
/*  metadata_cache_deserialise                                          */
/* ================================================================== */
int metadata_cache_deserialise(const cache_entry_t *entry,
                                col_metadata_t      *cols,
                                int                 *col_count,
                                int                  max_cols)
{
    if (!entry || !cols || !col_count || max_cols <= 0) return -1;

    const char *payload     = entry->output_document;
    size_t      payload_len = entry->output_length;

    if (!payload || payload_len < META_PAYLOAD_HDR_SIZE) return -1;

    int stored_count = 0;
    memcpy(&stored_count, payload, META_PAYLOAD_HDR_SIZE);

    if (stored_count <= 0 || stored_count > max_cols) return -1;

    size_t expected = META_PAYLOAD_HDR_SIZE +
                      (size_t)stored_count * sizeof(col_metadata_t);
    if (payload_len < expected) return -1;

    memcpy(cols,
           payload + META_PAYLOAD_HDR_SIZE,
           (size_t)stored_count * sizeof(col_metadata_t));

    *col_count = stored_count;
    return 0;
}

/* ================================================================== */
/*  metadata_cache_get_or_fetch                                         */
/* ================================================================== */
int metadata_cache_get_or_fetch(cache_t                  *cache,
                                 oci_context_t            *ctx,
                                 metadata_request_t       *req,
                                 col_metadata_t           *cols,
                                 int                      *col_count,
                                 int                       max_cols,
                                 metadata_cache_result_t  *result)
{
    if (!ctx || !req || !cols || !col_count || max_cols <= 0)
    {
        if (ctx)
            logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                         "[metadata_cache] Invalid arguments");
        return -1;
    }

    /* Zero the result struct so callers always see clean values      */
    if (result)
    {
        result->was_cache_hit   = 0;
        result->cache_lookup_us = 0;
        result->cache_key_hash  = 0;
        result->cache_key[0]    = '\0';
    }

    /* ---- Cache disabled: fall straight through to OCI ---- */
    if (!cache)
    {
        logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                     "[metadata_cache] Cache not active - "
                     "calling get_request_metadata directly "
                     "table='%s'", req->table_name);
        return get_request_metadata(ctx, req, cols, col_count, max_cols);
    }

    /* ---- Build lookup key ---- */
    char cache_key[260] = {0};
    if (!metadata_cache_make_key(req->owner, req->table_name,
                                  cache_key, sizeof(cache_key)))
    {
        logger_write(ctx->Metadata_logger, LOG_WARN, __func__, 0,
                     "[metadata_cache] make_key failed - "
                     "falling through to OCI table='%s'",
                     req->table_name);
        return get_request_metadata(ctx, req, cols, col_count, max_cols);
    }

    /* ---- Compute hash and store in result for metrics ---- */
    uint64_t key_hash = cache_hash_string(cache, cache_key);
    if (result)
    {
        result->cache_key_hash = key_hash;
        strncpy(result->cache_key, cache_key,
                sizeof(result->cache_key) - 1);
    }

    /* ---- Timed cache lookup ---- */
    uint64_t      lookup_start = metrics_now_us();
    cache_entry_t *hit         = metadata_cache_lookup(cache, cache_key);
    uint64_t      lookup_us    = metrics_now_us() - lookup_start;

    if (result)
        result->cache_lookup_us = lookup_us;

    if (hit)
    {
        if (result)
            result->was_cache_hit = 1;

        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "[metadata_cache] CACHE HIT key='%s' "
                     "payload_len=%zu hit_count=%llu lookup_us=%llu",
                     cache_key,
                     hit->output_length,
                     (unsigned long long)hit->hit_count,
                     (unsigned long long)lookup_us);

        int rc = metadata_cache_deserialise(hit, cols, col_count, max_cols);
        metadata_cache_release(cache, hit);

        if (rc != 0)
        {
            logger_write(ctx->Metadata_logger, LOG_WARN, __func__, 0,
                         "[metadata_cache] Deserialise failed key='%s' "
                         "- expiring and re-fetching from OCI",
                         cache_key);
            metadata_cache_invalidate(cache, req->owner, req->table_name);
            if (result) result->was_cache_hit = 0;
            return get_request_metadata(ctx, req, cols, col_count, max_cols);
        }

        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "[metadata_cache] Served from cache key='%s' "
                     "col_count=%d", cache_key, *col_count);
        return 0;
    }

    /* ---- CACHE MISS: query Oracle ---- */
    logger_write(ctx->Metadata_logger, LOG_DEBUG, __func__, 0,
                 "[metadata_cache] CACHE MISS key='%s' - "
                 "calling get_request_metadata", cache_key);

    int rc = get_request_metadata(ctx, req, cols, col_count, max_cols);
    if (rc != 0)
    {
        logger_write(ctx->Metadata_logger, LOG_ERROR, __func__, 0,
                     "[metadata_cache] get_request_metadata failed "
                     "table='%s' - not caching", req->table_name);
        return rc;
    }

    /* ---- Store in cache (failure is non-fatal) ---- */
    int store_rc = metadata_cache_store(cache,
                                         ctx->Metadata_logger,
                                         cache_key,
                                         cols,
                                         *col_count);
    if (store_rc == 0)
        logger_write(ctx->Metadata_logger, LOG_INFO, __func__, 0,
                     "[metadata_cache] Stored in cache key='%s' "
                     "col_count=%d", cache_key, *col_count);
    else
        logger_write(ctx->Metadata_logger, LOG_WARN, __func__, 0,
                     "[metadata_cache] Cache store failed (non-fatal) "
                     "key='%s' - caller still has valid cols[]",
                     cache_key);

    return 0;
}
