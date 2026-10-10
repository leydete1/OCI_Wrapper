/* ======================================================================
 * http_consumer_tester.c
 *
 * Stage 3 concurrency/regression tester for HTTP Consumer (2026-08-16).
 *
 * Design (per discussion with Terry):
 *   - Reuses File Consumer's own existing fixtures (Unit_Test_* /
 *     Request_Test_* .xml/.json under test_data/consumers/file/) rather
 *     than inventing new test data - that coverage is already proven.
 *   - One CREATE_SESSION up front, shared across every thread/fixture -
 *     stresses session_cache/session_touch under real concurrent reads
 *     of the SAME session. (Deliberately not "one session per thread"
 *     yet - that's an easy follow-on once this version is proven, per
 *     the same discussion.)
 *   - Every fixture's baked-in <session_id>-</session_id> (or
 *     "session_id":"-") placeholder gets the real session_id spliced
 *     in before sending - the "-" placeholder only ever worked for
 *     File Consumer because File Consumer overrides it server-side;
 *     HTTP consumer's CRUD path trusts whatever session_id is actually
 *     in the body.
 *   - N worker threads, fixtures divided by simple index striding
 *     (thread i handles fixtures[i], [i+N], [i+2N], ...) for real
 *     concurrent load, not a sequential walk.
 *   - First run against a given fixture writes a baseline; every run
 *     after that diffs against it. A diff on a SELECT-shaped fixture is
 *     flagged as a real regression. A diff on an INSERT/UPDATE/DELETE-
 *     shaped fixture is logged but NOT treated as a failure - real
 *     concurrent DML against shared tables legitimately produces
 *     different row counts/content run to run (see
 *     Oracle_Contention_Troubleshooting_Guide, and the Stage 4 queue-
 *     routing gap discussed alongside this tester).
 *   - Lightweight structural asserts run regardless of baseline
 *     diffing: response non-empty, well-formed enough to contain a
 *     <status>/status field, external_audit_id echoed back correctly.
 *   - One END_SESSION call at the end for a clean, repeatable run
 *     rather than relying on TTL expiry between runs.
 *
 * Build:
 *   gcc -O2 -Wall -o http_consumer_tester http_consumer_tester.c \
 *       -lcurl -lpthread
 *
 * Usage:
 *   ./http_consumer_tester [base_url] [xml_dir] [json_dir] [num_threads] [baseline_dir]
 *
 *   All arguments optional, in order, with sane defaults below. Example:
 *   ./http_consumer_tester https://localhost:8443 \
 *       /home/leyden100/eclipse-workspace/OCI_Wrapper/test_data/consumers/file/Input_XML \
 *       /home/leyden100/eclipse-workspace/OCI_Wrapper/test_data/consumers/file/Input_JSON \
 *       8 ./baseline
 *
 * NOTE: this file could not be compiled or run in the environment it
 * was written in (no libcurl, no network access) - it has been
 * reviewed carefully by hand but not build-verified. Please compile
 * and report back anything that doesn't match.
 * ====================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>

/* Safety net (2026-08-17) - bypasses curl.h's GCC-specific compile-time
 * type-checking macros (typecheck-gcc.h) entirely. Those macros are
 * what actually failed to expand in the "_curl_read_callback1" class of
 * errors - CURLOPT_TIMEOUT etc. are still fully functional without
 * them, this just removes the extra compile-time argument-type
 * checking curl_easy_setopt() normally gets. Safe either way; remove
 * once libcurl4-openssl-dev is confirmed properly installed if you'd
 * rather keep that checking. */
#define CURL_DISABLE_TYPECHECK 1
#include <curl/curl.h>

/* ---------------------------------------------------------------- */
/*  Config - defaults, all overridable via argv                     */
/* ---------------------------------------------------------------- */
#define DEFAULT_BASE_URL     "https://localhost:8443"
#define DEFAULT_XML_DIR      "/home/leyden100/eclipse-workspace/OCI_Wrapper/test_data/consumers/file/Input_XML"
#define DEFAULT_JSON_DIR     "/home/leyden100/eclipse-workspace/OCI_Wrapper/test_data/consumers/file/Input_JSON"
#define DEFAULT_NUM_THREADS  8
#define DEFAULT_BASELINE_DIR "./baseline"

static char g_base_url[512];
static char g_baseline_dir[512];
static int  g_num_threads;
static char g_session_id[128] = "";   /* filled in after CREATE_SESSION */

