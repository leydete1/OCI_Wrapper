#!/bin/bash
# Check_Results.sh - pass/fail + leak summary for every Driver_*_Test harness
LOGS=/home/leyden100/eclipse-workspace/OCI_Wrapper/logs
BASELINE='37017 byte\(s\) leaked in 83|37129 byte\(s\) leaked in 85'

pass=0; fail=0
for f in "$LOGS"/Driver_*_Test.log; do
  echo "=== $(basename "$f")"
  grep -E '^Test |^Round |^[[:space:]]*\[row .*FAILED|FAILED|^(PASS|FAIL)$' "$f" \
    | grep -v 'INI File Contents'
  leak=$(grep 'SUMMARY: AddressSanitizer' "$f")
  if echo "$leak" | grep -qE "$BASELINE"; then
    echo "  leak: baseline"
  else
    echo "  leak: *** CHECK *** $leak"
  fi
  if grep -qx 'PASS' "$f"; then pass=$((pass+1)); else fail=$((fail+1)); fi
done

echo
echo "===================================="
echo "TOTAL: $pass PASS, $fail FAIL"
echo "===================================="
