// Platform interface 3 of 3: what the player state tells the hardware, and the
// network side (NetworkLink) that acts on app commands. Plain C++.
#pragma once
#include <string>

class StatePort {
public:
  virtual ~StatePort() {}
  virtual void volume_changed(int level, int maximum, bool muted) = 0;
  virtual void play_state_changed(const std::string &state) = 0; // idle|started|paused|stopped
  virtual void default_volume_changed(int level) = 0;            // persist
};

// What the app asks of the listener side. Results come back through
// PlayerState::join_result / left().
class LinkPort {
public:
  virtual ~LinkPort() {}
  // Start joining the product with this jid. false = nobody by that jid is known.
  virtual bool join(const std::string &jid, const std::string &request_id) = 0;
  virtual void leave() = 0;
};
