#!/usr/bin/env python3
"""
copy_baselines_mssql.py  (driver_mssql Stage 7, point Z2 - 23 Oct 2026)

Fills Testers/baseline_mssql for Run_mssql.sh:

  - every fixture that already passes through the SQL Server gateway
    against its Oracle baseline gets that Oracle baseline, copied
    unchanged (same content, same proof);
  - the fixtures in NEEDS_MSSQL_BASELINE get NO copy: the first
    Run_mssql.sh run writes their SQL Server baseline ("Baseline created"),
    and those files are reviewed against the Oracle ones before they are
    kept (README.txt says what each may differ in);
  - fixtures listed in mssql_excluded.txt are skipped.

Never overwrites a file in baseline_mssql. Dry run unless --apply.

Usage:  cd ~/eclipse-workspace/OCI_Wrapper/Testers
        python3 copy_baselines_mssql.py            # dry run
        python3 copy_baselines_mssql.py --apply
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "baseline")
DST = os.path.join(HERE, "baseline_mssql")
EXCL = os.path.join(HERE, "mssql_excluded.txt")
ROOT = os.path.dirname(HERE)
FIXTURE_DIRS = [os.path.join(ROOT, "test_data/consumers/file/Input_XML"),
                os.path.join(ROOT, "test_data/consumers/file/Input_JSON")]

# fixture -> the documented way its SQL Server response may differ
NEEDS_MSSQL_BASELINE = {
    "select_integration_tests.xml":    "alias case (Second_Blob vs SECOND_BLOB); seed DESCRIPTION text of row 1",
    "select_integration_tests.json":   "alias case (Second_Blob vs SECOND_BLOB); seed DESCRIPTION text of row 1",
    "Verify_Round_5_Clob.xml":         "fetch_batch_size 5 (Oracle forces 1 for a CLOB column)",
    "Unit_Test_Procedure_Round_2.xml": "DATE_COL: field_type TIMESTAMP, value 2030-01-01 00:00:00 (Oracle DATE, 01-JAN-30)",
    "Unit_Test_Procedure_Round_2.json": "DATE_COL: field_type TIMESTAMP, value 2030-01-01 00:00:00 (Oracle DATE, 01-JAN-30)",
    "Request_Test_Select_2.json":      "TO BE DETERMINED from its first SQL Server response (point Z5)",
}

README = """baseline_mssql - SQL Server baselines for Run_mssql.sh (driver_mssql Stage 7)

Most files here are the Oracle baselines from ../baseline, copied unchanged
by copy_baselines_mssql.py: those fixtures give the same response on both
databases.

The fixtures below have a SQL Server baseline of their own, written by the
first Run_mssql.sh run and reviewed against the Oracle baseline. Each may
differ from Oracle ONLY as stated; any other difference is a defect.

%s
Fixtures not run on SQL Server at all: see ../mssql_excluded.txt.
"""


def excluded():
    out = set()
    with open(EXCL) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                out.add(line.split()[0])
    return out


def main():
    do_apply = "--apply" in sys.argv[1:]
    print("=== %s -> %s  (%s)" % (SRC, DST, "APPLY" if do_apply else "DRY RUN"))
    for p in (SRC, EXCL):
        if not os.path.exists(p):
            sys.exit("not found: %s" % p)
    excl = excluded()

    fixtures = []
    for d in FIXTURE_DIRS:
        if not os.path.isdir(d):
            sys.exit("not found: %s" % d)
        fixtures += [n for n in sorted(os.listdir(d)) if os.path.isfile(os.path.join(d, n))]

    copy, skip_excl, new, no_oracle, exists = [], [], [], [], []
    for fx in fixtures:
        if fx in excl:
            skip_excl.append(fx)
            continue
        if fx in NEEDS_MSSQL_BASELINE:
            new.append(fx)
            continue
        src = os.path.join(SRC, fx + ".baseline")
        dst = os.path.join(DST, fx + ".baseline")
        if os.path.exists(dst):
            exists.append(fx)
        elif not os.path.isfile(src):
            no_oracle.append(fx)
        else:
            copy.append(fx)

    print("\nfixtures found:                 %d" % len(fixtures))
    print("excluded (mssql_excluded.txt):  %d" % len(skip_excl))
    print("Oracle baseline to copy:        %d" % len(copy))
    print("already in baseline_mssql:      %d (left as they are)" % len(exists))
    print("SQL Server baseline to review:  %d" % len(new))
    for fx in new:
        print("    %-36s %s" % (fx, NEEDS_MSSQL_BASELINE[fx]))
    missing = [k for k in NEEDS_MSSQL_BASELINE if k not in fixtures]
    if missing:
        print("\nWARNING - listed for review but not found as fixtures: %s" % ", ".join(missing))
    if no_oracle:
        print("\nWARNING - no Oracle baseline (the first SQL Server run would create one "
              "unreviewed): %s" % ", ".join(no_oracle))

    if not do_apply:
        print("\nDry run only - nothing written. Re-run with --apply.")
        return

    os.makedirs(DST, exist_ok=True)
    for fx in copy:
        shutil.copy2(os.path.join(SRC, fx + ".baseline"), os.path.join(DST, fx + ".baseline"))
    readme = os.path.join(DST, "README.txt")
    if not os.path.exists(readme):
        lines = "".join("  %-36s %s\n" % (k, v) for k, v in NEEDS_MSSQL_BASELINE.items())
        with open(readme, "w") as f:
            f.write(README % lines)
    print("\nAPPLIED - %d baselines copied into baseline_mssql." % len(copy))
    print("Next: start the gateway on config_mssql.ini, then ./Run_mssql.sh")
    print("      expect 'Baseline created: %d' - send those %d files for review." % (len(new), len(new)))


if __name__ == "__main__":
    main()
