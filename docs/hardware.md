# Hardware

Plain ESP32 dev board (tested: a generic board, CP210x USB-serial, no PSRAM) and, for audio out, a PCM5102
board. **Audio has not been listened to yet.**

| PCM5102 pin | ESP32 pin (default) | Setting |
|---|---|---|
| BCK | GPIO 26 | Audio > I2S BCK pin |
| LCK / LRCK | GPIO 25 | Audio > I2S LRCK pin |
| DIN | GPIO 22 | Audio > I2S DATA pin |
| VIN, GND | 3V3 (or 5V per board), GND | |
| SCK | **GND** | most boards: solder bridge or wire so the chip makes its own clock |
| FLT, DEMP, FMT, XMT | per the board's silkscreen, usually low / low / low / high | |

Line-level output, **no hardware volume**: volume is digital in firmware (squared curve, ramped across each
24 ms block, applied from Mozart volume commands; the app sends mute as level 0).

I2S runs 16-bit stereo at the stream's rate: 48 kHz for what the Beoplay M5 sends, 44.1 kHz for beo's own source.
Reconfigured from the header when the format code changes.

Wi-Fi signal matters: the test board sat at about -65 to -72 dBm and lost a few seconds now and then. Put it
near the access point.

Later targets (not implemented): WT32-ETH01 (garnet_web has `examples/wt32_eth01`), Bluetooth A2DP, S/PDIF,
ESP32-P4.