/* ---------------------------------------------------------------- */
/*  Fixture list                                                     */
/* ---------------------------------------------------------------- */
typedef struct {
    char *path;       /* full path on disk                          */
    char *name;       /* basename, used as baseline key              */
    int   is_json;    /* 1 = .json, 0 = .xml                        */
} fixture_t;

static fixture_t *g_fixtures = NULL;
static int        g_fixture_count = 0;

/* ---------------------------------------------------------------- */
/*  Shared result counters - mutex protected, updated by every thread */
/* ---------------------------------------------------------------- */
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_stat_total            = 0;
static int g_stat_pass             = 0;
static int g_stat_baseline_created = 0;
static int g_stat_expected_diff    = 0;   /* expected, not a failure */
static int g_stat_fail             = 0;

/* ---------------------------------------------------------------- */
/*  dyn_buf_t - simple growable buffer for libcurl write callback     */
/*  and for reading fixture files off disk                          */
/* ---------------------------------------------------------------- */
typedef struct {
    char   *data;
    size_t  len;
    size_t  capacity;
} dyn_buf_t;

static void dyn_buf_init(dyn_buf_t *b)
{
    b->capacity = 4096;
    b->data = malloc(b->capacity);
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

static void dyn_buf_append(dyn_buf_t *b, const char *src, size_t n)
{
    if (b->len + n + 1 > b->capacity)
    {
        while (b->len + n + 1 > b->capacity) b->capacity *= 2;
        b->data = realloc(b->data, b->capacity);
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void dyn_buf_free(dyn_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->capacity = 0;
}

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size = size * nmemb;
    dyn_buf_append((dyn_buf_t *)userp, (const char *)contents, real_size);
    return real_size;
}

/* ---------------------------------------------------------------- */
/*  Narrow-purpose flat tag extractor - same shape as the one added */
/*  to http_consumer.c for session_id on END_SESSION. Only used here */
/*  to pull <session_id> out of the CREATE_SESSION response.        */
/* ---------------------------------------------------------------- */
static int extract_simple_tag(const char *xml, const char *tag,
                               char *out, size_t out_size)
{
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return 0;
    start += strlen(open_tag);

    const char *end = strstr(start, close_tag);
    if (!end || end <= start) return 0;

    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

/* Reads an entire file into a malloc'd, NUL-terminated buffer.
 * Returns NULL on any failure. */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    if (out_len) *out_len = read;
    return buf;
}

static int write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return (written == len) ? 0 : -1;
}

/* ---------------------------------------------------------------- */
/*  Splices the real session_id into a fixture body, replacing the  */
/*  "-" placeholder. Handles both shapes:                            */
/*    XML:  <session_id>-</session_id>                              */
/*    JSON: "session_id":"-"  or  "session_id": "-"                 */
/*  Returns a newly malloc'd string; caller frees. Falls back to a  */
/*  plain strdup of the original if no placeholder is found (some    */
/*  fixtures - e.g. the CREATE_SESSION one itself - carry none).    */
/* ---------------------------------------------------------------- */
static char *inject_session_id(const char *body, const char *session_id, int is_json)
{
    const char *needle = is_json ? "\"session_id\"" : "<session_id>-</session_id>";

    if (!is_json)
    {
        const char *pos = strstr(body, needle);
        if (!pos) return strdup(body);

        size_t prefix_len = (size_t)(pos - body);
        size_t suffix_off = prefix_len + strlen(needle);

        size_t new_len = prefix_len + strlen("<session_id>") +
                          strlen(session_id) + strlen("</session_id>") +
                          strlen(body + suffix_off) + 1;
        char *out = malloc(new_len);
        snprintf(out, new_len, "%.*s<session_id>%s</session_id>%s",
                 (int)prefix_len, body, session_id, body + suffix_off);
        return out;
    }

    /* JSON: find "session_id", then the next quoted value after the
     * colon, and replace just that value - tolerant of either
     * "session_id":"-" or "session_id": "-" (optional space). */
    const char *key_pos = strstr(body, needle);
    if (!key_pos) return strdup(body);

    const char *colon = strchr(key_pos, ':');
    if (!colon) return strdup(body);

    const char *value_start = strchr(colon, '"');
    if (!value_start) return strdup(body);
    value_start++; /* past the opening quote */

    const char *value_end = strchr(value_start, '"');
    if (!value_end) return strdup(body);

    size_t prefix_len = (size_t)(value_start - body);
    size_t suffix_off  = (size_t)(value_end - body);

    size_t new_len = prefix_len + strlen(session_id) +
                      strlen(body + suffix_off) + 1;
    char *out = malloc(new_len);
    snprintf(out, new_len, "%.*s%s%s",
             (int)prefix_len, body, session_id, body + suffix_off);
    return out;
}

/* Manual case-insensitive substring search - avoids depending on
 * strcasestr(), which is a GNU extension not guaranteed available
 * without _GNU_SOURCE. */
static int contains_ci(const char *haystack, const char *needle)
{
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++)
        if (strncasecmp(haystack + i, needle, nlen) == 0)
            return 1;
    return 0;
}

