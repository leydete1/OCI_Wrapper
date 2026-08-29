/*
 * crypt_helper.c
 *
 * Password Hashing Helper - Implementation
 * ---------------------------------------------
 * See crypt_helper.h for the full design description. Stage 1 of the
 * Security Module (2026-08-27) - the actual hash/verify calls that
 * "libsodium is linked" was standing in for until now.
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <sodium.h>

#include "crypt_helper.h"
#include "logger.h"
#include "ini_reader.h"

int crypt_init(void)
{
    /* sodium_init(): -1 = failure, 0 = first successful init,
     * 1 = already initialised - both 0 and 1 are fine here.          */
    return (sodium_init() < 0) ? CRYPT_ERR_HASH_FAILED : CRYPT_OK;
}

int crypt_hash_password(oci_context_t *ctx, const char *password,
                         char *hash_out, size_t hash_out_size)
{
    if (!ctx || !password || !password[0] || !hash_out ||
        hash_out_size < crypto_pwhash_STRBYTES)
        return CRYPT_ERR_INVALID_ARG;

    /* Config-driven cost, not hardcoded - see this function's own doc
     * comment in crypt_helper.h. libsodium enforces its own sane
     * minimums internally (crypto_pwhash_OPSLIMIT_MIN /
     * crypto_pwhash_MEMLIMIT_MIN) - a config.ini value set absurdly
     * low fails the crypto_pwhash_str() call below rather than
     * silently producing a weak-but-"successful" hash.                */
    unsigned long long opslimit = (unsigned long long)ctx->ini->auth_argon2_time_cost;
    size_t             memlimit = (size_t)ctx->ini->auth_argon2_mem_cost_kb * 1024UL;

    int rc = crypto_pwhash_str(hash_out, password, strlen(password),
                                opslimit, memlimit);
    if (rc != 0)
    {
        logger_write(ctx->crypt_logger, LOG_ERROR, __func__, 0,
                     "crypto_pwhash_str() failed (opslimit=%llu "
                     "memlimit=%zu bytes) - likely insufficient memory "
                     "at the configured auth_argon2_mem_cost_kb",
                     opslimit, memlimit);
        return CRYPT_ERR_HASH_FAILED;
    }

    return CRYPT_OK;
}

int crypt_verify_password(oci_context_t *ctx, const char *password,
                           const char *hash)
{
    if (!ctx || !password || !password[0] || !hash || !hash[0])
        return CRYPT_ERR_INVALID_ARG;

    int rc = crypto_pwhash_str_verify(hash, password, strlen(password));
    return (rc == 0) ? CRYPT_OK : CRYPT_ERR_HASH_FAILED;
}
