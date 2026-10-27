#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "internal.h"

namespace {

struct Sink {
  int32_t id;
  sq_log_sink_fn fn;
  void* user;
};

struct LogState {
  std::mutex m;
  std::atomic<int32_t> level{SQ_LOG_INFO};
  bool console = true;
  std::FILE* file = nullptr;
  std::vector<Sink> sinks;
  int32_t nextId = 1;
};

LogState& state() {
  static LogState* s = new LogState();  // intentionally leaked: usable during static destruction
  return *s;
}

const char* kNames[] = {"trace", "debug", "info", "warn", "error", "fatal", "off"};

std::string timestamp() {
  int64_t ms = sq_time_unix_ms();
  std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms % 1000));
  return buf;
}

}  // namespace

extern "C" {

void sq_log_write(int32_t level, const char* tag, const char* message) {
  LogState& s = state();
  if (level < s.level.load(std::memory_order_relaxed) || level >= SQ_LOG_OFF) return;
  if (level < SQ_LOG_TRACE) level = SQ_LOG_TRACE;
  if (!tag) tag = "-";
  if (!message) message = "";
  std::string line = timestamp() + " [" + kNames[level] + "] " + tag + ": " + message + "\n";
  std::vector<Sink> sinks;
  {
    std::lock_guard<std::mutex> lk(s.m);
    if (s.console) {
      std::fputs(line.c_str(), stderr);
      std::fflush(stderr);
    }
    if (s.file) {
      std::fputs(line.c_str(), s.file);
      std::fflush(s.file);
    }
    sinks = s.sinks;
  }
#ifdef _WIN32
  OutputDebugStringW(sqp::widen(line).c_str());
#endif
  for (auto& k : sinks) k.fn(k.user, level, tag, message);
}

void sq_log_printf(int32_t level, const char* tag, const char* fmt, ...) {
  if (level < state().level.load(std::memory_order_relaxed)) return;
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
  va_end(ap);
  sq_log_write(level, tag, buf);
}

void sq_log_set_level(int32_t level) { state().level.store(level); }
int32_t sq_log_get_level(void) { return state().level.load(); }
const char* sq_log_level_name(int32_t level) { return level >= 0 && level <= SQ_LOG_OFF ? kNames[level] : "?"; }
int32_t sq_log_level_from_name(const char* name) {
  if (!name) return -1;
  for (int32_t i = 0; i <= SQ_LOG_OFF; ++i)
    if (sq_ascii_casecmp(name, kNames[i]) == 0) return i;
  if (sq_ascii_casecmp(name, "warning") == 0) return SQ_LOG_WARN;
  return -1;
}
void sq_log_set_console(sq_bool enabled) {
  std::lock_guard<std::mutex> lk(state().m);
  state().console = enabled != 0;
}
sq_result sq_log_open_file(const char* path) {
  LogState& s = state();
  std::lock_guard<std::mutex> lk(s.m);
  if (s.file) {
    std::fclose(s.file);
    s.file = nullptr;
  }
  if (!path) return SQ_OK;
#ifdef _WIN32
  s.file = _wfopen(sqp::widen(path).c_str(), L"ab");
#else
  s.file = std::fopen(path, "ab");
#endif
  return s.file ? SQ_OK : SQ_E_IO;
}
int32_t sq_log_add_sink(sq_log_sink_fn fn, void* user) {
  if (!fn) return 0;
  LogState& s = state();
  std::lock_guard<std::mutex> lk(s.m);
  int32_t id = s.nextId++;
  s.sinks.push_back(Sink{id, fn, user});
  return id;
}
void sq_log_remove_sink(int32_t id) {
  LogState& s = state();
  std::lock_guard<std::mutex> lk(s.m);
  for (auto it = s.sinks.begin(); it != s.sinks.end(); ++it)
    if (it->id == id) {
      s.sinks.erase(it);
      return;
    }
}

}  // extern "C"
