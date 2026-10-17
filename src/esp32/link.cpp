// NetworkLink on the ESP32: beocore sessions (5222), FPS receive (UDP+TCP 1234), jitter buffer,
// I2S out. The protocol logic is in src/core; this file owns sockets, tasks and the DAC.
#include "link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <garnet_settings.h>
#include <lwip/sockets.h>

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/audio_control.h"
#include "../core/fps.h"
#include "../core/jitter_buffer.h"
#include "../core/networklink_rx.h"
#include "globals.h"

LinkStats gLinkStats;

namespace {

constexpr const char *kTag = "link";
constexpr int kMaxSessions = 2;

PlayerState *gState = nullptr;
std::recursive_mutex *gLock = nullptr;
Net *gNet = nullptr;

// ---- volume --------------------------------------------------------------------------

std::atomic<int> gLevel{30}, gMax{100};
std::atomic<bool> gMuted{false};

// ---- FPS receive + jitter buffer + I2S -----------------------------------------------------

std::mutex jbLock;
JitterBuffer *jb = nullptr;
std::atomic<bool> fpsRun{false};
std::atomic<bool> fpsTaskAlive{false};
std::atomic<uint32_t> wantRate{44100};
std::atomic<bool> gFlush{false};      // leaving: fade out and drop what is queued
std::atomic<bool> gSourceMuted{false}; // the source's pause/mute marker
std::atomic<uint32_t> toneUntilMs{0}; // test tone: 1 kHz, bypasses the stream and the volume
std::atomic<uint32_t> lastBeatMs{0};
std::string sourceJid;
uint32_t badLogged = 0; // unrecognised datagrams reported this session (fps task only)

i2s_chan_handle_t tx = nullptr;
uint32_t i2sRate = 0;

bool i2sEnsure(uint32_t rate) {
  if (tx && rate == i2sRate) return true;
  if (!tx) {
    int bck = gsGetInt("audio", "bck"), ws = gsGetInt("audio", "ws"), dout = gsGetInt("audio", "dout");
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 256;
    cc.auto_clear = true; // silence, not a repeat of the last buffer, when we run dry
    if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK) {
      log_e("%s: i2s_new_channel failed", kTag);
      tx = nullptr;
      return false;
    }
    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    sc.gpio_cfg.bclk = (gpio_num_t)bck;
    sc.gpio_cfg.ws = (gpio_num_t)ws;
    sc.gpio_cfg.dout = (gpio_num_t)dout;
    sc.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK || i2s_channel_enable(tx) != ESP_OK) {
      log_e("%s: I2S init failed (pins %d/%d/%d)", kTag, bck, ws, dout);
      i2s_del_channel(tx);
      tx = nullptr;
      return false;
    }
    log_i("%s: I2S out BCK %d WS %d DATA %d at %u Hz", kTag, bck, ws, dout, (unsigned)rate);
  } else {
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_disable(tx);
    i2s_channel_reconfig_std_clock(tx, &clk);
    i2s_channel_enable(tx);
    log_i("%s: I2S rate %u Hz", kTag, (unsigned)rate);
  }
  i2sRate = rate;
  gLinkStats.rate = (int)rate;
  return true;
}

