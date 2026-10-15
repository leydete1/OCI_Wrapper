#!/usr/bin/env python3
"""
apply_lob_output_tag.py - points a1-a4 (2026-10-14).

Usage:  python3 apply_lob_output_tag.py <OCI_Wrapper> [--apply]

Without --apply it is a dry run: it finds the files, checks every edit
matches exactly once and that nothing else calls the two changed
functions, and writes nothing.

LOB output files (the files a SELECT writes for BLOB/CLOB columns) were
named from per-SELECT counters only, so two SELECTs wrote the same name
and the later one overwrote the earlier one's file - a response could
point at another request's data. In a normal tester run three fixtures
each write CLOB_COL_row1_clob0.txt and three write blob_output_0, each
with different content.

  a1 - lob_output_tag() (Blob_Utils.c): "YYYYMMDD_HHMMSS_NNNNNN", the
       gateway clock plus a process-wide counter. The driver takes one
       tag per SELECT (any cursor with a BLOB or CLOB column, so the
       REF CURSOR and async paths too) and logs it on the select log.
  a2 - the tag goes into every name, after the base name:
         CLOB_COL_<tag>_row1_clob0.txt          (was CLOB_COL_row1_clob0.txt)
         blob_output_<tag>_0                    (was blob_output_0)
         Adam_1_<tag>_0.jpg                     (was Adam_1_<YYYYMMDD_HHMMSS>_0.jpg,
                                                 BLOB_append_file_timestamp=1)
       With BLOB_append_file_timestamp=0 a BLOB keeps its stored name,
       unchanged - that setting means "keep the original name".
  a3 - write_blob_to_file() writes <name>.part and renames it into
       place, so a reader sees the whole file or none; a short write or
       failed close is now an error instead of being ignored.
       generate_timestamp() uses localtime_r() (localtime() is not
       thread-safe - two worker threads could swap timestamps).
  a4 - http_consumer_tester.c: mask_lob_timestamps() also masks the
       tag (_YYYYMMDD_HHMMSS_NNNNNN_ -> _TS_), so BLOB names with a
       name column compare exactly as before.

Files edited (originals kept as *.bak_a):
  src/.../driver_oracle.c, Blob_Utils.c, Blob_Utils.h, Clob_Utils.c,
  Clob_Utils.h, Testers/http_consumer_tester.c
Running it twice is refused.
"""
import os
import re
import shutil
import sys

args = [a for a in sys.argv[1:] if a != "--apply"]
APPLY = "--apply" in sys.argv[1:]
if len(args) != 1:
    sys.exit(__doc__)
ROOT = os.path.abspath(args[0])

SKIP_DIRS = {"Debug", "Release", ".git", "removed_fix22_point18", "Backup",
             "baseline", "logs"}


def find(name, subdirs):
    hits = []
    for sub in subdirs:
        base = os.path.join(ROOT, sub)
        for dp, dns, fns in os.walk(base):
            dns[:] = [d for d in dns if d not in SKIP_DIRS]
            if name in fns:
                hits.append(os.path.join(dp, name))
    if len(hits) != 1:
        sys.exit("STOP: expected exactly one %s under %s, found %d: %s"
                 % (name, "/".join(subdirs), len(hits), hits))
    return hits[0]


FILES = {
    "driver_oracle.c": find("driver_oracle.c", ["src"]),
    "Blob_Utils.c":    find("Blob_Utils.c", ["src"]),
    "Blob_Utils.h":    find("Blob_Utils.h", ["include", "src"]),
    "Clob_Utils.c":    find("Clob_Utils.c", ["src"]),
    "Clob_Utils.h":    find("Clob_Utils.h", ["include", "src"]),
    "tester":          find("http_consumer_tester.c", ["Testers"]),
}

EDITS = {k: [] for k in FILES}

