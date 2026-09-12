/*
 * Driver_Pool_Test.c
 *
 * Validates db_driver_t's pooled surface against the existing,
 * known-good OCI_Connect_pool()/OCI_Pool_* path - same order-of-attack
 * step 4 reasoning as Driver_Connect_Test.c, extended to cover the path
 * that is actually live in production (config.ini has
 * use_connection_pool=1 - see Driver_Connect_Test.c's own run log).
 *
 * What it does:
 *   Round A - existing direct calls: OCI_Connect_pool(&ctx_a),
 *             OCI_Pool_get_session(&ctx_a, &worker_a),
 *             OCI_Pool_session_is_alive(&worker_a),
 *             OCI_Pool_release_session(&ctx_a, &worker_a),
 *             OCI_Disconnect_pool(&ctx_a).
 *   Round B - same sequence via db_driver_t: driver->connect(&ctx_b)
 *             (dispatches internally to OCI_Connect_pool because
 *             config.ini's use_connection_pool=1 - see driver_oracle.c),
 *             driver->get_session/session_is_alive/release_session/
 *             disconnect.
 *   Round B additionally confirms worker_b's OCI handles
 *   (envhp/errhp/srvhp/svchp) actually got populated by get_session(),
 *   and calls driver->health_check() once after release as a sanity
 *   check on the now-freed slot (expects 0 unrecoverable slots).
 *
 * This does NOT test reconnect_session() - that is a mid-loop repair
 * path for an already-borrowed, now-unhealthy session (see
 * OCI_Connection_Pool.h's own doc comment); exercising it meaningfully
 * needs a session to actually go bad first, which is a separate,
 * deliberately-induced-failure test, not part of this pass.
 *
 * This does NOT test execute_select - still NULL in this pass (see
 * driver_oracle.c).
 *
 * Build (same convention as Driver_Connect_Test.c/Build.sh - identical
 * -I/-L set, this file just replaces the previous .c as the compiled
 * unit; OCI_Connection_Pool.c is new here since Driver_Connect_Test.c
 * never needed it):
 *
 *   gcc -I/home/leyden100/eclipse-workspace/OCI_Wrapper/oci/instantclient-sdk-linux.x64-23.26.1.0.0/instantclient_23_26/sdk/include \
 *       -I/usr/include/cjson -I/usr/include/libxml2 \
 *       -I/home/leyden100/eclipse-workspace/OCI_Wrapper/include -I. \
 *       -O0 -g3 -Wall -fmessage-length=0 -fsanitize=address -fno-omit-frame-pointer \
 *       -o Driver_Pool_Test \
 *       Driver_Pool_Test.c db_driver.c driver_oracle.c \
 *       OCI_Connection.c OCI_Connection_Pool.c oci_cache.c string_utils.c \
 *       ini_reader.c logger.c \
 *       -L/home/leyden100/eclipse-workspace/OCI_Wrapper/oci \
 *       -Wl,--start-group -lclntsh -lldap -lsodium -lmicrohttpd -lcjson \
 *       -lxml2 -lclntshcore -lnnz -lcurl -lpthread -lm -Wl,--end-group
 *
 * Run - no arguments needed (same LD_LIBRARY_PATH/LSAN_OPTIONS export
 * pattern as Run_Manually.sh):
 *   ./Driver_Pool_Test
 *
 * Expected output on success:
 *   Round A (direct OCI_Connect_pool)   ... OK
 *   Round A get_session/is_alive/release... OK
 *   Round B (via db_driver_t)           ... OK
 *   Round B handle check                ... OK (envhp/errhp/srvhp/svchp populated)
 *   Round B session_is_alive            ... OK
 *   Round B health_check after release  ... OK (0 unrecoverable)
 *   PASS
 *
 * A non-zero exit code means at least one round failed - check
 * config.connectionpool_log_file_name (both rounds log there, same as
 * production does today) for the OCI error detail.
 *
 * Vendor-internal leak notes: same accepted category as
 * Driver_Connect_Test.c's own PASS run (Oracle Instant Client's
 * process-lifetime global init, already covered by
 * lsan_suppressions.txt / LSAN_OPTIONS exitcode=0 if a clean exit code
 * matters for this run) - not chased here, see that file's header for
 * the full reasoning. This file's own logger_close() calls at the end
 * cover this file's own strdup'd log paths, same pattern.
 */

#define _POSIX_C_SOURCE 200809L

/* Same hardcoded path convention as Driver_Connect_Test.c/
 * Level2_Insert_Test.c - update and rebuild if it ever moves. */
