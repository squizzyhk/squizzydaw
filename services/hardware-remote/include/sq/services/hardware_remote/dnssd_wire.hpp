// DNS message codec for multicast DNS / DNS-SD (RFC 1035 wire format, RFC 6762/6763 usage).
// Own implementation. Pure functions, no sockets: thread-safe; allocates (never on the audio thread).
//
// Names are dotted presentation strings. A literal '.' or '\' inside a label is escaped as
// "\." / "\\" (DNS-SD instance names may contain dots and spaces), e.g.
//   "Studio\. A._osc._udp.local"  = labels ["Studio. A", "_osc", "_udp", "local"].
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sq/interfaces/remote.h"

namespace sq::dnssd {

enum : uint16_t {
  kTypeA = 1,
  kTypePTR = 12,
  kTypeTXT = 16,
  kTypeAAAA = 28,
  kTypeSRV = 33,
  kTypeANY = 255,
};
enum : uint16_t { kClassIN = 1 };
inline constexpr uint16_t kFlagResponse = 0x8400;  // QR + AA (mDNS responses)
inline constexpr uint16_t kMdnsPort = 5353;
inline constexpr const char* kMdnsGroupV4 = "224.0.0.251";

struct Question {
  std::string name;
  uint16_t type = kTypePTR;
  uint16_t cls = kClassIN;
  bool unicastResponse = false;  // top bit of qclass in mDNS
};

struct Record {
  std::string name;
  uint16_t type = 0;
  uint16_t cls = kClassIN;
  bool cacheFlush = false;  // top bit of rrclass in mDNS
  uint32_t ttl = 120;
  // Decoded rdata (by type):
  std::string target;              // PTR target, SRV target
  uint16_t priority = 0, weight = 0, port = 0;  // SRV
  std::vector<std::string> txt;    // TXT strings ("key=value")
  uint8_t addr[16] = {};           // A (first 4 bytes) / AAAA
  std::vector<uint8_t> raw;        // rdata of other types, verbatim
};

struct Message {
  uint16_t id = 0;
  uint16_t flags = 0;
  std::vector<Question> questions;
  std::vector<Record> answers;
  std::vector<Record> authorities;
  std::vector<Record> additionals;
};

// Serializes with name compression. False on an invalid name (label > 63 bytes, name > 255).
bool encode(const Message& m, std::vector<uint8_t>& out, std::string* err = nullptr);
// Parses a datagram, following compression pointers (with loop protection).
bool decode(const uint8_t* data, size_t size, Message& m, std::string* err = nullptr);

// Name helpers.
std::vector<std::string> splitName(const std::string& name);    // unescapes
std::string joinName(const std::vector<std::string>& labels);   // escapes
bool sameName(const std::string& a, const std::string& b);      // ASCII case-insensitive, trailing '.' ignored

// "_osc._udp" or "_osc._udp.local." -> "_osc._udp.local"
std::string serviceDomain(const char* serviceType);
// "key=value;k2=v2" <-> TXT strings. ';' and '\' inside values are escaped with '\'.
std::vector<std::string> txtFromString(const char* txt);
std::string txtToString(const std::vector<std::string>& strings);

// DNS-SD announcement for one service instance: answers PTR(type -> instance), SRV, TXT and,
// when ipv4 is non-null, an A record for the host. ttl 0 makes it a goodbye packet.
// record->host is used as the SRV target; ".local" is appended when it has no dot.
Message buildAnnouncement(const sq_discovery_record& record, const uint8_t* ipv4, uint32_t ttl);
// One-shot PTR query for a service type.
Message buildBrowseQuery(const char* serviceType);

// Scans a response for instances of serviceType. Each complete instance (PTR + SRV found in the
// same message) is appended to found, PTRs with ttl 0 to lost. Records keep the instance label
// only in .name, .type = serviceType without ".local".
void extractInstances(const Message& m, const char* serviceType, std::vector<sq_discovery_record>& found,
                      std::vector<sq_discovery_record>& lost);

// True when the query asks for serviceType (PTR) or the instance itself (SRV/TXT/ANY).
bool queryMatches(const Message& query, const sq_discovery_record& record);

// ---- record <-> OS DNS-SD API helpers (portable, used by the system backend) ----------------
using TxtPair = std::pair<std::string, std::string>;

// "Studio A" + "_osc._udp" -> "Studio A._osc._udp.local" (escaped presentation form).
std::string instanceFullName(const sq_discovery_record& record);
// The SRV target for record.host: "" -> "squizzy.local", "box" -> "box.local", "a.b." -> "a.b".
std::string hostTarget(const char* host);
// "Studio\. A._osc._udp.local" -> instance "Studio. A", type "_osc._udp", domain "local".
// False unless the name has an instance label followed by "_service._udp|_tcp".
bool splitInstanceName(const std::string& fullName, std::string& instance, std::string& type, std::string& domain);
// "k=v;flag;k2=a\;b" -> {("k","v"), ("flag",""), ("k2","a;b")}. Keys are kept as given.
std::vector<TxtPair> txtPairs(const char* txt);
std::string txtFromPairs(const std::vector<TxtPair>& pairs);  // inverse ("k" when the value is empty)
// Builds a discovery record from the pieces an OS resolver reports. host may be a name or an
// address literal; a trailing '.' is removed. False when fullName is not a service instance.
bool makeRecord(const std::string& fullName, const std::string& host, uint16_t port, const std::vector<TxtPair>& txt,
                sq_discovery_record& out);
// PTR records of serviceType in m: (instance label, ttl). A ttl of 0 is a goodbye.
std::vector<std::pair<std::string, uint32_t>> extractPointers(const Message& m, const char* serviceType);

}  // namespace sq::dnssd
