// DNS wire codec for mDNS / DNS-SD.
#include "sq/services/hardware_remote/dnssd_wire.hpp"

#include <cstdio>
#include <cstring>
#include <map>

namespace sq::dnssd {
namespace {

bool fail(std::string* err, const char* msg) {
  if (err) *err = msg;
  return false;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string lowerStr(std::string s) {
  for (auto& c : s) c = lower(c);
  return s;
}

void put16(std::vector<uint8_t>& o, uint16_t v) {
  o.push_back(uint8_t(v >> 8));
  o.push_back(uint8_t(v));
}
void put32(std::vector<uint8_t>& o, uint32_t v) {
  put16(o, uint16_t(v >> 16));
  put16(o, uint16_t(v));
}
uint16_t get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) { return (uint32_t(get16(p)) << 16) | get16(p + 2); }

void safeCopy(char* dst, size_t cap, const std::string& s) {
  size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  std::memcpy(dst, s.data(), n);
  dst[n] = 0;
}

class Writer {
 public:
  explicit Writer(std::vector<uint8_t>& o) : o_(o) {}

  bool name(const std::string& n, std::string* err) {
    std::vector<std::string> labels = splitName(n);
    size_t total = 1;
    for (const auto& l : labels) {
      if (l.empty() || l.size() > 63) return fail(err, "dns: bad label length");
      total += l.size() + 1;
    }
    if (total > 255) return fail(err, "dns: name too long");
    for (size_t i = 0; i < labels.size(); ++i) {
      std::vector<std::string> rest(labels.begin() + static_cast<std::ptrdiff_t>(i), labels.end());
      std::string key = lowerStr(joinName(rest));
      auto it = offsets_.find(key);
      if (it != offsets_.end()) {
        put16(o_, uint16_t(0xC000 | it->second));
        return true;
      }
      if (o_.size() < 0x3FFF) offsets_[key] = uint16_t(o_.size());
      o_.push_back(uint8_t(labels[i].size()));
      o_.insert(o_.end(), labels[i].begin(), labels[i].end());
    }
    o_.push_back(0);
    return true;
  }

  bool record(const Record& r, std::string* err) {
    if (!name(r.name, err)) return false;
    put16(o_, r.type);
    put16(o_, uint16_t((r.cls & 0x7FFF) | (r.cacheFlush ? 0x8000 : 0)));
    put32(o_, r.ttl);
    size_t lenAt = o_.size();
    put16(o_, 0);
    size_t start = o_.size();
    switch (r.type) {
      case kTypePTR:
        if (!name(r.target, err)) return false;
        break;
      case kTypeSRV:
        put16(o_, r.priority);
        put16(o_, r.weight);
        put16(o_, r.port);
        if (!name(r.target, err)) return false;
        break;
      case kTypeTXT:
        if (r.txt.empty()) {
          o_.push_back(0);  // RFC 6763 6.1: an empty TXT record holds one empty string
        } else {
          for (const auto& s : r.txt) {
            if (s.size() > 255) return fail(err, "dns: TXT string too long");
            o_.push_back(uint8_t(s.size()));
            o_.insert(o_.end(), s.begin(), s.end());
          }
        }
        break;
      case kTypeA:
        o_.insert(o_.end(), r.addr, r.addr + 4);
        break;
      case kTypeAAAA:
        o_.insert(o_.end(), r.addr, r.addr + 16);
        break;
      default:
        o_.insert(o_.end(), r.raw.begin(), r.raw.end());
        break;
    }
    size_t len = o_.size() - start;
    if (len > 0xFFFF) return fail(err, "dns: rdata too long");
    o_[lenAt] = uint8_t(len >> 8);
    o_[lenAt + 1] = uint8_t(len);
    return true;
  }

 private:
  std::vector<uint8_t>& o_;
  std::map<std::string, uint16_t> offsets_;
};

class Reader {
 public:
  Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}

  bool name(size_t& pos, std::string& out) {
    std::vector<std::string> labels;
    size_t p = pos;
    bool jumped = false;
    int hops = 0;
    size_t total = 0;
    for (;;) {
      if (p >= n_) return false;
      uint8_t len = d_[p];
      if ((len & 0xC0) == 0xC0) {
        if (p + 1 >= n_) return false;
        size_t target = size_t(((len & 0x3F) << 8) | d_[p + 1]);
        if (!jumped) pos = p + 2;
        jumped = true;
        if (++hops > 64 || target >= n_) return false;  // loop protection
        p = target;
        continue;
      }
      if (len & 0xC0) return false;  // extended label types are not supported
      if (len == 0) {
        if (!jumped) pos = p + 1;
        break;
      }
      if (p + 1 + len > n_) return false;
      total += size_t(len) + 1;
      if (total > 255) return false;
      labels.emplace_back(reinterpret_cast<const char*>(d_ + p + 1), len);
      p += 1 + size_t(len);
    }
    out = joinName(labels);
    return true;
  }