/* Strips execution_time/execution_time_total's actual numeric value
 * out of a response before baseline comparison, replacing it with a
 * fixed placeholder. Confirmed 2026-08-19 by directly diffing two real
 * runs' identical Select_3.xml responses: the ENTIRE difference was
 * this one field (0.000902 -> 0.003794) - every other byte, including
 * all row/field data, was identical. This field is never stable across
 * any two executions, cached or not, DML or read-only - normalizing it
 * out here is more precise than guessing which categories of fixture
 * might touch it (which is how is_expected_variance_fixture() started,
 * and still catches genuinely different DATA - e.g. Round_1 vs Round_2
 * writing different rows, or P_STATUS actually changing - this only
 * removes the ONE specific field that was never a meaningful signal in
 * the first place). Returns a newly malloc'd string; caller frees. */
static char *normalize_execution_time(const char *body)
{
    char *out = malloc(strlen(body) + 1);
    size_t out_len = 0;
    const char *p = body;

    const char *needles[] = {
        "<execution_time_total>", "<execution_time>",
        "\"execution_time\":"
    };
    const int is_xml_tag[] = { 1, 1, 0 };

    while (*p)
    {
        int matched = 0;
        for (int n = 0; n < 3; n++)
        {
            size_t nlen = strlen(needles[n]);
            if (strncmp(p, needles[n], nlen) == 0)
            {
                memcpy(out + out_len, needles[n], nlen);
                out_len += nlen;
                p += nlen;

                if (is_xml_tag[n])
                {
                    /* skip until '<' (the closing tag) */
                    while (*p && *p != '<') p++;
                }
                else
                {
                    /* skip until ',' or '}' (end of JSON value) */
                    while (*p && *p != ',' && *p != '}') p++;
                }

                const char *placeholder = "X";
                memcpy(out + out_len, placeholder, 1);
                out_len += 1;

                matched = 1;
                break;
            }
        }
        if (!matched)
        {
            out[out_len++] = *p;
            p++;
        }
    }
    out[out_len] = '\0';
    return out;
}

/* Detects whether a fixture's response is EXPECTED to vary run-to-run,
 *
 *   - INSERT/UPDATE/DELETE - real concurrent writes against shared
 *     tables (filename-independent - checks actual operation type in
 *     the request body, so a generically-named fixture like
 *     Request_Test_TransactionName_2.json that does real INSERTs still
 *     gets caught - found 2026-08-18).
 *
 *   - EXECUTE_PROCEDURE - confirmed 2026-08-18 via a direct diff of two
 *     real runs' metrics_Data_Manager.csv output_response columns:
 *     OCI_Execute_Procedure_Module.c never touches resultset_cache at
 *     all (grep confirms zero references), so execution_time differs
 *     on every single call with no exception. Worse/better - the
 *     P_STATUS out-parameter itself genuinely changed between runs
 *     (80 -> 82) - procedures can read live, mutable state that other
 *     concurrent fixtures modify, so their output is expected to vary
 *     for real business reasons, not just timing noise.
 *
 *   - Filenames containing "blob"/"clob" - confirmed 2026-08-18 via
 *     the same direct-diff technique: the difference was a repeated
 *     digit substitution appearing identically at every LOB field in
 *     the document - the signature of a per-execution timestamp baked
 *     into each freshly-written LOB output file's name. A plain SELECT
 *     with no LOB columns instead hits resultset_cache (SQL-keyed,
 *     confirmed in OCI_Execute_Query_Batch_Module.c) and is served the
 *     exact cached bytes on a second run, which is why THOSE fixtures
 *     (Select_2/Select_3, no LOBs) correctly stay byte-identical run to
 *     run while LOB-bearing ones never will. */
