/*
 * OCI_Session_Manager.h
 *
 * Session Manager Module
 * ------------------------
 * Provides client session lifecycle management for the Data_Manager
 * project: create, validate, touch, end, and crash-recovery
 * reconciliation.
 *
 * Design overview
 * ---------------
 * A "session" here represents one client's logical connection to the
 * application layer (as distinct from an Oracle DB session / OCI
 * connection, and distinct from a Transaction Manager transaction -
 * one session may span many transactions over its lifetime).
 *
 *   1. session_create() generates a UUID, writes a permanent row to
 *      the OCI_SESSION table (via the existing insert pipeline - see
 *      "Reuse of existing modules" below), and stores an in-memory
 *      copy in session_cache for fast subsequent lookups.
 *   2. session_validate() / session_touch() are cache-only fast-path
 *      operations - they do not hit the database on every call.
 *   3. session_end() updates the permanent row (via the existing
 *      update pipeline) to a terminal status and invalidates the
 *      cache entry.
 *   4. session_reconcile_orphans(), intended to be called once at
 *      application startup before any client traffic is served, finds
 *      permanent rows left ACTIVE with no CLOSED_TS whose TTL has
 *      already elapsed - i.e. sessions that were never cleanly closed
 *      because the process crashed or was killed - and closes them out
 *      with status EXPIRED_ORPHAN.  It does not attempt to recover or
 *      infer any missing session data; it only closes the gap so the
 *      permanent table does not accumulate rows that look eternally
 *      ACTIVE.
 *
 * Reuse of existing modules (no direct OCI calls in this module)
 * -----------------------------------------------------------------
 *   - tx_generate_uuid()      (OCI_Transaction_Manager.h) for the
 *     session_id, so session IDs and transaction IDs share the same
 *     UUID format and generation code path.
 *   - execute_insert_batch()  (OCI_Insert_Execute_Module.h) to persist
 *     new sessions.  Metrics and audit-trail rows are produced
 *     automatically as a side effect of this call - this module does
 *     not call metrics_write() or audit_trail_insert() itself.
 *   - execute_update_batch()  (OCI_Update_Execute_Module.h) to close
 *     sessions out (end / expiry / orphan reconciliation).  Same
 *     metrics/audit side effects as above.
 *   - execute_query_batch()   (OCI_Execute_Query_Module.h) used only
 *     by session_reconcile_orphans() to find candidate rows.
 *   - session_cache.h for the in-memory fast-path lookup.
 *
 * This module builds the <Insert_Template> / <Update_Template> XML
 * for the OCI_SESSION table itself (the column layout is fixed and
 * known at compile time - see Create_Session_Table.txt) rather than
 * calling get_insert_template() / metadata_cache_get_or_fetch(), which
 * exist to describe arbitrary caller-supplied tables.
 *
 * Session ID
 * ----------
 * Returned to the client in the CREATE_SESSION result XML.  The client
 * is expected to pass it back on every subsequent request; once the
 * HTTP input module replaces the XML test-file input, it will read the
 * session_id from the request and call session_validate() before
 * dispatching to any other module.  Today, Test_XML_Runner.c passes
 * the generated session_id into tx_begin() in place of the previous
 * "Session_id_from_client_stub" placeholder.
 *
 * Threading
 * ---------
 * Like every other module in the project, thread safety at the
 * oci_context_t level is the caller's responsibility. session_cache
 * itself is internally thread-safe (pthread_rwlock_t, inherited from
 * oci_cache).
 *
 * XML output format
 * -----------------
 *   session_create() success:
 *     <session>
 *       <operation>CREATE_SESSION</operation>
 *       <session_id>xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx</session_id>
 *       <status>ACTIVE</status>
 *       <created_ts>2026-07-05 14:22:01</created_ts>
 *       <ttl_seconds>1800</ttl_seconds>
 *     </session>
 *
 *   session_end() success:
 *     <session>
 *       <operation>END_SESSION</operation>
 *       <session_id>...</session_id>
 *       <status>LOGGED_OUT|EXPIRED</status>
 *       <closed_ts>2026-07-05 14:55:10</closed_ts>
 *     </session>
 *
 *   session_reconcile_orphans() logs a summary to ctx->session_logger;
 *   it does not return XML (there is no client waiting on a startup
 *   call), only a count via its out-parameter.
 *
 * config.ini parameters (see session_cache.h for the session_cache_*
 * keys; the remaining keys below are read directly by this module)
 * -----------------------------------------------------------------
 *   session_default_ttl_seconds   = 1800   # used when a request does
 *                                           # not specify a TTL
 *   session_log_create            = 1      # log CREATE events
 *   session_log_end               = 1      # log END/EXPIRE events
 *   session_log_reconcile         = 1      # log reconciliation events
 *
 * Integration
 * -----------
 * Add to app_config_t in ini_reader.h:
 *   int  session_default_ttl_seconds;
 *   int  session_log_create;
 *   int  session_log_end;
 *   int  session_log_reconcile;
 *   (plus the session_cache_* keys documented in session_cache.h)
 *
 * Add oci_context_t->session_cache (cache_t*) to OCI_Connection.h,
 * next to resultset_cache / metadata_cache.
 *
 * Compile additions
 * -----------------
 *   OCI_Session_Manager.c  session_cache.c
 */

