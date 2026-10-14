// The NetworkLink listener: beocore sessions on port 5222 and the handshake that makes a
// source send us FPS audio. Event-driven (no blocking), no platform headers.
//
// Derived from beo's networklink/{beocore,sources}.py and roles/listener.py (Apache-2.0,
// Tobias Laursen): same stanza text, same order. Status: built; checked byte-for-byte against
// beo's own StatusBuilder on the host (tests/host/test_link.py); NOT yet checked against a
// real B&O source.
#pragma once
#include <map>
#include <string>

#include "identity.h"

class LinkEnv {
public:
  virtual ~LinkEnv() {}
  virtual void send(int session, const std::string &bytes) = 0;
  virtual void close_session(int session) = 0;
  virtual bool fps_open() = 0;   // bind UDP+TCP 1234 BEFORE we send our Connect
  virtual void fps_close() = 0;
  virtual std::string our_ip() = 0;
  virtual void started(const std::string &source_key, const std::string &source_jid) = 0;
  virtual void stopped() = 0;
  virtual void refused(const std::string &reason) = 0;
  virtual void notification(const std::string &kind, const std::string &xml) = 0;
  virtual void log(const std::string &line) = 0;
};

struct LinkVolume {
  int level = 30, minimum = 0, maximum = 100;
  bool muted = false;
};

class VolumeProvider {
public:
  virtual ~VolumeProvider() {}
  virtual LinkVolume link_volume() = 0;
};

class NetworkLinkRx {
public:
  NetworkLinkRx(const Identity &id, LinkEnv &env, VolumeProvider &vol) : id_(id), env_(env), vol_(vol) {}

  // A TCP session exists. dialled = we opened it (join); else the peer did.
  void session_opened(int sid, bool dialled);
  void session_data(int sid, const char *data, size_t n);
  void session_closed(int sid);

  // Listener-initiated join on a session we dialled: ask what is playing and connect to it.
  void start_join(int sid, const std::string &source_jid_hint);
  // Leave the current source: our own Disconnect + unsubscribe, then stop.
  void leave();

  int current_session() const { return cur_.sid; } // the session a stream runs on, or -1
  bool listening() const { return cur_.sid >= 0 && cur_.playing; }
  const std::string &source_jid() const { return cur_.source_jid; }

  // Names the entries enrolment and Attach use: one source, "CD", named after the product.
  std::string contact_info_xml() const;
  static std::string catalog_item(const Identity &id);

private:
  struct Session {
    bool dialled = false;
    bool greeting_owed = false;
    bool discarding = false;     // inside a reply we only need the head of (the source's FullStatus)
    std::string buf;
    std::string peer_jid;
    int next_id = 200; // rising decimal ids, as a product counts them
  };
  struct Current {
    int sid = -1;
    std::string source_key, source_jid;
    int connect_id = 0;      // our Connect request id awaiting its reply
    int subscribe_id = 0;    // our _notification-subscribe awaiting its FullStatus
    int experiences_id = 0;  // join: our Experiences request
    int connection_id = 0;
    bool playing = false;
  };

  void handle(int sid, const std::string &stanza);
  void handle_reply(int sid, const std::string &id, const std::string &xml);
  void begin_connect(int sid, const std::string &key, const std::string &jid);
  void send_request(int sid, const std::string &inner, int *id_out, const char *type = "set");
  void send_result(int sid, const std::string &id, const std::string &to, const std::string &inner);
  void send_error(int sid, const std::string &id, const std::string &to);
  void send_header(int sid, const std::string &to);
  void goodbye();
  void end_current(bool tell_source);

  Identity id_;
  LinkEnv &env_;
  VolumeProvider &vol_;
  std::map<int, Session> sessions_;
  Current cur_;
};

namespace beocore {
// Text helpers shared with tests.
std::string tag(const std::string &xml, const std::string &name);
std::string attr(const std::string &head, const std::string &name);
} // namespace beocore
