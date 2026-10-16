// What the product says it is, on every plane at once. Derived from beo's
// ProductIdentity / ProductProfile (Apache-2.0, Tobias Laursen); see NOTICE.
#pragma once
#include <string>

struct Identity {
  std::string type_no = "3150";           // Beolab 8: the only verified type number
  std::string item_no = "1200322";        // a Beolab 8's; 1454530 would trigger the Classics Adapter check
  std::string serial;                     // 8 digits, 99xxxxxx band
  std::string mac;                        // AA:BB:CC:DD:EE:FF, upper case
  std::string friendly = "ESP32 Headphones";
  std::string firmware = "1.9.0.30523";   // 2-4 numeric parts only, or the app crashes
  std::string product_default_name = "Beolab 8";
  std::string product_type = "SEOUL";     // the app's name for type 3150
  // Whether we claim to handle PROPRIETARY content (in Connect and in the shared-configuration entry).
  // beo does. A Moment answered "Unable to add sink" without it (findings 11.1); a setting lets us
  // test again with the entry and the Connect consistent.
  bool claim_proprietary = true;

  std::string bare_jid() const { return type_no + "." + item_no + "." + serial; }
  std::string jid() const { return bare_jid() + "@products.bang-olufsen.com"; }
};
