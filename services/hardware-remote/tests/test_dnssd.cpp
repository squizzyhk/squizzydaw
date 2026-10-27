// hardware-remote: DNS wire codec + DNS-SD record helpers (portable, no sockets).
#include <cstring>
#include <string>
#include <vector>

#include "sq/services/hardware_remote/dnssd_wire.hpp"
#include "sqtest.hpp"

namespace dn = sq::dnssd;

namespace {

sq_discovery_record makeRec(const char* name, const char* type, const char* host, uint16_t port, const char* txt) {
  sq_discovery_record r{};
  r.struct_size = sizeof r;
  std::snprintf(r.name, sizeof r.name, "%s", name);
  std::snprintf(r.type, sizeof r.type, "%s", type);
  std::snprintf(r.host, sizeof r.host, "%s", host);
  r.port = port;
  std::snprintf(r.txt, sizeof r.txt, "%s", txt);
  return r;
}

}  // namespace

SQ_TEST("dnssd: name escaping round-trips dots and backslashes") {
  std::vector<std::string> labels = {"Studio. A\\B", "_osc", "_udp", "local"};
  std::string joined = dn::joinName(labels);
  CHECK_EQ(joined, std::string("Studio\\. A\\\\B._osc._udp.local"));
  CHECK(dn::splitName(joined) == labels);
  CHECK(dn::sameName("FOO._osc._udp.local.", "foo._OSC._udp.local"));
  CHECK(!dn::sameName("foo._osc._udp.local", "bar._osc._udp.local"));
  CHECK_EQ(dn::serviceDomain("_osc._udp"), std::string("_osc._udp.local"));
  CHECK_EQ(dn::serviceDomain("_osc._udp.local."), std::string("_osc._udp.local"));
}

SQ_TEST("dnssd: TXT string <-> pairs") {
  auto pairs = dn::txtPairs("ver=1;flag;path=a\\;b;=bad");
  REQUIRE_EQ(pairs.size(), size_t(3));
  CHECK_EQ(pairs[0].first, std::string("ver"));
  CHECK_EQ(pairs[0].second, std::string("1"));
  CHECK_EQ(pairs[1].first, std::string("flag"));
  CHECK_EQ(pairs[1].second, std::string(""));
  CHECK_EQ(pairs[2].second, std::string("a;b"));
  CHECK_EQ(dn::txtFromPairs(pairs), std::string("ver=1;flag;path=a\\;b"));
  CHECK(dn::txtPairs(nullptr).empty());
  CHECK(dn::txtPairs("").empty());
}

SQ_TEST("dnssd: instance names split into instance, type and domain") {
  std::string inst, type, domain;
  REQUIRE(dn::splitInstanceName("Studio\\. A._osc._udp.local", inst, type, domain));
  CHECK_EQ(inst, std::string("Studio. A"));
  CHECK_EQ(type, std::string("_osc._udp"));
  CHECK_EQ(domain, std::string("local"));
  REQUIRE(dn::splitInstanceName("x._http._tcp.example.org.", inst, type, domain));
  CHECK_EQ(type, std::string("_http._tcp"));
  CHECK_EQ(domain, std::string("example.org"));
  CHECK(!dn::splitInstanceName("_osc._udp.local", inst, type, domain));  // no instance label
  CHECK(!dn::splitInstanceName("host.local", inst, type, domain));
  CHECK(!dn::splitInstanceName("a.b._osc._udp.local", inst, type, domain));  // unescaped dot in instance

  sq_discovery_record r = makeRec("Desk", "_osc._udp", "box", 9000, "a=1");
  CHECK_EQ(dn::instanceFullName(r), std::string("Desk._osc._udp.local"));
  CHECK_EQ(dn::hostTarget("box"), std::string("box.local"));
  CHECK_EQ(dn::hostTarget(""), std::string("squizzy.local"));
  CHECK_EQ(dn::hostTarget("a.b."), std::string("a.b"));

  sq_discovery_record out{};
  std::vector<dn::TxtPair> txt = {{"a", "1"}, {"b", ""}};
  REQUIRE(dn::makeRecord("My Desk._osc._udp.local.", "box.local.", 9001, txt, out));
  CHECK_EQ(out.struct_size, uint32_t(sizeof out));
  CHECK_EQ(std::string(out.name), std::string("My Desk"));
  CHECK_EQ(std::string(out.type), std::string("_osc._udp"));
  CHECK_EQ(std::string(out.host), std::string("box.local"));
  CHECK_EQ(out.port, uint16_t(9001));
  CHECK_EQ(std::string(out.txt), std::string("a=1;b"));
  CHECK(!dn::makeRecord("nothing", "h", 1, {}, out));
}

