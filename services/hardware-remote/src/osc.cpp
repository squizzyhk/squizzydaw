// OSC 1.0 codec. Big-endian, 4-byte aligned, strings NUL-terminated and padded.
#include "sq/services/hardware_remote/osc.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace sq::osc {
namespace {

void putU32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(static_cast<uint8_t>(v >> 24));
  o.push_back(static_cast<uint8_t>(v >> 16));
  o.push_back(static_cast<uint8_t>(v >> 8));
  o.push_back(static_cast<uint8_t>(v));
}
void putU64(std::vector<uint8_t>& o, uint64_t v) {
  putU32(o, static_cast<uint32_t>(v >> 32));
  putU32(o, static_cast<uint32_t>(v));
}
void pad4(std::vector<uint8_t>& o) {
  while (o.size() % 4) o.push_back(0);
}
void putString(std::vector<uint8_t>& o, const char* s, size_t n) {
  o.insert(o.end(), s, s + n);
  o.push_back(0);
  pad4(o);
}

uint32_t getU32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t getU64(const uint8_t* p) { return (uint64_t(getU32(p)) << 32) | getU32(p + 4); }

bool fail(std::string* err, const char* msg) {
  if (err) *err = msg;
  return false;
}

// Reads a padded OSC string at p[pos..size). Returns false if unterminated.
bool readString(const uint8_t* p, size_t size, size_t& pos, const char*& s, size_t& len) {
  if (pos >= size) return false;
  const void* nul = std::memchr(p + pos, 0, size - pos);
  if (!nul) return false;
  s = reinterpret_cast<const char*>(p + pos);
  len = static_cast<size_t>(static_cast<const uint8_t*>(nul) - (p + pos));
  size_t adv = (len + 4) & ~size_t(3);
  if (pos + adv > size) return false;
  pos += adv;
  return true;
}

bool decodeMessage(const uint8_t* p, size_t size, uint64_t tag, const MessageFn& fn, std::string* err) {
  size_t pos = 0;
  const char* addr = nullptr;
  size_t addrLen = 0;
  if (!readString(p, size, pos, addr, addrLen) || addrLen == 0 || addr[0] != '/')
    return fail(err, "osc: bad address");
  const char* tags = ",";
  size_t tagLen = 1;
  if (pos < size) {
    if (!readString(p, size, pos, tags, tagLen) || tagLen == 0 || tags[0] != ',') return fail(err, "osc: bad type tags");
  }
  // Older senders omit the type tag string entirely: treated as no arguments.
  std::vector<sq_osc_arg> args;
  args.reserve(tagLen);
  for (size_t t = 1; t < tagLen; ++t) {
    sq_osc_arg a{};
    char c = tags[t];
    switch (c) {
      case 'i':
      case 'c':
      case 'r':
      case 'm': {
        if (pos + 4 > size) return fail(err, "osc: truncated int32");
        uint32_t u = getU32(p + pos);
        pos += 4;
        a.type = 'i';
        a.i = (c == 'i') ? int64_t(int32_t(u)) : int64_t(u);
        a.d = double(a.i);
        break;
      }
      case 'h':
      case 't': {
        if (pos + 8 > size) return fail(err, "osc: truncated int64");
        uint64_t u = getU64(p + pos);
        pos += 8;
        a.type = 'h';
        a.i = int64_t(u);
        a.d = double(a.i);
        break;
      }
      case 'f': {
        if (pos + 4 > size) return fail(err, "osc: truncated float");
        uint32_t u = getU32(p + pos);
        pos += 4;
        float f;
        std::memcpy(&f, &u, 4);
        a.type = 'f';
        a.d = f;
        a.i = std::isfinite(f) ? int64_t(f) : 0;
        break;
      }
      case 'd': {
        if (pos + 8 > size) return fail(err, "osc: truncated double");
        uint64_t u = getU64(p + pos);
        pos += 8;
        double d;
        std::memcpy(&d, &u, 8);
        a.type = 'd';
        a.d = d;
        a.i = std::isfinite(d) ? int64_t(d) : 0;
        break;
      }
      case 's':
      case 'S': {
        const char* s = nullptr;
        size_t n = 0;
        if (!readString(p, size, pos, s, n)) return fail(err, "osc: bad string arg");
        a.type = 's';
        a.s = s;
        break;
      }
      case 'b': {
        if (pos + 4 > size) return fail(err, "osc: truncated blob size");
        uint32_t n = getU32(p + pos);
        pos += 4;
        size_t adv = (size_t(n) + 3) & ~size_t(3);
        if (n > size || pos + adv > size) return fail(err, "osc: truncated blob");
        a.type = 'b';
        a.blob = p + pos;
        a.blob_size = n;
        pos += adv;
        break;
      }
      case 'T':
        a.type = 'T';
        a.i = 1;
        a.d = 1;
        break;
      case 'F':
        a.type = 'F';
        break;
      case 'N':
        a.type = 'N';
        break;
      case 'I':
        a.type = 'd';
        a.d = std::numeric_limits<double>::infinity();
        break;
      case '[':
      case ']':
        continue;
      default:
        return fail(err, "osc: unknown type tag");
    }
    args.push_back(a);
  }
  // fn may be called with an address that is not NUL-terminated at addrLen? It is: readString
  // guarantees the NUL.
  fn(addr, args.data(), static_cast<uint32_t>(args.size()), tag);
  return true;
}

