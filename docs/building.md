# Building and flashing

## Prerequisites

- PlatformIO Core >= 6.2 (`~/.platformio/penv/bin/pio` if it is not on PATH).
- Next to this repo: `../garnet_web` and `../garnet_settings` (local checkouts, linked by `symlink://`
  in `platformio.ini`; the published alternative is in a comment there).
- Optional, for the host tests and the tools: a checkout of `djerik/beo` at `./beo` (it is **not** part of
  this repository, see `.gitignore`; the tools and tests read it, never modify it).
  `git clone https://github.com/djerik/beo beo`

## Build

    pio run                                   # first build compiles the IDF libs: ~3 minutes
    pio run -t upload --upload-port <port>    # ~30 s afterwards
    python tools/serial_log.py <port> out.log 600   # log without resetting the board

The platform is pinned to the same pioarduino release as garnet_web's examples (`55.03.312-1`, Arduino
3.3.12, IDF 5.5.5). PlatformIO keeps a single Arduino core package, so projects on different releases keep
replacing each other's core. **Never add `lib_ignore`** to this project (pioarduino rewrites the shared
framework build script; garnet_web's CLAUDE.md explains).

### The hybrid build

`custom_sdkconfig` in `platformio.ini` makes pioarduino recompile the IDF libraries with our settings. It
generates `sdkconfig.*`, `sdkconfig.defaults`, `managed_components/` and a copy of the partition table in the
project root (all but `partitions.csv` are git-ignored). It is required, not optional:

| Setting | Why |
|---|---|
| `CONFIG_LWIP_IP4_REASSEMBLY=y`, `..._REASS_MAX_PBUFS=10` | the shipped lwIP drops fragments; every FPS datagram is 3 |
| `CONFIG_LWIP_MAX_SOCKETS=24` | five servers and the stream exhausted 16 (`accept error 23`) |
| `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y` | IDF never calls the WebSocket handler for the handshake, so the snapshot goes out from this callback |
| `CONFIG_BT_ENABLED=n`, Wi-Fi buffer numbers, `ARDUINO_LOOP_STACK_SIZE=6144` | heap: see architecture.md. **Bluetooth must be re-enabled when A2DP is added** |

Partitions: `partitions.csv` is `huge_app` (one 3 MB app slot, no OTA, by decision).

## First run

1. The board opens an access point `ESP32 Headphones-xxxx`.
2. Open `http://192.168.4.1:8000`, password `changeme` (change it in System), choose your Wi-Fi.
3. After the reboot it appears in the B&O app (close the app fully and reopen it).

Settings (garnet_web UI): Product (name, mDNS `productType`, **Enrol**), Audio (default volume, buffer,
I2S pins, debug always-listen), Status (live numbers). Product name and Audio changes apply after a reboot.

**Enrol** adds the product to the configuration every B&O product on the network shares, so that sources
offer it. It is persistent and cannot be removed from here. It is off by default; this repository's author
enabled it on their own network.