  bool question(size_t& pos, Question& q) {
    if (!name(pos, q.name) || pos + 4 > n_) return false;
    q.type = get16(d_ + pos);
    uint16_t c = get16(d_ + pos + 2);
    q.cls = c & 0x7FFF;
    q.unicastResponse = (c & 0x8000) != 0;
    pos += 4;
    return true;
  }

  bool record(size_t& pos, Record& r) {
    if (!name(pos, r.name) || pos + 10 > n_) return false;
    r.type = get16(d_ + pos);
    uint16_t c = get16(d_ + pos + 2);
    r.cls = c & 0x7FFF;
    r.cacheFlush = (c & 0x8000) != 0;
    r.ttl = get32(d_ + pos + 4);
    uint16_t len = get16(d_ + pos + 8);
    pos += 10;
    if (pos + len > n_) return false;
    size_t end = pos + len;
    size_t p = pos;
    switch (r.type) {
      case kTypePTR:
        if (!name(p, r.target) || p > end) return false;
        break;
      case kTypeSRV:
        if (len < 7) return false;
        r.priority = get16(d_ + p);
        r.weight = get16(d_ + p + 2);
        r.port = get16(d_ + p + 4);
        p += 6;
        if (!name(p, r.target) || p > end) return false;
        break;
      case kTypeTXT:
        while (p < end) {
          uint8_t l = d_[p++];
          if (p + l > end) return false;
          if (l) r.txt.emplace_back(reinterpret_cast<const char*>(d_ + p), l);
          p += l;
        }
        break;
      case kTypeA:
        if (len != 4) return false;
        std::memcpy(r.addr, d_ + p, 4);
        break;
      case kTypeAAAA:
        if (len != 16) return false;
        std::memcpy(r.addr, d_ + p, 16);
        break;
      default:
        r.raw.assign(d_ + p, d_ + end);
        break;
    }
    pos = end;
    return true;
  }

 private:
  const uint8_t* d_;
  size_t n_;
};

// "Name._osc._udp.local" with serviceDomain "_osc._udp.local" -> "Name". Empty if no match.
std::string instanceLabel(const std::string& full, const std::string& domain) {
  std::vector<std::string> f = splitName(full);
  std::vector<std::string> d = splitName(domain);
  if (f.size() != d.size() + 1) return {};
  for (size_t i = 0; i < d.size(); ++i)
    if (lowerStr(f[i + 1]) != lowerStr(d[i])) return {};
  return f[0];
}

std::string stripLocal(const std::string& domain) {
  std::vector<std::string> l = splitName(domain);
  if (!l.empty() && lowerStr(l.back()) == "local") l.pop_back();
  return joinName(l);
}

}  // namespace

std::string instanceFullName(const sq_discovery_record& r) {
  std::vector<std::string> labels{std::string(r.name)};
  for (auto& l : splitName(serviceDomain(r.type))) labels.push_back(l);
  return joinName(labels);
}

std::string hostTarget(const char* host) {
  std::string h = host ? host : "";
  while (!h.empty() && h.back() == '.') h.pop_back();
  if (h.empty()) h = "squizzy";
  if (h.find('.') == std::string::npos) h += ".local";
  return h;
}

bool splitInstanceName(const std::string& fullName, std::string& instance, std::string& type, std::string& domain) {
  std::vector<std::string> l = splitName(fullName);
  // instance, _service, _proto, domain labels...
  if (l.size() < 3) return false;
  for (size_t i = 1; i + 1 < l.size(); ++i) {
    std::string proto = lowerStr(l[i + 1]);
    if (l[i].size() > 1 && l[i][0] == '_' && (proto == "_udp" || proto == "_tcp")) {
      std::vector<std::string> inst(l.begin(), l.begin() + static_cast<std::ptrdiff_t>(i));
      if (inst.size() != 1) return false;  // instance names are a single (escaped) label
      instance = inst[0];
      type = joinName({l[i], l[i + 1]});
      domain = joinName(std::vector<std::string>(l.begin() + static_cast<std::ptrdiff_t>(i + 2), l.end()));
      return true;
    }
  }
  return false;
}

std::vector<TxtPair> txtPairs(const char* txt) {
  std::vector<TxtPair> out;
  for (const std::string& s : txtFromString(txt)) {
    size_t eq = s.find('=');
    if (eq == 0) continue;  // RFC 6763 6.4: an empty key is invalid
    if (eq == std::string::npos)
      out.emplace_back(s, std::string());
    else
      out.emplace_back(s.substr(0, eq), s.substr(eq + 1));
  }
  return out;
}