#ifndef OCI_SESSION_MANAGER_H
#define OCI_SESSION_MANAGER_H

#include "OCI_Connection.h"
#include "session_cache.h"
#include "XML_Helper.h"
#include "logger.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Result codes                                                        */
/* ------------------------------------------------------------------ */
#define SESSION_OK                0
#define SESSION_ERR_INVALID_ARG  -1
#define SESSION_ERR_NOT_FOUND    -2   /* session_id not in cache        */
#define SESSION_ERR_EXPIRED      -3   /* found but past TTL             */
#define SESSION_ERR_DB_FAILURE   -4   /* execute_insert/update_batch failed */
#define SESSION_ERR_ALLOC        -5   /* malloc / calloc failure        */
#define SESSION_ERR_PARSE        -6   /* Session_Request XML malformed  */

/* ------------------------------------------------------------------ */
/*  session_request_t                                                    */
/*  Parsed from the <Session_Request> envelope (see                    */
/*  OCI_Test_Create_Session.xml).  All fields are caller-supplied and   */
/*  optional except operation; empty strings are stored as "-" in the   */
/*  permanent record, matching project convention elsewhere.           */
/* ------------------------------------------------------------------ */
typedef struct {
    char operation[32];            /* "CREATE_SESSION"                  */
    char client_id[128];
    char client_ip[64];
    char client_host[128];
    char application_name[128];
    int  requested_ttl_seconds;    /* 0 = use session_default_ttl_seconds */
} session_request_t;

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

/*
 * parse_session_request()
 *
 * Parse a <Session_Request> XML string into session_request_t.
 * Unrecognised or missing optional elements are left as empty
 * strings / 0.  Only <operation> is mandatory.
 *
 * Returns 0 on success, SESSION_ERR_PARSE if <operation> is missing.
 */
int parse_session_request(oci_context_t      *ctx,
                           const char         *input_xml,
                           session_request_t  *req);

/*
 * session_create()
 *
 * Create a new session.
 *
 *   - Generates a fresh UUID via tx_generate_uuid().
 *   - Builds an <Insert_Template> for OCI_SESSION and calls
 *     execute_insert_batch() (metrics + audit trail happen as a side
 *     effect of that call).
 *   - On successful insert, stores the session in ctx->session_cache.
 *   - Logs to ctx->session_logger.
 *   - Populates *result_xml with a heap-allocated XML string (see
 *     header comment for format).  Caller must free() it.
 *
 * Returns SESSION_OK on success, SESSION_ERR_DB_FAILURE if the insert
 * failed, SESSION_ERR_INVALID_ARG for NULL ctx/req.
 */
int session_create(oci_context_t            *ctx,
                    const session_request_t *req,
                    char                    **result_xml);

