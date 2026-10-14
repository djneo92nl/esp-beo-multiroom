# Derived files (Apache-2.0, from djerik/beo)

Unofficial; not affiliated with Bang & Olufsen. Changed from the originals:
rewritten from Python to C++, restricted to a listening line-in style speaker.

| File here | Derived from (beo) |
|---|---|
| `src/core/mozart_api.cpp`, `.h` | `packages/mozart/beo/mozart/facade.py` (routes, payload shapes, status codes), `docs/app-protocol.md` |
| `src/core/player_state.cpp`, `.h` | `facade.py` (volume, power, snapshot, join/leave events), `mozart/ws.py` (event envelope) |
| `src/core/identity.h` | `packages/product/beo/product/identity.py`, `packages/mozart/beo/mozart/profile.py` |
| `src/esp32/app.cpp` (serial derivation, mDNS TXT, WebSocket) | `product/identity.py` `derive_serial`, `facade.py` `run()`, `mozart/ws.py` |
| `tests/host/compare_golden.py` | reads beo's `apps/beo-adapter/tests/golden/*` |

| `src/core/networklink_rx.cpp`, `.h` | `networklink/beocore.py`, `networklink/sources.py` (every stanza text and field order), `networklink/roles/listener.py` (the handshake) |
| `src/core/fps.h` | `networklink/fps.py` (header layout, marker, rates) |
| `src/core/enrol.cpp`, `.h` | `networklink/configuration.py` |
| `src/esp32/app.cpp` (product records) | `networklink/discovery.py` `Advertiser.records()` |
| `src/esp32/link.cpp`, `enrol_esp.cpp` | `roles/listener.py`, `configuration.py` (flow), rewritten for sockets and tasks on the ESP32 |
| `tools/capture_fps.py`, `test_source.py` | import and use beo's `ListenerRole`, `SourceRole` from the `beo/` checkout (unmodified) |

Changes from the originals: one-source catalogue instead of the 13-source standard one; event-driven
instead of threads; ids and ordering kept; the jitter buffer, I2S output and drift handling are new.
