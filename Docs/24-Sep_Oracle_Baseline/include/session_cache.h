/*
 * session_cache.h
 *
 * Session Cache
 * -------------
 * Thin wrapper around oci_cache that provides a dedicated cache
 * instance for in-memory session lookups.
 *
 * Unlike resultset_cache (keyed on normalised SQL) the session cache
 * key IS the session_id UUID generated at session creation - no
 * normalisation step is required.
 *
 * The cached payload is a compact, pipe-delimited encoding of
 * session_record_t (see session_cache_encode / session_cache_decode
 * below).  This keeps the cache module free of any dependency on
 * OCI_Session_Manager.h and lets it be compiled and tested in
 * isolation, exactly like resultset_cache / metadata_cache.
 *
 * Integration point
 * -----------------
 * In OCI_Session_Manager.c:
 *
 *   1. session_create() builds a session_record_t, encodes it via
 *      session_cache_encode(), and calls session_cache_store().
 *   2. session_validate() / session_touch() call session_cache_lookup()
 *      - on HIT, decode the entry and return/update it, then call
 *      session_cache_release().
 *   3. session_end() calls session_cache_invalidate() after the
 *      permanent record has been updated in the database.
 *
 * config.ini keys
 * ---------------
 *   session_cache_enabled        = 1
 *   session_cache_ttl_seconds    = 1800
 *   session_cache_max_entries    = 5000
 *   session_cache_max_memory_mb  = 128
 *   session_cache_bucket_count   = 4096
 *   session_cache_hash_algorithm = fnv1a
 */

#ifndef SESSION_CACHE_H
#define SESSION_CACHE_H

#include <stdint.h>
#include <time.h>

#include "oci_cache.h"
#include "ini_reader.h"
#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Session status codes                                               */
/*  Shared with OCI_Session_Manager.h - kept here too so session_cache */
/*  has no compile-time dependency on the manager header.              */
/* ------------------------------------------------------------------ */
typedef enum {
    SESSION_STATUS_CREATED        = 0,  /* row inserted, not yet active   */
    SESSION_STATUS_ACTIVE         = 1,  /* in normal use                  */
    SESSION_STATUS_EXPIRED        = 2,  /* TTL lapsed, closed normally    */
    SESSION_STATUS_LOGGED_OUT     = 3,  /* explicit client logout         */
    SESSION_STATUS_EXPIRED_ORPHAN = 4   /* closed by startup reconciler   */
} session_status_t;

/* UUID string length (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx + NUL)      */
#define SESSION_UUID_LEN  37

/* ------------------------------------------------------------------ */
/*  session_record_t                                                    */
/*  In-memory representation of one session.  This is the value type   */
/*  stored (encoded) in the cache and mirrors the OCI_SESSION table.   */
/* ------------------------------------------------------------------ */
typedef struct {
    char             session_id       [SESSION_UUID_LEN];
    session_status_t status;
    time_t           created_ts;
    time_t           last_activity_ts;
    int              ttl_seconds;         /* 0 = use cache default TTL   */
    char             client_id  [128];
    char             client_ip  [64];
    char             client_host[128];
    char             application_name[128];
} session_record_t;

/*
 * session_cache_init()
 *
 * Create and return the session cache instance, reading all
 * configuration from ini.  Returns NULL if disabled or on error.
 */
cache_t *session_cache_init(const app_config_t *ini, logger_t *logger);


/*
 * session_cache_lookup()
 *
 * Look up a session by session_id.
 * Returns a borrowed cache_entry_t* on HIT (caller must call
 * session_cache_release() when done), NULL on MISS or expired.
 */
cache_entry_t *session_cache_lookup(cache_t *cache, const char *session_id);

/*
 * session_cache_release()
 *
 * Release a borrowed entry back to the cache.
 * Must be called after every successful session_cache_lookup().
 */
void session_cache_release(cache_t *cache, cache_entry_t *entry);

/*
 * session_cache_store()
 *
 * Encode and store a session_record_t in the cache, keyed on
 * rec->session_id.  If rec->ttl_seconds is 0 the cache's configured
 * default TTL is used.
 *
 * Returns  0  stored successfully
 *         -1  error or cache disabled
 */
int session_cache_store(cache_t *cache, const session_record_t *rec);

/*
 * session_cache_invalidate()
 *
 * Immediately expire the cache entry for session_id (does NOT touch
 * the permanent database record - callers must update that
 * separately, normally via OCI_Session_Manager's session_end()).
 */
int session_cache_invalidate(cache_t *cache, const char *session_id);

/*
 * session_cache_evict()
 *
 * Sweep expired entries.  Call periodically from a heartbeat thread.
 * Returns number of entries evicted.
 */
int session_cache_evict(cache_t *cache);

/*
 * session_cache_report()
 *
 * Write cache statistics to the logger.
 */
void session_cache_report(cache_t *cache);

/*
 * session_cache_destroy()
 *
 * Free all entries and the cache handle.  Call once at shutdown.
 */
void session_cache_destroy(cache_t *cache);

/*
 * session_cache_encode()
 *
 * Serialise a session_record_t into a heap-allocated, pipe-delimited
 * string suitable for storage as a cache_entry_t output_document.
 * Caller must free() the returned string.
 * Returns NULL on allocation failure.
 */
char *session_cache_encode(const session_record_t *rec);

/*
 * session_cache_decode()
 *
 * Parse a string produced by session_cache_encode() (or read back from
 * a cache_entry_t->output_document) into out.
 * Returns 0 on success, -1 on malformed input.
 */
int session_cache_decode(const char *encoded, session_record_t *out);

/*
 * session_status_str() / session_status_from_str()
 *
 * Convert between session_status_t and its human-readable / stored
 * string form ("CREATED","ACTIVE","EXPIRED","LOGGED_OUT",
 * "EXPIRED_ORPHAN").  session_status_str() return value is a string
 * literal - do NOT free.
 */
const char      *session_status_str(session_status_t status);
session_status_t session_status_from_str(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_CACHE_H */
