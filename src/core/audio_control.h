// Drift correction and gain stepping for the audio task. No platform headers.
//
// The source's sample clock and our I2S clock differ by some tens of ppm, and Wi-Fi delivers
// in bursts. The buffer fill is kept near a target by occasionally making one block one frame
// longer or shorter (an interpolated frame is inserted or two neighbours merged), at most once
// every few blocks, so the pitch moves by at most a few hundred ppm and never audibly.
#pragma once
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

} // namespace audio
