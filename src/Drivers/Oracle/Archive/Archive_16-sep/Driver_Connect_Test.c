/*
 * Driver_Connect_Test.c
 *
 * Validates db_driver_t's connect()/disconnect() against the existing,
 * known-good OCI_Connect()/OCI_Disconnect() path - order-of-attack step
 * 4 from DB_Driver_Abstraction_Notes.md: "Validate against the existing
 * connect/select test fixtures - same external behavior, different
 * internal wiring." Modelled directly on Level2_Insert_Test.c's own
 * init sequence (same config.ini load, same minimal logger set) so this
 * is a fair comparison against a pattern already proven live.
 *
 * What it does:
 *   Round A - connect via the existing direct call:  OCI_Connect(&ctx_a)
 *   Round B - connect via the new interface:         driver->connect(&ctx_b)
 *   Both use a fresh, non-pooled ctx (pool_slot_index = -1), same
 *   config.ini, same credentials - so success/failure and timing should
 *   be equivalent between the two. Round B additionally confirms
 *   ctx_b's OCI handles (envhp/errhp/srvhp/svchp) actually got
 *   populated, not just that connect() returned 0 - a wrapper that
 *   silently no-ops would still return 0 and this check would catch it.
 *
 * This does NOT test execute_select - that function pointer is NULL in
 * this pass (see driver_oracle.c). Only connect/disconnect are in
 * scope here.
 *
 * Build (adjust -I/-L paths to match your OCI Instant Client install -
 * same libraries as Level2_Insert_Test.c's own build line, minus the
 * parser/validate/metadata modules this test doesn't need, plus the
 * three new driver files):
 *
 *   gcc -o Driver_Connect_Test \
 *       Driver_Connect_Test.c \
 *       db_driver.c \
 *       driver_oracle.c \
 *       OCI_Connection.c \
 *       oci_cache.c \
 *       string_utils.c \
 *       ini_reader.c \
 *       logger.c \
 *       -I. -lclntsh -lpthread -lm
 *
 * Run - no arguments needed:
 *   ./Driver_Connect_Test
 *
 * Expected output on success:
 *   Round A (direct OCI_Connect)  ... OK
 *   Round B (via db_driver_t)     ... OK
 *   Round B handle check          ... OK (envhp/errhp/srvhp/svchp populated)
 *   PASS
 *
 * A non-zero exit code means at least one round failed - check
 * config.connection_log_file_name (same log file both rounds write to)
 * for the OCI error detail CHECK_OCI already logs.
 */

#define _POSIX_C_SOURCE 200809L

/* Same hardcoded path convention as Level2_Insert_Test.c - update and
 * rebuild if it ever moves. */
#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

#include <stdio.h>
#include <string.h>

#include "OCI_Connection.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

/* Shared init - identical steps Level2_Insert_Test.c uses, trimmed to
 * only the logger this test actually touches (error/connection). ctx
 * is zeroed and wired to config first, same order as the existing
 * harness, so behavior differences can only come from connect() itself,
 * not from init drift between the two rounds. */
