/*
 * OCI_Unit_Test_Module.h
 *
 * Shared test-core module - see Unit_Test_Module_Design_Specification.docx
 * for the full design (architecture, tiers, catalog, decisions).
 *
 * Core design rule: this module's own logic must never depend on the
 * request pipeline (Level 1 parsing, Level 2 validation, the
 * dispatcher) it exists to test. Every test function calls the real
 * internal functions directly - level1_parse(), level2_validate_insert(),
 * execute_insert_batch(), and so on - never through an XML/JSON
 * envelope. OP_UNIT_TEST (a thin dispatcher wrapper, built separately)
 * calls into the exact same functions this module exposes; it does not
 * duplicate any test logic.
 *
 * Tier model
 * ----------
 *   Tier 1 - pure parsing / config / logger checks, no OCI handle used
 *            even though ctx has one by the time any test runs.
 *   Tier 2 - validation against real metadata_cache; read-only.
 *   Tier 3 - full round trip against the dedicated test table
 *            (unit_test.ini: test_table_name/test_table_owner),
 *            installed as a real, persistent object in the DATA_MANAGER
 *            schema itself (2026-08-01 decision) - not created/torn
 *            down per run. Every Tier 3 test still rolls back its own
 *            transaction regardless of outcome.
 *
 * Every test is called with a single, fully set-up ctx (loggers
 * initialised AND connection established) - Tier 1/2 tests simply
 * don't touch ctx->svchp/errhp even though it's available by then.
 * This keeps invocation to one call from main(), rather than splitting
 * it by tier.
 */

#ifndef OCI_UNIT_TEST_MODULE_H
#define OCI_UNIT_TEST_MODULE_H

#include "OCI_Connection.h"
#include "logger.h"

#define UT_TEST_ID_LEN      32
#define UT_MODULE_CODE_LEN  16
#define UT_MESSAGE_LEN      512
#define UT_DESCRIPTION_LEN  256

/*
 * unit_test_config_t
 *
 * Mirrors unit_test.ini's key table in Unit_Test_Module_Design_
 * Specification.docx exactly - a deliberately separate, much smaller
 * file from config.ini, so test-only settings can never bleed into
 * runtime configuration by accident. Parsed by unit_test_load_config()
 * below, not by ini_reader.c's load_ini() - that function's ctx_map
 * machinery is built for config.ini's ~95 fields; this is a handful of
 * simple key=value pairs and doesn't need it.
 */
typedef struct {
    int  startup_self_test_enabled;
    int  startup_max_tier;
    int  startup_halt_on_tier1_fail;
    int  startup_halt_on_tier2_fail;
    int  startup_halt_on_tier3_fail;
    char test_table_name[128];
    char test_table_owner[128];
    char test_procedure_name[128];
    int  unit_test_log_summary_enabled;
} unit_test_config_t;

/*
 * unit_test_load_config()
 *
 * Reads unit_test.ini from path. If the file does not exist at all,
 * this is NOT an error - cfg is populated with startup_self_test_
 * enabled=0 and otherwise-safe defaults, and 0 is returned. This is a
 * deliberate backward-compatibility choice (2026-08-01): existing
 * deployments predate this feature and won't have this file yet: main()
 * must not suddenly require it or change behaviour by default just
 * because this module now exists in the build.
 *
 * If the file DOES exist, every key is optional - any key not present
 * falls back to the "recommended default" documented in the spec
 * (startup_max_tier=1, halt_on_tier1/2_fail=1, halt_on_tier3_fail=0,
 * unit_test_log_summary_enabled=1). test_table_name/owner/
 * test_procedure_name have no safe default - they're only actually
 * read by Tier 3 tests, added in a later pass; empty is fine here.
 *
 * Returns 0 always (this is deliberately not a hard-failure path - a
 * malformed unit_test.ini falls back to safe defaults with a WARN
 * logged, rather than blocking startup over the test harness's own
 * config, which would defeat the purpose of a safety net).
 */
int unit_test_load_config(const char *path, logger_t *logger, unit_test_config_t *cfg);

typedef enum {
    UT_TIER_1 = 1,
    UT_TIER_2 = 2,
    UT_TIER_3 = 3
} unit_test_tier_t;

/*
 * unit_test_result_t
 *
 * status is a string ("PASS"/"FAIL"/"SKIP"), matching
 * Unit_Test_Module_Design_Specification.docx's own unit_test_result_t
 * field of the same name - kept as a string here (not an enum) since
 * this struct is the one that will be echoed back on an OP_UNIT_TEST
 * response, and the spec documents it as char[16].
 */
typedef struct {
    char   test_id[UT_TEST_ID_LEN];
    int    tier;
    char   status[16];
    char   message[UT_MESSAGE_LEN];
    double execution_time_seconds;
} unit_test_result_t;

