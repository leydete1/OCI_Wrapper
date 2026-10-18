#!/usr/bin/env python3
"""
m2_compile_proof.py - driver_mssql point M2, item N4 (2026-10-17)

Proves the shared context is vendor-neutral: every core .c file must
compile with Oracle's headers made unusable.

Usage (after apply_neutral_context.py --apply):
    python3 m2_compile_proof.py <OCI_Wrapper> [--cflags "-DFOO -I/x/y"] [-v]

Part 1 - core, Oracle headers blocked
    A temporary stub folder gets one file per Oracle client header found
    in include/ (oci.h, oratypes.h, ociap.h, ori.h, xmlproc.h ...), plus
    oci.h and oratypes.h always. Each stub is only
        #error "M2 proof: <oci.h> is an Oracle header - core must not include it"
    The stub folder goes first on the -I list, so any core file that
    reaches an Oracle header, directly or through a project header,
    fails with that message naming the header.
    Core = every .c under src/ that is not Oracle side (same list as
    apply_neutral_context.py: driver_oracle.c, Connection.c,
    Connection_Pool.c, src/Drivers/Oracle/, src/test/; the Bootstrap counts
    as Oracle side only if it includes <oci.h> itself).
Part 2 - Oracle side, real headers
    The Oracle-side .c files compile against the real include/ with
    -Werror=incompatible-pointer-types, so a missing cast on a void*
    handle would show here rather than in the Eclipse build.

Both parts use gcc -fsyntax-only (no objects written, nothing linked).
Flags: -std=gnu11 -D_GNU_SOURCE -Werror=implicit-function-declaration
-Werror=incompatible-pointer-types, -I for include/ and every src/
folder holding headers, `pkg-config --cflags libxml-2.0` if present,
plus whatever --cflags adds. If a file fails for a reason unrelated to
M2 (a missing third-party -I or -D), add it with --cflags and re-run.

Exit code 0 = both parts pass. Writes nothing in the tree.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apply_neutral_context import (  # noqa: E402  (same folder)
    ORA_HDR_RE, INC_OCI_RE, WIRING_FILES, is_oracle_side, project_files, read)

BASE_FLAGS = ["-std=gnu11", "-fsyntax-only", "-D_GNU_SOURCE",
              "-Werror=implicit-function-declaration",
              "-Werror=incompatible-pointer-types"]


def pkg_cflags(name):
    try:
        out = subprocess.run(["pkg-config", "--cflags", name], capture_output=True,
                             text=True, timeout=10)
        return out.stdout.split() if out.returncode == 0 else []
    except (OSError, subprocess.SubprocessError):
        return []


def compile_one(root, rel, incs, extra):
    cmd = ["gcc"] + BASE_FLAGS + incs + extra + [os.path.join(root, rel)]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=root)
    errs = [l for l in r.stderr.splitlines()
            if "error:" in l or l.startswith("In file included") or l.lstrip().startswith("from ")]
    return r.returncode == 0, errs or r.stderr.splitlines()


def main():
    argv = sys.argv[1:]
    verbose = "-v" in argv
    extra = []
    if "--cflags" in argv:
        i = argv.index("--cflags")
        extra = argv[i + 1].split()
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith("-")]
    if len(args) != 1 or not os.path.isdir(args[0]):
        sys.exit(__doc__)
    root = os.path.abspath(args[0])
    if shutil.which("gcc") is None:
        sys.exit("gcc not found")

    files = project_files(root)
    proj = set(files)
    inc_dir = os.path.join(root, "include")

    stub = tempfile.mkdtemp(prefix="m2_stub_")
    stubbed = set()
    try:
        names = {"oci.h", "oratypes.h"}
        for d, _, fs in os.walk(inc_dir):
            for f in fs:
                rel = os.path.relpath(os.path.join(d, f), root)
                if f.endswith(".h") and (ORA_HDR_RE.match(f) or rel not in proj):
                    names.add(os.path.relpath(os.path.join(d, f), inc_dir))
        for n in names:
            p = os.path.join(stub, n)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w") as f:
                f.write('#error "M2 proof: <%s> is an Oracle header - core must not include it"\n' % n)
            stubbed.add(n)

        hdr_dirs = sorted({os.path.dirname(r) for r in files if r.endswith(".h")} - {"include"})
        real_incs = ["-I" + inc_dir] + ["-I" + os.path.join(root, d) for d in hdr_dirs]
        common = pkg_cflags("libxml-2.0")

        cs = [r for r in files if r.endswith(".c")]
        core, oracle = [], []
        for r in cs:
            if r in WIRING_FILES:
                (oracle if INC_OCI_RE.search(read(os.path.join(root, r))) else core).append(r)
            elif is_oracle_side(r):
                oracle.append(r)
            else:
                core.append(r)

        print("=== M2 compile proof: %s" % root)
        print("    %d Oracle headers stubbed, %d core files, %d Oracle-side files"
              % (len(stubbed), len(core), len(oracle)))
        if extra:
            print("    extra cflags: %s" % " ".join(extra))

        fails = 0
        for title, group, incs in (
                ("Part 1 - core, Oracle headers blocked", core, ["-I" + stub] + real_incs),
                ("Part 2 - Oracle side, real headers", oracle, real_incs)):
            print("\n--- %s" % title)
            for r in group:
                ok, errs = compile_one(root, r, incs, common + extra)
                print("  %s  %s" % ("PASS" if ok else "FAIL", r))
                if not ok:
                    fails += 1
                    for l in errs[: (40 if verbose else 6)]:
                        print("        " + l.replace(root + os.sep, ""))
        print("\nRESULT: %s - %d file(s) failed" % ("PASS" if fails == 0 else "FAIL", fails))
        sys.exit(0 if fails == 0 else 1)
    finally:
        shutil.rmtree(stub, ignore_errors=True)


if __name__ == "__main__":
    main()
