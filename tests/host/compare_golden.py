#!/usr/bin/env python3
"""Diff the core's answers against beo's golden files (recorded from the Python adapter).

Only routes that do not depend on the product profile are compared exactly. The profile-
dependent ones (remote menu, sources, playback state) and the BeoZone legacy surface are
skipped on purpose: the ESP32 is a line-in style speaker, not a Classics Adapter.
Usage: compare_golden.py <test binary> [golden dir]
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
binary = sys.argv[1]
GOLD = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "../../beo/apps/beo-adapter/tests/golden")
index = json.load(open(os.path.join(GOLD, "_index.json")))

SKIP = {"/api/v1/remote", "/api/v1/playback/sources", "/api/v1/playback/state", "/api/v1/playback/sources/active",
        "/api/v1/content/active", "/api/v1/classics-adapter", "/api/v1/beolink/peers", "/api/v1/beolink/audioleader",
        "/api/v1/beolink/listeners", "/BeoContent", "/BeoHome", "/BeoNotify", "/BeoSecurity",
        "/BeoSecurity/Sessions", "/BeoDevice/powerManagement"}
EXACT = [p for p in index if p not in SKIP and not p.startswith("/BeoZone") and p != "/"]
out = subprocess.run([binary, "golden"], input="".join("GET %s\n" % p for p in EXACT),
                     capture_output=True, text=True).stdout
got = {}
for l in out.splitlines():
    parts = l.split(" ", 3)
    if parts[0] == "GET":
        got[parts[1]] = (int(parts[2]), parts[3] if len(parts) > 3 else "")
bad = ok = 0
for p in EXACT:
    want = json.load(open(os.path.join(GOLD, index[p])))
    code, body = got[p]
    if isinstance(want, str) and want.startswith("<NOT_FOUND"):
        good = code == 404
    elif isinstance(want, str) and want.startswith("<NO_CONTENT"):
        good = code == 204
    else:
        good = code == 200 and json.loads(body) == want
    if good:
        ok += 1
    else:
        bad += 1
        print("DIFF", p, code, body[:300], "\n   want", json.dumps(want)[:300])
print("%d identical, %d different, of %d routes compared" % (ok, bad, len(EXACT)))
sys.exit(1 if bad else 0)
