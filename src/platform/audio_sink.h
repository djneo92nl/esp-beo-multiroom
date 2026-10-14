// Platform interface 2 of 3: where PCM goes. Declared in Phase 1, used from Phase 2.
#pragma once
#include <cstddef>
#include <cstdint>

class AudioSink {
public:
  virtual ~AudioSink() {}
  // Interleaved S16LE stereo. Returns frames accepted (may block briefly).
  virtual size_t write(const int16_t *frames, size_t frame_count) = 0;
  virtual void set_rate(uint32_t hz) = 0;
  // How much audio the sink itself holds, in microseconds.
  virtual uint32_t buffered_us() = 0;
  // +1 = play slightly faster, -1 slower, 0 = nominal (drift correction, Phase 3).
  virtual void set_drift_correction(int ppm) = 0;
};
