// midi-win: output scheduling, loopback cables, shared helpers.
#include "ports.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <mmsystem.h>
#endif

namespace sq::midiwin {

void copyCString(char* dst, size_t cap, const std::string& s) {
  if (!cap) return;
  size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  // Do not cut a UTF-8 sequence in half.
  while (n > 0 && n < s.size() && (uint8_t(s[n]) & 0xC0u) == 0x80u) --n;
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

TimerResolution::TimerResolution() {
#ifdef _WIN32
  timeBeginPeriod(1);
#endif
}
TimerResolution::~TimerResolution() {
#ifdef _WIN32
  timeEndPeriod(1);
#endif
}

// ---- OutputPort ------------------------------------------------------------------------------

OutputPort::OutputPort(const sq_midi_port_info& info, std::unique_ptr<OutputSink> sink)
    : info_(info), sink_(std::move(sink)), sysexPool_(new uint8_t[size_t(kSysexSlots) * kSysexMax]) {
  for (uint32_t i = 0; i < kSysexSlots; ++i) freeSysex_.push(i);
  heap_.reserve(kQueueCapacity);
}

sq_result OutputPort::start() {
  if (!waker_.ok()) return SQ_E_FAIL;
  sq_result r = sq_thread_create(&OutputPort::threadMain, this, "sq-midi-out", &thread_);
  if (SQ_FAILED(r)) return r;
  sq_thread_set_priority(thread_, SQ_PRIORITY_HIGH);
  return SQ_OK;
}

OutputPort::~OutputPort() {
  if (thread_) {
    quit_.store(true, std::memory_order_release);
    waker_.wake();
    sq_thread_join(thread_);
    thread_ = nullptr;
  }
  sink_.reset();
}

sq_result SQ_CALL OutputPort::getInfo(sq_midi_port_info* out) {
  if (!out || out->struct_size < sizeof(sq_midi_port_info)) return SQ_E_INVALIDARG;
  *out = info_;
  out->struct_size = sizeof(sq_midi_port_info);
  return SQ_OK;
}

// RT-safe: lock-free queue pushes, memcpy into a preallocated sysex slot, a non-blocking wake.
sq_result SQ_CALL OutputPort::send(const sq_midi_timed_event* events, uint32_t count) {
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
      std::memcpy(sysexPool_.get() + size_t(idx) * kSysexMax, e.data, e.size);
      s.sysex = int32_t(idx);
      s.size = e.size;
    } else {
      const uint32_t need = shortMessageLength(e.bytes[0]);
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

void OutputPort::threadMain(void* self) { static_cast<OutputPort*>(self)->run(); }

void OutputPort::emit(const Slot& s) noexcept {
  if (s.sysex >= 0) {
    sink_->emitLong(sysexPool_.get() + size_t(s.sysex) * kSysexMax, s.size);
    freeSysex_.push(uint32_t(s.sysex));
  } else {
    sink_->emitShort(s.bytes, s.size);
  }
}

void OutputPort::run() {
  TimerResolution res;
  uint64_t seq = 0;
  // Min-heap on (due, seq): equal due times keep submission order.
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
    int32_t timeout = 250;
    if (!heap_.empty()) {
      const uint64_t wait = heap_.front().due - now;
      timeout = int32_t(std::min<uint64_t>(250, wait / 1000000ull));
    }
    if (timeout > 0) {
      waker_.wait(timeout);
    } else if (!heap_.empty()) {
      sq_thread_yield();  // due within the next millisecond
    }
  }
  for (const Pending& p : heap_)
    if (p.slot.sysex >= 0) freeSysex_.push(uint32_t(p.slot.sysex));
  heap_.clear();
}

// ---- loopback --------------------------------------------------------------------------------

namespace {

struct LoopCable {
  std::atomic<InputDelivery*> subs[kMaxLoopbackListeners] = {};
  std::atomic<int32_t> busy{0};
};

LoopCable& cable(uint32_t i) {
  static LoopCable cables[kMaxLoopbackPorts];
  return cables[i];
}

void loopDeliver(uint32_t idx, const sq_midi_timed_event& e) noexcept {
  LoopCable& c = cable(idx);
  c.busy.fetch_add(1, std::memory_order_seq_cst);
  for (auto& slot : c.subs) {
    InputDelivery* d = slot.load(std::memory_order_seq_cst);
    if (d) d->deliver(&e, 1);
  }
  c.busy.fetch_sub(1, std::memory_order_seq_cst);
}

class LoopSink final : public OutputSink {
 public:
  explicit LoopSink(uint32_t idx) : idx_(idx) {}
  void emitShort(const uint8_t* bytes, uint32_t size) noexcept override {
    sq_midi_timed_event e{};
    e.time_ns = sq_time_monotonic_ns();
    std::memcpy(e.bytes, bytes, size < 4 ? size : 4);
    e.size = size;
    loopDeliver(idx_, e);
  }
  void emitLong(const uint8_t* data, uint32_t size) noexcept override {
    sq_midi_timed_event e{};
    e.time_ns = sq_time_monotonic_ns();
    e.data = data;
    e.size = size;
    loopDeliver(idx_, e);
  }

 private:
  uint32_t idx_;
};

}  // namespace

sq_result loopSubscribe(uint32_t idx, InputDelivery* d) {
  if (idx >= kMaxLoopbackPorts) return SQ_E_NOTFOUND;
  for (auto& slot : cable(idx).subs) {
    InputDelivery* expected = nullptr;
    if (slot.compare_exchange_strong(expected, d, std::memory_order_seq_cst)) return SQ_OK;
  }
  return SQ_E_BUSY;
}

void loopUnsubscribe(uint32_t idx, InputDelivery* d) {
  if (idx >= kMaxLoopbackPorts) return;
  LoopCable& c = cable(idx);
  for (auto& slot : c.subs) {
    InputDelivery* expected = d;
    slot.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst);
  }
  while (c.busy.load(std::memory_order_seq_cst) != 0) sq_thread_yield();
}

std::unique_ptr<OutputSink> makeLoopSink(uint32_t idx) { return std::make_unique<LoopSink>(idx); }

sq_result SQ_CALL LoopInputPort::getInfo(sq_midi_port_info* out) {
  if (!out || out->struct_size < sizeof(sq_midi_port_info)) return SQ_E_INVALIDARG;
  *out = d_.info();
  out->struct_size = sizeof(sq_midi_port_info);
  return SQ_OK;
}

sq_result SQ_CALL LoopInputPort::start() {
  if (d_.running()) return SQ_S_FALSE;
  d_.setRunning();
  return SQ_OK;
}

sq_result SQ_CALL LoopInputPort::stop() {
  if (!d_.running()) return SQ_S_FALSE;
  d_.stopAndDrain();
  return SQ_OK;
}

LoopInputPort::~LoopInputPort() {
  d_.stopAndDrain();
  if (subscribed_) loopUnsubscribe(cable_, &d_);
}

sq_result openLoopInput(const sq_midi_port_info& info, IMidiInputCallback* cb, uint32_t idx, IMidiInputPort** out) {
  auto* p = new LoopInputPort(info, cb, idx);
  sq_result r = p->open();
  if (SQ_FAILED(r)) {
    p->release();
    return r;
  }
  p->subscribed_ = true;
  *out = p;
  return SQ_OK;
}

}  // namespace sq::midiwin
