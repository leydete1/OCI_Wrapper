/*
 * session_cache.c
 *
 * Session Cache - Implementation
 * --------------------------------
 * Thin wrapper around oci_cache.  All heavy lifting (hash table,
 * thread safety, eviction, statistics) is in oci_cache.c.
 * This module provides:
 *
 *   - session_cache_init()       reads ini, calls cache_create()
 *   - session_cache_lookup()     delegates to cache_lookup()
 *   - session_cache_release()    delegates to cache_release()
 *   - session_cache_store()      encodes session_record_t, calls cache_insert()
 *   - session_cache_invalidate() delegates to cache_expire_entry()
 *   - session_cache_evict()      delegates to cache_evict_expired()
 *   - session_cache_report()     delegates to cache_dump_report()
 *   - session_cache_destroy()    delegates to cache_destroy()
 *   - session_cache_encode/decode()  pipe-delimited (de)serialisation
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

#include "session_cache.h"
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
/*  session_status_str / session_status_from_str                       */
/* ================================================================== */
const char *session_status_str(session_status_t status)
{
    switch (status)
    {
        case SESSION_STATUS_CREATED:        return "CREATED";
        case SESSION_STATUS_ACTIVE:         return "ACTIVE";
        case SESSION_STATUS_EXPIRED:        return "EXPIRED";
        case SESSION_STATUS_LOGGED_OUT:     return "LOGGED_OUT";
        case SESSION_STATUS_EXPIRED_ORPHAN: return "EXPIRED_ORPHAN";
        default:                            return "UNKNOWN";
    }
}

session_status_t session_status_from_str(const char *s)
{
    if (!s) return SESSION_STATUS_CREATED;
    if (strcasecmp(s, "ACTIVE")         == 0) return SESSION_STATUS_ACTIVE;
    if (strcasecmp(s, "EXPIRED")        == 0) return SESSION_STATUS_EXPIRED;
    if (strcasecmp(s, "LOGGED_OUT")     == 0) return SESSION_STATUS_LOGGED_OUT;
    if (strcasecmp(s, "EXPIRED_ORPHAN") == 0) return SESSION_STATUS_EXPIRED_ORPHAN;
    return SESSION_STATUS_CREATED;
}

/* ================================================================== */
/*  session_cache_init                                                  */
/* ================================================================== */
cache_t *session_cache_init(const app_config_t *ini, logger_t *logger)
{
    if (!ini || !logger) return NULL;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "Initialising session cache from ini");

    cache_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.cache_name, "session_cache", sizeof(cfg.cache_name) - 1);

    cfg.enabled          = ini->session_cache_enabled;
    cfg.ttl_seconds      = ini->session_cache_ttl_seconds     > 0
                           ? ini->session_cache_ttl_seconds   : 1800;
    cfg.max_entries      = (size_t)(ini->session_cache_max_entries > 0
                           ? ini->session_cache_max_entries   : 5000);
    cfg.max_memory_bytes = (size_t)(ini->session_cache_max_memory_mb > 0
                           ? ini->session_cache_max_memory_mb : 128)
                           * 1024UL * 1024UL;
    cfg.bucket_count     = (size_t)(ini->session_cache_bucket_count > 0
                           ? ini->session_cache_bucket_count  : 4096);
    cfg.hash_algorithm   = parse_hash_algorithm(
                               ini->session_cache_hash_algorithm);
    cfg.eviction_policy  = CACHE_EVICT_TTL_LRU;

    logger_write(logger, LOG_INFO, __func__, 0,
                 "session_cache config: enabled=%d ttl=%d "
                 "max_entries=%zu max_memory_mb=%zu buckets=%zu "
                 "hash_algorithm=%s",
                 cfg.enabled,
                 cfg.ttl_seconds,
                 cfg.max_entries,
                 cfg.max_memory_bytes / (1024 * 1024),
                 cfg.bucket_count,
                 ini->session_cache_hash_algorithm[0]
                     ? ini->session_cache_hash_algorithm : "fnv1a");

    if (!cfg.enabled)
    {
        logger_write(logger, LOG_INFO, __func__, 0,
                     "session_cache is disabled in config.ini");
        return NULL;
    }

    cache_t *cache = cache_create(&cfg, logger);
    if (!cache)
    {
        logger_write(logger, LOG_ERROR, __func__, 0,
                     "cache_create failed for session_cache");
        return NULL;
    }

    logger_write(logger, LOG_INFO, __func__, 0,
                 "session_cache initialised OK");
    return cache;
}

/* ================================================================== */
/*  session_cache_lookup / release                                      */
/* ================================================================== */
cache_entry_t *session_cache_lookup(cache_t *cache, const char *session_id)
{
    if (!cache || !session_id) return NULL;
    return cache_lookup(cache, session_id);
}

void session_cache_release(cache_t *cache, cache_entry_t *entry)
{
    cache_release(cache, entry);
}

