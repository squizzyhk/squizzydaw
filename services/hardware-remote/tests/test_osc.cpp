// hardware-remote: OSC 1.0 codec (messages, bundles, time tags, pattern matching, malformed input).
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "sq/platform.h"
#include "sq/services/hardware_remote/osc.hpp"
#include "sqtest.hpp"

namespace {

sq_osc_arg argI(int64_t v) {
  sq_osc_arg a{};
  a.type = 'i';
  a.i = v;
  return a;
}
sq_osc_arg argH(int64_t v) {
  sq_osc_arg a{};
  a.type = 'h';
  a.i = v;
  return a;
}
sq_osc_arg argF(double v) {
  sq_osc_arg a{};
  a.type = 'f';
  a.d = v;
  return a;
}
sq_osc_arg argD(double v) {
  sq_osc_arg a{};
  a.type = 'd';
  a.d = v;
  return a;
}
sq_osc_arg argS(const char* s) {
  sq_osc_arg a{};
  a.type = 's';
  a.s = s;
  return a;
}
sq_osc_arg argT(char t) {
  sq_osc_arg a{};
  a.type = t;
  return a;
}

std::vector<uint8_t> bytes(const char* s, size_t n) { return std::vector<uint8_t>(s, s + n); }

}  // namespace

SQ_TEST("osc: message wire layout matches the OSC 1.0 spec") {
  // "/oscillator/4/frequency" ,f 440.0  (the classic example from the specification)
  sq_osc_arg a = argF(440.0);
  std::vector<uint8_t> out;
  REQUIRE(sq::osc::encodeMessage("/oscillator/4/frequency", &a, 1, out));
  const char expect[] =
      "/oscillator/4/frequency\0"  // 23 chars + NUL = 24
      ",f\0\0"
      "\x43\xdc\x00\x00";
  REQUIRE_EQ(out.size(), sizeof expect - 1);
  CHECK(std::memcmp(out.data(), expect, out.size()) == 0);
}

SQ_TEST("osc: every argument type round-trips") {
  const uint8_t blob[5] = {1, 2, 3, 4, 5};
  sq_osc_arg b{};
  b.type = 'b';
  b.blob = blob;
  b.blob_size = 5;
  std::vector<sq_osc_arg> args = {argI(-7),   argH(-(int64_t(1) << 40)), argF(0.25), argD(-1.0e-9), argS("hello"),
                                  argS(""),   b,                         argT('T'),  argT('F'),     argT('N'),
                                  argS(nullptr)};
  std::vector<uint8_t> out;
  REQUIRE(sq::osc::encodeMessage("/a/b", args.data(), static_cast<uint32_t>(args.size()), out));
  CHECK_EQ(out.size() % 4, size_t(0));
  std::string err;
  auto msgs = sq::osc::decodeAll(out.data(), out.size(), &err);
  REQUIRE_EQ(msgs.size(), size_t(1));
  const auto& m = msgs[0];
  CHECK_EQ(m.address, std::string("/a/b"));
  CHECK_EQ(m.timeTag, sq::osc::kImmediately);
  REQUIRE_EQ(m.args.size(), args.size());
  CHECK_EQ(m.args[0].type, 'i');
  CHECK_EQ(m.args[0].i, int64_t(-7));
  CHECK_EQ(m.args[1].type, 'h');
  CHECK_EQ(m.args[1].i, -(int64_t(1) << 40));
  CHECK_EQ(m.args[2].type, 'f');
  CHECK_EQ(m.args[2].d, 0.25);
  CHECK_EQ(m.args[3].type, 'd');
  CHECK_EQ(m.args[3].d, -1.0e-9);
  CHECK_EQ(m.args[4].s, std::string("hello"));
  CHECK_EQ(m.args[5].s, std::string(""));
  CHECK_EQ(m.args[6].type, 'b');
  REQUIRE_EQ(m.args[6].blob.size(), size_t(5));
  CHECK_EQ(m.args[6].blob[4], uint8_t(5));
  CHECK_EQ(m.args[7].type, 'T');
  CHECK_EQ(m.args[8].type, 'F');
  CHECK_EQ(m.args[9].type, 'N');
  CHECK_EQ(m.args[10].s, std::string(""));
}

