// Enrolment: put this product's entry into the configuration every B&O product on the network
// shares, so that a source will offer it as a listener. Opt-in (settings: Product > Enrol).
// Protocol from beo's networklink/configuration.py; see findings.md section 3.5.
#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <mdns.h>

#include <atomic>
#include <string>

#include "../core/enrol.h"
#include "link.h"

namespace {

constexpr const char *kTag = "enrol";

Identity gId;

struct Leader {
  std::string ip, jid;
};

bool findLeader(Leader &out) {
  mdns_result_t *res = nullptr;
  if (mdns_query_ptr("_beoconfig", "_tcp", 3000, 4, &res) != ESP_OK || !res) return false;
  bool ok = false;
  for (mdns_result_t *r = res; r && !ok; r = r->next) {
    for (size_t i = 0; i < r->txt_count; i++)
      if (strcmp(r->txt[i].key, "jid") == 0 && r->txt[i].value) out.jid = r->txt[i].value;
    for (mdns_ip_addr_t *a = r->addr; a && !ok; a = a->next)
      if (a->addr.type == ESP_IPADDR_TYPE_V4) {
        char b[16];
        snprintf(b, sizeof b, IPSTR, IP2STR(&a->addr.u_addr.ip4));
        out.ip = b;
        ok = !out.jid.empty();
      }
  }
  mdns_query_results_free(res);
  return ok;
}

int dial(const std::string &ip) {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  sockaddr_in d = {};
  d.sin_family = AF_INET;
  d.sin_port = htons(5222);
  inet_pton(AF_INET, ip.c_str(), &d.sin_addr);
  timeval to = {8, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
  if (connect(fd, (sockaddr *)&d, sizeof d) != 0) {
    close(fd);
    return -1;
  }
  std::string hdr = "<stream:stream to=\"products.bang-olufsen.com\" from=\"" + gId.jid() +
                    "\" xmlns=\"jabber:client\" xmlns:stream=\"http://etherx.jabber.org/streams\" version=\"1.0\">";
  send(fd, hdr.data(), hdr.size(), 0);
  return fd;
}

void sendIq(int fd, const Leader &l, const std::string &body) {
  // The leader learns what we are from its own question to us, which follows the header; a
  // request that overtakes it may go unanswered (beo waits 2 s).
  vTaskDelay(pdMS_TO_TICKS(2000));
  std::string iq = "<iq id=\"201\" to=\"" + l.jid + "\" from=\"" + gId.jid() + "\" type=\"set\">" + body + "</iq>";
  send(fd, iq.data(), iq.size(), 0);
}

// 0 = reply seen and the document holds our entry, 1 = reply seen and it does not, -1 = no reply.
int readDocument(int fd, uint32_t timeoutMs) {
  enrol::DocScanner scan(gId.jid());
  uint32_t end = millis() + timeoutMs;
  char buf[1024];
  while (!scan.done() && (int32_t)(end - millis()) > 0) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    timeval tv = {1, 0};
    if (select(fd + 1, &rs, nullptr, nullptr, &tv) > 0) {
      int n = recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      scan.feed(buf, (size_t)n);
    }
  }
  if (!scan.done()) return -1;
  return scan.present() ? 0 : 1;
}

enum Result { kPresent, kEnrolled, kNoLeader, kNoAnswer, kRefused };

Result enrolOnce(bool allowInsert, bool skipCheck = false) {
  Leader l;
  if (!findLeader(l)) return kNoLeader;
  log_i("%s: configuration leader %s at %s", kTag, l.jid.c_str(), l.ip.c_str());
  int fd = dial(l.ip);
  if (fd < 0) return kNoAnswer;
  if (!skipCheck) {
    sendIq(fd, l, enrol::subscribe_body());
    int doc = readDocument(fd, 40000);
    close(fd);
    if (doc < 0) return kNoAnswer;
    if (doc == 0) return kPresent;
    if (!allowInsert) return kNoAnswer;
    log_w("%s: %s is not in the configuration held by %s; sending its entry", kTag, gId.jid().c_str(), l.jid.c_str());
    fd = dial(l.ip); // the leader stops answering a session that has written to it: a new one
    if (fd < 0) return kNoAnswer;
  } else {
    log_w("%s: sending the entry again, without checking (claims PROPRIETARY: %s)", kTag, gId.claim_proprietary ? "yes" : "no");
  }
  sendIq(fd, l, enrol::insert_body(gId));
  std::string reply;
  char buf[512];
  uint32_t end = millis() + 30000;
  while ((int32_t)(end - millis()) > 0 && reply.find("</iq>") == std::string::npos) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    timeval tv = {1, 0};
    if (select(fd + 1, &rs, nullptr, nullptr, &tv) > 0) {
      int n = recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      if (reply.size() < 8192) reply.append(buf, (size_t)n);
    }
  }
  close(fd);
  if (reply.empty()) return kNoAnswer;
  if (reply.find("<accepted>1</accepted>") == std::string::npos) {
    log_e("%s: the leader did not accept the entry: %.300s", kTag, reply.c_str());
    return kRefused;
  }
  return kEnrolled;
}

std::atomic<bool> gRunning{false};

void enrolTask(void *arg) {
  bool force = arg != nullptr;
  if (force) {
    // One Insert, no check: the entry in the configuration is stale (it still claims what we claimed
    // when it was first sent). Whether an Insert replaces an existing entry is not known.
    Result r = enrolOnce(true, true);
    static const char *names[] = {"present", "enrolled", "no leader found", "no answer", "refused"};
    log_w("%s: entry sent (%s). The leader answers on a session it opens to us, so 'no answer' is expected; "
          "check by joining.", kTag, names[r]);
    gRunning = false;
    vTaskDelete(nullptr);
  }
  bool inserted = false;
  for (;;) {
    // One Insert per boot: the leader's answer may arrive on a session it opened to us
    // instead of ours, so "no answer" here does not mean "not added". Check again later.
    Result r = enrolOnce(!inserted);
    if (r == kPresent) { log_i("%s: already in the network's configuration", kTag); break; }
    if (r == kEnrolled) { log_w("%s: added to the network's configuration; sources will now offer us", kTag); break; }
    static const char *names[] = {"present", "enrolled", "no leader found", "no answer", "refused"};
    if (inserted) {
      log_w("%s: gave up checking. The leader answers on a session it opens to us, not on ours, so a "
            "reply here is not expected; the Insert was sent once. Check in the B&O app whether sources offer this product.", kTag);
      break;
    }
    if (r == kRefused || r == kNoAnswer) inserted = true;
    log_w("%s: not yet confirmed in the network's configuration (%s); checking again in 60 s", kTag, names[r]);
    vTaskDelay(pdMS_TO_TICKS(60000));
  }
  gRunning = false;
  vTaskDelete(nullptr);
}

} // namespace

void enrolStart(const Identity &id) {
  gId = id;
  if (gRunning.exchange(true)) return;
  xTaskCreatePinnedToCore(enrolTask, "enrol", 7168, nullptr, 2, nullptr, 0);
}

void enrolSendNow(const Identity &id) {
  gId = id;
  if (gRunning.exchange(true)) {
    log_w("%s: an enrolment is already running", kTag);
    return;
  }
  xTaskCreatePinnedToCore(enrolTask, "enrol", 7168, (void *)1, 2, nullptr, 0);
}
