#!/bin/bash
# Run_Z7_Probe.sh - builds and runs MSSQL_Z7_Probe (Z7, 25 Oct 2026).
#
# Read-only apart from a #temp table that belongs to the probe's own
# session. Connects as DATA_MANAGER; the gateway does not need to be
# running (and is better stopped, so the timings are not disturbed).
#
#   cd ~/eclipse-workspace/OCI_Wrapper/MSSQL_Server/z7_probe
#   ./Run_Z7_Probe.sh
#
# Output: on screen and in z7_probe_out.txt next to this script.
set -u
cd "$(dirname "$0")" || exit 1

gcc -Wall -Wextra -O1 -o MSSQL_Z7_Probe MSSQL_Z7_Probe.c -lodbc || { echo "build failed"; exit 1; }

read -rs -p "DATA_MANAGER password: " P; echo
Z7_PWD="$P" ./MSSQL_Z7_Probe "$@" 2>&1 | tee z7_probe_out.txt
rc=${PIPESTATUS[0]}
unset P
echo "(exit $rc - output saved in $(pwd)/z7_probe_out.txt)"
