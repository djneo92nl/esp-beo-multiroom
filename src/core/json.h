// Tiny JSON helpers for the core: no platform headers, no allocations beyond std::string.
// Derived in spirit from beo's facade, which builds the same payloads with json.dumps.
#pragma once
#include <cstdio>
#include <string>

namespace json {

inline std::string str(const std::string &s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
    case '"': o += "\\\""; break;
    case '\\': o += "\\\\"; break;
    case '\n': o += "\\n"; break;
    case '\r': o += "\\r"; break;
    case '\t': o += "\\t"; break;
    default:
      if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
      else o += (char)c;
    }
  }
  return o + "\"";
}

inline const char *boolean(bool b) { return b ? "true" : "false"; }

// First integer after `"key"` at or beyond `from`. -1 when absent. The app's bodies are tiny
// and flat or one level deep, so a scanner is enough: {"level":30}, {"default":{"level":30}}.
inline bool find_int(const std::string &body, const char *key, int &out, size_t from = 0) {
  size_t k = body.find(std::string("\"") + key + "\"", from);
  if (k == std::string::npos) return false;
  size_t i = body.find(':', k);
  if (i == std::string::npos) return false;
  i++;
  while (i < body.size() && (body[i] == ' ' || body[i] == '{' || body[i] == '"')) {
    // "level":{"level":N} - step into the object and look for the inner key
    if (body[i] == '{') {
      size_t j = body.find("\"level\"", i);
      if (j == std::string::npos) return false;
      i = body.find(':', j);
      if (i == std::string::npos) return false;
      i++;
      continue;
    }
    i++;
  }
  bool neg = i < body.size() && body[i] == '-';
  if (neg) i++;
  if (i >= body.size() || body[i] < '0' || body[i] > '9') return false;
  int v = 0;
  while (i < body.size() && body[i] >= '0' && body[i] <= '9') v = v * 10 + (body[i++] - '0');
  out = neg ? -v : v;
  return true;
}

inline bool find_bool(const std::string &body, const char *key, bool &out) {
  size_t k = body.find(std::string("\"") + key + "\"");
  if (k == std::string::npos) return false;
  size_t t = body.find("true", k), f = body.find("false", k);
  size_t end = body.find_first_of(",}", k);
  if (t != std::string::npos && t < end) { out = true; return true; }
  if (f != std::string::npos && f < end) { out = false; return true; }
  return false;
}

inline bool find_string(const std::string &body, const char *key, std::string &out) {
  size_t k = body.find(std::string("\"") + key + "\"");
  if (k == std::string::npos) return false;
  size_t a = body.find('"', body.find(':', k) + 1);
  if (a == std::string::npos) return false;
  size_t b = body.find('"', a + 1);
  if (b == std::string::npos) return false;
  out = body.substr(a + 1, b - a - 1);
  return true;
}

} // namespace json