// The audio task. One block of the stream is 22 to 24 ms. Per block:
//  - drift: if the buffer holds more or less than its target, the block is made one frame
//    shorter or longer (interpolated), at most every 4th block (audio::DriftPolicy);
//  - gain: volume, the source's pause/play mute and fades all move the gain towards its target by at
//    most 0.12 per block, ramped across the block, so nothing steps;
//  - the end of the stream (underrun, Wi-Fi gap, leaving) fades the last samples to zero and the next
//    start fades in from zero; leaving also drops what is still queued.
void audioTask(void *) {
  static int16_t out[fps::kFramesPerDatagram * 2 + 2];
  static int16_t tmp[fps::kFramesPerDatagram * 2 + 2];
  float gain = 0.f, phase = 0.f;
  int16_t lastL = 0, lastR = 0;
  bool wasPlaying = false;
  audio::DriftPolicy drift;
  for (;;) {
    uint32_t until = toneUntilMs.load();
    if (until && (int32_t)(until - millis()) > 0) {
      // 1 kHz at -12 dBFS, both channels: tells wiring and DAC apart from the network stream.
      if (!i2sEnsure(44100)) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
      const float step = 2.f * 3.14159265f * 1000.f / 44100.f;
      for (size_t i = 0; i < fps::kFramesPerDatagram; i++) {
        int16_t v = (int16_t)(sinf(phase) * 8192.f);
        phase += step;
        if (phase > 6.2831853f) phase -= 6.2831853f;
        out[2 * i] = v;
        out[2 * i + 1] = v;
      }
      size_t w = 0;
      i2s_channel_write(tx, out, fps::kFramesPerDatagram * 4, &w, pdMS_TO_TICKS(200));
      continue;
    }
    uint32_t want = wantRate.load();
    size_t frames = 0;
    bool have = false, fadeOnly = false;
    {
      std::lock_guard<std::mutex> g(jbLock);
      if (jb) {
        bool flush = gFlush.exchange(false);
        const uint8_t *b = jb->front();
        if (b) {
          float max = (float)(gMax.load() > 0 ? gMax.load() : 100);
          float v = gMuted.load() ? 0.f : (float)gLevel.load() / max;
          float target = gSourceMuted.load() ? 0.f : v * sqrtf(v); // v^1.5 (squared was -21 dB at the default: too quiet)
          if (flush) target = 0.f;
          drift.target = (float)jb->prefill();
          int dir = drift.decide(jb->level());
          if (dir != 0) {
            gLinkStats.corrections++;
            if (gLinkStats.corrections % 20 == 1)
              log_i("%s: drift correction %s (average fill %.1f, target %.0f blocks), %u so far", kTag, dir > 0 ? "+1 frame" : "-1 frame",
                    drift.avg, drift.target, (unsigned)gLinkStats.corrections.load());
          }
          frames = audio::adjust_block(reinterpret_cast<const int16_t *>(b), fps::kFramesPerDatagram, dir, tmp);
          float g1 = flush ? 0.f : audio::step_gain(gain, target, 0.12f);
          float g0 = gain, step = (g1 - g0) / (float)frames;
          for (size_t i = 0; i < frames; i++) {
            g0 += step;
            out[2 * i] = (int16_t)((float)tmp[2 * i] * g0);
            out[2 * i + 1] = (int16_t)((float)tmp[2 * i + 1] * g0);
          }
          gain = g1;
          jb->pop();
          if (flush) jb->drain();
          gLinkStats.fill_ms = (int)(jb->level() * 1062 * 1000 / (i2sRate ? i2sRate : 44100));
          gLinkStats.underruns = jb->stats().underruns;
          have = true;
        } else {
          if (flush) jb->drain();
          gLinkStats.underruns = jb->stats().underruns;
          if (wasPlaying) {
            // The stream ran dry (Wi-Fi gap, source stopped): ramp the last sample to zero so there
            // is no click, and start the next one from silence.
            frames = 192;
            for (size_t i = 0; i < frames; i++) {
              float k = 1.f - (float)(i + 1) / (float)frames;
              out[2 * i] = (int16_t)((float)lastL * k);
              out[2 * i + 1] = (int16_t)((float)lastR * k);
            }
            gain = 0.f;
            drift.restart();
            wasPlaying = false;
            have = true;
            fadeOnly = true;
          }
        }
      }
    }
    if (!have) {
      vTaskDelay(pdMS_TO_TICKS(4));
      continue;
    }
    if (!i2sEnsure(want)) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!fadeOnly) {
      wasPlaying = true;
      lastL = out[2 * (frames - 1)];
      lastR = out[2 * (frames - 1) + 1];
    }
    size_t written = 0;
    i2s_channel_write(tx, out, frames * 4, &written, pdMS_TO_TICKS(200));
  }
}

