// Drift correction and gain stepping for the audio task. No platform headers.
//
// The source's sample clock and our I2S clock differ by some tens of ppm, and Wi-Fi delivers
// in bursts. The buffer fill is kept near a target by occasionally making one block one frame
// longer or shorter (an interpolated frame is inserted or two neighbours merged), at most once
// every few blocks, so the pitch moves by at most a few hundred ppm and never audibly.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace audio {

struct DriftPolicy {
  float target = 3.f;      // blocks the buffer should hold
  float band = 0.75f;      // act when the average is more than this far from the target
  uint32_t min_gap = 8;    // blocks between corrections (about 120 ppm at most)
  float smoothing = 1.f / 64.f; // the average spans ~64 blocks, about 1.5 s
  uint32_t since = 0;
  float avg = -1.f;

  // Wi-Fi delivers in bursts, so the instantaneous fill swings by several blocks and says
  // nothing about clock drift. The average over ~1.5 s does: act on that.
  // +1 = buffer is low, stretch one block by a frame; -1 = high, shorten it; 0 = leave it.
  int decide(size_t level) {
    if (avg < 0.f) avg = (float)level;
    avg += ((float)level - avg) * smoothing;
    since++;
    if (since < min_gap) return 0;
    if (avg > target + band) { since = 0; return -1; }
    if (avg < target - band) { since = 0; return +1; }
    return 0;
  }
  void restart() { avg = -1.f; since = 0; }
};

// Move g towards target by at most max_step (volume and mute go through this, so a step in the
// slider is never a step in the signal).
inline float step_gain(float g, float target, float max_step) {
  float d = target - g;
  if (d > max_step) d = max_step;
  if (d < -max_step) d = -max_step;
  return g + d;
}

// Copy a block of interleaved S16 stereo, one frame longer (dir +1), shorter (-1) or unchanged.
// `out` needs room for frames + 1. Returns the number of frames written. The change is made in
// the middle of the block: a frame that is the average of its neighbours is inserted, or two
// neighbouring frames are replaced by their average.
inline size_t adjust_block(const int16_t *in, size_t frames, int dir, int16_t *out) {
  size_t mid = frames / 2;
  if (dir == 0 || frames < 4) {
    for (size_t i = 0; i < frames * 2; i++) out[i] = in[i];
    return frames;
  }
  size_t o = 0;
  for (size_t f = 0; f < frames; f++) {
    if (dir < 0 && f == mid) {
      out[2 * o] = (int16_t)(((int)in[2 * f] + in[2 * (f + 1)]) / 2);
      out[2 * o + 1] = (int16_t)(((int)in[2 * f + 1] + in[2 * (f + 1) + 1]) / 2);
      o++;
      f++; // frame mid+1 is merged in
      continue;
    }
    out[2 * o] = in[2 * f];
    out[2 * o + 1] = in[2 * f + 1];
    o++;
    if (dir > 0 && f == mid) {
      out[2 * o] = (int16_t)(((int)in[2 * f] + in[2 * (f + 1)]) / 2);
      out[2 * o + 1] = (int16_t)(((int)in[2 * f + 1] + in[2 * (f + 1) + 1]) / 2);
      o++;
    }
  }
  return o;
}

// The source (a file played through DLNA on an M3, for one) can run ahead of our clock by about a
// percent for minutes, which one frame every few blocks cannot absorb. This proportional policy
// resamples every block by a few frames, in proportion to how far the smoothed fill is from the
// target: a fill above target shortens the blocks (we consume faster), below it lengthens them.
// 6 frames per block of error and a limit of 30 frames per 1062 move the pitch by at most 2.8 %.
struct RatePolicy {
  float target = 4.f;         // blocks the buffer should hold
  float deadband = 0.5f;      // no change while the average is this close to the target
  float frames_per_block = 6.f; // extra or fewer frames per block, per block of fill error
  int max_delta = 30;
  float smoothing = 1.f / 32.f; // the average spans ~32 blocks, about 0.75 s
  float avg = -1.f;

  // Returns the number of frames to add to this block (negative: remove).
  int decide(size_t level) {
    if (avg < 0.f) avg = (float)level;
    avg += ((float)level - avg) * smoothing;
    float e = avg - target;
    if (std::fabs(e) < deadband) return 0;
    e -= (e > 0.f ? deadband : -deadband);
    int d = (int)std::lround(-e * frames_per_block);
    if (d > max_delta) d = max_delta;
    if (d < -max_delta) d = -max_delta;
    return d;
  }
  void restart() { avg = -1.f; }
};

// Linear-interpolating resample of one block of interleaved S16 stereo to out_frames frames. The first
// and last frame are kept, so consecutive blocks join without a step. `out` needs room for out_frames.
inline void resample_block(const int16_t *in, size_t frames, size_t out_frames, int16_t *out) {
  if (out_frames == frames || frames < 2 || out_frames < 2) {
    size_t n = out_frames < frames ? out_frames : frames;
    for (size_t i = 0; i < n * 2; i++) out[i] = in[i];
    for (size_t i = n; i < out_frames; i++) { out[2 * i] = in[2 * (frames - 1)]; out[2 * i + 1] = in[2 * (frames - 1) + 1]; }
    return;
  }
  const uint64_t step = (((uint64_t)(frames - 1)) << 16) / (uint64_t)(out_frames - 1);
  for (size_t i = 0; i < out_frames; i++) {
    uint64_t pos = step * i;
    size_t idx = (size_t)(pos >> 16);
    uint32_t frac = (uint32_t)(pos & 0xffff);
    if (idx >= frames - 1) { idx = frames - 2; frac = 0x10000; }
    for (int c = 0; c < 2; c++) {
      int a = in[2 * idx + c], b = in[2 * (idx + 1) + c];
      out[2 * i + c] = (int16_t)(a + (((int64_t)(b - a) * frac) >> 16));
    }
  }
}

} // namespace audio
