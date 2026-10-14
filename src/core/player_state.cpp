#include "player_state.h"

#include "json.h"

namespace {
const char *kSourceOff =
    "{\"id\":\"\",\"name\":\"\",\"type\":{\"value\":\"unknown\"},\"isEnabled\":true,\"isPlayable\":true,"
    "\"isMultiroomAvailable\":true,\"isSeekable\":false}";
}

void PlayerState::set_volume(int level, int muted) {
  bool changed = false;
  if (level >= 0) {
    if (level > vol_.maximum) level = vol_.maximum;
    if (level != vol_.level) { vol_.level = level; changed = true; }
  }
  if (muted >= 0 && (muted != 0) != vol_.muted) { vol_.muted = muted != 0; changed = true; }
  if (!changed) return;
  events_.send("WebSocketEventVolume", volume_json());
  port_.volume_changed(vol_.level, vol_.maximum, vol_.muted);
}

void PlayerState::set_default_volume(int level) {
  if (level < 0) level = 0;
  if (level > vol_.maximum) level = vol_.maximum;
  vol_.def = level;
  port_.default_volume_changed(level);
}

void PlayerState::set_maximum(int level) {
  if (level < 1) return;
  vol_.maximum = level;
  if (vol_.level > level) vol_.level = level;
  if (vol_.def > level) vol_.def = level;
}

void PlayerState::set_power(const std::string &value) {
  if (power_ == value) return;
  power_ = value;
  events_.send("WebSocketEventPowerState", power_json());
  // Power decides whether there is a source to show, so the source follows at once.
  events_.send("WebSocketEventSourceChange", source_json());
  events_.send("WebSocketEventPlaybackSource", source_json());
}

void PlayerState::set_play(const std::string &state) {
  if (play_ == state) return;
  play_ = state;
  events_.send("WebSocketEventPlaybackState", "{\"value\":" + json::str(play_) + "}");
  port_.play_state_changed(play_);
}

void PlayerState::join_result(const std::string &leader_jid, const std::string &request_id, bool ok) {
  if (!ok) return; // silence: the app's own timer reports the failure
  leader_ = leader_jid;
  events_.send("WebSocketEventBeolinkJoinResult",
               "{\"jid\":" + json::str(leader_jid) + ",\"status\":\"joined\",\"type\":\"join\","
               "\"error\":\"ok\",\"requestID\":" + json::str(request_id) + "}");
}

void PlayerState::left() {
  leader_.clear();
  notify_listeners_changed();
}

void PlayerState::following(const std::string &leader_jid) {
  leader_ = leader_jid;
  set_play("started");
}

void PlayerState::notify_listeners_changed() {
  events_.send("WebSocketEventNotification", "{\"value\":\"beolinkListeners\",\"code\":\"\"}");
}

std::string PlayerState::volume_json() const {
  char b[200];
  snprintf(b, sizeof b, "{\"level\":{\"level\":%d},\"maximum\":{\"level\":%d},\"default\":{\"level\":%d},\"muted\":{\"muted\":%s}}",
           vol_.level, vol_.maximum, vol_.def, json::boolean(vol_.muted));
  return b;
}

std::string PlayerState::source_json() const {
  if (power_ != "on") return kSourceOff;
  return "{\"id\":\"lineIn\",\"name\":\"Line-In\",\"type\":{\"value\":\"lineIn\"},\"isEnabled\":true,"
         "\"isPlayable\":true,\"isMultiroomAvailable\":true,\"isSeekable\":false}";
}

// The plain-source form of beo's metadata: the same fields with nothing in them.
std::string PlayerState::metadata_json() const {
  return "{\"albumName\":\"\",\"artistName\":\"\",\"title\":\"Line-In\",\"track\":1,\"trackCount\":0,"
         "\"totalDuration\":240,\"totalDurationSeconds\":240,\"art\":[],\"bitdepth\":16,\"bitrate\":1411,"
         "\"samplerate\":44100,\"containerName\":\"\",\"encoding\":\"pcm\",\"genre\":\"\",\"id\":0,"
         "\"inputChannels\":\"2\",\"outputChannels\":\"2\",\"isExplicit\":false,\"organization\":\"\","
         "\"queueId\":\"00000000-0000-0000-0000-000000000000\","
         "\"remoteLeader\":{\"audioTransport\":\"\",\"friendlyName\":\"\",\"jid\":" + json::str(leader_) + "},"
         "\"remoteSource\":\"\",\"source\":\"lineIn\",\"sourceInternalId\":\"linein\",\"tags\":[],\"uri\":\"\"}";
}

std::string PlayerState::progress_json() const {
  return "{\"id\":\"1\",\"progress\":0,\"totalDuration\":240}";
}

std::string PlayerState::playback_state_json() const {
  return "{\"state\":{\"value\":" + json::str(play_) + "},\"source\":" + source_json() +
         ",\"progress\":" + progress_json() + ",\"metadata\":" + metadata_json() + "}";
}

std::vector<std::pair<std::string, std::string>> PlayerState::snapshot() const {
  std::vector<std::pair<std::string, std::string>> e;
  e.emplace_back("WebSocketEventPowerState", power_json());
  e.emplace_back("WebSocketEventVolume", volume_json());
  e.emplace_back("WebSocketEventSourceChange", source_json());
  e.emplace_back("WebSocketEventPlaybackSource", source_json());
  e.emplace_back("WebSocketEventPlaybackState", "{\"value\":" + json::str(play_) + "}");
  e.emplace_back("WebSocketEventPlaybackMetadata", metadata_json());
  e.emplace_back("WebSocketEventPlaybackProgress", progress_json());
  return e;
}
