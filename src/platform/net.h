// Platform interface 1 of 3: the network. Plain C++, no platform headers.
#pragma once
#include <string>
#include <vector>

struct Peer {
  std::string jid;   // full jid, <type>.<item>.<serial>@products.bang-olufsen.com
  std::string name;
  std::string ip;
};

class Net {
public:
  virtual ~Net() {}
  // Everyone heard from on mDNS so far, never ourselves. May be empty.
  virtual std::vector<Peer> peers() = 0;
  virtual std::string own_ip() = 0;
  virtual std::string new_guid() = 0; // lower-case hyphenated GUID
};
