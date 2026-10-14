#include "mozart_api.h"

#include <cstdlib>
#include <cstring>

#include "json.h"

namespace {

constexpr const char *kRoot = "b0000000-0000-4000-8000-000000000001";
constexpr const char *kLineIn = "b0000000-0000-4000-8000-000000000003";

HttpResponse ok(std::string body, int code = 200) { return {code, std::move(body)}; }
HttpResponse not_found() { return {404, "{}"}; }
HttpResponse no_content() { return {204, ""}; }

bool starts(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

std::string url_decode(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else o += s[i];
  }
  return o;
}

} // namespace

std::string Mozart::product_info() const {
  const std::string &fw = id_.firmware;
  std::string v = json::str(fw);
  return "{\"apple\":{\"homeApp\":false},\"bootloader\":{\"version\":" + v + "},"
         "\"componentVersions\":{\"items\":[{\"component\":\"SW\",\"version\":" + v + "}]},"
         "\"encryptionInfo\":{\"algorithm\":\"\",\"keyLength\":0,\"publicKey\":\"\"},"
         "\"fep\":{\"version\":" + v + "},\"name\":{\"friendlyName\":" + json::str(id_.friendly) + "},"
         "\"pcbRevisions\":{\"items\":[]},\"powerDelivery\":{\"version\":\"1.0\"},"
         "\"production\":{\"hardwareRevision\":\"1\",\"itemNumber\":" + json::str(id_.item_no) + ","
         "\"macAddresses\":{\"macAddresses\":[{\"name\":\"ethernet\",\"mac\":" + json::str(id_.mac) + "},"
         "{\"name\":\"wlansta\",\"mac\":" + json::str(id_.mac) + "}]},"
         "\"productDefaultName\":" + json::str(id_.product_default_name) + ",\"productionSite\":\"emulator\","
         "\"productionTimestamp\":\"2024-01-01T00:00:00Z\",\"serialNumber\":" + json::str(id_.serial) + ","
         "\"typeNumber\":" + json::str(id_.type_no) + ",\"wifiCountryCode\":\"DK\"},"
         "\"touch\":{\"version\":" + v + "},"
         "\"tv\":{\"edid\":{\"name\":\"\",\"year\":0},\"lge\":{\"country\":\"\",\"countryGroup\":\"\","
         "\"deviceId\":\"\",\"firmwareVersion\":\"\",\"language\":\"\",\"screenSize\":0,\"webOsDialect\":\"\"}}}";
}

// Root labelled MUSIC, GUID ids, key == id. One child, Line-In. The one-child tree is
// our own (beo's plain speaker has the root only): see findings.md section 2.2.
std::string Mozart::remote_menu() const {
  return std::string("{\"") + kRoot + "\":{\"id\":\"" + kRoot + "\",\"label\":\"MUSIC\",\"available\":true,"
         "\"fixed\":true,\"disabled\":false,\"dynamicList\":\"none\",\"content\":{\"contentUri\":\"\","
         "\"label\":\"MUSIC\",\"categories\":[\"music\"],\"source\":{\"value\":\"lineIn\"}},"
         "\"firstChildMenuItemId\":\"" + kLineIn + "\"},\"" + kLineIn + "\":{\"id\":\"" + kLineIn + "\","
         "\"label\":\"Line-In\",\"available\":true,\"fixed\":false,\"disabled\":false,\"dynamicList\":\"none\","
         "\"parentMenuItemId\":\"" + kRoot + "\",\"content\":{\"contentUri\":\"lineIn\",\"label\":\"Line-In\","
         "\"categories\":[\"music\"],\"source\":{\"value\":\"lineIn\"}},"
         "\"actionList\":[{\"type\":\"triggerContent\",\"contentId\":\"lineIn\"}]}}";
}

std::string Mozart::playback_sources() const {
  return "{\"items\":[{\"id\":" + json::str("linein:" + id_.jid()) + ",\"name\":\"Line-In\","
         "\"type\":{\"value\":\"lineIn\"},\"isEnabled\":true,\"isPlayable\":true,"
         "\"isMultiroomAvailable\":true,\"isSeekable\":false}]}";
}

std::string Mozart::beodevice() const {
  return "{\"beoDevice\":{\"productId\":{\"productType\":" + json::str(id_.product_type) + ",\"typeNumber\":" +
         json::str(id_.type_no) + ",\"serialNumber\":" + json::str(id_.serial) + ",\"itemNumber\":" +
         json::str(id_.item_no) + "},\"productFamily\":\"mozart\",\"productFriendlyName\":{\"productFriendlyName\":" +
         json::str(id_.friendly) + "},\"software\":{\"version\":" + json::str(id_.firmware) + "},"
         "\"hardware\":{\"version\":\"S53\",\"type\":" + json::str(id_.type_no) + ",\"item\":" + json::str(id_.item_no) +
         ",\"serial\":" + json::str(id_.serial) + ",\"mac\":" + json::str(id_.mac) + "},"
         "\"profiles\":[{\"name\":\"lineInSettingsProfile\",\"version\":1}]}}";
}

