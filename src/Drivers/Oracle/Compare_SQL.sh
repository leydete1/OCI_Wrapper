#!/bin/bash
# Compare_SQL.sh - Oracle dialect extraction, Stage 2 proof.
#
# Pulls every SQL statement core logs out of two log folders - one from a
# Stage 1 production run, one from a Stage 2 run of the same fixtures -
# and diffs them. Stage 2 must not change a single statement.
#
# Usage:  ./Compare_SQL.sh <stage1_logs_dir> <stage2_logs_dir>
#
# Statements compared (the log line each module writes):
#   INSERT SQL:            insert_Data_Manager.log      build_insert_sql()
#   UPDATE SQL:            update_Data_Manager.log      build_update_sql()
#   DELETE SQL:            delete_Data_Manager.log      build_delete_sql()
#   PL/SQL block:          procedure_Data_Manager.log   build_plsql_block()
#   Before-image SELECT:   audit_Data_Manager.log       audit_trail_fetch_before_image()
#   Count query:           select_Data_Manager.log      execute_query_batch() row-count guard
#   Truncated fetch SQL:   select_Data_Manager.log      execute_query_batch() row limit
#   Cleaned SQL:           select_Data_Manager.log      every SELECT, incl. the session sweep
#
# Each set is de-duplicated and sorted, so thread order and how many times
# a fixture ran do not matter - only which distinct statements were built.

set -u
PATTERN='INSERT SQL: .*|UPDATE SQL: .*|DELETE SQL: .*|PL/SQL block: .*|Before-image SELECT: .*|Count query: .*|Truncated fetch SQL: .*|Cleaned SQL: .*'
A="${1:?usage: $0 <stage1_logs_dir> <stage2_logs_dir>}"
B="${2:?usage: $0 <stage1_logs_dir> <stage2_logs_dir>}"

for d in "$A" "$B"; do
    n=$(cat "$d"/*.log 2>/dev/null | grep -cE "$PATTERN")
    if [ "$n" -eq 0 ]; then
        echo "ERROR: no logged SQL statements found in $d/*.log - is this the"
        echo "       logs folder of a production (HTTP tester) run?"
        exit 2
    fi
done

# log_include_trace_context=1 appends " sid=<uuid>" / " txid=<uuid>" to
# every line - different on every run, so they are stripped before
# comparing, and so are UUIDs inside statements (session IDs in the
# OCI_SESSION before-images). "Before-image SELECT: fetched N row(s)..." is
# a result line, not a statement, and is dropped.
extract() {
    cat "$1"/*.log 2>/dev/null \
      | grep -ohE "$PATTERN" \
      | grep -v '^Before-image SELECT: fetched ' \
      | sed -E 's/( (sid|txid)=[0-9a-fA-F-]+)+[[:space:]]*$//; s/[[:space:]]*$//' \
      | sed -E 's/[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/<uuid>/g' \
      | sort -u
}

extract "$A" > /tmp/sql_stage1.txt
extract "$B" > /tmp/sql_stage2.txt

printf '%-22s %8s %8s\n' "Statement kind" "Stage 1" "Stage 2"
for kind in "INSERT SQL" "UPDATE SQL" "DELETE SQL" "PL/SQL block" \
            "Before-image SELECT" "Count query" "Truncated fetch SQL" "Cleaned SQL"; do
    a=$(grep -c "^$kind: " /tmp/sql_stage1.txt)
    b=$(grep -c "^$kind: " /tmp/sql_stage2.txt)
    printf '%-22s %8s %8s\n' "$kind" "$a" "$b"
done

echo
if diff /tmp/sql_stage1.txt /tmp/sql_stage2.txt > /tmp/sql_diff.txt; then
    echo "IDENTICAL - $(wc -l < /tmp/sql_stage1.txt) distinct statements, no differences"
    exit 0
else
    echo "DIFFERENT - lines starting '<' are Stage 1 only, '>' are Stage 2 only:"
    cat /tmp/sql_diff.txt
    exit 1
fi
