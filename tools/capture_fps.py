#!/usr/bin/env python3
"""A NetworkLink listener on this machine that records everything a source sends it.

Uses beo's own ListenerRole (the reference for how a listener behaves) from a checkout of
djerik/beo, with no dependency beyond the standard library: mDNS records are published
with macOS `dns-sd`. It writes, under OUTDIR:

    xmpp.log      every beocore stanza, both directions, with timestamps
    notify.log    the first of each notification kind the source sends
    stream.fps    every UDP datagram (audio AND the 16-byte markers) as
                  "<f64 monotonic seconds><u32 length><bytes>" after the magic b"FPSCAP1\\n"
    summary.json  counters at exit: datagrams, gaps, markers, header constants, pacing

    capture_fps.py [--beo PATH] [--out DIR] [--seconds N] [--name NAME] [--serial 99xxxxxx]
                   [--enrol] [--join IP JID]

Passive by default: it publishes its records and waits for a source to dial it (the B&O
app's "add to experience" on the source). --join IP JID makes it ask that product what is
playing and connect, which is the listener-initiated path. --enrol writes this node into
the configuration every product on the network shares; it is opt-in because that change
is persistent and cannot be undone from here.

Run a capture with the source's room mute on and another with it off, and diff the two
summary.json files (markers, header constants, arrival pacing).
"""
import argparse, json, os, socket, struct, subprocess, sys, threading, time, uuid, http.server

ap = argparse.ArgumentParser()
ap.add_argument("--beo", default=os.path.join(os.path.dirname(__file__), "..", "beo"))
ap.add_argument("--out", default=None)
ap.add_argument("--seconds", type=float, default=600)
ap.add_argument("--name", default="Capture Node")
ap.add_argument("--serial", default="99000777")
ap.add_argument("--enrol", action="store_true")
ap.add_argument("--join", nargs=2, metavar=("IP", "JID"))
a = ap.parse_args()

for p in ("product", "networklink"):
    sys.path.insert(0, os.path.join(a.beo, "packages", p))
from beo.networklink import FpsReceiver, ProductIdentity, Catalog, STANDARD_SOURCES   # noqa: E402
from beo.networklink.beocore import Server                                           # noqa: E402
from beo.networklink.roles.listener import ListenerRole                              # noqa: E402
from beo.networklink import configuration                                            # noqa: E402

out = a.out or os.path.join("captures", "fps-" + time.strftime("%Y%m%d-%H%M%S"))
os.makedirs(out, exist_ok=True)
t0 = time.monotonic()
xmpp = open(os.path.join(out, "xmpp.log"), "w", buffering=1)
notify = open(os.path.join(out, "notify.log"), "w", buffering=1)
stream = open(os.path.join(out, "stream.fps"), "wb")
stream.write(b"FPSCAP1\n")


def own_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("203.0.113.1", 1))
        return s.getsockname()[0]
    finally:
        s.close()


ip = own_ip()
mac = ":".join("%02X" % ((uuid.getnode() >> b) & 0xFF) for b in range(40, -8, -8))
ident = ProductIdentity.from_jid("3150.1200322.%s" % a.serial, a.name, mac, product_type="SEOUL",
                                 host="Capture-Node-%s" % a.serial)


class Recorder(FpsReceiver):
    """The beo test receiver, plus a copy of every raw datagram."""
    def decode(self, data, addr, now):
        stream.write(struct.pack("<dI", time.monotonic() - t0, len(data)) + data)
        return super().decode(data, addr, now)


def tap(direction, peer, text):
    xmpp.write("%9.3f %s %s %s\n" % (time.monotonic() - t0, direction, peer, text))


seen = set()


def on_notification(kind, text):
    if kind not in seen:
        seen.add(kind)
        notify.write("%9.3f %s %s\n" % (time.monotonic() - t0, kind, text))