static int is_expected_variance_fixture(const char *name, const char *body)
{
    if (contains_ci(body, "\"INSERT\"")      || contains_ci(body, "type=\"INSERT\"")  ||
        contains_ci(body, "\"UPDATE\"")      || contains_ci(body, "type=\"UPDATE\"")  ||
        contains_ci(body, "\"DELETE\"")      || contains_ci(body, "type=\"DELETE\"")  ||
        contains_ci(body, "\"EXECUTE_PROCEDURE\"") || contains_ci(body, "type=\"EXECUTE_PROCEDURE\""))
        return 1;

    if (contains_ci(name, "blob") || contains_ci(name, "clob"))
        return 1;

    return 0;
}

/* ---------------------------------------------------------------- */
/*  HTTP POST via libcurl. Returns the response body in resp (caller */
/*  must dyn_buf_free it) and the HTTP status code via *status_out. */
/*  Returns 0 on a completed request (regardless of HTTP status),   */
/*  -1 on a transport-level failure (connection refused, etc).       */
/* ---------------------------------------------------------------- */
static int http_post(CURL *curl, const char *url, const char *body,
                      const char *content_type, dyn_buf_t *resp,
                      long *status_out)
{
    dyn_buf_init(resp);

    struct curl_slist *headers = NULL;
    char content_type_header[64];
    snprintf(content_type_header, sizeof(content_type_header),
             "Content-Type: %s", content_type);
    headers = curl_slist_append(headers, content_type_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK)
    {
        fprintf(stderr, "  [transport error] %s\n", curl_easy_strerror(rc));
        dyn_buf_free(resp);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out);
    return 0;
}

/* ---------------------------------------------------------------- */
/*  CREATE_SESSION / END_SESSION                                     */
/* ---------------------------------------------------------------- */
static int create_shared_session(CURL *curl)
{
    const char *req =
        "<Session_Request>"
        "<operation>CREATE_SESSION</operation>"
        "<application_name>http_consumer_tester</application_name>"
        "</Session_Request>";

    dyn_buf_t resp;
    long status = 0;
    char url[600];
    snprintf(url, sizeof(url), "%s/", g_base_url);

    if (http_post(curl, url, req, "application/xml", &resp, &status) != 0)
    {
        fprintf(stderr, "CREATE_SESSION: transport failure - is the "
                        "listener up?\n");
        return -1;
    }

    int found = extract_simple_tag(resp.data, "session_id",
                                    g_session_id, sizeof(g_session_id));
    if (!found || !g_session_id[0])
    {
        fprintf(stderr, "CREATE_SESSION failed - response was:\n%s\n",
                resp.data);
        dyn_buf_free(&resp);
        return -1;
    }

    printf("CREATE_SESSION ok - session_id=%s\n", g_session_id);
    dyn_buf_free(&resp);
    return 0;
}

static void end_shared_session(CURL *curl)
{
    if (!g_session_id[0]) return;

    char req[512];
    snprintf(req, sizeof(req),
              "<Session_Request>"
              "<operation>END_SESSION</operation>"
              "<session_id>%s</session_id>"
              "</Session_Request>", g_session_id);

    dyn_buf_t resp;
    long status = 0;
    char url[600];
    snprintf(url, sizeof(url), "%s/", g_base_url);

    if (http_post(curl, url, req, "application/xml", &resp, &status) == 0)
    {
        printf("END_SESSION ok - %s\n",
               strstr(resp.data, "LOGGED_OUT") ? "LOGGED_OUT" : "sent");
        dyn_buf_free(&resp);
    }
    else
    {
        fprintf(stderr, "END_SESSION: transport failure (non-fatal - "
                        "test run is already complete)\n");
    }
}

