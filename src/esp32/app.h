// ESP32 platform layer: wires the core to garnet_web, esp_http_server and mDNS.
#pragma once

// Registers the garnet_settings groups. Call before gwBegin().
void appRegisterSettings();
// Derives the identity from the efuse MAC and seeds the hostname. Call before gwBegin().
void appInit();
// Call from loop(): starts the servers and mDNS records once the uplink is up.
void appLoop();
