// midi-win: WinMM backend (midiIn* / midiOut*). Input: the WinMM callback thread feeds an
// InputDelivery (short messages and sysex via two rotating long buffers). Output: an OutputSink
// used by OutputPort's scheduling worker (short messages; sysex through a prepared header).
// Non-Windows builds compile stubs (no devices).
#include "ports.hpp"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#include <cstring>

namespace sq::midiwin {

#ifdef _WIN32
namespace {

std::string utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

uint32_t shortLength(uint8_t status) {
  if (status >= 0xF8) return 1;
  switch (status & 0xF0) {
    case 0xC0:
    case 0xD0: return 2;
    case 0xF0: return status == 0xF1 || status == 0xF3 ? 2 : status == 0xF2 ? 3 : 1;
    default: return 3;
  }
}

class WinmmInputPort final : public Object<WinmmInputPort, IMidiInputPort> {
 public:
  static constexpr uint32_t kLongBuffers = 4;
  static constexpr uint32_t kLongSize = 4096;

  WinmmInputPort(const sq_midi_port_info& info, IMidiInputCallback* cb) : d_(info, cb), recycler_(&WinmmInputPort::readd, this) {}

  sq_result open(uint32_t index) {
    // The recycler re-queues sysex buffers from its own thread: MidiInProc must not call any
    // multimedia function (midiInAddBuffer included) - doing so can deadlock inside WinMM.
    if (!recycler_.start()) return SQ_E_FAIL;
    MMRESULT r = midiInOpen(&h_, index, reinterpret_cast<DWORD_PTR>(&WinmmInputPort::proc), reinterpret_cast<DWORD_PTR>(this),
                            CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
      h_ = nullptr;
      recycler_.shutdown();
      return r == MMSYSERR_ALLOCATED ? SQ_E_BUSY : SQ_E_NOTFOUND;
    }
    for (uint32_t i = 0; i < kLongBuffers; ++i) {
      MIDIHDR& hd = hdr_[i];
      std::memset(&hd, 0, sizeof hd);
      hd.lpData = reinterpret_cast<LPSTR>(buf_[i]);
      hd.dwBufferLength = kLongSize;
      hd.dwUser = i;
      if (midiInPrepareHeader(h_, &hd, sizeof hd) == MMSYSERR_NOERROR) {
        prepared_[i] = true;
        midiInAddBuffer(h_, &hd, sizeof hd);
      }
    }
    return SQ_OK;
  }

  sq_result SQ_CALL getInfo(sq_midi_port_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = d_.info();
    return SQ_OK;
  }
  sq_result SQ_CALL start() override {
    if (!h_) return SQ_E_CLOSED;
    t0_ = sq_time_monotonic_ns();
    d_.setRunning();
    return midiInStart(h_) == MMSYSERR_NOERROR ? SQ_OK : SQ_E_FAIL;
  }
  sq_result SQ_CALL stop() override {
    if (h_) midiInStop(h_);
    d_.stopAndDrain();
    return SQ_OK;
  }

 public:  // sq::Object deletes through the derived type
  ~WinmmInputPort() override {
    // Teardown order: no re-add can start once closing_ is set and the recycler is joined, so
    // midiInReset returns every buffer for good and Unprepare/Close cannot see a re-queued one.
    closing_.store(true, std::memory_order_seq_cst);
    recycler_.shutdown();
    d_.stopAndDrain();
    if (h_) {
      midiInReset(h_);  // returns the long buffers (MIM_LONGDATA, ignored while closing)
      for (uint32_t i = 0; i < kLongBuffers; ++i)
        if (prepared_[i]) midiInUnprepareHeader(h_, &hdr_[i], sizeof hdr_[i]);
      midiInClose(h_);
      h_ = nullptr;
    }
    // A proc invocation that was already past its entry may still be returning: wait it out
    // before the memory it touches goes away.
    while (procInflight_.load(std::memory_order_seq_cst) != 0) sq_thread_yield();
  }

 private:
  // Recycler thread (never the WinMM callback): put buffer `index` back into the driver.
  static void readd(void* user, uint32_t index) {
    auto* self = static_cast<WinmmInputPort*>(user);
    if (index >= kLongBuffers || !self->h_ || self->closing_.load(std::memory_order_seq_cst)) return;
    midiInAddBuffer(self->h_, &self->hdr_[index], sizeof self->hdr_[index]);
  }

  // WinMM callback thread: no multimedia API calls in here (deliver + lock-free post only).
  static void CALLBACK proc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2) {
    auto* self = reinterpret_cast<WinmmInputPort*>(inst);
    if (!self) return;
    self->procInflight_.fetch_add(1, std::memory_order_seq_cst);
    if (msg == MIM_DATA) {
      sq_midi_timed_event e{};
      e.time_ns = self->t0_ + static_cast<uint64_t>(p2) * 1000000ull;  // ms since midiInStart
      auto m = static_cast<uint32_t>(p1);
      e.bytes[0] = static_cast<uint8_t>(m);
      e.bytes[1] = static_cast<uint8_t>(m >> 8);
      e.bytes[2] = static_cast<uint8_t>(m >> 16);
      e.size = shortLength(e.bytes[0]);
      self->d_.deliver(&e, 1);
    } else if (msg == MIM_LONGDATA) {
      auto* hd = reinterpret_cast<MIDIHDR*>(p1);
      const bool closing = self->closing_.load(std::memory_order_seq_cst);
      if (hd && hd->dwBytesRecorded > 0 && !closing) {
        sq_midi_timed_event e{};
        e.time_ns = self->t0_ + static_cast<uint64_t>(p2) * 1000000ull;
        e.data = reinterpret_cast<const uint8_t*>(hd->lpData);
        e.size = hd->dwBytesRecorded;
        self->d_.deliver(&e, 1);
      }
      if (hd && !closing) self->recycler_.post(static_cast<uint32_t>(hd->dwUser));
    }
    self->procInflight_.fetch_sub(1, std::memory_order_seq_cst);
  }

