#!/usr/bin/env python3
"""
apply_fix22_points_18_20.py - fix-22 points 18, 19 and 20 (2026-10-13)

Usage (from anywhere):
    python3 apply_fix22_points_18_20.py <OCI_Wrapper folder>            # dry run
    python3 apply_fix22_points_18_20.py <OCI_Wrapper folder> --apply    # do it

The dry run changes nothing: it finds every file, checks each edit can
be made exactly, and lists every other place in the tree that mentions
the Insert Template feature. Run it, read the report, then --apply.

Point 18 - remove the Insert Template feature (no callers):
  - Insert_Template_Module.c / .h are MOVED (not deleted) to
    <OCI_Wrapper>/removed_fix22_point18/.
  - Insert_Validate_Module.c: validate_insert_template() and its private
    helpers (parse_field_block, extract_tag, extract_tag_int,
    trim_inplace_v) are removed; validate_field() and every type
    validator stay - Level 2 uses them for every INSERT and UPDATE.
    This also ends the -Wunused-function warning for extract_tag_int.
  - Insert_Validate_Module.h: validate_insert_template() and the unused
    field_report_t are removed; comments updated.
  - Data_Manager_Bootstrap.c: the #include "Insert_Template_Module.h"
    line is removed.
Point 19 - ini_reader.h: BLOB_default_file_name grows from char[10] to
  char[64]; "blob_output" was being cut to "blob_outp".
Point 20 - Unit_Test_CheckPermission_Round_3.xml: comment corrected
  (expects SESSION_NOT_FOUND with session validation on).

Every edited file is kept as <file>.bak_p18. Nothing is written unless
every edit can be made; a second --apply is refused.
"""
import hashlib, os, re, shutil, sys

R3_OLD_MD5 = "be2509e3e5aba8c0a30bc234049a89d0"
R3_NEW = '<?xml version="1.0" encoding="UTF-8"?>\n\n<!--\n========================================================================\n Round 3 - CHECK_PERMISSION, unknown/never-authenticated session_id\n No real session needed for this one - it proves an unknown session_id\n is refused cleanly rather than crashing or returning ALLOWED. Safe to\n run via the standard Run.sh HTTP tester or File Consumer as-is, no\n substitution needed.\n\n Expected result depends on session_validation_enabled (config.ini):\n   1 (the tester\'s setting): error_code SESSION_NOT_FOUND - dispatcher\n     session validation rejects the request before CHECK_PERMISSION\n     runs, so the permission cache is never consulted.\n   0: <result>DENIED</result>, and in security_Data_Manager.log\n     "no cached permission list (unknown, expired, or\n     never-authenticated session)" - the cache-miss path, separate\n     from Round 2\'s real-but-ungranted-permission denial.\n\n 2026-10-13 (fix-22 point 20): this comment used to describe only the\n second case.\n========================================================================\n-->\n\n<request version="1.0">\n    <external_audit_id>audit_stub_check_permission_round_3</external_audit_id>\n    <session_id>00000000-0000-0000-0000-000000000000</session_id>\n    <transaction required="0">\n        <operation type="CHECK_PERMISSION">\n            <permission_code>CUSTOMER.READ</permission_code>\n        </operation>\n    </transaction>\n</request>\n'

NAMES = ["Insert_Template_Module", "get_insert_template",
         "parse_template_request", "validate_insert_template",
         "field_report_t", "template_request_t", "resolve_insert_default"]
SKIP_DIRS = {"Backup", "removed_fix22_point18", ".git", ".metadata"}

def die(msg):
    sys.exit("NOT APPLIED: " + msg + " - nothing changed")

# ----------------------------------------------------------------------
def find_one(root, name):
    hits = []
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS and x != "Debug"]
        if name in files:
            hits.append(os.path.join(d, name))
    if len(hits) != 1:
        die("expected exactly one %s under %s, found %d: %s"
            % (name, root, len(hits), hits))
    return hits[0]

def blank_comments(src):
    """Comments replaced by spaces (newlines kept), so line numbers hold."""
    return re.sub(r"/\*.*?\*/|//[^\n]*",
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), src, flags=re.S)

