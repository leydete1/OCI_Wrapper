#!/bin/bash
# ======================================================================
# Run_mssql.sh  (driver_mssql Stage 7: points Z1, Z3, Z6 - 23 Oct 2026;
#                part 2a, 23 Oct 2026: reset checked, reset after the run,
#                fixtures only)
#
# The HTTP tester through the SQL Server gateway. Run.sh (Oracle) and
# ./baseline are not touched.
#
#   1. asks once for the DATA_MANAGER password (not echoed) and resets the
#      SQL Server tester objects (Reset_Tester_Objects_mssql.sql). The
#      reset counts only if it printed all four of its "ok" lines - a
#      silent sqlcmd that still reports success no longer lets the run go
#      on (23 Oct, 17:16). Stops if the reset is not confirmed.
#   2. builds fixtures_mssql/Input_XML (*.xml) and Input_JSON (*.json)
#      from the real fixture folders, leaving out every file listed in
#      mssql_excluded.txt - rebuilt on every run
#   3. builds run_plan_mssql.txt from run_plan.txt without those fixtures
#   4. runs the tester against ./baseline_mssql
#   5. resets again, so the database is left in its seed state for the
#      SQL Server harnesses (Select Tests 4, 8 and 14 count the seed rows).
#      Same password, no second prompt. A failed reset here is reported,
#      and the script exits non-zero.
#
# --no-reset skips both resets (and the password prompt).
#
# The password goes to sqlcmd through SQLCMDPASSWORD, set for the sqlcmd
# command only - never on a command line (no -P), never written to a file.
# Each reset's full output is kept in reset_mssql_before.log /
# reset_mssql_after.log beside this script.
#
# The gateway must already be running on Props/config_mssql.ini.
#
# Usage:  cd ~/eclipse-workspace/OCI_Wrapper/Testers; ./Run_mssql.sh [--no-reset]
# ======================================================================
set -u
cd "$(dirname "$0")" || exit 1
ROOT="$(cd .. && pwd)"
SRC_XML="$ROOT/test_data/consumers/file/Input_XML"
SRC_JSON="$ROOT/test_data/consumers/file/Input_JSON"
RESET="$ROOT/MSSQL_Server/SQL/Reset_Tester_Objects_mssql.sql"
EXCL=./mssql_excluded.txt
OUT=./fixtures_mssql
BASE=./baseline_mssql

# the four lines Reset_Tester_Objects_mssql.sql prints when it succeeds
RESET_OK_LINES=(
    "auth.test.nearlock"
    "auth.test.user"
    "auth.test.locked"
    "OCI_FIELD_TEST"
)

DO_RESET=1
[ "${1:-}" = "--no-reset" ] && DO_RESET=0

for f in "$EXCL" ./run_plan.txt ./http_consumer_tester "$SRC_XML" "$SRC_JSON"; do
    [ -e "$f" ] || { echo "Run_mssql.sh: $f not found"; exit 1; }
done
[ -d "$BASE" ] || { echo "Run_mssql.sh: $BASE not found - run copy_baselines_mssql.py first"; exit 1; }

echo "=== SQL Server run - the gateway must be running on config_mssql.ini"

DMPW=""
cleanup() { DMPW=""; unset DMPW; }
trap cleanup EXIT

# run_reset <log file> - 0 only when sqlcmd succeeded AND all four ok lines
# were printed
run_reset() {
    local log="$1" rc missing=""
    SQLCMDPASSWORD="$DMPW" sqlcmd -S localhost -U DATA_MANAGER -d DataManager -C -b \
        -i "$RESET" > "$log" 2>&1 < /dev/null
    rc=$?
    for name in "${RESET_OK_LINES[@]}"; do
        grep -Eq "^ok[[:space:]]+$name[[:space:]]" "$log" || missing="$missing $name"
    done
    if [ $rc -ne 0 ] || [ -n "$missing" ]; then
        echo "    sqlcmd exit code $rc; ok lines missing:${missing:- none}"
        echo "    ---- sqlcmd output ($log) ----"
        sed 's/^/    /' "$log"
        return 1
    fi
    grep -E "^ok[[:space:]]" "$log" | sed 's/^/    /'
    return 0
}

# 1. reset (before)
if [ $DO_RESET -eq 1 ]; then
    [ -f "$RESET" ] || { echo "Run_mssql.sh: $RESET not found"; exit 1; }
    command -v sqlcmd > /dev/null || { echo "Run_mssql.sh: sqlcmd not on PATH"; exit 1; }
    read -rs -p "DATA_MANAGER password: " DMPW
    echo
    [ -n "$DMPW" ] || { echo "Run_mssql.sh: no password given - tester not run"; exit 1; }
    echo "=== reset (before): $RESET"
    if ! run_reset ./reset_mssql_before.log; then
        echo "Run_mssql.sh: reset NOT confirmed - tester not run"
        exit 1
    fi
else
    echo "=== --no-reset: database not reset before or after the run"
fi

# 2. fixture folders without the excluded fixtures (fixture files only)
excluded=$(grep -v '^[[:space:]]*#' "$EXCL" | awk 'NF {print $1}')
rm -rf "$OUT"
mkdir -p "$OUT/Input_XML" "$OUT/Input_JSON"
n_skip=0
n_other=0
for spec in "$SRC_XML:$OUT/Input_XML:xml" "$SRC_JSON:$OUT/Input_JSON:json"; do
    src=${spec%%:*}; rest=${spec#*:}; dst=${rest%%:*}; ext=${rest#*:}
    for f in "$src"/*; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        if [ "${b##*.}" != "$ext" ]; then
            n_other=$((n_other + 1))
            continue
        fi
        if printf '%s\n' "$excluded" | grep -qxF "$b"; then
            n_skip=$((n_skip + 1))
            continue
        fi
        cp -p "$f" "$dst/"
    done
done
echo "=== fixtures: $(ls "$OUT/Input_XML" | wc -l) xml + $(ls "$OUT/Input_JSON" | wc -l) json; $n_skip excluded (mssql_excluded.txt); $n_other other file(s) ignored"

# 3. run plan without the excluded fixtures
awk -v list="$excluded" '
    BEGIN { n = split(list, a, "\n"); for (i = 1; i <= n; i++) skip[a[i]] = 1 }
    { t = $1; if (t != "" && substr(t, 1, 1) != "#" && substr(t, 1, 1) != "[" && (t in skip)) next; print }
' ./run_plan.txt > ./run_plan_mssql.txt

# 4. the tester
rm -f "$BASE"/*.current
./http_consumer_tester https://localhost:8443 \
    "$PWD/${OUT#./}/Input_XML" "$PWD/${OUT#./}/Input_JSON" 8 "$BASE" ./run_plan_mssql.txt
tester_rc=$?

# 5. reset (after) - always, whatever the tester's result
if [ $DO_RESET -eq 1 ]; then
    echo "=== reset (after): leaving the database in its seed state"
    if ! run_reset ./reset_mssql_after.log; then
        echo "Run_mssql.sh: WARNING - reset after the run NOT confirmed;"
        echo "             reset by hand before the SQL Server harnesses"
        [ $tester_rc -ne 0 ] && exit $tester_rc
        exit 1
    fi
fi
exit $tester_rc