# ---------------------------------------------------------------- Blob_Utils.h
EDITS["Blob_Utils.h"] += [
("write_blob_to_file doc",
"""/* Write item->blob_data (item->blob_size bytes) to output_dir/item->file_name.
 * Returns 0 on success, -1 on invalid input or file open failure. */
""",
"""/* Write item->blob_data (item->blob_size bytes) to output_dir/item->file_name.
 * 2026-10-14 (point a3): written as <file_name>.part and renamed into
 * place, so a reader sees the whole file or none of it.
 * Returns 0 on success, -1 on invalid input, open, write, close or
 * rename failure (the .part file is removed). */
"""),
("generate_timestamp doc + tag",
"""/* Format the current local time as YYYYMMDD_HHMMSS into buffer. */
void generate_timestamp(char *buffer, size_t size, oci_context_t *ctx);
""",
"""/* Format the current local time as YYYYMMDD_HHMMSS into buffer.
 * ctx is not used and may be NULL. */
void generate_timestamp(char *buffer, size_t size, oci_context_t *ctx);

/* 2026-10-14 (point a1) - one tag per SELECT that writes LOB files:
 * "YYYYMMDD_HHMMSS_NNNNNN" (local time plus a counter shared by every
 * thread of the process, 000001-999999 then round again). Two SELECTs
 * never get the same tag unless a million SELECTs start within one
 * second, so file names built from it never collide. out_size should
 * be at least LOB_OUTPUT_TAG_SIZE. */
#define LOB_OUTPUT_TAG_SIZE 32
void lob_output_tag(char *out, size_t out_size);
"""),
("build_filename_with_timestamp decl",
"""/* Build output = "<name>_<timestamp>_<idx><ext>" from original's name/ext,
 * used to disambiguate multiple BLOB output files per row. */
void build_filename_with_timestamp(const char *original, char *output,
                                    size_t out_size, int idx,
                                    oci_context_t *ctx);
""",
"""/* Build output = "<name>_<tag>_<idx><ext>" from original's name/ext.
 * 2026-10-14 (point a2): tag is the SELECT's lob_output_tag(); it used
 * to be a bare YYYYMMDD_HHMMSS taken here, which two requests in the
 * same second shared. */
void build_filename_with_timestamp(const char *original, const char *tag,
                                    char *output, size_t out_size, int idx,
                                    oci_context_t *ctx);
"""),
]