SQ_TEST("osc: string padding at every length") {
  for (int len = 0; len < 9; ++len) {
    std::string s(static_cast<size_t>(len), 'x');
    sq_osc_arg a = argS(s.c_str());
    std::vector<uint8_t> out;
    REQUIRE(sq::osc::encodeMessage("/s", &a, 1, out));
    CHECK_EQ(out.size() % 4, size_t(0));
    auto msgs = sq::osc::decodeAll(out.data(), out.size());
    REQUIRE_EQ(msgs.size(), size_t(1));
    CHECK_EQ(msgs[0].args[0].s, s);
  }
}

SQ_TEST("osc: invalid addresses and types are rejected on encode") {
  std::vector<uint8_t> out;
  std::string err;
  CHECK(!sq::osc::encodeMessage("noslash", nullptr, 0, out, &err));
  CHECK(!sq::osc::encodeMessage("/with space", nullptr, 0, out, &err));
  CHECK(!sq::osc::encodeMessage("/hash#", nullptr, 0, out, &err));
  sq_osc_arg bad = argT('x');
  CHECK(!sq::osc::encodeMessage("/ok", &bad, 1, out, &err));
  CHECK(!sq::osc::encodeMessage("/ok", nullptr, 1, out, &err));
  sq_osc_arg nullBlob{};
  nullBlob.type = 'b';
  nullBlob.blob_size = 3;
  CHECK(!sq::osc::encodeMessage("/ok", &nullBlob, 1, out, &err));
}

SQ_TEST("osc: nested bundles keep the innermost time tag, depth first") {
  sq_osc_arg one = argI(1), two = argI(2), three = argI(3);
  std::vector<uint8_t> m1, m2, m3, inner, outer;
  REQUIRE(sq::osc::encodeMessage("/one", &one, 1, m1));
  REQUIRE(sq::osc::encodeMessage("/two", &two, 1, m2));
  REQUIRE(sq::osc::encodeMessage("/three", &three, 1, m3));
  const uint64_t tagOuter = (uint64_t(3900000000u) << 32) | 0x80000000u;
  const uint64_t tagInner = tagOuter + (uint64_t(1) << 32);
  sq::osc::encodeBundle(tagInner, {m2}, inner);
  sq::osc::encodeBundle(tagOuter, {m1, inner, m3}, outer);
  CHECK(std::memcmp(outer.data(), "#bundle\0", 8) == 0);
  auto msgs = sq::osc::decodeAll(outer.data(), outer.size());
  REQUIRE_EQ(msgs.size(), size_t(3));
  CHECK_EQ(msgs[0].address, std::string("/one"));
  CHECK_EQ(msgs[0].timeTag, tagOuter);
  CHECK_EQ(msgs[1].address, std::string("/two"));
  CHECK_EQ(msgs[1].timeTag, tagInner);
  CHECK_EQ(msgs[2].address, std::string("/three"));
  CHECK_EQ(msgs[2].args[0].i, int64_t(3));
  CHECK_EQ(msgs[2].timeTag, tagOuter);
  // An empty bundle is valid and yields nothing.
  std::vector<uint8_t> empty;
  sq::osc::encodeBundle(sq::osc::kImmediately, {}, empty);
  CHECK_EQ(empty.size(), size_t(16));
  std::string err;
  CHECK(sq::osc::decodePacket(empty.data(), empty.size(), [](const char*, const sq_osc_arg*, uint32_t, uint64_t) {}, &err));
}

SQ_TEST("osc: time tags convert to and from Unix nanoseconds") {
  // 1970-01-01 is NTP second 2208988800.
  CHECK_EQ(sq::osc::timeTagFromUnixNs(0), sq::osc::kNtpUnixOffsetSeconds << 32);
  CHECK_EQ(sq::osc::timeTagToUnixNs(sq::osc::kNtpUnixOffsetSeconds << 32), uint64_t(0));
  // Half a second is fraction 0x80000000.
  CHECK_EQ(sq::osc::timeTagFromUnixNs(500000000ull) & 0xFFFFFFFFull, uint64_t(0x80000000ull));
  const uint64_t samples[] = {1ull, 999999999ull, 1700000000123456789ull, 2051222400000000001ull};  // last: 2035, inside NTP era 0
  for (uint64_t ns : samples) {
    uint64_t back = sq::osc::timeTagToUnixNs(sq::osc::timeTagFromUnixNs(ns));
    uint64_t diff = back > ns ? back - ns : ns - back;
    CHECK(diff <= 1);
  }
  CHECK_EQ(sq::osc::timeTagToUnixNs(sq::osc::kImmediately), uint64_t(0));
}

