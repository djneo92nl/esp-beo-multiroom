// ESP32 platform layer: servers, mDNS, peers. All hardware and ESP-IDF calls live here;
// the core (src/core) sees only the interfaces in src/platform.
#include "app.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_random.h>
#include <garnet_web.h>
#include <garnet_settings.h>
#include <mbedtls/sha256.h>
#include <mdns.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "../core/mozart_api.h"
#include "globals.h"
#include "link.h"

Identity gIdentity;
std::atomic<int> gWsClients{0};
std::atomic<int> gPeerCount{0};
std::atomic<bool> gPeerRefreshNow{false};

namespace {

constexpr const char *kTag = "mozart";
constexpr int kMozartPort = 80;
constexpr int kPingPort = 8080; // the app's liveness ping, and where peers read the legacy surface
constexpr int kWsPort = 9339;

// ---- identity ------------------------------------------------------------------------

std::string hex12(const uint8_t *mac) {
  char b[13];
  snprintf(b, sizeof b, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return b;
}

// beo: "99" + "%06d" % (int(sha256(seed)[:12 hex], 16) % 1_000_000). Same machine, same serial.
std::string deriveSerial(const std::string &seed) {
  uint8_t h[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t *>(seed.data()), seed.size(), h, 0);
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v = (v << 8) | h[i]; // first 12 hex digits
  char b[16];
  snprintf(b, sizeof b, "99%06u", (unsigned)(v % 1000000ULL));
  return b;
}

std::string hostFor(const std::string &friendlyOrPrefix, const std::string &serial) {
  // Letters, digits and hyphens only, lower case (garnet_web's hostname rule).
  std::string h;
  for (char c : friendlyOrPrefix) {
    c = (char)tolower((unsigned char)c);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) h += c;
    else if (!h.empty() && h.back() != '-') h += '-';
  }
  while (!h.empty() && h.back() == '-') h.pop_back();
  if (h.empty()) h = "beolab-8";
  if (h.size() > 22) h.resize(22);
  return h + "-" + serial; // <= 31 characters
}

// ---- the core, behind one lock ---------------------------------------------------------

class EspNet : public Net {
public:
  std::vector<Peer> peers() override {
    std::lock_guard<std::mutex> g(lock_);
    return peers_;
  }
  std::string own_ip() override { return WiFi.localIP().toString().c_str(); }
  std::string new_guid() override {
    uint8_t r[16];
    esp_fill_random(r, sizeof r);
    r[6] = (r[6] & 0x0F) | 0x40;
    r[8] = (r[8] & 0x3F) | 0x80;
    char b[37];
    snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3],
             r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    return b;
  }