SQ_TEST("dnssd: announcement encodes, compresses and decodes back to the record") {
  sq_discovery_record r = makeRec("Studio. A", "_osc._udp", "mixbox", 8000, "proto=osc;ver=1.0");
  const uint8_t ip[4] = {192, 168, 1, 20};
  dn::Message ann = dn::buildAnnouncement(r, ip, 120);
  std::vector<uint8_t> wire;
  std::string err;
  REQUIRE(dn::encode(ann, wire, &err));
  // Compression: the service domain appears once in full, later as a pointer, so the packet is
  // much smaller than the sum of all names.
  CHECK(wire.size() < 200);
  dn::Message back;
  REQUIRE(dn::decode(wire.data(), wire.size(), back, &err));
  CHECK_EQ(back.flags, dn::kFlagResponse);
  REQUIRE_EQ(back.answers.size(), size_t(3));
  REQUIRE_EQ(back.additionals.size(), size_t(1));
  CHECK(back.answers[1].cacheFlush);

  std::vector<sq_discovery_record> found, lost;
  dn::extractInstances(back, "_osc._udp", found, lost);
  REQUIRE_EQ(found.size(), size_t(1));
  CHECK(lost.empty());
  CHECK_EQ(std::string(found[0].name), std::string("Studio. A"));
  CHECK_EQ(std::string(found[0].type), std::string("_osc._udp"));
  CHECK_EQ(std::string(found[0].host), std::string("192.168.1.20"));
  CHECK_EQ(found[0].port, uint16_t(8000));
  CHECK_EQ(std::string(found[0].txt), std::string("proto=osc;ver=1.0"));
  auto ptrs = dn::extractPointers(back, "_osc._udp");
  REQUIRE_EQ(ptrs.size(), size_t(1));
  CHECK_EQ(ptrs[0].first, std::string("Studio. A"));

  // Other service types are not reported.
  found.clear();
  dn::extractInstances(back, "_midi._udp", found, lost);
  CHECK(found.empty());

  // Goodbye (ttl 0) reports the instance as lost.
  REQUIRE(dn::encode(dn::buildAnnouncement(r, nullptr, 0), wire, &err));
  REQUIRE(dn::decode(wire.data(), wire.size(), back, &err));
  found.clear();
  dn::extractInstances(back, "_osc._udp", found, lost);
  CHECK(found.empty());
  REQUIRE_EQ(lost.size(), size_t(1));
  CHECK_EQ(std::string(lost[0].name), std::string("Studio. A"));
}

SQ_TEST("dnssd: browse query matches adverts of its type only") {
  sq_discovery_record r = makeRec("Desk", "_osc._udp", "", 9000, "");
  dn::Message q = dn::buildBrowseQuery("_osc._udp");
  std::vector<uint8_t> wire;
  REQUIRE(dn::encode(q, wire));
  dn::Message back;
  REQUIRE(dn::decode(wire.data(), wire.size(), back));
  REQUIRE_EQ(back.questions.size(), size_t(1));
  CHECK_EQ(back.questions[0].type, uint16_t(dn::kTypePTR));
  CHECK(dn::queryMatches(back, r));
  CHECK(!dn::queryMatches(dn::buildBrowseQuery("_http._tcp"), r));
  dn::Message srvQ;
  dn::Question qq;
  qq.name = "desk._OSC._udp.local";
  qq.type = dn::kTypeSRV;
  srvQ.questions.push_back(qq);
  CHECK(dn::queryMatches(srvQ, r));
  // Responses are never treated as queries.
  dn::Message resp = dn::buildAnnouncement(r, nullptr, 120);
  CHECK(!dn::queryMatches(resp, r));
}

SQ_TEST("dnssd: encoder rejects invalid names") {
  dn::Message m;
  dn::Question q;
  q.name = std::string(64, 'a') + ".local";
  m.questions.push_back(q);
  std::vector<uint8_t> wire;
  std::string err;
  CHECK(!dn::encode(m, wire, &err));
  m.questions[0].name.clear();
  for (int i = 0; i < 9; ++i) m.questions[0].name += std::string(30, 'b') + ".";
  CHECK(!dn::encode(m, wire, &err));  // > 255 bytes
}

SQ_TEST("dnssd: decoder survives compression loops, truncation and corruption") {
  // Header + one question whose name is a pointer to itself.
  const uint8_t loop[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 12, 0, 1};
  dn::Message m;
  std::string err;
  CHECK(!dn::decode(loop, sizeof loop, m, &err));
  // Pointer past the end.
  const uint8_t far[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0, 12, 0, 1};
  CHECK(!dn::decode(far, sizeof far, m, &err));
  CHECK(!dn::decode(loop, 5, m, &err));
  CHECK(!dn::decode(nullptr, 0, m, &err));

  sq_discovery_record r = makeRec("Fuzz", "_osc._udp", "fz", 1234, "k=v;x=y");
  const uint8_t ip[4] = {10, 0, 0, 1};
  std::vector<uint8_t> wire;
  REQUIRE(dn::encode(dn::buildAnnouncement(r, ip, 120), wire));
  for (size_t n = 0; n < wire.size(); ++n) dn::decode(wire.data(), n, m);
  for (size_t i = 0; i < wire.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> c = wire;
      c[i] = static_cast<uint8_t>(c[i] ^ (1u << bit));
      if (dn::decode(c.data(), c.size(), m)) {
        std::vector<sq_discovery_record> f, l;
        dn::extractInstances(m, "_osc._udp", f, l);
        for (auto& x : f) CHECK(std::memchr(x.name, 0, sizeof x.name) != nullptr);
      }
    }
}
