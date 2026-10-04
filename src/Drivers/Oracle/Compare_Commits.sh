#!/bin/bash
# Compare_Commits.sh - Oracle dialect extraction, Stage 3 proof.
#
# Counts commit attempts in two log folders - one from a Stage 2b run,
# one from a Stage 3 run of the same fixtures - per log file:
#   attempts  - every "Commit attempt n/m" line (before Stage 3 the line
#               read "OCITransCommit attempt n/m"; the pattern matches both)
#   retries   - attempts with n > 1
#   max       - highest n seen (must be <= tx_max_retries + 1 = 4)
# Stage 3 must not change how many commits happen or how often they
# retry. The attempts column can drift slightly between runs for the
# same data-driven reasons the tester's Expected-diff count does; the
# retries and max columns must not.
#
# Usage:  ./Compare_Commits.sh <stage2b_logs_dir> <stage3_logs_dir>

set -u
A="${1:?usage: $0 <stage2b_logs_dir> <stage3_logs_dir>}"
B="${2:?usage: $0 <stage2b_logs_dir> <stage3_logs_dir>}"

stats() {   # $1 = folder; prints "file attempts retries max" per file
    for f in "$1"/*.log; do
        [ -f "$f" ] || continue
        grep -ohE 'Commit attempt [0-9]+/[0-9]+' "$f" \
          | awk -v name="$(basename "$f")" '
              { split($3, p, "/"); n++; if (p[1] > 1) r++; if (p[1] > m) m = p[1] }
              END { if (n) printf "%s %d %d %d\n", name, n, r, m }'
    done
}

stats "$A" | sort > /tmp/commits_a.txt
stats "$B" | sort > /tmp/commits_b.txt

if [ ! -s /tmp/commits_a.txt ] || [ ! -s /tmp/commits_b.txt ]; then
    echo "ERROR: no commit attempt lines found in one of the folders - is"
    echo "       each the logs folder of a production (HTTP tester) run?"
    exit 2
fi

printf '%-34s %18s %18s\n' "" "--- Stage 2b ---" "--- Stage 3 ---"
printf '%-34s %6s %5s %5s  %6s %5s %5s\n' "Log file" "att" "retry" "max" "att" "retry" "max"
join -a1 -a2 -e 0 -o 0,1.2,1.3,1.4,2.2,2.3,2.4 /tmp/commits_a.txt /tmp/commits_b.txt \
  | awk '{ printf "%-34s %6s %5s %5s  %6s %5s %5s\n", $1,$2,$3,$4,$5,$6,$7
           ta+=$2; tb+=$5; ra+=$3; rb+=$6; if ($4>ma) ma=$4; if ($7>mb) mb=$7 }
         END { printf "%-34s %6d %5d %5d  %6d %5d %5d\n", "TOTAL", ta,ra,ma, tb,rb,mb
               bad = (mb > 4) || (ra != rb)
               print ""
               if (mb > 4)   print "FAIL - an attempt number above 4 (tx_max_retries + 1)"
               if (ra != rb) print "CHECK - retry counts differ between the runs"
               if (!bad)     print "OK - max attempt <= 4 and retry counts identical" }'
