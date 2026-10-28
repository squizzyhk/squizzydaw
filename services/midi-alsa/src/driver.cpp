// midi-alsa: IMidiDriver (category "MidiDriver") over the ALSA sequencer, the platform MIDI
// service on Linux. libasound.so.2 is loaded at runtime (seq_api.hpp); without it, or without
// /dev/snd/seq, the driver lists 0 ports and logs why.
//
// Ports
//   Every sequencer port of another client (hardware and software, "Midi Through" included;
//   the System client and ports flagged NO_EXPORT excluded) that can be subscribed to:
//     readable (CAP_READ|SUBS_READ)   -> an input port of this driver  "alsa:in:<client>:<port>#n"
//     writable (CAP_WRITE|SUBS_WRITE) -> an output port                 "alsa:out:<client>:<port>#n"
//   n counts ports with the same client and port names (1-based), so ids are stable across
//   runs and plug order (ALSA client numbers are not).
//
// One sequencer client per IMidiDriver instance ("Squizzy Studio"), with one writable port
// ("Squizzy Studio In") that every opened input is connected to (snd_seq_connect_from) and one
// readable port ("Squizzy Studio Out") that output events are sent from, addressed directly to
// the destination port.
//
// Threading (same contract as midi-win)
//   - A reader thread (realtime priority when permitted) polls the sequencer, decodes events to
//     raw MIDI bytes and calls IMidiInputCallback::onMidi through an RT-safe gate; sysex chunks
//     are reassembled into one complete F0..F7 message first (fixed 64 KB buffer per port).
//     Timestamps are sq_time_monotonic_ns() at arrival.
//   - IMidiOutputPort::send is lock-free (bounded queue + preallocated sysex slots); a worker
//     thread per output port schedules by time_ns and writes to the sequencer.
//   - Hot-plug: the client is subscribed to the System Announce port. While IMidiDeviceListeners
//     are registered, a watcher thread re-enumerates after client/port start/exit/change
//     announcements (and every Services.midi-alsa.HotplugPollMs ms, default 2000, as a fallback,
//     which also picks up a sequencer that appears later) and calls onPortsChanged() on change.
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "midi_alsa.hpp"
#include "seq_api.hpp"
#include "seq_event.hpp"
#include "sq/interfaces/midi.h"
#include "sq/platform.h"
#include "sq/sdk.hpp"

