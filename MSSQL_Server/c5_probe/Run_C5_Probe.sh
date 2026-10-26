#!/bin/bash
# Run_C5_Probe.sh - builds and runs MSSQL_C5_Probe (clean-up Part 2,
# point C5-P, 26 Oct 2026).
#
# Step 1 (once, as sa): the trigger test tables. The script asks first;
# answer n if they already exist.
#   MSSQL_Server/SQL/Create_Trigger_Test_mssql.sql
# Step 2 (as DATA_MANAGER): the probe. Every case is rolled back, so it
# leaves no rows behind. The gateway does not need to be running.
#
#   cd ~/eclipse-workspace/OCI_Wrapper/MSSQL_Server/c5_probe
#   ./Run_C5_Probe.sh
#
# Passwords are typed at hidden prompts and passed for one command only
# (SQLCMDPASSWORD for sqlcmd, C5_PWD for the probe); never on a command
# line, never saved.
#
# Output: on screen and in c5_probe_out.txt next to this script.
set -u
cd "$(dirname "$0")" || exit 1

gcc -Wall -Wextra -Wno-format-truncation -O1 -o MSSQL_C5_Probe MSSQL_C5_Probe.c -lodbc \
    || { echo "build failed"; exit 1; }

read -r -p "Create / refresh the trigger test tables now, as sa? [y/N] " A
if [ "${A:-n}" = "y" ] || [ "${A:-n}" = "Y" ]; then
    read -rs -p "sa password: " S; echo
    SQLCMDPASSWORD="$S" sqlcmd -S localhost -U sa -C -b -d DataManager \
        -i ../SQL/Create_Trigger_Test_mssql.sql
    rc=$?
    unset S
    [ $rc -eq 0 ] || { echo "Create_Trigger_Test_mssql.sql failed (exit $rc) - probe not run"; exit 1; }
fi

read -rs -p "DATA_MANAGER password: " P; echo
C5_PWD="$P" ./MSSQL_C5_Probe "$@" 2>&1 | tee c5_probe_out.txt
rc=${PIPESTATUS[0]}
unset P
echo "(exit $rc - output saved in $(pwd)/c5_probe_out.txt)"
