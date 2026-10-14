# ESP32 network-speaker port: Phase 0 findings

Status: **Phase 0, analysis only.** No firmware exists yet. Nothing in this
document has been run on an ESP32, on a B&O product or against the B&O app.

This project is unofficial and not affiliated with, endorsed by, or supported
by Bang & Olufsen. Behaviour described here is derived from
[djerik/beo](https://github.com/djerik/beo) (Apache-2.0, © Tobias Laursen),
which reverse-engineered it. Beo, Mozart, NetworkLink, BeoLink and Bang &
Olufsen are trademarks of Bang & Olufsen, used only to name protocols.

Files read (local checkouts): `beo/` (README, docs/architecture, app-protocol,
product-matrix, configuration, local-testing, getting-started, mozart
identity.md; all of `packages/networklink`, `packages/mozart`,
`packages/product`; `apps/beo-adapter`: main, listening, speaker, streaming,
machines, golden answers), and `garnet_web` / `garnet_settings` (README,
CLAUDE.md, headers, `gw_server`, `gw_net`, `gw_core`, examples). The beo tree
is untouched.

## How to read the evidence tags

Every claim carries one tag.

| Tag | Meaning |
|---|---|
| **[read]** | Read in beo's code or docs, or in garnet's code. This says what *the Python code does*, not that it works against real equipment. |
| **[beo-verified]** | beo's own docs say it was measured on real equipment. I have not re-checked it. |
| **[measured]** | I measured it here (a host-side `pio run` of a throwaway garnet_web build, or an `sdkconfig` of the installed core). Nothing measured on hardware. |
| **[inferred]** | My reasoning from the above. Could be wrong. |
| **[unknown]** | Neither the code nor I can say. Listed again in §7. |

The most important caveat, stated once: beo's **listener** side (another
product plays to the Pi) is, in beo's own words, "built and not yet verified
end to end" (README "What is verified", architecture.md use case 6). There is
no captured stream from a *real* B&O source in the repo. What the Python code
knows about a source's behaviour towards a listener is what its own
`SourceRole` sends, which was modelled on real *listeners*. So everything in §3
to §5 about "what the line-in source will do to the ESP32" is **[inferred]**
until `tools/` captures it.

---

## 0. Summary of what matters

1. **The stream is, as far as the library knows, uncompressed 16-bit PCM**,
   44.1 kHz stereo, 1062 frames (4248 bytes) per UDP datagram after a 32-byte
   header **[read]**. No codec is needed. But this is the format the library
   *sends*; a real source's format has never been captured **[unknown]**, and
   the line-in source is catalogued as `PROPRIETARY` content protection (§3.6,
   §4). That is the biggest functional risk for the record-player use case.
2. **A datagram is 4280 bytes, so every one is three IP fragments.** The
   precompiled lwIP in the pinned Arduino core has IPv4 reassembly **switched
   off** **[measured]**: `CONFIG_LWIP_IP4_REASSEMBLY` is not set, so lwip's
   `IP_REASSEMBLY` is 0 (`lwipopts.h`). An ESP32 built the way garnet_web is
   built would silently receive nothing. The fix is a custom sdkconfig through
   pioarduino's `custom_sdkconfig` (hybrid compile). Same framework, same
   platform, but it is a build-system change I have **not** tried yet.
3. **A 1 s jitter buffer does not fit** on a plain ESP32 next to Wi-Fi,
   mDNS, two HTTP servers and garnet_web: 1 s is 172 KB of PCM in a chip with
   ~270 KB of heap-capable DRAM before the Wi-Fi stack takes its share. The
   realistic budget is **~0.35 to 0.5 s** (§4.3, partly **[inferred]**). The
   1.1 s in the protocol is a *timestamp lead*, not a buffering requirement
   (§4.5).
4. **Port 80 collides with garnet_web in three ways** (port, `GET /`, and the
   `/api/*` wildcard that would swallow `/api/v1/*`). It can be solved with a
   small, upstreamable garnet_web change (§6.2).
5. **To be offered as a listener at all, the product probably has to be
   enrolled in the network's shared configuration** (an XMPP `Insert` to an
   elected leader), and the Python code reads the *whole* configuration
   document first, which may be too large for a PSRAM-less ESP32 (§3.5).
6. garnet_web's baseline (Wi-Fi + OTA only) already takes **81.5 % of the
   1.25 MB OTA slot** of `default.csv` **[measured]**; the product needs a
   bigger partition layout.

---

## 1. mDNS

Two families of records, from two different places in beo. The first is what
the *app* needs, the second is what *other products* need to find a listener.
`facade.py` registers only the first; `apps/beo-adapter/listening.py` +
`discovery.Advertiser` register the second **[read]**.

### 1.1 For the B&O app (Mozart), `facade.run()`

| | |
|---|---|
| Service type | `_bangolufsen._tcp.local.` (the app browses exactly `_beoremote`, `_beo_settings`, `_bangolufsen`, `_zenith`; Mozart products use `_bangolufsen`; `_mozart._tcp` is never browsed) **[read, beo-verified]** |
| Instance name | the product's friendly name, e.g. `Beolab 8 (emulated)` **[read]** |
| Port | 80 **[read]** |
| Host | `<prefix>-<serial>.local.` e.g. `Beolab-8-99012345.local.`; prefix = display name with spaces → hyphens, or the `--name` with non-alphanumerics → `-` **[read]**. One host name for *every* record of the product (two host names made one product look like two) |
| A / SRV TTL, TXT TTL | zeroconf defaults, 120 s / 4500 s **[read]** |

TXT (all values are strings) **[read]**:

```
tn=3150            type number; must be a type the app knows
in=1200322         item number (1454530 is the Classics-Adapter gate only)
sn=99012345        serial (8 digits)
fn=Beolab 8 (emulated)    friendly name (same as instance name)
fv=1.9.0.30523     firmware: 2 to 4 numeric parts only; a hash suffix crashes the app
wa=AA:BB:CC:DD:EE:FF      MAC, upper case, colons
```

Behaviour at start/stop **[read]**:
- On SIGTERM the Python code sends an mDNS goodbye (TTL 0) first, because the
  app only restarts its WebSocket on a Bonjour "found" and a found needs a
  "lost" before it.
- app-protocol.md §1 says a starting product sends a goodbye for its own name
  *before* announcing. The **code does not do that**; it registers with
  `cooperating_responders=True` (skips the conflict probe). The doc and the
  code disagree. An ESP32 that crashes or loses power sends no goodbye, so
  after a reboot the app may keep the old record. **[unknown]** how the app
  reacts to a re-announcement with no prior goodbye.

### 1.2 For other products (the NetworkLink plane), `discovery.Advertiser`

Four records on the same host name, same IP **[read]**:

| Type | Instance | Port | TXT |
|---|---|---|---|
| `_beocore._tcp.local.` | the **jid** (one DNS label, "invariant 13") | 5222 | `hash=sha-1`, `jid=<jid>`, `name=<name>`, `node=beocore`, `txtvers=1`, `ver=dSsQBJenapvOryzvtU90IM7SI+o=` (a constant capability hash shared by every product) |
| `_beoremote._tcp.local.` | the name | **8080** | `assoc=` (empty), `item=<item>`, `jid=<jid>`, `mac=<MAC>`, `name=<name>`, `productType=<…>`, `services=BeoInput,BeoNotify,BeoZone,BeoDevice,BeoContent,BeoHome,BeoSecurity`, `type=<type>` |
| `_http._tcp.local.` | the name | 80 | none |
| `_beo_settings._tcp.local.` | the name | 80 | none |

