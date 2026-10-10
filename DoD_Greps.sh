#!/bin/bash
# DoD_Greps.sh - Oracle dialect extraction, Definition-of-done checks (v2).
#
# Run from the project root:  cd ~/eclipse-workspace/OCI_Wrapper && ./DoD_Greps.sh
# Output goes to the screen and to DoD_Greps.log (in the project root).
#
# v2 (2026-10-08), after the first run:
#   - Comments are ignored. Each file is copied with its /* */ and //
#     comments blanked out (line numbers kept, string literals kept),
#     and the greps run on the copies. Comments that still name OCI
#     functions are history, not dependencies.
#   - The Oracle client headers that live in include/ (oci*.h, or*.h,
#     occi*.h, odci.h, jzn*.h, nz*.h, xa.h, ldap.h) are skipped.
#   - OCI_DEPENDENCY_LIST / OCI_OBJECT_REF / OCI_FIELD_REF are the SQL
#     parser's own type names, not OCI; skipped like OCI_SESSION.
#   - src/test/ is test code, skipped like Unit_Test_Module.
#   - Grep 2's ":%d" placeholder pattern no longer matches JSON format
#     strings such as \"ttl_seconds\":%d.
#   - Grep 0 accepts harness names with underscores and the scripts and
#     logs that live in that folder.
#
# Each check prints its hits; "none" means it passed. A hit is either a
# real leftover or an entry for dod_allowlist.txt (one exact output line
# per entry). The allow-list is created empty if missing.

cd "$(dirname "$0")" || exit 1
[ -d src ] && [ -d include ] || { echo "Run from the project root (src/ and include/ not found)"; exit 1; }
[ -f dod_allowlist.txt ] || : > dod_allowlist.txt

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src" "$TMP/include"

# Copy every .c/.h with comments blanked, preserving paths and line numbers.
python3 - "$TMP" <<'PYEOF'
import os, sys
out = sys.argv[1]
VENDOR = ('oci', 'or', 'occi', 'odci', 'jzn', 'nz', 'xa.h', 'ldap.h')
def strip(src):
    res = []; i = 0; n = len(src); st = None
    while i < n:
        c = src[i]; d = src[i+1] if i + 1 < n else ''
        if st is None:
            if c == '/' and d == '*': st = 'b'; res.append('  '); i += 2; continue
            if c == '/' and d == '/': st = 'l'; res.append('  '); i += 2; continue
            if c in '"\'': st = c
            res.append(c); i += 1; continue
        if st == 'b':
            if c == '*' and d == '/': st = None; res.append('  '); i += 2; continue
            res.append('\n' if c == '\n' else ' '); i += 1; continue
        if st == 'l':
            if c == '\n': st = None; res.append(c)
            else: res.append(' ')
            i += 1; continue
        # inside a string or char literal
        res.append(c)
        if c == '\\' and i + 1 < n: res.append(src[i+1]); i += 2; continue
        if c == st or c == '\n': st = None
        i += 1
    return ''.join(res)
for top in ('src', 'include'):
    for root, dirs, files in os.walk(top):
        for f in files:
            if not f.endswith(('.c', '.h')): continue
            if top == 'include' and f.startswith(VENDOR): continue
            p = os.path.join(root, f)
            try: s = open(p, encoding='utf-8', errors='replace').read()
            except OSError: continue
            q = os.path.join(out, p)
            os.makedirs(os.path.dirname(q), exist_ok=True)
            open(q, 'w', encoding='utf-8').write(strip(s))
PYEOF

{
echo "=== Grep 0 - src/Drivers/Oracle/ holds only harness files and scripts"
ls src/Drivers/Oracle/ \
  | grep -vE '^(Driver_[A-Za-z0-9_]+_Test(\.c|\.log)?|Build\.sh|Run_Manually\.sh|Get_Test_Results\.sh|Compare_[A-Za-z]+\.sh|DoD_Greps\.(sh|log)|dod_allowlist\.txt)$' \
  || echo "none"

cd "$TMP"

echo
echo "=== Grep 1 - OCI calls, OCI types/constants or oci.h outside the driver (comments ignored)"
grep -rnE -e 'OCI[A-Z][A-Za-z]+[[:space:]]*\(|#include[[:space:]]*[<"]oci\.h[">]' \
          -e '\bOCI[A-Z][A-Za-z]+[[:space:]]*\*|\bSQLT_[A-Z]+|\bOCI_[A-Z][A-Z0-9_]*\b' \
          -e 'oci_trans_commit_retry|\bOCI_[A-Z][a-z][A-Za-z_]*[[:space:]]*\(' \
  src/ include/ --include='*.[ch]' \
  | grep -vE 'driver_oracle|/Connection(_Pool)?\.[ch]:|Unit_Test_Module|/Drivers/Oracle/|^src/test/' \
  | grep -vE 'OCI_(SESSION|METRICS|FIELD_TEST|DEPENDENCY_LIST|OBJECT_REF|FIELD_REF)\b|OCI_[A-Z0-9_]+_H\b' \
  | sed -E 's/[[:space:]]+/ /g' \
  | grep -vFf "$OLDPWD/dod_allowlist.txt" \
  || echo "none"

echo
echo "=== Grep 2 - Oracle SQL fragments or dictionary views built in core (comments ignored)"
grep -rnE -e 'TO_DATE|TO_TIMESTAMP|TO_[YD][MS]INTERVAL|EMPTY_[BC]LOB|RETURNING ROWID' \
          -e 'FROM DUAL|BEGIN %s|SYS_REFCURSOR|SYSDATE|SYSTIMESTAMP|NUMTODSINTERVAL|NEXTVAL|CURRVAL' \
          -e 'NVL\(|ROWNUM|DECODE\(|[^\\]":%d|[ (,=]:%d' \
          -e 'ALL_(TAB|TABLES|OBJECTS|COLUMNS|CONS)|USER_(TAB|OBJECTS)|DBA_|V\$' \
  src/ include/ --include='*.[ch]' \
  | grep -vE 'driver_oracle|/Drivers/Oracle/|Unit_Test_Module|^src/test/|DDL_Modules\.[ch]:|DDL_Execute_Module\.[ch]:' \
  | sed -E 's/[[:space:]]+/ /g' \
  | grep -vFf "$OLDPWD/dod_allowlist.txt" \
  || echo "none"

cd "$OLDPWD"
echo
echo "=== Hit counts by file (grep 1 + grep 2)"
} 2>&1 | tee DoD_Greps.log

grep -E '^(src|include)/' DoD_Greps.log | cut -d: -f1 | sort | uniq -c | sort -rn | tee -a DoD_Greps.log