# ---------------------------------------------------------------- Blob_Utils.c
EDITS["Blob_Utils.c"] += [
("includes",
"""#include <time.h>
""",
"""#include <time.h>
#include <errno.h>     /* write_blob_to_file() - 2026-10-14, point a3 */
#include <pthread.h>   /* lob_output_tag() counter - point a1          */
"""),
("write_blob_to_file body",
"""    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", output_dir, item->file_name);

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Opening file %s", path);
    FILE *fp = fopen(path, "wb");
    if (!fp)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Error opening file %s", path);
        return -1;
    }

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Writing file %s", path);
    fwrite(item->blob_data, 1, item->blob_size, fp);
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Closing file %s", path);
    fclose(fp);

    return 0;
""",
"""    /* 2026-10-14 (point a3): written as <path>.part, then renamed into
     * place - rename() replaces the name in one step, so anything
     * reading the output folder sees the whole file or none of it. A
     * short write or a failed close used to go unnoticed. */
    char path[1024];
    char part[1040];
    snprintf(path, sizeof(path), "%s/%s", output_dir, item->file_name);
    snprintf(part, sizeof(part), "%s.part", path);

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Opening file %s", part);
    FILE *fp = fopen(part, "wb");
    if (!fp)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Error opening file %s: %s", part, strerror(errno));
        return -1;
    }

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Writing file %s", part);
    size_t written  = fwrite(item->blob_data, 1, item->blob_size, fp);
    int    close_rc = fclose(fp);
    if (written != (size_t)item->blob_size || close_rc != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Error writing file %s: %zu of %zu bytes written, "
                     "close rc=%d", part, written, (size_t)item->blob_size,
                     close_rc);
        remove(part);
        return -1;
    }

    if (rename(part, path) != 0)
    {
        logger_write(ctx->select_logger, LOG_ERROR, __func__, 0,
                     "Error renaming %s to %s: %s", part, path,
                     strerror(errno));
        remove(part);
        return -1;
    }
    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0, "Wrote file %s", path);

    return 0;
"""),
("generate_timestamp localtime_r",
"""    (void)ctx;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    strftime(buffer, size, "%Y%m%d_%H%M%S", t);
}
""",
"""    (void)ctx;
    time_t now = time(NULL);
    struct tm t;
    /* 2026-10-14: localtime_r - localtime() returns a buffer shared by
     * every thread, so two workers could read each other's time. */
    localtime_r(&now, &t);

    strftime(buffer, size, "%Y%m%d_%H%M%S", &t);
}

/* 2026-10-14 (point a1) - see Blob_Utils.h. */
static pthread_mutex_t g_lob_tag_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long   g_lob_tag_seq   = 0;

void lob_output_tag(char *out, size_t out_size)
{
    char ts[32];
    generate_timestamp(ts, sizeof(ts), NULL);

    pthread_mutex_lock(&g_lob_tag_mutex);
    g_lob_tag_seq = (g_lob_tag_seq % 999999UL) + 1;   /* 1..999999 */
    unsigned long seq = g_lob_tag_seq;
    pthread_mutex_unlock(&g_lob_tag_mutex);

    snprintf(out, out_size, "%s_%06lu", ts, seq);
}
"""),
("build_filename_with_timestamp def",
"""void build_filename_with_timestamp(const char *original, char *output,
                                    size_t out_size, int idx, oci_context_t *ctx)
{
    char name[256] = {0};
    char ext[64] = {0};
    char timestamp[32];

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "In build_filename_with_timestamp for idx=%d", idx);
    generate_timestamp(timestamp, sizeof(timestamp), ctx);
""",
"""/* 2026-10-14 (point a2): the time part is now the caller's tag (one per
 * SELECT, from lob_output_tag()) instead of a timestamp taken here. */
void build_filename_with_timestamp(const char *original, const char *tag,
                                    char *output, size_t out_size, int idx,
                                    oci_context_t *ctx)
{
    char name[256] = {0};
    char ext[64] = {0};

    logger_write(ctx->select_logger, LOG_DEBUG, __func__, 0,
                 "In build_filename_with_timestamp for idx=%d", idx);
"""),
("build_filename_with_timestamp format",
"""    snprintf(output, out_size, "%s_%s_%d%s", name, timestamp, idx, ext);
""",
"""    snprintf(output, out_size, "%s_%s_%d%s", name, tag, idx, ext);
"""),
]

# ---------------------------------------------------------------- Clob_Utils.h
EDITS["Clob_Utils.h"] += [
("build_clob_filename decl",
"""void build_clob_filename(const char *col_name, unsigned int abs_rownum,
                          int clob_index, char *output, size_t out_size,
                          oci_context_t *ctx);
""",
"""/* 2026-10-14 (point a2): the pattern is now
 * "<col_name>_<tag>_row<abs_rownum>_clob<clob_index><ext>", tag being the
 * SELECT's lob_output_tag() (Blob_Utils.h). Without it every SELECT on a
 * column wrote <col>_row1_clob0.txt and overwrote the others' files. */
void build_clob_filename(const char *col_name, const char *tag,
                          unsigned int abs_rownum, int clob_index,
                          char *output, size_t out_size,
                          oci_context_t *ctx);
"""),
]

