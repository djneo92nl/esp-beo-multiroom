// Unit checks for the FPS parser and the jitter buffer (no platform headers).
#include <cassert>
#include <cstdio>
#include <vector>

#include "../../src/core/jitter_buffer.h"

static size_t budget = 1u << 30;
static size_t fake_free() { return budget; }

int main() {
  // parser
  std::vector<uint8_t> d(fps::kHeaderBytes + fps::kPayloadBytes, 0);
  d[0] = 0x12; d[1] = 0x34; d[2] = 0x00; d[3] = 0x07; d[4] = 0x00; d[5] = 0x01;
  fps::Packet p = fps::parse(d.data(), d.size());
  assert(p.kind == fps::Kind::Audio && p.seq == 7 && p.format == 1 && p.pcm_bytes == fps::kPayloadBytes);
  assert(fps::rate_for_format(1) == 44100 && fps::rate_for_format(2) == 48000);
  uint8_t m[16] = {0x12, 0x34, 0x00, 0x08, 0x00, 0x03, 0x02, 0x00};
  p = fps::parse(m, 16);
  assert(p.kind == fps::Kind::Marker && p.seq == 8 && p.marker_counter == 3);
  d[0] = 0;
  assert(fps::parse(d.data(), d.size()).kind == fps::Kind::Bad);
  assert(fps::parse(d.data(), 3).kind == fps::Kind::Bad);

  // buffer: prefill, order, underrun, refill
  JitterBuffer jb(8, 0.5f);
  assert(jb.capacity() == 8);
  std::vector<uint8_t> blk(fps::kPayloadBytes);
  assert(jb.front() == nullptr); // nothing queued
  for (int i = 0; i < 3; i++) { blk[0] = (uint8_t)i; jb.push(blk.data(), blk.size(), 0); }
  assert(jb.front() == nullptr); // 3 < prefill 4: not yet
  blk[0] = 3; jb.push(blk.data(), blk.size(), 0);
  for (int i = 0; i < 4; i++) { const uint8_t *f = jb.front(); assert(f && f[0] == i); jb.pop(); }
  assert(jb.front() == nullptr && jb.stats().underruns == 1); // ran dry: counted once, re-prefills
  // a gap becomes silence blocks, then the audio
  for (int i = 0; i < 4; i++) jb.push(blk.data(), blk.size(), 0);
  for (int i = 0; i < 4; i++) { assert(jb.front()); jb.pop(); }
  blk[0] = 9; jb.push(blk.data(), blk.size(), 2);
  assert(jb.stats().lost == 2 && jb.level() == 3);
  // overrun drops the newest
  jb.reset();
  for (int i = 0; i < 10; i++) jb.push(blk.data(), blk.size(), 0);
  assert(jb.level() == 8 && jb.stats().overruns == 2);

  // allocation stops at the reserve
  budget = 3 * fps::kPayloadBytes + 100000;
  size_t count = 0;
  struct S { static size_t f() { return budget - 0; } };
  JitterBuffer small(20, 0.5f, +[]() -> size_t { static size_t left = 5 * fps::kPayloadBytes; size_t r = left; left = left >= fps::kPayloadBytes ? left - fps::kPayloadBytes : 0; return r; }, 0);
  count = small.capacity();
  assert(count == 5);
  printf("jitter/parser checks passed\n");
}