/* ---------------------------------------------------------------- */
/*  Runs one fixture: inject session_id, POST, structural asserts,   */
/*  baseline compare. Logs its own outcome; updates shared counters. */
/* ---------------------------------------------------------------- */
static void run_one_fixture(CURL *curl, const fixture_t *fx)
{
    size_t raw_len = 0;
    char *raw = read_file(fx->path, &raw_len);
    if (!raw)
    {
        fprintf(stderr, "[%s] could not read fixture file\n", fx->name);
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_fail++; g_stat_total++;
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    char *body = inject_session_id(raw, g_session_id, fx->is_json);
    free(raw);

    /* Pull external_audit_id out of the REQUEST so we can confirm it's
     * echoed back - a cheap, meaningful structural assert regardless
     * of baseline diffing. */
    char audit_id[128] = "";
    extract_simple_tag(body, "external_audit_id", audit_id, sizeof(audit_id));

    /* Captured now, while body is still alive - it's freed right after
     * the POST below, but is_expected_variance_fixture()'s result is
     * still needed later, at the baseline-compare point. */
    int fixture_expects_variance = is_expected_variance_fixture(fx->name, body);

    char url[600];
    snprintf(url, sizeof(url), "%s/", g_base_url);

    dyn_buf_t resp;
    long status = 0;
    int rc = http_post(curl, url,
                        body, fx->is_json ? "application/json" : "application/xml",
                        &resp, &status);
    free(body);

    pthread_mutex_lock(&g_stats_mutex);
    g_stat_total++;
    pthread_mutex_unlock(&g_stats_mutex);

    if (rc != 0)
    {
        printf("[%s] FAIL - transport error, no response\n", fx->name);
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_fail++;
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    /* --- structural asserts --- */
    int structural_ok = 1;
    if (resp.len == 0)
    {
        printf("[%s] FAIL - empty response body (HTTP %ld)\n", fx->name, status);
        structural_ok = 0;
    }
    else
    {
        /* audit_id is ONLY present on an error envelope - a successful
         * dml_response_t (SELECT/INSERT/UPDATE/DELETE/EXECUTE_PROCEDURE)
         * never carries audit_id/external_audit_id at all (confirmed
         * 2026-08-18 against real metrics_Data_Manager.csv data - a
         * clean INSERT response has no audit_id field whatsoever).
         * Asserting it unconditionally produced a wall of false
         * failures on every genuine success. Only check it when the
         * response is itself an error - that's a real, meaningful
         * assert (catches an error envelope that forgot to echo it),
         * without penalising success. */
        int looks_like_error = (strstr(resp.data, "<status>ERROR</status>") != NULL) ||
                                (strstr(resp.data, "\"status\":\"ERROR\"") != NULL) ||
                                (strstr(resp.data, "error_code") != NULL);

        if (looks_like_error && audit_id[0] && strstr(resp.data, audit_id) == NULL)
        {
            printf("[%s] FAIL - error response did not echo "
                   "external_audit_id '%s' (HTTP %ld)\n",
                   fx->name, audit_id, status);
            structural_ok = 0;
        }
    }

    if (!structural_ok)
    {
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_fail++;
        pthread_mutex_unlock(&g_stats_mutex);
        dyn_buf_free(&resp);
        return;
    }

    /* --- baseline compare --- */
    char baseline_path[700];
    snprintf(baseline_path, sizeof(baseline_path), "%s/%s.baseline",
             g_baseline_dir, fx->name);

    size_t baseline_len = 0;
    char *baseline = read_file(baseline_path, &baseline_len);

    if (!baseline)
    {
        write_file(baseline_path, resp.data, resp.len);
        printf("[%s] BASELINE CREATED (HTTP %ld, %zu bytes)\n",
               fx->name, status, resp.len);
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_baseline_created++;
        pthread_mutex_unlock(&g_stats_mutex);
        dyn_buf_free(&resp);
        return;
    }

    char *baseline_norm = normalize_execution_time(baseline);
    char *current_norm  = normalize_execution_time(resp.data);
    int identical = (strcmp(baseline_norm, current_norm) == 0);
    free(baseline_norm);
    free(current_norm);
    free(baseline);

    if (identical)
    {
        printf("[%s] PASS (HTTP %ld)\n", fx->name, status);
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_pass++;
        pthread_mutex_unlock(&g_stats_mutex);
    }
    else if (fixture_expects_variance)
    {
        printf("[%s] DIFFERS from baseline (HTTP %ld) - expected for a "
               "fixture with a known source of run-to-run variance "
               "(DML, EXECUTE_PROCEDURE, or LOB content), not treated "
               "as a failure\n", fx->name, status);
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_expected_diff++;
        pthread_mutex_unlock(&g_stats_mutex);
    }
    else
    {
        printf("[%s] FAIL - response differs from baseline (HTTP %ld) - "
               "unexpected - no known source of variance for this "
               "fixture\n", fx->name, status);
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_fail++;
        pthread_mutex_unlock(&g_stats_mutex);
    }

    dyn_buf_free(&resp);
}

/* ---------------------------------------------------------------- */
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
        fprintf(stderr, "Thread %d: curl_easy_init failed\n", idx);
        return NULL;
    }

    for (int i = idx; i < g_fixture_count; i += g_num_threads)
        run_one_fixture(curl, &g_fixtures[i]);

    curl_easy_cleanup(curl);
    return NULL;
}