# ---------------------------------------------------------------- Clob_Utils.c
EDITS["Clob_Utils.c"] += [
("build_clob_filename def",
"""void build_clob_filename(const char *col_name, unsigned int abs_rownum,
                          int clob_index, char *output, size_t out_size,
                          oci_context_t *ctx)
{
    const char *ext = (ctx->ini->clob_default_extension[0] != '\\0')
                      ? ctx->ini->clob_default_extension
                      : ".txt";

    snprintf(output, out_size, "%s_row%u_clob%d%s",
             col_name, abs_rownum, clob_index, ext);
}
""",
"""/* 2026-10-14 (point a2): the SELECT's tag goes after the column name -
 * see Clob_Utils.h. */
void build_clob_filename(const char *col_name, const char *tag,
                          unsigned int abs_rownum, int clob_index,
                          char *output, size_t out_size,
                          oci_context_t *ctx)
{
    const char *ext = (ctx->ini->clob_default_extension[0] != '\\0')
                      ? ctx->ini->clob_default_extension
                      : ".txt";

    snprintf(output, out_size, "%s_%s_row%u_clob%d%s",
             col_name, tag, abs_rownum, clob_index, ext);
}
"""),
]

# ---------------------------------------------------------------- driver_oracle.c
EDITS["driver_oracle.c"] += [
("cursor field",
"""    int             clob_index;
""",
"""    int             clob_index;

    /* 2026-10-14 (point a1) - this SELECT's lob_output_tag(), taken in
     * oracle_select_describe_and_define() when the result has a BLOB or
     * CLOB column; it goes into every LOB file name the cursor writes,
     * so two SELECTs never write the same file. */
    char            lob_tag[LOB_OUTPUT_TAG_SIZE];
"""),
("take the tag",
"""    for (ub4 i = 0; i < cur->col_count; i++)
    {
        if (cur->data_types[i] == SQLT_CLOB)
        {
            cur->batch_size = 1;
            break;
        }
    }
""",
"""    for (ub4 i = 0; i < cur->col_count; i++)
    {
        if (cur->data_types[i] == SQLT_CLOB)
        {
            cur->batch_size = 1;
            break;
        }
    }

    /* 2026-10-14 (point a1) - one output tag per SELECT with a LOB
     * column; logged so a file can be traced to its request. Both
     * select_open() and select_open_from_cursor() come through here. */
    for (ub4 i = 0; i < cur->col_count; i++)
    {
        if (cur->data_types[i] == SQLT_CLOB || cur->data_types[i] == SQLT_BLOB)
        {
            lob_output_tag(cur->lob_tag, sizeof(cur->lob_tag));
            logger_write(ctx->select_logger, LOG_INFO, __func__, 0,
                         "LOB output tag %s for this SELECT", cur->lob_tag);
            break;
        }
    }
"""),
("BLOB name with name column",
"""            build_filename_with_timestamp(name_val, final_name,
""",
"""            build_filename_with_timestamp(name_val, cur->lob_tag, final_name,
"""),
("BLOB fallback name",
"""        snprintf(final_name, sizeof(final_name), "%s_%d",
                 ctx->ini->BLOB_default_file_name, blob_index);
""",
"""        /* 2026-10-14 (point a2): tagged - was "<default>_<n>", which
         * every SELECT shared (blob_output_0, blob_output_1 ...). */
        snprintf(final_name, sizeof(final_name), "%s_%s_%d",
                 ctx->ini->BLOB_default_file_name, cur->lob_tag, blob_index);
"""),
("CLOB name",
"""    build_clob_filename(cur->col_names[col_idx], cur->abs_rownum, clob_index,
""",
"""    build_clob_filename(cur->col_names[col_idx], cur->lob_tag,
                         cur->abs_rownum, clob_index,
"""),
]

