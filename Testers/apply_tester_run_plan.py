#!/usr/bin/env python3
"""
apply_tester_run_plan.py - points d1-d3 (2026-10-14), http_consumer_tester.c.

Usage:  python3 apply_tester_run_plan.py <path/to/http_consumer_tester.c>

Adds a run plan to the tester, read from ./run_plan.txt (a 6th argument
overrides the path). Without the file the tester behaves as before:
every fixture runs in parallel.

  d1 - run_plan.txt has two kinds of section:
         [solo]          run one at a time, in the order listed, BEFORE
                         anything else - they see the database exactly as
                         Reset_Tester_Objects.sql left it.
         [chain <name>]  run in the order listed, one after the other, on
                         one thread. Chains run alongside each other and
                         alongside every fixture not in the plan.
       The 8 worker threads now take work from one shared queue (chains
       first, as they take longest) instead of striding over the fixture
       list. A name in the plan with no fixture is reported as a FAIL; a
       fixture listed twice, an unknown section or an unknown word stops
       the tester before it starts.
  d3 - a fixture in the plan is compared strictly: a difference from its
       baseline is a FAIL, not an "expected diff", because its data no
       longer depends on run order. Ending its line with the word
       "variance" keeps the old tolerant rule for that fixture.

Edits the file in place and keeps the original as .bak_d. Each edit must
match exactly once or nothing is written. Running it twice is refused.
"""
import shutil
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
path = sys.argv[1]
raw = open(path, "rb").read()
src = raw.decode("utf-8")
crlf = "\r\n" in src
if crlf:
    src = src.replace("\r\n", "\n")

if "load_run_plan" in src:
    sys.exit("already applied - nothing to do")

edits = []

# 1. usage comment
edits.append(("usage comment",
""" *   ./http_consumer_tester [base_url] [xml_dir] [json_dir] [num_threads] [baseline_dir]
""",
""" *   ./http_consumer_tester [base_url] [xml_dir] [json_dir] [num_threads] [baseline_dir] [run_plan]
 *
 *   run_plan defaults to ./run_plan.txt; if it does not exist every
 *   fixture runs in parallel (2026-10-14, point d1 - see load_run_plan()).
"""))

# 2. ctype.h
edits.append(("includes",
"""#include <pthread.h>
""",
"""#include <pthread.h>
#include <ctype.h>      /* isspace() - run plan parser, point d1 */
"""))

# 3. default plan path
edits.append(("defaults",
"""#define DEFAULT_BASELINE_DIR "./baseline"
""",
"""#define DEFAULT_BASELINE_DIR "./baseline"
#define DEFAULT_RUN_PLAN     "./run_plan.txt"   /* point d1 */
"""))

# 4. fixture_t fields
edits.append(("fixture_t",
"""    int   is_json;    /* 1 = .json, 0 = .xml                        */
""",
"""    int   is_json;    /* 1 = .json, 0 = .xml                        */
    int   planned;    /* 1 = listed in run_plan.txt (point d1)       */
    int   strict;     /* 1 = a baseline difference is always a FAIL
                         (planned, no 'variance' word - point d3)    */
"""))

# 5. strict comparison
edits.append(("variance decision",
"""    int fixture_expects_variance = is_expected_variance_fixture(fx->name, body);
""",
"""    /* 2026-10-14 (point d3): a fixture in the run plan runs in a fixed
     * order against fixed data, so any difference is a real one. */
    int fixture_expects_variance =
        fx->strict ? 0 : is_expected_variance_fixture(fx->name, body);
"""))

# 6. FAIL message names the reason
edits.append(("FAIL message",
"""        printf("[%s] FAIL - response differs from baseline (HTTP %ld) - "
               "unexpected - no known source of variance for this "
               "fixture\\n", fx->name, status);
""",
"""        printf("[%s] FAIL - response differs from baseline (HTTP %ld) - "
               "%s\\n", fx->name, status,
               fx->strict ? "fixture is in run_plan.txt, compared strictly"
                          : "unexpected - no known source of variance for "
                            "this fixture");
"""))

