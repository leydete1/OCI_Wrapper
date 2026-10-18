#!/usr/bin/env python3
"""
apply_neutral_context.py - driver_mssql point M2, items N1-N3 (2026-10-17)

Makes the shared context (oci_context_t, lob_item_t in Connection.h)
vendor-neutral, so core code no longer needs Oracle's headers.

Usage:
    python3 apply_neutral_context.py <OCI_Wrapper>            # dry run (default)
    python3 apply_neutral_context.py <OCI_Wrapper> --apply    # write changes

What it changes
  N1  include/Connection.h
        - '#include <oci.h>' becomes '#include <stddef.h>'
        - the five handle fields envhp, errhp, srvhp, svchp, authp become
          'void *' (names unchanged, so every ctx->xxx use still compiles
          and struct copies behave exactly as before)
      Oracle-side files (fixed list below, plus any file under src/Drivers/
      Oracle/ and src/test/) that use an OCI identifier get an explicit
      '#include <oci.h>' - before their first #include "..." line (.c),
      or straight after the include guard's #define (.h).
      Address-of sites '&x->envhp', '&x.lob_loc' etc. in those files that
      are NOT already cast get the cast to the old type, e.g.
          &ctx->envhp      ->  (OCIEnv **)&ctx->envhp
          &item.lob_loc    ->  (OCILobLocator **)&item.lob_loc
      This gives the expression exactly the type it had before, so the
      call is unchanged. Receivers named 'pool' and 'slot' are skipped:
      those are pool_slot_t / oci_pool_handle_t in Connection_Pool.h,
      which keep their OCI types.
  N2  include/Connection.h, lob_item_t
        ub4 blob_size      ->  uint32_t blob_size
        OCILobLocator *lob_loc  ->  void *lob_loc
  N3  src/core/Response_Writer.c   (ub4)  ->  (uint32_t)

Checks (dry run and --apply alike)
  - every fixed edit must match exactly once, otherwise nothing is written;
  - every .c/.h under src/ and every project header under include/
    (a header is "project" if some project file includes it with quotes)
    is scanned, comments and strings removed, for OCI identifiers
    (OCIxxx types/calls, OCI_SUCCESS/HTYPE/ATTR... macros, sword/ub4/
    sb4/dvoid/oratext ...). A file outside the Oracle side that uses one
    is listed as a LEAK and --apply refuses until it is decided;
  - refuses to run twice (any *.bak_m2 present).

Backups: every changed file is first copied to <file>.bak_m2.
The script compiles nothing; run m2_compile_proof.py afterwards.
"""
import os
import re
import shutil
import sys

BAK = ".bak_m2"
TAG = "M2 2026-10-17"

# ---------------------------------------------------------------- Oracle side
ORACLE_FILES = {
    "src/execute/driver_oracle.c",
    "src/infra/Connection.c",
    "src/infra/Connection_Pool.c",
    "include/Connection_Pool.h",
    "include/Unit_Test_Module.h",
    "include/driver_oracle.h",
}
ORACLE_DIRS = ("src/Drivers/Oracle/", "src/test/")
# app wiring: allowed to include <oci.h> if it has to, but reported (NOTE)
WIRING_FILES = {"src/app/Data_Manager_Bootstrap.c"}


def is_oracle_side(rel):
    return rel in ORACLE_FILES or rel in WIRING_FILES or rel.startswith(ORACLE_DIRS)


# ---------------------------------------------------------------- fixed edits
CONN_H = "include/Connection.h"
RESP_W = "src/core/Response_Writer.c"