# ---------------------------------------------------------------- tester
EDITS["tester"] += [
("mask comment",
"""/* 2026-10-10 (point 12): replaces every _YYYYMMDD_HHMMSS_ in s with
 * _TS_, in place.""",
"""/* 2026-10-14 (point a4): also masks the gateway's LOB output tag,
 * _YYYYMMDD_HHMMSS_NNNNNN_ (the time plus a per-SELECT counter), as one
 * _TS_ - so a BLOB name taken from a name column compares exactly as it
 * did before the tag, and a CLOB name compares as <col>_TS_row1_clob0.
 *
 * 2026-10-10 (point 12): replaces every _YYYYMMDD_HHMMSS_ in s with
 * _TS_, in place."""),
("mask body",
"""        if (k == 17)
        {
            memcpy(w, "_TS_", 4);
            w += 4;
            p += 17;
            continue;
        }
""",
"""        if (k == 17)
        {
            /* point a4: a 6-digit counter and '_' after it are part of
             * the tag - checked the same way, stopping at the first
             * byte that does not fit, so nothing past a NUL is read. */
            int extra = 0;
            while (extra < 7 &&
                   (extra < 6 ? (p[17 + extra] >= '0' && p[17 + extra] <= '9')
                              : (p[17 + extra] == '_')))
                extra++;
            memcpy(w, "_TS_", 4);
            w += 4;
            p += 17 + (extra == 7 ? 7 : 0);
            continue;
        }
"""),
]

DONE_MARK = {
    "driver_oracle.c": "lob_output_tag(cur->lob_tag",
    "Blob_Utils.c": "void lob_output_tag(",
    "Blob_Utils.h": "LOB_OUTPUT_TAG_SIZE",
    "Clob_Utils.c": "const char *tag,",
    "Clob_Utils.h": "const char *tag,",
    "tester": "point a4",
}

# ---------------------------------------------------------------- reference scan
def blank_comments(text):
    """Comments replaced by spaces (newlines kept), strings kept."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c; j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1]); i = j + 1
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2); j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        elif text.startswith("//", i):
            j = text.find("\n", i); j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


CHANGED = re.compile(r"\b(build_clob_filename|build_filename_with_timestamp)\b")
edited = {os.path.realpath(p) for p in FILES.values()}
others = []
for sub in ("src", "include", "Testers"):
    for dp, dns, fns in os.walk(os.path.join(ROOT, sub)):
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        for fn in fns:
            if not fn.endswith((".c", ".h")):
                continue
            p = os.path.join(dp, fn)
            if os.path.realpath(p) in edited:
                continue
            try:
                t = blank_comments(open(p, encoding="utf-8", errors="replace").read())
            except OSError:
                continue
            for ln, line in enumerate(t.split("\n"), 1):
                if CHANGED.search(line):
                    others.append("%s:%d: %s" % (os.path.relpath(p, ROOT), ln, line.strip()))
if others:
    print("STOP: other code calls a function whose arguments change:")
    for o in others:
        print("   " + o)
    sys.exit("Nothing written. Send me those files and I will add them.")

# ---------------------------------------------------------------- apply
results = {}
for key, path in FILES.items():
    raw = open(path, "rb").read().decode("utf-8")
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")
    if DONE_MARK[key] in text:
        sys.exit("STOP: %s already has this change - nothing written" % path)
    for name, old, new in EDITS[key]:
        n = text.count(old)
        if n != 1:
            sys.exit("STOP: %s - edit '%s' matched %d times (expected 1). "
                     "Nothing written." % (path, name, n))
        text = text.replace(old, new)
    results[key] = (path, text.replace("\n", "\r\n") if crlf else text)

print("%s - %d files, all edits match:" % ("APPLYING" if APPLY else "DRY RUN", len(results)))
for key, (path, _) in results.items():
    print("   %-16s %2d edit(s)  %s" % (key, len(EDITS[key]), os.path.relpath(path, ROOT)))

if not APPLY:
    print("Dry run only - run again with --apply to write the files.")
    sys.exit(0)

for key, (path, text) in results.items():
    shutil.copyfile(path, path + ".bak_a")
    open(path, "wb").write(text.encode("utf-8"))
print("Done. Originals kept as *.bak_a. Rebuild (Clean then Build - Blob_Utils.h "
      "changed) and run Get_Test_Results.sh, then rebuild the tester.")
