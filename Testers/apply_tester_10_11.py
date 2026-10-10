#!/usr/bin/env python3
"""
apply_tester_10_11.py - points 10 and 11 (2026-10-10), http_consumer_tester.c.

Usage:  python3 apply_tester_10_11.py <path/to/http_consumer_tester.c>

Edits the file in place and keeps the original as .bak:

  Point 10 - the tester keeps what it got. For every fixture that does
    not PASS (FAIL, DIFFERS-expected, or a structural FAIL) the response
    is written to <baseline_dir>/<fixture>.current, next to
    <fixture>.baseline. A fixture that PASSes has any old .current
    removed, so the folder only ever holds this run's differences.
    Diff them with:
      for f in baseline/*.current; do diff -u "${f%.current}.baseline" "$f"; done

  Point 11 - session ids handled the same way in both formats.
    a) Requests: only "-" is a placeholder and is replaced with the run's
       session, in both formats; any other value is a deliberate test
       value and is sent as written. XML already worked this way; JSON
       used to replace every value.
    b) Responses: session_id is ignored when comparing with the baseline,
       like execution_time (a successful AUTHENTICATE returns a new
       session_id on every run).

Each edit must match exactly once, or nothing is written and the script
names the edit that did not match. Running it twice is refused.
"""
import shutil
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
path = sys.argv[1]
src = open(path, encoding="utf-8").read()

if "is_session_placeholder" in src:
    sys.exit("already applied - nothing to do")

NEW_INJECT = r'''/* ---------------------------------------------------------------- */
/*  Splices the real session_id into a fixture body when the body   */
/*  carries a placeholder. Handles both shapes:                      */
/*    XML:  <session_id>...</session_id>                            */
/*    JSON: "session_id":"..."  or  "session_id": "..."             */
/*  Returns a newly malloc'd string; caller frees. Falls back to a  */
/*  plain strdup of the original if there is no session_id or it is */
/*  not a placeholder.                                               */
/*                                                                   */
/*  2026-10-10 (point 11): the same rule for both formats, the one   */
/*  this file's header always described - only "-" is a placeholder  */
/*  and is replaced; any other value is a deliberate test value and  */
/*  is sent as written. XML already worked this way; JSON replaced   */
/*  every value, so the two formats of one fixture tested different  */
/*  things - CheckPermission R1/R2 .xml sent PASTE_REAL_SESSION_ID_  */
/*  HERE (SESSION_NOT_FOUND) while the .json sent the run's session  */
/*  (DENIED), and a .json fixture's deliberately bad session id was  */
/*  silently replaced.                                               */
/* ---------------------------------------------------------------- */
static int is_session_placeholder(const char *v, size_t n)
{
    while (n > 0 && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) { v++; n--; }
    while (n > 0 && (v[n-1] == ' ' || v[n-1] == '\t' || v[n-1] == '\n' || v[n-1] == '\r')) n--;

    static const char *placeholders[] = { "-" };
    for (size_t i = 0; i < sizeof(placeholders) / sizeof(placeholders[0]); i++)
        if (n == strlen(placeholders[i]) && strncmp(v, placeholders[i], n) == 0)
            return 1;
    return 0;
}

static char *inject_session_id(const char *body, const char *session_id, int is_json)
{
    const char *value_start = NULL;
    const char *value_end   = NULL;

    if (!is_json)
    {
        const char *open = strstr(body, "<session_id>");
        if (!open) return strdup(body);
        value_start = open + strlen("<session_id>");
        value_end   = strstr(value_start, "</session_id>");
        if (!value_end) return strdup(body);
    }
    else
    {
        /* find "session_id", then the next quoted value after the
         * colon - tolerant of "session_id":"x" or "session_id": "x" */
        const char *key_pos = strstr(body, "\"session_id\"");
        if (!key_pos) return strdup(body);
        const char *colon = strchr(key_pos + strlen("\"session_id\""), ':');
        if (!colon) return strdup(body);
        value_start = strchr(colon, '"');
        if (!value_start) return strdup(body);
        value_start++;                       /* past the opening quote */
        value_end = strchr(value_start, '"');
        if (!value_end) return strdup(body);
    }

    if (!is_session_placeholder(value_start, (size_t)(value_end - value_start)))
        return strdup(body);                 /* deliberate test value */

    size_t prefix_len = (size_t)(value_start - body);
    size_t new_len    = prefix_len + strlen(session_id) + strlen(value_end) + 1;
    char *out = malloc(new_len);
    if (!out) return strdup(body);
    snprintf(out, new_len, "%.*s%s%s",
             (int)prefix_len, body, session_id, value_end);
    return out;
}

'''

out = src

def replace_once(name, old, new):
    global out
    n = out.count(old)
    if n != 1:
        sys.exit("NOT APPLIED: '%s' matched %d times (expected 1) - "
                 "file left unchanged" % (name, n))
    out = out.replace(old, new)