# 7. worker: queue instead of striding, plus the plan code
edits.append(("worker_main",
"""/* ---------------------------------------------------------------- */
/*  Worker thread - index-strided over g_fixtures for real           */
/*  concurrent load rather than a sequential per-thread chunk.       */
/* ---------------------------------------------------------------- */
typedef struct { int thread_index; } worker_arg_t;

static void *worker_main(void *arg_v)
{
    worker_arg_t *arg = (worker_arg_t *)arg_v;
    int idx = arg->thread_index;
    free(arg);

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        fprintf(stderr, "Thread %d: curl_easy_init failed\\n", idx);
        return NULL;
    }

    for (int i = idx; i < g_fixture_count; i += g_num_threads)
        run_one_fixture(curl, &g_fixtures[i]);

    curl_easy_cleanup(curl);
    return NULL;
}
""",
r"""/* ---------------------------------------------------------------- */
/*  Run plan (2026-10-14, points d1-d3).                             */
/*                                                                   */
/*  run_plan.txt - '#' starts a comment, blank lines are ignored:    */
/*                                                                   */
/*    [solo]                 one at a time, in this order, before    */
/*    <fixture file name>    anything else (database as the reset    */
/*                           left it)                                */
/*    [chain <name>]         in this order, one after the other, on  */
/*    <fixture file name>    one thread; chains run alongside each   */
/*    <fixture> variance     other and the unlisted fixtures         */
/*                                                                   */
/*  A listed fixture is compared strictly (point d3) unless its line */
/*  ends with 'variance'. Unlisted fixtures run in parallel as       */
/*  before.                                                          */
/* ---------------------------------------------------------------- */
#define PLAN_MAX_CHAINS 64

typedef struct {
    char  name[64];
    int  *fx;           /* indexes into g_fixtures, in run order */
    int   count;
} plan_chain_t;

static int          *g_solo = NULL;
static int           g_solo_count = 0;
static plan_chain_t  g_chains[PLAN_MAX_CHAINS];
static int           g_chain_count = 0;
static char        **g_plan_missing = NULL;   /* listed, no such fixture */
static int           g_plan_missing_count = 0;

static char *plan_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static int find_fixture(const char *name)
{
    for (int i = 0; i < g_fixture_count; i++)
        if (strcmp(g_fixtures[i].name, name) == 0)
            return i;
    return -1;
}

static int append_index(int **arr, int *count, int value)
{
    int *grown = realloc(*arr, sizeof(int) * (size_t)(*count + 1));
    if (!grown) return -1;
    grown[(*count)++] = value;
    *arr = grown;
    return 0;
}

/* Returns 0 (plan loaded, or no plan file), -1 on an error in the
 * file - the tester then stops before it starts. */
static int load_run_plan(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        printf("No run plan (%s) - every fixture runs in parallel\n", path);
        return 0;
    }

    enum { SEC_NONE, SEC_SOLO, SEC_CHAIN } sec = SEC_NONE;
    plan_chain_t *chain = NULL;
    char line[512];
    int  lineno = 0;

    while (fgets(line, sizeof(line), f))
    {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *s = plan_trim(line);
        if (!*s) continue;

        if (*s == '[')
        {
            char *end = strchr(s, ']');
            if (!end) goto bad_line;
            *end = '\0';
            char *sec_name = plan_trim(s + 1);

            if (strcasecmp(sec_name, "solo") == 0)
            {
                sec = SEC_SOLO;
                continue;
            }
            if (strncasecmp(sec_name, "chain", 5) == 0 &&
                isspace((unsigned char)sec_name[5]))
            {
                char *cname = plan_trim(sec_name + 5);
                if (!*cname || g_chain_count >= PLAN_MAX_CHAINS) goto bad_line;
                chain = &g_chains[g_chain_count++];
                memset(chain, 0, sizeof(*chain));
                snprintf(chain->name, sizeof(chain->name), "%s", cname);
                sec = SEC_CHAIN;
                continue;
            }
            goto bad_line;
        }

        if (sec == SEC_NONE) goto bad_line;   /* fixture before a section */

        /* "<name>" or "<name> variance" */
        char *name = s;
        char *rest = s;
        while (*rest && !isspace((unsigned char)*rest)) rest++;
        int variance = 0;
        if (*rest)
        {
            *rest++ = '\0';
            rest = plan_trim(rest);
            if (strcasecmp(rest, "variance") == 0) variance = 1;
            else goto bad_line;
        }

        int i = find_fixture(name);
        if (i < 0)
        {
            char **grown = realloc(g_plan_missing,
                                   sizeof(char *) * (size_t)(g_plan_missing_count + 1));
            if (!grown) goto bad_line;
            g_plan_missing = grown;
            g_plan_missing[g_plan_missing_count++] = strdup(name);
            continue;
        }
        if (g_fixtures[i].planned)
        {
            fprintf(stderr, "%s line %d: '%s' is listed twice\n",
                    path, lineno, name);
            fclose(f);
            return -1;
        }
        g_fixtures[i].planned = 1;
        g_fixtures[i].strict  = !variance;

        int rc = (sec == SEC_SOLO)
                     ? append_index(&g_solo, &g_solo_count, i)
                     : append_index(&chain->fx, &chain->count, i);
        if (rc != 0) goto bad_line;
    }
    fclose(f);

    int planned = g_solo_count;
    for (int c = 0; c < g_chain_count; c++) planned += g_chains[c].count;
    printf("Run plan %s: %d solo, %d chain(s), %d fixture(s) planned, "
           "%d not found\n", path, g_solo_count, g_chain_count, planned,
           g_plan_missing_count);
    return 0;

bad_line:
    fprintf(stderr, "%s line %d: not understood (expected [solo], "
                    "[chain <name>], or '<fixture> [variance]' inside a "
                    "section)\n", path, lineno);
    fclose(f);
    return -1;
}

/* A planned name with no fixture is a FAIL, so a renamed or deleted
 * fixture cannot silently drop out of the run. */
static void report_plan_missing(void)
{
    for (int k = 0; k < g_plan_missing_count; k++)
    {
        printf("[%s] FAIL - listed in run_plan.txt but no such fixture\n",
               g_plan_missing[k]);
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_total++;
        g_stat_fail++;
        pthread_mutex_unlock(&g_stats_mutex);
    }
}

static void run_solo_phase(CURL *curl)
{
    if (g_solo_count == 0) return;
    printf("--- Solo phase: %d fixture(s), one at a time ---\n", g_solo_count);
    for (int k = 0; k < g_solo_count; k++)
        run_one_fixture(curl, &g_fixtures[g_solo[k]]);
    printf("--- Parallel phase ---\n");
}

/* Work queue: one unit per chain (chain >= 0), then one per unlisted
 * fixture (chain = -1). Workers take the next unit until none is left. */
typedef struct { int chain; int fx; } work_unit_t;

static work_unit_t     *g_units = NULL;
static int              g_unit_count = 0;
static int              g_next_unit = 0;
static pthread_mutex_t  g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static int build_work_queue(void)
{
    g_units = malloc(sizeof(work_unit_t) *
                     (size_t)(g_chain_count + g_fixture_count + 1));
    if (!g_units) return -1;
    for (int c = 0; c < g_chain_count; c++)
        g_units[g_unit_count++] = (work_unit_t){ c, -1 };
    for (int i = 0; i < g_fixture_count; i++)
        if (!g_fixtures[i].planned)
            g_units[g_unit_count++] = (work_unit_t){ -1, i };
    return 0;
}

static int next_unit(void)
{
    pthread_mutex_lock(&g_queue_mutex);
    int u = (g_next_unit < g_unit_count) ? g_next_unit++ : -1;
    pthread_mutex_unlock(&g_queue_mutex);
    return u;
}

/* ---------------------------------------------------------------- */
/*  Worker thread - takes units from the shared queue (2026-10-14,   */
/*  point d1; it used to stride over g_fixtures by thread index).    */
/* ---------------------------------------------------------------- */
typedef struct { int thread_index; } worker_arg_t;

static void *worker_main(void *arg_v)
{
    worker_arg_t *arg = (worker_arg_t *)arg_v;
    int idx = arg->thread_index;
    free(arg);

    CURL *curl = curl_easy_init();
    if (!curl)
    {
        fprintf(stderr, "Thread %d: curl_easy_init failed\n", idx);
        return NULL;
    }

    for (int u = next_unit(); u >= 0; u = next_unit())
    {
        const work_unit_t *w = &g_units[u];
        if (w->chain >= 0)
        {
            const plan_chain_t *c = &g_chains[w->chain];
            printf("--- chain %s: %d fixture(s) in order ---\n",
                   c->name, c->count);
            for (int k = 0; k < c->count; k++)
                run_one_fixture(curl, &g_fixtures[c->fx[k]]);
        }
        else
        {
            run_one_fixture(curl, &g_fixtures[w->fx]);
        }
    }

    curl_easy_cleanup(curl);
    return NULL;
}
"""))

