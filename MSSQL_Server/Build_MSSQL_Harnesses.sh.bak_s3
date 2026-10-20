#!/bin/bash
# ======================================================================
# Build_MSSQL_Harnesses.sh  (driver_mssql Stage 2, P10)
#
# Builds and runs the SQL Server driver harnesses with AddressSanitizer,
# writing each run to logs/MSSQL_Driver_<name>_Test.log - the file names
# Get_Test_Results_mssql.sh reads. The Oracle harnesses and
# Get_Test_Results.sh are not touched (their Driver_*_Test.log glob does
# not match MSSQL_Driver_*).
#
# Usage:  MSSQL_Server/Build_MSSQL_Harnesses.sh [config_mssql.ini]
# Needs:  unixodbc-dev, msodbcsql18, the DataManager database
#         (Create_MSSQL_Test_Objects.sql), Props/config_mssql.ini with
#         the real DATA_MANAGER password.
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

CFLAGS="-g -O0 -Wall -Wextra -std=gnu11 -fsanitize=address -fno-omit-frame-pointer"
INC="-I$ROOT/include"
COMMON="$ROOT/src/execute/driver_mssql.c $ROOT/src/infra/MSSQL_Pool.c \
        $ROOT/src/infra/ini_reader.c $ROOT/src/infra/logger.c $ROOT/src/core/ctx_utils.c"
LIBS="-lodbc -lpthread -lm"

export ASAN_OPTIONS="detect_leaks=1"
export LSAN_OPTIONS="suppressions=$ROOT/MSSQL_Server/lsan_mssql.supp:print_suppressions=1"

rc=0
for name in Connect Pool; do
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