/*
 * unit_test_fn_t
 *
 * ctx is always non-NULL and fully set up - see this file's own top
 * comment. Returns 0 for PASS, -1 for FAIL (message populated with the
 * specific reason). SKIP is never returned by a test itself - it is
 * decided solely by the orchestrator (unit_test_run_all() etc.) when a
 * test's own tier exceeds max_tier, keeping that decision in one place
 * rather than duplicated in every test.
 */
typedef int (*unit_test_fn_t)(oci_context_t *ctx, char *message, size_t message_max);

typedef struct {
    char           test_id[UT_TEST_ID_LEN];
    int            tier;
    char           module[UT_MODULE_CODE_LEN];
    char           description[UT_DESCRIPTION_LEN];
    unit_test_fn_t fn;
} unit_test_case_t;

/*
 * unit_test_set_ini_path()
 *
 * Must be called once by main() before unit_test_run_all(), with the
 * same config.ini path passed to load_ini() at real startup (argv[1]
 * in Test_XML_Runner.c today). Used only by UT-INI-002, which re-runs
 * load_ini() against this same known-good file to independently
 * re-verify the unguarded-CFG_STRING-field safeguard. Deliberately not
 * a new oci_context_t field (a test-harness concern, not a connection
 * one) and deliberately not threaded through every test function's own
 * signature just for the one test that needs it.
 */
void unit_test_set_ini_path(const char *path);

/*
 * unit_test_set_tier3_objects()
 *
 * Must be called once by main() before any Tier 3 test runs, with the
 * table_name/table_owner/procedure_name from the already-loaded
 * unit_test_config_t (unit_test.ini's own test_table_name/
 * test_table_owner/test_procedure_name keys). Every Tier 3 test reads
 * these rather than hardcoding a table/procedure name directly, so a
 * deployment can point Tier 3 at whatever dedicated schema objects it
 * actually has, without editing any test code.
 */
void unit_test_set_tier3_objects(const char *table_name, const char *table_owner,
                                  const char *procedure_name);

/*
 * unit_test_run_all()
 *
 * Runs every registered test with tier <= max_tier, in registration
 * order. Allocates *results_out (caller frees via
 * unit_test_free_results()); *result_count_out is set to the number of
 * entries actually run (tests above max_tier are not included at all,
 * not even as SKIP entries - a SKIP entry is reserved for a test that
 * was explicitly requested, e.g. via OP_UNIT_TEST, but could not run in
 * this environment).
 *
 * Returns 0 if every test run returned PASS, -1 if any returned FAIL.
 * (Matches the "halt on tier1/2 fail" behaviour in unit_test.ini -
 * main() reads *result_count_out/results_out itself to decide whether
 * to exit, per-tier, rather than this function knowing about halting
 * policy at all.)
 */
int unit_test_run_all(oci_context_t *ctx, int max_tier,
                       unit_test_result_t **results_out, int *result_count_out);

/*
 * unit_test_run_by_id()
 *
 * Runs exactly one test regardless of its tier (the caller - e.g.
 * OP_UNIT_TEST's dispatcher wrapper - is assumed to have already
 * decided this specific test is runnable in this environment; if the
 * required tier's resources aren't actually available, the test itself
 * will fail with a clear message rather than being silently skipped).
 * Returns 0 if found and run, -1 if test_id is not registered at all
 * (result_out->status set to "SKIP" with an explanatory message in
 * that case).
 */
int unit_test_run_by_id(oci_context_t *ctx, const char *test_id,
                         unit_test_result_t *result_out);

/*
 * unit_test_run_by_module()
 *
 * Runs every registered test for one module code (e.g. "DEL" for every
 * DELETE test), regardless of tier - same reasoning as
 * unit_test_run_by_id() above.
 */
int unit_test_run_by_module(oci_context_t *ctx, const char *module,
                             unit_test_result_t **results_out, int *result_count_out);

/*
 * unit_test_write_summary()
 *
 * Writes the consolidated PASS/FAIL summary block described in
 * Unit_Test_Module_Design_Specification.docx's own Reporting section,
 * to the given logger (main Data_Manager.log at startup; whichever
 * logger is appropriate for an on-demand OP_UNIT_TEST call).
 */
void unit_test_write_summary(logger_t *logger,
                              const unit_test_result_t *results, int count);

/*
 * unit_test_free_results()
 *
 * Frees an array returned by unit_test_run_all()/unit_test_run_by_module().
 * Safe to call with NULL.
 */
void unit_test_free_results(unit_test_result_t *results);

#endif /* OCI_UNIT_TEST_MODULE_H */