#define CONFIG_INI "/home/leyden100/eclipse-workspace/OCI_Wrapper/Props/config.ini"

#include <stdio.h>
#include <string.h>

#include "OCI_Connection.h"
#include "OCI_Connection_Pool.h"
#include "db_driver.h"
#include "driver_oracle.h"
#include "ini_reader.h"
#include "logger.h"

/* Identical to Driver_Connect_Test.c's init_ctx() - kept as its own
 * copy rather than a shared header for now, same reasoning as that
 * file: this is a standalone fixture, not a build target that other
 * code links against. */
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

    /* ---- Round A: direct OCI_Connect_pool()/OCI_Pool_*(), the
     * known-good path already running in production today ---- */
    oci_context_t ctx_a, worker_a; app_config_t config_a;
    logger_t err_a, conn_a;
    printf("Round A (direct OCI_Connect_pool)   ... ");
    if (init_ctx(&ctx_a, &config_a, &err_a, &conn_a) != 0) { printf("INIT FAILED\n"); return 1; }
    if (OCI_Connect_pool(&ctx_a) != 0) { printf("FAILED - see %s\n", config_a.connectionpool_log_file_name); return 1; }
    printf("OK\n");

    printf("Round A get_session/is_alive/release... ");
    memset(&worker_a, 0, sizeof(worker_a));
    if (OCI_Pool_get_session(&ctx_a, &worker_a) != 0)
    {
        printf("FAILED - get_session\n"); failed = 1;
    }
    else if (!OCI_Pool_session_is_alive(&worker_a))
    {
        printf("FAILED - session_is_alive returned 0\n"); failed = 1;
        OCI_Pool_release_session(&ctx_a, &worker_a);
    }
    else
    {
        OCI_Pool_release_session(&ctx_a, &worker_a);
        printf("OK\n");
    }
    OCI_Disconnect_pool(&ctx_a);

    /* ---- Round B: via db_driver_t ---- */
    oci_context_t ctx_b, worker_b; app_config_t config_b;
    logger_t err_b, conn_b;
    printf("Round B (via db_driver_t)           ... ");
    if (init_ctx(&ctx_b, &config_b, &err_b, &conn_b) != 0) { printf("INIT FAILED\n"); return 1; }

    const db_driver_t *driver = db_driver_get(&ctx_b);
    if (!driver || !driver->connect || !driver->disconnect || !driver->get_session ||
        !driver->release_session || !driver->session_is_alive || !driver->health_check)
    {
        printf("FAILED - db_driver_get() returned an incomplete driver\n");
        return 1;
    }

    if (driver->connect(&ctx_b) != 0)
    {
        printf("FAILED - see %s\n", config_b.connectionpool_log_file_name);
        return 1;
    }
    printf("OK\n");

    memset(&worker_b, 0, sizeof(worker_b));
    if (driver->get_session(&ctx_b, &worker_b) != 0)
    {
        printf("FAILED - get_session\n");
        failed = 1;
    }
    else
    {
        printf("Round B handle check                ... ");
        if (worker_b.envhp && worker_b.errhp && worker_b.srvhp && worker_b.svchp)
            printf("OK (envhp/errhp/srvhp/svchp populated)\n");
        else
        {
            printf("FAILED - get_session() returned 0 but handles are NULL\n");
            failed = 1;
        }

        printf("Round B session_is_alive            ... ");
        if (driver->session_is_alive(&worker_b))
            printf("OK\n");
        else
        {
            printf("FAILED\n");
            failed = 1;
        }

        driver->release_session(&ctx_b, &worker_b);

        printf("Round B health_check after release   ... ");
        int unrecovered = driver->health_check(&ctx_b);
        if (unrecovered == 0)
            printf("OK (0 unrecoverable)\n");
        else
        {
            printf("FAILED - %d unrecoverable slot(s)\n", unrecovered);
            failed = 1;
        }
    }

    driver->disconnect(&ctx_b);

    printf("%s\n", failed ? "FAIL" : "PASS");

    /* ---- Cleanup - closes the loggers LeakSanitizer will otherwise
     * flag, same reasoning and same fix as Driver_Connect_Test.c. Does
     * NOT chase the separate Oracle Instant Client process-lifetime
     * leaks - see that file's header comment and tonight's accepted
     * decision to leave the sanitizer on and live with vendor leaks. */
    logger_close(&err_a);
    logger_close(&conn_a);
    logger_close(&err_b);
    logger_close(&conn_b);

    return failed ? 1 : 0;
}
