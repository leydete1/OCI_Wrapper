/*
 * OCI_Auth_Manager.h
 *
 * Authentication Manager - Public Interface
 * ------------------------------------------
 * See Security_Module_Design_Specification.docx, Section 6.3, for the
 * full design description. This header owns authenticate_request_t -
 * the OP_AUTHENTICATE payload struct - per the convention documented
 * in OCI_Request_Response_Types.h's own comment on
 * input_c_operation_t.payload (each operation type's concrete struct
 * lives in that operation's own module header, not in the shared
 * types file).
 *
 * Stage 2 (2026-08-27): LOCAL authentication source only. A user whose
 * APP_USER.AUTH_SOURCE_ID resolves to SOURCE_TYPE = 'LDAP' or 'AD' is
 * currently rejected with AUTH_ERR_DENIED (logged clearly as
 * "LDAP/AD not yet implemented" internally, but never revealed to the
 * caller as anything other than a generic denial) - see auth_
 * authenticate()'s own doc comment below. Delegated LDAP/AD
 * authentication is Stage 3.
 */

#ifndef OCI_AUTH_MANAGER_H
#define OCI_AUTH_MANAGER_H

#include "OCI_Connection.h"   /* oci_context_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Error codes                                                         */
/* ------------------------------------------------------------------ */
#define AUTH_OK                       0
#define AUTH_ERR_INVALID_ARG         -1
#define AUTH_ERR_DENIED              -2  /* bad credential, unknown user,
                                           * disabled, or locked -
                                           * deliberately not distinguished
                                           * to the caller (Security Module
                                           * Design Specification, Section 5) */
#define AUTH_ERR_SOURCE_UNAVAILABLE  -3  /* LDAP/AD source unreachable -
                                           * not produced yet in Stage 2 */
#define AUTH_ERR_DB_FAILURE          -4
#define AUTH_ERR_ALLOC               -5

/* ------------------------------------------------------------------ */
/*  authenticate_request_t                                               */
/*  Payload for OP_AUTHENTICATE. Filled by Level 2 parsing exactly as   */
/*  select_request_t / insert_request_t are for their own operation     */
/*  types.                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    char username[128];
    char credential[256];    /* password; caller (dispatcher) does not
                               * need to zero this - auth_authenticate()
                               * only ever reads it, and the enclosing
                               * input_c_operation_t/payload is freed by
                               * the same per-request cleanup path every
                               * other operation's payload already goes
                               * through.                                */
} authenticate_request_t;

/*
 * auth_authenticate()
 *
 * Looks up APP_USER by username (case-insensitive, matching the
 * UPPER(USERNAME) unique index), verifies the supplied credential
 * against PASSWORD_HASH using Argon2id (crypt_verify_password(),
 * crypt_helper.h - Stage 1) for LOCAL-source users, applies lockout
 * policy on failure, and on success calls session_create()
 * (OCI_Session_Manager.h, unchanged) and returns its session_id /
 * ttl_seconds along with the user's display_name.
 *
 * A single generic AUTH_ERR_DENIED covers: unknown username, disabled
 * account, locked account, wrong password, and (Stage 2 only) a
 * non-LOCAL auth source - never distinguished to the caller, to avoid
 * username enumeration. The specific reason is always logged
 * internally via ctx->security_logger.
 *
 * *session_id_out and *display_name_out are heap-allocated and
 * populated only on AUTH_OK - caller must free() both. Left NULL on
 * every other return code. *ttl_seconds_out is left untouched on
 * failure.
 *
 * Returns AUTH_OK, AUTH_ERR_INVALID_ARG, AUTH_ERR_DENIED, or
 * AUTH_ERR_DB_FAILURE.
 */
int auth_authenticate(oci_context_t                 *ctx,
                       const authenticate_request_t  *req,
                       char                         **session_id_out,
                       char                         **display_name_out,
                       int                            *ttl_seconds_out);

#ifdef __cplusplus
}
#endif

#endif /* OCI_AUTH_MANAGER_H */