HttpResponse Mozart::handle(const HttpRequest &req) {
  std::string path = req.path;
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  if (path.empty()) path = "/";
  if (req.method == "GET" || req.method == "HEAD") return get(path);
  return write(req, path);
}

HttpResponse Mozart::get(const std::string &p) {
  const Volume &v = state_.volume();
  if (p == "/api/v1/product/info") return ok(product_info());
  if (p == "/api/v1/remote") return ok(remote_menu());
  if (p == "/api/v1/playback/sources") return ok(playback_sources());
  if (p == "/api/v1/playback/sources/active") return ok(state_.source_json());
  if (p == "/api/v1/playback/state") return ok(state_.playback_state_json());
  if (p == "/api/v1/state") return ok("{\"power\":" + state_.power_json() + ",\"volume\":" + state_.volume_json() + "}");
  if (p == "/api/v1/state/power") return ok(state_.power_json());
  if (p == "/api/v1/sound/volume") return ok(state_.volume_json());
  if (p == "/api/v1/sound/volume/level") return ok("{\"level\":" + std::to_string(v.level) + "}");
  if (p == "/api/v1/sound/volume/mute") return ok(std::string("{\"muted\":") + json::boolean(v.muted) + "}");
  if (p == "/api/v1/sound/volume/settings")
    return ok("{\"default\":{\"level\":" + std::to_string(v.def) + "},\"maximum\":{\"level\":" + std::to_string(v.maximum) + "}}");
  if (p == "/api/v1/content-triggers") return ok("{\"items\":[]}");
  if (p == "/api/v1/playback/queue/settings")
    return ok("{\"consume\":false,\"gapless\":true,\"repeat\":\"none\",\"shuffle\":false}");
  // We are a listener: nobody is expanded *to* us, so no audioleader group of our own.
  if (p == "/api/v1/beolink/audioleader") return no_content();
  if (p == "/api/v1/beolink/enabled") return ok("{\"value\":true}");
  if (p == "/api/v1/beolink/self") return ok("{\"jid\":" + json::str(id_.jid()) + ",\"friendlyName\":" + json::str(id_.friendly) + "}");
  if (p == "/api/v1/beolink/listeners") return ok("[]");
  if (p == "/api/v1/beolink/available-listeners") {
    std::string o = "[";
    bool first = true;
    for (const Peer &peer : net_.peers()) {
      if (!first) o += ",";
      first = false;
      o += "{\"jid\":" + json::str(peer.jid) + "}"; // only jid: other fields did nothing
    }
    return ok(o + "]");
  }
  if (p == "/api/v1/network") return ok("{\"interfaces\":[{\"type\":\"wlan\",\"macAddress\":" + json::str(id_.mac) + "}]}");
  if (p == "/api/v1/softwareupdate") return ok("{\"state\":\"idle\",\"version\":" + json::str(id_.firmware) + "}");
  if (p == "/api/v1/setup/linein") return ok("{\"enabled\":true}");
  if (p == "/api/v1/hdmi") return ok("{\"items\":[]}");
  if (p == "/api/v1/scenes" || p == "/api/v1/alarms" || p == "/api/v1/timers" || p == "/api/v1/sound/features" ||
      p == "/api/v1/setup/presets")
    return ok("{}");
  if (p == "/api/v1/cast/tos-accepted") return ok("{\"accepted\":true}");
  if (p == "/api/v1/cast/version") return ok("{\"version\":\"1.56.500000\"}");
  if (p == "/api/v1/playback/queue") return ok("{\"items\":[]}");
  if (p == "/api/v1/proximity/settings") return ok("{\"disabled\":true}");
  if (p == "/api/v1/setup/enableusagereport") return ok("{\"enabled\":false}");
  if (p == "/api/v1/softwareupdate/settings") return ok("{\"automaticUpdate\":false}");
  if (p == "/api/v1/sound/settings/adjustments")
    return ok("{\"advancedBeamforming\":0,\"ambience\":0.0,\"bass\":0,\"directivity\":\"omni\",\"ecoMode\":false,"
              "\"fadein\":false,\"loudness\":false,\"treble\":0}");
  if (p == "/api/v1/sound/settings/toneTouch") return ok("{\"x\":0.0,\"y\":0.0,\"z\":0.0}");
  // A lone speaker: "standalone" stops the app offering to pair us (safer than 404).
  if (p == "/api/v1/speakerlink/role") return ok("{\"role\":\"none\",\"desired\":\"none\",\"channel\":\"all\",\"primary\":\"\"}");
  if (p == "/api/v1/speakerlink/association")
    return ok("{\"name\":" + json::str(id_.friendly) + ",\"type\":\"standalone\",\"supported\":[\"standalone\"],"
              "\"secondaries\":[],\"primary\":{\"name\":\"\",\"channel\":\"left\"},"
              "\"secondary\":{\"name\":\"\",\"channel\":\"left\",\"ipAddress\":\"\",\"serialNumber\":\"\"}}");
  // Hardware a speaker lacks answers 404, as a real product does.
  // Legacy surfaces (a few; the rest 404, see findings.md section 2.5).
  if (p == "/Ping") return ok("{}");
  if (p == "/BeoDevice") return ok(beodevice());
  if (p == "/BeoZone/System") return ok("{\"features\":[\"PRODUCTS\"]}");
  if (p == "/")
    return ok("{\"services\":[{\"name\":\"BeoNotify\",\"path\":\"BeoNotify/\"},{\"name\":\"BeoZone\",\"path\":\"BeoZone/\"},"
              "{\"name\":\"BeoDevice\",\"path\":\"BeoDevice/\"},{\"name\":\"BeoContent\",\"path\":\"BeoContent/\"},"
              "{\"name\":\"BeoHome\",\"path\":\"BeoHome/\"},{\"name\":\"BeoSecurity\",\"path\":\"BeoSecurity/\"},"
              "{\"name\":\"Ping\",\"path\":\"Ping\"}]}");
  // 404, never an empty 200: the app reads {} as an object with every field missing.
  return not_found();
}