namespace sq::alsaseq {
namespace {

void silentHandler(const char*, int, const char*, int, const char*, ...) {}

void copyStr(char* dst, size_t cap, const std::string& s) {
  if (!cap) return;
  size_t n = std::min(s.size(), cap - 1);
  while (n > 0 && n < s.size() && (uint8_t(s[n]) & 0xC0u) == 0x80u) --n;
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

uint16_t key(SeqAddr a) { return uint16_t((a.client << 8) | a.port); }

// ---- lock-free building blocks ---------------------------------------------------------------

// Bounded MPMC queue (sequence-numbered cells); push/pop never allocate or block.
template <class T>
class MpmcQueue {
 public:
  explicit MpmcQueue(size_t pow2) : cells_(new Cell[pow2]), mask_(pow2 - 1) {
    for (size_t i = 0; i < pow2; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
  }
  bool push(const T& v) noexcept {
    size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const intptr_t dif = intptr_t(c.seq.load(std::memory_order_acquire)) - intptr_t(pos);
      if (dif == 0) {
        if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          c.value = v;
          c.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;
      } else {
        pos = tail_.load(std::memory_order_relaxed);
      }
    }
  }
  bool pop(T& v) noexcept {
    size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Cell& c = cells_[pos & mask_];
      const intptr_t dif = intptr_t(c.seq.load(std::memory_order_acquire)) - intptr_t(pos + 1);
      if (dif == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          v = c.value;
          c.seq.store(pos + mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (dif < 0) {
        return false;
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

 private:
  struct Cell {
    std::atomic<size_t> seq{0};
    T value{};
  };
  std::unique_ptr<Cell[]> cells_;
  size_t mask_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

// eventfd-based wake-up: wake() is a single non-blocking write, callable from any thread.
class Waker {
 public:
  Waker() : fd_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
  ~Waker() {
    if (fd_ >= 0) ::close(fd_);
  }
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;
  bool ok() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  void wake() noexcept {
    const uint64_t one = 1;
    [[maybe_unused]] ssize_t r = ::write(fd_, &one, sizeof one);
  }
  void drain() noexcept {
    uint64_t v;
    [[maybe_unused]] ssize_t r = ::read(fd_, &v, sizeof v);
  }
  void wait(int timeoutMs) noexcept {  // < 0 = until woken
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, timeoutMs) > 0) drain();
  }

 private:
  int fd_;
};

// ---- sequencer client -------------------------------------------------------------------------

struct SeqPortDesc {
  SeqAddr addr{};
  std::string client, port;
  unsigned caps = 0;
};

// Receives decoded events on the reader thread.
class InputSink {
 public:
  virtual void onSeqEvent(const SeqEvent& ev, uint64_t now) noexcept = 0;
  SeqAddr source{};

 protected:
  ~InputSink() = default;
};

class SeqClient {
 public:
  static constexpr uint32_t kMaxSinks = 64;

  static std::shared_ptr<SeqClient> open(const SeqApi* a, std::string& why) {
    a->lib_error_set_handler(&silentHandler);
    snd_seq_t* seq = nullptr;
    int r = a->open(&seq, "default", kOpenDuplex, kNonblock);
    a->lib_error_set_handler(nullptr);
    if (r < 0) {
      why = std::string("cannot open the ALSA sequencer (") + a->strerror(r) + "); is /dev/snd/seq present (snd-seq module)?";
      return nullptr;
    }
    auto c = std::shared_ptr<SeqClient>(new SeqClient(a, seq));
    if (!c->init(why)) return nullptr;
    return c;
  }

  ~SeqClient() {
    if (thread_.joinable()) {
      quit_.store(true);
      stop_.wake();
      thread_.join();
    }
    a_->close(seq_);
  }

  int clientId() const { return clientId_; }
  uint64_t announcements() const { return announce_.load(std::memory_order_acquire); }

  // Main/control thread.
  std::vector<SeqPortDesc> listPorts() {
    std::vector<SeqPortDesc> out;
    snd_seq_client_info_t* ci = nullptr;
    snd_seq_port_info_t* pi = nullptr;
    if (a_->client_info_malloc(&ci) < 0) return out;
    if (a_->port_info_malloc(&pi) < 0) {
      a_->client_info_free(ci);
      return out;
    }
    a_->client_info_set_client(ci, -1);
    while (a_->query_next_client(seq_, ci) >= 0) {
      const int client = a_->client_info_get_client(ci);
      if (client == kClientSystem || client == clientId_) continue;
      const char* cname = a_->client_info_get_name(ci);
      a_->port_info_set_client(pi, client);
      a_->port_info_set_port(pi, -1);
      while (a_->query_next_port(seq_, pi) >= 0) {
        const unsigned caps = a_->port_info_get_capability(pi);
        if (caps & kCapNoExport) continue;
        SeqPortDesc d;
        d.addr = SeqAddr{uint8_t(client), uint8_t(a_->port_info_get_port(pi))};
        d.client = cname ? cname : "";
        const char* pn = a_->port_info_get_name(pi);
        d.port = pn ? pn : "";
        d.caps = caps;
        out.push_back(std::move(d));
      }
    }
    a_->port_info_free(pi);
    a_->client_info_free(ci);
    return out;
  }

  // Connects `src` to our input port (reference counted per source) and starts dispatching to
  // `sink`. Main thread.
  sq_result attach(InputSink* sink) {
    std::lock_guard<std::mutex> lk(subMu_);
    int& n = subs_[key(sink->source)];
    if (n == 0) {
      const int r = a_->connect_from(seq_, inPort_, sink->source.client, sink->source.port);
      if (r < 0) {
        subs_.erase(key(sink->source));
        sq::logf(SQ_LOG_WARN, "subscribe %u:%u: %s", sink->source.client, sink->source.port, a_->strerror(r));
        return r == -EPERM || r == -EACCES ? SQ_E_ACCESS : (r == -EBUSY ? SQ_E_BUSY : SQ_E_NOTFOUND);
      }
    }
    for (auto& s : sinks_) {
      InputSink* expected = nullptr;
      if (s.compare_exchange_strong(expected, sink, std::memory_order_seq_cst)) {
        ++n;
        return SQ_OK;
      }
    }
    if (n == 0) {
      a_->disconnect_from(seq_, inPort_, sink->source.client, sink->source.port);
      subs_.erase(key(sink->source));
    }
    return SQ_E_BUSY;
  }

  // Stops dispatching to `sink` (returns after any in-flight dispatch finished) and drops the
  // subscription when it was the last user of that source.
  void detach(InputSink* sink) {
    for (auto& s : sinks_) {
      InputSink* expected = sink;
      s.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
    }
    while (busy_.load(std::memory_order_seq_cst) != 0) sq_thread_yield();
    std::lock_guard<std::mutex> lk(subMu_);
    auto it = subs_.find(key(sink->source));
    if (it != subs_.end() && --it->second <= 0) {
      a_->disconnect_from(seq_, inPort_, sink->source.client, sink->source.port);
      subs_.erase(it);
    }
  }

  // Any non-RT thread. Sends one event directly to `dest`.
  bool send(SeqAddr dest, SeqEvent& ev) {
    ev.source.client = uint8_t(clientId_);
    ev.source.port = uint8_t(outPort_);
    ev.dest = dest;
    ev.queue = kQueueDirect;
    std::lock_guard<std::mutex> lk(outMu_);  // alsa-lib shares a temp buffer for variable events
    for (int attempt = 0; attempt < 50; ++attempt) {
      const int r = a_->event_output_direct(seq_, &ev);
      if (r >= 0) return true;
      if (r != -EAGAIN) return false;
      sq_thread_sleep_ms(1);  // kernel pool full: give the receiver a moment
    }
    return false;
  }

 private:
  SeqClient(const SeqApi* a, snd_seq_t* seq) : a_(a), seq_(seq) {}

  bool init(std::string& why) {
    a_->set_client_name(seq_, "Squizzy Studio");
    clientId_ = a_->client_id(seq_);
    inPort_ = a_->create_simple_port(seq_, "Squizzy Studio In", kCapWrite | kCapSubsWrite, kTypeMidiGeneric | kTypeApplication);
    outPort_ = a_->create_simple_port(seq_, "Squizzy Studio Out", kCapRead | kCapSubsRead, kTypeMidiGeneric | kTypeApplication);
    if (clientId_ < 0 || inPort_ < 0 || outPort_ < 0 || !stop_.ok()) {
      why = "cannot create sequencer ports";
      return false;
    }
    if (a_->connect_from(seq_, inPort_, kClientSystem, kPortSystemAnnounce) < 0)
      sq::logf(SQ_LOG_WARN, "no System Announce subscription: hot-plug falls back to polling");
    const int n = a_->poll_descriptors_count(seq_, POLLIN);
    if (n <= 0) {
      why = "sequencer has no poll descriptors";
      return false;
    }
    fds_.resize(size_t(n) + 1);
    a_->poll_descriptors(seq_, fds_.data(), unsigned(n), POLLIN);
    fds_[size_t(n)] = pollfd{stop_.fd(), POLLIN, 0};
    try {
      thread_ = std::thread([this] { readerMain(); });
    } catch (...) {
      why = "cannot start the MIDI reader thread";
      return false;
    }
    return true;
  }

  void dispatch(const SeqEvent& ev, uint64_t now) noexcept {
    if (ev.source.client == kClientSystem) {
      if (isAnnounce(ev.type)) announce_.fetch_add(1, std::memory_order_acq_rel);
      return;
    }
    busy_.fetch_add(1, std::memory_order_seq_cst);
    const uint16_t k = key(ev.source);
    for (auto& s : sinks_) {
      InputSink* sink = s.load(std::memory_order_seq_cst);
      if (sink && key(sink->source) == k) sink->onSeqEvent(ev, now);
    }
    busy_.fetch_sub(1, std::memory_order_seq_cst);
  }

  // Reader thread: RT rules apply (no allocation, no locks).
  void readerMain() {
    sq_thread_set_current_name("sq-alsa-midi-in");
    if (sq_thread_set_current_priority(SQ_PRIORITY_REALTIME) != SQ_OK) sq_thread_set_current_priority(SQ_PRIORITY_HIGH);
    while (!quit_.load(std::memory_order_acquire)) {
      for (auto& f : fds_) f.revents = 0;
      const int pr = ::poll(fds_.data(), fds_.size(), 1000);
      if (quit_.load(std::memory_order_acquire)) break;
      if (pr <= 0) continue;
      if (fds_.back().revents) stop_.drain();
      for (;;) {
        SeqEvent* ev = nullptr;
        const int r = a_->event_input(seq_, &ev);
        if (r == -ENOSPC) {  // kernel input queue overran: events were lost
          overruns_.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (r < 0) break;  // -EAGAIN: drained
        if (ev) dispatch(*ev, sq_time_monotonic_ns());
      }
    }
  }

  const SeqApi* a_;
  snd_seq_t* seq_;
  int clientId_ = -1, inPort_ = -1, outPort_ = -1;
  std::vector<pollfd> fds_;
  Waker stop_;
  std::atomic<bool> quit_{false};
  std::thread thread_;
  std::atomic<InputSink*> sinks_[kMaxSinks] = {};
  std::atomic<int32_t> busy_{0};
  std::atomic<uint64_t> announce_{0}, overruns_{0};
  std::mutex subMu_;
  std::map<uint16_t, int> subs_;
  std::mutex outMu_;
};

// ---- input port -------------------------------------------------------------------------------

class InputPort final : public Object<InputPort, IMidiInputPort>, public InputSink {
 public:
  InputPort(std::shared_ptr<SeqClient> c, const sq_midi_port_info& info, SeqAddr src, IMidiInputCallback* cb)
      : client_(std::move(c)), info_(info), cb_(cb), sysex_(new SysexAssembler()) {
    source = src;
  }
  sq_result open() {
    const sq_result r = client_->attach(this);
    attached_ = SQ_SUCCEEDED(r);
    return r;
  }

  sq_result SQ_CALL getInfo(sq_midi_port_info* out) override {
    if (!out || out->struct_size < sizeof(sq_midi_port_info)) return SQ_E_INVALIDARG;
    *out = info_;
    return SQ_OK;
  }
  sq_result SQ_CALL start() override {
    if (running_.load()) return SQ_S_FALSE;
    running_.store(true, std::memory_order_seq_cst);
    return SQ_OK;
  }
  sq_result SQ_CALL stop() override {
    if (!running_.load()) return SQ_S_FALSE;
    drain();
    return SQ_OK;
  }

  // Reader thread.
  void onSeqEvent(const SeqEvent& ev, uint64_t now) noexcept override {
    inflight_.fetch_add(1, std::memory_order_seq_cst);
    if (running_.load(std::memory_order_seq_cst)) {
      if (ev.type == kEvSysex) {
        if ((ev.flags & kFlagLengthVariable) && ev.data.ext.ptr &&
            sysex_->feed(static_cast<const uint8_t*>(ev.data.ext.ptr), ev.data.ext.len)) {
          sq_midi_timed_event e{};
          e.time_ns = now;
          e.data = sysex_->data();
          e.size = sysex_->size();
          cb_->onMidi(&info_, &e, 1);
        }
      } else {
        ShortMsg m[4];
        const uint32_t n = decodeShort(ev, m);
        if (n) {
          sq_midi_timed_event e[4]{};
          for (uint32_t i = 0; i < n; ++i) {
            e[i].time_ns = now;
            std::memcpy(e[i].bytes, m[i].bytes, 3);
            e[i].size = m[i].size;
          }
          cb_->onMidi(&info_, e, n);
        }
      }
    }
    inflight_.fetch_sub(1, std::memory_order_seq_cst);
  }

 public:  // sq::Object deletes through the derived type
  ~InputPort() override {
    drain();
    if (attached_) client_->detach(this);
  }

 private:
  void drain() {
    running_.store(false, std::memory_order_seq_cst);
    while (inflight_.load(std::memory_order_seq_cst) != 0) sq_thread_yield();
  }
  std::shared_ptr<SeqClient> client_;
  sq_midi_port_info info_;
  Ptr<IMidiInputCallback> cb_;
  std::unique_ptr<SysexAssembler> sysex_;
  std::atomic<bool> running_{false};
  std::atomic<int32_t> inflight_{0};
  bool attached_ = false;
};

// ---- output port ------------------------------------------------------------------------------

class OutputPort final : public Object<OutputPort, IMidiOutputPort> {
 public:
  static constexpr uint32_t kQueueCapacity = 4096;
  static constexpr uint32_t kSysexSlots = 16;
  static constexpr uint32_t kSysexMax = 65536;
  static constexpr uint32_t kSysexChunk = 256;

  OutputPort(std::shared_ptr<SeqClient> c, const sq_midi_port_info& info, SeqAddr dest)
      : client_(std::move(c)), info_(info), dest_(dest), pool_(new uint8_t[size_t(kSysexSlots) * kSysexMax]) {
    for (uint32_t i = 0; i < kSysexSlots; ++i) freeSysex_.push(i);
    heap_.reserve(kQueueCapacity);
  }
  sq_result start() {
    if (!waker_.ok()) return SQ_E_FAIL;
    try {
      thread_ = std::thread([this] { run(); });
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }

  sq_result SQ_CALL getInfo(sq_midi_port_info* out) override {
    if (!out || out->struct_size < sizeof(sq_midi_port_info)) return SQ_E_INVALIDARG;
    *out = info_;
    return SQ_OK;
  }

  // RT-safe: lock-free pushes, memcpy into a preallocated sysex slot, a non-blocking wake.
  sq_result SQ_CALL send(const sq_midi_timed_event* events, uint32_t count) override {
    if (!events && count) return SQ_E_INVALIDARG;
    sq_result result = SQ_OK;
    uint32_t queued = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const sq_midi_timed_event& e = events[i];
      Slot s;
      s.time = e.time_ns;
      if (e.data && e.size) {
        if (e.size > kSysexMax || e.data[0] != 0xF0u) {
          result = SQ_E_INVALIDARG;
          break;
        }
        uint32_t idx = 0;
        if (!freeSysex_.pop(idx)) {
          result = SQ_E_BUSY;
          break;
        }
        std::memcpy(pool_.get() + size_t(idx) * kSysexMax, e.data, e.size);
        s.sysex = int32_t(idx);
        s.size = e.size;
      } else {
        const uint32_t need = shortLength(e.bytes[0]);
        if (need == 0 || e.size < need || e.size > 4) {
          result = SQ_E_INVALIDARG;
          break;
        }
        std::memcpy(s.bytes, e.bytes, 4);
        s.size = need;
      }
      if (!queue_.push(s)) {
        if (s.sysex >= 0) freeSysex_.push(uint32_t(s.sysex));
        result = SQ_E_BUSY;
        break;
      }
      ++queued;
    }
    if (queued) waker_.wake();
    return result;
  }

 public:  // sq::Object deletes through the derived type
  ~OutputPort() override {
    if (thread_.joinable()) {
      quit_.store(true, std::memory_order_release);
      waker_.wake();
      thread_.join();
    }
  }

 private:
  struct Slot {
    uint64_t time = 0;
    uint32_t size = 0;
    int32_t sysex = -1;
    uint8_t bytes[4] = {};
  };
  struct Pending {
    uint64_t due, seq;
    Slot slot;
  };

  void emit(const Slot& s) {
    SeqEvent ev;
    if (s.sysex >= 0) {
      const uint8_t* p = pool_.get() + size_t(s.sysex) * kSysexMax;
      for (uint32_t off = 0; off < s.size; off += kSysexChunk) {
        encodeSysexChunk(p + off, std::min(kSysexChunk, s.size - off), ev);
        if (!client_->send(dest_, ev)) break;
      }
      freeSysex_.push(uint32_t(s.sysex));
    } else if (encodeShort(s.bytes, s.size, ev)) {
      client_->send(dest_, ev);
    }
  }

  void run() {
    sq_thread_set_current_name("sq-alsa-midi-out");
    sq_thread_set_current_priority(SQ_PRIORITY_HIGH);
    uint64_t seq = 0;
    auto later = [](const Pending& a, const Pending& b) { return a.due != b.due ? a.due > b.due : a.seq > b.seq; };
    for (;;) {
      const bool quitting = quit_.load(std::memory_order_acquire);
      Slot s;
      while (heap_.size() < kQueueCapacity && queue_.pop(s)) {
        heap_.push_back(Pending{s.time, seq++, s});
        std::push_heap(heap_.begin(), heap_.end(), later);
      }
      const uint64_t now = sq_time_monotonic_ns();
      while (!heap_.empty() && heap_.front().due <= now) {
        std::pop_heap(heap_.begin(), heap_.end(), later);
        emit(heap_.back().slot);
        heap_.pop_back();
      }
      if (quitting) break;  // events scheduled in the future are dropped on close
      int timeout = 250;
      if (!heap_.empty()) timeout = int(std::min<uint64_t>(250, (heap_.front().due - now) / 1000000ull));
      if (timeout > 0) waker_.wait(timeout);
      else if (!heap_.empty()) sq_thread_yield();  // due within the next millisecond
    }
    for (const Pending& p : heap_)
      if (p.slot.sysex >= 0) freeSysex_.push(uint32_t(p.slot.sysex));
    heap_.clear();
  }

  std::shared_ptr<SeqClient> client_;
  sq_midi_port_info info_;
  SeqAddr dest_;
  MpmcQueue<Slot> queue_{kQueueCapacity};
  MpmcQueue<uint32_t> freeSysex_{kSysexSlots};
  std::unique_ptr<uint8_t[]> pool_;
  std::vector<Pending> heap_;
  Waker waker_;
  std::atomic<bool> quit_{false};
  std::thread thread_;
};

// ---- driver -----------------------------------------------------------------------------------

struct PortEntry {
  sq_midi_port_info info{};
  SeqAddr addr{};
};

std::string portLabel(const SeqPortDesc& d) {
  if (d.port.rfind(d.client, 0) == 0) return d.port;  // "Midi Through Port-0" already says it
  return d.client + ": " + d.port;
}

std::vector<PortEntry> buildEntries(const std::vector<SeqPortDesc>& ports) {
  std::vector<PortEntry> out;
  for (const bool input : {true, false}) {
    std::map<std::string, int> seen;
    for (const auto& d : ports) {
      const unsigned need = input ? (kCapRead | kCapSubsRead) : (kCapWrite | kCapSubsWrite);
      if ((d.caps & need) != need) continue;
      const std::string base = d.client + ":" + d.port;
      const int n = ++seen[base];
      PortEntry e;
      e.info.struct_size = sizeof e.info;
      copyStr(e.info.id, sizeof e.info.id, std::string(input ? "alsa:in:" : "alsa:out:") + base + "#" + std::to_string(n));
      copyStr(e.info.name, sizeof e.info.name, portLabel(d) + (n > 1 ? " (" + std::to_string(n) + ")" : ""));
      e.info.flags = input ? SQ_MIDI_PORT_INPUT : SQ_MIDI_PORT_OUTPUT;
      e.addr = d.addr;
      out.push_back(e);
    }
  }
  return out;
}

class MidiDriver final : public Object<MidiDriver, IMidiDriver> {
 public:
  MidiDriver() = default;
  ~MidiDriver() override { stopWatcher(); }

  sq_result SQ_CALL refreshPorts() override {
    bool changed;
    std::vector<Ptr<IMidiDeviceListener>> ls;
    {
      std::lock_guard<std::mutex> rk(refreshMu_);
      std::shared_ptr<SeqClient> c = ensureClient();
      auto fresh = c ? buildEntries(c->listPorts()) : std::vector<PortEntry>{};
      std::lock_guard<std::mutex> lk(mu_);
      changed = fresh.size() != ports_.size() ||
                !std::equal(fresh.begin(), fresh.end(), ports_.begin(), [](const PortEntry& a, const PortEntry& b) {
                  return std::strcmp(a.info.id, b.info.id) == 0 && key(a.addr) == key(b.addr);
                });
      ports_ = std::move(fresh);
      enumerated_ = true;
      ls = listeners_;
    }
    if (changed)
      for (auto& l : ls) l->onPortsChanged();
    return SQ_OK;
  }
  uint32_t SQ_CALL getPortCount() override {
    ensure();
    std::lock_guard<std::mutex> lk(mu_);
    return uint32_t(ports_.size());
  }
  sq_result SQ_CALL getPortInfo(uint32_t index, sq_midi_port_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    ensure();
    std::lock_guard<std::mutex> lk(mu_);
    if (index >= ports_.size()) return SQ_E_NOTFOUND;
    *out = ports_[index].info;
    return SQ_OK;
  }
  sq_result SQ_CALL openInput(const char* portId, IMidiInputCallback* cb, IMidiInputPort** out) override {
    if (!portId || !cb || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    PortEntry e;
    std::shared_ptr<SeqClient> c;
    if (!find(portId, SQ_MIDI_PORT_INPUT, e, c)) return SQ_E_NOTFOUND;
    auto p = Ptr<InputPort>::adopt(new (std::nothrow) InputPort(c, e.info, e.addr, cb));
    if (!p) return SQ_E_OUTOFMEMORY;
    const sq_result r = p->open();
    if (SQ_FAILED(r)) return r;
    *out = p.detach();
    return SQ_OK;
  }
  sq_result SQ_CALL openOutput(const char* portId, IMidiOutputPort** out) override {
    if (!portId || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    PortEntry e;
    std::shared_ptr<SeqClient> c;
    if (!find(portId, SQ_MIDI_PORT_OUTPUT, e, c)) return SQ_E_NOTFOUND;
    auto p = Ptr<OutputPort>::adopt(new (std::nothrow) OutputPort(c, e.info, e.addr));
    if (!p) return SQ_E_OUTOFMEMORY;
    const sq_result r = p->start();
    if (SQ_FAILED(r)) return r;
    *out = p.detach();
    return SQ_OK;
  }
  sq_result SQ_CALL addListener(IMidiDeviceListener* l) override {
    if (!l) return SQ_E_INVALIDARG;
    ensure();  // baseline first, so the watcher reports real changes only
    bool first;
    {
      std::lock_guard<std::mutex> lk(mu_);
      first = listeners_.empty();
      listeners_.push_back(Ptr<IMidiDeviceListener>(l));
    }
    if (first) startWatcher();
    return SQ_OK;
  }
  sq_result SQ_CALL removeListener(IMidiDeviceListener* l) override {
    bool last = false, found = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->get() == l) {
          listeners_.erase(it);
          found = true;
          last = listeners_.empty();
          break;
        }
    }
    if (!found) return SQ_E_NOTFOUND;
    // The watcher cannot join itself (removal from inside onPortsChanged): it then idles until
    // the next add/remove or the driver's destruction.
    if (last && std::this_thread::get_id() != watcherId_.load()) stopWatcher();
    return SQ_OK;
  }

 private:
  // Under refreshMu_. Retries opening a missing sequencer (rate-limited to once per second).
  std::shared_ptr<SeqClient> ensureClient() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (client_) return client_;
    }
    const uint64_t now = sq_time_monotonic_ns();
    if (lastOpenTry_ && now - lastOpenTry_ < 1000000000ull) return nullptr;
    lastOpenTry_ = now;
    std::string why;
    const SeqApi* a = loadSeq(&why);
    std::shared_ptr<SeqClient> c = a ? SeqClient::open(a, why) : nullptr;
    if (!c) {
      if (why != lastWhy_) sq::logf(SQ_LOG_WARN, "ALSA MIDI unavailable, 0 ports: %s", why.c_str());
      lastWhy_ = why;
      return nullptr;
    }
    lastWhy_.clear();
    sq::logf(SQ_LOG_INFO, "ALSA sequencer client %d opened", c->clientId());
    std::lock_guard<std::mutex> lk(mu_);
    client_ = c;
    return c;
  }
  void ensure() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (enumerated_) return;
    }
    refreshPorts();
  }
  bool find(const char* id, uint32_t dir, PortEntry& out, std::shared_ptr<SeqClient>& c) {
    ensure();
    std::lock_guard<std::mutex> lk(mu_);
    if (!client_) return false;
    for (const auto& p : ports_)
      if ((p.info.flags & dir) && std::strcmp(p.info.id, id) == 0) {
        out = p;
        c = client_;
        return true;
      }
    return false;
  }

  void startWatcher() {
    std::lock_guard<std::mutex> lk(watchMu_);
    if (watcher_.joinable()) return;
    watchQuit_ = false;
    int64_t poll = 2000;
    if (IHostContext* h = host()) poll = h->getSettings()->getInt("Services.midi-alsa.HotplugPollMs", 2000);
    pollMs_ = poll <= 0 ? 0 : uint32_t(std::clamp<int64_t>(poll, 50, 3600000));
    try {
      watcher_ = std::thread([this] { watch(); });
    } catch (...) {
      sq::logf(SQ_LOG_WARN, "hot-plug watcher not started; call refreshPorts() manually");
    }
  }
  void stopWatcher() {
    std::thread t;
    {
      std::lock_guard<std::mutex> lk(watchMu_);
      watchQuit_ = true;
      t = std::move(watcher_);
    }
    watchCv_.notify_all();
    if (t.joinable()) t.join();
    watcherId_.store(std::thread::id{});
  }
  void watch() {
    watcherId_.store(std::this_thread::get_id());
    uint64_t seen = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (client_) seen = client_->announcements();
    }
    uint64_t lastPoll = sq_time_monotonic_ns();
    std::unique_lock<std::mutex> lk(watchMu_);
    while (!watchQuit_) {
      watchCv_.wait_for(lk, std::chrono::milliseconds(100));
      if (watchQuit_) break;
      lk.unlock();
      uint64_t now = sq_time_monotonic_ns(), ann = seen;
      {
        std::lock_guard<std::mutex> g(mu_);
        if (client_) ann = client_->announcements();
      }
      const bool pollDue = pollMs_ && now - lastPoll >= uint64_t(pollMs_) * 1000000ull;
      if (ann != seen || pollDue) {
        if (ann != seen) sq_thread_sleep_ms(50);  // let a device finish creating its ports
        seen = ann;
        lastPoll = now;
        refreshPorts();
      }
      lk.lock();
    }
  }

  std::mutex refreshMu_;  // taken before mu_, never while calling listeners
  std::mutex mu_;
  std::shared_ptr<SeqClient> client_;
  std::vector<PortEntry> ports_;
  bool enumerated_ = false;
  uint64_t lastOpenTry_ = 0;
  std::string lastWhy_;
  std::vector<Ptr<IMidiDeviceListener>> listeners_;
  std::mutex watchMu_;
  std::condition_variable watchCv_;
  bool watchQuit_ = false;
  uint32_t pollMs_ = 2000;
  std::atomic<std::thread::id> watcherId_{};
  std::thread watcher_;
};

// The module registers one driver as the host's IMidiDriver service so consumers that ask the
// host (hardware-surface) get the platform driver on Linux, whatever other MidiDriver classes
// (midi-win's loopback-only build) are loaded.
Ptr<MidiDriver>& serviceDriver() {
  static Ptr<MidiDriver> d;
  return d;
}
IHostContext*& serviceHost() {
  static IHostContext* h = nullptr;
  return h;
}

sq_result onInit(IHostContext* h) {
  auto d = Ptr<MidiDriver>::adopt(new (std::nothrow) MidiDriver());
  if (!d) return SQ_E_OUTOFMEMORY;
  if (SQ_SUCCEEDED(h->registerService(&IMidiDriver::iid, static_cast<IMidiDriver*>(d.get())))) {
    serviceDriver() = d;
    serviceHost() = h;
  }
  return SQ_OK;
}

void onShutdown() {
  Ptr<MidiDriver>& d = serviceDriver();
  if (d && serviceHost()) serviceHost()->unregisterService(&IMidiDriver::iid, static_cast<IMidiDriver*>(d.get()));
  d = nullptr;
  serviceHost() = nullptr;
}

constexpr ClassEntry kClasses[] = {
    classEntry<MidiDriver>(kAlsaMidiDriverClassId, sq::midi::kDriverCategory, "ALSA MIDI Driver"),
};

constexpr ModuleDesc kModule{
    .fourcc = SQ_FOURCC('M', 'I', 'D', 'A'),
    .name = "midi-alsa",
    .description = "Linux MIDI ports via the ALSA sequencer (libasound loaded at runtime)",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace
}  // namespace sq::alsaseq

SQ_MODULE_EXPORTS(sq::alsaseq::kModule)