# ---- 11a: inject_session_id(), replaced from its header comment to the
#      start of contains_ci()'s comment.
start = "/* ---------------------------------------------------------------- */\n/*  Splices the real session_id into a fixture body, replacing the  */"
end   = "/* Manual case-insensitive substring search"
if out.count(start) != 1 or out.count(end) != 1:
    sys.exit("NOT APPLIED: inject_session_id() block not found exactly once "
             "(start=%d end=%d) - file left unchanged"
             % (out.count(start), out.count(end)))
i, j = out.index(start), out.index(end)
block = out[i:j]
if j < i or block.count("static char *inject_session_id(") != 1:
    sys.exit("NOT APPLIED: inject_session_id() block looks different from "
             "the expected one - file left unchanged")
out = out[:i] + NEW_INJECT + out[j:]

# ---- 11b: the normalised copy can now grow by one character per match
#      (an empty <session_id></session_id> becomes <session_id>X</...>),
#      so size it for that. Each match consumes at least 12 characters,
#      so twice the input is always enough.
replace_once("normalize buffer size",
'''    char *out = malloc(strlen(body) + 1);
    size_t out_len = 0;
    const char *p = body;''',
'''    char *out = malloc(2 * strlen(body) + 2);   /* see point 11 note below */
    size_t out_len = 0;
    const char *p = body;''')

# ---- 11b: session_id ignored in the baseline comparison
replace_once("normalize needles",
'''    const char *needles[] = {
        "<execution_time_total>", "<execution_time>",
        "\\"execution_time\\":"
    };
    const int is_xml_tag[] = { 1, 1, 0 };''',
'''    /* 2026-10-10 (point 11): session_id is ignored too - a successful
     * AUTHENTICATE returns a new session_id on every run, so its
     * response could never match a baseline. No other response
     * carries one (error envelopes and CHECK_PERMISSION do not). */
    const char *needles[] = {
        "<execution_time_total>", "<execution_time>",
        "\\"execution_time\\":",
        "<session_id>", "\\"session_id\\":"
    };
    const int is_xml_tag[] = { 1, 1, 0, 1, 0 };''')

replace_once("normalize loop bound",
'''        for (int n = 0; n < 3; n++)
        {
            size_t nlen = strlen(needles[n]);''',
'''        for (int n = 0; n < (int)(sizeof(needles) / sizeof(needles[0])); n++)
        {
            size_t nlen = strlen(needles[n]);''')

# ---- 10: keep the response of every fixture that does not PASS
replace_once("helpers before run_one_fixture",
'''static void run_one_fixture(CURL *curl, const fixture_t *fx)
{''',
'''/* 2026-10-10 (point 10): the response of a fixture that does not PASS
 * is kept as <baseline_dir>/<name>.current, beside <name>.baseline, so
 * a difference can be read with diff rather than guessed at. A PASS
 * removes any .current left by an earlier run. */
static void save_current(const fixture_t *fx, const dyn_buf_t *resp)
{
    char path[720];
    snprintf(path, sizeof(path), "%s/%s.current", g_baseline_dir, fx->name);
    if (resp->data && write_file(path, resp->data, resp->len) != 0)
        fprintf(stderr, "[%s] could not write %s\\n", fx->name, path);
}

static void clear_current(const fixture_t *fx)
{
    char path[720];
    snprintf(path, sizeof(path), "%s/%s.current", g_baseline_dir, fx->name);
    remove(path);   /* usually absent - that is fine */
}

static void run_one_fixture(CURL *curl, const fixture_t *fx)
{''')

replace_once("structural fail keeps response",
'''    if (!structural_ok)
    {
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_fail++;
        pthread_mutex_unlock(&g_stats_mutex);
        dyn_buf_free(&resp);
        return;
    }''',
'''    if (!structural_ok)
    {
        save_current(fx, &resp);   /* point 10 */
        pthread_mutex_lock(&g_stats_mutex);
        g_stat_fail++;
        pthread_mutex_unlock(&g_stats_mutex);
        dyn_buf_free(&resp);
        return;
    }''')

replace_once("PASS clears .current",
'''        printf("[%s] PASS (HTTP %ld)\\n", fx->name, status);''',
'''        printf("[%s] PASS (HTTP %ld)\\n", fx->name, status);
        clear_current(fx);   /* point 10 */''')

replace_once("DIFFERS keeps response",
'''               "as a failure\\n", fx->name, status);''',
'''               "as a failure\\n", fx->name, status);
        save_current(fx, &resp);   /* point 10 */''')

replace_once("FAIL keeps response",
'''               "fixture\\n", fx->name, status);''',
'''               "fixture\\n", fx->name, status);
        save_current(fx, &resp);   /* point 10 */''')

shutil.copyfile(path, path + ".bak")
open(path, "w", encoding="utf-8").write(out)
print("applied points 10 and 11 to %s (original kept as %s.bak)" % (path, path))
