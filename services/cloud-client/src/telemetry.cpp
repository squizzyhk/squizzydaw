#include "telemetry.hpp"

#include <cstring>

#include "json_util.hpp"

namespace sq::cloud {

namespace {

sq_json* eventToJson(const TelemetryEvent& e) {
  sq_json* o = sq_json_new_object();
  sq_json_set(o, "name", jsonStr(e.name));
  sq_json_set(o, "time_ms", jsonNum(e.timeMs));
  sq_json* p = sq_json_new_object();
  for (const auto& pr : e.props) {
    sq_json* v = nullptr;
    switch (pr.kind) {
      case TelemetryProp::Int: v = jsonNum(pr.i); break;
      case TelemetryProp::Double: v = sq_json_new_number(pr.d); break;
      case TelemetryProp::Bool: v = sq_json_new_bool(pr.b ? SQ_TRUE : SQ_FALSE); break;
      case TelemetryProp::String: v = jsonStr(pr.s); break;
    }
    sq_json_set(p, pr.key.c_str(), v);
  }
  sq_json_set(o, "props", p);
  return o;
}

bool eventFromJson(const sq_json* o, TelemetryEvent& e) {
  if (!o || sq_json_type_of(o) != SQ_JSON_OBJECT) return false;
  e.name = jsonString(o, "name");
  e.timeMs = jsonInt(o, "time_ms", 0);
  if (!TelemetryQueue::validName(e.name.c_str())) return false;
  const sq_json* p = sq_json_get(o, "props");
  if (p && sq_json_type_of(p) == SQ_JSON_OBJECT) {
    for (size_t i = 0; i < sq_json_size(p); ++i) {
      TelemetryProp pr;
      pr.key = sq_json_key_at(p, i);
      const sq_json* v = sq_json_at(p, i);
      switch (sq_json_type_of(v)) {
        case SQ_JSON_NUMBER: {
          double d = sq_json_get_number(v);
          if (d == static_cast<double>(static_cast<int64_t>(d))) {
            pr.kind = TelemetryProp::Int;
            pr.i = static_cast<int64_t>(d);
          } else {
            pr.kind = TelemetryProp::Double;
            pr.d = d;
          }
          break;
        }
        case SQ_JSON_BOOL:
          pr.kind = TelemetryProp::Bool;
          pr.b = sq_json_get_bool(v) != SQ_FALSE;
          break;
        case SQ_JSON_STRING: {
          size_t n = 0;
          const char* s = sq_json_get_string(v, &n);
          pr.kind = TelemetryProp::String;
          pr.s.assign(s, n);
          break;
        }
        default:
          continue;
      }
      e.props.push_back(std::move(pr));
    }
  }
  return true;
}

}  // namespace

bool TelemetryQueue::validName(const char* name) {
  if (!name) return false;
  size_t n = std::strlen(name);
  if (n == 0 || n > 64) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
  }
  return true;
}

std::string TelemetryQueue::encodeBatch(const std::vector<TelemetryEvent>& events) {
  JsonPtr root(sq_json_new_object());
  sq_json* arr = sq_json_new_array();
  for (const auto& e : events) sq_json_append(arr, eventToJson(e));
  sq_json_set(root.get(), "events", arr);
  return jsonWrite(root.get());
}

void TelemetryQueue::persistLocked() {
  if (path_.empty()) return;
  if (!optIn_ || q_.empty()) {
    sq_fs_remove(path_.c_str());
    return;
  }
  sq::writeFile(path_, encodeBatch(q_));
}

void TelemetryQueue::load() {
  std::lock_guard<std::mutex> lk(m_);
  q_.clear();
  if (path_.empty() || !optIn_) return;
  auto data = sq::readFile(path_);
  if (!data) return;
  JsonPtr j = jsonParse(*data);
  const sq_json* arr = j ? sq_json_get(j.get(), "events") : nullptr;
  if (!arr || sq_json_type_of(arr) != SQ_JSON_ARRAY) return;
  for (size_t i = 0; i < sq_json_size(arr) && q_.size() < max_; ++i) {
    TelemetryEvent e;
    if (eventFromJson(sq_json_at(arr, i), e)) {
      e.seq = nextSeq_++;
      q_.push_back(std::move(e));
    }
  }
}

void TelemetryQueue::setOptIn(bool on) {
  std::lock_guard<std::mutex> lk(m_);
  optIn_ = on;
  if (on) return;  // keep any persisted queue: load() picks it up
  q_.clear();
  persistLocked();  // opting out wipes the file too
}

bool TelemetryQueue::optedIn() const {
  std::lock_guard<std::mutex> lk(m_);
  return optIn_;
}

bool TelemetryQueue::track(TelemetryEvent e) {
  if (!validName(e.name.c_str())) return false;
  std::lock_guard<std::mutex> lk(m_);
  if (!optIn_) return false;
  if (max_ == 0) return false;
  if (q_.size() >= max_) q_.erase(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(q_.size() - max_ + 1));
  e.seq = nextSeq_++;
  q_.push_back(std::move(e));
  persistLocked();
  return true;
}

size_t TelemetryQueue::size() const {
  std::lock_guard<std::mutex> lk(m_);
  return q_.size();
}

std::vector<TelemetryEvent> TelemetryQueue::snapshot() const {
  std::lock_guard<std::mutex> lk(m_);
  return q_;
}

int TelemetryQueue::flush(HttpTransport& http, const std::string& url, size_t batchSize) {
  std::vector<TelemetryEvent> batch;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!optIn_ || q_.empty() || batchSize == 0) return -1;
    size_t n = q_.size() < batchSize ? q_.size() : batchSize;
    batch.assign(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(n));
  }
  HttpRequest req;
  req.method = "POST";
  req.url = url;
  req.headers = {{"Content-Type", "application/json"}};
  req.body = encodeBatch(batch);
  HttpResponse res = http.send(req);
  if (res.status >= 200 && res.status < 300) {
    std::lock_guard<std::mutex> lk(m_);
    // Remove exactly the uploaded events (by sequence number): events tracked meanwhile stay, and
    // events already trimmed or cleared meanwhile are simply gone.
    const uint64_t last = batch.back().seq;
    size_t n = 0;
    while (n < q_.size() && q_[n].seq <= last) ++n;
    q_.erase(q_.begin(), q_.begin() + static_cast<std::ptrdiff_t>(n));
    persistLocked();
  }
  return res.status;
}

}  // namespace sq::cloud