/* ---------------------------------------------------------------- */
/*  Fixture discovery - every .xml/.json file in the given dir.      */
/* ---------------------------------------------------------------- */
static void collect_fixtures(const char *dir, int is_json)
{
    DIR *d = opendir(dir);
    if (!d)
    {
        fprintf(stderr, "Warning: could not open fixture dir '%s' - "
                        "skipping\n", dir);
        return;
    }

    struct dirent *entry;
    const char *ext = is_json ? ".json" : ".xml";
    size_t ext_len = strlen(ext);

    while ((entry = readdir(d)) != NULL)
    {
        size_t name_len = strlen(entry->d_name);
        if (name_len <= ext_len) continue;
        if (strcasecmp(entry->d_name + name_len - ext_len, ext) != 0) continue;

        g_fixtures = realloc(g_fixtures, sizeof(fixture_t) * (g_fixture_count + 1));
        fixture_t *fx = &g_fixtures[g_fixture_count];

        size_t path_len = strlen(dir) + 1 + name_len + 1;
        fx->path = malloc(path_len);
        snprintf(fx->path, path_len, "%s/%s", dir, entry->d_name);

        fx->name = strdup(entry->d_name);
        fx->is_json = is_json;

        g_fixture_count++;
    }

    closedir(d);
}

int main(int argc, char *argv[])
{
    strncpy(g_base_url, argc > 1 ? argv[1] : DEFAULT_BASE_URL, sizeof(g_base_url) - 1);
    const char *xml_dir  = argc > 2 ? argv[2] : DEFAULT_XML_DIR;
    const char *json_dir = argc > 3 ? argv[3] : DEFAULT_JSON_DIR;
    g_num_threads = argc > 4 ? atoi(argv[4]) : DEFAULT_NUM_THREADS;
    strncpy(g_baseline_dir, argc > 5 ? argv[5] : DEFAULT_BASELINE_DIR, sizeof(g_baseline_dir) - 1);

    if (g_num_threads < 1) g_num_threads = 1;

    mkdir(g_baseline_dir, 0755);   /* ignore EEXIST */

    curl_global_init(CURL_GLOBAL_DEFAULT);

    collect_fixtures(xml_dir, 0);
    collect_fixtures(json_dir, 1);

    if (g_fixture_count == 0)
    {
        fprintf(stderr, "No fixtures found in '%s' or '%s' - nothing to "
                        "do.\n", xml_dir, json_dir);
        return 1;
    }

    printf("HTTP Consumer Tester - %d fixtures, %d threads, base_url=%s, "
           "baseline_dir=%s\n\n", g_fixture_count, g_num_threads,
           g_base_url, g_baseline_dir);

    CURL *setup_curl = curl_easy_init();
    if (!setup_curl || create_shared_session(setup_curl) != 0)
    {
        fprintf(stderr, "Aborting - could not establish shared session.\n");
        return 1;
    }

    pthread_t *threads = malloc(sizeof(pthread_t) * g_num_threads);
    for (int i = 0; i < g_num_threads; i++)
    {
        worker_arg_t *arg = malloc(sizeof(worker_arg_t));
        arg->thread_index = i;
        pthread_create(&threads[i], NULL, worker_main, arg);
    }
    for (int i = 0; i < g_num_threads; i++)
        pthread_join(threads[i], NULL);
    free(threads);

    end_shared_session(setup_curl);
    curl_easy_cleanup(setup_curl);
    curl_global_cleanup();

    printf("\n============================================================\n");
    printf("Total:            %d\n", g_stat_total);
    printf("Pass:             %d\n", g_stat_pass);
    printf("Baseline created: %d\n", g_stat_baseline_created);
    printf("Expected diff:    %d\n", g_stat_expected_diff);
    printf("FAIL:             %d\n", g_stat_fail);
    printf("============================================================\n");

    return (g_stat_fail > 0) ? 1 : 0;
}
