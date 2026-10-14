// A ring of fixed-size PCM blocks between the network task and the audio task.
// Single producer, single consumer, lock-free (indices are atomics). No platform headers.
//
// Blocks, not one big allocation: on a PSRAM-less ESP32 the largest free heap block is far
// smaller than the free total (findings.md section 4.3).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#include "fps.h"

class JitterBuffer {
public:
  struct Stats {
    uint32_t received = 0, lost = 0, overruns = 0, underruns = 0, dropped_markers = 0;
    uint32_t restarts = 0;
  };

  // `capacity` blocks of fps::kPayloadBytes, each allocated on its own (the largest contiguous
  // heap block is much smaller than the free total, and shrinks as the heap fragments). If
  // `free_heap` is given, allocation stops while fewer than `reserve` bytes would remain, so
  // the buffer never starves the rest of the system. Playback starts once `prefill_ratio`
  // of the blocks are queued.
  JitterBuffer(size_t capacity, float prefill_ratio, size_t (*free_heap)() = nullptr, size_t reserve = 0) {
    for (size_t i = 0; i < capacity; i++) {
      if (free_heap && free_heap() < fps::kPayloadBytes + reserve) break;
      uint8_t *b = new (std::nothrow) uint8_t[fps::kPayloadBytes];
      if (!b) break;
      blocks_.push_back(b);
    }
    cap_ = blocks_.size();
    prefill_ = (size_t)((float)cap_ * prefill_ratio);
    if (prefill_ >= cap_ && cap_ > 0) prefill_ = cap_ - 1;
  }
  ~JitterBuffer() {
    for (uint8_t *b : blocks_) delete[] b;
  }
  JitterBuffer(const JitterBuffer &) = delete;
  JitterBuffer &operator=(const JitterBuffer &) = delete;

  size_t capacity() const { return cap_; }
  size_t level() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }

  // Producer. `gap` = datagrams missing before this one; they are replaced by silence blocks
  // (up to a few) so the stream keeps its length and does not shorten on every loss.
  void push(const uint8_t *pcm, size_t bytes, uint16_t gap) {
    if (cap_ == 0) return;
    stats_.received++;
    stats_.lost += gap;
    for (uint16_t i = 0; i < gap && i < 4; i++) store(nullptr, 0);
    store(pcm, bytes);
  }

  // Consumer: pointer to the next block, or nullptr (not yet prefilled, or empty -> underrun).
  const uint8_t *front() {
    if (cap_ == 0) return nullptr;
    size_t lvl = level();
    if (!playing_) {
      if (lvl < prefill_) return nullptr;
      playing_ = true;
    }
    if (lvl == 0) {
      playing_ = false; // run dry: refill to the prefill mark before resuming
      stats_.underruns++;
      return nullptr;
    }
    return blocks_[r_.load(std::memory_order_relaxed) % cap_];
  }
  void pop() { r_.fetch_add(1, std::memory_order_release); }

  // A new stream: empty, not playing, counters back to zero. Call with neither side running.
  void reset() {
    r_.store(w_.load());
    playing_ = false;
    stats_ = Stats();
  }
  bool playing() const { return playing_; }
  const Stats &stats() const { return stats_; }

private:
  void store(const uint8_t *pcm, size_t bytes) {
    size_t w = w_.load(std::memory_order_relaxed);
    if (w - r_.load(std::memory_order_acquire) >= cap_) {
      // Full: drop the newest rather than race the consumer for the oldest.
      stats_.overruns++;
      return;
    }
    uint8_t *dst = blocks_[w % cap_];
    size_t n = bytes < fps::kPayloadBytes ? bytes : fps::kPayloadBytes;
    if (pcm) memcpy(dst, pcm, n);
    else n = 0;
    if (n < fps::kPayloadBytes) memset(dst + n, 0, fps::kPayloadBytes - n);
    w_.store(w + 1, std::memory_order_release);
  }

  size_t cap_ = 0, prefill_ = 0;
  std::vector<uint8_t *> blocks_;
  std::atomic<size_t> w_{0}, r_{0};
  bool playing_ = false;
  Stats stats_;
};
