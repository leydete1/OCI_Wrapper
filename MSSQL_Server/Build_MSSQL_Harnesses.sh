#!/bin/bash
# ======================================================================
# Build_MSSQL_Harnesses.sh  (driver_mssql Stage 2, P10; Stage 3, T10)
#
# Builds and runs the SQL Server driver harnesses with AddressSanitizer,
# writing each run to logs/MSSQL_Driver_<name>_Test.log - the file names
# Get_Test_Results_mssql.sh reads. The Oracle harnesses and
# Get_Test_Results.sh are not touched (their Driver_*_Test.log glob does
# not match MSSQL_Driver_*).
#
#   Connect, Pool          Stage 2 - connections, pool, commit/rollback
#   Dialect                Stage 3 - SQL text only, no database
#   Select, Metadata       Stage 3 - scalar SELECT, describe_table
#
# Usage:  MSSQL_Server/Build_MSSQL_Harnesses.sh [config_mssql.ini]
# Needs:  unixodbc-dev, msodbcsql18, the DataManager database
#         (Create_MSSQL_Test_Objects.sql, then
#         Reset_Tester_Objects_mssql.sql for the OCI_FIELD_TEST seed rows),
#         Props/config_mssql.ini with the real DATA_MANAGER password.
# ======================================================================
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INI="${1:-$ROOT/Props/config_mssql.ini}"
BIN="$ROOT/MSSQL_Server/bin"
LOGS="$ROOT/logs"
mkdir -p "$BIN" "$LOGS"

if grep -q '^ *password *= *CHANGE_ME' "$INI"; then
    echo "Set the DATA_MANAGER password in $INI first (password = CHANGE_ME)"
    exit 1
fi

# Core files the Stage 3 code links against, found by name wherever they
# live under src/ (exactly one copy each).
find_src() {
    local hits
    hits=$(find "$ROOT/src" -name "$1" -not -path '*/Drivers/*' | grep -v '\.bak')
    if [ "$(printf '%s\n' "$hits" | grep -c .)" != "1" ]; then
        echo "BUILD FAILED - expected exactly one $1 under src/, found: ${hits:-none}" >&2
        exit 1
    fi
    printf '%s' "$hits"
}
RESULTSET_BUILDER=$(find_src Resultset_Builder.c) || exit 1
DB_TYPE_CLASS=$(find_src db_type_class.c) || exit 1

CFLAGS="-g -O0 -Wall -Wextra -std=gnu11 -fsanitize=address -fno-omit-frame-pointer"
INC="-I$ROOT/include"
# Their headers too, in case either is kept outside include/.
for h in db_type_class.h Resultset_Builder.h; do
    hp=$(find "$ROOT/include" "$ROOT/src" -name "$h" -not -path '*/Drivers/*' | head -1)
    [ -n "$hp" ] && [ "$(dirname "$hp")" != "$ROOT/include" ] && INC="$INC -I$(dirname "$hp")"
done
COMMON="$ROOT/src/execute/driver_mssql.c $ROOT/src/infra/MSSQL_Pool.c \
        $ROOT/src/execute/mssql_dialect.c $ROOT/src/execute/mssql_select.c \
        $ROOT/src/execute/mssql_metadata.c \
        $RESULTSET_BUILDER $DB_TYPE_CLASS \
        $ROOT/src/infra/ini_reader.c $ROOT/src/infra/logger.c $ROOT/src/core/ctx_utils.c"
LIBS="-lodbc -lpthread -lm"

export ASAN_OPTIONS="detect_leaks=1"
export LSAN_OPTIONS="suppressions=$ROOT/MSSQL_Server/lsan_mssql.supp:print_suppressions=1"

rc=0
for name in Connect Pool Dialect Select Metadata; do
    src="$ROOT/src/Drivers/MSSQL/MSSQL_Driver_${name}_Test.c"
    exe="$BIN/MSSQL_Driver_${name}_Test"
    log="$LOGS/MSSQL_Driver_${name}_Test.log"
    echo "=== build MSSQL_Driver_${name}_Test"
    if ! gcc $CFLAGS $INC -o "$exe" "$src" $COMMON $LIBS; then
        echo "BUILD FAILED" | tee "$log"
        rc=1
        continue
    fi
    echo "=== run   MSSQL_Driver_${name}_Test  -> $log"
    "$exe" "$INI" > "$log" 2>&1 || rc=1
    grep -E '^Test |^(PASS|FAIL)$' "$log"
done
exit $rc
