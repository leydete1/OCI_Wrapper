#!/usr/bin/env python3
"""
apply_tester_12.py - point 12 (2026-10-10), http_consumer_tester.c.

Usage:  python3 apply_tester_12.py <path/to/http_consumer_tester.c>
Run it AFTER apply_tester_10_11.py (it checks).

LOB output file names carry the time they were written, e.g.
  Adam_1_20261010_215252_0.jpg
so a SELECT returning BLOBs never matched its baseline. Before comparing,
both the baseline and the response now have every _YYYYMMDD_HHMMSS_
replaced by _TS_ (the same idea as execution_time). The fixture-name rule
for "blob"/"clob" fixtures is left as it is.

Edits the file in place, keeps the original as .bak (overwriting the one
apply_tester_10_11.py left - copy it first if you want to keep both).
Refuses to run twice or if the text it expects is not there.
"""
import shutil
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
path = sys.argv[1]
src = open(path, encoding="utf-8").read()

if "mask_lob_timestamps" in src:
    sys.exit("already applied - nothing to do")
if "save_current" not in src:
    sys.exit("run apply_tester_10_11.py first - nothing changed")

out = src

def replace_once(name, old, new):
    global out
    n = out.count(old)
    if n != 1:
        sys.exit("NOT APPLIED: '%s' matched %d times (expected 1) - "
                 "file left unchanged" % (name, n))
    out = out.replace(old, new)

replace_once("helper before save_current",
'''static void save_current(const fixture_t *fx, const dyn_buf_t *resp)''',
'''/* 2026-10-10 (point 12): replaces every _YYYYMMDD_HHMMSS_ in s with
 * _TS_, in place. LOB output file names carry the time they were
 * written (BLOB_append_file_timestamp=1), so a SELECT returning BLOBs
 * could never match its baseline. The result is never longer than s. */
static void mask_lob_timestamps(char *s)
{
    /* pattern, one byte at a time: '_' 8 digits '_' 6 digits '_'
     * (17 bytes). Checked left to right and stopped at the first
     * mismatch, so the terminating NUL (not a digit, not '_') ends the
     * check before anything past it is read. */
    static const char shape[] = "_DDDDDDDD_DDDDDD_";
    char *w = s;
    const char *p = s;
    while (*p)
    {
        int k = 0;
        while (k < 17 &&
               (shape[k] == 'D' ? (p[k] >= '0' && p[k] <= '9')
                                : (p[k] == '_')))
            k++;
        if (k == 17)
        {
            memcpy(w, "_TS_", 4);
            w += 4;
            p += 17;
            continue;
        }
        *w++ = *p++;
    }
    *w = '\\0';
}

static void save_current(const fixture_t *fx, const dyn_buf_t *resp)''')

replace_once("mask both sides before compare",
'''    char *current_norm  = normalize_execution_time(resp.data);''',
'''    char *current_norm  = normalize_execution_time(resp.data);
    mask_lob_timestamps(baseline_norm);   /* point 12 */
    mask_lob_timestamps(current_norm);''')

shutil.copyfile(path, path + ".bak")
open(path, "w", encoding="utf-8").write(out)
print("applied point 12 to %s (original kept as %s.bak)" % (path, path))