  // One mDNS pass over the product-to-product and app-facing service types. Blocking
  // (about a second), so it runs on its own task and never on a server task.
  void refresh() {
    std::vector<Peer> found;
    const std::string own = gIdentity.jid();
    query("_beocore", found, own);
    query("_bangolufsen", found, own);
    // One mDNS pass often misses someone who is there (replies are multicast and can be lost), and
    // the app's join names a product we heard a minute ago. So keep what was seen for 20 minutes.
    uint32_t now = millis();
    std::lock_guard<std::mutex> g(lock_);
    for (Peer &p : found) {
      bool known = false;
      for (Seen &k : seen_)
        if (k.peer.jid == p.jid) {
          std::string keep = k.peer.ip;
          k.peer = p;
          if (k.peer.ip.empty()) k.peer.ip = keep; // a pass that missed the A record must not erase a good address
          k.at = now;
          known = true;
        }
      if (!known) {
        seen_.push_back({p, now});
        log_i("mozart: peer heard: %s at %s", p.jid.substr(0, p.jid.find('@')).c_str(), p.ip.c_str());
      }
    }
    for (size_t i = 0; i < seen_.size();)
      if (now - seen_[i].at > 20u * 60u * 1000u) {
        log_i("mozart: peer forgotten (not heard for 20 minutes): %s", seen_[i].peer.jid.c_str());
        seen_.erase(seen_.begin() + i);
      }
      else i++;
    peers_.clear();
    for (Seen &k : seen_) peers_.push_back(k.peer);
    gPeerCount = (int)peers_.size();
  }

private:
  static std::string txt(const mdns_result_t *r, const char *key) {
    for (size_t i = 0; i < r->txt_count; i++)
      if (strcmp(r->txt[i].key, key) == 0 && r->txt[i].value) return r->txt[i].value;
    return "";
  }
  void query(const char *type, std::vector<Peer> &out, const std::string &own) {
    mdns_result_t *res = nullptr;
    if (mdns_query_ptr(type, "_tcp", 1500, 20, &res) != ESP_OK || !res) return;
    for (mdns_result_t *r = res; r; r = r->next) {
      std::string jid = txt(r, "jid");
      if (jid.empty() && !txt(r, "tn").empty() && !txt(r, "in").empty() && !txt(r, "sn").empty())
        jid = txt(r, "tn") + "." + txt(r, "in") + "." + txt(r, "sn") + "@products.bang-olufsen.com";
      if (jid.empty() || jid == own) continue;
      bool dup = false;
      for (const Peer &p : out) dup |= p.jid == jid;
      if (dup) continue;
      std::string ip;
      for (mdns_ip_addr_t *a = r->addr; a && ip.empty(); a = a->next)
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
          char b[16];
          snprintf(b, sizeof b, IPSTR, IP2STR(&a->addr.u_addr.ip4));
          ip = b;
        }
      out.push_back({jid, r->instance_name ? r->instance_name : "", ip});
    }
    mdns_query_results_free(res);
  }
  struct Seen {
    Peer peer;
    uint32_t at;
  };
  std::mutex lock_;
  std::vector<Peer> peers_;
  std::vector<Seen> seen_;
};

// ---- WebSocket hub: ws://<ip>:9339 -----------------------------------------------------

class WsHub : public EventSink {
public:
  httpd_handle_t server = nullptr;
  std::mutex lock;
  std::vector<int> fds;
  std::vector<std::pair<std::string, std::string>> (*snapshot)() = nullptr;

  void add(int fd) {
    std::lock_guard<std::mutex> g(lock);
    for (int f : fds) if (f == fd) return;
    fds.push_back(fd);
    gWsClients = (int)fds.size();
  }
  void remove(int fd) {
    std::lock_guard<std::mutex> g(lock);
    for (size_t i = 0; i < fds.size(); i++)
      if (fds[i] == fd) { fds.erase(fds.begin() + i); break; }
    gWsClients = (int)fds.size();
  }

  static std::string envelope(const std::string &type, const std::string &data) {
    return "{\"eventType\":\"" + type + "\",\"eventData\":" + data + "}";
  }

  void send(const std::string &type, const std::string &data) override {
    if (!server) return;
    std::string msg = envelope(type, data);
    std::vector<int> targets;
    {
      std::lock_guard<std::mutex> g(lock);
      targets = fds;
    }
    for (int fd : targets) {
      httpd_ws_frame_t f = {};
      f.type = HTTPD_WS_TYPE_TEXT;
      f.payload = (uint8_t *)msg.data();
      f.len = msg.size();
      if (httpd_ws_send_frame_async(server, fd, &f) != ESP_OK) remove(fd);
    }
    log_i("%s: WS -> %s to %u client(s)", kTag, type.c_str(), (unsigned)targets.size());
  }
};

class Port : public StatePort {
public:
  void volume_changed(int level, int maximum, bool muted) override {
    log_i("%s: volume %d/%d%s", kTag, level, maximum, muted ? " (muted)" : "");
    linkSetVolume(level, maximum, muted);
  }
  void play_state_changed(const std::string &s) override { log_i("%s: playback %s", kTag, s.c_str()); }
  void default_volume_changed(int level) override { gsSetInt("audio", "defvol", level); }
};

