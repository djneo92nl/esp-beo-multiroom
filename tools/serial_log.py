#!/usr/bin/env python3
"""Append the board's serial output to a file for N seconds: serial_log.py PORT OUT SECONDS
Does not reset the board. Needs pyserial (PlatformIO's python has it)."""
import sys, time, serial
port, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
s = serial.Serial(port, 115200, timeout=1)
end = time.time() + secs
with open(out, "ab") as f:
    while time.time() < end:
        b = s.read(4096)
        if b:
            f.write(b); f.flush()
