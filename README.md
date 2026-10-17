# ESP32 network speaker (proof of concept)

A plain ESP32 that shows up in the Bang & Olufsen app as a Mozart product and plays NetworkLink audio
to headphones through a PCM5102, when the app adds it to a source's experience.

**Unofficial. Not affiliated with, endorsed by, or supported by Bang & Olufsen.** The protocol knowledge
comes from [djerik/beo](https://github.com/djerik/beo) (Apache-2.0, Tobias Laursen). See `NOTICE` and
[docs/esp32-port/DERIVED.md](docs/esp32-port/DERIVED.md) for which files are derived. Beo, Mozart,
NetworkLink, BeoLink are Bang & Olufsen trademarks, used only to name protocols.

## What works (and what was actually run)

| | Status |
|---|---|
| Appears in the B&O app, setup completes, volume works from the app | **verified on hardware** |
| REST answers match beo's recorded answers (36 routes) | **verified on the host** |
| mDNS records, WebSocket snapshot, peer discovery | **verified on hardware** |
| NetworkLink listener handshake | byte-identical to beo's wire code (**host**); works against beo's Python source and a **real Beoplay M5** (B&O Radio) (**hardware**) |
| Receiving a real stream (M5 B&O Radio: 48 kHz PCM, 0 loss on a steady link) | **verified on hardware** |
| Hearing it | **verified by ear (MAX98357A)**: test tone and B&O Radio from an M3/M5 play, with occasional break-ups |
| Drift correction, mute fades, clean rejoin (Phase 3) | drift correction and fades **built, host-tested, partly measured** (fill still above target); mute marker and rejoin untested |
| Line-in of a Beosound Moment | **encrypted, not supported** (measured: full-scale noise on a silent input, receiver logs `Encryption ON`; findings section 14) |
| Line-in of a Beoplay M3 / M5 | **joins and streams plain PCM** (measured with a silent input, `0x1234`); not yet heard with music |
| Enrolment in the network's shared configuration | done once on the author's network; the leader's reply is not read (see CLAUDE.md) |

Buffer: about 216 ms on a plain ESP32 beside Wi-Fi, the web UI and the servers (heap-limited; more needs PSRAM).

## Quick start

    git clone https://github.com/djerik/beo beo       # optional: only for the host tests and tools
    pio run -t upload --upload-port <port>            # needs ../garnet_web and ../garnet_settings

Then join the board's setup access point, open `http://192.168.4.1:8000` (password `changeme`) and choose
your Wi-Fi. Details: [docs/building.md](docs/building.md). Wiring (PCM5102, SCK to ground):
[docs/hardware.md](docs/hardware.md).

## Documentation

| | |
|---|---|
| [docs/architecture.md](docs/architecture.md) | layers, ports, the join flow, audio path, memory |
| [docs/building.md](docs/building.md) | build, the hybrid sdkconfig, first run, settings |
| [docs/hardware.md](docs/hardware.md) | board, PCM5102 wiring, Wi-Fi |
| [docs/testing.md](docs/testing.md) | host tests against beo, tools, what to read in the log |
| [docs/esp32-port/findings.md](docs/esp32-port/findings.md) | protocol findings, measurements, the PROPRIETARY result, decisions |
| [docs/esp32-port/DERIVED.md](docs/esp32-port/DERIVED.md) | which files derive from beo |
| [CLAUDE.md](CLAUDE.md) | project rules, state, and the ordered TODO |

## Layout

    src/core      no platform headers: mozart_api, player_state, networklink_rx, enrol, fps, jitter_buffer
    src/platform  interfaces: net.h, audio_sink.h, state.h
    src/esp32     garnet_settings groups, esp_http_server, mDNS, sockets/tasks, I2S, enrolment
    tests/host    the core compiled on the host and diffed against beo
    tools/        capture and test scripts (stdlib Python)
    docs/         documentation

## Licence

Apache-2.0 (`LICENSE`). Derived from beo; the `NOTICE` attribution must be kept.