static int init_ctx(oci_context_t *ctx, app_config_t *config,
                     logger_t *error_logger, logger_t *connection_logger)
{
    memset(ctx,    0, sizeof(*ctx));
    memset(config, 0, sizeof(*config));
    ctx->ini             = config;
    ctx->pool_slot_index = -1;

    if (load_ini(CONFIG_INI, config, ctx) != 0)
    {
        fprintf(stderr, "Failed to load ini file: %s\n", CONFIG_INI);
        return -1;
    }

    if (logger_init_str(error_logger, config->error_log_file_name,
                         config->error_log_file_max_size,
                         config->error_log_file_rotation_number,
                         config->error_log_level) != 0)
    {
        fprintf(stderr, "Failed to init error_logger\n");
        return -1;
    }
    ctx->error_logger = error_logger;

    if (logger_init_str2(connection_logger, config->connection_log_file_name,
                          config->connection_log_file_max_size,
                          config->connection_log_file_rotation_number,
                          config->connection_log_level, ctx->error_logger) != 0)
    {
        fprintf(stderr, "Failed to init connection_logger\n");
        return -1;
    }
    ctx->connection_logger = connection_logger;

    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failed = 0;

    /* ---- Round A: direct OCI_Connect(), the known-good path ---- */
    oci_context_t ctx_a; app_config_t config_a;
    logger_t err_a, conn_a;
    printf("Round A (direct OCI_Connect)  ... ");
    if (init_ctx(&ctx_a, &config_a, &err_a, &conn_a) != 0) { printf("INIT FAILED\n"); return 1; }
    if (OCI_Connect(&ctx_a) != 0) { printf("FAILED - see %s\n", config_a.connection_log_file_name); failed = 1; }
    else { printf("OK\n"); OCI_Disconnect(&ctx_a); }

    /* ---- Round B: via db_driver_t ---- */
    oci_context_t ctx_b; app_config_t config_b;
    logger_t err_b, conn_b;
    printf("Round B (via db_driver_t)     ... ");
    if (init_ctx(&ctx_b, &config_b, &err_b, &conn_b) != 0) { printf("INIT FAILED\n"); return 1; }

    const db_driver_t *driver = db_driver_get(&ctx_b);
    if (!driver || !driver->connect || !driver->disconnect)
    {
        printf("FAILED - db_driver_get() returned an incomplete driver\n");
        return 1;
    }

    if (driver->connect(&ctx_b) != 0)
    {
        printf("FAILED - see %s\n", config_b.connection_log_file_name);
        failed = 1;
    }
    else
    {
        printf("OK\n");

        /* connect() dispatches internally on ctx->ini->use_connection_pool
         * (see driver_oracle.c) - when pooled, it calls OCI_Connect_pool(),
         * which populates ctx->pool_handle only and deliberately leaves
         * envhp/errhp/srvhp/svchp NULL on the master ctx (those only get
         * set on a worker ctx via get_session() - see Driver_Pool_Test.c,
         * which already checks that path). This check used to assume
         * connect() always meant direct-mode handles; that stopped being
         * universally true the moment pooled dispatch was added here, not
         * a regression in connect() itself - config.ini's own
         * use_connection_pool value decides which shape to expect. */
        printf("Round B handle check          ... ");
        if (config_b.use_connection_pool)
        {
            if (ctx_b.pool_handle)
                printf("OK (pool_handle populated, pooled mode)\n");
            else
            {
                printf("FAILED - connect() returned 0 but pool_handle is NULL\n");
                failed = 1;
            }
        }
        else if (ctx_b.envhp && ctx_b.errhp && ctx_b.srvhp && ctx_b.svchp)
            printf("OK (envhp/errhp/srvhp/svchp populated)\n");
        else
        {
            printf("FAILED - connect() returned 0 but handles are NULL\n");
            failed = 1;
        }

        driver->disconnect(&ctx_b);
    }

    printf("%s\n", failed ? "FAIL" : "PASS");

    /* ---- Cleanup - closes the loggers LeakSanitizer will otherwise
     * flag (logger_init and logger_init_str variants strdup the file
     * paths and never free them until logger_close() is called). Same
     * pattern and same
     * reasoning as Level2_Insert_Test.c's own cleanup section: this
     * does NOT chase the separate OCIEnvCreate/OPENSSL_cleanup/
     * ___pthread_once leaks reported inside libclntsh.so/libnnz.so -
     * those are Oracle Instant Client's own one-time process-lifetime
     * global init, not anything allocated here, and are already
     * handled by lsan_suppressions.txt (see "Suppressions used" in the
     * run output). Only the four loggers this file itself created are
     * this file's responsibility.                                     */
    logger_close(&err_a);
    logger_close(&conn_a);
    logger_close(&err_b);
    logger_close(&conn_b);

    return failed ? 1 : 0;
}
