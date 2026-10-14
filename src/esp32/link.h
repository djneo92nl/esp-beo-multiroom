// ESP32 side of NetworkLink: sockets, tasks, the jitter buffer and the I2S output.
#pragma once
#include <atomic>
#include <mutex>

#include "../core/player_state.h"
#include "../platform/net.h"
#include "../platform/state.h"

struct LinkStats {
  std::atomic<uint32_t> received{0}, lost{0}, underruns{0}, overruns{0}, markers{0}, bad{0}, corrections{0};
  std::atomic<int> fill_ms{0};
  std::atomic<int> rate{0};
  std::atomic<int> buffer_ms{0};
  std::atomic<bool> listening{false};
};
extern LinkStats gLinkStats;

// Starts the 5222 server, the audio task and (if enabled) the always-listen debug port.
void linkStart(PlayerState *state, std::recursive_mutex *lock, Net *net);
LinkPort &linkPort();
// The product's volume changed (called from any task).
void linkSetVolume(int level, int maximum, bool muted);
// Enrol in the network's shared configuration, retrying until it is known to be there.
void enrolStart(const Identity &id);
