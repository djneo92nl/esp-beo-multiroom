// The product's state as the app sees it. No platform headers.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "identity.h"
#include "../platform/net.h"
#include "../platform/state.h"

// Sends {"eventType":..., "eventData":...} to every WebSocket client.
class EventSink {
public:
  virtual ~EventSink() {}
  virtual void send(const std::string &type, const std::string &data_json) = 0;
};

struct Volume {
  int level = 30;
  int maximum = 100;   // the app scales a group against this; no "range" field
  int def = 30;
  bool muted = false;
};

class PlayerState {
public:
  PlayerState(const Identity &id, EventSink &events, StatePort &port) : id_(id), events_(events), port_(port) {}

  const Volume &volume() const { return vol_; }
  const std::string &power() const { return power_; }
  const std::string &play() const { return play_; }
  const std::string &leader() const { return leader_; }

  // level/muted: -1 = leave alone. Emits only on a real change, like beo's set_volume.
  void set_volume(int level, int muted);
  void set_default_volume(int level);
  void set_maximum(int level);
  void set_power(const std::string &value);   // on | standby
  void set_play(const std::string &state);    // idle | started | paused | stopped

  // The listener side reports back. A failed join is silence (the app times out itself).
  void join_result(const std::string &leader_jid, const std::string &request_id, bool ok);
  void left();
  // A source started streaming to us without the app asking (it expanded to us): note who.
  void following(const std::string &leader_jid);

  std::string volume_json() const;
  std::string source_json() const;
  std::string metadata_json() const;
  std::string progress_json() const;
  std::string playback_state_json() const;
  std::string power_json() const { return "{\"value\":" + std::string("\"") + power_ + "\"}"; }

  // Every state once, in the order a product sends them, for a new WebSocket client.
  std::vector<std::pair<std::string, std::string>> snapshot() const;

private:
  void notify_listeners_changed();
  Identity id_;
  EventSink &events_;
  StatePort &port_;
  Volume vol_;
  std::string power_ = "on";
  std::string play_ = "idle";
  std::string leader_;
};
