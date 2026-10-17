// Unit checks for the FPS parser and the jitter buffer (no platform headers).
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "../../src/core/audio_control.h"
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
  // drift policy: acts on the smoothed fill, never on a burst, and not more often than min_gap
  audio::DriftPolicy dp;
  dp.target = 3; dp.band = 0.75f; dp.min_gap = 8;
  int acts = 0;
  for (int i = 0; i < 400; i++) acts += dp.decide(3) != 0;
  assert(acts == 0);                                      // on target: never
  acts = 0;
  for (int i = 0; i < 400; i++) acts += dp.decide(i % 2 ? 6 : 0) != 0; // a swing of +-3 around 3
  assert(acts == 0);                                      // bursts average out: no correction
  dp = audio::DriftPolicy(); dp.target = 3;
  int minus = 0, plus = 0;
  for (int i = 0; i < 400; i++) { int d = dp.decide(5); minus += d < 0; plus += d > 0; }
  assert(minus > 20 && plus == 0 && minus <= 400 / 8 + 1); // persistently high: shorten, rate limited
  dp = audio::DriftPolicy(); dp.target = 3;
  minus = plus = 0;
  for (int i = 0; i < 400; i++) { int d = dp.decide(1); minus += d < 0; plus += d > 0; }
  assert(plus > 20 && minus == 0);                        // persistently low: stretch

  // gain stepping
  assert(audio::step_gain(0.f, 1.f, 0.1f) > 0.099f && audio::step_gain(0.f, 1.f, 0.1f) < 0.101f);
  assert(audio::step_gain(0.95f, 1.f, 0.1f) == 1.f);
  assert(audio::step_gain(1.f, 0.f, 0.25f) == 0.75f);

  // block adjustment: lengths, and a ramp stays a ramp (no step at the edit)
  const size_t N = 1062;
  std::vector<int16_t> in(N * 2), out(N * 2 + 2);
  for (size_t i = 0; i < N; i++) { in[2 * i] = (int16_t)(i * 10); in[2 * i + 1] = (int16_t)(-(int)i * 10); }
  assert(audio::adjust_block(in.data(), N, 0, out.data()) == N);
  size_t longer = audio::adjust_block(in.data(), N, +1, out.data());
  assert(longer == N + 1);
  for (size_t f = 1; f < longer; f++) assert(abs(out[2 * f] - out[2 * (f - 1)]) <= 10); // smooth
  size_t shorter = audio::adjust_block(in.data(), N, -1, out.data());
  assert(shorter == N - 1);
  for (size_t f = 1; f < shorter; f++) assert(abs(out[2 * f] - out[2 * (f - 1)]) <= 15);
  assert(out[0] == in[0] && out[2 * (shorter - 1)] == in[2 * (N - 1)]); // ends unchanged

  // rate policy: proportional, deadband, clamped; and a simulated source 1.1 % fast settles near the target
  audio::RatePolicy rp;
  for (int i = 0; i < 200; i++) assert(rp.decide(4) == 0);        // on target: untouched
  rp = audio::RatePolicy(); rp.decide(4); for (int i = 0; i < 200; i++) assert(rp.decide(i % 2 ? 6 : 2) == 0); // burst swings average out
  rp = audio::RatePolicy(); int dl = 0; for (int i = 0; i < 300; i++) dl = rp.decide(7);
  assert(dl < 0 && dl >= -rp.max_delta);                          // high: shorter blocks, clamped
  rp = audio::RatePolicy(); for (int i = 0; i < 300; i++) dl = rp.decide(1);
  assert(dl > 0 && dl <= rp.max_delta);                           // low: longer blocks
  {
    audio::RatePolicy r2; double fill = 4.0; const double F = 1062, src = 1.011; // source blocks per played block
    int worst = 0;
    for (int i = 0; i < 6000; i++) {
      int d = r2.decide((size_t)(fill + 0.5));
      double played_per_block = (F + d) / F;   // output time per consumed block, in block times
      fill += src * played_per_block - 1.0;    // arrivals during it minus the one consumed
      if (fill < 0) fill = 0; if (fill > 9) fill = 9;
      if (i > 3000 && (int)(fill + 0.5) > worst) worst = (int)(fill + 0.5);
    }
    assert(fill > 2.0 && fill < 8.0 && worst < 9);                // no overrun after settling
  }
  // resample: lengths, ends kept, a ramp stays a ramp, silence stays silence
  for (int d : {-30, -7, 5, 30}) {
    std::vector<int16_t> o2((N + 40) * 2);
    audio::resample_block(in.data(), N, (size_t)((int)N + d), o2.data());
    size_t M = (size_t)((int)N + d);
    assert(o2[0] == in[0] && abs(o2[2 * (M - 1)] - in[2 * (N - 1)]) <= 10);
    for (size_t f = 1; f < M; f++) assert(abs(o2[2 * f] - o2[2 * (f - 1)]) <= 12);
  }
  { std::vector<int16_t> z(N * 2, 0), o3((N + 40) * 2, 1); audio::resample_block(z.data(), N, N - 11, o3.data());
    for (size_t i = 0; i < (N - 11) * 2; i++) assert(o3[i] == 0); }

  printf("jitter/parser/audio-control checks passed\n");
}
