/*
 * ldap_auth_helper.h
 *
 * Delegated LDAP/AD Bind Check - Public Interface
 * ---------------------------------------------------
 * Stage 3 of the Security Module (Security_Module_Design_
 * Specification.docx, Section 5 - "Delegated authentication against
 * LDAP/AD via an LDAP bind").
 *
 * IMPORTANT - why this is its own module:
 * This project already ships an ldap.h (Oracle's own internal LDAP
 * header, used for OCI directory-naming resolution - see the ldap.h
 * already in this codebase). That is NOT the OpenLDAP client library
 * this file needs. Isolating the real OpenLDAP <ldap.h> to its own
 * translation unit (ldap_auth_helper.c never includes oci.h/
 * OCI_Connection.h or anything that pulls in Oracle's ldap.h) means
 * no other file in this codebase is ever at risk of the collision -
 * but it is NOT sufficient on its own: this project's Eclipse-
 * generated build applies the same -I list (which includes the
 * Oracle Instant Client SDK's own include dir) to every .c file it
 * compiles, and that SDK also ships its own ldap.h, ahead of
 * /usr/include in search order. ldap_auth_helper.c therefore includes
 * the real system ldap.h via its absolute path rather than a plain
 * #include <ldap.h> - see the top of ldap_auth_helper.c for why, and
 * the build log this fixed (initial build resolved to Oracle's
 * ldap.h despite the isolation, confirmed by "implicit declaration of
 * ldap_initialize" and an ldap_sasl_bind_s signature mismatch).
 *
 * This header (the only thing OCI_Auth_Manager.c sees) exposes zero
 * LDAP-specific types - plain strings and an int result only.
 *
 * Build requirement: libldap2-dev must be installed (provides the
 * real system <ldap.h> and libldap to link against - add -lldap to
 * the link step, same --start-group as -lsodium etc.).
 */

#ifndef LDAP_AUTH_HELPER_H
#define LDAP_AUTH_HELPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LDAP_AUTH_BIND_OK           0
#define LDAP_AUTH_BIND_INVALID_ARG -1
#define LDAP_AUTH_BIND_FAILED      -2  /* wrong credential OR unreachable
                                         * server OR malformed DN - see
                                         * err_buf for which, but the
                                         * caller (auth_authenticate())
                                         * folds all of these into the
                                         * same generic AUTH_ERR_DENIED
                                         * regardless, per Security_
                                         * Module_Design_Specification
                                         * .docx Section 5.              */

/*
 * ldap_auth_bind_check()
 *
 * Attempts an LDAP simple bind as bind_dn/password against ldap_url.
 * This is the entire check - a successful bind IS the proof of a
 * correct credential, exactly as a real LDAP/AD client authenticates.
 * No search, no group lookup - authentication only, per this
 * project's authorization model being entirely local (Security_
 * Module_Design_Specification.docx Section 2/5 - LDAP/AD group
 * membership is explicitly out of scope).
 *
 * ldap_url: e.g. "ldap://localhost:389" or "ldaps://dc01.corp.local:636"
 * bind_dn:  the fully-resolved DN to bind as, e.g.
 *           "uid=jsmith,ou=people,dc=example,dc=com" (OpenLDAP-style)
 *           or "jsmith@corp.local" (AD UPN-style) - caller
 *           (auth_authenticate(), OCI_Auth_Manager.c) builds this from
 *           AUTH_SOURCE.CONFIGURATION's bind_dn_pattern + username.
 * password: the credential to bind with, as supplied by the client.
 *
 * err_buf/err_buf_size: on LDAP_AUTH_BIND_FAILED, populated with a
 * short internal-logging-only description (e.g. "invalid credentials"
 * vs "can't contact LDAP server") - never surfaced to the end caller
 * of AUTHENTICATE, only written to ctx->security_logger by
 * auth_authenticate(). Safe to pass NULL/0 to skip.
 *
 * Uses a fixed network timeout (LDAP_AUTH_BIND_TIMEOUT_SECONDS below)
 * so a stalled/unreachable directory server can't block a worker
 * thread indefinitely.
 *
 * Returns LDAP_AUTH_BIND_OK, LDAP_AUTH_BIND_INVALID_ARG, or
 * LDAP_AUTH_BIND_FAILED.
 */
int ldap_auth_bind_check(const char *ldap_url,
                          const char *bind_dn,
                          const char *password,
                          char *err_buf, size_t err_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* LDAP_AUTH_HELPER_H */
