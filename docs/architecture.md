# Architecture

An ESP32 that appears in the B&O app as a Mozart product and plays NetworkLink audio from a source
the app joins it to. Unofficial; see `NOTICE`. Evidence tags used in this repo: **read** (in beo's
code), **measured** (run here), **inferred**, **unknown**. Findings and measurements:
[esp32-port/findings.md](esp32-port/findings.md).

## Layers

```
src/core        pure C++17, no platform headers; compiled and tested on the host
  mozart_api      REST surface the B&O app uses (request in, response out)
  player_state    volume, power, play state, WebSocket events and snapshot
  networklink_rx  beocore listener: stanza parser + handshake state machine (event driven)
  enrol           the network's shared-configuration entry and requests
  fps             FPS datagram parser (audio and the 16-byte marker)
  jitter_buffer   ring of 4248-byte blocks, single producer / single consumer, lock free
  identity, json  small helpers
src/platform    the three interfaces core sees: net.h, audio_sink.h, state.h (StatePort, LinkPort)
src/esp32       the implementation on ESP-IDF / Arduino
  app.cpp         identity from the efuse MAC, REST servers, WebSocket, mDNS records, peer discovery
  link.cpp        sockets and tasks for NetworkLink, FPS receive, jitter buffer, I2S output
  enrol_esp.cpp   enrolment client (opt-in)
  settings.cpp    garnet_settings groups (the only place user settings live)
src/main.cpp    garnet_web + the above
```

`audio_sink.h` is declared but not yet used: `link.cpp` drives I2S directly. Moving it behind the
interface is a cleanup, not a behaviour change.

## Network surface

| Port | What | Who |
|---|---|---|
| 80 | Mozart REST (`/api/v1/*`) and a few legacy paths | the B&O app. The app builds URLs without a port, so this cannot move |
| 8080 | the same handler: the app's liveness ping `/?`, `/`, `/Ping`, `/BeoDevice` | the app, other products |
| 9339 | WebSocket notifications (snapshot on connect) | the app |
| 5222 | beocore (XMPP-like), accepts sessions and dials sources | other products |
| 1234 UDP/TCP | FPS audio and heartbeat, only while listening | the source |
| 8000 | garnet_web settings UI | you (password `changeme` until changed) |

garnet_web is on 8000 so Mozart can own 80 (decision after Phase 0). Consequence: no OS captive-portal
detection; first-run setup is "join the setup AP, open `http://192.168.4.1:8000`".

mDNS (all on garnet's responder, host name `<model>-<serial>`): `_bangolufsen._tcp` (the app),
`_beocore._tcp` (instance = the jid), `_beoremote._tcp` (8080), `_beo_settings._tcp`. Other products find
us by `_beocore`, and four real products opened sessions to us within seconds.

## Flow: how a stream starts (measured against a Beoplay M5, B&O Radio)

1. In the app, "add to experience": the app `POST /api/v1/beolink/join/<source jid>` to **us**.
2. `Mozart` -> `LinkPort::join` -> link task dials the source on 5222 (the source's address comes from
   the peer list, remembered for 20 minutes).
3. We send `Experiences`; the source answers with what it plays; we bind UDP+TCP 1234, send `Connect`
   (`FPS://<our ip>`, declaring `NONE`, `DECIMATE`, `PROPRIETARY`), then `_notification-subscribe`.
4. The source opens the TCP heartbeat, streams UDP, asks our volume, and answers our subscribe.
5. `started()` -> `PlayerState::join_result` -> WebSocket `WebSocketEventBeolinkJoinResult` (type `join`).
6. Teardown from either side: `Disconnect`, our own `Disconnect` + unsubscribe, FPS closed.

A source may also dial us (ConnectRequest); that path is built and tested against beo's source.

## Audio path

`fps` task (core 0, prio 6): `recvfrom` -> parse -> sequence gap -> `JitterBuffer::push`.
`audio` task (core 1, prio 7): wait for prefill (half the blocks) -> pop block -> volume ramp across the
block (squared curve) -> `i2s_channel_write` (blocks on the DMA, which paces playback). I2S is
reconfigured when the header's format code changes (1 = 44.1 kHz, 2 = 48 kHz). Underrun re-prefills.

No drift correction yet (Phase 3). The PTS in the header is ignored: no PTP, no multiroom sync.

## Memory (the hard constraint)

Plain ESP32, no PSRAM. Measured: ~120 KB free with everything up, the stream's own plumbing takes ~55 KB.
Therefore:

- the jitter buffer is allocated **once at boot, block by block**, as many as fit beside a 100 KB reserve
  (about 6 blocks = 144 ms at 48 kHz); the Status page and the log show what was granted;
- large stanzas we do not need (the source's FullStatus) are skipped without buffering;
- `custom_sdkconfig` in `platformio.ini` trims Wi-Fi buffers, turns Bluetooth off (it must come back for
  A2DP), enables IP reassembly (every FPS datagram is 3 IP fragments), 24 sockets, WebSocket post-handshake
  callback;
- a stream pauses peer discovery; logging of inbound stanzas is short.

## Identity

jid `3150.1200322.99<6 digits>@products.bang-olufsen.com`: Beolab 8 type (the only type number verified
against the real app), item 1200322, serial derived from the efuse MAC like beo derives it from the machine id.
The product name is a setting; the host name and the serial are fixed at first boot.

## Not here

Master Link, classic systems, Bluetooth, Ethernet, S/PDIF, ESP32-P4: later targets, hardware stays behind
`src/platform`.