void fpsTask(void *) {
  fpsTaskAlive = true;
  int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  int lis = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  int conn = -1;
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(fps::kPort);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  int one = 1;
  setsockopt(lis, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  int rcv = 16 * 1024;
  setsockopt(udp, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
  bool ok = udp >= 0 && lis >= 0 && bind(udp, (sockaddr *)&a, sizeof a) == 0 && bind(lis, (sockaddr *)&a, sizeof a) == 0 &&
            listen(lis, 1) == 0;
  if (!ok) log_e("%s: could not bind FPS port 1234", kTag);
  std::vector<uint8_t> buf(4352);
  uint16_t prev = 0;
  bool have_prev = false;
  uint16_t fmt = 0;
  uint32_t lastLog = millis();
  // What the stream actually looks like, to tell plain PCM from something else and real-time
  // pacing from bursts: signal level, and the spacing of arrivals.
  int32_t peak = 0;
  uint64_t sumsq = 0, nsamp = 0;
  uint32_t lastArrival = 0, dmin = 0xFFFFFFFF, dmax = 0, dn = 0, dsum = 0, headersLogged = 0;
  while (ok && fpsRun) {
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(udp, &rs);
    FD_SET(lis, &rs);
    int maxfd = udp > lis ? udp : lis;
    if (conn >= 0) {
      FD_SET(conn, &rs);
      if (conn > maxfd) maxfd = conn;
    }
    timeval tv = {0, 200000};
    if (select(maxfd + 1, &rs, nullptr, nullptr, &tv) > 0) {
      if (FD_ISSET(udp, &rs)) {
        int n = recvfrom(udp, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n > 0) {
          fps::Packet p = fps::parse(buf.data(), (size_t)n);
          if (p.kind == fps::Kind::Bad) {
            // Not what beo's sender sends. Say so (a few times): this is how a different stream
            // format or protection scheme would show up.
            gLinkStats.bad++;
            if (badLogged++ < 5) {
              char hx[400];
              int k = n < 130 ? n : 130;
              for (int i = 0; i < k; i++) snprintf(hx + i * 3, 4, "%02x ", buf[i]);
              log_w("%s: UNRECOGNISED datagram, %d bytes: %s", kTag, n, hx);
              if (badLogged <= 3) {
                // The whole datagram, to work out offline what it is (findings.md section 11).
                // One short line per 96 bytes, each tagged, so other tasks' log lines cannot
                // corrupt it: reassemble offline by the tag.
                for (int off = 0; off < n; off += 96) {
                  char line[240];
                  int m = n - off < 96 ? n - off : 96, k2 = snprintf(line, sizeof line, "\nDG%u.%04d.%d:", (unsigned)badLogged, off, n);
                  for (int i = 0; i < m; i++) k2 += snprintf(line + k2, sizeof line - k2, "%02x", buf[off + i]);
                  Serial.print(line);
                  Serial.print(";\n");
                }
              }
            }
          }
          if (p.kind != fps::Kind::Bad) {
            // Markers share the audio sequence counter, so continuity is checked on both.
            uint16_t gap = have_prev ? (uint16_t)(p.seq - prev - 1) : 0;
            if (gap > 1000) gap = 0; // a restart, not a loss
            prev = p.seq;
            have_prev = true;
            if (p.kind == fps::Kind::Marker) {
              gLinkStats.markers++;
              // beo: counter 2 opens the stream, then +1 per event: odd = MuteAllSinks (pause), even =
              // unmute (play). That is beo's reading of its own sender; not yet seen from a real pause.
              bool mute = (p.marker_counter & 1) != 0;
              gSourceMuted = mute;
              log_i("%s: marker #%u -> source %s", kTag, p.marker_counter, mute ? "paused: muting" : "playing: unmuting");
            } else {
              if (p.format != fmt) {
                fmt = p.format;
                wantRate = fps::rate_for_format(fmt);
                log_i("%s: stream format %u = %u Hz", kTag, fmt, (unsigned)wantRate.load());
              }
              uint32_t nowUs = (uint32_t)esp_timer_get_time();
              if (lastArrival) {
                uint32_t dt = (nowUs - lastArrival) / 100; // 0.1 ms
                if (dt < dmin) dmin = dt;
                if (dt > dmax) dmax = dt;
                dsum += dt;
                dn++;
              }
              lastArrival = nowUs;
              const int16_t *smp = reinterpret_cast<const int16_t *>(p.pcm);
              size_t cnt = p.pcm_bytes / 2;
              for (size_t i = 0; i < cnt; i++) {
                int32_t v = smp[i] < 0 ? -(int32_t)smp[i] : smp[i];
                if (v > peak) peak = v;
                sumsq += (uint64_t)(smp[i] * smp[i]);
              }
              nsamp += cnt;
              if (headersLogged < 2 && p.header) {
                headersLogged++;
                char hx[100];
                for (int i = 0; i < 32; i++) snprintf(hx + i * 3, 4, "%02x ", p.header[i]);
                log_i("%s: audio datagram header: %s| first PCM bytes: %02x %02x %02x %02x %02x %02x %02x %02x", kTag, hx,
                      p.pcm[0], p.pcm[1], p.pcm[2], p.pcm[3], p.pcm[4], p.pcm[5], p.pcm[6], p.pcm[7]);
              }
              std::lock_guard<std::mutex> g(jbLock);
              if (jb) {
                jb->push(p.pcm, p.pcm_bytes, gap);
                gLinkStats.received = jb->stats().received;
                gLinkStats.lost = jb->stats().lost;
                gLinkStats.overruns = jb->stats().overruns;
              }
            }
          }
        }
      }
      if (FD_ISSET(lis, &rs)) {
        int c = accept(lis, nullptr, nullptr);
        if (c >= 0) {
          if (conn >= 0) close(conn);
          conn = c;
          lastBeatMs = millis();
          log_i("%s: source opened the heartbeat connection", kTag);
        }
      }
      if (conn >= 0 && FD_ISSET(conn, &rs)) {
        char hb[16];
        int n = recv(conn, hb, sizeof hb, 0);
        if (n <= 0) {
          close(conn);
          conn = -1;
          log_i("%s: heartbeat connection closed", kTag);
        } else lastBeatMs = millis();
      }
    }
    if (millis() - lastLog > 5000) {
      lastLog = millis();
      log_i("%s: signal peak %d rms %d (of 32767); arrivals min/avg/max %u.%u/%u.%u/%u.%u ms", kTag, (int)peak,
            nsamp ? (int)sqrtf((float)((double)sumsq / (double)nsamp)) : 0, dn ? dmin / 10 : 0, dn ? dmin % 10 : 0,
            dn ? (dsum / dn) / 10 : 0, dn ? (dsum / dn) % 10 : 0, dmax / 10, dmax % 10);
      peak = 0; sumsq = 0; nsamp = 0; dmin = 0xFFFFFFFF; dmax = 0; dn = 0; dsum = 0;
      log_i("%s: fill %d ms, rx %u, lost %u, underruns %u, overruns %u, corrections %u, markers %u, bad %u, heap %u (max block %u)", kTag,
            gLinkStats.fill_ms.load(), (unsigned)gLinkStats.received.load(), (unsigned)gLinkStats.lost.load(),
            (unsigned)gLinkStats.underruns.load(), (unsigned)gLinkStats.overruns.load(), (unsigned)gLinkStats.corrections.load(), (unsigned)gLinkStats.markers.load(), (unsigned)gLinkStats.bad.load(),
            (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
  }
  if (udp >= 0) close(udp);
  if (lis >= 0) close(lis);
  if (conn >= 0) close(conn);
  fpsTaskAlive = false;
  vTaskDelete(nullptr);
}

size_t freeHeap() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

// Allocated once, at boot, while the heap is still in one piece: block by block, as many as
// the Buffer setting asks for and the free heap allows beside a reserve for everything else.
void allocateBuffer() {
  int ms = gsGetInt("audio", "bufms");
  size_t want = (size_t)((ms * 44100 / 1000 + fps::kFramesPerDatagram - 1) / fps::kFramesPerDatagram);
  // Measured (findings.md 4.3): the stream's own plumbing (Wi-Fi RX buffers, IP reassembly, the
  // UDP queue, I2S DMA, two task stacks) takes ~55 KB on top of an idle system, and a
  // session's stanza handling a little more. 36 KB left over ran out of memory mid-handshake.
  constexpr size_t kReserve = 88 * 1024;
  std::lock_guard<std::mutex> g(jbLock);
  jb = new JitterBuffer(want, 0.5f, freeHeap, kReserve);
  gLinkStats.buffer_ms = (int)(jb->capacity() * 1062 * 1000 / 44100);
  log_i("%s: jitter buffer %u of %u blocks = %u ms (%u bytes); heap now free %u, largest block %u", kTag,
        (unsigned)jb->capacity(), (unsigned)want, (unsigned)(jb->capacity() * 1062 * 1000 / 44100),
        (unsigned)(jb->capacity() * fps::kPayloadBytes), (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool fpsOpen() {
  if (fpsRun) return true;
  {
    std::lock_guard<std::mutex> g(jbLock);
    if (!jb || jb->capacity() < 2) { log_e("%s: no jitter buffer", kTag); return false; }
    jb->reset();
  }
  log_i("%s: stream starts, buffer %u ms", kTag, (unsigned)(jb->capacity() * 1062 * 1000 / 44100));
  gLinkStats.received = gLinkStats.lost = gLinkStats.underruns = gLinkStats.overruns = 0;
  gLinkStats.markers = gLinkStats.corrections = gLinkStats.bad = 0;
  badLogged = 0;
  gSourceMuted = false;
  gFlush = false;
  fpsRun = true;
  xTaskCreatePinnedToCore(fpsTask, "fps", 4096, nullptr, 6, nullptr, 0);
  gLinkStats.listening = true;
  return true;
}

void fpsClose() {
  if (!fpsRun) return;
  gFlush = true; // fade out what is queued instead of letting it play on or cutting it
  fpsRun = false;
  for (int i = 0; i < 40 && fpsTaskAlive; i++) vTaskDelay(pdMS_TO_TICKS(25));
  gLinkStats.listening = false;
  gLinkStats.fill_ms = 0;
}

// ---- beocore sessions ----------------------------------------------------------------------

class Env : public LinkEnv, public VolumeProvider {
public:
  struct Conn {
    int fd = -1;
    int id = 0;
    bool open() const { return fd >= 0; }
  };
  Conn conns[kMaxSessions];
  std::vector<int> toClose;
  std::string pendingRequest; // the app's join request id, answered when the stream runs
  std::unique_ptr<NetworkLinkRx> rx;

  void send(int sid, const std::string &b) override {
    log_i("%s: >> [%d] %.80s", kTag, sid, b.c_str());
    for (Conn &c : conns)
      if (c.id == sid && c.open()) {
        size_t off = 0;
        while (off < b.size()) {
          int n = ::send(c.fd, b.data() + off, b.size() - off, 0);
          if (n <= 0) { toClose.push_back(sid); return; }
          off += (size_t)n;
        }
      }
  }
  void close_session(int sid) override { toClose.push_back(sid); }
  bool fps_open() override { return fpsOpen(); }
  void fps_close() override { fpsClose(); }
  std::string our_ip() override { return WiFi.localIP().toString().c_str(); }
  void log(const std::string &l) override { log_i("%s: [heap %u] %.300s", kTag, (unsigned)esp_get_free_heap_size(), l.c_str()); }

  void started(const std::string &key, const std::string &jid) override {
    log_i("%s: listening to %s from %s", kTag, key.c_str(), jid.c_str());
    sourceJid = jid;
    std::lock_guard<std::recursive_mutex> g(*gLock);
    if (!pendingRequest.empty()) {
      gState->join_result(jid, pendingRequest, true);
      pendingRequest.clear();
    }
    gState->following(jid);
  }
  void stopped() override {
    log_i("%s: no longer listening", kTag);
    std::lock_guard<std::recursive_mutex> g(*gLock);
    gState->left();
    gState->set_play("stopped");
  }
  void refused(const std::string &why) override {
    log_e("%s: refused: %s", kTag, why.c_str());
    pendingRequest.clear();
  }
  void notification(const std::string &kind, const std::string &) override {
    static std::string seen;
    if (seen.find("|" + kind + "|") == std::string::npos) {
      seen += "|" + kind + "|";
      log_i("%s: first %s notification from the source", kTag, kind.c_str());
    }
  }
  LinkVolume link_volume() override {
    LinkVolume v;
    v.level = gLevel.load();
    v.maximum = gMax.load();
    v.muted = gMuted.load();
    return v;
  }
};

Env env;
std::mutex reqLock;
std::string reqJoinIp, reqJoinJid, reqJoinId;
std::atomic<bool> reqLeave{false};

class Port : public LinkPort {
public:
  bool join(const std::string &jid, const std::string &rid) override {
    std::string ip;
    for (const Peer &p : gNet->peers())
      if (p.jid == jid) ip = p.ip;
    if (ip.empty()) {
      log_w("%s: join %s: nothing by that jid has been heard on the network", kTag, jid.c_str());
      return false;
    }
    log_i("%s: join %s requested (%s)", kTag, jid.c_str(), ip.c_str());
    std::lock_guard<std::mutex> g(reqLock);
    reqJoinIp = ip;
    reqJoinJid = jid;
    reqJoinId = rid;
    return true;
  }
  void leave() override { reqLeave = true; }
} port;

// A slot for a new session: a free one, else the oldest that is not carrying the stream (real
// products open a session to us as soon as they see us and keep it idle; two slots fill at once).
Env::Conn *pickSlot(const std::function<void(int)> &drop) {
  for (auto &c : env.conns)
    if (!c.open()) return &c;
  Env::Conn *oldest = nullptr;
  for (auto &c : env.conns)
    if (c.id != env.rx->current_session() && (!oldest || c.id < oldest->id)) oldest = &c;
  if (oldest) drop(oldest->id);
  return oldest;
}

void linkTask(void *) {
  env.rx.reset(new NetworkLinkRx(gIdentity, env, env));
  int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  int one = 1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(5222);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(lfd, (sockaddr *)&a, sizeof a) != 0 || listen(lfd, 2) != 0) {
    log_e("%s: could not listen on 5222", kTag);
    vTaskDelete(nullptr);
  }
  log_i("%s: beocore listening on 5222; heap free %u, largest block %u", kTag, (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  int nextId = 1;
  auto drop = [&](int sid) {
    for (auto &c : env.conns)
      if (c.id == sid && c.open()) { close(c.fd); c.fd = -1; env.rx->session_closed(sid); c.id = 0; }
  };
  char buf[1500];
  uint32_t lastHeap = millis();
  for (;;) {
    if (millis() - lastHeap > 10000) {
      lastHeap = millis();
      log_i("%s: heap free %u, largest block %u, min ever %u; wifi status %d rssi %d dBm ch %d txpower %d (0.25 dBm units) bssid %s ip %s", kTag,
            (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
            (unsigned)esp_get_minimum_free_heap_size(), (int)WiFi.status(), (int)WiFi.RSSI(), (int)WiFi.channel(), (int)WiFi.getTxPower(), WiFi.BSSIDstr().c_str(), WiFi.localIP().toString().c_str());
    }
    // requests from the app side
    if (reqLeave.exchange(false)) env.rx->leave();
    std::string ip, jid, rid;
    {
      std::lock_guard<std::mutex> g(reqLock);
      ip.swap(reqJoinIp); jid.swap(reqJoinJid); rid.swap(reqJoinId);
    }
    if (!ip.empty()) {
      int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
      sockaddr_in d = {};
      d.sin_family = AF_INET;
      d.sin_port = htons(5222);
      inet_pton(AF_INET, ip.c_str(), &d.sin_addr);
      timeval to = {4, 0};
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
      if (connect(fd, (sockaddr *)&d, sizeof d) == 0) {
        Env::Conn *slot = pickSlot(drop);
        if (slot) {
          slot->fd = fd; slot->id = nextId++;
          log_i("%s: dialled %s for join (session %d)", kTag, ip.c_str(), slot->id);
          env.pendingRequest = rid;
          env.rx->session_opened(slot->id, true);
          env.rx->start_join(slot->id, jid);
        } else { log_w("%s: no free session slot for the join", kTag); close(fd); }
      } else {
        log_w("%s: could not open a session to %s", kTag, ip.c_str());
        close(fd);
      }
    }

    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(lfd, &rs);
    int maxfd = lfd;
    for (auto &c : env.conns)
      if (c.open()) { FD_SET(c.fd, &rs); if (c.fd > maxfd) maxfd = c.fd; }
    timeval tv = {0, 100000};
    if (select(maxfd + 1, &rs, nullptr, nullptr, &tv) > 0) {
      if (FD_ISSET(lfd, &rs)) {
        sockaddr_in pa;
        socklen_t pl = sizeof pa;
        int fd = accept(lfd, (sockaddr *)&pa, &pl);
        if (fd >= 0) {
          Env::Conn *slot = pickSlot(drop);
          if (!slot) { close(fd); continue; }
          slot->fd = fd; slot->id = nextId++;
          char ipb[16];
          inet_ntop(AF_INET, &pa.sin_addr, ipb, sizeof ipb);
          log_i("%s: %s opened a session to us", kTag, ipb);
          env.rx->session_opened(slot->id, false);
        }
      }
      for (auto &c : env.conns) {
        if (c.open() && FD_ISSET(c.fd, &rs)) {
          int n = recv(c.fd, buf, sizeof buf, 0);
          if (n <= 0) drop(c.id);
          else {
            try {
              env.rx->session_data(c.id, buf, (size_t)n);
            } catch (const std::bad_alloc &) {
              log_e("%s: out of memory handling a stanza; dropping the session", kTag);
              drop(c.id);
            }
          }
        }
      }
    }
    while (!env.toClose.empty()) {
      int sid = env.toClose.back();
      env.toClose.pop_back();
      drop(sid);
    }
  }
}

} // namespace

LinkPort &linkPort() { return port; }

void linkTestTone() {
  log_i("%s: test tone for 5 s on BCK/LRCK/DATA", kTag);
  toneUntilMs = millis() + 5000;
}

void linkSetVolume(int level, int maximum, bool muted) {
  gLevel = level;
  gMax = maximum;
  gMuted = muted;
}

void linkStart(PlayerState *state, std::recursive_mutex *lock, Net *net) {
  allocateBuffer();
  gState = state;
  gLock = lock;
  gNet = net;
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 7, nullptr, 1);
  xTaskCreatePinnedToCore(linkTask, "beocore", 6144, nullptr, 3, nullptr, 0);
  if (gsGetBool("audio", "listen")) {
    log_i("%s: debug: always listening on FPS port 1234", kTag);
    fpsOpen();
  }
}
