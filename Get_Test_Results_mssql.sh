#!/bin/bash
# Get_Test_Results_mssql.sh - pass/fail + leak summary for the SQL Server
# driver harnesses (MSSQL_Driver_*_Test.log, written by
# MSSQL_Server/Build_MSSQL_Harnesses.sh). driver_mssql Stage 2 (D9).
LOGS=/home/leyden100/eclipse-workspace/OCI_Wrapper/logs

pass=0; fail=0
for f in "$LOGS"/MSSQL_Driver_*_Test.log; do
  [ -e "$f" ] || { echo "no MSSQL_Driver_*_Test.log in $LOGS"; exit 1; }
  echo "=== $(basename "$f")"
  grep -E '^Test |FAILED|^(PASS|FAIL)$|BUILD FAILED' "$f" | grep -v 'INI File Contents'
  leak=$(grep 'SUMMARY: AddressSanitizer' "$f")
  if [ -z "$leak" ]; then echo "  leak: none"; else echo "  leak: *** CHECK *** $leak"; fi
  if grep -qx 'PASS' "$f"; then pass=$((pass+1)); else fail=$((fail+1)); fi
done

echo
echo "===================================="
echo "TOTAL: $pass PASS, $fail FAIL"
echo "===================================="