HttpResponse Mozart::write(const HttpRequest &req, const std::string &p) {
  const std::string &body = req.body;

  if (starts(p, "/api/v1/sound/volume")) {
    int n;
    bool b;
    if (p.size() >= 6 && p.compare(p.size() - 6, 6, "/level") == 0) {
      if (json::find_int(body, "level", n)) state_.set_volume(n, -1);
      return ok(state_.volume_json(), 201); // 201 is what the app takes as done
    }
    if (p.size() >= 5 && p.compare(p.size() - 5, 5, "/mute") == 0) {
      if (json::find_bool(body, "muted", b)) state_.set_volume(-1, b ? 1 : 0);
      return ok(state_.volume_json(), 201);
    }
    if (p.size() >= 9 && p.compare(p.size() - 9, 9, "/settings") == 0) {
      size_t m = body.find("\"maximum\"");
      if (m != std::string::npos && json::find_int(body, "maximum", n, m) && n > 0) state_.set_maximum(n);
      size_t d = body.find("\"default\"");
      if (d != std::string::npos && json::find_int(body, "default", n, d)) state_.set_default_volume(n);
      return ok(state_.volume_json(), 201);
    }
    int lvl = -1, mu = -1;
    if (json::find_int(body, "level", n)) lvl = n;
    if (json::find_bool(body, "muted", b)) mu = b ? 1 : 0;
    state_.set_volume(lvl, mu);
    return ok(state_.volume_json(), 200);
  }

  // The app's power button; state/power {"value":"on"} wakes.
  if (p == "/api/v1/state/standby") {
    if (link_) link_->leave();
    state_.left();
    state_.set_power("standby");
    return ok("{}");
  }
  if (p == "/api/v1/state/power") {
    std::string want = "on";
    json::find_string(body, "value", want);
    if (want != "on") {
      if (link_) link_->leave();
      state_.left();
      state_.set_power("standby");
    } else state_.set_power("on");
    return ok("{}");
  }

  if (starts(p, "/api/v1/playback/command/")) {
    std::string c = p.substr(strlen("/api/v1/playback/command/"));
    if (c == "play") state_.set_play("started");
    else if (c == "pause") state_.set_play("paused");
    else if (c == "stop") state_.set_play("stopped");
    else if (c == "skip" || c == "prev") { /* line-in has nothing to skip */ }
    else return not_found(); // the app knows exactly five commands
    return ok("{}");
  }

  // The app tells *us* to join a source. 202 now, a join event later and only if true.
  if (starts(p, "/api/v1/beolink/join/")) {
    std::string jid = url_decode(p.substr(strlen("/api/v1/beolink/join/")));
    std::string rid = net_.new_guid();
    if (link_) link_->join(jid, rid);
    return ok("{\"jid\":" + json::str(jid) + ",\"requestID\":" + json::str(rid) +
                  ",\"result\":\"/api/v1/beolink/join/result/" + rid + "\"}",
              202);
  }
  if (p == "/api/v1/beolink/leave") {
    if (link_) link_->leave();
    state_.left();
    return ok("{}", 202);
  }
  // expand/unexpand are sent to the *source*, never to a listener (inferred): a 404 says
  // "Remote device not found", which the app knows.
  if (starts(p, "/api/v1/beolink/expand/") || starts(p, "/api/v1/beolink/unexpand/"))
    return ok("{\"error\":\"Remote device not found\"}", 404);

  if (starts(p, "/api/v1/remote/")) return ok("{}"); // the app renames menu items; accept

  return not_found();
}
