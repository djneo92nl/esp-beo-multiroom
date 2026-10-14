#!/usr/bin/env python3
"""Play a test tone to a listener with beo's own SourceRole: the stand-in for a B&O source.

    test_source.py ESP_IP ESP_JID [--seconds 20] [--freq 440] [--beo PATH]

This is how the ESP32 listener is exercised without touching the household's products.
It is beo's source, not a real one: a real source may behave differently (see
docs/esp32-port/findings.md). The stream is PCM; PTS is our monotonic clock, which a B&O
listener would reject but the ESP32 ignores.
"""
import argparse, logging, os, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("ip")
ap.add_argument("jid")
ap.add_argument("--seconds", type=float, default=20)
ap.add_argument("--freq", type=float, default=440.0)
ap.add_argument("--beo", default=os.path.join(os.path.dirname(__file__), "..", "beo"))
a = ap.parse_args()
for p in ("product", "networklink"):
    sys.path.insert(0, os.path.join(a.beo, "packages", p))
from beo.networklink import (Catalog, FORMAT_44100, MonotonicClock, Peer, ProductIdentity, SOURCES,   # noqa: E402
                             STANDARD_SOURCES, SourceRole, ToneSource)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname).1s %(message)s")
ident = ProductIdentity.from_jid("3150.1200322.99000778", "Test Source", "02:00:5E:00:53:78", product_type="SEOUL",
                                 host="Test-Source-99000778")
role = SourceRole(ident, Catalog(STANDARD_SOURCES), MonotonicClock(), resolve_fps_host=False)
peer = Peer(jid=a.jid if "@" in a.jid else a.jid + "@products.bang-olufsen.com", ip=a.ip, name="listener")
lis = role.expand(peer, SOURCES["linein"], ToneSource(FORMAT_44100, a.freq, 0.2))
print("expand ->", "playing" if lis else "FAILED")
if lis:
    time.sleep(a.seconds)
    role.unexpand(peer)
    print("unexpanded")
