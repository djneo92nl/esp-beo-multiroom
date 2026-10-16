#include "enrol.h"

#include <vector>

namespace enrol {

namespace {

std::string esc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

// item(name, children..., data): the data element comes first, then the children.
std::string item(const std::string &name, const std::string &children, const char *data = nullptr) {
  std::string body = children;
  if (data) body = "<data>" + esc(data) + "</data>" + body;
  return "<item name=\"" + item_key(name) + "\">" + body + "</item>";
}
std::string item_d(const std::string &name, const std::string &data) { return item(name, "", data.c_str()); }
std::string empty() { return item_d("^emptyValue", "true"); }

std::string many(const std::string &name, const std::vector<std::string> &values) {
  std::string o;
  for (size_t i = 0; i < values.size(); i++) o += item_d(i == 0 ? name : name + "_" + std::to_string(i), values[i]);
  return o;
}
std::string schemes(const std::vector<std::string> &v) {
  return v.empty() ? empty() : item("Schemes#1", many("Scheme#1", v));
}

std::string upper(std::string s) {
  for (char &c : s) c = (char)toupper((unsigned char)c);
  return s;
}

std::string source_entry(const Identity &id) {
  return item("CD",
              item_d("Name#1", id.friendly) + item("SourceType#1", item_d("Type#1", "CD")) +
                  item_d("Category#1", "MUSIC") + item_d("InUse#1", "true") + item("Profile#1", empty()) +
                  item_d("Linkable#1", "true") + item("ContentProtection#1", schemes({"NONE"})) +
                  item("EmbeddedBinary#1", schemes({})) +
                  item("UniqueSourceId#1", item_d("Source#1", "CD") + item_d("Jid#1", id.jid())) +
                  item("RecommendedIrMappings#1", empty()) + item_d("Index#1", "0") + item_d("ProductName#1", id.friendly));
}

} // namespace

std::string item_key(const std::string &name) {
  std::string o;
  for (char c : name) {
    if (c == '.') o += "%2E";
    else o += c;
  }
  return o;
}

std::string entry_xml(const Identity &id) {
  return item(
      id.jid(),
      item_d("Name#1", id.friendly) + item("Macs#1", item_d("Mac#1", upper(id.mac))) +
          item("Sources#1", source_entry(id)) + item_d("ConnectionType#1", "WIRELESS") + item_d("RenderDelayMs#1", "86") +
          item("Render#1",
               item("Fps#1", item("ContentProtection#1", schemes(id.claim_proprietary ? std::vector<std::string>{"NONE", "DECIMATE", "PROPRIETARY"} : std::vector<std::string>{"NONE", "DECIMATE"})) +
                                 item("StreamingVersions#1", item_d("Ver#1", "1"))) +
                   item("Multiroom#1", item("SupportedExperiences#1", many("SupportedExperience#1", {"join", "expand"})))) +
          item("EmbeddedBinary#1", empty()) + item("Fps#1", item("StreamingVersions#1", item_d("Ver#1", "1"))) +
          item("RoleList#1", item("Roles#1", item_d("Role#1", "integratedMaster"))) +
          item("Integrate#1", item_d("Role#1", "none") + item("Jid#1", empty())));
}

std::string insert_body(const Identity &id) {
  return "<Insert xmlns=\"beocore.configuration.1\"><config>" + item("Devices#1", entry_xml(id)) +
         "</config><version>1</version></Insert>";
}

std::string subscribe_body() {
  return "<_notification-subscribe xmlns=\"beocore.configuration.1\"><configuration><revision>0</revision>"
         "</configuration></_notification-subscribe>";
}

DocScanner::DocScanner(const std::string &jid) : needle_("<item name=\"" + item_key(jid) + "\">") {}

void DocScanner::feed(const char *data, size_t n) {
  if (done_) return;
  std::string w = tail_ + std::string(data, n);
  // The leader says other things on this stream too (its own questions to us, with their own
  // ids). Only the iq answering ours counts: id 201, type result.
  size_t scan = 0;
  while (!in_reply_) {
    size_t iq = w.find("<iq ", scan);
    if (iq == std::string::npos) break;
    size_t close = w.find('>', iq);
    if (close == std::string::npos) break; // head not complete yet
    std::string head = w.substr(iq, close - iq);
    if (head.find("id=\"201\"") != std::string::npos && head.find("type=\"result\"") != std::string::npos) {
      in_reply_ = true;
      w.erase(0, close + 1);
      break;
    }
    scan = close + 1;
  }
  if (!in_reply_) {
    size_t keep = 400;
    tail_ = w.size() > keep ? w.substr(w.size() - keep) : w;
    return;
  }
  // Order matters: an entry counts only before a <deleted> section, as in configuration.enrolled().
  size_t del = w.find("<deleted>");
  size_t hit = w.find(needle_);
  if (hit != std::string::npos && !deleted_seen_ && (del == std::string::npos || hit < del)) present_ = true;
  if (del != std::string::npos) deleted_seen_ = true;
  if (w.find("</iq>") != std::string::npos) done_ = true;
  // Keep enough to match a needle or marker split across chunks.
  size_t keep = needle_.size() + 16;
  tail_ = w.size() > keep ? w.substr(w.size() - keep) : w;
}

} // namespace enrol