/*
 * session_validate()
 *
 * Fast-path check: is session_id present in session_cache and not
 * expired?  Does NOT touch the database.  A cache miss is treated as
 * invalid - the cache TTL is the source of truth for "is this session
 * still usable right now", not the permanent table (which exists for
 * durability, not for the hot path).
 *
 * On success (SESSION_OK), *out is populated with the cached record
 * fields.  Pass NULL for out if you only need the return code.
 *
 * Returns SESSION_OK, SESSION_ERR_NOT_FOUND, SESSION_ERR_INVALID_ARG.
 */
int session_validate(oci_context_t    *ctx,
                      const char       *session_id,
                      session_record_t *out);

/*
 * session_touch()
 *
 * Update last_activity_ts on a cached session to the current time,
 * extending its effective idle window.  Cache-only; does not write
 * through to the database on every call (LAST_ACTIVITY_TS in the
 * permanent table is refreshed only at session_end() / reconciliation
 * time from the last cached value).
 *
 * Returns SESSION_OK, SESSION_ERR_NOT_FOUND, SESSION_ERR_INVALID_ARG.
 */
int session_touch(oci_context_t *ctx, const char *session_id);

/*
 * session_end()
 *
 * Close a session out, either by explicit client logout or by TTL
 * expiry detected in the normal request path (as opposed to the
 * startup orphan sweep - see session_reconcile_orphans()).
 *
 *   - Builds an <Update_Template> for OCI_SESSION keyed on SESSION_ID,
 *     setting STATUS, CLOSED_TS, CLOSE_REASON, LAST_ACTIVITY_TS, and
 *     calls execute_update_batch() (metrics + audit as a side effect).
 *   - Invalidates the session_cache entry.
 *   - Logs to ctx->session_logger.
 *   - Populates *result_xml.  Caller must free() it.
 *
 * status must be SESSION_STATUS_EXPIRED or SESSION_STATUS_LOGGED_OUT;
 * any other value is rejected with SESSION_ERR_INVALID_ARG.
 *
 * Returns SESSION_OK, SESSION_ERR_DB_FAILURE, SESSION_ERR_INVALID_ARG.
 */
int session_end(oci_context_t    *ctx,
                 const char       *session_id,
                 session_status_t  status,
                 const char       *reason,
                 char            **result_xml);

/*
 * session_reconcile_orphans()
 *
 * Startup crash-recovery sweep.  Intended to be called once, after
 * OCI_Connect() / session_cache_init() and before any client traffic
 * is served.
 *
 *   - Queries OCI_SESSION (via execute_query_batch()) for rows where
 *     STATUS = 'ACTIVE' AND CLOSED_TS IS NULL AND the TTL window has
 *     already elapsed (CREATED_TS + TTL_SECONDS < now).
 *   - For each match, calls the same update path as session_end(),
 *     setting STATUS = 'EXPIRED_ORPHAN' and a CLOSE_REASON explaining
 *     this was a startup reconciliation, not a normal expiry.
 *   - Does NOT attempt to restore or infer any other missing session
 *     data - closing the gap is the entire scope of this function.
 *   - Logs a summary line (rows found / rows closed / failures) to
 *     ctx->session_logger regardless of session_log_reconcile so the
 *     operator always sees startup recovery activity.
 *
 * Parameters
 *   ctx              - OCI context (connection + loggers)
 *   orphan_count      - out: number of rows found and closed (may be
 *                        NULL if the caller only needs the return code)
 *
 * Returns SESSION_OK in essentially all cases, including when zero
 * orphans are found (the normal outcome) and when the reconciliation
 * query itself fails or is rejected for any reason - reconciliation is
 * a best-effort startup nicety, not a path worth blocking application
 * startup over, and it retries automatically on the next startup.
 * Failure to close an individual orphan row (once genuinely found) is
 * likewise logged but does not fail the whole sweep.
 */
int session_reconcile_orphans(oci_context_t *ctx, int *orphan_count);

/*
 * session_status_from_enum_str() is provided by session_cache.h as
 * session_status_str() / session_status_from_str() - re-declared here
 * only in comment form so callers of this header know where to find
 * it without an extra include search.
 */

#ifdef __cplusplus
}
#endif

#endif /* OCI_SESSION_MANAGER_H */
