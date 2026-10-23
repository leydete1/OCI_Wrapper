#!/bin/bash
# ======================================================================
# Run_mssql.sh  (driver_mssql Stage 7, points Z1, Z3, Z6 - 23 Oct 2026)
#
# The HTTP tester through the SQL Server gateway. Run.sh (Oracle) and
# ./baseline are not touched.
#
#   1. resets the SQL Server tester objects (Reset_Tester_Objects_mssql.sql,
#      sqlcmd prompts for the DATA_MANAGER password) - stops if it fails;
#      --no-reset skips this
#   2. builds fixtures_mssql/Input_XML and Input_JSON from the real fixture
#      folders, leaving out every file listed in mssql_excluded.txt -
#      rebuilt on every run, so fixture changes are always picked up
#   3. builds run_plan_mssql.txt from run_plan.txt without those fixtures
#   4. runs the tester against ./baseline_mssql
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

for f in "$EXCL" ./run_plan.txt ./http_consumer_tester "$SRC_XML" "$SRC_JSON"; do
    [ -e "$f" ] || { echo "Run_mssql.sh: $f not found"; exit 1; }
done
[ -d "$BASE" ] || { echo "Run_mssql.sh: $BASE not found - run copy_baselines_mssql.py first"; exit 1; }

echo "=== SQL Server run - the gateway must be running on config_mssql.ini"

# 1. reset
if [ "${1:-}" != "--no-reset" ]; then
    [ -f "$RESET" ] || { echo "Run_mssql.sh: $RESET not found"; exit 1; }
    echo "=== reset: $RESET (DATA_MANAGER password)"
    if ! sqlcmd -S localhost -U DATA_MANAGER -d DataManager -C -b -i "$RESET"; then
        echo "Run_mssql.sh: reset FAILED - tester not run"
        exit 1
    fi
fi

# 2. fixture folders without the excluded fixtures
excluded=$(grep -v '^[[:space:]]*#' "$EXCL" | awk 'NF {print $1}')
rm -rf "$OUT"
mkdir -p "$OUT/Input_XML" "$OUT/Input_JSON"
n_skip=0
for pair in "$SRC_XML:$OUT/Input_XML" "$SRC_JSON:$OUT/Input_JSON"; do
    src=${pair%%:*}; dst=${pair#*:}
    for f in "$src"/*; do
        [ -f "$f" ] || continue
        b=$(basename "$f")
        if printf '%s\n' "$excluded" | grep -qxF "$b"; then
            n_skip=$((n_skip + 1))
            continue
        fi
        cp -p "$f" "$dst/"
    done
done
echo "=== fixtures: $(ls "$OUT/Input_XML" | wc -l) xml + $(ls "$OUT/Input_JSON" | wc -l) json; $n_skip excluded (mssql_excluded.txt)"

# 3. run plan without the excluded fixtures
awk -v list="$excluded" '
    BEGIN { n = split(list, a, "\n"); for (i = 1; i <= n; i++) skip[a[i]] = 1 }
    { t = $1; if (t != "" && substr(t, 1, 1) != "#" && substr(t, 1, 1) != "[" && (t in skip)) next; print }
' ./run_plan.txt > ./run_plan_mssql.txt

# 4. the tester
rm -f "$BASE"/*.current
./http_consumer_tester https://localhost:8443 \
    "$PWD/${OUT#./}/Input_XML" "$PWD/${OUT#./}/Input_JSON" 8 "$BASE" ./run_plan_mssql.txt
