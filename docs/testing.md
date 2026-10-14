# Testing and tools

Nothing here is run by CI; everything is a command. "Host" tests compile `src/core` with the system C++
compiler, no ESP32 needed. They compare the core against **beo's own code or recorded answers**.

## Host tests (need `./beo`)

    cd tests/host
    OUT=$TMPDIR   # anywhere writable
    c++ -std=c++17 -o $OUT/mozart_test test_mozart.cpp ../../src/core/mozart_api.cpp ../../src/core/player_state.cpp
    python3 compare_golden.py $OUT/mozart_test            # 36 routes identical to beo's golden answers

    c++ -std=c++17 -o $OUT/link_test test_link.cpp ../../src/core/networklink_rx.cpp
    python3 test_link.py $OUT/link_test                   # 19 lines: our listener vs beo's wire builders

    c++ -std=c++17 -o $OUT/enrol_test test_enrol.cpp ../../src/core/enrol.cpp
    python3 test_enrol.py $OUT/enrol_test                 # enrolment bodies vs beo's configuration.py

    c++ -std=c++17 -o $OUT/jt test_jitter.cpp && $OUT/jt  # parser and jitter buffer unit checks

`run.sh` does the first of these. Expected: all pass. `test_link.py` declares the same schemes as the firmware
(`NONE`, `DECIMATE`, `PROPRIETARY`); change both together.

What the host tests do **not** cover: the discard path for a multi-chunk FullStatus, the ESP32 sockets and tasks,
I2S, mDNS, garnet_web. Those are exercised on hardware only.

## Tools (`tools/`, stdlib only, Python 3)

| Tool | Does | Status |
|---|---|---|
| `capture_api.py HOST` | read-only GET sweep of a product's HTTP surface into `captures/` | run on four real products |
| `capture_fps.py` | a listener on this machine (beo's `ListenerRole`) that records XMPP, notifications and every FPS datagram; mDNS via macOS `dns-sd`; `--enrol` and `--join IP JID` are opt-in | self-tested for 4 s only; never used to receive a real stream |
| `test_source.py IP JID` | beo's `SourceRole` sends a tone to the ESP32 (a stand-in source) | used for all soak tests |
| `replay_fps.py IP file` | replays a `stream.fps` capture to UDP 1234 (set Audio > "Debug: always listen") | **untested** |
| `serial_log.py PORT OUT SECS` | append the serial log to a file without resetting the board; needs pyserial (PlatformIO's python has it) | used daily |

`captures/` is git-ignored: it holds the addresses, serials and MACs of real products.

## On hardware: what to look at

The serial log (and Settings > Status) shows, every 5 s while listening: signal peak/rms, arrival spacing
(min/avg/max in ms; real-time is 22.1 ms at 48 kHz), buffer fill, datagrams received/lost, underruns/overruns,
markers, unrecognised datagrams (`bad`), heap. Every 10 s: heap, largest block, Wi-Fi status, RSSI.
Wi-Fi drops are logged with their reason. Unrecognised datagrams are dumped (first 3) as `DG<n>.<offset>.<len>:<hex>;`
lines for offline analysis.

Gotchas: `pkill -f serial_log.py` also matches any shell command whose text contains that string; the serial
port is exclusive (stop the logger before `pio run -t upload`); power-cycling the board kills a running logger.
