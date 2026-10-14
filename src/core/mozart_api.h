// The Mozart REST surface the B&O app talks to. Pure request -> response; the
// platform layer owns sockets. Derived from beo's mozart/facade.py (Apache-2.0,
// Tobias Laursen); every shape follows docs/app-protocol.md and the golden files.
#pragma once
#include <string>

#include "identity.h"
#include "player_state.h"
#include "../platform/net.h"
#include "../platform/state.h"

struct HttpRequest {
  std::string method; // GET POST PUT PATCH DELETE
  std::string path;   // without query
  std::string body;
};

struct HttpResponse {
  int code = 200;
  std::string body = "{}"; // empty body for 204
};

class Mozart {
public:
  Mozart(const Identity &id, PlayerState &state, Net &net, LinkPort *link)
      : id_(id), state_(state), net_(net), link_(link) {}
  void set_link(LinkPort *link) { link_ = link; }

  HttpResponse handle(const HttpRequest &req);

private:
  HttpResponse get(const std::string &path);
  HttpResponse write(const HttpRequest &req, const std::string &path);
  std::string product_info() const;
  std::string remote_menu() const;
  std::string playback_sources() const;
  std::string beodevice() const;

  Identity id_;
  PlayerState &state_;
  Net &net_;
  LinkPort *link_;
};
