#!/bin/bash
# Compare_Security.sh - Oracle dialect extraction, Stage 5 proof.
#
# Stage 5 moved the Security module's database calls (Auth_Manager.c,
# Authz_Manager.c) onto the driver without changing what they decide.
# This script pulls every security DECISION out of two log folders - one
# from a run before Stage 5, one after, same fixtures - and diffs them.
#
# Usage:  ./Compare_Security.sh <before_logs_dir> <after_logs_dir>
#
# Decision lines compared (message text only, session ids removed - they
# are random UUIDs and differ on every run):
#   Login denied:      "DENIED username='..' ...: <reason>"
#   Login succeeded:   "SUCCESS username='..' user_id=N"
#   Account locked:    "user_id=N LOCKED after N failed attempts ..."
#   Permissions built: "Permission cache built for user_id=N: K permission(s)"
#   Permission check:  "ALLOWED permission_code='..'" /
#                      "DENIED permission_code='..': <reason>"
#
# Each set is de-duplicated and sorted, so the number of times a fixture
# ran and the thread order do not matter - only which distinct decisions
# were made.
#
# Reported separately (not part of the diff):
#   - "OCI Error" lines in the security log. After Stage 5 the driver
#     logs SELECT errors in the select log, so this count may drop.
#   - Stage 5 failure lines ("user lookup failed", "permission query
#     failed", "commit failed for user_id"). Expected 0 on a clean run.

set -u
A="${1:?usage: $0 <before_logs_dir> <after_logs_dir>}"
B="${2:?usage: $0 <before_logs_dir> <after_logs_dir>}"
NA=$(basename "$A"); NB=$(basename "$B")

PATTERN="DENIED username='[^']*'.*|SUCCESS username='[^']*' user_id=[0-9]+|user_id=[0-9]+ LOCKED after [0-9]+ failed attempts.*|Permission cache built for session_id=[^ ]+ user_id=[0-9]+: [0-9]+ permission\(s\).*|ALLOWED session_id=[^ ]+ permission_code='[^']*'|DENIED session_id=[^ ]+ permission_code='[^']*'.*"
NEWFAIL="user lookup failed|permission query failed|commit failed for user_id|longer than its buffer"

for d in "$A" "$B"; do
    n=$(cat "$d"/*.log 2>/dev/null | grep -cE "$PATTERN")
    if [ "$n" -eq 0 ]; then
        echo "ERROR: no security decision lines found in $d/*.log - is this the"
        echo "       logs folder of a run that included the Authenticate /"
        echo "       CheckPermission fixtures?"
        exit 2
    fi
done

extract() {
    cat "$1"/*.log 2>/dev/null \
      | grep -ohE "$PATTERN" \
      | sed -E 's/session_id=[^ ]+ //; s/( (sid|txid)=[0-9a-fA-F-]+)+[[:space:]]*$//; s/[[:space:]]*$//' \
      | sort -u
}

TMP=$(mktemp -d)
extract "$A" > "$TMP/a.txt"
extract "$B" > "$TMP/b.txt"

count() { grep -cE "$2" "$1"; }

printf '%-22s %14s %14s\n' "Decision kind" "$NA" "$NB"
printf '%-22s %14s %14s\n' "Login denied" \
    "$(count "$TMP/a.txt" '^DENIED username=')" "$(count "$TMP/b.txt" '^DENIED username=')"
printf '%-22s %14s %14s\n' "Login succeeded" \
    "$(count "$TMP/a.txt" '^SUCCESS ')" "$(count "$TMP/b.txt" '^SUCCESS ')"
printf '%-22s %14s %14s\n' "Account locked" \
    "$(count "$TMP/a.txt" ' LOCKED after ')" "$(count "$TMP/b.txt" ' LOCKED after ')"
printf '%-22s %14s %14s\n' "Permissions built" \
    "$(count "$TMP/a.txt" '^Permission cache built')" "$(count "$TMP/b.txt" '^Permission cache built')"
printf '%-22s %14s %14s\n' "Permission allowed" \
    "$(count "$TMP/a.txt" '^ALLOWED ')" "$(count "$TMP/b.txt" '^ALLOWED ')"
printf '%-22s %14s %14s\n' "Permission denied" \
    "$(count "$TMP/a.txt" '^DENIED permission_code=')" "$(count "$TMP/b.txt" '^DENIED permission_code=')"

ea=$(cat "$A"/security*.log 2>/dev/null | grep -c "OCI Error")
eb=$(cat "$B"/security*.log 2>/dev/null | grep -c "OCI Error")
printf '%-22s %14s %14s   (may drop - SELECT errors now in the select log)\n' "OCI Error (security)" "$ea" "$eb"
fa=$(cat "$A"/*.log 2>/dev/null | grep -cE "$NEWFAIL")
fb=$(cat "$B"/*.log 2>/dev/null | grep -cE "$NEWFAIL")
printf '%-22s %14s %14s   (expected: after = 0)\n' "Stage 5 failure lines" "$fa" "$fb"

echo
rc=0
if diff "$TMP/a.txt" "$TMP/b.txt" > "$TMP/diff.txt"; then
    echo "IDENTICAL - $(wc -l < "$TMP/a.txt") distinct security decisions, no differences"
else
    echo "DIFFERENT - lines starting '<' are $NA only, '>' are $NB only:"
    cat "$TMP/diff.txt"
    rc=1
fi

if [ "$fb" -ne 0 ]; then
    echo "CHECK - $fb Stage 5 failure line(s) in $NB:"
    cat "$B"/*.log 2>/dev/null | grep -hE "$NEWFAIL" | head -10
    rc=1
fi

rm -rf "$TMP"
exit $rc