WsHub hub;
EspNet net;
Port port;
PlayerState *state = nullptr;
Mozart *mozart = nullptr;
std::recursive_mutex coreLock; // httpd runs one task per server; the core is single-threaded

httpd_handle_t srv80 = nullptr, srv8080 = nullptr;
bool started = false, serversUp = false, mdnsUp = false;

const char *statusLine(int code) {
  switch (code) {
  case 200: return "200 OK";
  case 201: return "201 Created";
  case 202: return "202 Accepted";
  case 204: return "204 No Content";
  case 400: return "400 Bad Request";
  default: return "404 Not Found";
  }
}

esp_err_t mozartHandler(httpd_req_t *req) {
  const char *method = "GET";
  switch (req->method) {
  case HTTP_GET: method = "GET"; break;
  case HTTP_HEAD: method = "HEAD"; break;
  case HTTP_POST: method = "POST"; break;
  case HTTP_PUT: method = "PUT"; break;
  case HTTP_PATCH: method = "PATCH"; break;
  case HTTP_DELETE: method = "DELETE"; break;
  default: break;
  }
  HttpRequest r;
  r.method = method;
  r.path = req->uri;
  size_t q = r.path.find('?');
  if (q != std::string::npos) r.path.resize(q);
  if (req->content_len > 0) {
    size_t n = req->content_len < 2048 ? req->content_len : 2048, got = 0;
    r.body.resize(n);
    while (got < n) {
      int k = httpd_req_recv(req, &r.body[got], n - got);
      if (k <= 0) return ESP_FAIL;
      got += k;
    }
    // Anything beyond 2 KB is not an app request; drain so the connection stays usable.
    size_t left = req->content_len - n;
    char sink[64];
    while (left > 0) {
      int k = httpd_req_recv(req, sink, left < sizeof sink ? left : sizeof sink);
      if (k <= 0) break;
      left -= k;
    }
  }
  HttpResponse resp;
  {
    std::lock_guard<std::recursive_mutex> g(coreLock);
    resp = mozart->handle(r);
  }
  log_i("%s: %s %s -> %d%s", kTag, method, r.path.c_str(), resp.code, resp.code == 404 ? "  (not modelled)" : "");
  httpd_resp_set_status(req, statusLine(resp.code));
  httpd_resp_set_type(req, "application/json");
  if (resp.code == 204) return httpd_resp_send(req, nullptr, 0);
  return httpd_resp_send(req, resp.body.c_str(), resp.body.size());
}

httpd_handle_t startRest(int port) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
  cfg.ctrl_port = port + 32768;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.stack_size = 4096;
  cfg.max_open_sockets = 4;
  cfg.max_uri_handlers = 8;
  cfg.lru_purge_enable = true;
  httpd_handle_t h = nullptr;
  if (httpd_start(&h, &cfg) != ESP_OK) {
    log_e("%s: could not start the REST server on port %d (is garnet_web still on it?)", kTag, port);
    return nullptr;
  }
  for (httpd_method_t m : {HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT, HTTP_PATCH, HTTP_DELETE}) {
    httpd_uri_t u = {};
    u.uri = "/*";
    u.method = m;
    u.handler = mozartHandler;
    httpd_register_uri_handler(h, &u);
  }
  return h;
}

// IDF does not call the URI handler for the handshake itself, so the snapshot a new client
// must get (the app fetches nothing on connect) goes out from the post-handshake callback.
esp_err_t wsHandshaken(httpd_req_t *req) {
  int fd = httpd_req_to_sockfd(req);
  hub.add(fd);
  std::vector<std::pair<std::string, std::string>> snap;
  {
    std::lock_guard<std::recursive_mutex> g(coreLock);
    snap = state->snapshot();
  }
  for (auto &e : snap) {
    std::string msg = WsHub::envelope(e.first, e.second);
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    f.payload = (uint8_t *)msg.data();
    f.len = msg.size();
    if (httpd_ws_send_frame(req, &f) != ESP_OK) break;
  }
  log_i("%s: WS client connected (fd %d), snapshot of %u events", kTag, fd, (unsigned)snap.size());
  return ESP_OK;
}

