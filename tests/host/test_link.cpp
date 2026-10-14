// Drives NetworkLinkRx from stdin and prints what it sends, one "SEND <sid> <bytes>" per line
// (newlines inside XML are escaped as \n). test_link.py feeds the same conversation to beo's
// own code and diffs the two.
#include <cstdio>
#include <iostream>
#include <string>

#include "../../src/core/networklink_rx.h"

struct Env : LinkEnv {
  void send(int s, const std::string &b) override {
    std::string e;
    for (char c : b) e += c == '\n' ? std::string("\\n") : std::string(1, c);
    printf("SEND %d %s\n", s, e.c_str());
  }
  void close_session(int s) override { printf("CLOSE %d\n", s); }
  bool fps_open() override { printf("FPS open\n"); return true; }
  void fps_close() override { printf("FPS close\n"); }
  std::string our_ip() override { return "203.0.113.2"; }
  void started(const std::string &k, const std::string &j) override { printf("STARTED %s %s\n", k.c_str(), j.c_str()); }
  void stopped() override { printf("STOPPED\n"); }
  void refused(const std::string &r) override { printf("REFUSED %s\n", r.c_str()); }
  void notification(const std::string &k, const std::string &) override { printf("NOTIFY %s\n", k.c_str()); }
  void log(const std::string &) override {}
};
struct Vol : VolumeProvider {
  LinkVolume link_volume() override { LinkVolume v; v.level = 35; v.maximum = 90; return v; }
};

int main() {
  Identity id;
  id.type_no = "3150"; id.item_no = "1200322"; id.serial = "99000777"; id.mac = "00:00:5e:00:53:07"; id.friendly = "Capture Node";
  Env env; Vol vol;
  NetworkLinkRx rx(id, env, vol);
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.compare(0, 5, "OPEN ") == 0) { rx.session_opened(atoi(line.c_str() + 5), line.back() == '1'); continue; }
    if (line.compare(0, 5, "DATA ") == 0) {
      size_t sp = line.find(' ', 5);
      std::string x = line.substr(sp + 1), d;
      for (size_t i = 0; i < x.size(); i++) d += (x[i] == '\\' && i + 1 < x.size() && x[i + 1] == 'n') ? (i++, '\n') : x[i];
      rx.session_data(atoi(line.c_str() + 5), d.data(), d.size());
      continue;
    }
    if (line == "LEAVE") rx.leave();
    if (line.compare(0, 5, "JOIN ") == 0) rx.start_join(atoi(line.c_str() + 5), "");
  }
}
