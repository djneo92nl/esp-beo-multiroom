// Host-side harness: runs the core (no platform headers) and prints one line per
// request, "<METHOD> <path> <code> <body>", so compare_golden.py can diff it against
// beo's golden answers. Build and run with tests/host/run.sh.
#include <cstdio>
#include <iostream>
#include <string>

#include "../../src/core/mozart_api.h"

struct Events : EventSink {
  void send(const std::string &t, const std::string &d) override { printf("EVENT %s %s\n", t.c_str(), d.c_str()); }
};
struct Port : StatePort {
  void volume_changed(int l, int m, bool mu) override { printf("PORT volume %d/%d %d\n", l, m, mu); }
  void play_state_changed(const std::string &s) override { printf("PORT play %s\n", s.c_str()); }
  void default_volume_changed(int l) override { printf("PORT default %d\n", l); }
};
struct FakeNet : Net {
  std::vector<Peer> peers() override {
    return {{"2730.1200322.99000101@products.bang-olufsen.com", "One", "203.0.113.11"},
            {"2730.1200322.99000102@products.bang-olufsen.com", "Two", "203.0.113.12"},
            {"3400.1200488.99000103@products.bang-olufsen.com", "TV", "203.0.113.13"}};
  }
  std::string own_ip() override { return "203.0.113.2"; }
  std::string new_guid() override { return "11111111-2222-4333-8444-555555555555"; }
};
struct Link : LinkPort {
  bool join(const std::string &j, const std::string &r) override { printf("LINK join %s %s\n", j.c_str(), r.c_str()); return true; }
  void leave() override { printf("LINK leave\n"); }
};

int main(int argc, char **argv) {
  Identity id;
  id.serial = "99000100";
  id.mac = "00:00:5E:00:53:01";
  // The golden files were recorded as a Classics Adapter: same type, other item and name.
  bool golden = argc > 1 && std::string(argv[1]) == "golden";
  if (golden) { id.item_no = "1454530"; id.friendly = "Classics Adapter (emu)"; }
  Events ev; Port port; FakeNet net; Link link;
  PlayerState st(id, ev, port);
  if (golden) { st.set_maximum(72); st.set_power("standby"); }
  Mozart m(id, st, net, &link);
  std::string line;
  while (std::getline(std::cin, line)) {
    size_t a = line.find(' '), b = line.find(' ', a + 1);
    HttpRequest r{line.substr(0, a), line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1),
                  b == std::string::npos ? "" : line.substr(b + 1)};
    HttpResponse resp = m.handle(r);
    printf("%s %s %d %s\n", r.method.c_str(), r.path.c_str(), resp.code, resp.body.c_str());
  }
}
