/*
 * authz_cache.c
 *
 * Permission Cache - Implementation (Security Module Stage 5)
 * ---------------------------------------------------------------
 * Thin wrapper around oci_cache, structured identically to
 * session_cache.c (same file, same author intent: hash table, thread
 * safety, eviction, and statistics all live in oci_cache.c; this file
 * only knows about the permission-list payload shape).
 *
 * Keyed by session_id (not user_id) - built once, at session_create()
 * time, by authz_build_permission_cache() (OCI_Authz_Manager.c),
 * called from OCI_Auth_Manager.c right after a successful
 * authentication. authz_has_permission() is therefore a pure cache
 * lookup on the request's critical path - it never touches the
 * database (Security_Module_Design_Specification.docx Section 6.6).
 *
 * A permission change made by an administrator takes effect on the
 * user's NEXT session, not mid-session - the cache is the source of
 * truth for the lifetime of a session, matching session_cache's own
 * "cache miss = invalid, not a reason to fall back to the database"
 * philosophy.
 *
 *   - authz_cache_init()        reads ini, calls cache_create()
 *   - authz_cache_lookup()      delegates to cache_lookup()
 *   - authz_cache_release()     delegates to cache_release()
 *   - authz_cache_store()       encodes a permission-code list, calls
 *                                cache_insert()
 *   - authz_cache_invalidate()  delegates to cache_expire_entry()
 *   - authz_cache_evict()       delegates to cache_evict_expired()
 *   - authz_cache_report()      delegates to cache_dump_report()
 *   - authz_cache_destroy()     delegates to cache_destroy()
 *   - authz_cache_encode/decode()  pipe-delimited permission-code list
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "authz_cache.h"
#include "oci_cache.h"
#include "ini_reader.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/*  Internal: map config.ini hash algorithm string to enum, matching  */
/*  session_cache.c's own parse_hash_algorithm() exactly.              */
/* ------------------------------------------------------------------ */
static cache_hash_algorithm_t parse_hash_algorithm(const char *s)
{
    if (!s) return CACHE_HASH_FNV1A;
    if (strcasecmp(s, "djb2")    == 0) return CACHE_HASH_DJB2;
    if (strcasecmp(s, "murmur3") == 0) return CACHE_HASH_MURMUR3;
    return CACHE_HASH_FNV1A;   /* default */
}

/* ================================================================== */
/*  authz_cache_init                                                    */
/* ================================================================== */
cache_t *authz_cache_init(const app_config_t *ini, logger_t *logger)
{
    if (!ini || !logger) return NULL;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Initialising authz (permission) cache from ini");

    cache_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.cache_name, "authz_cache", sizeof(cfg.cache_name) - 1);

    cfg.enabled          = ini->authz_cache_enabled;
    cfg.ttl_seconds      = ini->authz_cache_ttl_seconds     > 0
                           ? ini->authz_cache_ttl_seconds   : 1800;
    cfg.max_entries      = (size_t)(ini->authz_cache_max_entries > 0
                           ? ini->authz_cache_max_entries   : 5000);
    cfg.max_memory_bytes = (size_t)(ini->authz_cache_max_memory_mb > 0
                           ? ini->authz_cache_max_memory_mb : 32)
                           * 1024UL * 1024UL;
    cfg.bucket_count     = (size_t)(ini->authz_cache_bucket_count > 0
                           ? ini->authz_cache_bucket_count  : 1024);
    cfg.hash_algorithm   = parse_hash_algorithm(
                               ini->authz_cache_hash_algorithm);
    cfg.eviction_policy  = CACHE_EVICT_TTL_LRU;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "authz_cache config: enabled=%d ttl=%d "
                 "max_entries=%zu max_memory_mb=%zu buckets=%zu "
                 "hash_algorithm=%s",
                 cfg.enabled,
                 cfg.ttl_seconds,
                 cfg.max_entries,
                 cfg.max_memory_bytes / (1024 * 1024),
                 cfg.bucket_count,
                 ini->authz_cache_hash_algorithm[0]
                     ? ini->authz_cache_hash_algorithm : "fnv1a");

    if (!cfg.enabled)
    {
        logger_write(logger, LOG_INFO, __func__, 0,
                     "authz_cache is disabled in config.ini - "
                     "authz_has_permission() will deny everything "
                     "(cache miss = denied, same as an expired session)");
        return NULL;
    }

    cache_t *cache = cache_create(&cfg, logger);
    if (!cache)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "cache_create failed for authz_cache");
        return NULL;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "authz_cache initialised OK");
    return cache;
}

/* ================================================================== */
/*  authz_cache_lookup / release                                        */
/* ================================================================== */
cache_entry_t *authz_cache_lookup(cache_t *cache, const char *session_id)
{
    if (!cache || !session_id) return NULL;
    return cache_lookup(cache, session_id);
}

void authz_cache_release(cache_t *cache, cache_entry_t *entry)
{
    cache_release(cache, entry);
}

