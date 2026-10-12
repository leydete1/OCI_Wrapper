#!/usr/bin/env python3
"""
apply_point16_fetch_batch_size.py - point 16 (2026-10-11),
Execute_Query_Batch_Module.c.   (v3: bc.fetch_count or plain fetch_count)

Usage:  python3 apply_point16_fetch_batch_size.py <path/to/Execute_Query_Batch_Module.c>

<fetch_batch_size> in a SELECT response reported the configured
query_fetch_batch_size (fetch_count) rather than the batch size the
driver actually used. select_open() lowers the batch size to 1 whenever
a CLOB column is present (the OCI array-fetch restriction) and returns
the size it really used in batch_size_driver, so a CLOB select fetched
one row at a time while its response said 5. Wrong since the Phase 2b
driver switch (2026-09-15); the 18 Aug baselines still show 1.

One edit: the xml_append() that writes <fetch_batch_size> now passes
batch_size_driver instead of fetch_count. Nothing else changes.

v3: your file no longer has batch_ctx_t - fetch_count is a plain local
(was bc.fetch_count) - so the match now accepts either form.

v2: the first version matched the exact spacing of one copy of the file
and found nothing in yours. This one matches the statement whatever the
spacing, tabs or line endings, and if it still finds nothing it prints
every line that mentions fetch_batch_size so we can see why.

Edits the file in place and keeps the original as .bak. Refuses to run
twice or if the statement is not found exactly once.
"""
import re
import shutil
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
path = sys.argv[1]
raw = open(path, "rb").read()
src = raw.decode("utf-8")
nl = "\r\n" if "\r\n" in src else "\n"

if "point 16" in src:
    sys.exit("already applied - nothing to do")

# xml_append( xml , "<fetch_batch_size>%u</fetch_batch_size>\n" , bc.fetch_count ) ;
pat = re.compile(
    r'(?P<indent>[ \t]*)xml_append\s*\(\s*xml\s*,\s*'
    r'"<fetch_batch_size>%(?P<fmt>[a-z]+)</fetch_batch_size>\\n"\s*,\s*'
    r'(?:\(\s*\w+\s*\)\s*)?(?:bc\s*\.\s*)?fetch_count\s*\)\s*;')

matches = list(pat.finditer(src))
if len(matches) != 1:
    print("NOT APPLIED: the <fetch_batch_size> xml_append matched %d times "
          "(expected 1) - file left unchanged" % len(matches))
    print("Lines mentioning fetch_batch_size / fetch_count:")
    for i, line in enumerate(src.splitlines(), 1):
        if "fetch_batch_size" in line or "fetch_count" in line:
            print("  %5d: %s" % (i, line.rstrip()))
    sys.exit(1)

if not re.search(r'\bint\s+batch_size_driver\b', src):
    sys.exit("NOT APPLIED: no 'int batch_size_driver' declaration found - "
             "file left unchanged")

m = matches[0]
ind = m.group("indent")
new = (ind + "/* 2026-10-11 (point 16): the batch size actually used, as" + nl +
       ind + " * select_open() returned it - 1 whenever a CLOB column is present" + nl +
       ind + " * (OCI array-fetch restriction). fetch_count is only what was" + nl +
       ind + " * asked for (query_fetch_batch_size), so a CLOB select used to" + nl +
       ind + " * report 5 while fetching one row at a time. */" + nl +
       ind + 'xml_append(xml, "<fetch_batch_size>%d</fetch_batch_size>\\n",' + nl +
       ind + "           batch_size_driver);")

out = src[:m.start()] + new + src[m.end():]
shutil.copyfile(path, path + ".bak")
open(path, "wb").write(out.encode("utf-8"))
line_no = src.count("\n", 0, m.start()) + 1
print("applied point 16 to %s at line %d (original kept as %s.bak)"
      % (path, line_no, path))
