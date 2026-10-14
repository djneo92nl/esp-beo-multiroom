// FPS: B&O's NetworkLink audio datagram. Layout from beo's networklink/fps.py
// (Apache-2.0, Tobias Laursen); see docs/esp32-port/findings.md section 4.1. No platform headers.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fps {

constexpr uint16_t kMagic = 0x1234;
constexpr size_t kHeaderBytes = 32;
constexpr size_t kFramesPerDatagram = 1062;
constexpr size_t kPayloadBytes = kFramesPerDatagram * 4; // S16LE stereo
constexpr size_t kMarkerBytes = 16;
constexpr uint16_t kPort = 1234;

enum class Kind : uint8_t { Bad, Audio, Marker };

struct Packet {
  Kind kind = Kind::Bad;
  uint16_t seq = 0;
  uint16_t format = 0;       // high word of header bytes 4..7: 1 = 44.1 kHz, 2 = 48 kHz
  uint16_t marker_counter = 0;
  const uint8_t *header = nullptr; // the 32 header bytes of an audio datagram
  const uint8_t *pcm = nullptr;
  size_t pcm_bytes = 0;
};

inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// Header fields are big-endian, PCM is little-endian. A marker is 16 bytes with 0x0200 at 6..7.
inline Packet parse(const uint8_t *d, size_t n) {
  Packet p;
  if (n < 4 || be16(d) != kMagic) return p;
  p.seq = be16(d + 2);
  if (n == kMarkerBytes && d[6] == 0x02 && d[7] == 0x00) {
    p.kind = Kind::Marker;
    p.marker_counter = be16(d + 4);
    return p;
  }
  if (n < kHeaderBytes) return p;
  p.kind = Kind::Audio;
  p.header = d;
  p.format = be16(d + 4);
  p.pcm = d + kHeaderBytes;
  p.pcm_bytes = n - kHeaderBytes;
  return p;
}

inline uint32_t rate_for_format(uint16_t format) { return format == 2 ? 48000 : 44100; }

} // namespace fps