bool decodeAny(const uint8_t* p, size_t size, uint64_t tag, const MessageFn& fn, std::string* err, int depth) {
  if (size < 4 || size % 4) return fail(err, "osc: packet size not a multiple of 4");
  if (p[0] == '/') return decodeMessage(p, size, tag, fn, err);
  if (p[0] != '#') return fail(err, "osc: not a message or bundle");
  if (depth > 8) return fail(err, "osc: bundles nested too deep");
  if (size < 16 || std::memcmp(p, "#bundle\0", 8) != 0) return fail(err, "osc: bad bundle header");
  uint64_t inner = getU64(p + 8);
  size_t pos = 16;
  while (pos < size) {
    if (pos + 4 > size) return fail(err, "osc: truncated bundle element size");
    uint32_t n = getU32(p + pos);
    pos += 4;
    if (n > size - pos) return fail(err, "osc: truncated bundle element");
    if (!decodeAny(p + pos, n, inner, fn, err, depth + 1)) return false;
    pos += n;
  }
  return true;
}

bool validAddress(const char* a) {
  if (!a || a[0] != '/') return false;
  for (const char* c = a; *c; ++c)
    if (*c == ' ' || *c == '#' || static_cast<unsigned char>(*c) < 0x20) return false;
  return true;
}

// Work limits for one patternMatch call. Patterns arrive from the network: without a budget,
// "/*a*a*a...*b" is exponential and a long run of '*' or '{' recurses deep enough to overflow
// the receive thread's stack. A pattern that exceeds either limit simply does not match.
struct MatchBudget {
  long steps = 200000;
  int depth = 0;
};
constexpr int kMaxMatchDepth = 32;

// Matches one address part (no '/') against one pattern part.
bool matchPart(const char* pat, const char* pe, const char* s, const char* se, MatchBudget& b) {
  if (++b.depth > kMaxMatchDepth) {
    b.steps = -1;
    --b.depth;
    return false;
  }
  struct DepthGuard {
    MatchBudget& b;
    ~DepthGuard() { --b.depth; }
  } guard{b};
  while (pat < pe) {
    if (--b.steps < 0) return false;
    char c = *pat;
    if (c == '*') {
      while (pat < pe && *pat == '*') ++pat;  // "**" == "*"
      if (pat == pe) return true;
      for (const char* t = s; t <= se; ++t) {
        if (matchPart(pat, pe, t, se, b)) return true;
        if (b.steps < 0) return false;
      }
      return false;
    }
    if (s >= se) return false;
    if (c == '?') {
      ++pat;
      ++s;
      continue;
    }
    if (c == '[') {
      const char* close = pat + 1;
      while (close < pe && *close != ']') ++close;
      if (close >= pe) return false;
      const char* q = pat + 1;
      bool negate = false;
      if (q < close && *q == '!') {
        negate = true;
        ++q;
      }
      bool hit = false;
      while (q < close) {
        if (q + 2 < close && q[1] == '-') {
          if (*s >= q[0] && *s <= q[2]) hit = true;
          q += 3;
        } else {
          if (*s == *q) hit = true;
          ++q;
        }
      }
      if (hit == negate) return false;
      pat = close + 1;
      ++s;
      continue;
    }
    if (c == '{') {
      const char* close = pat + 1;
      while (close < pe && *close != '}') ++close;
      if (close >= pe) return false;
      const char* alt = pat + 1;
      while (alt <= close) {
        const char* end = alt;
        while (end < close && *end != ',') ++end;
        size_t n = static_cast<size_t>(end - alt);
        if (static_cast<size_t>(se - s) >= n && std::memcmp(alt, s, n) == 0 && matchPart(close + 1, pe, s + n, se, b))
          return true;
        if (b.steps < 0) return false;
        alt = end + 1;
      }
      return false;
    }
    if (c != *s) return false;
    ++pat;
    ++s;
  }
  return s == se;
}

}  // namespace