// Frames from the app: read and ignore (ping/pong/close are answered by the server).
esp_err_t wsHandler(httpd_req_t *req) {
  httpd_ws_frame_t f = {};
  esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
  if (err != ESP_OK) return err;
  if (f.len > 0 && f.len < 512) {
    std::vector<uint8_t> buf(f.len + 1);
    f.payload = buf.data();
    err = httpd_ws_recv_frame(req, &f, f.len);
  }
  return err;
}

void wsClosed(httpd_handle_t, int fd) {
  hub.remove(fd);
  close(fd);
}

httpd_handle_t startWs() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = kWsPort;
  cfg.ctrl_port = kWsPort + 32768;
  cfg.stack_size = 4096;
  cfg.max_open_sockets = 3;
  cfg.lru_purge_enable = true;
  cfg.close_fn = wsClosed;
  httpd_handle_t h = nullptr;
  if (httpd_start(&h, &cfg) != ESP_OK) {
    log_e("%s: could not start the WebSocket server", kTag);
    return nullptr;
  }
  httpd_uri_t u = {};
  u.uri = "/";
  u.method = HTTP_GET;
  u.handler = wsHandler;
  u.is_websocket = true;
  u.ws_post_handshake_cb = wsHandshaken;
  httpd_register_uri_handler(h, &u);
  return h;
}

void peerTask(void *) {
  for (;;) {
    // Not while a stream runs: a query's results and sockets cost heap the stream needs.
    if (mdnsUp && !gLinkStats.listening) net.refresh();
    // A join that named an unknown product asks for another pass at once, so the app's retry finds it.
    for (int i = 0; i < 20 && !gPeerRefreshNow.load(); i++) vTaskDelay(pdMS_TO_TICKS(500));
    gPeerRefreshNow = false;
  }
}

// ---- mDNS: the Mozart record the app browses for ---------------------------------------

bool addMdns() {
  const Identity &id = gIdentity;
  mdns_txt_item_t txt[] = {
      {"tn", id.type_no.c_str()}, {"in", id.item_no.c_str()}, {"sn", id.serial.c_str()},
      {"fn", id.friendly.c_str()}, {"fv", id.firmware.c_str()}, {"wa", id.mac.c_str()},
  };
  esp_err_t e = mdns_service_add(id.friendly.c_str(), "_bangolufsen", "_tcp", kMozartPort, txt, 6);
  if (e == ESP_ERR_INVALID_STATE) return false; // garnet_web has not started the responder yet
  auto report = [&](const char *what, esp_err_t r) {
    if (r == ESP_OK) log_i("%s: mDNS %s", kTag, what);
    else log_e("%s: mDNS %s FAILED: %s", kTag, what, esp_err_to_name(r));
  };
  report("_bangolufsen._tcp (the app)", e);

  // The product-to-product records (beo discovery.Advertiser): a source finds listeners by these.
  std::string jid = id.jid();
  std::string ptype = gsGetString("product", "ptype").c_str();
  mdns_txt_item_t core[] = {{"hash", "sha-1"}, {"jid", jid.c_str()}, {"name", id.friendly.c_str()},
                            {"node", "beocore"}, {"txtvers", "1"}, {"ver", "dSsQBJenapvOryzvtU90IM7SI+o="}};
  report("_beocore._tcp (NetworkLink)", mdns_service_add(jid.c_str(), "_beocore", "_tcp", 5222, core, 6));
  mdns_txt_item_t remote[] = {{"assoc", ""}, {"item", id.item_no.c_str()}, {"jid", jid.c_str()}, {"mac", id.mac.c_str()},
                              {"name", id.friendly.c_str()}, {"productType", ptype.c_str()},
                              {"services", "BeoInput,BeoNotify,BeoZone,BeoDevice,BeoContent,BeoHome,BeoSecurity"},
                              {"type", id.type_no.c_str()}};
  report("_beoremote._tcp", mdns_service_add(id.friendly.c_str(), "_beoremote", "_tcp", kPingPort, remote, 8));
  report("_beo_settings._tcp", mdns_service_add(id.friendly.c_str(), "_beo_settings", "_tcp", kMozartPort, nullptr, 0));
  return true;
}

} // namespace

