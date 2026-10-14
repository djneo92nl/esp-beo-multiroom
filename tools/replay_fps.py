#!/usr/bin/env python3
"""Replay a stream.fps capture (see capture_fps.py) to a listener's UDP port 1234.

    replay_fps.py ESP_IP stream.fps [--speed 1.0] [--port 1234] [--loop]

Datagrams go out at their recorded spacing, sequence numbers and all. The listener must
already accept FPS on that port (a session, or the firmware's always-listen debug switch).
"""
import argparse, socket, struct, time

ap = argparse.ArgumentParser()
ap.add_argument("ip")
ap.add_argument("file")
ap.add_argument("--speed", type=float, default=1.0)
ap.add_argument("--port", type=int, default=1234)
ap.add_argument("--loop", action="store_true")
a = ap.parse_args()

recs = []
with open(a.file, "rb") as f:
    assert f.read(8) == b"FPSCAP1\n", "not a capture file"
    while True:
        h = f.read(12)
        if len(h) < 12:
            break
        t, n = struct.unpack("<dI", h)
        recs.append((t, f.read(n)))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
while True:
    start = time.perf_counter()
    for t, d in recs:
        wait = start + t / a.speed - time.perf_counter()
        if wait > 0:
            time.sleep(wait)
        s.sendto(d, (a.ip, a.port))
    print("sent %d datagrams" % len(recs))
    if not a.loop:
        break
