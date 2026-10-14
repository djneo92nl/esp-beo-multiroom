#include <Arduino.h>
#include <garnet_web.h>

#include "esp32/app.h"

#ifndef GW_UI_PORT
#define GW_UI_PORT 8000
#endif

void setup() {
  Serial.begin(115200);
  appRegisterSettings();
  appInit();

  GwConfig cfg;
  cfg.name = "ESP32 Headphones";   // setup AP SSID prefix
  cfg.defaultPassword = "changeme"; // web UI password until changed in System
  cfg.appVersion = "0.1.0-phase1";
  // Mozart must own port 80 (the app builds its URLs without a port), so the settings UI
  // moves. Consequence: no OS captive-portal detection; open http://192.168.4.1:8000 on the setup AP.
  cfg.port = GW_UI_PORT;
  gwBegin(cfg);
}

void loop() {
  gwLoop();
  appLoop();
  delay(5);
}
