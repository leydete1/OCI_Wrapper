#!/bin/bash
# Compare_Metrics.sh - Oracle dialect extraction, item 3b proof.
#
# The metrics DB writer logs one line per batch (normally in
# metrics_writer_app.log; every *.log in the folder is searched):
#   "Inserted <n> of <m> metrics record(s) this batch"
# and, from 3b on, "Commit failed ..." if a batch's commit fails.
# For each log folder this prints:
#   batches    - number of batches flushed
#   inserted   - sum of <n>   (rows written to OCI_METRICS)
#   offered    - sum of <m>   (records handed to the DB writer)
#   failed     - "Commit failed" lines (must be 0)
#   csv_rows   - data rows in metrics_Data_Manager.csv (header excluded)
# 3b must keep inserted == offered (every record persisted) and no
# commit failures. offered should track csv_rows the same way in both
# runs - every record goes to both destinations - allowing for the
# usual run-to-run drift in how many requests the tester produced.
#
# Usage:  ./Compare_Metrics.sh <stage3_logs_dir> <stage3b_logs_dir>

set -u
A="${1:?usage: $0 <stage3_logs_dir> <stage3b_logs_dir>}"
B="${2:?usage: $0 <stage3_logs_dir> <stage3b_logs_dir>}"

stats() {
    # Searches every *.log in the folder: the per-batch "Inserted" line is
    # written on the DB writer session's metrics_writer_logger, which is
    # normally metrics_writer_app.log - grepping all logs avoids depending
    # on that wiring.
    local d="$1" c="$1/metrics_Data_Manager.csv"
    local line
    line=$(cat "$d"/*.log 2>/dev/null \
           | grep -ohE 'Inserted [0-9]+ of [0-9]+ metrics record' \
           | awk '{ b++; i += $2; o += $4 } END { printf "%d %d %d", b, i, o }')
    local failed csv
    failed=$(cat "$d"/*.log 2>/dev/null | grep -c 'Commit failed - rolling back; 0 of')
    csv=0
    [ -f "$c" ] && csv=$(grep -cv '^session_id,' "$c")
    echo "$line $failed $csv"
}

read ba ia oa fa ca <<< "$(stats "$A")"
read bb ib ob fb cb <<< "$(stats "$B")"

printf '%-12s %10s %10s\n' "" "Stage 3" "Stage 3b"
printf '%-12s %10s %10s\n' "batches"  "$ba" "$bb"
printf '%-12s %10s %10s\n' "inserted" "$ia" "$ib"
printf '%-12s %10s %10s\n' "offered"  "$oa" "$ob"
printf '%-12s %10s %10s\n' "failed"   "$fa" "$fb"
printf '%-12s %10s %10s\n' "csv_rows" "$ca" "$cb"
echo

bad=0
if [ "$bb" -eq 0 ]; then
    echo "FAIL - no 'Inserted N of M' lines in the 3b logs. Check that"
    echo "       metrics_db_enabled = 1, and send me metrics_writer_app.log"
    bad=1
fi
if [ "$ib" -ne "$ob" ]; then echo "FAIL - 3b inserted $ib of $ob offered records"; bad=1; fi
if [ "$fb" -ne 0 ]; then echo "FAIL - $fb commit failure(s) in the 3b run"; bad=1; fi
if [ "$ia" -ne "$oa" ]; then echo "NOTE - the Stage 3 baseline itself inserted $ia of $oa"; fi
[ "$bad" -eq 0 ] && echo "OK - every offered metrics record inserted and committed"
exit $bad
