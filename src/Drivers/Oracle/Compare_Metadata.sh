#!/bin/bash
# Compare_Metadata.sh - Oracle dialect extraction, Stage 4a proof.
#
# Stage 4a moved the Oracle metadata code from Table_Metadata_Module.c
# into driver_oracle.c without changing what it does. This script pulls
# every metadata result line out of two log folders - one from a run
# before 4a, one after, same fixtures - and diffs them.
#
# Usage:  ./Compare_Metadata.sh <before_logs_dir> <after_logs_dir>
#
# Result lines compared (message text only - the function name the
# logger stamps changed in 4a, the messages did not):
#   Describe column:  "Column N: name='..' type='..' len=.. prec=.. scale=..
#                      null='..' default='..'"   (get_request_metadata /
#                      oracle_describe_table, one per column, DEBUG)
#   Describe OK:      "get_request_metadata OK: table='..' owner='..'
#                      columns=N"
#   Describe failed:  "Table '..' not found in ALL_TABLES" and
#                      "Table '..' owner '..' not found or has no
#                      accessible columns"
#   Define column:    "Column N name=.. type=.. size=.. buf_size=.."
#                      (get_multi_metadata / oracle_define_columns, one per
#                      column of every SELECT result)
#
# Each set is de-duplicated and sorted, so thread order and cache hits
# (a hit skips the describe entirely) do not matter - only which
# distinct results were produced.
#
# Expected difference (reported separately, not part of the diff):
#   The debug get_table_metadata() call in dispatch_select() is gone, so
#   its lines ("Calling get_table_metadata", "get_table_metadata OK",
#   "Entering get_table_metadata") must be 0 in the after folder.

set -u
A="${1:?usage: $0 <before_logs_dir> <after_logs_dir>}"
B="${2:?usage: $0 <before_logs_dir> <after_logs_dir>}"
NA=$(basename "$A"); NB=$(basename "$B")

PATTERN="Column [0-9]+: name='.*|get_request_metadata OK: table=.*|Table '[^']*' not found in ALL_TABLES.*|Table '[^']*' owner '[^']*' not found or has no accessible columns.*|Column [0-9]+ name=[^ ]* type=[0-9]+ size=[0-9]+ buf_size=[0-9]+.*"
GONE="Calling get_table_metadata|get_table_metadata OK|Entering get_table_metadata"

for d in "$A" "$B"; do
    n=$(cat "$d"/*.log 2>/dev/null | grep -cE "$PATTERN")
    if [ "$n" -eq 0 ]; then
        echo "ERROR: no metadata result lines found in $d/*.log - is this the"
        echo "       logs folder of a production (HTTP tester) run with"
        echo "       Metadata_log_level = DEBUG?"
        exit 2
    fi
done

# Same trace-context stripping as Compare_SQL.sh.
extract() {
    cat "$1"/*.log 2>/dev/null \
      | grep -ohE "$PATTERN" \
      | sed -E 's/( (sid|txid)=[0-9a-fA-F-]+)+[[:space:]]*$//; s/[[:space:]]*$//' \
      | sort -u
}

TMP=$(mktemp -d)
extract "$A" > "$TMP/a.txt"
extract "$B" > "$TMP/b.txt"

count() { grep -cE "$2" "$1"; }

printf '%-22s %14s %14s\n' "Result kind" "$NA" "$NB"
printf '%-22s %14s %14s\n' "Describe column" \
    "$(count "$TMP/a.txt" '^Column [0-9]+: ')" "$(count "$TMP/b.txt" '^Column [0-9]+: ')"
printf '%-22s %14s %14s\n' "Describe OK" \
    "$(count "$TMP/a.txt" '^get_request_metadata OK')" "$(count "$TMP/b.txt" '^get_request_metadata OK')"
printf '%-22s %14s %14s\n' "Describe failed" \
    "$(count "$TMP/a.txt" '^Table ')" "$(count "$TMP/b.txt" '^Table ')"
printf '%-22s %14s %14s\n' "Define column" \
    "$(count "$TMP/a.txt" '^Column [0-9]+ name=')" "$(count "$TMP/b.txt" '^Column [0-9]+ name=')"

ga=$(cat "$A"/*.log 2>/dev/null | grep -cE "$GONE")
gb=$(cat "$B"/*.log 2>/dev/null | grep -cE "$GONE")
printf '%-22s %14s %14s   (expected: after = 0)\n' "Debug lines (removed)" "$ga" "$gb"

echo
rc=0
if diff "$TMP/a.txt" "$TMP/b.txt" > "$TMP/diff.txt"; then
    echo "IDENTICAL - $(wc -l < "$TMP/a.txt") distinct metadata results, no differences"
else
    echo "DIFFERENT - lines starting '<' are $NA only, '>' are $NB only:"
    cat "$TMP/diff.txt"
    rc=1
fi

if [ "$gb" -ne 0 ]; then
    echo "FAIL - $gb debug get_table_metadata line(s) still present in $NB"
    rc=1
fi

rm -rf "$TMP"
exit $rc
