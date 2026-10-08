#!/bin/bash
# DoD_Greps.sh - Oracle dialect extraction, Definition-of-done checks.
#
# Run from the project root:  cd ~/eclipse-workspace/OCI_Wrapper && ./DoD_Greps.sh
# Output goes to the screen and to DoD_Greps.log.
#
# Each check prints its hits; "none" means it passed. Every hit is
# either a real leftover or an entry for dod_allowlist.txt (one exact
# output line per entry). The allow-list is created empty if missing.

cd "$(dirname "$0")" || exit 1
[ -f dod_allowlist.txt ] || : > dod_allowlist.txt

{
echo "=== Grep 0 - src/Drivers/Oracle/ holds only harness files and scripts"
ls src/Drivers/Oracle/ \
  | grep -vE '^(Driver_[A-Za-z0-9]+_Test(\.c|\.log)?|Build\.sh|Run_Manually\.sh|Get_Test_Results\.sh|Compare_[A-Za-z]+\.sh)$' \
  || echo "none"

echo
echo "=== Grep 1 - OCI calls, OCI types/constants or oci.h outside the driver"
grep -rnE -e 'OCI[A-Z][A-Za-z]+[[:space:]]*\(|#include[[:space:]]*[<"]oci\.h[">]' \
          -e '\bOCI[A-Z][A-Za-z]+[[:space:]]*\*|\bSQLT_[A-Z]+|\bOCI_[A-Z][A-Z0-9_]*\b' \
          -e 'oci_trans_commit_retry|\bOCI_[A-Z][a-z][A-Za-z_]*[[:space:]]*\(' \
  src/ include/ --include='*.[ch]' \
  | grep -vE 'driver_oracle|/Connection(_Pool)?\.[ch]:|Unit_Test_Module|/Drivers/Oracle/' \
  | grep -vE 'OCI_(SESSION|METRICS|FIELD_TEST)\b|OCI_[A-Z0-9_]+_H\b' \
  | grep -vFf dod_allowlist.txt \
  || echo "none"

echo
echo "=== Grep 2 - Oracle SQL fragments or dictionary views built in core"
grep -rnE -e 'TO_DATE|TO_TIMESTAMP|TO_[YD][MS]INTERVAL|EMPTY_[BC]LOB|RETURNING ROWID' \
          -e 'FROM DUAL|BEGIN %s|SYS_REFCURSOR|SYSDATE|SYSTIMESTAMP|NUMTODSINTERVAL|NEXTVAL|CURRVAL' \
          -e 'NVL\(|ROWNUM|DECODE\(|":%d|[ (,=]:%d' \
          -e 'ALL_(TAB|TABLES|OBJECTS|COLUMNS|CONS)|USER_(TAB|OBJECTS)|DBA_|V\$' \
  src/ include/ --include='*.[ch]' \
  | grep -vE 'driver_oracle|/Drivers/Oracle/|Unit_Test_Module|DDL_Modules\.[ch]:|DDL_Execute_Module\.[ch]:' \
  | grep -vFf dod_allowlist.txt \
  || echo "none"

echo
echo "=== Hit counts by file (grep 1 + grep 2)"
} 2>&1 | tee DoD_Greps.log

grep -E '^(src|include)/' DoD_Greps.log | cut -d: -f1 | sort | uniq -c | sort -rn | tee -a DoD_Greps.log
