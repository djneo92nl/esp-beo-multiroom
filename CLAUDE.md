# ESP32 network speaker (unofficial B&O NetworkLink receiver)

Proof of concept: a plain ESP32 shows up in the Bang & Olufsen app as a Mozart product and plays NetworkLink
audio, from a source the app joins it to, to headphones through a PCM5102. **Unofficial, not affiliated with
Bang & Olufsen.** Protocol knowledge is derived from djerik/beo (Apache-2.0): keep `NOTICE`, keep
`docs/esp32-port/DERIVED.md` current when a file is derived, never modify the `./beo` checkout (it is not part of
this repo). Read first: `README.md`, `docs/architecture.md`, `docs/esp32-port/findings.md`.

## Rules (from the owner)

- Honest status. Never claim something works on hardware unless it was run. Tag behaviour as
  **verified / built / guessed** (findings.md uses read / measured / inferred / unknown).
- Reuse garnet_web / garnet_settings (`../garnet_web`, `../garnet_settings`) for Wi-Fi setup, settings UI, system
  group. All user-facing settings are `garnet_settings` groups (`src/esp32/settings.cpp`), not Kconfig. Do not
  rebuild a settings page, portal or OTA. If the library lacks something, list it and propose a small upstreamable
  change. Read garnet_web's CLAUDE.md before touching it; **never add `lib_ignore`**.
- One framework: PlatformIO + pioarduino (pinned release), ESP-IDF APIs only from inside that project.
- Core (`src/core`) includes no platform headers; hardware stays behind `src/platform`.
- Keep the beo Python tree untouched; everything of ours lives outside `./beo`.
- Work phase by phase; stop for review at the end of a phase.
- Git: commit on the currently checked-out branch, never create or switch branches unless asked (check with
  `git branch --show-current` right before committing).
- The user's B&O products are real: read-only requests are fine; anything that changes them (enrolment,
  expand, source changes) needs the user's explicit go-ahead, and do not try to decrypt or defeat a source's
  content protection (see below).

## State (2026-10-14)

| Phase | State |
|---|---|
| 0 analysis | done: `docs/esp32-port/findings.md` |
| 1 appear in the app | **verified on hardware**: discovered, set up, volume works from the app, WebSocket snapshot works. 36/36 routes identical to beo's recorded answers (host) |
| 2 receive and play | **verified against a real source on the unprotected path**: Beoplay M5 B&O Radio (`NONE`) joins and streams 48 kHz PCM, 0 loss on a steady link. Handshake byte-identical to beo (host). **Audio was never listened to (no DAC attached)** |
| 3 polish | not started (see TODO) |
| line-in of the M5 / Moment | **blocked**: `PROPRIETARY` stream is encrypted (payload is statistically random, magic `0x4321`, 10 ms / 1972 B datagrams); a source refuses a listener that does not claim `PROPRIETARY`. See findings 11 |

## TODO, in order

1. **Hear it.** Attach a PCM5102 (docs/hardware.md), play M5 B&O Radio, listen: clicks, speed, volume, mute.
   Everything about audio quality is unverified until then.
2. **Phase 3 polish** (the log already shows the need: ~20 overruns per minute, underruns after Wi-Fi gaps):
   - drift correction: keep buffer fill near target by dropping/repeating a sample, log corrections
     (`gLinkStats.corrections` exists, unused). The source's clock runs a little ahead of I2S;
   - mute handling: the 16-byte FPS marker (pause/play toggle) is only counted; fade out/in instead; fade at
     stream start/stop and on rejoin; today playback stops abruptly and leftover blocks drain;
   - Wi-Fi reconnect: fade, keep the buffer sane, rejoin; check `leave`/rejoin sequences from the app
     (`beolink/leave`, `state/standby`) leave no stale session or FPS task;
   - volume: Mozart level -> gain curve is a guess (squared); the source asks our volume with range 0..100,
     whether that scale is right for the source is unknown; we never push VOLUME notifications to the source.
3. **Reliability.** Heap is thin (~45-65 KB free during a stream). Candidates: move `link.cpp` I2S behind
   `AudioSink`; fewer/lower stacks (log high-water marks first: `uxTaskGetStackHighWaterMark`); consider
   disabling Wi-Fi AMPDU; fewer httpd instances. Bluetooth is off in `custom_sdkconfig` and must be
   re-enabled for A2DP, which will cost heap. The buffer is ~144 ms; more needs PSRAM.
4. **Untested paths:** a source dialling *us* (ConnectRequest) against a real source (only beo's Python
   source tested); 44.1 kHz stream; `tools/capture_fps.py` against a real stream and `tools/replay_fps.py`
   (never run); the debug "always listen" switch.
5. **DECIMATE retest (needs the owner's go-ahead):** our shared-configuration entry still lists `PROPRIETARY`
   under `Render#1/Fps#1/ContentProtection#1`. The Moment answered "Unable to add sink" when our Connect
   declared only `NONE`/`DECIMATE`; unknown whether it looked at the Connect or at the entry. Re-inserting
   changes the household's configuration again (whether Insert replaces is unknown). Probably moot.
6. **Enrolment robustness:** the leader replies to our request on a session *it* opens to us, not on ours, so
   `enrol_esp.cpp` cannot read the reply (it inserts once per boot, then gives up). Handle the reply in the
   inbound session (`<accepted>1</accepted>`), or stop enrolling every boot once done (a persisted flag).
7. **Mozart/app:** the Line-In-only remote tree and plain-speaker states were never recorded against the real
   app beyond what works (discovery, setup, volume). Missing legacy surface on 8080 that real products serve
   (`/BeoNotify/Notifications` held-open stream, `/BeoZone/*`, see tools/capture_api.py output): needed only if
   a peer asks. `_beoremote` `productType` is the type name (`SEOUL`, a setting); a real M3 once only answered
   to its hardware code (`CA17`): unknown which a source wants from a listener.
8. **Security hygiene:** garnet_web default password `changeme` is compiled in; the Mozart API is
   unauthenticated by design (LAN). Say so in the docs; consider forcing a password change.
9. **Later targets** (stay behind `src/platform`): WT32-ETH01, Bluetooth A2DP, S/PDIF, ESP32-P4.
10. **Cleanup:** debug logging is verbose (every stanza, 300 chars); the unrecognised-datagram hex dump; the
    `captures/` and `.pio` directories are git-ignored. Decide what to keep at log level info.

## Gotchas learned the hard way

- IDF does not call a WebSocket URI handler for the handshake: snapshot is sent from
  `ws_post_handshake_cb` (needs the sdkconfig flag).
- The shipped lwIP has IP reassembly off, so an unmodified build receives no audio at all.
- `pkill -f serial_log.py` kills any shell command whose text contains it; stop the logger before flashing;
  power-cycling the board ends a running logger. `sleep N && ...` chains are blocked in this environment:
  use background commands / Monitor.
- Real products open sessions to us as soon as they see our `_beocore` record and keep them idle; two session
  slots fill at once, so a new dial must drop the oldest idle one (`pickSlot`).
- One mDNS pass misses products; the peer list is kept for 20 minutes.
- The app joins via the listener (`POST beolink/join/<jid>` to us), not via expand on the source.
- Heap: a single big `malloc` fails long before the free total runs out; allocate in blocks at boot.
- The Wi-Fi link at -65 to -72 dBm drops for seconds; put the board near the AP before blaming the stream.

## Verify a change

Host tests (see docs/testing.md; need `./beo`): all four must pass. Firmware: `pio run`, flash, watch the
serial log (`tools/serial_log.py`). Say plainly which of the two a claim rests on.
