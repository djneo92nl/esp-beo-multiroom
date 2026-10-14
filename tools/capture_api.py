#!/usr/bin/env python3
"""Read-only capture of a B&O product's HTTP surface (stdlib only, GET requests only).

    capture_api.py HOST [OUTDIR]

Walks the service index on :8080 (BeoZone family) and the Mozart paths on :80, following
no links it does not know, and writes one JSON file per answer plus index.json with the
status codes. Used to compare a real product (or the Pi adapter) with the ESP32.
"""
import json, os, sys, urllib.request, urllib.error

host = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join("captures", "api-" + host)
os.makedirs(out, exist_ok=True)

PATHS_8080 = ["/", "/Ping", "/BeoDevice", "/BeoNotify", "/BeoZone", "/BeoZone/System", "/BeoZone/System/Products",
              "/BeoZone/Zone", "/BeoZone/Zone/Sources", "/BeoZone/Zone/ActiveSources", "/BeoZone/Zone/Stream",
              "/BeoZone/Zone/Sound", "/BeoZone/Zone/Sound/Volume/Speaker/Level", "/BeoZone/Zone/Sound/Mode",
              "/BeoZone/Zone/List", "/BeoZone/Zone/PlayQueue", "/BeoContent", "/BeoHome", "/BeoSecurity",
              "/BeoInput", "/BeoDevice/powerManagement"]
PATHS_80 = ["/api/v1/product/info", "/api/v1/state", "/api/v1/beolink/self", "/api/v1/beolink/listeners",
            "/api/v1/beolink/audioleader", "/api/v1/beolink/peers", "/api/v1/beolink/available-listeners",
            "/api/v1/playback/state", "/api/v1/playback/sources", "/api/v1/playback/sources/active",
            "/api/v1/sound/volume", "/api/v1/remote"]

index = {}
for port, paths in ((8080, PATHS_8080), (80, PATHS_80)):
    for p in paths:
        url = "http://%s:%d%s" % (host, port, p)
        try:
            with urllib.request.urlopen(url, timeout=5) as r:
                code, body = r.status, r.read()
        except urllib.error.HTTPError as e:
            code, body = e.code, e.read()
        except Exception as e:  # noqa: BLE001
            code, body = 0, str(e).encode()
        name = "%d_%s.json" % (port, p.strip("/").replace("/", "_") or "root")
        with open(os.path.join(out, name), "wb") as f:
            f.write(body)
        index["%d%s" % (port, p)] = code
        print(code, port, p, len(body))
json.dump(index, open(os.path.join(out, "index.json"), "w"), indent=1, sort_keys=True)
