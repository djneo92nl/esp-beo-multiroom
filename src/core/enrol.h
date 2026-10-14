// The network's shared configuration: a product's entry and the requests that enrol it.
// Derived from beo's networklink/configuration.py (Apache-2.0, Tobias Laursen). No platform
// headers. Status: byte-for-byte equal to beo's builders on the host; never sent to a real
// leader yet.
#pragma once
#include <string>

#include "identity.h"

namespace enrol {

std::string entry_xml(const Identity &id);           // beo: device_entry(ident, [CD named after the product])
std::string insert_request();                        // body of the Insert iq, given entry_xml
std::string insert_body(const Identity &id);         // <Insert ...><config>Devices#1 ...</config>...
std::string subscribe_body();                        // ask for the whole document
std::string item_key(const std::string &name);       // dots in names are written %2E

// Looks through a document that arrives in chunks for this product's entry, without keeping
// the document (it is the whole network's, and a PSRAM-less ESP32 cannot hold it).
class DocScanner {
public:
  explicit DocScanner(const std::string &jid);
  void feed(const char *data, size_t n);
  bool present() const { return present_; }  // an entry for the jid before any <deleted> section
  bool done() const { return done_; }        // the closing </iq> of OUR reply (iq id 201, type result) has been seen

private:
  std::string needle_, tail_;
  bool present_ = false, deleted_seen_ = false, done_ = false, in_reply_ = false;
};

} // namespace enrol