bool encodeMessage(const char* address, const sq_osc_arg* args, uint32_t count, std::vector<uint8_t>& out,
                   std::string* err) {
  out.clear();
  if (!validAddress(address)) return fail(err, "osc: invalid address");
  if (count && !args) return fail(err, "osc: null args");
  putString(out, address, std::strlen(address));
  std::string tags(",");
  for (uint32_t k = 0; k < count; ++k) {
    char t = args[k].type;
    if (!std::strchr("ihfdsbTFN", t) || t == 0) return fail(err, "osc: unsupported argument type");
    tags.push_back(t);
  }
  putString(out, tags.data(), tags.size());
  for (uint32_t k = 0; k < count; ++k) {
    const sq_osc_arg& a = args[k];
    switch (a.type) {
      case 'i':
        putU32(out, static_cast<uint32_t>(static_cast<int32_t>(a.i)));
        break;
      case 'h':
        putU64(out, static_cast<uint64_t>(a.i));
        break;
      case 'f': {
        float f = static_cast<float>(a.d);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        putU32(out, u);
        break;
      }
      case 'd': {
        uint64_t u;
        std::memcpy(&u, &a.d, 8);
        putU64(out, u);
        break;
      }
      case 's': {
        const char* s = a.s ? a.s : "";
        putString(out, s, std::strlen(s));
        break;
      }
      case 'b': {
        if (a.blob_size && !a.blob) return fail(err, "osc: null blob");
        putU32(out, a.blob_size);
        if (a.blob_size) out.insert(out.end(), static_cast<const uint8_t*>(a.blob), static_cast<const uint8_t*>(a.blob) + a.blob_size);
        pad4(out);
        break;
      }
      default:
        break;  // T F N carry no data
    }
  }
  return true;
}

void encodeBundle(uint64_t timeTag, const std::vector<std::vector<uint8_t>>& elements, std::vector<uint8_t>& out) {
  out.clear();
  static const char kHeader[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
  out.insert(out.end(), kHeader, kHeader + 8);
  putU64(out, timeTag);
  for (const auto& e : elements) {
    putU32(out, static_cast<uint32_t>(e.size()));
    out.insert(out.end(), e.begin(), e.end());
  }
}

uint64_t timeTagFromUnixNs(uint64_t unixNs) {
  uint64_t secs = unixNs / 1000000000ull + kNtpUnixOffsetSeconds;
  uint64_t ns = unixNs % 1000000000ull;
  uint64_t frac = ((ns << 32) + 500000000ull) / 1000000000ull;  // ns < 2^30: no overflow
  if (frac > 0xFFFFFFFFull) {
    frac = 0;
    ++secs;
  }
  return (secs << 32) | frac;
}

uint64_t timeTagToUnixNs(uint64_t timeTag) {
  uint64_t secs = timeTag >> 32;
  uint64_t frac = timeTag & 0xFFFFFFFFull;
  if (secs < kNtpUnixOffsetSeconds) return 0;
  uint64_t ns = (frac * 1000000000ull + 0x80000000ull) >> 32;
  return (secs - kNtpUnixOffsetSeconds) * 1000000000ull + ns;
}

bool decodePacket(const uint8_t* data, size_t size, const MessageFn& fn, std::string* err) {
  if (!data) return fail(err, "osc: null packet");
  return decodeAny(data, size, kImmediately, fn, err, 0);
}

bool patternMatch(const char* pattern, const char* address) {
  if (!pattern || !address || *pattern != '/' || *address != '/') return false;
  const char* p = pattern;
  const char* a = address;
  MatchBudget budget;
  while (*p && *a) {
    if (*p != '/' || *a != '/') return false;
    ++p;
    ++a;
    const char* pe = p;
    // '/' inside [] or {} is not a separator in practice; OSC forbids it there anyway.
    while (*pe && *pe != '/') ++pe;
    const char* ae = a;
    while (*ae && *ae != '/') ++ae;
    if (!matchPart(p, pe, a, ae, budget)) return false;
    p = pe;
    a = ae;
  }
  return *p == 0 && *a == 0;
}

std::vector<OwnedMessage> decodeAll(const uint8_t* data, size_t size, std::string* err) {
  std::vector<OwnedMessage> msgs;
  decodePacket(
      data, size,
      [&](const char* addr, const sq_osc_arg* args, uint32_t n, uint64_t tag) {
        OwnedMessage m;
        m.address = addr;
        m.timeTag = tag;
        for (uint32_t k = 0; k < n; ++k) {
          OwnedArg o;
          o.type = args[k].type;
          o.i = args[k].i;
          o.d = args[k].d;
          if (args[k].s) o.s = args[k].s;
          if (args[k].blob)
            o.blob.assign(static_cast<const uint8_t*>(args[k].blob),
                          static_cast<const uint8_t*>(args[k].blob) + args[k].blob_size);
          m.args.push_back(std::move(o));
        }
        msgs.push_back(std::move(m));
      },
      err);
  return msgs;
}

}  // namespace sq::osc