role = ListenerRole(ident, ip, catalog=Catalog(STANDARD_SOURCES), receiver_factory=lambda fmt: Recorder(fmt),
                    tap=tap, on_notification=on_notification,
                    on_started=lambda cur: print("listening to", cur.source_key, cur.source_jid),
                    on_stopped=lambda cur: print("stream ended"),
                    on_refused=lambda cur, why: print("REFUSED:", why))
role.sink = type("Sink", (), {"write": lambda self, blk: None})()   # receive and count only
server = Server(ident, role.serve, tap=tap).open()


class Light(http.server.BaseHTTPRequestHandler):
    """Just enough of the BeoZone surface for a peer that looks at us on :8080."""
    def do_GET(self):
        body = b"{}"
        if self.path.rstrip("/") == "":
            body = json.dumps({"services": [{"name": "BeoDevice", "path": "BeoDevice/"}, {"name": "Ping", "path": "Ping"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *x):
        pass


try:
    threading.Thread(target=http.server.ThreadingHTTPServer(("0.0.0.0", 8080), Light).serve_forever, daemon=True).start()
except OSError as e:
    print("port 8080 not available (%s); peers cannot read our service index" % e)

jid = ident.full_jid()
reg = [
    ["dns-sd", "-R", jid, "_beocore._tcp", "local.", "5222", "hash=sha-1", "jid=" + jid, "name=" + a.name,
     "node=beocore", "txtvers=1", "ver=dSsQBJenapvOryzvtU90IM7SI+o="],
    ["dns-sd", "-R", a.name, "_beoremote._tcp", "local.", "8080", "assoc=", "item=1200322", "jid=" + jid,
     "mac=" + mac, "name=" + a.name, "productType=SEOUL", "type=3150",
     "services=BeoInput,BeoNotify,BeoZone,BeoDevice,BeoContent,BeoHome,BeoSecurity"],
]
procs = [subprocess.Popen(c, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for c in reg]
print("capture node %s at %s, writing to %s" % (jid, ip, out))


def leader_via_dnssd(seconds=8.0):
    """(ip, jid) of the configuration leader, found with dns-sd instead of zeroconf."""
    import re
    b = subprocess.Popen(["dns-sd", "-B", "_beoconfig._tcp", "local."], stdout=subprocess.PIPE, text=True)
    name = None
    end = time.time() + seconds
    while time.time() < end and not name:
        line = b.stdout.readline()
        m = re.search(r"Add\s+\d+\s+\d+\s+\S+\s+_beoconfig\._tcp\.\s+(.+)$", line)
        if m:
            name = m.group(1).strip()
    b.kill()
    if not name:
        return None
    r = subprocess.Popen(["dns-sd", "-L", name, "_beoconfig._tcp", "local."], stdout=subprocess.PIPE, text=True)
    host = jidv = None
    end = time.time() + 6
    while time.time() < end and not (host and jidv):
        line = r.stdout.readline()
        m = re.search(r"can be reached at (\S+):\d+", line)
        if m:
            host = m.group(1).rstrip(".")
        m = re.search(r"jid=(\S+)", line)
        if m:
            jidv = m.group(1)
    r.kill()
    return (socket.gethostbyname(host), jidv) if host and jidv else None


if a.enrol:
    entry = configuration.device_entry(ident, [__import__("beo.networklink", fromlist=["SOURCES"]).SOURCES["CD"].named(a.name)])
    print("enrol:", configuration.ensure_enrolled(ident, entry, leader=leader_via_dnssd))

if a.join:
    threading.Thread(target=lambda: print("join:", role.join(a.join[0], a.join[1])), daemon=True).start()

rx_stats = {}
try:
    end = time.time() + a.seconds
    while time.time() < end:
        time.sleep(1)
        cur = role.current
        if cur is not None and cur.receiver is not None:
            rx_stats = cur.receiver.stats()
except KeyboardInterrupt:
    pass
finally:
    for p in procs:
        p.kill()
    role.close()
    server.close()
    stream.close()
    summary = dict(rx_stats)
    summary["identity"] = jid
    json.dump(summary, open(os.path.join(out, "summary.json"), "w"), indent=1, default=str)
    print(json.dumps(summary, indent=1, default=str)[:1500])
