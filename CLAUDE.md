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

## State (2026-10-17)

| Phase | State |
|---|---|
| 0 analysis | done: `docs/esp32-port/findings.md` |
| 1 appear in the app | **verified on hardware**: discovered, set up, volume works from the app, WebSocket snapshot works. 36/36 routes identical to beo's recorded answers (host) |
| 2 receive and play | **verified against a real source on the unprotected path**: Beoplay M5 B&O Radio (`NONE`) joins and streams 48 kHz PCM, 0 loss on a steady link. Handshake byte-identical to beo (host). **Audio was never listened to (no DAC attached)** |
| 3 polish | **partly done**: smoothed drift correction, gain ramp, fades on underrun/flush, marker mute are built and host-tested; on hardware the 150 s test dropped corrections 1352 -> 762, underruns 77 -> 13, but the fill still averages above target (correction rate too low). Mute marker (odd counter = mute is a guess), rejoin and Wi-Fi-gap fades are untested |
| hearing it | **verified by ear (2026-10-17)**: a MAX98357A on BCLK 26 / LRC 25 / DIN 22 plays the test tone and B&O Radio from the M3/M5; occasional break-ups (underruns, see findings 14.1). Very loud: the amp's default gain. The PCM5102 board stays unsoldered |
| Beoplay M3 B&O Radio | **streams 44.1 kHz PCM** to the board (beo's native format), ~2 % loss at -75..-85 dBm |
| line-in of the M5 / Moment | **encrypted, out of reach (measured 2026-10-17)**: with the line-in silent (owner confirmed with an M3) the payload is still full-scale noise (entropy 7.978, 0.5 % zero bytes over 200 datagrams), and the M5 as a listener logs `Encryption ON` and never `OFF`, unlike B&O Radio. **Decrypting is out of scope for this assistant**; the project stops here. Findings 14 |

## TODO, in order

1. **Finish Phase 3.** Heard and measured so far: see the State table and findings 14.1.
   - drift: let the correction rate scale with the distance from target (up to one frame per block); the fill
     averages 7.7 blocks against a target of 4, so overruns and underruns persist (3-4 underruns per minute);
   - mute handling: markers set `gSourceMuted` by counter parity (odd = mute is a guess from beo's comments);
     test it from the app, plus fades at stream start/stop, on rejoin and after a Wi-Fi gap;
   - check `leave`/rejoin sequences from the app (`beolink/leave`, `state/standby`) leave no stale session or FPS task;
   - volume: Mozart level -> gain curve v^1.5 is a guess; the amp (MAX98357A) is loud at 20 %; we never push
     VOLUME notifications to the source. Check whether the test tone ignores the volume setting.
   - the test board's Wi-Fi is poor (-77 dBm at 2 m): try another board, antenna or supply.
3. **Reliability.** Heap is thin (~45-65 KB free during a stream). Candidates: move `link.cpp` I2S behind
   `AudioSink`; fewer/lower stacks (log high-water marks first: `uxTaskGetStackHighWaterMark`); consider
   disabling Wi-Fi AMPDU; fewer httpd instances. Bluetooth is off in `custom_sdkconfig` and must be
   re-enabled for A2DP, which will cost heap. The buffer is ~144 ms; more needs PSRAM.
4. **Untested paths:** a source dialling *us* (ConnectRequest) against a real source (only beo's Python
   source tested); 44.1 kHz stream; `tools/capture_fps.py` against a real stream and `tools/replay_fps.py`
   (never run); the debug "always listen" switch.
5. **Dropped:** the DECIMATE retest and the line-in goal. The Moment's line-in is encrypted (findings 14); the
   setting "Claim PROPRIETARY support" stays for experiments. A PSRAM ESP32-S3 would allow a ~1 s buffer
   (S3 has no Bluetooth Classic, so no A2DP there).
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
- The Wi-Fi link at -65 to -85 dBm drops for seconds; this particular board is poor even next to the AP.
- The board's DHCP address changes between boots (192.168.1.158, then .33): use the mDNS name, or read the IP
  from the serial log, before curl-ing it.
- Real-product volume: the app sets level 0 for mute; default volume is 60 and the gain curve v^1.5 (squared
  at 30 was -21 dB, silent through a bare DAC).

## Verify a change

Host tests (see docs/testing.md; need `./beo`): all four must pass. Firmware: `pio run`, flash, watch the
serial log (`tools/serial_log.py`). Say plainly which of the two a claim rests on.