/* ================================================================== */
/*  authz_cache_store                                                   */
/* ================================================================== */
int authz_cache_store(cache_t *cache, const char *session_id,
                       const char *permission_codes_csv)
{
    if (!cache || !session_id || !session_id[0])
    {
        /* FIX (2026-08-31): this guard was previously silent - no log
         * line at all - which is exactly what made the real bug here
         * (ctx->authz_cache never being copied into per-worker-thread
         * contexts, see ctx_utils.c's copy_shared_ctx_fields() and
         * OCI_Unit_Test_Module.c's own worker_ctx_storage setup) so
         * hard to find: every authz_cache_store() call from a worker
         * thread was silently returning -1 here, with nothing in any
         * log file pointing at why. Session_cache_store() has this
         * exact same silent-guard shape too (session_cache.c) - not
         * fixed here, out of scope for the Security Module, but worth
         * knowing this class of bug isn't unique to authz_cache.      */
        if (cache)
            logger_write(cache->logger, LOG_ERROR, __func__, 0,
                         "[authz_cache] invalid arguments (session_id=%s)",
                         session_id ? session_id : "(null)");
        return -1;
    }

    /* An empty permission list is a valid, real state (a user with no
     * granted permissions) - stored as "" via authz_cache_encode(),
     * not treated as an error.                                       */
    char *doc = authz_cache_encode(permission_codes_csv ? permission_codes_csv : "");
    if (!doc)
    {
        logger_write(cache->logger, LOG_ERROR, __func__, 0,
                     "[authz_cache] encode failed for session_id=%s",
                     session_id);
        return -1;
    }

    cache_entry_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    /* ttl_seconds = 0 -> cache default (authz_cache_ttl_seconds)
     * applies, same "mirrors session TTL by default" intent as the
     * config key's own doc comment (ini_reader.h).                   */

    int rc = cache_insert(cache, session_id, doc, strlen(doc), &opts);
    if (rc != 0)
    {
        /* cache_insert does not take ownership on failure - free here */
        free(doc);
    }
    return rc;
}

/* ================================================================== */
/*  authz_cache_invalidate                                              */
/* ================================================================== */
int authz_cache_invalidate(cache_t *cache, const char *session_id)
{
    if (!cache || !session_id) return -1;
    return cache_expire_entry(cache, session_id);
}

/* ================================================================== */
/*  authz_cache_evict / report / destroy                                */
/* ================================================================== */
int authz_cache_evict(cache_t *cache)
{
    if (!cache) return -1;
    return cache_evict_expired(cache);
}

void authz_cache_report(cache_t *cache)
{
    if (!cache) return;
    cache_dump_report(cache);
}

void authz_cache_destroy(cache_t *cache)
{
    if (!cache) return;
    cache_destroy(cache);
}

/* ================================================================== */
/*  authz_cache_encode / decode                                         */
/*                                                                      */
/*  Format (pipe-delimited, versioned - bump the version prefix if      */
/*  this layout ever changes, matching session_cache_encode()'s own     */
/*  convention exactly):                                                */
/*                                                                      */
/*    AZ1|<comma-separated PERMISSION_CODE list, "-" if empty>          */
/*                                                                      */
/*  A single pipe-delimited field holding a comma-separated list        */
/*  (rather than one pipe-delimited field per permission) keeps this    */
/*  encoding stable regardless of how many permissions a user has -     */
/*  session_cache_encode() has a fixed number of fields because a       */
/*  session record has a fixed number of attributes; a permission list  */
/*  does not.                                                            */
/* ================================================================== */
#define AUTHZ_CACHE_ENCODING_VERSION "AZ1"

char *authz_cache_encode(const char *permission_codes_csv)
{
    const char *codes = (permission_codes_csv && permission_codes_csv[0])
                        ? permission_codes_csv : "-";

    size_t needed = strlen(AUTHZ_CACHE_ENCODING_VERSION) + 1 /* '|' */
                  + strlen(codes) + 1 /* '\0' */;
    char *buf = malloc(needed);
    if (!buf) return NULL;

    int n = snprintf(buf, needed, "%s|%s",
                      AUTHZ_CACHE_ENCODING_VERSION, codes);
    if (n < 0 || (size_t)n >= needed)
    {
        free(buf);
        return NULL;
    }
    return buf;
}

int authz_cache_decode(const char *encoded, char *out_csv, size_t out_csv_size)
{
    if (!encoded || !out_csv || out_csv_size == 0) return -1;
    out_csv[0] = '\0';

    const char *pipe = strchr(encoded, '|');
    if (!pipe) return -1;

    size_t version_len = (size_t)(pipe - encoded);
    if (version_len != strlen(AUTHZ_CACHE_ENCODING_VERSION) ||
        strncmp(encoded, AUTHZ_CACHE_ENCODING_VERSION, version_len) != 0)
        return -1;

    const char *codes = pipe + 1;
    if (strcmp(codes, "-") == 0)
    {
        out_csv[0] = '\0';   /* empty permission list - a valid state */
        return 0;
    }

    strncpy(out_csv, codes, out_csv_size - 1);
    out_csv[out_csv_size - 1] = '\0';
    return 0;
}
