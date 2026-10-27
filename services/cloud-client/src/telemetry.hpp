// cloud-client: opt-in telemetry queue.
//   - Opted out (the default): track() drops events; nothing is kept in memory or on disk.
//   - Opted in: events are appended to a bounded FIFO (oldest dropped beyond maxEvents) that is
//     persisted to `path` after every change, so a crash or an offline session loses nothing.
//   - flush() uploads up to batchSize events as one JSON POST and removes them on a 2xx answer;
//     on failure they stay queued.
// Thread-safe (one internal mutex); never RT.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "http.hpp"

namespace sq::cloud {

struct TelemetryProp {
  enum Kind { Int, Double, Bool, String } kind = String;
  std::string key;
  int64_t i = 0;
  double d = 0;
  bool b = false;
  std::string s;
};

struct TelemetryEvent {
  std::string name;
  int64_t timeMs = 0;  // unix ms
  std::vector<TelemetryProp> props;
  uint64_t seq = 0;    // assigned by the queue (not persisted)
};

class TelemetryQueue {
 public:
  TelemetryQueue(std::string path, size_t maxEvents = 1000) : path_(std::move(path)), max_(maxEvents) {}

  static bool validName(const char* name);
  void load();  // reads the persisted queue (only meaningful when opted in)
  void setOptIn(bool on);
  bool optedIn() const;
  // False when dropped (not opted in, or invalid name).
  bool track(TelemetryEvent e);
  size_t size() const;
  std::vector<TelemetryEvent> snapshot() const;
  // Returns the HTTP status (0 = transport failure, -1 = nothing to send / not opted in).
  int flush(HttpTransport& http, const std::string& url, size_t batchSize = 100);

  static std::string encodeBatch(const std::vector<TelemetryEvent>& events);

 private:
  void persistLocked();
  std::string path_;
  size_t max_;
  mutable std::mutex m_;
  bool optIn_ = false;
  uint64_t nextSeq_ = 1;
  std::vector<TelemetryEvent> q_;
};

}  // namespace sq::cloud
