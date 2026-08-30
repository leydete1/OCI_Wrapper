/*
 * ldap_bind_helper.c
 *
 * Standalone LDAP bind-check helper - Security Module Stage 3.
 *
 * WHY THIS IS A SEPARATE EXECUTABLE, NOT A FUNCTION INSIDE OCI_WRAPPER:
 * Three separate attempts to make OpenLDAP and Oracle's Instant Client
 * (libclntsh.so) coexist safely in the SAME process all failed, each
 * one uncovering a deeper problem than the last:
 *   1. #include <ldap.h> resolved to Oracle's own bundled ldap.h
 *      (compile-time header collision).
 *   2. Even with the right header, libclntsh.so exports its own
 *      ldap_sasl_bind_s()/ldap_unbind_s() symbols, silently hijacking
 *      calls to real OpenLDAP's (link-time symbol collision).
 *   3. Even calling through explicit dlsym()-resolved pointers,
 *      libldap.so.2's OWN INTERNAL calls to its ber_* BER-encoding
 *      layer still collided with libclntsh.so's own colliding ber_*
 *      exports (dlopen()'s RTLD_DEEPBIND fixed this, but RTLD_DEEPBIND
 *      is documented as incompatible with AddressSanitizer, which
 *      this whole project is built with, and ASan aborted the process
 *      outright).
 *   4. dlmopen(LM_ID_NEWLM, ...) - a separate link-map namespace -
 *      fixed the symbol collision without needing RTLD_DEEPBIND, but
 *      introduced a genuine, different crash: libldap's own internals
 *      segfaulted under real concurrent multi-threaded use, a known
 *      general pitfall of dlmopen() and thread-local storage, not an
 *      ASan-specific issue this time.
 *
 * Every one of those was a real fix for the specific bug it targeted -
 * the underlying problem is that Oracle's client library and OpenLDAP
 * cannot safely share ONE process's address space AT ALL when both
 * are exercised concurrently. The only fix left that doesn't just
 * find a fifth, deeper collision is to stop sharing the address space:
 * this helper links ONLY against -lldap, never touches Oracle's
 * client library at all, and OCI_Wrapper talks to it as a completely
 * separate OS process (fork()/exec(), via ldap_auth_helper.c).
 *
 * Usage:
 *   ldap_bind_helper <ldap_url> <bind_dn>
 *   (password is read from stdin, one line, NOT passed as an argv
 *   argument - argv is visible to any local user via `ps`/`/proc`;
 *   stdin is not)
 *
 * Exit codes:
 *   0 - bind succeeded (credential is correct)
 *   1 - bind failed (wrong credential, unreachable server, or any
 *       other LDAP-level failure) - a one-line reason is printed to
 *       stdout for the parent process to log internally
 *   2 - usage error (wrong argc, or failed to read password from
 *       stdin) - not an LDAP failure at all, a caller mistake
 *
 * Build (separate from OCI_Wrapper's own build entirely):
 *   gcc -O2 -Wall -o ldap_bind_helper ldap_bind_helper.c -lldap
 */

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <ldap.h>          /* plain #include - no header collision here,
                            * this process never links Oracle's client
                            * library, so there's nothing to collide
                            * with (see this file's own header comment) */

#define LDAP_BIND_HELPER_TIMEOUT_SECONDS 5

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <ldap_url> <bind_dn>\n", argv[0]);
        fprintf(stderr, "  (password is read from stdin, one line)\n");
        return 2;
    }

    const char *ldap_url = argv[1];
    const char *bind_dn  = argv[2];

    char password[512];
    if (!fgets(password, sizeof(password), stdin))
    {
        fprintf(stderr, "failed to read password from stdin\n");
        return 2;
    }
    /* Strip the trailing newline fgets() leaves in place. */
    size_t plen = strlen(password);
    if (plen > 0 && password[plen - 1] == '\n')
        password[plen - 1] = '\0';

    LDAP *ld = NULL;
    int rc = ldap_initialize(&ld, ldap_url);
    if (rc != LDAP_SUCCESS || !ld)
    {
        printf("ldap_initialize() failed - malformed ldap_url\n");
        return 1;
    }

    int version = LDAP_VERSION3;
    ldap_set_option(ld, LDAP_OPT_PROTOCOL_VERSION, &version);

    struct timeval net_timeout;
    net_timeout.tv_sec  = LDAP_BIND_HELPER_TIMEOUT_SECONDS;
    net_timeout.tv_usec = 0;
    ldap_set_option(ld, LDAP_OPT_NETWORK_TIMEOUT, &net_timeout);

    struct berval cred;
    cred.bv_val = password;
    cred.bv_len = strlen(password);

    /* Zero the password out of our own memory as soon as it's handed
     * to the bind call - it's still readable in libldap's own copy
     * for the duration of the call, but this process exits
     * immediately after, so the exposure window is as small as this
     * design allows.                                                 */
    rc = ldap_sasl_bind_s(ld, bind_dn, LDAP_SASL_SIMPLE, &cred,
                           NULL, NULL, NULL);
    memset(password, 0, sizeof(password));

    if (rc != LDAP_SUCCESS)
    {
        printf("%s\n", ldap_err2string(rc));
        ldap_unbind_ext_s(ld, NULL, NULL);
        return 1;
    }

    ldap_unbind_ext_s(ld, NULL, NULL);
    return 0;
}
