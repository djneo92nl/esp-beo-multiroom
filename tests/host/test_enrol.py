#!/usr/bin/env python3
"""The C++ enrolment builders against beo's configuration.py. Usage: test_enrol.py <binary> [beo]"""
import os, subprocess, sys
beo = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../beo")
for p in ("product", "networklink"):
    sys.path.insert(0, os.path.join(beo, "packages", p))
from beo.networklink import ProductIdentity, SOURCES, configuration          # noqa: E402
from beo.networklink.beocore import double_quotes                           # noqa: E402
ident = ProductIdentity.from_jid("3150.1200322.99000777", "Capture Node", "00:00:5e:00:53:07", product_type="SEOUL")
entry = configuration.device_entry(ident, [SOURCES["CD"].named("Capture Node")])
want = [double_quotes(configuration.insert_request(entry)), double_quotes(configuration.subscribe_request()), entry,
        "scan present=1 done=1", "scan present=0 done=1"]
got = subprocess.run([sys.argv[1]], capture_output=True, text=True).stdout.split("\n")[:5]
bad = [i for i in range(5) if got[i] != want[i]]
for i in bad:
    print("DIFF", i, "\n got ", got[i][:300], "\n want", want[i][:300])
print("%d of 5 identical" % (5 - len(bad)))
sys.exit(1 if bad else 0)