# 8. collect_fixtures initialises the new fields
edits.append(("collect_fixtures",
"""        fx->is_json = is_json;
""",
"""        fx->is_json = is_json;
        fx->planned = 0;
        fx->strict  = 0;
"""))

# 9. main: plan path
edits.append(("main args",
"""    strncpy(g_baseline_dir, argc > 5 ? argv[5] : DEFAULT_BASELINE_DIR, sizeof(g_baseline_dir) - 1);
""",
"""    strncpy(g_baseline_dir, argc > 5 ? argv[5] : DEFAULT_BASELINE_DIR, sizeof(g_baseline_dir) - 1);
    const char *plan_path = argc > 6 ? argv[6] : DEFAULT_RUN_PLAN;   /* d1 */
"""))

# 10. main: load plan after discovery, before the session
edits.append(("main banner",
"""    printf("HTTP Consumer Tester - %d fixtures, %d threads, base_url=%s, "
           "baseline_dir=%s\\n\\n", g_fixture_count, g_num_threads,
           g_base_url, g_baseline_dir);
""",
"""    printf("HTTP Consumer Tester - %d fixtures, %d threads, base_url=%s, "
           "baseline_dir=%s\\n", g_fixture_count, g_num_threads,
           g_base_url, g_baseline_dir);

    if (load_run_plan(plan_path) != 0 || build_work_queue() != 0)   /* d1 */
    {
        fprintf(stderr, "Aborting - run plan not usable.\\n");
        return 1;
    }
    printf("\\n");
"""))

# 11. main: solo phase, then the threads
edits.append(("main threads",
"""    pthread_t *threads = malloc(sizeof(pthread_t) * g_num_threads);
""",
"""    report_plan_missing();         /* d1 */
    run_solo_phase(setup_curl);    /* d1 - before any parallel work */

    pthread_t *threads = malloc(sizeof(pthread_t) * g_num_threads);
"""))

out = src
for name, old, new in edits:
    n = out.count(old)
    if n != 1:
        sys.exit("NOT APPLIED: edit '%s' matched %d times (expected 1) - "
                 "file left unchanged" % (name, n))
    out = out.replace(old, new)

if crlf:
    out = out.replace("\n", "\r\n")
shutil.copyfile(path, path + ".bak_d")
open(path, "wb").write(out.encode("utf-8"))
print("applied points d1-d3 to %s (%d edits; original kept as %s.bak_d)"
      % (path, len(edits), path))