The jid is `<type>.<item>.<serial>@products.bang-olufsen.com`. A source finds
listeners by these records ("a source offers as listeners only the products it
has found on the product-to-product plane") **[read]**; the facade's own
`PeerDirectory` browses `_beocore`, `_beoremote`, `_bangolufsen`.

Open points on the records:
- **`productType` in `_beoremote`.** `apps/beo-adapter` passes the Mozart type
  *name* (`SEOUL` for Beolab 8). app-protocol.md §2 reports the opposite
  measurement for the *source* role: a listening M3 only opened the
  connection when we announced its own hardware code (`CA17`), and "the name of
  our own type makes it ignore us entirely". Which holds for a real source
  contacting an ESP32 as listener is **[unknown]**; beo's listener path uses
  the type name and is unverified.
- Whether the **`_beo_settings`** and **`_http`** records matter to a source is
  **[unknown]**. The app browses `_beo_settings`, but the Mozart product is
  found through `_bangolufsen` **[read]**.

### 1.3 Serial, identity

`beo/packages/mozart/docs/identity.md`, `product/identity.py` **[read]**:
- jid = `<type>.<item>.<serial>`; only **3150.1454530.99xxxxxx** (Beolab 8
  carrying the Classics Adapter item) is verified against the real app
  **[beo-verified]**.
- Serial = `"99" + "%06d" % (int(sha256(seed).hexdigest()[:12], 16) % 1_000_000)`.
  Band 99 000 000 to 99 999 999 sits above every serial anyone has seen. Never
  random: the jid is the app's primary key. Precedence: `--serial`,
  `BEO_SERIAL`, `/etc/beo/serial`, derived from `/etc/machine-id`, else MAC.
- **ESP32:** seed = the efuse base MAC string (garnet_web already reads it with
  `esp_read_mac`), same formula, optionally stored in NVS. **[inferred]**; the
  Python formula is **[read]** and trivially portable (mbedTLS SHA-256).
- MAC: one MAC for every record. The facade reports the machine's own.

---

## 2. Mozart API

Port **80** (REST), port **8080** (liveness ping and, by beo's choice, the same
handler as 80), port **9339** (WebSocket). The app requests
`http://{ip}/...` with no port, and `http://{ip}:8080/?` as a ping; no answer
means "offline" **[read, beo-verified]**.

### 2.1 Which product profile to imitate

- The only verified `(type, item)` pair is Beolab 8, **3150**, with item
  `1454530` and a `ClassicsAdapterProfile` **[beo-verified]**. The type number
  is what decides the picture and settings pages; `in=1454530` only triggers
  the Classics-Adapter check, which a speaker does not want.
- The facade's default **plain speaker** is `ProductProfile` with item
  `1200322` (a real Beolab 8's), firmware `1.9.0.30523`, volume max 100, and an
  empty remote menu **[read]**. **That profile has never been recorded against
  the real app**: the golden answers (`apps/beo-adapter/tests/golden`) are all
  taken with `ClassicsAdapterProfile`, and `test_machines.py` only asserts
  that a no-machine product *is* a `ProductProfile`. The
  Mac run in `local-testing.md` was `--machine bs9000`, not a plain speaker.
- Worse, reading `facade.current_source()` for a plain speaker: when power is
  "on", `plain_source(None)` is `None`, so it falls through to the disc branch
  and reports `classicsAdapter` / "CD 1" with an empty URI prefix; and
  `GET api/v1/content/active` would index an empty dict. So the plain-speaker
  "on" state is **[inferred] broken in the Python facade**, not a model to
  port. The ESP32 must define its own "on, line-in-style source" state.
- **Recommendation: type 3150 (Beolab 8), item 1200322**, using the
  Classics-Adapter profile's *response shapes* (which are verified) with the
  classics source removed. This is a choice for you (§8). Choosing any other
  type is an extrapolation per product-matrix.md (ten of eleven types are
  untried).

### 2.2 Minimal REST set the app requires

Rules that apply to everything **[read, beo-verified]**:
- Unmodelled endpoints answer **404 with `{}`**, never an empty 200 (an empty
  200 is read as an object with every field missing, and crashed the app).
- One success code per endpoint (below). `204` has no body and no
  `Content-Length: 2`.
- Types are exact; a wrong type is as fatal as a missing field.
- HTTP/1.1 keep-alive, `Content-Type: application/json`, Nagle off.

GET routes that return data (Python `GET_ROUTES`). Abridged shapes follow
the golden files:

| Route | Answer |
|---|---|
| `/api/v1/product/info` | full tree, §2.3. Every field is dereferenced unchecked |
| `/api/v1/remote` | dict keyed by lower-case hyphenated GUID, see below |
| `/api/v1/playback/sources` | `{"items":[ MozartSource… ]}` |
| `/api/v1/playback/sources/active` | one MozartSource |
| `/api/v1/playback/state` | `{state, source, progress, metadata}` |
| `/api/v1/state` | `{"power":{"value":"on"},"volume":{…}}` |
| `/api/v1/state/power` | `{"value":"on"}` (`networkStandby`, `on`, `standby`, `shutdown`, `storage`) |
| `/api/v1/sound/volume` (+`/level`, `/mute`, `/settings`) | volume state |
| `/api/v1/beolink/self` | `{"jid":…, "friendlyName":…}` |
| `/api/v1/beolink/enabled` | `{"value":true}` |
| `/api/v1/beolink/listeners` | `[]` or `[{"jid":…,"audioTransport":"wireless"}]` |
| `/api/v1/beolink/available-listeners` | `[{"jid":…}]`, only `jid`, every peer including active ones |
| `/api/v1/beolink/audioleader` | `{"jid","friendlyName","audioTransport"}` or **204** |
| `/api/v1/beolink/peers` | non-Mozart extension, not needed by the app |
| `/api/v1/network`, `softwareupdate`, `softwareupdate/settings`, `setup/linein`, `setup/presets` (`{}`), `setup/enableusagereport`, `proximity/settings`, `cast/tos-accepted`, `cast/version`, `speakerlink/role`, `speakerlink/association` (`type: standalone`), `playback/queue`, `playback/queue/settings`, `content-triggers`, `hdmi`, `scenes`, `alarms`, `timers`, `sound/features`, `sound/settings/adjustments`, `sound/settings/toneTouch` | small constant shapes (see golden files) |
| `sound/listening-modes/active`, `stand/info`, `stand/endstops`, `stand/status` | **404** (feature absent) |

`GET /api/v1/remote` for the product (the source list comes from here, not
`playback/sources`). Root label must be `MUSIC`; ids are GUIDs; the dict key
equals the item's id; children are reached by `firstChildMenuItemId` /
`nextSiblingMenuItemId` **[read, beo-verified]**:

```json
{
  "b0000000-0000-4000-8000-000000000001": {
    "id": "b0000000-0000-4000-8000-000000000001", "label": "MUSIC",
    "available": true, "fixed": true, "disabled": false, "dynamicList": "none",
    "content": {"contentUri": "", "label": "MUSIC", "categories": ["music"],
                "source": {"value": "lineIn"}},
    "firstChildMenuItemId": "b0000000-0000-4000-8000-000000000003"
  },
  "b0000000-0000-4000-8000-000000000003": {
    "id": "b0000000-0000-4000-8000-000000000003", "label": "Line-In",
    "available": true, "fixed": false, "disabled": false, "dynamicList": "none",
    "parentMenuItemId": "b0000000-0000-4000-8000-000000000001",
    "content": {"contentUri": "lineIn", "label": "Line-In", "categories": ["music"],
                "source": {"value": "lineIn"}},
    "actionList": [{"type": "triggerContent", "contentId": "lineIn"}]
  }
}
```

The golden file has a root and two children (`classicsAdapter`, `lineIn`
labelled "Radio"); the one-child form above is my removal of the classics item,
**[inferred]**. The Python plain speaker has *only* the root, which
would give an empty source list.

`playback/sources` item **[read]**:

```json
{"id":"linein:3150.1200322.99012345@products.bang-olufsen.com","name":"Line-In",
 "type":{"value":"lineIn"},"isEnabled":true,"isPlayable":true,
 "isMultiroomAvailable":true,"isSeekable":false}
```

`type` is an object with `value`, not a string. Volume **[read]**
(0..100 scale, followers scale against `maximum`; a `range` field makes the
app double the level):

```json
{"level":{"level":30},"maximum":{"level":100},"default":{"level":30},"muted":{"muted":false}}
```

Write routes **[read]**:

| Route | Behaviour / success code |
|---|---|
| `PUT/POST api/v1/sound/volume/level` body `{"level":N}` | set, answer **201** + volume state; WS `WebSocketEventVolume` |
| `…/volume/mute` body `{"muted":bool}` | **201** |
| `…/volume/settings` `{"default":{"level":N},"maximum":{"level":N}}` | **201**, persisted default |
| `PUT api/v1/state/standby` (empty body) | the power button; 200 `{}` |
| `PUT api/v1/state/power` `{"value":"on"}` | wake; 200 `{}` |
| `POST api/v1/playback/command/{play,pause,stop,skip,prev}` | exactly those five; anything else **404** |
| `POST api/v1/beolink/expand/{jid}` | the app tells *us* (as source) to add a listener. **202** |
| `POST api/v1/beolink/unexpand/{jid}` | **202** + `WebSocketEventNotification{"value":"beolinkListeners"}` |
| `POST api/v1/beolink/join/{jid}` | the app tells *us* to join `{jid}`. **202**, body `{"jid":…,"requestID":<guid>,"result":"/api/v1/beolink/join/result/<guid>"}` |
| `POST api/v1/beolink/leave` | **202** + the listeners-changed notification |
| `PUT api/v1/remote/{id}` `{"label":…}` | persist label, 200 |

For the ESP32 only `join`, `leave`, power, volume/mute and the read routes are
live. `expand`/`unexpand` are source-side and must still answer 202/404
sensibly, since the app sends them to *the source product*, not to us
**[inferred]**.

### 2.3 `GET api/v1/product/info` (abridged, from the golden file)

```json
{
  "apple": {"homeApp": false},
  "bootloader": {"version": "1.9.0.30523"},
  "componentVersions": {"items": [{"component": "SW", "version": "1.9.0.30523"}]},
  "encryptionInfo": {"algorithm": "", "keyLength": 0, "publicKey": ""},
  "fep": {"version": "1.9.0.30523"},
  "name": {"friendlyName": "ESP32 Headphones"},
  "pcbRevisions": {"items": []},
  "powerDelivery": {"version": "1.0"},
  "production": {
    "hardwareRevision": "1", "itemNumber": "1200322",
    "macAddresses": {"macAddresses": [
      {"name": "ethernet", "mac": "AA:BB:CC:DD:EE:FF"},
      {"name": "wlansta",  "mac": "AA:BB:CC:DD:EE:FF"}]},
    "productDefaultName": "Beolab 8", "productionSite": "emulator",
    "productionTimestamp": "2024-01-01T00:00:00Z", "serialNumber": "99012345",
    "typeNumber": "3150", "wifiCountryCode": "DK"},
  "touch": {"version": "1.9.0.30523"},
  "tv": {"edid": {"name": "", "year": 0},
         "lge": {"country": "", "countryGroup": "", "deviceId": "",
                 "firmwareVersion": "", "language": "", "screenSize": 0,
                 "webOsDialect": ""}}
}
```

Required: parseable `productionTimestamp`; macAddresses entries whose `name`
contains `ethernet` and `wlansta`; the `tv` subtree present **[read]**.

### 2.4 `playback/state` and WebSocket

`playback/state` **[read]** has `state.value`, a full MozartSource under
`source`, `progress {id, progress, totalDuration}` and a `metadata` object.
The metadata object in the golden file is a long fixed shape (art, bitdepth,
`remoteLeader`, `queueId` …). For the ESP32 I would copy the shape and fill
`remoteLeader {audioTransport, friendlyName, jid}` with the source we follow.
That is a guess **[inferred]**; beo only fills it with empty strings.

WebSocket on **ws://{ip}:9339**, plain RFC 6455, no sub-protocol, text frames
only, envelope **[read]**:

```json
{"eventType": "WebSocketEventVolume",
 "eventData": {"level":{"level":30},"maximum":{"level":100},
               "default":{"level":30},"muted":{"muted":false}}}
```

- The server **must answer pings with pongs** (the app drops the socket
  otherwise), and the app closes it without them.
- **On connect, push a snapshot** in this order: `WebSocketEventPowerState`
  `{"value":…}`, `WebSocketEventVolume`, `WebSocketEventSourceChange`
  (MozartSource), `WebSocketEventPlaybackSource`, `WebSocketEventPlaybackState`
  `{"value":"started"|"paused"|"stopped"|"idle"}`, `WebSocketEventPlaybackMetadata`,
  `WebSocketEventPlaybackProgress {"id","progress","totalDuration"}`, then one
  `WebSocketEventBeolinkJoinResult` per listener. The app fetches nothing on
  connect.
- `WebSocketEventBeolinkJoinResult`: `status` (`idle|joined|failed`), `error`
  (`ok|busy|leaderless|alone|silent|missing|rejected|timeout`), `type`
  (`join|joinRequested|expand|expandRequested`), `requestID` (GUID), `jid`.
  **`type` decides behaviour**: `join` makes us a follower of `jid`; `expand`
  adds a listener. For the ESP32 joining a source: `{"jid": <source jid>,
  "status":"joined","type":"join","error":"ok","requestID": <the join
  request's id>}`.
- Send `joined` only when it is true. A *failure* is answered with **silence**;
  the app's own 25 s timer reports it, and a `failed` event breaks the join
  bookkeeping **[read, beo-verified for expand; for join the code does the
  same]**.
- Leaving: `WebSocketEventNotification {"value":"beolinkListeners","code":""}`
  makes the app refetch `beolink/listeners`.
- Event names are one of 24; an invented name is silently dropped.

### 2.5 Legacy BeoZone surfaces

Peers read these, and a product advertising a service it does not deliver is
"not a usable peer" **[read]**: `/` (service index), `/Ping` (200 empty),
`/BeoNotify` and the long-held **`/BeoNotify/Notifications`** stream (line-
delimited JSON, each line starting with `{`, connection held open),
`/BeoDevice` (its `productType` must equal `_beoremote`'s),
`/BeoZone/Zone/Sources`, `/BeoZone/Zone/ActiveSources`, `/BeoZone/System`,
`/BeoZone/System/Products`, `/BeoZone/Zone/{Sound,Stream,List,PlayQueue,…}`,
`/BeoHome`, `/BeoSecurity`, `/BeoContent`. The Python facade serves one
handler on both 80 and 8080 **[read]**; which port peers actually use for
which path is **[unknown]**. `_beoremote` announces 8080 **[read]**, so peers
probably use 8080 for these **[inferred]**.

**ESP32 consequence:** the `/BeoNotify/Notifications` handler never returns.
`esp_http_server` serves requests on **one task**, so a held-open handler
would block everything else on that instance. It needs an async handler
(`httpd_req_async_handler_begin`), a dedicated instance, or dropping `BeoNotify`
from the announced `services`, and whether a source tolerates the latter is
**[unknown]** (beo's notes say a product polled the 404 "several times a
second", so it does look for it).

---

## 3. NetworkLink

### 3.1 Planes and ports **[read]**

| Plane | Port | Transport | Cast |
|---|---|---|---|
| beocore control (XMPP-like) | **5222** | TCP | unicast. Either side may dial; both are served alike |
| FPS audio | **1234** | UDP (audio + 16-byte markers) | **unicast** to the address in the Connect |
| FPS heartbeat | **1234** | TCP, source→us, one `0x0a` byte per second | unicast |
| PTP | 3319/3320 | UDP | multicast 224.0.0.107 (not needed by us, §4.5) |
| NSDK (logs) | 80 | HTTP | unicast, diagnostics only |
| mDNS | 5353 | multicast | |
| Shared configuration leader | 5222 | TCP | found via `_beoconfig._tcp` |

There is no multicast audio. The audio stream goes to whatever the listener
puts in `<connectionInfo>FPS://<ip></connectionInfo>` **[read]**.

### 3.2 beocore framing **[read]**

XMPP-flavoured but not a real XMPP server:
- Stream header with no `<?xml?>` declaration, single-quoted by the code and
  rewritten to double quotes on the wire (`double_quotes()`): "every stanza
  from a product" uses double quotes.
- `<stream:stream to='products.bang-olufsen.com' from='<our full jid>'
  xmlns='jabber:client' xmlns:stream='http://etherx.jabber.org/streams'
  version='1.0'>`; no whitespace keepalives; connection lives while idle.
- When *the peer* dialled us, we do **not** send our header at accept; it goes
  out when the peer's own header has been read, with `to=<peer jid from its
  header>`, followed immediately by our own `disco#info` `get`.
- Every iq we originate has a **decimal** id from one rising counter; the Python
  source uses 51900.. on a per-listener counter and fixed ids 50900, 51002,
  50909-50913, 51003 for specific requests.
- Notifications are `<message><body><_notification xmlns='beocore.notifications.1'>
  <NOTIFICATION>…</NOTIFICATION></_notification></body></message>`, **not** iq.
- `</stream:stream>` is sent before closing the socket. Without it the peer
  keeps state and delivers a stale Disconnect into the next session.
- Answer `disco#info` `get` with identity `category='client' type='product'`
  and features `jid\20escaping, beocore.player.1, beocore.tunnel.1,
  beocore.product.1, beocore.configuration.1, beocore.notifications.1`.
- Anything unhandled of type get/set is answered with `feature-not-implemented`.
- The Python parser is a regex over `<iq…>…</iq>` / `<message…>…</message>`,
  so it needs the whole stanza in the buffer. An ESP32 needs a small
  incremental tokenizer, with a bounded stanza size (see §3.5, and the 10 KB
  `Experiences`/`GetContactInfo` bodies).
- Product limits: "the listener allows two sockets per peer and closes the
  oldest" and "a listener whose goodbye meets a closed socket wakes the source
  by MAC" **[read, from comments]**.

### 3.3 Joining as a renderer, the listener handshake **[read]**

Two initiators exist; the XML is identical after the first message.

**A. The source dials us (the app's "expand" on the source).** Source-side
sequence of `SourceRole.expand()`, which is also what a real source is
*assumed* to do **[inferred]**:

1. Source opens TCP 5222 to us, sends its header. We answer with our header +
   `disco#info get`.
2. Source: `Attach` (set, id 50900) carrying its `contactInfo`. We reply
   `<Attach xmlns='beocore.product.1'>` + **our** `contactInfo` (name, MAC,
   sources, roleCapability).
3. Source: `ConnectRequest` (set, id 51002) with
   `<uniqueSourceId><source>KEY</source><jid>SOURCE_JID</jid></uniqueSourceId>`.
   We reply `<ConnectRequest xmlns='beocore.player.1'><connected>1</connected></ConnectRequest>`.
4. **We bind UDP+TCP 1234 first**, then send our `Connect` request:
   ```xml
   <Connect xmlns='beocore.player.1'>
     <sourceId>KEY</sourceId>
     <connectionInfo>FPS://OUR.IP</connectionInfo>
     <contentProtectionInfo>&lt;?xml version="1.0" encoding="utf-8"?&gt;\n&lt;CpCapabilities&gt;&lt;ProtectionSchemes&gt;&lt;Scheme&gt;NONE&lt;/Scheme&gt;&lt;Scheme&gt;DECIMATE&lt;/Scheme&gt;&lt;Scheme&gt;PROPRIETARY&lt;/Scheme&gt;&lt;/ProtectionSchemes&gt;&lt;/CpCapabilities&gt;</contentProtectionInfo>
     <uniqueSourceId><source>KEY</source><jid>SOURCE_JID</jid></uniqueSourceId>
   </Connect>
   ```
   (the line break after the declaration is part of it). The source starts
   streaming **before** it answers, with `<Connect><connectionId>…</connectionId></Connect>`.
   The reply may be an iq error whose `<text>` carries a reason; the Python
   listener logs it ("not enrolled" is how the source says no).
5. We then send `_notification-subscribe` (kinds `device`, `playing`,
   `source`); the source answers with a `FullStatus` (~4 KB: SOURCE,
   NOW_PLAYING_NET_RADIO, PROGRESS_INFORMATION "preparing", VOLUME,
   SOFTWARE_UPDATE_STATE).
6. Meanwhile the source asks us: `_notification-unsubscribe`, then
   `_notification-subscribe` kind `renderer` (we answer with a `FullStatus`
   holding only our VOLUME item), then `GetVolume`, `GetVolumeMuted`,
   `GetVolumeRange`, all sent together; we answer each, longest name first
   (`GetVolumeMuted`/`GetVolumeRange` before `GetVolume`, same prefix):
   - `<GetVolume xmlns='beocore.product.1'><volume>N</volume></GetVolume>`
   - `<GetVolumeMuted …><muted>0|1</muted></GetVolumeMuted>`
   - `<GetVolumeRange …><range><minimum>0</minimum><maximum>90</maximum></range></GetVolumeRange>`
   `Volume` default is level 35, range 0..90 in beo. Our Mozart scale is
   0..100; **the two scales differ** and must be kept consistent.
7. The source pushes its opening (`SOURCE_EXPERIENCE_CHANGED`, `NOW_PLAYING_*`,
   `PROGRESS_INFORMATION preparing 0`, `play 0`), then `PROGRESS_INFORMATION`
   every second. Notifications are `<message>`; we only need to log/count
   them (`ListenerRole._notified`).

**B. We dial the source (the app's `beolink/join/{jid}` on us).**
`ListenerRole.join(ip, jid)` **[read]**: open session to the source, send
`<Experiences xmlns='beocore.player.1'></Experiences>`; read from the reply
the first `<uniqueSourceId><source>K</source><jid>J</jid>`; if none, fail;
else send our `Connect` for it (step 4 onward). A `Connect` for a source we
already follow is skipped, since a second one restarts the stream.

We **do not** send STREAMING_STATUS or any other notification to the source
in beo's `ListenerRole`. The Python source comment says STREAMING_STATUS "is
the only thing it [a real receiver] reports about actually rendering" **[read]**.
Whether a real source requires notifications from a listener, or drops one that
sends none, is **[unknown]**, and also unverified in beo.

### 3.4 Teardown ("removed from an experience") **[read]**

From the source (a real source assumed to match `SourceRole.unexpand`):
1. `_notification-unsubscribe` (iq set, answered).
2. Pushes: `SOURCE_EXPERIENCE_CHANGED` (state `stop`, same `lastUsed`, listener
   list = only us), `NOW_PLAYING_ENDED`, `PROGRESS_INFORMATION` with only
   `<state>stop</state>`, `SOURCE` with empty data.
3. `Disconnect` (iq set, id 51003, `<connectionId>`) → we answer
   `<Disconnect xmlns='beocore.player.1'/>`, close the receiver, then send our
   **own** `Disconnect` and `_notification-unsubscribe` ("the listener's
   farewell", within milliseconds). A source that closes before it arrives
   wakes by MAC and delivers it into the next session.
4. The source stops FPS and sends `</stream:stream>`.

From us (leaving, app `beolink/leave`): send the same `Disconnect` +
unsubscribe, stop the receiver, close. A `Disconnect` that arrives before any
stream started is left over from a previous session: acknowledge and ignore
(source-side rule; the listener side has no such check in beo).

Pause/play in the *source* are signalled by a 16-byte FPS marker, §4.4.

### 3.5 The shared configuration (enrolment)

**A source streams only to a listener it finds in the configuration the
network shares, and offers only those** **[read]**. A listener that is not
enrolled is refused at `Connect` with an error text. `configuration.py`:

- The document is held by an elected leader (it changes "several times a day"),
  announced as **`_beoconfig._tcp.local.`** with a `jid` TXT. Find it by mDNS
  browse.
- The product's entry sits under `Devices#1`, an XML tree of
  `<item name="…"><data>…</data>…</item>`, names ending `#1`, lists with
  `_1`, `_2` suffixes, an empty value as `^emptyValue`=`true`, dots in names
  written `%2E`.
- Enrol = open a session to the leader, wait 2 s, send
  `<Insert xmlns='beocore.configuration.1'><config>…Devices#1…</config><version>1</version></Insert>`;
  accepted if the reply contains `<accepted>1</accepted>`. No revision: the
  leader stamps it. An `Insert` to a non-leader changes nothing.
- The entry carries name, MAC, sources, `ConnectionType#1`
  (`WIRELESS`/`WIRED`), `RenderDelayMs#1` (86 in beo), `Render#1/Fps#1`
  (content protection `NONE`, `DECIMATE`, `PROPRIETARY`, stream version 1),
  `Multiroom#1/SupportedExperiences#1` (`join`, `expand`), role
  `integratedMaster`.
- **Before inserting, beo reads the whole document** with a
  `_notification-subscribe` whose reply is the configuration for the entire
  network, with a **40 s timeout**, to avoid inserting twice. That document has
  an entry per product, each with its source list, so it is probably tens to
  hundreds of kilobytes. **A PSRAM-less ESP32 cannot buffer it**; it would have
  to be streamed through a scanner that looks only for `name="<jid>"`, or the
  Insert has to be sent blind, and whether a duplicate Insert is harmless is
  **[unknown]**. beo's own comment says one cannot rely on writing the cloud-
  managed list locally for the `--airplay` path ("expects membership in the
  speakers' cloud-managed device list, which cannot be written locally"),
  while `configuration.py` does exactly that for NetworkLink. Both statements
  are in the repo; I take the second (code, unverified) as the working
  assumption, but it is **[unknown]** whether a source enrols *us* or ignores
  the local write.
- `BEO_ENROL` is opt-in in beo because it changes a document every product
  holds. The ESP32 needs the same switch (a garnet_settings toggle), off by
  default.

### 3.6 Source catalogue, line-in

The line-in source of a real device is catalogued **[read]** (from
`STANDARD_SOURCES`, which beo models on "what a networked product lists") as:
key `linein`, catalogue identifier `AUX_A`, name "Line-In", `sourceType`
`LINE IN`, `linkable` 1, **`contentProtection` `PROPRIETARY`**. Linkable
means it can be borrowed, which matches the use case. The `PROPRIETARY`
scheme, and what a source does when a listener declares (as beo's Connect does)
all three of `NONE`, `DECIMATE`, `PROPRIETARY`, is a major **[unknown]**: the
stream could be plain PCM, plain PCM decimated, or an encrypted/obfuscated
form. beo's *own* sender sends PCM regardless, and its listener plays blocks
straight to the sound card; but that has only been run against beo's own
sender.

If the real source stamps PROPRIETARY as "we send something we do not
document", this project stops here and says so (your rule: "if the stream is
not PCM, stop"). We cannot tell from the repo. §7 item 1.

---

## 4. Audio

### 4.1 Datagram layout **[read]** (`fps.py`)

Big-endian header, then PCM. Total datagram **4280 bytes**.

| Offset | Size | Field |
|---|---|---|
| 0..1 | 2 | magic `0x1234` |
| 2..3 | 2 | sequence, uint16, +1 per datagram, wraps at 65536. Markers use the same counter |
| 4..7 | 4 | **session field**, constant within a stream, different per session (`00016100`, `00014100`, `0001b100`, …). The **high word is the format**: `0001` = S16LE 44.1 kHz, `0002` = S16LE 48 kHz (from fpsd log lines quoted in the code). The low word is not constant and the receiver "plays the same whatever value it carries" |
| 8..15 | 8 | uint64 ns **PTS** on the network's PTP clock; +24 081 632 ns per datagram at 44.1 kHz, +22 125 000 ns at 48 kHz |
| 16..23 | 8 | `0xFFFFFFFFFFFFFFFF` constant, meaning unknown |
| 24..31 | 8 | int64 ns **stream clock**, same cadence, time since *this* stream began, starts near 0.3 s |
| 32.. | 4248 | PCM |

Audio: **signed 16-bit little-endian, 2 channels interleaved, 1062 frames per
datagram**, no compression, no encryption (as the library sends it)
**[read]**. 1062 frames at 44.1 kHz = 24.0816 ms = 41.5 datagrams/s ≈ 1.41
Mbit/s payload + 0.75 % header; at 48 kHz, 22.125 ms.

**Marker** (control datagram), 16 bytes **[read]**:
`12 34 | seq(2) | counter(2) | 02 00 | 00×8`. Sent once after the first audio
datagram, and again at every pause and every play together with a
`PROGRESS_INFORMATION` notification, counter 2 at start then +1 per event.
beo's reading: "the audio datagrams continue unchanged with music during the
pause, and nothing goes on 1235/1236 or TCP, so it is this datagram that asks
the listener to mute/unmute (MuteAllSinks / UnmuteAllSinks)" **[read; the
mute semantics are beo's inference]**. It is a *toggle* ("only on a real
transition").

Receiver rules **[read]**: magic must match; `len==16 && data[6:8]==0200` is a
marker; otherwise ≥ 32 bytes is audio; a gap is `(seq - prev) & 0xFFFF != 1`.
UDP `recvfrom(8192)`.

**Heartbeat**: the source opens TCP to the same port on us and writes `0x0a`
once per second; we never answer. It lets us notice a vanished source before
the audio dries up **[read]**.

DSCP: all FPS traffic is ToS 0x88 (AF41), set by the source.

### 4.2 Codec feasibility

**There is no codec to decode on the ESP32, so CPU is not an issue.**
Receive is **~176 KB/s** (44100 × 4 B). Wi-Fi 802.11n throughput on a classic ESP32 for this is well within what it
does **[inferred]**; the delicate parts are fragmentation (§4.2.1) and RAM
(§4.3). If the real stream turns out *not* to be PCM, this section is void.

#### 4.2.1 IP fragmentation: blocking issue, **[measured]**

- 4280 B > 1500 MTU: every datagram is 3 fragments (1480 + 1480 + 1320 payload
  bytes) **[inferred from the size]**.
- In the installed `framework-arduinoespressif32-libs` (IDF 5.5.5, Arduino
  3.3.12, the exact release garnet_web pins), both `esp32/sdkconfig` and the
  per-memory-type `sdkconfig.h` files have **no `CONFIG_LWIP_IP4_REASSEMBLY`**;
  `lwipopts.h` maps that to `IP_REASSEMBLY 0`. `CONFIG_LWIP_IP_REASS_MAX_PBUFS`
  is 10. With reassembly off, lwIP discards fragmented packets before UDP sees
  them (lwIP behaviour from its source, not re-read here). So a stock build
  receives **zero** audio.
- Fix: pioarduino supports `custom_sdkconfig` in `platformio.ini`
  (`platform.py` reads it; hybrid compile of the IDF libs). Required entries:
  `CONFIG_LWIP_IP4_REASSEMBLY=y`, `CONFIG_LWIP_IP_REASS_MAX_PBUFS` (≥ 12), and
  likely `CONFIG_LWIP_MAX_SOCKETS`, `CONFIG_LWIP_UDP_RECVMBOX_SIZE`. This keeps
  PlatformIO + pioarduino + Arduino (no second framework), but the first
  build recompiles the IDF libs, needs the IDF tool packages (present under
  `~/.platformio/packages`), and takes much longer. **Not tried yet.** I
  would try it first thing in Phase 2.
- Also note: **one lost fragment loses the whole 24 ms datagram**, and
  reassembly timeouts at 3 s (`IP_REASS_MAXAGE`).

Other lwIP/Wi-Fi defaults that matter **[measured, same files]**:
`CONFIG_LWIP_UDP_RECVMBOX_SIZE=6` (only six datagrams, ~144 ms, may queue
between lwIP and our recv task, so the receiver task must run at high priority
and never block), `CONFIG_LWIP_MAX_SOCKETS=16` (see §6.4), Wi-Fi AMPDU on,
`FREERTOS_HZ=1000`, default CPU 160 MHz (Arduino default, garnet does not
change it), Wi-Fi power save at the Arduino default (modem sleep). **garnet_web
never calls `WiFi.setSleep(false)`** (grep of `gw_net.cpp`), so the app must,
or arrivals will bunch at beacon intervals **[inferred]**. Arduino's loop task
runs on core 1 (`CONFIG_ARDUINO_RUNNING_CORE 1`), where `gwLoop()` lives.

### 4.3 RAM budget for a ~1 s buffer

Hard numbers **[measured]**: a throwaway `pio run` (scratchpad, not part of
this project) of garnet_web with `GARNET_WEB_WIFI` + `GARNET_WEB_OTA` on
`esp32dev`, pioarduino 55.03.312-1:

```
RAM:   15.3% (used 50024 bytes from 327680 bytes)     .dram0.data 24 125 + .dram0.bss 25 896
Flash: 81.5% (used 1068736 bytes from 1310720 bytes)  default.csv OTA slot
IRAM text 84 095
```

Before adding mDNS records, Mozart, XMPP or audio. (No `GARNET_WEB_BT`; with it
Bluedroid is compiled in, and `CONFIG_BT_ENABLED` is set in the shipped libs.)

The runtime numbers below are **[inferred / guessed from experience with
Arduino-ESP32, not measured]**, and are the first thing to measure on
hardware (flash the baseline, read System → "Memory" / "Largest free block",
which garnet_web already shows; no code needed):

| Item | Estimate |
|---|---|
| DRAM for data + heap, classic ESP32 | 320 KB (PlatformIO's "RAM" total) |
| minus static data/bss of baseline | −50 KB → ~270 KB heap at boot |
| Wi-Fi driver + lwIP + tcpip/event tasks (static+dynamic RX/TX buffers, 8 static RX, up to 32 dynamic) | −60 to −90 KB |
| garnet_web httpd (8 KB stack) + sockets, mDNS (4 KB stack), Arduino loop (8 KB), captive DNS idle | −25 to −35 KB |
| Our tasks: Mozart httpd + 8080 + WS (3 × ~6 KB), XMPP (6 KB), FPS rx (4 KB), I2S feeder (4 KB) + buffers (reassembly pbufs ≤ 15 KB, stanza buffers ≤ 12 KB) | −45 to −65 KB |
| **Free for the jitter buffer** | **≈ 60 to 110 KB, and the largest contiguous block is smaller** |

1 s of PCM is **176 400 B**; 1062 frames = 4248 B per block. Therefore:

| Jitter buffer | Bytes | Blocks of 4248 B | Fits? |
|---|---|---|---|
| 1.0 s | 172 KB | 42 | **No** |
| 0.5 s | 86 KB | 21 | borderline, needs the lean task layout above |
| 0.4 s | 69 KB | 17 | probably |
| 0.3 s | 52 KB | 12 | yes |

**Proposal:** a pool of 4248 B blocks (not one large `malloc`: the largest free
block on a fragmented DRAM heap is far below the total), a garnet_settings
Number field "Buffer (ms)" with range 100 to 600 and default 400, and a clamp
at start-up from `heap_caps_get_largest_free_block`/free heap with a log line
saying so. The honest answer to "~1 s" is: **not without PSRAM**. Options if
more is wanted: PSRAM board (ESP32-WROVER; the user said assume none), drop
mono (changes the audio), or µ-law re-encoding in the buffer (halves it, costs
audible quality on a record player; I would not). ESP32-P4 later has PSRAM.

### 4.4 Mute / pause marker

See §4.1. The ESP32 should treat the marker as the source's pause/play
toggle: fade out/in the sink (Phase 3). Whether the toggle can desynchronise
(first marker is also "stream started") is **[unknown]**: beo sends marker #2
once at start without a pause, and the receiver only counts markers
(`marker_counters`).

### 4.5 Timing: what the PTS means for us

- PTS is a time on the products' PTP clock (IEEE 1588v2, domain 0, ports
  3319/3320, group 224.0.0.107). A *real listener* plays a block at its PTS;
  one that is "not valid PTS" (own uptime, or > ~1.1 s ahead) produces silence
  **[read]**. That matters when *we are the source*. As a listener on an ESP32
  we may **ignore the PTS** and play on our own clock (your "no tight multiroom
  sync needed"). No PTP slave is needed **[inferred]**.
- A real product's packets lie **1.11 s ahead** of the listener's PTP now
  (`ptsToPlayMs ≈ 1080..1110`, "its pipeline delay is 1.117 s") **[read, from
  fpsd-log notes in `nsdk.py`]**. Since that offset is constant, the source
  paces at real time and the 1.1 s is a lead, not a burst. Therefore a real
  source does *not* need us to buffer 1 s **[inferred]**. Contradicting note:
  `AlsaPlayback` says each block is "about two seconds after it arrives". That
  comment describes beo's own playback expectations, and I cannot reconcile it.
  **Whether a real source paces at real time or bursts ahead is the thing the
  capture tool must answer before Phase 2** (compare arrival times with
  sequence numbers). If it bursts 1 to 2 s ahead, the jitter buffer must absorb
  that, which does not fit.
- Drift: the source's sample clock and the ESP32's I2S clock differ by tens of
  ppm; at 44.1 kHz, 50 ppm is 2.2 samples/s, so a 400 ms buffer drifts a block
  in about 20 min. Drop/repeat one sample when fill leaves a band (Phase 3).
- Rate: format code in the header selects 44.1 vs 48 kHz. beo says "every
  listener plays 44.1 kHz" for what it sends **[read]**, but a real line-in
  source might send 48 kHz **[unknown]**. The I2S driver must reconfigure from
  the header, and 1062 frames/datagram at both rates is **[read]**.

### 4.6 PCM5102 note (for the Phase 2 docs)

Line-level output, no hardware volume. The common PCM5102 boards need **SCK
tied to ground** (so the internal PLL generates the system clock; solder
bridge or jumper, depending on the board), and the format/filter/mute pads
(FLT, DEMP, XSMT, FMT) set to the usual values per the board. Apply volume in
firmware (Phase 3).

---

## 5. Does muting the source change the stream?

**Nothing in the repo says.** What the repo does say:

- A source's *transport* pause/play changes the stream only by the 16-byte
  marker; "PCM continues unchanged" through a pause, and the source `PROGRESS`
  switches to `pause` **[read]**.
- A source's own `VOLUME` (level/muted/range) is reported to the listener in
  `FullStatus` (`<speaker><level>…<muted>…`) and is *asked* of the listener by
  the source, never set: "a source never sets the listener's volume"
  **[read]**.
- The Python sender captures PCM from its sound card and never applies its own
  volume to the stream **[read]**, so for beo's own source the stream is
  independent of the source's volume.

So **[inferred]**: a source's *room* mute is a post-stream renderer function and
the stream the ESP32 gets is unchanged. There are three ways that could be
wrong: the product may send a `MuteAllSinks`-style marker when it mutes
(unknown, since the code interprets markers only as pause/play); `VOLUME`
notifications with `muted=1` may reach us and an over-eager implementation
could mute the ESP32 (don't apply the *source's* mute to our output; only
apply the Mozart commands the app sends us); or the source may sink pre-fader
only for some source types. **The capture tool must record a stream with the
source muted** (your request) and diff markers/levels against an unmuted one.

---

## 6. garnet_web integration

### 6.1 What it is **[read]**

- HTTP server: **`esp_http_server`**, one instance, one task, 8 KB stack,
  wildcard URI match, `lru_purge_enable`, **`max_uri_handlers = 6`**,
  `ctrl_port = port + 32768`. Port = `GwConfig.port`, default **80**.
- Only 4 routes: `GET /` (gzipped UI), `GET /api/*`, `POST /api/*` and the
  404 error hook (captive portal 302 redirect in portal mode, else
  `404 "Not found"` plain text). Everything below `/api/` is string matching.
- Auth: every `/api/*` (except session/login) needs a session cookie; POSTs
  also need `X-GW: 1`. No unauthenticated API.
- Network: `Network.begin()`, own Wi-Fi list (5 saved networks), `WiFi.persistent(false)`,
  captive-portal AP + `DNSServer` when no uplink, `WIFI_AP_STA`; Ethernet
  optional (`GARNET_WEB_ETH`), primary when up.
- mDNS: **`MDNS.begin(hostname)`** (Arduino wrapper over IDF mdns) and
  `MDNS.addService("http","tcp",gwCfg.port)`, started once the uplink is up
  (`gwNetLoop` → `startMdnsOnce`). Hostname = settings `system/hostname` or
  `<name>-<last 2 MAC bytes>`, lower-case `[a-z0-9-]`, ≤ 32 characters.
- Callbacks (`onChange`, `action`, `onSave`) run on the task that calls
  `gwLoop()`. `GsField::info` getters run on the **server task** and must be
  read-only. `gsGet*` read NVS every time (no cache): never call them from the
  audio path.
- Settings model: `GsField` types Toggle, Text, Password, Number (int32, min,
  max, step), Select, Button, Info. Groups: id ≤ 15 chars (also the NVS
  namespace), ≤ 32 fields, ≤ 16 groups in total across the app and garnet's own.
  `gsWithReboot()` marks a group "reboots after save". Info getters have no
  context pointer (plain function pointer returning `String`), so live numbers
  need globals/atomics.
- Not provided: Bluetooth sink name, I2S, `WiFi.setSleep`. `GARNET_WEB_BT`
  gives on/off + advertised name for BT (not a sink).

### 6.2 Port 80 conflict: can Mozart coexist? **Not as built.**

Three separate collisions:

1. **Port**: both want 80. A second `esp_http_server` on 80 fails to bind.
   Moving garnet off 80 breaks captive-portal detection, because OS probes go to
   port 80 and the portal's 302 lives in garnet's 404 hook.
2. **`GET /`**: garnet serves its UI; the BeoZone service index also lives at
   `/`. Probably peers fetch it on **8080** (see §2.5), so serving the UI on 80
   and the BeoZone index on 8080 sidesteps this **[inferred]**.
3. **`/api/*`**: garnet's wildcard handler would receive `/api/v1/*`. The
   handlers are matched in registration order **[inferred from IDF docs;
   verify]**, so Mozart's routes must be registered *before* garnet's, and with
   all of GET/PUT/POST/PATCH/DELETE (otherwise IDF answers `405`, and the app
   expects `404` for unknown routes). Also `/BeoDevice`, `/BeoZone/*`, `/Ping`
   would otherwise fall into the portal 404 hook.

**Proposed small, upstreamable garnet_web change** (list, not implemented):

- (a) A public hook invoked from `gwServerBegin()` *before* garnet registers its
  own routes: `GwConfig.onServer(httpd_handle_t)`, or `httpd_handle_t
  gwHttpServer()` plus a documented "register before `gwBegin`" ordering, plus
  `GwConfig.maxUriHandlers` (default 6, since Mozart and the legacy surface
  need more). The app then registers `/api/v1/*` and the BeoZone paths on
  **garnet's** server on 80, unauthenticated. No parallel server, no port
  changes, captive portal untouched.
- (b) Port **8080** (ping + BeoZone + the held-open notification stream) is a
  small *second* `esp_http_server` owned by the app; garnet is not involved.
- (c) An mDNS hook: `GwConfig.onMdnsReady()` (or an exported `gwMdnsReady()`)
  called right after garnet's `MDNS.begin`, so the app can add its records on
  the same responder. Without it the app polls `gwNetMode()` and races
  `startMdnsOnce` (garnet's flag is private).
- (d) Optional: `GwConfig.keepWifiAwake` (or just document that the app calls
  `WiFi.setSleep(false)`).

If you prefer *no* garnet change: run garnet on, say, 8081 and give up
captive-portal detection for first-run setup (the user types
`192.168.4.1:8081`). I advise against it; a captive portal is the reason to
use garnet.

### 6.3 Custom groups and fields

No library change needed for settings, only the existing API **[read]**.
Groups (each one `static const GsGroup`, register before `gwBegin`):

| Group (≤ 15 chars) | Fields |
|---|---|
| `product` | Text name, Select profile (type), Info jid, Info serial, Toggle "Enrol in network config" |
| `audio` | Number I2S BCK / LRCK / DOUT (−1 = off, with `gsWithReboot`), Number default volume, Number buffer ms (100..600), Select I2S rate-follow mode |
| `status` | Info fields: free heap, largest block, buffer fill %, packets received/lost, underruns, corrections, source jid, stream format (via globals/atomics, since getters run on the server task) |
| (later) `bluetooth` | sink name when A2DP exists |

Hostname: set `system/hostname` with `gsSetString` before `gwBegin` on first
boot, to the B&O host name; garnet's own default is `name-xxxx`. It must pass
garnet's validator (lower case a to z, 0 to 9, hyphen, ≤ 32). The Python host is
mixed case (`Beolab-8-99012345`); DNS is case-insensitive, but whether the app
cares is **[unknown]**.

### 6.4 mDNS clash

One responder, one hostname. IDF mDNS allows a per-service instance name, so
the B&O instance names are set per service and do not touch garnet's
`<hostname>._http._tcp`. `CONFIG_MDNS_MAX_SERVICES=10`; we need ~6 **[measured
config, counted]**. Things that are **[unknown]** until a build runs:

- whether IDF accepts a second `_http._tcp` (beo's own advertises `_http` at
  port 80, garnet's `_http` is at `gwCfg.port`; if refused, drop beo's, which is
  probably unneeded);
- whether the instance name may contain dots (`_beocore`: the jid) and whether
  TXT values may be empty (`assoc=`);
- an underscore in a service type: `_beo_settings` (zeroconf needed
  `strict=False`);
- the goodbye-before-announce behaviour (§1.1). IDF sends a goodbye on
  `mdns_service_remove`, so remove-then-add at boot is a candidate.

Sockets: garnet httpd (1 listen + up to 7 open + ctrl), Mozart 8080 (1+2),
WebSocket 9339 (1+2), XMPP 5222 (1+2 + outbound to a leader), FPS UDP + TCP
listen + TCP conn (3), mDNS (1 to 3), DNS captive (portal only) comes to ~20
against `CONFIG_LWIP_MAX_SOCKETS=16`. It needs the custom sdkconfig anyway
(§4.2.1), or merging 8080 + 9339 + 5222 into one `select()` loop task (which also saves
stacks).

### 6.5 Other garnet_web facts

- **Flash**: baseline Wi-Fi+OTA = 1 068 736 B in a 1 310 720 B slot (**81.5 %**)
  **[measured]**. Adding mDNS records, esp_http_server routes, I2S driver,
  XMPP and the Mozart tables will not fit `default.csv`: choose a partition
  table with larger OTA slots (4 MB flash: e.g. `min_spiffs.csv` 1.9 MB × 2
  **[inferred, check]**). `huge_app.csv` has no OTA.
- Build: the examples pin
  `platform = …/55.03.312-1/platform-espressif32.zip`; "never add `lib_ignore`"
  (it rewrites the shared IDF build script for every project on the machine,
  CLAUDE.md). `symlink://` libs work **[measured]**: my baseline used
  `symlink:///…/garnet_settings` + `…/garnet_web` and built in 72 s.
  Note: the first build here replaced the installed Arduino core with 3.3.12
  (`Reinstall Arduino framework`). It is the pinned release, but it is a
  change to the shared `~/.platformio` that I made.
- `GARNET_WEB_ETH` / `examples/wt32_eth01` exist for the later WT32-ETH01
  target; garnet makes Ethernet primary and Wi-Fi STA off when it is up.
  `ESP.getFreeHeap`, `getMaxAllocHeap`, `getMinFreeHeap` are already System
  Info fields, and `GARNET_WEB_LOG` streams `log_x` to the browser (8 KB ring).

---

## 7. Unknowns and inferences, consolidated

Ranked by how much they can change the plan.

1. **Real-source stream format and protection.** No captured source→listener
   stream in the repo. Is the line-in (`PROPRIETARY`) stream plain PCM? Does a
   source refuse a listener that declares `NONE` only, or all three, as beo
   does? Does it send 48 kHz? *Capture first.*
2. **Real-time pacing vs bursting.** Arrival time vs sequence. Decides whether a
   ~0.4 s buffer is enough (§4.5).
3. **Will the real source open a session to us and accept us?** Needs: our
   `_beocore` record, enrolment, the XMPP greeting, `Attach`/`ConnectRequest`.
   beo's whole listener path is "built, not verified end to end".
4. **Enrolment**: leader discovery, document size on a real network, whether a
   blind `Insert` is idempotent, whether the local write is honoured (§3.5).
5. **Source muted**: any stream or marker difference (§5).
6. lwIP reassembly (blocking, verified off) and the `custom_sdkconfig` path
   (untried); real free heap / largest block on hardware (not measured).
7. Whether a real source needs notifications from the listener
   (STREAMING_STATUS etc.), and whether it expects `VOLUME` pushes on change.
8. `productType` for `_beoremote`: type name (beo listening code) vs hardware
   code (`CA17`, beo's source-side measurement).
9. The plain-speaker Mozart profile in the real app (never recorded), and my
   one-child `remote` tree.
10. mDNS: goodbye-first vs code, duplicate `_http`, dotted instance names,
    empty TXT values, host-name case, how the app reacts to an ESP32 that
    re-announces without a goodbye.
11. Which port peers use for the legacy BeoZone paths (80 vs 8080); whether a
    source tolerates a missing `BeoNotify`.
12. Marker toggle semantics (first marker vs pause).
13. `Volume` scales: beocore default range 0..90 vs Mozart 0..100.
14. Drift sign/size of the real source (50 ppm assumed).

### Things I inferred rather than read

- "Real products pace at real time" (from a constant 1.11 s PTS lead).
- "A source's room mute does not alter the stream."
- "Peers use 8080 for BeoZone paths."
- "Handler registration order decides `/api/v1/*` vs garnet's `/api/*`."
- Every RAM estimate except the 50 024 B static and the 320 KB total.
- The 3-fragment arithmetic and that lwIP drops fragments when reassembly is
  off (the config is measured; the behaviour is from lwIP's source from memory).

---

## 8. Decisions I need from you before Phase 1

1. **Product identity**: Beolab 8 type 3150, item 1200322, friendly name
   configurable (my recommendation), or another type?
2. **garnet_web change** (§6.2): may I prepare the small hook patch
   (`onServer` + `maxUriHandlers` + `onMdnsReady`) in `garnet_web`, as an
   upstreamable commit in your checkout? Or do you want to stay on unmodified
   garnet and accept losing captive-portal detection?
3. **`custom_sdkconfig`** (§4.2.1): OK to introduce the pioarduino hybrid build
   for lwIP reassembly (slower first build, more disk)? It is the only way I see
   that keeps one framework.
4. **Buffer target**: accept ~0.4 s default / 0.6 s max for plain ESP32
   (PSRAM variants later)?
5. **Capture before Phase 2**: the tools in `tools/` capture from the Pi
   adapter, but **a real-source capture needs a real B&O source and the Pi
   adapter in listener mode (`BEO_LISTEN=on`, `BEO_ENROL=1`)**. Enrolment writes
   to the document every product on your network holds. Are you willing to enrol
   the Pi (and later the ESP32) into your network's configuration?

---

## 8a. Decisions taken (review of Phase 0)

- **garnet_web runs on its own port (8000), Mozart owns 80 and 8080.** Mozart's
  port cannot move; garnet's can. Consequence, accepted: no OS captive-portal
  detection (probes go to port 80), so first-run setup is "join the setup AP,
  open `http://192.168.4.1:8000`". The §6.2 hook patch is therefore **not**
  needed for ports. No garnet_web change is planned for Phase 1.
- **OTA dropped** (no `GARNET_WEB_OTA`); partition table `huge_app.csv`
  (no OTA slots, ~3 MB app), which also fixes the flash-slot problem of §6.5.
- **Buffer: 0.4 s default** (Phase 2).
- **Enrolment allowed** (settings toggle, default off).
- Assumed without an explicit answer, change if wrong: identity Beolab 8 /
  3150 / item 1200322; `custom_sdkconfig` for lwIP reassembly in Phase 2.

## 9. Licence

Derived from djerik/beo (Apache-2.0): the protocol constants, XML bodies and
field orders in §2 to §4 are taken from the files below, and any
ESP32 code that reproduces them is a derived work that keeps the `NOTICE`
attribution and states what was changed:

`packages/networklink/beo/networklink/{beocore,fps,sources,discovery,configuration}.py`,
`roles/{listener,source}.py`; `packages/mozart/beo/mozart/{facade,profile,ws}.py`;
`packages/product/beo/product/{identity,playback}.py`; the golden files under
`apps/beo-adapter/tests/golden/`; `docs/app-protocol.md`.

Unofficial; not affiliated with Bang & Olufsen. Apache-2.0 `LICENSE` and
`NOTICE` will be copied into this project at Phase 1 with the derived files
listed per source file.


---

## 10. Phase 2 measurements (real hardware, this household's network)

Tags as above; everything here was **measured** unless marked. Board: plain ESP32 dev board
(no PSRAM), 160/240 MHz, Wi-Fi to a home router. Source: beo's own Python `SourceRole` on a Mac
(`tools/test_source.py`), **not a real B&O source**. No real source has streamed to the board yet.

### 10.1 What works

- **Handshake with beo's source: complete.** header, disco, `Attach`, `ConnectRequest`, our
  `Connect`, `_notification-subscribe`, the source's renderer subscription and the three volume
  questions, notifications, teardown (`Disconnect`, our own `Disconnect` + unsubscribe). The C++
  listener's replies are **byte-identical** to beo's builders on the host (19 lines,
  `tests/host/test_link.py`; enrolment bodies 5 of 5, `tests/host/test_enrol.py`).
- **Streaming**: a 180 s stream, 7496 datagrams: 7 lost (0.09 %), 5 underruns, 21 overruns,
  heap steady at ~43 KB, no crash. Fill sits at 120 to 190 ms of a 216 ms buffer. The overruns are
  source/I2S clock drift (the Mac's 44.1 kHz against the ESP32's), which Phase 3's drift correction is for.
- **I2S** initialises and the DMA is paced (the fill is stable), but **nobody has listened to it**:
  no PCM5102 was attached or measured. "Clean audio" is **not verified**.
- **IP reassembly works** with `custom_sdkconfig` (3-fragment datagrams arrive). The hybrid build
  takes ~80 s the first time, 30 s after.
- **mDNS**: IDF accepts all the product records on garnet's responder: `_bangolufsen` (app),
  `_beocore` with the jid (dots and `@`) as instance name, `_beoremote` with an empty `assoc`,
  `_beo_settings` (underscore type). The earlier unknowns of §6.4 are closed. A second `_http._tcp` was
  not tried (not added).
- **Real products dial us.** Within seconds of the `_beocore` record appearing, the four real
  NetworkLink products on the network (a Beoplay M5, M3, Beosound Essence and Moment) each opened a session to
  the ESP32. The Moment's first stanza was `<Experiences xmlns="beocore.player.1">` as a `set`, i.e. beo's
  "OneWayJoin": it asks every known device what it plays. We answer with the idle (empty) form.
  That is the first evidence from a real product that our `_beocore` presence is read as a peer.

### 10.2 Corrections to Phase 0

| Phase 0 said | Measured |
|---|---|
| ~270 KB heap at boot, 60 to 110 KB free for the buffer | **~120 KB free** with garnet, Mozart (3 servers), mDNS and beocore up, after trimming; **85 KB** before trimming |
| 0.4 s buffer is feasible | **~220 ms.** The stream's own plumbing (Wi-Fi RX buffers, IP reassembly, the UDP queue, I2S DMA, two task stacks) takes about 55 KB on top of an idle system. A 409 ms buffer ran out of memory mid-handshake (heap down to 696 bytes). The buffer is now allocated block by block at boot and trimmed to what the heap allows beside a 90 KB reserve; the Status page shows the result |
| Real products serve BeoZone on 80 or 8080 (unknown) | **8080.** Port 80 answers 301/404 for `/BeoDevice`, `/BeoZone/*` on all four; 8080 serves the index and everything else. Their Mozart API (`/api/v1`) answers 500: they are BeoZone-only products |
| Largest contiguous block bounds the buffer | Yes: after one stream a 110 KB block had fragmented to 65 KB. Fixed by allocating 4248-byte blocks individually at boot |

Radio and heap settings that made this possible (`platformio.ini` `custom_sdkconfig`): Bluetooth off,
Wi-Fi static RX 6 / dynamic RX 12 / dynamic TX 8 / RX BA window 4, reassembly pbufs 10, 24 sockets,
Arduino loop stack 6 KB. Bluetooth must come back for the later A2DP target, which costs heap again.

### 10.3 What the real network looks like (read-only HTTP, `tools/capture_api.py`, `captures/api-*`)

All four products list `LINE IN`, `linkable: true`, **`contentProtection: PROPRIETARY`**. The
Master Link product on the network (type 1790) lists its sources, including `LineIn`, with protection
**`NONE`**, as borrowed sources. If the record player is on such a Master Link line-in, the stream may
be plain; if it is on one of the ASE products' `LINE IN` it carries `PROPRIETARY`. **Which one it is
decides whether this project can work**, and only a capture shows it. The `ActiveSources` of the
Essence showed Spotify playing, unrelated.

### 10.4 Still not known (needs a real source)

1. The stream format/protection of a real `LINE IN` (§3.6): run `tools/capture_fps.py` and have
   the app add the "Capture Node" to a playing line-in. The tool is ready and was self-tested only.
2. Real-time pacing vs. bursting (§4.5), the 44.1 vs 48 kHz format code, and the effect of muting the
   source (§5): same capture, twice (muted, unmuted), then compare `summary.json`.
3. Whether a real source accepts the ESP32 once enrolled. Enrolment is implemented
   (`src/esp32/enrol_esp.cpp`) but **never run**: the toggle is off, and a real run changes the
   configuration every product holds.
4. Whether the app/ReMoment shows the ESP32 as addable. The ESP32 is not enrolled, so the source
   would refuse it at `Connect` (§3.5).


---

## 11. First stream from a real B&O source: it is not beo's PCM (stop point)

Measured on 2026-10-14: a Beosound Moment, line-in playing, joined by the ESP32 after the B&O
app sent `POST /api/v1/beolink/join/<Moment jid>` to it (the app asks the *listener* to join; the
ESP32 then dials the source). Everything on the control plane worked:

- Our `Experiences` request was answered with the Moment's experience: source `AUX_A`/`linein`,
  type `LINE IN`, `contentProtection` **`PROPRIETARY`**, state `play`.
- Our `Connect` (declaring `NONE`, `DECIMATE`, `PROPRIETARY` as beo does) was accepted:
  `<connectionId>CON_<our jid>_5</connectionId>`. The Moment opened the TCP heartbeat to port 1234, sent
  its renderer subscription and the three volume questions, and a `SOURCE` notification; the app reported the
  product added. The Connect reply carries no protection information.

**What arrived on UDP 1234 is not the format beo documents** (logged by the `UNRECOGNISED datagram`
counter, ~100 per second):

```
43 21 9b e5 | 00 02 85 00 | 00 40 46 95 6a 39 eb 6d | ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff | 03 04 10 5e d4 17 09 a6 ...
magic 0x4321  seq(+1)       fmt word 0002, 8500       8..15 grows by exactly 10 000 000 per datagram      16..31 all 0xFF        payload
```

| | beo / FPS as the library sends it | real Moment |
|---|---|---|
| magic | `0x1234` | **`0x4321`** |
| datagram | 4280 B (32 + 4248) | **1972 B** |
| cadence | 24.08 ms (1062 frames) | **10 ms** (counter step of 10 000 000 ns) |
| rate code (bytes 4..5) | `0001` = 44.1 kHz | `0002` = **48 kHz** |
| bytes 24..31 | stream clock | `ff` x 8 |
| payload | PCM | starts `03 04 10 5e` (constant), then bytes that change every datagram |

1972 - 52 = **1920 = 480 frames x 4 bytes**, exactly 10 ms of 48 kHz S16 stereo; so the payload looks
like PCM-sized data behind a 20-byte header, and the content looks like noise from the first bytes. That
fits an **encrypted or scrambled PCM stream**, which is what `PROPRIETARY` content protection suggests.
**This is inferred from 40 logged bytes of five datagrams, not proven**: I have not dumped a full datagram.

**Stopping here, as agreed: the stream is not PCM in the format we can play.** Nothing in beo, in
this repository or in the handshake gives a key or a description of the scheme, and I am not going to
guess at one. Decoding a `PROPRIETARY` stream would be an attempt to defeat the source's protection, which is
not what this project is for.

Options (none tried; the first two change what we *declare*, not how we decode):

1. **Declare no `PROPRIETARY` support.** The scheme list in our `Connect` is ours to choose. A source whose
   content is `PROPRIETARY` may then refuse the listener, or send a reduced-quality form (the scheme is called
   `DECIMATE`) in plain PCM. What the Moment does is **unknown**; it is a one-line change in
   `networklink_rx.cpp` (`kCpCapabilities`) and the Moment is the test source.
2. **Use a source whose line-in is `NONE`.** The Master Link product on the network (type 1790) lists a
   `LineIn` with protection `NONE`; beo's own sender and its `NONE` sources are plain PCM. The record
   player is on the Beoplay M5, whose line-in is `PROPRIETARY`, so this would need re-cabling.
3. **Capture a full datagram** (`tools/capture_fps.py` records them; `--join IP JID` on the Moment) to settle
   inferred-vs-proven, and look at whether the Moment sends any other framing (the 48 kHz / 10 ms format
   would also need support: the format code, 480-frame blocks).
4. Stop: the use case (a record player on an M5/Moment line-in) is blocked by the product's own protection.

### 11.1 Option 1 tried: declaring only `NONE` and `DECIMATE` (2026-10-14, measured)

The Moment **refuses**. With `PROPRIETARY` left out of the `CpCapabilities` in our `Connect`, it answered
the Connect with an iq error whose text is `Unable to add sink`, three times out of three (one per
join the app retried). No stream, no heartbeat. It did not fall back to `DECIMATE` or to plain PCM. So
for this source a listener must claim `PROPRIETARY`, and then receives the stream described above.

Not tried: the entry we inserted into the shared configuration still lists `PROPRIETARY` under
`Render#1/Fps#1/ContentProtection#1`. The refusal says "sink", which may mean the Moment looked at
Connect, at that entry, or both. The firmware currently declares only `NONE`/`DECIMATE` in the Connect
(the host differential test was adjusted to match); `kCpCapabilities` in `networklink_rx.cpp` restores beo's
original.

### 11.2 Option 2: one full datagram (measured, 2026-10-14)

With beo's original declaration restored (`NONE`, `DECIMATE`, `PROPRIETARY`) the Moment streams again.
One full 1972-byte datagram was recovered from the serial dump (the other two were interleaved with log
lines and arrived incomplete):

```
43 21 33 4e | 00 02 85 00 | 00 40 47 89 ba e1 87 6b | ff x16 | 03 04 10 5e  85 cb f5 a8 ee 55 2f d4 a4 0a 4c 19 a4 c7 32 7f | payload (1920 bytes)
```

Payload analysed as 480 frames of S16LE stereo: byte entropy **7.76 bits/byte**; mean |sample| 16729 and mean
|difference of consecutive samples| 22629. Uniform random 16-bit values give 16384 and 21845. Real PCM audio
is smooth, so its consecutive-sample difference is far below its amplitude; here it is *larger*. **The
payload is statistically indistinguishable from random bytes, i.e. encrypted (or compressed and then
encrypted).** One datagram only, but it is unambiguous on that measure.

So a `PROPRIETARY` line-in is, as far as we can see, an encrypted stream, the Moment will not send anything else to
a listener that does not claim the scheme, and the project has no key and no description of the scheme.
beo contains nothing on it (searched for `PROPRIETARY`, `DECIMATE`, encryption, 0x4321, 48 kHz, 1972): its
sources with `PROPRIETARY` are listed in its catalogue, and its only verified streaming path is a
BeoSound 9000 over Master Link (`NONE`). Beo's own docs mark "another product plays to the Pi" as built
and not verified end to end.

**Conclusion for the use case: the ESP32 cannot play the line-in of a Beoplay M5 or Beosound Moment over
NetworkLink.** A source labelled `NONE` (the Master Link product's line-in) is the only kind seen here
that would give plain PCM.

---

## 12. A real unprotected source works: B&O Radio on a Beoplay M5 (measured, 2026-10-14)

Source: Beoplay M5, source `beoradio` (`BEO RADIO`, protection `NONE`), joined from the B&O app
(`POST /api/v1/beolink/join/<M5 jid>` to the ESP32, which dials the M5). Same control sequence as the Moment
(§11), but the stream is **exactly the format beo documents**: magic `0x1234`, rate code `0002` = **48 kHz**,
32-byte header, S16LE stereo PCM in 1062-frame datagrams (22.1 ms), real audio (peak ~20 000, rms ~4 600), with
the heartbeat on TCP 1234. The ESP32 reconfigures I2S to 48 kHz from the header.

Over about 80 s on a ~-67 dBm Wi-Fi link: arrivals averaged **22.0 ms** (expected 22.1), 0 datagrams lost,
4 underruns, 22 overruns, heap 45 to 55 KB free during the stream. An earlier run on the same board had a
7-second Wi-Fi gap (363 datagrams lost) followed by a catch-up burst from the source that overran the buffer
(392 overruns). **The audio has not been listened to** (no DAC attached).

Things this run settled or changed:

- **Join works first time** once the product list is remembered for 20 minutes (a single mDNS pass often
  misses a product, which made "nothing by that jid has been heard").
- **The source's FullStatus reply is several KB; it is no longer buffered** (`discarding` in
  `networklink_rx.cpp`), which was a large part of the heap pressure at stream start.
- The app joins via the **listener** (`beolink/join` on the ESP32), not via `expand` on the source.
- Overruns while steady mean the source's 48 kHz clock is a little ahead of our I2S clock, or a burst after a
  Wi-Fi gap: Phase 3's drift correction.
- The line-in of the M5 and Moment stays blocked (§11); `DECIMATE`-only with an updated configuration entry is
  still untried.