// ---- public --------------------------------------------------------------------------

void appInit() {
  // A weak link right beside the access point can be a brown-out (power) or an antenna problem: say how we started.
  log_i("boot: reset reason %d (1 power-on, 4 panic/wdt, 15 brownout)", (int)esp_reset_reason());
  // Say when the radio link drops and why: an intermittent network was the first thing to
  // look like a stream problem.
  WiFi.onEvent([](arduino_event_id_t ev, arduino_event_info_t info) {
    if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      log_w("wifi: disconnected, reason %d, rssi was %d", info.wifi_sta_disconnected.reason, (int)info.wifi_sta_disconnected.rssi);
    else if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED) log_i("wifi: associated, channel %d", info.wifi_sta_connected.channel);
    else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) log_i("wifi: got ip %s", WiFi.localIP().toString().c_str());
    else if (ev == ARDUINO_EVENT_WIFI_STA_LOST_IP) log_w("wifi: lost ip");
  });
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char m[18];
  snprintf(m, sizeof m, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  gIdentity.mac = m;
  gIdentity.serial = deriveSerial(hex12(mac));
  // appRegisterSettings() has run, so the group is known and a never-written namespace is handled.
  gIdentity.claim_proprietary = gsGetBool("product", "claimprop");
  String n = gsGetString("product", "name");
  if (n.length()) gIdentity.friendly = n.c_str();
  // garnet_web names the host "<name>-<mac>" unless system/hostname is set. The B&O records
  // want one host name shaped "<model>-<serial>" on every record, so seed it once.
  Preferences sys;
  if (sys.begin("system", false)) {
    if (sys.getString("hostname", "").length() == 0)
      sys.putString("hostname", hostFor(gIdentity.product_default_name, gIdentity.serial).c_str());
    sys.end();
  }
}

void appLoop() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  GwNetMode mode = gwNetMode();
  bool up = mode == GwNetMode::WiFi || mode == GwNetMode::Ethernet;
  if (!up) return;
  WiFi.setSleep(false); // modem sleep bunches packets at the beacon interval; cheap to re-assert

  if (!started) {
    started = true;
    state = new PlayerState(gIdentity, hub, port);
    state->set_maximum(100);
    state->set_default_volume(gsGetInt("audio", "defvol"));
    state->set_volume(gsGetInt("audio", "defvol"), 0);
    mozart = new Mozart(gIdentity, *state, net, nullptr);
    srv80 = startRest(kMozartPort);
    srv8080 = startRest(kPingPort);
    hub.server = startWs();
    serversUp = srv80 && srv8080 && hub.server;
    log_i("%s: Mozart API on ports %d and %d, notifications on %d", kTag, kMozartPort, kPingPort, kWsPort);
    xTaskCreate(peerTask, "peers", 4096, nullptr, 1, nullptr);
    linkSetVolume(state->volume().level, state->volume().maximum, state->volume().muted);
    mozart->set_link(&linkPort());
    linkStart(state, &coreLock, &net);
    if (gsGetBool("product", "enrol")) enrolStart(gIdentity);
  }
  if (serversUp && !mdnsUp) mdnsUp = addMdns();
}