std::string txtFromPairs(const std::vector<TxtPair>& pairs) {
  std::vector<std::string> strings;
  for (const auto& [k, v] : pairs) {
    if (k.empty()) continue;
    strings.push_back(v.empty() ? k : k + "=" + v);
  }
  return txtToString(strings);
}

bool makeRecord(const std::string& fullName, const std::string& host, uint16_t port, const std::vector<TxtPair>& txt,
                sq_discovery_record& out) {
  std::string instance, type, domain;
  if (!splitInstanceName(fullName, instance, type, domain)) return false;
  out = sq_discovery_record{};
  out.struct_size = sizeof out;
  safeCopy(out.name, sizeof out.name, instance);
  safeCopy(out.type, sizeof out.type, type);
  std::string h = host;
  while (!h.empty() && h.back() == '.') h.pop_back();
  safeCopy(out.host, sizeof out.host, h);
  out.port = port;
  safeCopy(out.txt, sizeof out.txt, txtFromPairs(txt));
  return true;
}

std::vector<std::pair<std::string, uint32_t>> extractPointers(const Message& m, const char* serviceType) {
  std::vector<std::pair<std::string, uint32_t>> out;
  std::string domain = serviceDomain(serviceType);
  for (const auto* sec : {&m.answers, &m.authorities, &m.additionals})
    for (const auto& r : *sec) {
      if (r.type != kTypePTR || !sameName(r.name, domain)) continue;
      std::string label = instanceLabel(r.target, domain);
      if (!label.empty()) out.emplace_back(label, r.ttl);
    }
  return out;
}

std::vector<std::string> splitName(const std::string& name) {
  std::vector<std::string> labels;
  std::string cur;
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if (c == '\\' && i + 1 < name.size()) {
      cur.push_back(name[++i]);
    } else if (c == '.') {
      if (!cur.empty()) labels.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) labels.push_back(cur);
  return labels;
}

std::string joinName(const std::vector<std::string>& labels) {
  std::string out;
  for (size_t i = 0; i < labels.size(); ++i) {
    if (i) out.push_back('.');
    for (char c : labels[i]) {
      if (c == '.' || c == '\\') out.push_back('\\');
      out.push_back(c);
    }
  }
  return out;
}

bool sameName(const std::string& a, const std::string& b) {
  return lowerStr(joinName(splitName(a))) == lowerStr(joinName(splitName(b)));
}

std::string serviceDomain(const char* serviceType) {
  std::vector<std::string> l = splitName(serviceType ? serviceType : "");
  if (l.empty() || lowerStr(l.back()) != "local") l.push_back("local");
  return joinName(l);
}

