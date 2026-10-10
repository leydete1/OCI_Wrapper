#!/usr/bin/env python3
"""
apply_dispatcher_sid.py - point 2 (2026-10-10), dispatcher.c part.

Usage:  python3 apply_dispatcher_sid.py <path/to/dispatcher.c>

Makes two edits in place and keeps the original as dispatcher.c.bak:
  1. process_xml_file() starts with logger_clear_sid(), so a request's
     first log lines (Level 1 parse) never carry the previous request's
     session id.
  2. Updates the comment above logger_set_sid() to say where the sid is
     now cleared.

Each edit must match exactly once, or nothing is written and the script
says which one did not match. Running it twice is refused (already applied).
"""
import shutil
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
path = sys.argv[1]
src = open(path, encoding="utf-8").read()

if "2026-10-10 - start every request with no trace sid" in src:
    sys.exit("already applied - nothing to do")

edits = [
    ("process_xml_file() opening",
     """                      response_object_t  *resp)
{
    /* xml/len aliases keep the rest of this function's body (which""",
     """                      response_object_t  *resp)
{
    /* 2026-10-10 - start every request with no trace sid. Until the
     * session_id is known (logger_set_sid() below), this thread's log
     * lines - the Level 1 parse lines included - carried whatever sid
     * the thread's previous request left behind, because the HTTP
     * worker never cleared it (worker.c does). Cleared here so no
     * caller can leak one into the next request's lines. */
    logger_clear_sid();

    /* xml/len aliases keep the rest of this function's body (which"""),
    ("comment above logger_set_sid()",
     """         * automatically. Cleared in worker.c once this request is
         * fully done, so it never leaks into the next request handled
         * by the same thread. */
        logger_set_sid(new_request.session_id);""",
     """         * automatically. Cleared at the start of this function and by
         * each worker loop (worker.c, http_worker_pool.c) once the
         * request is fully done, so it never leaks into the next
         * request handled by the same thread. */
        logger_set_sid(new_request.session_id);"""),
]

out = src
for name, old, new in edits:
    n = out.count(old)
    if n != 1:
        sys.exit("NOT APPLIED: '%s' matched %d times (expected 1) - "
                 "dispatcher.c left unchanged" % (name, n))
    out = out.replace(old, new)

shutil.copyfile(path, path + ".bak")
open(path, "w", encoding="utf-8").write(out)
print("applied 2 edits to %s (original kept as %s.bak)" % (path, path))
