/*
 * crypt_helper.h
 *
 * Password Hashing Helper - Public Interface
 * ---------------------------------------------
 * Stage 1 of the Security Module (Security_Module_Design_
 * Specification.docx, Section 6.5). Thin wrapper around libsodium's
 * crypto_pwhash_str() / crypto_pwhash_str_verify() (Argon2id) - this
 * is the piece that turns "libsodium is linked" into "there is a
 * function that hashes a password", which did not previously exist.
 *
 * Deliberately its own small module, not folded into OCI_Auth_
 * Manager.c: hashing/verifying a password has no OCI dependency at
 * all (no oci_context_t->svchp/errhp use), and a future admin tool for
 * provisioning APP_USER rows will need crypt_hash_password() without
 * wanting the rest of Auth Manager's DB-lookup machinery pulled in
 * with it.
 */

#ifndef CRYPT_HELPER_H
#define CRYPT_HELPER_H

#include "OCI_Connection.h"   /* oci_context_t - ctx->crypt_logger, ctx->ini */

#ifdef __cplusplus
extern "C" {
#endif

#define CRYPT_OK               0
#define CRYPT_ERR_INVALID_ARG -1
#define CRYPT_ERR_HASH_FAILED -2   /* crypto_pwhash_str() itself failed -
                                    * almost always out of memory at the
                                    * configured cost parameters        */

/*
 * crypt_init()
 *
 * Calls libsodium's sodium_init() exactly once for the process.
 * MUST be called before crypt_hash_password() / crypt_verify_password()
 * (or any other libsodium function anywhere in the codebase) is ever
 * called - libsodium is explicit that its functions are not safe to
 * use before this. Idempotent and thread-safe by libsodium's own
 * design (safe to call more than once, safe to call from multiple
 * threads), but call it once from main()/bootstrap during startup,
 * before any worker thread exists, so no thread is ever racing this
 * one-time setup.
 *
 * Returns CRYPT_OK on success, CRYPT_ERR_HASH_FAILED if sodium_init()
 * itself reported failure (sodium_init() returns -1 on failure, 0 on
 * first success, 1 if already initialised - both 0 and 1 are success
 * here).
 */
int crypt_init(void);

/*
 * crypt_hash_password()
 *
 * Hashes password into a libsodium-encoded string (algorithm,
 * version, cost parameters, and salt are all embedded in the string
 * itself by crypto_pwhash_str() - nothing else needs to be stored
 * alongside it). This is APP_USER.PASSWORD_HASH's value directly
 * (Security_Module_Design_Specification.docx Section 4.2) -
 * PASSWORD_PARAMS stays unused/NULL for now, per that section's own
 * note.
 *
 * Cost parameters come from ctx->ini->auth_argon2_mem_cost_kb /
 * auth_argon2_time_cost (ini_reader.h, config.ini) - not hardcoded -
 * so changing config.ini changes the cost of every password hashed
 * from that point on without a code change. Existing hashes keep
 * verifying correctly regardless of later config changes, since their
 * own cost parameters travel with them inside the hash string.
 *
 * hash_out must be at least 128 bytes (crypto_pwhash_STRBYTES from
 * libsodium's own header, no application call needed to know this -
 * it doesn't change per-hash, only crypto_pwhash_str()'s current
 * default algorithm ever changes it, and 128 has been that constant
 * for every algorithm version libsodium has shipped).
 *
 * Returns CRYPT_OK, CRYPT_ERR_INVALID_ARG (NULL ctx/password/hash_out,
 * or hash_out_size too small), or CRYPT_ERR_HASH_FAILED.
 */
int crypt_hash_password(oci_context_t *ctx, const char *password,
                         char *hash_out, size_t hash_out_size);

/*
 * crypt_verify_password()
 *
 * Verifies password against hash (as produced by crypt_hash_password()
 * above, or by anything else using crypto_pwhash_str()'s output
 * format). Constant-time by libsodium's own design - no separate
 * timing-safe-compare step is needed or added here.
 *
 * Returns CRYPT_OK if the password matches, CRYPT_ERR_INVALID_ARG for
 * NULL ctx/password/hash, or CRYPT_ERR_HASH_FAILED for a genuine
 * mismatch (libsodium does not distinguish "wrong password" from
 * "malformed hash string" in its own return code, and neither does
 * this wrapper - the caller, OCI_Auth_Manager.c's auth_authenticate(),
 * already folds any verification failure into the same generic
 * AUTH_ERR_DENIED regardless).
 */
int crypt_verify_password(oci_context_t *ctx, const char *password,
                           const char *hash);

#ifdef __cplusplus
}
#endif

#endif /* CRYPT_HELPER_H */