std::vector<std::string> txtFromString(const char* txt) {
  std::vector<std::string> out;
  if (!txt) return out;
  std::string cur;
  for (const char* c = txt; *c; ++c) {
    if (*c == '\\' && c[1]) {
      cur.push_back(*++c);
    } else if (*c == ';') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(*c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string txtToString(const std::vector<std::string>& strings) {
  std::string out;
  for (const auto& s : strings) {
    if (s.empty()) continue;
    if (!out.empty()) out.push_back(';');
    for (char c : s) {
      if (c == ';' || c == '\\') out.push_back('\\');
      out.push_back(c);
    }
  }
  return out;
}

bool encode(const Message& m, std::vector<uint8_t>& out, std::string* err) {
  out.clear();
  put16(out, m.id);
  put16(out, m.flags);
  put16(out, uint16_t(m.questions.size()));
  put16(out, uint16_t(m.answers.size()));
  put16(out, uint16_t(m.authorities.size()));
  put16(out, uint16_t(m.additionals.size()));
  Writer w(out);
  for (const auto& q : m.questions) {
    if (!w.name(q.name, err)) return false;
    put16(out, q.type);
    put16(out, uint16_t((q.cls & 0x7FFF) | (q.unicastResponse ? 0x8000 : 0)));
  }
  for (const auto* sec : {&m.answers, &m.authorities, &m.additionals})
    for (const auto& r : *sec)
      if (!w.record(r, err)) return false;
  if (out.size() > 9000) return fail(err, "dns: message too large for mDNS");
  return true;
}

bool decode(const uint8_t* data, size_t size, Message& m, std::string* err) {
  m = Message{};
  if (!data || size < 12) return fail(err, "dns: short header");
  m.id = get16(data);
  m.flags = get16(data + 2);
  uint16_t qd = get16(data + 4), an = get16(data + 6), ns = get16(data + 8), ar = get16(data + 10);
  Reader r(data, size);
  size_t pos = 12;
  for (uint16_t i = 0; i < qd; ++i) {
    Question q;
    if (!r.question(pos, q)) return fail(err, "dns: bad question");
    m.questions.push_back(std::move(q));
  }
  struct Sec {
    uint16_t count;
    std::vector<Record>* dst;
  } secs[] = {{an, &m.answers}, {ns, &m.authorities}, {ar, &m.additionals}};
  for (auto& s : secs) {
    for (uint16_t i = 0; i < s.count; ++i) {
      Record rec;
      if (!r.record(pos, rec)) return fail(err, "dns: bad resource record");
      s.dst->push_back(std::move(rec));
    }
  }
  return true;
}

Message buildAnnouncement(const sq_discovery_record& record, const uint8_t* ipv4, uint32_t ttl) {
  Message m;
  m.flags = kFlagResponse;
  std::string domain = serviceDomain(record.type);
  std::string instance = instanceFullName(record);
  std::string host = hostTarget(record.host);

  Record ptr;
  ptr.name = domain;
  ptr.type = kTypePTR;
  ptr.ttl = ttl ? 4500 : 0;  // RFC 6762 10: 75 minutes for shared records
  ptr.target = instance;
  m.answers.push_back(ptr);

  Record srv;
  srv.name = instance;
  srv.type = kTypeSRV;
  srv.cacheFlush = true;
  srv.ttl = ttl;
  srv.port = record.port;
  srv.target = host;
  m.answers.push_back(srv);

  Record txt;
  txt.name = instance;
  txt.type = kTypeTXT;
  txt.cacheFlush = true;
  txt.ttl = ttl ? 4500 : 0;
  txt.txt = txtFromString(record.txt);
  m.answers.push_back(txt);

  if (ipv4) {
    Record a;
    a.name = host;
    a.type = kTypeA;
    a.cacheFlush = true;
    a.ttl = ttl;
    std::memcpy(a.addr, ipv4, 4);
    m.additionals.push_back(a);
  }
  return m;
}

Message buildBrowseQuery(const char* serviceType) {
  Message m;
  Question q;
  q.name = serviceDomain(serviceType);
  q.type = kTypePTR;
  m.questions.push_back(q);
  return m;
}

void extractInstances(const Message& m, const char* serviceType, std::vector<sq_discovery_record>& found,
                      std::vector<sq_discovery_record>& lost) {
  if (!(m.flags & 0x8000)) return;  // queries carry no answers we trust
  std::string domain = serviceDomain(serviceType);
  std::string typeNoLocal = stripLocal(domain);
  std::vector<const Record*> all;
  for (const auto* sec : {&m.answers, &m.authorities, &m.additionals})
    for (const auto& r : *sec) all.push_back(&r);

  auto findRec = [&](const std::string& name, uint16_t type) -> const Record* {
    for (const Record* r : all)
      if (r->type == type && sameName(r->name, name)) return r;
    return nullptr;
  };

  for (const Record* r : all) {
    if (r->type != kTypePTR || !sameName(r->name, domain)) continue;
    std::string label = instanceLabel(r->target, domain);
    if (label.empty()) continue;
    sq_discovery_record out{};
    out.struct_size = sizeof out;
    safeCopy(out.name, sizeof out.name, label);
    safeCopy(out.type, sizeof out.type, typeNoLocal);
    if (r->ttl == 0) {
      lost.push_back(out);
      continue;
    }
    const Record* srv = findRec(r->target, kTypeSRV);
    if (!srv) continue;
    out.port = srv->port;
    std::string host = srv->target;
    if (const Record* a = findRec(srv->target, kTypeA)) {
      char ip[32];
      std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", a->addr[0], a->addr[1], a->addr[2], a->addr[3]);
      host = ip;
    }
    safeCopy(out.host, sizeof out.host, host);
    if (const Record* t = findRec(r->target, kTypeTXT)) safeCopy(out.txt, sizeof out.txt, txtToString(t->txt));
    found.push_back(out);
  }
}

bool queryMatches(const Message& query, const sq_discovery_record& record) {
  if (query.flags & 0x8000) return false;
  std::string domain = serviceDomain(record.type);
  std::string instance = instanceFullName(record);
  for (const auto& q : query.questions) {
    if (q.type == kTypePTR && sameName(q.name, domain)) return true;
    if ((q.type == kTypeSRV || q.type == kTypeTXT || q.type == kTypeANY) && sameName(q.name, instance)) return true;
  }
  return false;
}

}  // namespace sq::dnssd
