#include <cstdio>
#include "../../src/core/enrol.h"
int main() {
  Identity id; id.serial = "99000777"; id.mac = "00:00:5e:00:53:07"; id.friendly = "Capture Node";
  printf("%s\n%s\n%s\n", enrol::insert_body(id).c_str(), enrol::subscribe_body().c_str(), enrol::entry_xml(id).c_str());
  // scanner: entry split across chunks, then </iq>
  enrol::DocScanner s(id.jid());
  std::string doc = "<iq id=\"xmpp1\" type=\"get\"></iq><iq id=\"201\" type=\"result\"><x><item name=\"" + enrol::item_key(id.jid()) + "\"></item></x></iq>";
  for (size_t i = 0; i < doc.size(); i += 7) s.feed(doc.substr(i, 7).data(), doc.substr(i, 7).size());
  printf("scan present=%d done=%d\n", s.present(), s.done());
  enrol::DocScanner d(id.jid());
  std::string doc2 = "<iq id=\"201\" type=\"result\"><deleted><item name=\"" + enrol::item_key(id.jid()) + "\"></item></deleted></iq>";
  d.feed(doc2.data(), doc2.size());
  printf("scan present=%d done=%d\n", d.present(), d.done());
}