def scan_refs(root, handled):
    """(code_refs, comment_only) - code_refs are real uses in C code or
    build files; comment_only are files that merely mention the names
    in comments (history notes, left as they are)."""
    code_refs, comment_only = [], {}
    pat = re.compile(r"\b(?:" + "|".join(NAMES) + r")\b")
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
        for f in files:
            if not (f.endswith((".c", ".h", ".mk", ".sh"))
                    or f.lower() == "makefile"):
                continue
            p = os.path.join(d, f)
            if os.path.abspath(p) in handled:
                continue
            try:
                text = open(p, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            is_c = f.endswith((".c", ".h"))
            code = blank_comments(text) if is_c else text
            raw_lines = text.splitlines()
            for i, ln in enumerate(code.splitlines(), 1):
                if pat.search(ln):
                    code_refs.append((p, i, raw_lines[i-1].strip()))
            if is_c and pat.search(text) and not pat.search(code):
                comment_only[p] = len(pat.findall(text))
    return code_refs, comment_only

# ----------------------------------------------------------------------
def func_span(src, name):
    """Start/end of a top-level C function definition, including the
    comment block directly above it (one blank line allowed between)."""
    m = list(re.finditer(r"(?m)^[A-Za-z_][^\n;{}]*\b" + re.escape(name) +
                         r"\s*\([^;{]*\)\s*\{", src))
    if len(m) != 1:
        die("definition of %s() matched %d times" % (name, len(m)))
    start = m[0].start()
    # matching brace (skips strings, chars and comments)
    i = m[0].end() - 1
    depth = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            q = c; i += 1
            while i < n and src[i] != q:
                i += 2 if src[i] == "\\" else 1
        elif src.startswith("/*", i):
            i = src.index("*/", i) + 1
        elif src.startswith("//", i):
            i = src.index("\n", i)
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    else:
        die("no closing brace for %s()" % name)
    end = i + 1
    if src.startswith("\n", end):
        end += 1
    # comment block above
    lines_before = src[:start].split("\n")
    lines_before.pop()                      # empty piece before start
    k = len(lines_before)
    blank_used = False
    j = k - 1
    if j >= 0 and lines_before[j].strip() == "":
        j -= 1; blank_used = True
    top = j + 1
    while j >= 0:
        s = lines_before[j].strip()
        if s.startswith("/*") or s.startswith("*") or s.endswith("*/"):
            top = j; j -= 1
        else:
            break
    if top <= (k - 1 - (1 if blank_used else 0)):
        start = len("\n".join(lines_before[:top])) + (1 if top > 0 else 0)
    # swallow one blank line that followed
    if src.startswith("\n", end):
        end += 1
    return start, end

def strip_comments(src):
    return re.sub(r"/\*.*?\*/|//[^\n]*", " ", src, flags=re.S)

def remove_funcs(src, names):
    for nm in names:
        s, e = func_span(src, nm)
        src = src[:s] + src[e:]
    return src

def replace_once(src, old, new, what, required=True):
    n = src.count(old)
    if n == 1:
        return src.replace(old, new), None
    if required:
        die("%s matched %d times" % (what, n))
    return src, "comment not updated (text differs): " + what

# ----------------------------------------------------------------------
NEW_C_HEADER = """/*
 * Insert_Validate_Module.c
 *
 * validate_field(): checks one value against its column's metadata
 * (type, length, precision/scale, nullability). Level 2 calls it for
 * every INSERT and UPDATE field (level2_validate_insert/_update, with
 * metadata from metadata_cache). The rules are listed in the header.
 * Makes no database calls.
 *
 * 2026-10-13 (fix-22 point 18): the Insert Template feature this file
 * started in (validate_insert_template() and its XML helpers) had no
 * callers and was removed, with Insert_Template_Module.c/.h.
 */"""

def edit_validate_c(src):
    m = re.match(r"\s*/\*.*?\*/", src, re.S)
    if not m or "Insert Template Validator" not in m.group(0):
        die("Insert_Validate_Module.c header comment not recognised")
    src = src[:m.start()] + ("\n" if src.startswith("\n") else "") + NEW_C_HEADER + src[m.end():]
    src = remove_funcs(src, ["validate_insert_template", "parse_field_block",
                             "extract_tag_int", "extract_tag", "trim_inplace_v"])
    code = strip_comments(src)
    for nm in ["validate_insert_template", "parse_field_block",
               "extract_tag", "extract_tag_int", "trim_inplace_v"]:
        if re.search(r"\b%s\s*\(" % nm, code):
            die("%s still referenced in Insert_Validate_Module.c after removal" % nm)
    if "validate_field" not in src:
        die("validate_field missing after edit - refusing")
    bare = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', '""', strip_comments(src))
    if bare.count("{") != bare.count("}"):
        die("brace count differs after Insert_Validate_Module.c edit")
    return src

def edit_validate_h(src):
    notes = []
    # field_report_t
    m = list(re.finditer(r"typedef struct \{[^{}]*\}\s*field_report_t;\s*\n", src))
    if len(m) != 1:
        die("field_report_t typedef matched %d times" % len(m))
    src = src[:m[0].start()] + src[m[0].end():]
    # validate_insert_template comment + declaration (+ Public API banner)
    m = list(re.finditer(
        r"(/\* -{20,} \*/\s*\n/\*\s*Public API\s*\*/\s*\n/\* -{20,} \*/\s*\n\s*)?"
        r"/\*\s*\n \* validate_insert_template\(\)\n.*?\*/\s*\n"
        r"int validate_insert_template\([^;]*\);\s*\n", src, re.S))
    if len(m) != 1:
        die("validate_insert_template() declaration block matched %d times" % len(m))
    src = src[:m[0].start()] + src[m[0].end():]
    if "validate_insert_template(" in src.replace("validate_insert_template()", ""):
        pass
    # comments
    src, w = replace_once(src,
        " * Stage 2 - Insert Template Validator\n"
        " * -------------------------------------\n"
        " * Accepts a fully-formed <Insert Template> XML (produced by Stage 1\n"
        " * and then populated by the caller with <insert_value> entries) and\n"
        " * validates every value against its declared Oracle column metadata.\n",
        " * validate_field() - checks one value against its column's metadata.\n"
        " * Level 2 (level2_validate_insert/_update) builds a parsed_field_t\n"
        " * from the request and metadata_cache and calls it for every field.\n"
        " * (2026-10-13, fix-22 point 18: the Insert Template feature this\n"
        " * header was written for had no callers and was removed.)\n",
        "header introduction", required=False)
    notes += [w] if w else []
    m = re.search(r" \* Return value\n \*   validate_insert_template\(\) returns.*?(?=\n \*/)", src, re.S)
    if m:
        src = src[:m.start()] + (" * Return value\n"
              " *   validate_field() returns FIELD_VALID, or the failure kind with\n"
              " *   a human-readable message in msg.") + src[m.end():]
    else:
        notes.append("comment not updated (text differs): header 'Return value' section")
    src, w = replace_once(src,
        " * top-level comment block - this is the single place both\n"
        " * validate_insert_template() (below, XML-string path) and\n"
        " * level2_validate_insert() (OCI_Level2_Parser.c, insert_request_t\n"
        " * path) ultimately call.\n",
        " * top-level comment block. Called by Level 2 for every INSERT and\n"
        " * UPDATE field.\n",
        "validate_field() comment", required=False)
    notes += [w] if w else []
    if re.search(r"\bfield_report_t\b|\bvalidate_insert_template\s*\(", strip_comments(src)):
        die("Insert_Validate_Module.h still declares removed items")
    return src, notes

def edit_bootstrap(src):
    m = list(re.finditer(r'(?m)^[ \t]*#include[ \t]+"Insert_Template_Module\.h"[^\n]*\n', src))
    if len(m) != 1:
        die('#include "Insert_Template_Module.h" in Data_Manager_Bootstrap.c matched %d times' % len(m))
    return src[:m[0].start()] + src[m[0].end():]

def edit_ini_h(src):
    m = list(re.finditer(r"char\s+BLOB_default_file_name\s*\[\s*10\s*\]\s*;", src))
    if len(m) != 1:
        die("char BLOB_default_file_name[10]; matched %d times in ini_reader.h" % len(m))
    new = ("char BLOB_default_file_name      [64];   /* fix-22 point 19 (2026-10-13):\n"
           "                                               was [10] - load_ini cut\n"
           "                                               'blob_output' to 'blob_outp' */")
    return src[:m[0].start()] + new + src[m[0].end():]

# ----------------------------------------------------------------------
def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--apply"):
        sys.exit(__doc__)
    root = os.path.abspath(sys.argv[1])
    apply = len(sys.argv) == 3
    if not os.path.isdir(root):
        die("%s is not a folder" % root)
    if os.path.isdir(os.path.join(root, "removed_fix22_point18")):
        sys.exit("already applied (removed_fix22_point18 exists) - nothing to do")

    f_tc  = find_one(root, "Insert_Template_Module.c")
    f_th  = find_one(root, "Insert_Template_Module.h")
    f_vc  = find_one(root, "Insert_Validate_Module.c")
    f_vh  = find_one(root, "Insert_Validate_Module.h")
    f_bs  = find_one(root, "Data_Manager_Bootstrap.c")
    f_ini = find_one(root, "ini_reader.h")
    f_r3  = os.path.join(root, "test_data", "consumers", "file", "Input_XML",
                         "Unit_Test_CheckPermission_Round_3.xml")
    if not os.path.isfile(f_r3):
        die("fixture not found: " + f_r3)
    if hashlib.md5(open(f_r3, "rb").read()).hexdigest() != R3_OLD_MD5:
        die("Unit_Test_CheckPermission_Round_3.xml is not the fix-22 version")

    rd = lambda p: open(p, encoding="utf-8").read()
    new_vc = edit_validate_c(rd(f_vc))
    new_vh, notes = edit_validate_h(rd(f_vh))
    new_bs = edit_bootstrap(rd(f_bs))
    new_ini = edit_ini_h(rd(f_ini))

    handled = {os.path.abspath(p) for p in (f_tc, f_th, f_vc, f_vh)}
    refs, comment_only = scan_refs(root, handled)
    other = []
    for p, i, ln in refs:
        if os.path.abspath(p) == os.path.abspath(f_bs) and "Insert_Template_Module.h" in ln:
            continue
        other.append((p, i, ln))
    code_refs = [r for r in other if r[0].endswith((".c", ".h"))
                 and os.sep + "Debug" + os.sep not in r[0]]
    build_refs = [r for r in other if r not in code_refs]

    print("Files:")
    for p in (f_tc, f_th, f_vc, f_vh, f_bs, f_ini, f_r3):
        print("  " + os.path.relpath(p, root))
    for n in notes:
        print("  note: " + n)
    if comment_only:
        print("\nMentioned in comments only (history notes - left unchanged):")
        for p, n in sorted(comment_only.items()):
            print("  %s (%d)" % (os.path.relpath(p, root), n))
    if build_refs:
        print("\nBuild files that mention the removed module (generated ones are\n"
              "rewritten when Eclipse rebuilds; hand-written ones: remove the line):")
        for p, i, ln in build_refs:
            print("  %s:%d: %s" % (os.path.relpath(p, root), i, ln))
    if code_refs:
        print("\nSource files that still use the removed feature:")
        for p, i, ln in code_refs:
            print("  %s:%d: %s" % (os.path.relpath(p, root), i, ln))
        die("other source files use the Insert Template feature - send this output to Claude")

    if not apply:
        print("\nDry run OK - every edit can be made. Re-run with --apply.")
        return

    for p, txt in ((f_vc, new_vc), (f_vh, new_vh), (f_bs, new_bs), (f_ini, new_ini)):
        shutil.copyfile(p, p + ".bak_p18")
        open(p, "w", encoding="utf-8").write(txt)
    shutil.copyfile(f_r3, f_r3 + ".bak_p18")
    open(f_r3, "w", encoding="utf-8").write(R3_NEW)
    dest = os.path.join(root, "removed_fix22_point18")
    os.makedirs(dest)
    shutil.move(f_tc, dest)
    shutil.move(f_th, dest)
    print("\nApplied. Edited files kept as *.bak_p18; Insert_Template_Module.c/.h\n"
          "moved to removed_fix22_point18/. In Eclipse: refresh the project (F5)\n"
          "so the build drops the moved file, then rebuild.")

main()
