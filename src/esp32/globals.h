#pragma once
#include <atomic>

#include "../core/identity.h"

// Info getters in garnet_settings are plain function pointers that run on the web
// server task, so live numbers are shared through these.
extern Identity gIdentity;
extern std::atomic<int> gWsClients;
extern std::atomic<int> gPeerCount;
extern std::atomic<bool> gPeerRefreshNow; // set when a join named a product we have not heard: look again now