SQ_TEST("osc: decoder accepts the optional/extended tags") {
  // ",SchtI[i]" with values; S symbol, c char, h int64, t timetag, I infinitum, array brackets.
  std::vector<uint8_t> p;
  auto put = [&](const void* d, size_t n) { p.insert(p.end(), static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + n); };
  put("/x\0\0", 4);
  put(",Scht", 5);
  put("I[i]\0\0\0", 7);  // ",SchtI[i]" = 9 chars + NUL + 2 pad -> 12 bytes total
  put("sym\0", 4);
  put("\0\0\0A", 4);
  put("\0\0\0\0\0\0\0\x05", 8);
  put("\0\0\0\x01\0\0\0\x02", 8);
  put("\0\0\0\x09", 4);
  REQUIRE_EQ(p.size() % 4, size_t(0));
  std::string err;
  auto msgs = sq::osc::decodeAll(p.data(), p.size(), &err);
  REQUIRE_EQ(msgs.size(), size_t(1));
  const auto& a = msgs[0].args;
  REQUIRE_EQ(a.size(), size_t(6));
  CHECK_EQ(a[0].s, std::string("sym"));
  CHECK_EQ(a[1].i, int64_t('A'));
  CHECK_EQ(a[2].i, int64_t(5));
  CHECK_EQ(a[3].i, int64_t((uint64_t(1) << 32) | 2));
  CHECK(std::isinf(a[4].d));
  CHECK_EQ(a[5].i, int64_t(9));
}

SQ_TEST("osc: a message without a type tag string has no arguments") {
  auto p = bytes("/legacy\0", 8);
  auto msgs = sq::osc::decodeAll(p.data(), p.size());
  REQUIRE_EQ(msgs.size(), size_t(1));
  CHECK(msgs[0].args.empty());
}

SQ_TEST("osc: malformed packets are rejected without reading out of bounds") {
  auto none = [](const char*, const sq_osc_arg*, uint32_t, uint64_t) {};
  std::string err;
  CHECK(!sq::osc::decodePacket(nullptr, 0, none, &err));
  auto odd = bytes("/ab\0\0", 5);
  CHECK(!sq::osc::decodePacket(odd.data(), odd.size(), none, &err));  // not a multiple of 4
  auto unterminated = bytes("/abc", 4);
  CHECK(!sq::osc::decodePacket(unterminated.data(), unterminated.size(), none, &err));
  auto badTag = bytes("/a\0\0,x\0\0", 8);
  CHECK(!sq::osc::decodePacket(badTag.data(), badTag.size(), none, &err));
  auto truncInt = bytes("/a\0\0,i\0\0", 8);
  CHECK(!sq::osc::decodePacket(truncInt.data(), truncInt.size(), none, &err));
  auto hugeBlob = bytes("/a\0\0,b\0\0\xff\xff\xff\xf0", 12);
  CHECK(!sq::osc::decodePacket(hugeBlob.data(), hugeBlob.size(), none, &err));
  // Bundle element whose size runs past the end.
  std::vector<uint8_t> bundle;
  sq::osc::encodeBundle(1, {}, bundle);
  const uint8_t big[4] = {0, 0, 1, 0};
  bundle.insert(bundle.end(), big, big + 4);
  CHECK(!sq::osc::decodePacket(bundle.data(), bundle.size(), none, &err));
  // Bundles nested deeper than the limit.
  std::vector<uint8_t> msg, nest;
  sq_osc_arg a = argI(1);
  REQUIRE(sq::osc::encodeMessage("/deep", &a, 1, msg));
  nest = msg;
  for (int i = 0; i < 12; ++i) {
    std::vector<uint8_t> next;
    sq::osc::encodeBundle(1, {nest}, next);
    nest.swap(next);
  }
  CHECK(!sq::osc::decodePacket(nest.data(), nest.size(), none, &err));
  // Every truncation of a valid packet must fail cleanly or decode a prefix, never crash.
  std::vector<uint8_t> full, m2;
  sq_osc_arg args[3] = {argS("abcdef"), argD(1.5), argI(3)};
  REQUIRE(sq::osc::encodeMessage("/t/u", args, 3, m2));
  sq::osc::encodeBundle(7, {m2, m2}, full);
  for (size_t n = 0; n < full.size(); ++n) {
    std::vector<uint8_t> cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
    sq::osc::decodePacket(cut.empty() ? nullptr : cut.data(), cut.size(), none, &err);
  }
  // Single-bit corruption sweep.
  for (size_t i = 0; i < full.size(); ++i)
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<uint8_t> c = full;
      c[i] = static_cast<uint8_t>(c[i] ^ (1u << bit));
      sq::osc::decodePacket(c.data(), c.size(), none, &err);
    }
}

