// midi-win: port objects shared by the WinMM backend and the built-in loopback ports.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rt_util.hpp"
#include "sq/interfaces/midi.h"
#include "sq/sdk.hpp"

namespace sq::midiwin {

void copyCString(char* dst, size_t cap, const std::string& s);

// Delivery gate in front of an IMidiInputCallback: start/stop without racing in-flight calls.
class InputDelivery {
 public:
  InputDelivery(const sq_midi_port_info& info, IMidiInputCallback* cb) : info_(info), cb_(cb) {}

  // Driver thread. RT-safe.
  void deliver(const sq_midi_timed_event* events, uint32_t count) noexcept {
    inflight_.fetch_add(1, std::memory_order_seq_cst);
    if (running_.load(std::memory_order_seq_cst)) cb_->onMidi(&info_, events, count);
    inflight_.fetch_sub(1, std::memory_order_seq_cst);
  }
  bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  void setRunning() noexcept { running_.store(true, std::memory_order_seq_cst); }
  // Returns once no onMidi() call is in progress any more.
  void stopAndDrain() noexcept {
    running_.store(false, std::memory_order_seq_cst);
    while (inflight_.load(std::memory_order_seq_cst) != 0) sq_thread_yield();
  }
  const sq_midi_port_info& info() const { return info_; }

 private:
  sq_midi_port_info info_;
  Ptr<IMidiInputCallback> cb_;
  std::atomic<bool> running_{false};
  std::atomic<int32_t> inflight_{0};
};

// Where an output port's worker thread puts due events. Called on the worker thread only.
class OutputSink {
 public:
  virtual ~OutputSink() = default;
  virtual void emitShort(const uint8_t* bytes, uint32_t size) noexcept = 0;
  virtual void emitLong(const uint8_t* data, uint32_t size) noexcept = 0;  // complete F0..F7 message
};

// IMidiOutputPort with a lock-free send() and a scheduling worker thread.
class OutputPort final : public Object<OutputPort, IMidiOutputPort> {
 public:
  static constexpr uint32_t kQueueCapacity = 4096;
  static constexpr uint32_t kSysexSlots = 16;
  static constexpr uint32_t kSysexMax = 65536;

  OutputPort(const sq_midi_port_info& info, std::unique_ptr<OutputSink> sink);
  sq_result start();  // spawn the worker; call once after construction

  sq_result SQ_CALL getInfo(sq_midi_port_info* out) override;
  sq_result SQ_CALL send(const sq_midi_timed_event* events, uint32_t count) override;

 public:  // sq::Object deletes through the derived type
  ~OutputPort() override;

 private:
  struct Slot {
    uint64_t time = 0;
    uint32_t size = 0;
    int32_t sysex = -1;  // index into the sysex pool, or -1 for a short message
    uint8_t bytes[4] = {};
  };
  struct Pending {
    uint64_t due;
    uint64_t seq;
    Slot slot;
  };

  static void threadMain(void* self);
  void run();
  void emit(const Slot& s) noexcept;

  sq_midi_port_info info_;
  std::unique_ptr<OutputSink> sink_;
  MpmcQueue<Slot> queue_{kQueueCapacity};
  MpmcQueue<uint32_t> freeSysex_{kSysexSlots};
  std::unique_ptr<uint8_t[]> sysexPool_;
  std::vector<Pending> heap_;
  Waker waker_;
  std::atomic<bool> quit_{false};
  sq_thread* thread_ = nullptr;
};

// ---- loopback ports ------------------------------------------------------------------------
// N virtual cables: what is sent to "loopback:out:K" arrives at every open "loopback:in:K".
constexpr uint32_t kMaxLoopbackPorts = 16;
constexpr uint32_t kMaxLoopbackListeners = 8;

sq_result loopSubscribe(uint32_t cable, InputDelivery* d);
void loopUnsubscribe(uint32_t cable, InputDelivery* d);
std::unique_ptr<OutputSink> makeLoopSink(uint32_t cable);

class LoopInputPort final : public Object<LoopInputPort, IMidiInputPort> {
 public:
  LoopInputPort(const sq_midi_port_info& info, IMidiInputCallback* cb, uint32_t cable) : d_(info, cb), cable_(cable) {}
  sq_result open() { return loopSubscribe(cable_, &d_); }
  sq_result SQ_CALL getInfo(sq_midi_port_info* out) override;
  sq_result SQ_CALL start() override;
  sq_result SQ_CALL stop() override;

 public:  // sq::Object deletes through the derived type
  ~LoopInputPort() override;

 private:
  InputDelivery d_;
  uint32_t cable_;
  bool subscribed_ = false;
  friend sq_result openLoopInput(const sq_midi_port_info&, IMidiInputCallback*, uint32_t, IMidiInputPort**);
};

sq_result openLoopInput(const sq_midi_port_info& info, IMidiInputCallback* cb, uint32_t cable, IMidiInputPort** out);

// ---- WinMM backend (Windows only; stubs elsewhere) -------------------------------------------
struct WinmmDevice {
  std::string name;  // UTF-8 product name as reported by the driver
  uint32_t index;    // device index at enumeration time
};
void winmmEnumerate(std::vector<WinmmDevice>& inputs, std::vector<WinmmDevice>& outputs);
// Re-reads the name of device `index` (SQ_E_NOTFOUND if the index no longer exists).
sq_result winmmDeviceName(bool input, uint32_t index, std::string& name);
sq_result openWinmmInput(const sq_midi_port_info& info, uint32_t deviceIndex, IMidiInputCallback* cb, IMidiInputPort** out);
sq_result openWinmmOutputSink(uint32_t deviceIndex, std::unique_ptr<OutputSink>& out);

}  // namespace sq::midiwin