  InputDelivery d_;
  BufferRecycler recycler_;
  HMIDIIN h_ = nullptr;
  uint64_t t0_ = 0;
  std::atomic<bool> closing_{false};
  std::atomic<int32_t> procInflight_{0};
  bool prepared_[kLongBuffers] = {};
  MIDIHDR hdr_[kLongBuffers]{};
  uint8_t buf_[kLongBuffers][kLongSize]{};
};

class WinmmSink final : public OutputSink {
 public:
  explicit WinmmSink(HMIDIOUT h) : h_(h) {}
  ~WinmmSink() override {
    if (h_) {
      midiOutReset(h_);
      midiOutClose(h_);
    }
  }
  void emitShort(const uint8_t* b, uint32_t size) noexcept override {
    DWORD m = b[0] | (size > 1 ? static_cast<DWORD>(b[1]) << 8 : 0u) | (size > 2 ? static_cast<DWORD>(b[2]) << 16 : 0u);
    midiOutShortMsg(h_, m);
  }
  // Worker thread: blocking until the driver consumed the buffer is acceptable here.
  void emitLong(const uint8_t* data, uint32_t size) noexcept override {
    MIDIHDR hd{};
    hd.lpData = reinterpret_cast<LPSTR>(const_cast<uint8_t*>(data));
    hd.dwBufferLength = size;
    hd.dwBytesRecorded = size;
    if (midiOutPrepareHeader(h_, &hd, sizeof hd) != MMSYSERR_NOERROR) return;
    if (midiOutLongMsg(h_, &hd, sizeof hd) == MMSYSERR_NOERROR)
      for (int i = 0; i < 2000 && !(hd.dwFlags & MHDR_DONE); ++i) Sleep(1);
    midiOutUnprepareHeader(h_, &hd, sizeof hd);
  }

 private:
  HMIDIOUT h_;
};

}  // namespace

void winmmEnumerate(std::vector<WinmmDevice>& inputs, std::vector<WinmmDevice>& outputs) {
  inputs.clear();
  outputs.clear();
  UINT ni = midiInGetNumDevs();
  for (UINT i = 0; i < ni; ++i) {
    MIDIINCAPSW caps{};
    if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) inputs.push_back(WinmmDevice{utf8(caps.szPname), i});
  }
  UINT no = midiOutGetNumDevs();
  for (UINT i = 0; i < no; ++i) {
    MIDIOUTCAPSW caps{};
    if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) outputs.push_back(WinmmDevice{utf8(caps.szPname), i});
  }
}

sq_result winmmDeviceName(bool input, uint32_t index, std::string& name) {
  if (input) {
    MIDIINCAPSW caps{};
    if (midiInGetDevCapsW(index, &caps, sizeof caps) != MMSYSERR_NOERROR) return SQ_E_NOTFOUND;
    name = utf8(caps.szPname);
  } else {
    MIDIOUTCAPSW caps{};
    if (midiOutGetDevCapsW(index, &caps, sizeof caps) != MMSYSERR_NOERROR) return SQ_E_NOTFOUND;
    name = utf8(caps.szPname);
  }
  return SQ_OK;
}

sq_result openWinmmInput(const sq_midi_port_info& info, uint32_t deviceIndex, IMidiInputCallback* cb, IMidiInputPort** out) {
  auto p = Ptr<WinmmInputPort>::adopt(new WinmmInputPort(info, cb));
  sq_result r = p->open(deviceIndex);
  if (SQ_FAILED(r)) return r;
  *out = p.detach();
  return SQ_OK;
}

sq_result openWinmmOutputSink(uint32_t deviceIndex, std::unique_ptr<OutputSink>& out) {
  HMIDIOUT h = nullptr;
  MMRESULT r = midiOutOpen(&h, deviceIndex, 0, 0, CALLBACK_NULL);
  if (r != MMSYSERR_NOERROR) return r == MMSYSERR_ALLOCATED ? SQ_E_BUSY : SQ_E_NOTFOUND;
  out = std::make_unique<WinmmSink>(h);
  return SQ_OK;
}

#else  // !_WIN32

void winmmEnumerate(std::vector<WinmmDevice>& inputs, std::vector<WinmmDevice>& outputs) {
  inputs.clear();
  outputs.clear();
}
sq_result winmmDeviceName(bool, uint32_t, std::string&) { return SQ_E_NOTFOUND; }
sq_result openWinmmInput(const sq_midi_port_info&, uint32_t, IMidiInputCallback*, IMidiInputPort** out) {
  if (out) *out = nullptr;
  return SQ_E_UNSUPPORTED;
}
sq_result openWinmmOutputSink(uint32_t, std::unique_ptr<OutputSink>&) { return SQ_E_UNSUPPORTED; }

#endif

}  // namespace sq::midiwin