/* ================================================================== */
/*  session_cache_store                                                 */
/* ================================================================== */
int session_cache_store(cache_t *cache, const session_record_t *rec)
{
    if (!cache || !rec || !rec->session_id[0]) return -1;

    char *doc = session_cache_encode(rec);
    if (!doc)
    {
        logger_write(cache->logger, LOG_ERROR, __func__, 0,
                     "[session_cache] encode failed for session_id=%s",
                     rec->session_id);
        return -1;
    }

    cache_entry_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.ttl_seconds = rec->ttl_seconds;   /* 0 = cache default applies */
    opts.client_ip   = rec->client_ip[0]   ? rec->client_ip   : NULL;
    opts.client_host = rec->client_host[0] ? rec->client_host : NULL;
    opts.client_id   = rec->client_id[0]   ? rec->client_id   : NULL;

    int rc = cache_insert(cache, rec->session_id, doc, strlen(doc), &opts);
    if (rc != 0)
    {
        /* cache_insert does not take ownership on failure - free here */
        free(doc);
    }
    return rc;
}

/* ================================================================== */
/*  session_cache_invalidate                                            */
/* ================================================================== */
int session_cache_invalidate(cache_t *cache, const char *session_id)
{
    if (!cache || !session_id) return -1;
    return cache_expire_entry(cache, session_id);
}

/* ================================================================== */
/*  session_cache_evict / report / destroy                              */
/* ================================================================== */
int session_cache_evict(cache_t *cache)
{
    if (!cache) return -1;
    return cache_evict_expired(cache);
}

void session_cache_report(cache_t *cache)
{
    if (!cache) return;
    cache_dump_report(cache);
}

void session_cache_destroy(cache_t *cache)
{
    if (!cache) return;
    cache_destroy(cache);
}

/* ================================================================== */
/*  session_cache_encode / decode                                       */
/*                                                                      */
/*  Format (pipe-delimited, fixed field order - bump a version prefix   */
/*  if the layout ever changes so decode() can reject stale entries):   */
/*                                                                      */
/*    SC1|<session_id>|<status>|<created_ts>|<last_activity_ts>|        */
/*        <ttl_seconds>|<client_id>|<client_ip>|<client_host>|          */
/*        <application_name>                                           */
/* ================================================================== */
#define SESSION_CACHE_ENCODING_VERSION "SC1"

char *session_cache_encode(const session_record_t *rec)
{
    if (!rec) return NULL;

    char *buf = malloc(1024);
    if (!buf) return NULL;

    int n = snprintf(buf, 1024,
                     "%s|%s|%s|%ld|%ld|%d|%s|%s|%s|%s",
                     SESSION_CACHE_ENCODING_VERSION,
                     rec->session_id,
                     session_status_str(rec->status),
                     (long)rec->created_ts,
                     (long)rec->last_activity_ts,
                     rec->ttl_seconds,
                     rec->client_id[0]         ? rec->client_id         : "-",
                     rec->client_ip[0]         ? rec->client_ip         : "-",
                     rec->client_host[0]       ? rec->client_host       : "-",
                     rec->application_name[0]  ? rec->application_name  : "-");

    if (n < 0 || n >= 1024)
    {
        free(buf);
        return NULL;
    }
    return buf;
}

int session_cache_decode(const char *encoded, session_record_t *out)
{
    if (!encoded || !out) return -1;

    memset(out, 0, sizeof(*out));

    char work[1024];
    strncpy(work, encoded, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *version    = strtok(work, "|");
    char *session_id = strtok(NULL, "|");
    char *status_str = strtok(NULL, "|");
    char *created    = strtok(NULL, "|");
    char *last_act   = strtok(NULL, "|");
    char *ttl        = strtok(NULL, "|");
    char *client_id  = strtok(NULL, "|");
    char *client_ip  = strtok(NULL, "|");
    char *client_host= strtok(NULL, "|");
    char *app_name   = strtok(NULL, "|");

    if (!version || strcmp(version, SESSION_CACHE_ENCODING_VERSION) != 0)
        return -1;
    if (!session_id || !status_str || !created || !last_act || !ttl)
        return -1;

    strncpy(out->session_id, session_id, sizeof(out->session_id) - 1);
    out->status           = session_status_from_str(status_str);
    out->created_ts       = (time_t)atol(created);
    out->last_activity_ts = (time_t)atol(last_act);
    out->ttl_seconds      = atoi(ttl);

    if (client_id  && strcmp(client_id,  "-") != 0)
        strncpy(out->client_id,  client_id,  sizeof(out->client_id)  - 1);
    if (client_ip  && strcmp(client_ip,  "-") != 0)
        strncpy(out->client_ip,  client_ip,  sizeof(out->client_ip)  - 1);
    if (client_host&& strcmp(client_host,"-") != 0)
        strncpy(out->client_host,client_host,sizeof(out->client_host)- 1);
    if (app_name   && strcmp(app_name,   "-") != 0)
        strncpy(out->application_name, app_name,
                sizeof(out->application_name) - 1);

    return 0;
}
