// garnet_settings groups. Everything the user can change lives here, not in Kconfig.
#include <garnet_settings.h>

#include "app.h"
#include <esp_heap_caps.h>

#include "globals.h"
#include "link.h"

namespace {

String jidInfo() { return String(gIdentity.jid().c_str()); }
String serialInfo() { return String(gIdentity.serial.c_str()); }
String wsInfo() { return String(gWsClients); }
String peersInfo() { return String(gPeerCount); }
String listenInfo() { return gLinkStats.listening ? "Listening" : "Idle"; }
String fillInfo() { return String(gLinkStats.fill_ms.load()) + " of " + String(gLinkStats.buffer_ms.load()) + " ms"; }
String rxInfo() { return String(gLinkStats.received.load()); }
String lostInfo() { return String(gLinkStats.lost.load()); }
String underInfo() { return String(gLinkStats.underruns.load()) + " / " + String(gLinkStats.overruns.load()); }
String rateInfo() { return gLinkStats.rate.load() ? String(gLinkStats.rate.load()) + " Hz" : String("-"); }
String markerInfo() { return String(gLinkStats.markers.load()); }
String heapInfo() {
  return String(esp_get_free_heap_size() / 1024) + " KB, largest block " +
         String(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024) + " KB";
}

void sendEntryNow() { enrolSendNow(gIdentity); }
void toneNow() { linkTestTone(); }

const GsField kProductFields[] = {
    gsWithHelp(gsText("name", "Product name", "ESP32 Headphones", 32), "Shown in the B&O app. Applied after reboot."),
    gsInfo("jid", "Product id", jidInfo),
    gsInfo("serial", "Serial", serialInfo),
    gsButton("resend", "Send enrolment entry again", sendEntryNow,
             "Changes the configuration every B&O product on the network shares. Send once, after changing 'Claim PROPRIETARY'."),
    gsWithHelp(gsText("ptype", "Product type (mDNS)", "SEOUL", 24),
               "Announced to other products as productType. Beo uses the type name; a real M3 only answered to its hardware code (CA17). Applied after reboot."),
    gsWithHelp(gsToggle("claimprop", "Claim PROPRIETARY support", true),
               "Declared to sources in Connect and in the enrolment entry. Off = NONE and DECIMATE only. A Beosound Moment refused a listener that did not claim it. Applied after reboot."),
    gsWithHelp(gsToggle("enrol", "Enrol in network configuration", false),
               "Adds this product to the list every B&O product on the network shares, so sources offer it. Persistent: it is never removed from here."),
};
const GsGroup kProduct = gsWithReboot(gsGroup("product", "Product", "speaker", kProductFields));

const GsField kAudioFields[] = {
    gsNumber("defvol", "Default volume", 60, 0, 100, 1),
    gsButton("tone", "Play test tone (5 s)", toneNow),
    gsWithHelp(gsNumber("bufms", "Buffer (ms)", 250, 100, 600, 50), "Jitter buffer, allocated at boot and trimmed to what the free heap allows (see Status). Applied after reboot."),
    gsNumber("bck", "I2S BCK pin", 26, 0, 39, 1),
    gsNumber("ws", "I2S LRCK pin", 25, 0, 39, 1),
    gsNumber("dout", "I2S DATA pin", 22, 0, 33, 1),
    gsWithHelp(gsToggle("listen", "Debug: always listen on FPS port", false), "Plays any stream sent to UDP 1234 without a session (for replay tests)."),
};
const GsGroup kAudio = gsWithReboot(gsGroup("audio", "Audio", "volume", kAudioFields));

const GsField kStatusFields[] = {
    gsInfo("ws", "App connections", wsInfo),
    gsInfo("peers", "Products on network", peersInfo),
    gsInfo("link", "NetworkLink", listenInfo),
    gsInfo("rate", "Sample rate", rateInfo),
    gsInfo("fill", "Buffer fill", fillInfo),
    gsInfo("rx", "Datagrams received", rxInfo),
    gsInfo("lost", "Datagrams lost", lostInfo),
    gsInfo("under", "Underruns / overruns", underInfo),
    gsInfo("markers", "Pause/play markers", markerInfo),
    gsInfo("heap", "Free memory", heapInfo),
};
const GsGroup kStatus = gsGroup("status", "Status", "info", kStatusFields);

} // namespace

void appRegisterSettings() {
  gsRegister(kProduct);
  gsRegister(kAudio);
  gsRegister(kStatus);
}
