/*
 * OCI_Authz_Manager.h
 *
 * Authorization Manager - Public Interface (Security Module Stage 5)
 * ---------------------------------------------------------------------
 * See Security_Module_Design_Specification.docx Section 6.4/6.6 for
 * the full design description. This header owns check_permission_
 * request_t - the OP_CHECK_PERMISSION payload struct - per the same
 * convention OCI_Auth_Manager.h follows for authenticate_request_t
 * (each operation type's concrete struct lives in that operation's
 * own module header).
 *
 * Authorization in this project is entirely local (USER -> USER_ROLE
 * -> ROLE -> ROLE_PERMISSION -> PERMISSION) - LDAP/AD group membership
 * is never consulted for authorization decisions, only for the
 * identity check itself (OCI_Auth_Manager.c, Stage 3). Roles are a
 * convenience grouping for administrators; the actual authorization
 * decision made by this module is always a permission check, never a
 * role-name comparison.
 */

#ifndef OCI_AUTHZ_MANAGER_H
#define OCI_AUTHZ_MANAGER_H

#include "OCI_Connection.h"   /* oci_context_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Error codes                                                         */
/* ------------------------------------------------------------------ */
#define AUTHZ_OK                   0
#define AUTHZ_ERR_INVALID_ARG     -1
#define AUTHZ_ERR_DENIED          -2   /* authenticated (or the session_id
                                         * looked valid at cache-build time),
                                         * but lacks the permission - also
                                         * returned for an unknown/expired
                                         * session_id, deliberately not
                                         * distinguished from a real denial
                                         * (see authz_has_permission()'s own
                                         * doc comment on why)              */
#define AUTHZ_ERR_DB_FAILURE      -4   /* only returned by authz_build_
                                         * permission_cache() - authz_has_
                                         * permission() itself never touches
                                         * the database, so it never returns
                                         * this code                        */

/* ------------------------------------------------------------------ */
/*  check_permission_request_t                                          */
/*  Payload for OP_CHECK_PERMISSION. Filled by Level 2 parsing exactly  */
/*  as authenticate_request_t is for OP_AUTHENTICATE. session_id is     */
/*  NOT part of this struct - it comes from the envelope's own          */
/*  input_c_request_t.session_id field, matching every other operation  */
/*  type's existing session handling, not duplicated per-operation.    */
/* ------------------------------------------------------------------ */
typedef struct {
    char permission_code[100];
} check_permission_request_t;

/*
 * authz_build_permission_cache()
 *
 * Queries USER_ROLE -> ROLE_PERMISSION -> PERMISSION for user_id (a
 * single join, no pagination - a user's total permission count is not
 * expected to be large enough to need it), builds a comma-separated
 * PERMISSION_CODE list, and stores it in ctx->authz_cache keyed by
 * session_id via authz_cache_store() (authz_cache.h).
 *
 * Called once, by auth_authenticate() (OCI_Auth_Manager.c), right
 * after a successful session_create() - this is what Security_Module_
 * Design_Specification.docx Section 6.6 means by "built once per
 * session at session_create() time". A permission change made by an
 * administrator takes effect on the user's NEXT session, not mid-
 * session, by design - this function is never called again for an
 * existing session_id.
 *
 * A user with zero granted permissions is a valid state, not an
 * error - an empty list is still cached (every authz_has_permission()
 * call for that session then correctly returns AUTHZ_ERR_DENIED).
 *
 * This function's own failure is deliberately NOT allowed to turn a
 * successful authentication into a denial - matching auth_authenticate()
 * ' record_auth_success()'s own "best-effort bookkeeping" philosophy.
 * A DB failure here is logged via ctx->security_logger, and the caller
 * proceeds with the login regardless; the resulting session simply has
 * no cached permissions (every authz_has_permission() call denies)
 * until the user logs in again.
 *
 * Returns AUTHZ_OK, AUTHZ_ERR_INVALID_ARG, or AUTHZ_ERR_DB_FAILURE.
 */
int authz_build_permission_cache(oci_context_t *ctx,
                                  const char    *session_id,
                                  int            user_id);

/*
 * authz_has_permission()
 *
 * Cache-only permission check - does NOT touch the database. Looks up
 * session_id in ctx->authz_cache (built once at login time by authz_
 * build_permission_cache()) and checks whether permission_code appears
 * in the cached list.
 *
 * An unknown/expired/never-built session_id and a genuinely denied
 * permission both return the same AUTHZ_ERR_DENIED - deliberately not
 * distinguished, same anti-enumeration reasoning auth_authenticate()
 * already applies to AUTH_ERR_DENIED (Security_Module_Design_
 * Specification.docx Section 5): a caller probing with a stale
 * session_id should not be able to tell that apart from a valid
 * session that's simply missing one permission.
 *
 * Returns AUTHZ_OK if permission_code is granted, AUTHZ_ERR_DENIED
 * otherwise (including any lookup/cache-miss case), or
 * AUTHZ_ERR_INVALID_ARG for NULL ctx/session_id/permission_code.
 */
int authz_has_permission(oci_context_t *ctx,
                          const char    *session_id,
                          const char    *permission_code);

#ifdef __cplusplus
}
#endif

#endif /* OCI_AUTHZ_MANAGER_H */
