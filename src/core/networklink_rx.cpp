#include "networklink_rx.h"

#include <cstdio>
#include <cstring>

#include "json.h"

namespace {

constexpr const char *kDomain = "products.bang-olufsen.com";
constexpr size_t kMaxStanza = 24 * 1024;

// What a listener says it can decode (escaped XML inside the Connect). The line break after
// the declaration is part of it: the content is text, so a parser does not smooth it over.
// beo declares NONE, DECIMATE and PROPRIETARY, and so do we: a Beosound Moment refuses ("Unable
// to add sink") a listener that does not claim PROPRIETARY (findings.md section 11.1). What it
// then sends is not plain PCM, and nothing here plays it.
const char *kCpCapabilities =
    "&lt;?xml version=\"1.0\" encoding=\"utf-8\"?&gt;\n"
    "&lt;CpCapabilities&gt;&lt;ProtectionSchemes&gt;&lt;Scheme&gt;NONE&lt;/Scheme&gt;"
    "&lt;Scheme&gt;DECIMATE&lt;/Scheme&gt;&lt;Scheme&gt;PROPRIETARY&lt;/Scheme&gt;"
    "&lt;/ProtectionSchemes&gt;&lt;/CpCapabilities&gt;";

std::string xesc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

bool has(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

std::string upper(std::string s) {
  for (char &c : s) c = (char)toupper((unsigned char)c);
  return s;
}

} // namespace

namespace beocore {

std::string tag(const std::string &xml, const std::string &name) {
  std::string open = "<" + name + ">", close = "</" + name + ">";
  size_t a = xml.find(open);
  if (a == std::string::npos) return "";
  a += open.size();
  size_t b = xml.find(close, a);
  return b == std::string::npos ? "" : xml.substr(a, b - a);
}

// Attribute of the first element in `head`, either quote style.
std::string attr(const std::string &head, const std::string &name) {
  size_t end = head.find('>');
  std::string h = head.substr(0, end == std::string::npos ? head.size() : end);
  for (const char *q : {"\"", "'"}) {
    std::string key = " " + name + "=" + q;
    size_t a = h.find(key);
    if (a == std::string::npos) continue;
    a += key.size();
    size_t b = h.find(q, a);
    if (b != std::string::npos) return h.substr(a, b - a);
  }
  return "";
}

} // namespace beocore

using beocore::attr;
using beocore::tag;

// ---- XML bodies, in the field order a product uses (sources.py) ---------------------------

std::string NetworkLinkRx::catalog_item(const Identity &id) {
  // SOURCES["CD"].named(<product name>): key CD, type CD, category 3, no IR, protection NONE.
  return "<item><identifier>CD</identifier><name>" + xesc(id.friendly) + "</name>"
         "<sourceType><type>CD</type></sourceType><category>3</category><inUse>1</inUse>"
         "<profile></profile><sourceKey></sourceKey><linkable>1</linkable>"
         "<productFriendlyName>" + xesc(id.friendly) + "</productFriendlyName>"
         "<contentProtection><schemeList><item>NONE</item></schemeList></contentProtection>"
         "<embeddedBinary><schemeList></schemeList></embeddedBinary>"
         "<uniqueSourceId><source>CD</source><jid>" + id.jid() + "</jid></uniqueSourceId>"
         "<recommendedIrMapping></recommendedIrMapping></item>";
}

std::string NetworkLinkRx::contact_info_xml() const {
  return "<contactInfo><name>" + xesc(id_.friendly) + "</name><macs><item>" + upper(id_.mac) + "</item></macs>"
         "<sources>" + catalog_item(id_) + "</sources>"
         "<roleCapability><roles><item>integratedMaster</item></roles></roleCapability></contactInfo>";
}

static std::string renderer_status(const LinkVolume &v) {
  return "<FullStatus><status><item><type>VOLUME</type><kind>renderer</kind><data><speaker><level>" +
         std::to_string(v.level) + "</level><muted>" + (v.muted ? "1" : "0") + "</muted><range><minimum>" +
         std::to_string(v.minimum) + "</minimum><maximum>" + std::to_string(v.maximum) +
         "</maximum></range></speaker></data></item></status></FullStatus>";
}

// ---- wire ---------------------------------------------------------------------------

void NetworkLinkRx::send_header(int sid, const std::string &to) {
  env_.send(sid, "<stream:stream to=\"" + to + "\" from=\"" + id_.jid() +
                     "\" xmlns=\"jabber:client\" xmlns:stream=\"http://etherx.jabber.org/streams\" version=\"1.0\">");
}

void NetworkLinkRx::send_request(int sid, const std::string &inner, int *id_out, const char *type) {
  Session &s = sessions_[sid];
  int n = ++s.next_id;
  if (id_out) *id_out = n;
  env_.send(sid, "<iq id=\"" + std::to_string(n) + "\" to=\"" + s.peer_jid + "\" from=\"" + id_.jid() +
                     "\" type=\"" + type + "\">" + inner + "</iq>");
}

void NetworkLinkRx::send_result(int sid, const std::string &id, const std::string &to, const std::string &inner) {
  env_.send(sid, "<iq id=\"" + id + "\" to=\"" + to + "\" from=\"" + id_.jid() + "\" type=\"result\">" + inner + "</iq>");
}

void NetworkLinkRx::send_error(int sid, const std::string &id, const std::string &to) {
  env_.send(sid, "<iq id=\"" + id + "\" to=\"" + to + "\" from=\"" + id_.jid() +
                     "\" type=\"error\"><error type=\"cancel\"><feature-not-implemented "
                     "xmlns=\"urn:ietf:params:xml:ns:xmpp-stanzas\"/></error></iq>");
}

// ---- session lifecycle ----------------------------------------------------------------

void NetworkLinkRx::session_opened(int sid, bool dialled) {
  Session &s = sessions_[sid];
  s = Session();
  s.dialled = dialled;
  if (dialled) send_header(sid, kDomain);   // we speak first on a session we open
  else s.greeting_owed = true;              // otherwise our header answers the peer's, naming it
}

void NetworkLinkRx::session_closed(int sid) {
  auto it = sessions_.find(sid);
  if (it == sessions_.end()) return;
  sessions_.erase(it);
  if (cur_.sid == sid) {
    env_.log("session to the source closed");
    end_current(false);
  }
}

void NetworkLinkRx::session_data(int sid, const char *data, size_t n) {
  auto it = sessions_.find(sid);
  if (it == sessions_.end()) return;
  Session &s = it->second;
  if (s.discarding) {
    // Skip the rest of a large reply without keeping it. The tail is kept in case "</iq>" is split.
    s.buf.append(data, n);
    size_t e = s.buf.find("</iq>");
    if (e == std::string::npos) {
      if (s.buf.size() > 5) s.buf.erase(0, s.buf.size() - 5);
      return;
    }
    s.buf.erase(0, e + 5);
    s.discarding = false;
    data = nullptr;
    n = 0;
  } else {
    s.buf.append(data, n);
  }
  for (;;) {
    // Skip whitespace and an <?xml?> declaration.
    size_t i = 0;
    while (i < s.buf.size() && (s.buf[i] == ' ' || s.buf[i] == '\r' || s.buf[i] == '\n' || s.buf[i] == '\t')) i++;
    s.buf.erase(0, i);
    if (s.buf.empty()) return;
    if (s.buf.compare(0, 5, "<?xml") == 0) {
      size_t e = s.buf.find("?>");
      if (e == std::string::npos) return;
      s.buf.erase(0, e + 2);
      continue;
    }
    if (s.buf.compare(0, 14, "<stream:stream") == 0) {
      size_t e = s.buf.find('>');
      if (e == std::string::npos) return;
      std::string head = s.buf.substr(0, e + 1);
      s.buf.erase(0, e + 1);
      env_.log("<< [" + std::to_string(sid) + "] " + head);
      std::string from = attr(head, "from");
      if (!from.empty() && s.peer_jid.empty()) s.peer_jid = from;
      if (s.greeting_owed) {
        // Answer the peer's header, naming it, then ask who it is (a product's greeting).
        s.greeting_owed = false;
        send_header(sid, from.empty() ? kDomain : from);
        if (!s.peer_jid.empty())
          send_request(sid, "<query xmlns=\"http://jabber.org/protocol/disco#info\"/>", nullptr, "get");
      }
      continue;
    }
    if (s.buf.compare(0, 16, "</stream:stream>") == 0) {
      env_.close_session(sid);
      return;
    }
    bool iq = s.buf.compare(0, 3, "<iq") == 0;
    bool msg = s.buf.compare(0, 8, "<message") == 0;
    if (!iq && !msg) {
      // Not something we understand; do not let junk grow without bound.
      if (s.buf.size() > 4096) env_.close_session(sid);
      size_t e = s.buf.find('<', 1);
      if (e == std::string::npos) return;
      s.buf.erase(0, e);
      continue;
    }
    size_t open_end = s.buf.find('>');
    if (open_end == std::string::npos) goto need_more;
    {
      // The source's FullStatus (our _notification-subscribe reply) is several KB and we only
      // need to know it arrived: do not hold it (a plain ESP32 has no heap to spare for it).
      if (iq && s.buf[open_end - 1] != '/' && cur_.sid == sid && cur_.subscribe_id != 0 &&
          attr(s.buf, "id") == std::to_string(cur_.subscribe_id) && attr(s.buf, "type") == "result") {
        std::string head = s.buf.substr(0, open_end + 1);
        size_t c = s.buf.find("</iq>", open_end);
        if (c == std::string::npos) {
          s.buf.clear();
          s.discarding = true;
          s.buf.reserve(8);
        } else s.buf.erase(0, c + 5);
        env_.log("<< [" + std::to_string(sid) + "] " + head + "... (reply not kept)");
        handle_reply(sid, std::to_string(cur_.subscribe_id), head);
        if (sessions_.find(sid) == sessions_.end()) return;
        continue;
      }
      size_t end;
      if (s.buf[open_end - 1] == '/') end = open_end + 1;
      else {
        std::string close = iq ? "</iq>" : "</message>";
        size_t c = s.buf.find(close, open_end);
        if (c == std::string::npos) goto need_more;
        end = c + close.size();
      }
      std::string stanza = s.buf.substr(0, end);
      s.buf.erase(0, end);
      handle(sid, stanza);
      if (sessions_.find(sid) == sessions_.end()) return;
      continue;
    }
  need_more:
    if (s.buf.size() > kMaxStanza) {
      env_.log("stanza too large for this device; closing the session");
      env_.close_session(sid);
    }
    return;
  }
}

// ---- stanza handling (ListenerRole._handle) -------------------------------------------------

void NetworkLinkRx::handle(int sid, const std::string &x) {
  Session &s = sessions_[sid];
  // Notifications arrive every second for the whole stream; not worth a log line each.
  if (x.compare(0, 8, "<message") != 0) env_.log("<< [" + std::to_string(sid) + "] " + x.substr(0, 300));
  std::string typ = attr(x, "type"), from = attr(x, "from"), id = attr(x, "id");
  if (!from.empty() && s.peer_jid.empty()) s.peer_jid = from;
  const std::string &to = from.empty() ? s.peer_jid : from;

  if ((typ == "result" || typ == "error") && !id.empty()) {
    handle_reply(sid, id, x);
    return;
  }

  // The session answers disco#info itself.
  if (typ == "get" && has(x, "http://jabber.org/protocol/disco#info")) {
    send_result(sid, id, to,
                "<query xmlns=\"http://jabber.org/protocol/disco#info\"><identity category=\"client\" name=\"" +
                    xesc(id_.friendly) + "\" type=\"product\"/><feature var=\"jid\\20escaping\"/>"
                    "<feature var=\"beocore.player.1\"/><feature var=\"beocore.tunnel.1\"/>"
                    "<feature var=\"beocore.product.1\"/><feature var=\"beocore.configuration.1\"/>"
                    "<feature var=\"beocore.notifications.1\"/></query>");
    return;
  }

  bool set = typ == "set", getset = typ == "get" || typ == "set";

  if (has(x, "<ConnectRequest") && set) {
    std::string key = tag(x, "source");
    std::string jid = tag(x, "jid");
    if (jid.empty()) jid = s.peer_jid;
    env_.log("ConnectRequest: " + jid + " asks us to play " + key);
    send_result(sid, id, to, "<ConnectRequest xmlns=\"beocore.player.1\"><connected>1</connected></ConnectRequest>");
    begin_connect(sid, key, jid);
    return;
  }
  if (has(x, "<_notification-subscribe") && set) {
    send_result(sid, id, to, "<_notification-subscribe xmlns=\"beocore.notifications.1\">" +
                                 renderer_status(vol_.link_volume()) + "</_notification-subscribe>");
    return;
  }
  if (has(x, "<_notification-unsubscribe") && set) {
    send_result(sid, id, to, "<_notification-unsubscribe xmlns=\"beocore.notifications.1\"/>");
    return;
  }
  if (has(x, "<Disconnect") && set) {
    env_.log("Disconnect connectionId=" + tag(x, "connectionId"));
    send_result(sid, id, to, "<Disconnect xmlns=\"beocore.player.1\"/>");
    if (cur_.sid == sid) end_current(true);
    return;
  }
  // Three questions with one prefix: the longer names first.
  if (set && has(x, "<GetVolumeMuted")) {
    send_result(sid, id, to, std::string("<GetVolumeMuted xmlns=\"beocore.product.1\"><muted>") +
                                 (vol_.link_volume().muted ? "1" : "0") + "</muted></GetVolumeMuted>");
    return;
  }
  if (set && has(x, "<GetVolumeRange")) {
    LinkVolume v = vol_.link_volume();
    send_result(sid, id, to, "<GetVolumeRange xmlns=\"beocore.product.1\"><range><minimum>" + std::to_string(v.minimum) +
                                 "</minimum><maximum>" + std::to_string(v.maximum) + "</maximum></range></GetVolumeRange>");
    return;
  }
  if (set && has(x, "<GetVolume")) {
    send_result(sid, id, to, "<GetVolume xmlns=\"beocore.product.1\"><volume>" + std::to_string(vol_.link_volume().level) +
                                 "</volume></GetVolume>");
    return;
  }
  if (x.compare(0, 8, "<message") == 0 && has(x, "<NOTIFICATION>")) {
    env_.notification(tag(x, "type"), x);
    return;
  }
  if (getset) {
    if (has(x, "<Attach")) {
      env_.log("Attach from " + from);
      send_result(sid, id, to, "<Attach xmlns=\"beocore.product.1\">" + contact_info_xml() + "</Attach>");
      return;
    }
    if (has(x, "<GetContactInfo")) {
      send_result(sid, id, to, "<GetContactInfo xmlns=\"beocore.product.1\">" + contact_info_xml() + "</GetContactInfo>");
      return;
    }
    if (has(x, "<Experiences")) {
      send_result(sid, id, to, "<Experiences xmlns=\"beocore.player.1\"><experienceInfo><list></list></experienceInfo></Experiences>");
      return;
    }
    env_.log("unanswered " + typ + ": " + x.substr(0, 160));
    if (!id.empty()) send_error(sid, id, to);
  }
}

// ---- the handshake we drive ---------------------------------------------------------------

void NetworkLinkRx::begin_connect(int sid, const std::string &key, const std::string &jid) {
  if (cur_.sid >= 0) end_current(true); // another stream takes this one's place
  cur_ = Current();
  cur_.sid = sid;
  cur_.source_key = key;
  cur_.source_jid = jid;
  // The source starts sending before it answers the Connect, so the receiver is bound first.
  if (!env_.fps_open()) {
    env_.refused("could not bind the FPS port");
    cur_ = Current();
    return;
  }
  send_request(sid,
               "<Connect xmlns=\"beocore.player.1\"><sourceId>" + key + "</sourceId><connectionInfo>FPS://" +
                   env_.our_ip() + "</connectionInfo><contentProtectionInfo>" + kCpCapabilities +
                   "</contentProtectionInfo><uniqueSourceId><source>" + key + "</source><jid>" + jid +
                   "</jid></uniqueSourceId></Connect>",
               &cur_.connect_id);
}

void NetworkLinkRx::handle_reply(int sid, const std::string &id, const std::string &x) {
  int n = atoi(id.c_str());
  if (cur_.sid != sid) return;
  if (n == cur_.connect_id && n != 0) {
    cur_.connect_id = 0;
    if (attr(x, "type") == "error") {
      size_t t = x.find("<text");
      std::string reason = "error without a text";
      if (t != std::string::npos) {
        size_t a = x.find('>', t), b = x.find("</text>", t);
        if (a != std::string::npos && b != std::string::npos) reason = x.substr(a + 1, b - a - 1);
      }
      env_.log("the source refused our Connect: " + reason);
      env_.fps_close();
      env_.refused(reason);
      cur_ = Current();
      return;
    }
    std::string cid = tag(x, "connectionId");
    cur_.connection_id = 1; // beo counts per session; the source's own value is a jid and is not echoed back
    env_.log("Connect accepted, connectionId=" + cid);
    send_request(sid,
                 "<_notification-subscribe xmlns=\"beocore.notifications.1\"><_notification-filter><Filter><Filter>"
                 "<includeKindFilter><item>device</item><item>playing</item><item>source</item></includeKindFilter>"
                 "<excludeTypeFilter></excludeTypeFilter></Filter></Filter></_notification-filter></_notification-subscribe>",
                 &cur_.subscribe_id);
    return;
  }
  if (n == cur_.subscribe_id && n != 0) {
    cur_.subscribe_id = 0;
    cur_.playing = true;
    env_.started(cur_.source_key, cur_.source_jid);
    return;
  }
  if (n == cur_.experiences_id && n != 0) {
    cur_.experiences_id = 0;
    // <uniqueSourceId><source>K</source><jid>J</jid>
    size_t u = x.find("<uniqueSourceId><source>");
    if (u == std::string::npos) {
      env_.log("join: nothing playing to join");
      env_.refused("nothing playing");
      cur_ = Current();
      return;
    }
    std::string rest = x.substr(u);
    std::string key = tag(rest, "source"), jid = tag(rest, "jid");
    cur_.sid = -1; // begin_connect re-binds it
    begin_connect(sid, key, jid);
  }
}

void NetworkLinkRx::start_join(int sid, const std::string &source_jid_hint) {
  if (cur_.sid >= 0 && cur_.playing) return; // a second Connect would restart the stream
  // We dialled it, so we know who it is before it says (beo's dial() passes the jid): the
  // request must name the peer, or it goes unanswered.
  if (!source_jid_hint.empty() && sessions_[sid].peer_jid.empty()) sessions_[sid].peer_jid = source_jid_hint;
  cur_ = Current();
  cur_.sid = sid;
  send_request(sid, "<Experiences xmlns=\"beocore.player.1\"></Experiences>", &cur_.experiences_id);
}

void NetworkLinkRx::goodbye() {
  if (cur_.sid < 0 || sessions_.find(cur_.sid) == sessions_.end()) return;
  if (cur_.connection_id)
    send_request(cur_.sid, "<Disconnect xmlns=\"beocore.player.1\"><connectionId>" + std::to_string(cur_.connection_id) +
                               "</connectionId></Disconnect>", nullptr);
  send_request(cur_.sid, "<_notification-unsubscribe xmlns=\"beocore.notifications.1\"></_notification-unsubscribe>", nullptr);
}

void NetworkLinkRx::end_current(bool tell_source) {
  if (cur_.sid < 0) return;
  bool was_playing = cur_.playing;
  env_.fps_close();
  if (tell_source) goodbye();
  cur_ = Current();
  if (was_playing) env_.stopped();
}

void NetworkLinkRx::leave() { end_current(true); }
