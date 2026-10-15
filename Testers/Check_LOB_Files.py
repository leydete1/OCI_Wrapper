#!/usr/bin/env python3
"""
Check_LOB_Files.py - point a5 (2026-10-14): do the LOB files a SELECT
response points at hold that SELECT's data?

Usage (gateway running, after Reset_Tester_Objects.sql):
    python3 Check_LOB_Files.py [base_url] [blob_dir] [clob_dir]

Defaults:
    base_url  https://localhost:8443
    blob_dir  /home/leyden100/eclipse-workspace/OCI_Wrapper/output_lobs
    clob_dir  /home/leyden100/eclipse-workspace/OCI_Wrapper/output_clobs
(use the BLOB_output_dir and CLOB_output_dir values from config.ini).

What it does:
  - opens a session (CREATE_SESSION, as the tester does);
  - runs 5 rounds; each round sends three SELECTs AT THE SAME TIME, one
    each for the seed rows 4242, 4243 and 4244:
        SELECT NUMBER_COL, CLOB_COL, BLOB_COL FROM OCI_FIELD_TEST
         WHERE NUMBER_COL = <n> AND <round> = <round>
    (the "AND r = r" makes each round's SQL different, so every round
    really runs instead of coming from the resultset cache);
  - when all rounds are done, opens every CLOB and BLOB file each
    response names and compares it with that row's seed value:
        4242  CLOB 'Seed CLOB value for row 4242.'  BLOB 01 02 03 04 05 06 07 08
        4243  CLOB 'Seed CLOB value for row 4243.'  BLOB DE AD BE EF 42 43
        4244  CLOB 'Seed CLOB value for row 4244.'  BLOB DE AD BE EF 42 44
  - ends the session.

Before point a1-a3 every response names CLOB_COL_row1_clob0.txt and
blob_output_0, so expect most files to hold another row's data. After,
every file is the response's own. Exit code 0 = all OK, 1 = not.
Reads nothing but the responses and the two output folders; writes
nothing.
"""
import json
import os
import re
import ssl
import sys
import threading
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "https://localhost:8443"
BLOB_DIR = sys.argv[2] if len(sys.argv) > 2 else \
    "/home/leyden100/eclipse-workspace/OCI_Wrapper/output_lobs"
CLOB_DIR = sys.argv[3] if len(sys.argv) > 3 else \
    "/home/leyden100/eclipse-workspace/OCI_Wrapper/output_clobs"
ROUNDS = 5

SEED = {
    4242: (b"Seed CLOB value for row 4242.", bytes.fromhex("0102030405060708")),
    4243: (b"Seed CLOB value for row 4243.", bytes.fromhex("DEADBEEF4243")),
    4244: (b"Seed CLOB value for row 4244.", bytes.fromhex("DEADBEEF4244")),
}
CTX = ssl._create_unverified_context()   # same as the tester (self-signed)


def post(body, ctype):
    req = urllib.request.Request(BASE + "/", data=body.encode("utf-8"),
                                 headers={"Content-Type": ctype}, method="POST")
    with urllib.request.urlopen(req, context=CTX, timeout=60) as r:
        return r.read().decode("utf-8", errors="replace")


def create_session():
    resp = post("<Session_Request><operation>CREATE_SESSION</operation>"
                "<application_name>Check_LOB_Files</application_name>"
                "</Session_Request>", "application/xml")
    m = re.search(r"<session_id>([^<]+)</session_id>", resp)
    if not m:
        sys.exit("CREATE_SESSION failed - response was:\n" + resp)
    return m.group(1)


def end_session(sid):
    try:
        post("<Session_Request><operation>END_SESSION</operation>"
             "<session_id>%s</session_id></Session_Request>" % sid,
             "application/xml")
    except Exception:
        pass


def select(sid, n, rnd):
    body = {
        "external_audit_id": "check_lob_files_%d_%d" % (n, rnd),
        "session_id": sid,
        "transaction": {"required": 1, "operations": [{
            "type": "SELECT",
            "sql": "SELECT NUMBER_COL, CLOB_COL, BLOB_COL FROM OCI_FIELD_TEST "
                   "WHERE NUMBER_COL = %d AND %d = %d" % (n, rnd, rnd)}]}}
    return post(json.dumps(body), "application/json")


def names_in(resp):
    """(clob file names, blob file names) a response points at."""
    clobs = set(re.findall(r"(CLOB_COL[^\"'<>\s/\\]*?\.txt)", resp))
    blobs = set(re.findall(r'"file_name"\s*:\s*"([^"]+)"', resp)) | \
        set(re.findall(r"<file_name>([^<]+)</file_name>", resp))
    return sorted(clobs), sorted(blobs)


def owner_of(data, kind):
    for n, (c, b) in SEED.items():
        if data == (c if kind == "CLOB" else b):
            return n
    return None


def main():
    sid = create_session()
    print("session %s - %d rounds x 3 SELECTs at once (rows 4242, 4243, 4244)"
          % (sid, ROUNDS))
    results = []          # (round, row, response)
    lock = threading.Lock()

    def worker(n, rnd):
        try:
            r = select(sid, n, rnd)
        except Exception as e:
            r = "TRANSPORT ERROR %s" % e
        with lock:
            results.append((rnd, n, r))

    for rnd in range(1, ROUNDS + 1):
        ths = [threading.Thread(target=worker, args=(n, rnd)) for n in SEED]
        for t in ths:
            t.start()
        for t in ths:
            t.join()
    end_session(sid)

    ok = wrong = missing = noname = 0
    for rnd, n, resp in sorted(results):
        clobs, blobs = names_in(resp)
        if not clobs or not blobs:
            noname += 1
            print("  round %d row %d: NO FILE NAMES in response: %.200s"
                  % (rnd, n, resp.replace("\n", " ")))
            continue
        for kind, names, folder in (("CLOB", clobs, CLOB_DIR), ("BLOB", blobs, BLOB_DIR)):
            want = SEED[n][0] if kind == "CLOB" else SEED[n][1]
            for name in names:
                path = os.path.join(folder, name)
                try:
                    data = open(path, "rb").read()
                except OSError:
                    missing += 1
                    print("  round %d row %d: %s %s MISSING (%s)" % (rnd, n, kind, name, path))
                    continue
                if data == want:
                    ok += 1
                    print("  round %d row %d: %s %-50s OK" % (rnd, n, kind, name))
                else:
                    wrong += 1
                    who = owner_of(data, kind)
                    print("  round %d row %d: %s %-50s WRONG - holds %s"
                          % (rnd, n, kind, name,
                             "row %d's data" % who if who else repr(data[:40])))

    print("\nRESULT: %d file(s) OK, %d WRONG, %d missing, %d response(s) without file names"
          % (ok, wrong, missing, noname))
    sys.exit(0 if (wrong == 0 and missing == 0 and noname == 0) else 1)


if __name__ == "__main__":
    main()