FIXED = {
    CONN_H: [
        ("#include <oci.h>\n",
         "#include <stddef.h>   /* %s: was <oci.h> - context is vendor-neutral */\n" % TAG),
        ("    OCIEnv     *envhp;\n"
         "    OCIError   *errhp;\n"
         "    OCIServer  *srvhp;\n"
         "    OCISvcCtx  *svchp;\n"
         "    OCISession *authp;\n",
         "    /* %s (N1): driver-owned handles, opaque to core */\n"
         "    /* Oracle: OCIEnv*, OCIError*, OCIServer*, OCISvcCtx*,      */\n"
         "    /* OCISession* - the Oracle files cast them where needed.   */\n"
         "    void       *envhp;\n"
         "    void       *errhp;\n"
         "    void       *srvhp;\n"
         "    void       *svchp;\n"
         "    void       *authp;\n" % TAG),
        ("    ub4            blob_size;\n",
         "    uint32_t       blob_size;        /* %s (N2): was ub4 */\n" % TAG),
        ("    OCILobLocator *lob_loc;\n",
         "    void          *lob_loc;          /* %s (N2): OCILobLocator* on Oracle */\n" % TAG),
    ],
    RESP_W: [
        ("item.blob_size                = (ub4)fld->blob_detail.file_size;",
         "item.blob_size                = (uint32_t)fld->blob_detail.file_size;"),
    ],
}

CAST_TYPE = {
    "envhp": "OCIEnv **", "errhp": "OCIError **", "srvhp": "OCIServer **",
    "svchp": "OCISvcCtx **", "authp": "OCISession **",
    "lob_loc": "OCILobLocator **",
}
SKIP_RECEIVERS = {"pool", "slot"}

# &recv->field  /  &recv.field   (recv = simple name, maybe arr[i])
ADDR_RE = re.compile(
    r"&\s*(?P<recv>[A-Za-z_]\w*(?:\[[^\]\n]*\])?)\s*(?P<op>->|\.)\s*"
    r"(?P<field>envhp|errhp|srvhp|svchp|authp|lob_loc)\b")
CAST_BEFORE_RE = re.compile(r"\)\s*$")

OCI_RE = re.compile(
    r"\bOCI[A-Z][a-z]\w*\b"
    r"|\bOCI_(?:DEFAULT|SUCCESS|SUCCESS_WITH_INFO|ERROR|NO_DATA|INVALID_HANDLE|"
    r"NEED_DATA|STILL_EXECUTING|CONTINUE|THREADED|OBJECT|NTV_SYNTAX|"
    r"COMMIT_ON_SUCCESS|DESCRIBE_ONLY|ONE_PIECE|FIRST_PIECE|NEXT_PIECE|LAST_PIECE|"
    r"(?:HTYPE|DTYPE|ATTR|CRED|SESSGET|SESSRLS|STMT|TRANS|FETCH|LOB|IND|SPC|"
    r"TEMP|DURATION|PTYPE|TYPECODE)_\w+)\b"
    r"|\b(?:sword|sb[1248]|ub[1248]|oratext|OraText|dvoid)\b"
    r"|\bSQLT_\w+\b")