SQ_TEST("osc: address pattern matching") {
  using sq::osc::patternMatch;
  CHECK(patternMatch("/mixer/track/1/volume", "/mixer/track/1/volume"));
  CHECK(!patternMatch("/mixer/track/1/volume", "/mixer/track/2/volume"));
  CHECK(patternMatch("/mixer/track/?/volume", "/mixer/track/7/volume"));
  CHECK(!patternMatch("/mixer/track/?/volume", "/mixer/track/12/volume"));
  CHECK(patternMatch("/mixer/*/volume", "/mixer/master/volume"));
  CHECK(!patternMatch("/mixer/*", "/mixer/track/1"));  // '*' never crosses '/'
  CHECK(patternMatch("/mixer/track/[0-9]", "/mixer/track/5"));
  CHECK(!patternMatch("/mixer/track/[!0-9]", "/mixer/track/5"));
  CHECK(patternMatch("/mixer/track/[!0-9]", "/mixer/track/x"));
  CHECK(patternMatch("/mixer/track/[abc]", "/mixer/track/b"));
  CHECK(patternMatch("/fx/{reverb,delay}/mix", "/fx/delay/mix"));
  CHECK(!patternMatch("/fx/{reverb,delay}/mix", "/fx/chorus/mix"));
  CHECK(patternMatch("/a*b*c", "/aXXbYYc"));
  CHECK(patternMatch("/*", "/"));
  CHECK(!patternMatch("/a", "/a/b"));
  CHECK(!patternMatch("a", "/a"));
  CHECK(!patternMatch("/[abc", "/a"));  // unterminated bracket never matches
}

// Verifier regression: patterns come from the network. Before the work budget, "/*a*a...*b"
// against a long run of 'a' took exponential time, and a long run of '*' or '{' recursed once per
// character on the receive thread's stack.
SQ_TEST("osc: hostile patterns are bounded in time and stack depth") {
  using sq::osc::patternMatch;
  std::string star = "/";
  for (int i = 0; i < 30; ++i) star += "*a";
  star += "b";
  std::string addr = "/" + std::string(60, 'a');
  uint64_t t0 = sq_time_monotonic_ns();
  CHECK(!patternMatch(star.c_str(), addr.c_str()));
  std::string deep = "/" + std::string(60000, '*') + "x" + std::string(60000, '*') + "y";
  CHECK(!patternMatch(deep.c_str(), ("/" + std::string(60000, 'q')).c_str()));
  std::string braces = "/";
  for (int i = 0; i < 20000; ++i) braces += "{a,b}";
  CHECK(!patternMatch(braces.c_str(), ("/" + std::string(20001, 'a')).c_str()));
  uint64_t ms = (sq_time_monotonic_ns() - t0) / 1000000;
  CHECK(ms < 2000);
  // Legitimate patterns still match.
  CHECK(patternMatch("/**/x", "/abc/x"));
  CHECK(patternMatch("/a*b*c*d*e", "/a1b2c3d4e"));
  CHECK(patternMatch("/{x,y}*{1,2}", "/yzz2"));
}
