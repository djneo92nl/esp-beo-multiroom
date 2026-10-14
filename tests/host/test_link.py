#!/usr/bin/env python3
"""Differential test: the C++ listener against beo's own wire builders.

Feeds one conversation to the compiled core (tests/host/test_link.cpp) and builds what beo's
Python (StatusBuilder, Requests, Session's quoting) says each reply must be. Byte-for-byte.
Usage: test_link.py <test_link binary> [beo path]
"""
import os, subprocess, sys
beo = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../beo")
for p in ("product", "networklink"):
    sys.path.insert(0, os.path.join(beo, "packages", p))
from beo.networklink import Catalog, ProductIdentity, SOURCES                          # noqa: E402
from beo.networklink.beocore import double_quotes, DISCO_FEATURES                      # noqa: E402
from beo.networklink.sources import Requests, StatusBuilder, Volume                    # noqa: E402

ident = ProductIdentity.from_jid("3150.1200322.99000777", "Capture Node", "00:00:5e:00:53:07", product_type="SEOUL")
ME = ident.full_jid()
SRC = "2997.1290084.99000500@products.bang-olufsen.com"
cat = Catalog([SOURCES["CD"].named("Capture Node")])
vol = Volume(35, False, 0, 90)


def result(id_, inner):
    return double_quotes(f"<iq id='{id_}' to='{SRC}' from='{ME}' type='result'>{inner}</iq>")


def req(id_, inner, typ="set"):
    return double_quotes(f"<iq id='{id_}' to='{SRC}' from='{ME}' type='{typ}'>{inner}</iq>")


def inbound(id_, typ, inner):
    return f"<iq id='{id_}' type='{typ}' from='{SRC}' to='{ME}'>{inner}</iq>"


header = lambda to: double_quotes(f"<stream:stream to='{to}' from='{ME}' xmlns='jabber:client' "
                                  "xmlns:stream='http://etherx.jabber.org/streams' version='1.0'>")
feats = "".join(f"<feature var='{f}'/>" for f in DISCO_FEATURES)
disco = (f"<query xmlns='http://jabber.org/protocol/disco#info'><identity category='client' name='Capture Node' "
         f"type='product'/>{feats}</query>")

# (stimulus line, [expected output lines])
steps = [
    ("OPEN 1 0", []),
    ("DATA 1 " + f"<stream:stream to='{ME}' from='{SRC}' xmlns='jabber:client' xmlns:stream='x' version='1.0'>",
     ["SEND 1 " + header(SRC), "SEND 1 " + req(201, "<query xmlns='http://jabber.org/protocol/disco#info'/>", "get")]),
    ("DATA 1 " + inbound("77", "get", "<query xmlns='http://jabber.org/protocol/disco#info'/>"),
     ["SEND 1 " + result("77", disco)]),
    ("DATA 1 " + inbound("50900", "set", "<Attach xmlns='beocore.product.1'><role>0</role></Attach>"),
     ["SEND 1 " + result("50900", StatusBuilder.attach_reply(cat.contact_info_xml(ident)))]),
    ("DATA 1 " + inbound("51002", "set", f"<ConnectRequest xmlns='beocore.player.1'><uniqueSourceId><source>linein</source><jid>{SRC}</jid></uniqueSourceId></ConnectRequest>"),
     ["SEND 1 " + result("51002", StatusBuilder.connect_request_reply()), "FPS open",
      "SEND 1 " + req(202, Requests.connect("linein", SRC, "203.0.113.2"))]),
    ("DATA 1 " + f"<iq id='202' type='result' from='{SRC}' to='{ME}'><Connect xmlns='beocore.player.1'><connectionId>{SRC}</connectionId></Connect></iq>",
     ["SEND 1 " + req(203, Requests.subscribe_source())]),
    ("DATA 1 " + f"<iq id='203' type='result' from='{SRC}' to='{ME}'><_notification-subscribe xmlns='beocore.notifications.1'><FullStatus><status></status></FullStatus></_notification-subscribe></iq>",
     [f"STARTED linein {SRC}"]),
    ("DATA 1 " + inbound("50910", "set", Requests.subscribe_renderer()),
     ["SEND 1 " + result("50910", StatusBuilder.subscribe_reply(StatusBuilder.renderer_status(vol)))]),
    ("DATA 1 " + inbound("50911", "set", Requests.get_volume()),
     ["SEND 1 " + result("50911", StatusBuilder.get_volume_reply(35))]),
    ("DATA 1 " + inbound("50912", "set", Requests.get_volume_muted()),
     ["SEND 1 " + result("50912", StatusBuilder.get_volume_muted_reply(False))]),
    ("DATA 1 " + inbound("50913", "set", Requests.get_volume_range()),
     ["SEND 1 " + result("50913", StatusBuilder.get_volume_range_reply(0, 90))]),
    ("DATA 1 " + inbound("51003", "set", Requests.disconnect(1)),
     ["SEND 1 " + result("51003", StatusBuilder.disconnect_reply()), "FPS close",
      "SEND 1 " + req(204, Requests.disconnect(1)), "SEND 1 " + req(205, Requests.unsubscribe()), "STOPPED"]),
    ("DATA 1 " + inbound("9", "set", "<Bogus xmlns='x'/>"),
     ["SEND 1 " + double_quotes(f"<iq id='9' to='{SRC}' from='{ME}' type='error'><error type='cancel'><feature-not-implemented "
                                "xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/></error></iq>")]),
]
p = subprocess.run([sys.argv[1]], input="".join(s[0].replace("\n", "\\n") + "\n" for s in steps),
                   capture_output=True, text=True)
got = p.stdout.split("\n")
want = [l.replace("\n", "\\n") for s in steps for l in s[1]]
got = [g for g in got if g]
bad = 0
for i in range(max(len(got), len(want))):
    g = got[i] if i < len(got) else "<missing>"
    w = want[i] if i < len(want) else "<extra>"
    if g != w:
        bad += 1
        print("DIFF at output %d\n  got  %s\n  want %s" % (i, g[:400], w[:400]))
print("%d lines identical, %d different" % (len(want) - bad, bad))
sys.exit(1 if bad else 0)