INC_OCI_RE = re.compile(r'^\s*#\s*include\s*<oci\.h>', re.M)
# Oracle client headers that ship in include/ - never "project" headers
ORA_HDR_RE = re.compile(r"^(oci|ora|ori|orl|oro|ort|occi|nz|odci|jzn|xml[a-z]|xa\.h)")
QUOTED_INC_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def strip_c(src):
    """Remove comments, string and char literals; keep line breaks."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * src.count("\n", i, j))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_preproc_includes(s):
    return re.sub(r'^\s*#\s*include.*$', "", s, flags=re.M)


def read(p):
    with open(p, encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def write(p, s):
    with open(p, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(s)


def project_files(root):
    """All .c/.h under src/, plus include/ headers some project file
    includes with quotes (transitively). Oracle client headers are never
    quoted-included, so they drop out."""
    srcs = []
    for d, _, fs in os.walk(os.path.join(root, "src")):
        for f in fs:
            if f.endswith((".c", ".h")):
                srcs.append(os.path.relpath(os.path.join(d, f), root))
    inc_dir = os.path.join(root, "include")
    inc_names = {}
    if os.path.isdir(inc_dir):
        for d, _, fs in os.walk(inc_dir):
            for f in fs:
                if f.endswith(".h"):
                    rel = os.path.relpath(os.path.join(d, f), root)
                    inc_names.setdefault(os.path.relpath(os.path.join(d, f), inc_dir), rel)
    seen, todo, headers = set(srcs), list(srcs), set()
    while todo:
        rel = todo.pop()
        for name in QUOTED_INC_RE.findall(read(os.path.join(root, rel))):
            hit = inc_names.get(name) or inc_names.get(os.path.basename(name))
            if hit and ORA_HDR_RE.match(os.path.basename(hit)):
                continue
            if hit and hit not in seen:
                seen.add(hit)
                headers.add(hit)
                todo.append(hit)
    return sorted(srcs) + sorted(headers)


def apply_fixed(root, contents, errors):
    for rel, edits in FIXED.items():
        p = os.path.join(root, rel)
        if rel not in contents:
            if not os.path.isfile(p):
                errors.append("%s: file not found" % rel)
                continue
            contents[rel] = read(p)
        s = contents[rel]
        for old, new in edits:
            k = s.count(old)
            first = old.strip().splitlines()[0]
            if k != 1:
                errors.append("%s: expected 1 match, found %d: %r" % (rel, k, first))
                continue
            s = s.replace(old, new)
            print("  EDIT  %-34s %s" % (rel, first))
        contents[rel] = s


def add_cast_sites(rel, s, report):
    """Cast uncast &recv->field sites. Works on the raw text but only at
    positions that are code (not comment/string) in the stripped copy."""
    out, last, n = [], 0, 0
    comment_spans = comment_ranges(s)
    for m in ADDR_RE.finditer(s):
        if in_spans(m.start(), comment_spans):
            continue
        # unary '&' only: skip '&&' and binary 'a & b'
        prev = s[:m.start()].rstrip()
        if s[m.start() + 1:m.start() + 2] == "&" or (prev and prev[-1] == "&"):
            continue
        if prev and prev[-1] not in "(,=!?:{};" and not re.search(r"\breturn$", prev):
            continue
        recv_base = re.match(r"[A-Za-z_]\w*", m.group("recv")).group(0)
        line = s.count("\n", 0, m.start()) + 1
        before = s[max(0, m.start() - 40):m.start()]
        if CAST_BEFORE_RE.search(before):
            continue                                   # already cast
        if recv_base in SKIP_RECEIVERS:
            report.append("  KEEP  %s:%d  %s  (pool/slot types stay OCI)" % (rel, line, m.group(0)))
            continue
        cast = "(%s)" % CAST_TYPE[m.group("field")]
        out.append(s[last:m.start()])
        out.append(cast + m.group(0))
        last = m.end()
        n += 1
        report.append("  CAST  %s:%d  %s -> %s%s" % (rel, line, m.group(0), cast, m.group(0)))
    out.append(s[last:])
    return "".join(out), n


def comment_ranges(s):
    spans, i, n = [], 0, len(s)
    while i < n:
        if s.startswith("/*", i):
            j = s.find("*/", i + 2)
            j = n if j < 0 else j + 2
            spans.append((i, j))
            i = j
        elif s.startswith("//", i):
            j = s.find("\n", i)
            j = n if j < 0 else j
            spans.append((i, j))
            i = j
        elif s[i] in "\"'":
            q, j = s[i], i + 1
            while j < n and s[j] != q and s[j] != "\n":
                j += 2 if s[j] == "\\" else 1
            spans.append((i, j + 1))
            i = j + 1
        else:
            i += 1
    return spans


def in_spans(pos, spans):
    for a, b in spans:
        if a <= pos < b:
            return True
        if a > pos:
            return False
    return False


def add_oci_include(rel, s):
    line = "#include <oci.h>   /* %s (N1): Oracle side - Connection.h no longer pulls it in */\n" % TAG
    if rel.endswith(".h"):
        m = re.search(r'^\s*#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1\b[^\n]*\n', s, re.M)
        if m:
            return s[:m.end()] + "\n" + line + s[m.end():], "after include guard"
    m = re.search(r'^[ \t]*#[ \t]*include[ \t]*"', s, re.M)
    if m:
        return s[:m.start()] + line + s[m.start():], "before first #include \"...\""
    m = re.search(r'^[ \t]*#[ \t]*include', s, re.M)
    if m:
        return s[:m.start()] + line + s[m.start():], "before first #include"
    return line + s, "at top of file"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    do_apply = "--apply" in sys.argv[1:]
    if len(args) != 1 or not os.path.isdir(args[0]):
        sys.exit(__doc__)
    root = os.path.abspath(args[0])

    files = project_files(root)
    for rel in files + list(FIXED):
        if os.path.exists(os.path.join(root, rel) + BAK):
            sys.exit("REFUSED: %s%s exists - M2 has already been applied "
                     "(restore the .bak_m2 files first to run it again)" % (rel, BAK))

    print("=== %s  (%s)" % (root, "APPLY" if do_apply else "DRY RUN"))
    print("\n--- fixed edits (N1/N2 Connection.h, N3 Response_Writer.c)")
    contents, errors = {}, []
    apply_fixed(root, contents, errors)

    print("\n--- scan: %d project files" % len(files))
    leaks, notes, report, changed_extra = [], [], [], set()
    for rel in files:
        s = contents.get(rel) or read(os.path.join(root, rel))
        code = strip_preproc_includes(strip_c(s))
        hits = sorted(set(OCI_RE.findall(code)))
        if rel in FIXED and rel == CONN_H:
            if hits:
                errors.append("%s still uses OCI after the edit: %s" % (rel, ", ".join(hits)))
            continue
        if not hits and not INC_OCI_RE.search(s):
            continue
        if not is_oracle_side(rel):
            leaks.append("  LEAK  %-40s %s%s" % (
                rel, ", ".join(hits[:8]) + (" ..." if len(hits) > 8 else ""),
                "  [includes <oci.h> directly]" if INC_OCI_RE.search(s) else ""))
            continue
        if rel in WIRING_FILES:
            notes.append("  NOTE  %s uses OCI (%s) - app wiring, allowed for now"
                         % (rel, ", ".join(hits[:6])))
        s2, ncast = add_cast_sites(rel, s, report)
        if ncast:
            s = s2
            changed_extra.add(rel)
        if hits and not INC_OCI_RE.search(s):
            s, where = add_oci_include(rel, s)
            report.append("  INCL  %-40s <oci.h> %s" % (rel, where))
            changed_extra.add(rel)
        contents[rel] = s

    for l in report:
        print(l)
    for l in notes:
        print(l)
    if leaks:
        print("\n--- LEAKS: core files that use OCI identifiers (must be decided before --apply)")
        for l in leaks:
            print(l)
    if errors:
        print("\n--- ERRORS")
        for e in errors:
            print("  " + e)

    to_write = sorted(set(FIXED) | changed_extra)
    print("\n--- files to change: %d" % len(to_write))
    for rel in to_write:
        print("  " + rel)

    if errors or leaks:
        print("\nNOT APPLIED: fix the errors / decide the leaks above first.")
        sys.exit(1)
    if not do_apply:
        print("\nDry run only - nothing written. Re-run with --apply.")
        return
    for rel in to_write:
        p = os.path.join(root, rel)
        shutil.copy2(p, p + BAK)
    for rel in to_write:
        write(os.path.join(root, rel), contents[rel])
    print("\nAPPLIED - backups are <file>%s. Next: m2_compile_proof.py %s" % (BAK, root))


if __name__ == "__main__":
    main()
