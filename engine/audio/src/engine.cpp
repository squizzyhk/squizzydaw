// Squizzy Studio engine/audio - EngineCore (RT) + AudioEngine (IAudioEngine service).
#include "sq/engine/audio/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "sq/cpp/attributes.hpp"
#include "sq/engine/audio/builtin.hpp"
#include "sq/platform.h"
#include "sq/platform.hpp"

namespace sq::engine::audio {

// =============================================================================================
// EngineCore
// =============================================================================================
EngineCore::EngineCore() = default;

EngineCore::~EngineCore() {
  if (sessionOwner_) {
    session_.store(nullptr);
    sessionOwner_->finish();
  }
  lingering_.clear();
  // Commands never applied: free the automation data they own.
  if (hasHeld_ && held_.type == CmdType::SetAutomation) delete static_cast<AutomationData*>(held_.ptr);
  hasHeld_ = false;
  Command c;
  while (cmds_.pop(c))
    if (c.type == CmdType::SetAutomation) delete static_cast<AutomationData*>(c.ptr);
  Graph* g = nullptr;
  while (retire_.pop(g)) delete g;
  AutomationData* a = nullptr;
  while (autoRetire_.pop(a)) delete a;
  delete pending_.exchange(nullptr);
  delete cur_;
  cur_ = nullptr;
}

void EngineCore::setGraph(std::unique_ptr<Graph> g) {
  if (g) {
    generation_.store(g->generation());
    latency_.store(g->latency());
  }
  newest_ = g.get();
  if (!running_.load()) {
    collectGarbage();
    delete pending_.exchange(nullptr);
    delete cur_;
    cur_ = g.release();
    publish();
    return;
  }
  // The RT side takes the pointer with an exchange; whatever we get back was never seen by it.
  Graph* old = pending_.exchange(g.release(), std::memory_order_acq_rel);
  delete old;
}

void EngineCore::collectGarbage() {
  Graph* g = nullptr;
  while (retire_.pop(g)) delete g;
  AutomationData* a = nullptr;
  while (autoRetire_.pop(a)) delete a;
  if (!lingering_.empty()) {
    const bool idle = !running_.load();
    const uint64_t now = epoch_.load(std::memory_order_acquire);
    for (size_t i = 0; i < lingering_.size();) {
      if (idle || now >= lingering_[i].first) lingering_.erase(lingering_.begin() + static_cast<std::ptrdiff_t>(i));
      else ++i;
    }
  }
}

bool EngineCore::post(const Command& c) {
  if (!running_.load()) {
    collectGarbage();
    installPending();
    apply(c);
    publish();
    return true;
  }
  return cmds_.push(c);
}

bool EngineCore::pushLiveMidi(const uint8_t* bytes, uint32_t size) noexcept {
  if (!bytes || size == 0) return true;
  bool ok = true;
  // Parsed outside the lock; the lock (producers only) is held just for the queue pushes, and a
  // producer that finds it taken yields (the holder may be a descheduled lower-priority thread).
  constexpr uint32_t kBatch = 32;
  LiveMidiEvent batch[kBatch];
  uint32_t nb = 0;
  auto flush = [&]() {
    if (nb == 0) return;
    for (uint32_t spins = 0; liveLock_.test_and_set(std::memory_order_acquire); ++spins)
      if (spins >= 16) sq_thread_yield();
    for (uint32_t k = 0; k < nb; ++k)
      if (!live_.push(batch[k])) {
        liveDropped_.fetch_add(1, std::memory_order_relaxed);
        ok = false;
      }
    liveLock_.clear(std::memory_order_release);
    nb = 0;
  };
  uint8_t status = 0;
  uint32_t i = 0;
  while (i < size) {
    const uint8_t b = bytes[i];
    if (b >= 0xF8) {  // realtime: skip
      ++i;
      continue;
    }
    if (b >= 0xF0) {  // sysex / system common: skip up to the next status byte
      status = 0;
      ++i;
      while (i < size && bytes[i] < 0x80) ++i;
      continue;
    }
    if (b & 0x80) {
      status = b;
      ++i;
    }
    if (!status) {  // data byte without a status
      ++i;
      continue;
    }
    const uint32_t dataLen = ((status & 0xF0u) == 0xC0 || (status & 0xF0u) == 0xD0) ? 1u : 2u;
    if (i + dataLen > size) break;  // incomplete message
    batch[nb++] = LiveMidiEvent{{status, bytes[i], dataLen > 1 ? bytes[i + 1] : uint8_t{0}}, static_cast<uint8_t>(1 + dataLen)};
    i += dataLen;
    if (nb == kBatch) flush();
  }
  flush();
  return ok;
}

size_t EngineCore::drainCapturedMidi(std::vector<CapturedMidi>& out) {
  size_t n = 0;
  CapturedMidi c;
  while (capture_.pop(c)) {
    out.push_back(c);
    ++n;
  }
  return n;
}

void EngineCore::setDeviceLatency(uint32_t inFrames, uint32_t outFrames) noexcept {
  recordComp_.store(inFrames + outFrames);
  outLatency_.store(outFrames);
}

void EngineCore::startRecording(std::unique_ptr<RecordSession> session) {
  stopRecording(nullptr);
  if (!session) return;
  session->start();
  sessionOwner_ = std::move(session);
  session_.store(sessionOwner_.get(), std::memory_order_release);
}

bool EngineCore::stopRecording(std::vector<RecordedTake>* takes) {
  if (!sessionOwner_) return false;
  session_.store(nullptr, std::memory_order_release);
  // Any callback that started before the store may still push into the session; two completed
  // callbacks after it guarantee none does.
  const uint64_t safeEpoch = epoch_.load(std::memory_order_acquire) + 2;
  const bool confirmed = waitForCallback(1000);
  std::unique_ptr<RecordSession> s = std::move(sessionOwner_);
  std::vector<RecordedTake> t = s->finish();  // stops the writer; the rings stay allocated
  if (takes) *takes = std::move(t);
  if (!confirmed && running_.load()) lingering_.emplace_back(safeEpoch, std::move(s));
  return true;
}

bool EngineCore::waitForCallback(uint32_t timeoutMs) {
  if (!running_.load()) return true;
  // A callback in flight may still hold an old pointer; two completed callbacks after the
  // store guarantee the RT side has observed it.
  const uint64_t target = epoch_.load(std::memory_order_acquire) + 2;
  const uint64_t deadline = sq_time_monotonic_ns() + static_cast<uint64_t>(timeoutMs) * 1000000ull;
  while (epoch_.load(std::memory_order_acquire) < target) {
    if (!running_.load()) return true;
    if (sq_time_monotonic_ns() > deadline) return false;
    sq_thread_sleep_ms(1);
  }
  return true;
}

uint64_t EngineCore::droppedRecordFrames() const noexcept {
  uint64_t n = 0;
  if (sessionOwner_)
    for (size_t i = 0; i < sessionOwner_->streamCount(); ++i) n += sessionOwner_->stream(i).overflowFrames();
  return n;
}

TransportSnapshot EngineCore::transport() const noexcept {
  TransportSnapshot s;
  for (int tries = 0; tries < 10000; ++tries) {
    const uint32_t s1 = seq_.load(std::memory_order_acquire);
    if (s1 & 1u) {
      std::atomic_thread_fence(std::memory_order_acquire);
      continue;
    }
    s.flags = tFlags_.load(std::memory_order_relaxed);
    s.frame = tFrame_.load(std::memory_order_relaxed);
    s.ppq = tPpq_.load(std::memory_order_relaxed);
    s.bpm = tBpm_.load(std::memory_order_relaxed);
    s.numerator = tNum_.load(std::memory_order_relaxed);
    s.denominator = tDen_.load(std::memory_order_relaxed);
    s.loopStart = tLoopStart_.load(std::memory_order_relaxed);
    s.loopEnd = tLoopEnd_.load(std::memory_order_relaxed);
    s.loopStartPpq = tLoopStartPpq_.load(std::memory_order_relaxed);
    s.loopEndPpq = tLoopEndPpq_.load(std::memory_order_relaxed);
    s.sampleRate = tRate_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_.load(std::memory_order_relaxed) == s1) break;
  }
  return s;
}

void EngineCore::publish() noexcept {
  const uint32_t s = seq_.load(std::memory_order_relaxed);
  seq_.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  uint32_t flags = 0;
  if (playing_) flags |= SQ_TRANSPORT_PLAYING;
  if (recording_ && playing_) flags |= SQ_TRANSPORT_RECORDING;
  if (looping_) flags |= SQ_TRANSPORT_LOOPING;
  if (metronome_) flags |= SQ_TRANSPORT_METRONOME;
  tFlags_.store(flags, std::memory_order_relaxed);
  tFrame_.store(pos_, std::memory_order_relaxed);
  if (cur_) {
    tPpq_.store(cur_->tempo().frameToPpq(pos_), std::memory_order_relaxed);
    tBpm_.store(cur_->tempo().bpmAt(pos_), std::memory_order_relaxed);
    tNum_.store(cur_->numerator(), std::memory_order_relaxed);
    tDen_.store(cur_->denominator(), std::memory_order_relaxed);
    tLoopStartPpq_.store(cur_->tempo().frameToPpq(loopStart_), std::memory_order_relaxed);
    tLoopEndPpq_.store(cur_->tempo().frameToPpq(loopEnd_), std::memory_order_relaxed);
    tRate_.store(cur_->sampleRate(), std::memory_order_relaxed);
  }
  tLoopStart_.store(loopStart_, std::memory_order_relaxed);
  tLoopEnd_.store(loopEnd_, std::memory_order_relaxed);
  seq_.store(s + 2, std::memory_order_release);
}

void EngineCore::installPending() noexcept {
  if (pending_.load(std::memory_order_acquire) == nullptr) return;
  if (cur_ && retire_.freeApprox() == 0) return;  // control thread is behind on garbage: next block
  Graph* p = pending_.exchange(nullptr, std::memory_order_acq_rel);
  if (!p) return;
  if (cur_) retire_.push(cur_);
  cur_ = p;
}

namespace {
// Commands addressed to one graph (Command::generation).
bool carriesGeneration(CmdType t) noexcept {
  switch (t) {
    case CmdType::Play:
    case CmdType::Stop:
    case CmdType::Locate:
    case CmdType::Loop:
    case CmdType::Metronome:
    case CmdType::Record:
    case CmdType::LowLatency:
    case CmdType::MasterVolume:
    case CmdType::MasterPan:
    case CmdType::MasterMute:
    case CmdType::MidiCapture: return false;
    default: return true;
  }
}
}  // namespace

bool EngineCore::deferForPending(const Command& c) noexcept {
  if (!carriesGeneration(c.type)) return false;
  if (cur_ && c.generation == cur_->generation()) return false;
  // Generations only grow: a command newer than the installed graph targets the published one.
  if (cur_ && c.generation < cur_->generation()) return false;
  installPending();
  if (cur_ && c.generation <= cur_->generation()) return false;
  return pending_.load(std::memory_order_acquire) != nullptr;  // install deferred: keep it
}

void EngineCore::drainCommands() noexcept {
  if (hasHeld_) {
    if (deferForPending(held_)) return;
    hasHeld_ = false;
    apply(held_);
  }
  Command c;
  while (cmds_.pop(c)) {
    if (deferForPending(c)) {
      held_ = c;
      hasHeld_ = true;
      return;
    }
    apply(c);
  }
}

void EngineCore::captureLive(const LiveMidiEvent& e) noexcept {
  // The player reacts to what they hear: the output of pos_ is heard outLatency frames later.
  const int64_t len = loopEnd_ - loopStart_;
  const bool loopOn = looping_ && len > 0;
  int64_t f = pos_ - static_cast<int64_t>(outLatency_.load(std::memory_order_relaxed));
  uint64_t pass = transportPass_, wraps = loopWraps_;
  if (loopOn && capWrapped_ && transportPass_ == wrapPass_ && f < loopStart_ && pos_ >= loopStart_ && pos_ < loopEnd_) {
    f += len;  // heard before the wrap
    --pass;
    --wraps;
  }
  auto push = [&](int64_t frame, uint8_t b0, uint8_t b1, uint8_t b2) {
    CapturedMidi cm{frame, {b0, b1, b2}, 3};
    capture_.push(cm);  // full: dropped (the control thread drains it in idle())
  };
  // A new transport pass (loop wrap / locate) is detected by counter, not by f going backwards
  // (latency-adjusted frames can step back within one pass).
  if (capLast_ != INT64_MIN && pass != capPass_) {
    // The transport went back (loop wrap / locate): held notes end where the old pass ended and
    // start again at the new position.
    const int64_t closeAt = (loopOn && capLast_ >= loopStart_ && capLast_ < loopEnd_) ? loopEnd_ : capLast_;
    const int64_t reopenAt = (loopOn && f >= loopStart_ && f < loopEnd_) ? loopStart_ : f;
    // Passes the note was held through completely (several wraps without a live event, no locate)
    // become whole-loop notes; bounded so a note held for a long time cannot flood the RT thread.
    uint64_t whole = 0;
    if (loopOn && pass - capPass_ == wraps - capWraps_ && wraps - capWraps_ > 1) whole = std::min<uint64_t>(wraps - capWraps_ - 1, 16);
    for (uint32_t ch = 0; ch < 16; ++ch)
      for (uint32_t k = 0; k < 128; ++k)
        if (const uint8_t vel = capVel_[ch][k]) {
          const auto kb = static_cast<uint8_t>(k);
          push(closeAt, static_cast<uint8_t>(0x80u | ch), kb, 0);
          for (uint64_t w = 0; w < whole; ++w) {
            push(loopStart_, static_cast<uint8_t>(0x90u | ch), kb, vel);
            push(loopEnd_, static_cast<uint8_t>(0x80u | ch), kb, 0);
          }
          push(reopenAt, static_cast<uint8_t>(0x90u | ch), kb, vel);
        }
  }
  capLast_ = f;
  capPass_ = pass;
  capWraps_ = wraps;
  const uint8_t st = e.bytes[0] & 0xF0u, ch = e.bytes[0] & 0x0Fu, key = e.bytes[1] & 0x7Fu;
  if (st == 0x90 && e.bytes[2] > 0) capVel_[ch][key] = e.bytes[2];
  else if (st == 0x80 || st == 0x90) capVel_[ch][key] = 0;
  CapturedMidi cm{f, {e.bytes[0], e.bytes[1], e.bytes[2]}, e.size};
  capture_.push(cm);
}

void EngineCore::apply(const Command& c) noexcept {
  switch (c.type) {
    case CmdType::Play: playing_ = true; break;
    case CmdType::Stop: playing_ = false; recording_ = false; break;
    case CmdType::Locate:
      pos_ = c.a;
      ++transportPass_;
      break;
    case CmdType::Loop:
      loopStart_ = c.a;
      loopEnd_ = c.b;
      looping_ = c.value != 0.0 && c.b > c.a;
      break;
    case CmdType::Metronome: metronome_ = c.value != 0.0; break;
    case CmdType::Record:
      recording_ = c.value != 0.0;
      if (recording_) {
        recStartFrame_.store(pos_, std::memory_order_relaxed);
        std::memset(capVel_, 0, sizeof capVel_);
        capLast_ = INT64_MIN;
        capWrapped_ = false;
      }
      break;
    case CmdType::MidiCapture: midiCapture_ = c.value != 0.0; break;
    case CmdType::SetAutomation: {
      auto* data = static_cast<AutomationData*>(c.ptr);
      AutomationData* old = data;
      if (cur_ && c.generation == cur_->generation()) old = cur_->swapAutomation(data);
      if (old && !autoRetire_.push(old)) {
        // Cannot happen (the control thread drains the queue before every post); never free here.
      }
      break;
    }
    case CmdType::LowLatency: lowLatency_ = c.value != 0.0; break;
    case CmdType::MasterVolume:
      if (cur_) cur_->setMasterVolume(static_cast<float>(c.value));
      break;
    case CmdType::MasterPan:
      if (cur_) cur_->setMasterPan(static_cast<float>(c.value));
      break;
    case CmdType::MasterMute:
      if (cur_) cur_->setMasterMute(c.value != 0.0);
      break;
    default: {
      if (!cur_ || c.generation != cur_->generation()) break;
      if (c.type == CmdType::MasterParam) {
        cur_->setMasterParam(static_cast<int32_t>(c.a), static_cast<uint32_t>(c.b), static_cast<float>(c.value));
        break;
      }
      if (c.index < 0) break;
      const size_t i = static_cast<size_t>(c.index);
      if (c.type == CmdType::BusVolume || c.type == CmdType::BusMute || c.type == CmdType::BusPan || c.type == CmdType::BusParam ||
          c.type == CmdType::BusSendLevel || c.type == CmdType::BusSolo) {
        if (i >= cur_->busCount()) break;
        if (c.type == CmdType::BusVolume) cur_->bus(i).setVolume(static_cast<float>(c.value));
        else if (c.type == CmdType::BusMute) cur_->bus(i).setMute(c.value != 0.0);
        else if (c.type == CmdType::BusPan) cur_->bus(i).setPan(static_cast<float>(c.value));
        else if (c.type == CmdType::BusSolo) {
          cur_->bus(i).setSolo(c.value != 0.0);
          cur_->updateSolo();
        }
        else if (c.type == CmdType::BusParam) cur_->setBusParam(i, static_cast<int32_t>(c.a), static_cast<uint32_t>(c.b), static_cast<float>(c.value));
        else cur_->setBusSendLevel(i, static_cast<size_t>(c.a), static_cast<float>(c.value));
        break;
      }
      if (i >= cur_->trackCount()) break;
      TrackProcessor& t = cur_->track(i);
      switch (c.type) {
        case CmdType::TrackVolume: t.setVolume(static_cast<float>(c.value)); break;
        case CmdType::TrackPan: t.setPan(static_cast<float>(c.value)); break;
        case CmdType::TrackMute: t.setMute(c.value != 0.0); break;
        case CmdType::TrackSolo:
          t.setSolo(c.value != 0.0);
          cur_->updateSolo();
          break;
        case CmdType::TrackMonitor: t.setMonitor(c.value != 0.0); break;
        case CmdType::TrackLiveMidi: t.setLiveMidi(c.value != 0.0); break;
        case CmdType::TrackParam: {
          const int32_t slot = static_cast<int32_t>(c.a);
          const uint32_t param = static_cast<uint32_t>(c.b);
          if (c.fastLaneOnly) cur_->setTrackFastParam(i, slot, param, static_cast<float>(c.value));
          else cur_->setTrackParam(i, slot, param, static_cast<float>(c.value));
          break;
        }
        case CmdType::TrackSendLevel: cur_->setTrackSendLevel(i, static_cast<size_t>(c.a), static_cast<float>(c.value)); break;
        default: break;
      }
      break;
    }
  }
}

sq_result EngineCore::onStart(const sq_stream_format*, const sq_stream_format* output, const sq_latency_info* latency) {
  if (output) sampleRate_ = output->sample_rate;
  if (latency) {
    recordComp_.store(latency->input_frames + latency->output_frames);
    outLatency_.store(latency->output_frames);
  }
  LiveMidiEvent stale;
  while (live_.pop(stale)) {
  }  // the audio thread is not running yet: we are the only consumer
  collectGarbage();
  installPending();
  running_.store(true);
  return SQ_OK;
}

void EngineCore::onStop() {
  running_.store(false);
  // The audio thread is gone: we are the only consumer. Commands still queued are applied now, in
  // order, so they never run after commands that post() applies directly from here on.
  installPending();
  collectGarbage();
  installPending();
  drainCommands();
  collectGarbage();
  publish();
}

void EngineCore::process(const sq_audio_io* io) {
  rt::Scope scope;
  const uint64_t t0 = sq_time_monotonic_ns();
  installPending();
  drainCommands();
  if (!io) return;
  if (io->flags & SQ_AUDIO_IO_XRUN) xruns_.fetch_add(1, std::memory_order_relaxed);
  sq_audio_block* outb = io->output;
  const uint32_t frames = outb ? outb->frames : 0;
  const uint32_t outCh = outb ? std::min<uint32_t>(outb->channel_count, kMaxChannels) : 0;
  if (!cur_ || !outb) {
    for (uint32_t ch = 0; ch < outCh; ++ch) std::memset(outb->channels[ch], 0, sizeof(float) * frames);
    publish();
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    return;
  }
  {
    LiveMidiEvent e;
    while (live_.pop(e)) {
      cur_->routeLiveMidi(e.bytes, e.size);
      if (recording_ && playing_ && midiCapture_) captureLive(e);
    }
  }
  const sq_audio_block* in = io->input;
  const uint32_t inCh = in ? std::min<uint32_t>(in->channel_count, kMaxChannels) : 0;
  RecordSession* sess = session_.load(std::memory_order_acquire);
  const uint32_t comp = recordComp_.load(std::memory_order_relaxed);
  const uint32_t maxB = cur_->maxBlock();
  uint32_t off = 0;
  const int64_t lat = static_cast<int64_t>(cur_->latency());
  while (off < frames) {
    uint32_t n = std::min(frames - off, maxB);
    // Sources are read `lat` frames ahead of the output (PDC). While a loop is active (the output
    // has not passed its end) the read position wraps at the loop end lat frames before the
    // output does, so the first frames after the wrap come from the loop start and not from the
    // material behind the loop end. Blocks are split where either cursor reaches the loop end.
    // A transport positioned at/after the loop end plays on (the loop is not active then).
    const bool loopActive = playing_ && looping_ && pos_ < loopEnd_;
    int64_t readPos = pos_ + lat;
    if (loopActive) {
      if (pos_ + n > loopEnd_) n = static_cast<uint32_t>(loopEnd_ - pos_);
      const int64_t len = loopEnd_ - loopStart_;
      if (readPos >= loopEnd_) readPos = loopStart_ + (readPos - loopEnd_) % len;
      if (readPos + n > loopEnd_) n = static_cast<uint32_t>(loopEnd_ - readPos);
    }
    float* op[kMaxChannels];
    const float* ip[kMaxChannels];
    for (uint32_t ch = 0; ch < outCh; ++ch) op[ch] = outb->channels[ch] + off;
    for (uint32_t ch = 0; ch < inCh; ++ch) ip[ch] = in->channels[ch] + off;
    ProcessContext ctx;
    ctx.sampleRate = cur_->sampleRate();
    ctx.frames = n;
    ctx.outputFrame = pos_;
    ctx.timelineFrame = readPos;
    ctx.timelineExplicit = true;
    ctx.playing = playing_;
    ctx.input = inCh ? ip : nullptr;
    ctx.inputChannels = inCh;
    cur_->process(ctx, op, outCh, lowLatency_, metronome_ && playing_);
    if (recording_ && playing_ && sess) {
      for (size_t s = 0; s < sess->streamCount(); ++s) sess->stream(s).push(inCh ? ip : nullptr, inCh, n, pos_ - static_cast<int64_t>(comp));
    }
    if (playing_) {
      pos_ += n;
      if (loopActive && pos_ >= loopEnd_) {
        pos_ = loopStart_;
        ++transportPass_;
        ++loopWraps_;
        wrapPass_ = transportPass_;
        if (recording_) capWrapped_ = true;
      }
    }
    off += n;
  }
  const double sr = cur_->sampleRate() > 0 ? cur_->sampleRate() : 48000.0;
  const double budget = static_cast<double>(frames) * 1e9 / sr;
  const double load = budget > 0 ? static_cast<double>(sq_time_monotonic_ns() - t0) / budget : 0.0;
  cpuAvg_ = cpuAvg_ * 0.9 + load * 0.1;
  cpu_.store(cpuAvg_, std::memory_order_relaxed);
  publish();
  epoch_.fetch_add(1, std::memory_order_acq_rel);
}

// =============================================================================================
// AudioEngine
// =============================================================================================
namespace {

class DocObserver final : public sq::Object<DocObserver, IObserver> {
 public:
  explicit DocObserver(AudioEngine* e) : engine_(e) {}
  void detach() { engine_ = nullptr; }
  void SQ_CALL onNotify(IUnknown*, const char* topic, IAttributes* args) override {
    if (!engine_ || !topic || std::strcmp(topic, "document.changed") != 0) return;
    const std::string reason = args ? sq::getAttrString(args, "reason") : std::string();  // document.h
    using Change = AudioEngine::DocChange;
    engine_->onDocumentChanged(reason == "commit" ? Change::Commit
                               : reason == "undo" ? Change::Undo
                               : reason == "redo" ? Change::Redo
                                                  : Change::Other);
  }

 private:
  AudioEngine* engine_;
};

// Driver notification thread -> main thread: only atomics (audiodevice.h: "just post").
class DeviceListener final : public sq::Object<DeviceListener, IAudioDeviceListener> {
 public:
  DeviceListener(std::shared_ptr<std::atomic<int32_t>> reset, std::shared_ptr<std::atomic<bool>> list)
      : reset_(std::move(reset)), list_(std::move(list)) {}
  void SQ_CALL onDeviceListChanged() override { list_->store(true); }
  void SQ_CALL onDeviceReset(IAudioDevice*, int32_t reason) override {
    if (reason == 0) reason = SQ_AUDIO_RESET_REQUESTED;
    if (reason == SQ_AUDIO_RESET_DEVICE_LOST) {
      reset_->store(reason);  // a loss wins over any pending re-configure request
      return;
    }
    int32_t expected = 0;
    reset_->compare_exchange_strong(expected, reason);
  }

 private:
  std::shared_ptr<std::atomic<int32_t>> reset_;
  std::shared_ptr<std::atomic<bool>> list_;
};

std::string sanitize(const std::string& s) {
  std::string o;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
    o += ok ? c : '_';
  }
  if (o.empty()) o = "track";
  return o;
}

std::string uniquePath(const std::string& dir, const std::string& stem, const std::string& ext) {
  for (int i = 1; i < 100000; ++i) {
    std::string p = sq::joinPath(dir, stem + "_" + std::to_string(i) + "." + ext);
    if (!sq_fs_exists(p.c_str())) return p;
  }
  return sq::joinPath(dir, stem + "." + ext);
}

std::string dirOf(const std::string& path) {
  const size_t s = path.find_last_of("/\\");
  return s == std::string::npos ? std::string() : path.substr(0, s);
}

}  // namespace

AudioEngine::AudioEngine(IClassRegistry* reg, ISettings* settings) : reg_(reg), settings_(settings) {
  core_ = sq::Ptr<EngineCore>::adopt(new EngineCore());
  if (settings_) mix64_ = settings_->getBool("Engine.Mix64", SQ_FALSE) != 0;
  meterSlotFor(sq_guid{});  // table entry 0 = master (slot 0)
  tempo_.set({}, desc_.sampleRate);
  compileAndInstall();
}

AudioEngine::~AudioEngine() {
  closeDevice();
  detachDocument();
  for (auto& t : mainThreadTasks_) {
    t->cancel();
    if (t->awaitingMainThread()) t->completeOnMainThread(SQ_E_CLOSED);
  }
  mainThreadTasks_.clear();
  reapOfflineGraphs(true, 30000);
}

void AudioEngine::detachDocument() {
  if (doc_ && docObserver_) {
    sq::Ptr<ISubject> subj = doc_.as<ISubject>();
    if (subj) subj->removeObserver(docObserver_.get());
  }
  if (docObserver_) static_cast<DocObserver*>(docObserver_.get())->detach();
  docObserver_ = nullptr;
  doc_ = nullptr;
}

sq_result AudioEngine::registerService(IHostContext* host) {
  if (!host) return SQ_E_INVALIDARG;
  return host->registerService(&IAudioEngine::iid, static_cast<IAudioEngine*>(this));
}
void AudioEngine::unregisterService(IHostContext* host) {
  if (host) host->unregisterService(&IAudioEngine::iid, static_cast<IAudioEngine*>(this));
}

// Main thread. Table index == meter slot (index 0 = master, null id).
int32_t AudioEngine::meterSlotFor(const sq_guid& id) {
  auto it = meterIndex_.find(id);
  if (it != meterIndex_.end()) return meterTable_[it->second].slot;
  const uint32_t n = meterCount_.load(std::memory_order_relaxed);
  if (n >= MeterBank::kSlots) return -1;
  MeterEntry& e = meterTable_[n];
  e.id = id;
  e.slot = static_cast<int32_t>(n);
  e.channels.store(0, std::memory_order_relaxed);
  meterIndex_[id] = n;
  meterCount_.store(n + 1, std::memory_order_release);  // publishes the entry to readers
  return e.slot;
}

void AudioEngine::setMeterChannels(const sq_guid& id, uint32_t channels) {
  auto it = meterIndex_.find(id);
  if (it != meterIndex_.end()) meterTable_[it->second].channels.store(channels, std::memory_order_release);
}

int32_t AudioEngine::trackIndex(const sq_guid& id) const {
  for (size_t i = 0; i < desc_.tracks.size(); ++i)
    if (desc_.tracks[i].id == id) return static_cast<int32_t>(i);
  return -1;
}

sq_result AudioEngine::compileAndInstall() {
  GraphDesc d = desc_;
  if (device_) d.sampleRate = sampleRate_;
  for (auto& t : d.tracks) {
    t.meterSlot = meterSlotFor(t.id);
    auto m = monitorOverride_.find(t.id);
    if (m != monitorOverride_.end()) t.monitor = m->second;
    auto a = armOverride_.find(t.id);
    if (a != armOverride_.end()) t.armed = a->second;
  }
  for (size_t i = 0; i < d.tracks.size(); ++i) {
    d.tracks[i].liveMidi = wantsLiveMidi(d.tracks[i]);
    if (i < desc_.tracks.size()) desc_.tracks[i].liveMidi = d.tracks[i].liveMidi;
  }
  for (auto& b : d.buses) b.meterSlot = meterSlotFor(b.id);
  d.masterMeterSlot = 0;
  GraphConfig cfg;
  cfg.maxBlock = std::max<uint32_t>(blockFrames_, 16);
  cfg.outputChannels = outChannels_ ? outChannels_ : 2;
  cfg.mix64 = mix64_;
  cfg.meters = &core_->meters();
  std::string err;
  ++compileCount_;
  const bool carry = carryNext_;  // not across a document switch (the newest graph is another song's)
  ParkPlan plan;
  if (carry) captureStatesInto(d, &plan);
  std::unique_ptr<Graph> g = Graph::compile(d, cfg, &err);
  if (!g) {
    // invalid bus routing (e.g. a cycle from the document): fall back to routing buses to master
    // without bus-to-bus sends
    for (auto& b : d.buses) {
      b.output = -1;
      b.sends.clear();
    }
    g = Graph::compile(d, cfg, &err);
  }
  if (!g) return SQ_E_FAIL;
  nodeLatency_.clear();
  setMeterChannels(sq_guid{}, cfg.outputChannels);
  auto destCh = [&](int32_t dest) -> uint32_t {
    if (dest < 0) return cfg.outputChannels;
    const uint32_t c = d.buses[static_cast<size_t>(dest)].channels;
    return c == 0 ? 1 : std::min<uint32_t>(c, kMaxChannels);
  };
  for (size_t i = 0; i < d.tracks.size(); ++i) {
    nodeLatency_[d.tracks[i].id] = NodeLatency{g->trackArrival(i), g->trackCompensation(i)};
    setMeterChannels(d.tracks[i].id, destCh(d.tracks[i].output));
  }
  for (size_t b = 0; b < d.buses.size(); ++b) {
    nodeLatency_[d.buses[b].id] = NodeLatency{g->busArrival(b), g->busCompensation(b)};
    setMeterChannels(d.buses[b].id, destCh(d.buses[b].output));
  }
  const bool changed = graphLatency_ != g->latency();
  graphLatency_ = g->latency();
  tempo_.set(d.tempo, d.sampleRate);
  core_->collectGarbage();
  core_->setGraph(std::move(g));
  // The installed graph is now what the next carry pairs with; a failed compile above keeps the
  // previous graph, its identity, the kept states and carryNext_ as they were.
  if (carry)
    commitParkPlan(std::move(plan));
  else
    liveIdent_ = identify(d);
  carryNext_ = true;
  if (changed) notify("engine.latency");
  return SQ_OK;
}

sq_result AudioEngine::rebuild() { return rebuildFromDocument(false); }

namespace {
// Chain index of a "user.inserts" line, -1 when bypassed / unavailable / out of range.
int32_t chainOf(const std::vector<int32_t>& map, int32_t slot) {
  return slot >= 0 && static_cast<size_t>(slot) < map.size() ? map[static_cast<size_t>(slot)] : -1;
}
// Line index whose chain index is `chain` (the inverse of chainOf), -1 when none.
int32_t slotOf(const std::vector<int32_t>& map, size_t chain) {
  for (size_t i = 0; i < map.size(); ++i)
    if (map[i] == static_cast<int32_t>(chain)) return static_cast<int32_t>(i);
  return -1;
}
bool captureInto(const AudioProcessor* p, PluginState* out) {
  if (!p || !p->pluginInstance()) return false;
  if (auto* fx = dynamic_cast<const PluginFx*>(p)) return fx->saveState(out);
  if (auto* sy = dynamic_cast<const PluginSynth*>(p)) return sy->saveState(out);
  return false;
}
}  // namespace

sq::Ptr<IPluginInstance> AudioEngine::pluginInstance(const sq_guid& node, int32_t slot) {
  Graph* g = core_->newestGraph();
  if (!g) return {};
  const AudioProcessor* p = nullptr;
  if (sq_guid_is_null(&node)) {
    const int32_t c = chainOf(desc_.masterSlotToChain, slot);
    if (c >= 0) p = g->master().insert(static_cast<size_t>(c));
  } else if (const int32_t ti = g->findTrack(node); ti >= 0) {
    TrackProcessor& t = g->track(static_cast<size_t>(ti));
    if (slot < 0) p = t.instrument();
    else if (const int32_t c = chainOf(t.desc().slotToChain, slot); c >= 0) p = t.insert(static_cast<size_t>(c));
  } else if (const int32_t bi = g->findBus(node); bi >= 0 && static_cast<size_t>(bi) < desc_.buses.size()) {
    const int32_t c = chainOf(desc_.buses[static_cast<size_t>(bi)].slotToChain, slot);
    if (c >= 0) p = g->bus(static_cast<size_t>(bi)).insert(static_cast<size_t>(c));
  }
  IPluginInstance* inst = p ? p->pluginInstance() : nullptr;
  return inst ? sq::Ptr<IPluginInstance>(inst) : sq::Ptr<IPluginInstance>{};
}

std::vector<AudioEngine::PluginSlot> AudioEngine::pluginSlots() {
  std::vector<PluginSlot> out;
  Graph* g = core_->newestGraph();
  if (!g) return out;
  auto push = [&](const sq_guid& node, int32_t slot, const AudioProcessor* p) {
    IPluginInstance* inst = p ? p->pluginInstance() : nullptr;
    if (inst) out.push_back(PluginSlot{node, slot, sq::Ptr<IPluginInstance>(inst)});
  };
  for (size_t i = 0; i < g->trackCount(); ++i) {
    TrackProcessor& t = g->track(i);
    push(t.desc().id, -1, t.instrument());
    for (size_t c = 0; c < t.insertCount(); ++c) {
      const int32_t s = slotOf(t.desc().slotToChain, c);
      if (s >= 0) push(t.desc().id, s, t.insert(c));
    }
  }
  for (size_t b = 0; b < g->busCount() && b < desc_.buses.size(); ++b)
    for (size_t c = 0; c < g->bus(b).insertCount(); ++c) {
      const int32_t s = slotOf(desc_.buses[b].slotToChain, c);
      if (s >= 0) push(g->busId(b), s, g->bus(b).insert(c));
    }
  for (size_t c = 0; c < g->master().insertCount(); ++c) {
    const int32_t s = slotOf(desc_.masterSlotToChain, c);
    if (s >= 0) push(sq_guid{}, s, g->master().insert(c));
  }
  return out;
}

bool AudioEngine::locatePlugin(IPluginInstance* inst, sq_guid* node, int32_t* slot) {
  if (!inst) return false;
  for (const PluginSlot& s : pluginSlots())
    if (s.instance.get() == inst) {
      if (node) *node = s.node;
      if (slot) *slot = s.slot;
      return true;
    }
  return false;
}

void AudioEngine::notePluginValues(IPluginInstance* inst, const ParamList& values) {
  if (!inst || values.empty() || !haveDesc_) return;
  sq_guid node{};
  int32_t slot = 0;
  if (!locatePlugin(inst, &node, &slot)) return;
  // desc_ describes the newest graph (installed from it, or brought up to date by mix commands),
  // so its slot maps resolve the line to the inserts[] index the next applyMixChanges diffs.
  const auto sameId = [&](const auto& d) { return d.id == node; };
  std::pair<NodeKey, int32_t> key{};
  if (sq_guid_is_null(&node)) {
    const int32_t c = chainOf(desc_.masterSlotToChain, slot);
    if (c < 0) return;
    key = {NodeKey{NodeKind::Master, node}, c};
  } else if (auto t = std::find_if(desc_.tracks.begin(), desc_.tracks.end(), sameId); t != desc_.tracks.end()) {
    const int32_t c = slot < 0 ? -1 : chainOf(t->slotToChain, slot);
    if (slot >= 0 && c < 0) return;
    key = {NodeKey{NodeKind::Track, node}, c};
  } else if (auto b = std::find_if(desc_.buses.begin(), desc_.buses.end(), sameId);
             b != desc_.buses.end() && slot >= 0) {
    const int32_t c = chainOf(b->slotToChain, slot);
    if (c < 0) return;
    key = {NodeKey{NodeKind::Bus, node}, c};
  } else {
    return;
  }
  ParamList& held = heldValues_[key];
  for (const auto& [id, v] : values) {
    bool known = false;
    for (auto& [hid, hv] : held)
      if (hid == id) {
        hv = v;
        known = true;
      }
    if (!known) held.emplace_back(id, v);
  }
}

namespace {
// Processor (chain) position of every inserts[] entry, -1 for a null prototype (TrackProcessor /
// SubMixProcessor skip those).
std::vector<int32_t> chainPositions(const std::vector<ProcessorProto>& inserts) {
  std::vector<int32_t> pos(inserts.size(), -1);
  int32_t n = 0;
  for (size_t c = 0; c < inserts.size(); ++c)
    if (inserts[c]) pos[c] = n++;
  return pos;
}
}  // namespace

std::map<AudioEngine::NodeKey, AudioEngine::NodeIdent> AudioEngine::identify(const GraphDesc& d) {
  auto lines = [](const std::vector<ProcessorProto>& inserts, const std::vector<int32_t>& slotToChain,
                  const std::vector<std::string>& keys) {
    std::vector<LineIdent> out;
    const std::vector<int32_t> pos = chainPositions(inserts);
    std::vector<bool> listed(inserts.size(), false);
    for (size_t s = 0; s < keys.size(); ++s) {  // a document desc: every line, bypassed ones too
      LineIdent l;
      l.key = keys[s];
      const int32_t c = s < slotToChain.size() ? slotToChain[s] : -1;
      const size_t ci = static_cast<size_t>(c);
      if (c >= 0 && ci < inserts.size() && pos[ci] >= 0 && !listed[ci]) {
        l.entry = pos[ci];
        listed[ci] = true;
      }
      out.push_back(std::move(l));
    }
    for (size_t c = 0; c < inserts.size(); ++c)  // no keys (or an entry no line names): keyless, in order
      if (pos[c] >= 0 && !listed[c]) {
        LineIdent l;
        l.entry = pos[c];
        out.push_back(std::move(l));
      }
    return out;
  };
  std::map<NodeKey, NodeIdent> out;
  for (const TrackDesc& t : d.tracks) {
    NodeIdent& n = out[NodeKey{NodeKind::Track, t.id}];
    n.lines = lines(t.inserts, t.slotToChain, t.lineKeys);
    n.hasInstrument = t.instrument != nullptr;
    if (n.hasInstrument) n.instrument = t.instrumentKey;
  }
  for (const BusDesc& b : d.buses)
    out[NodeKey{NodeKind::Bus, b.id}].lines = lines(b.inserts, b.slotToChain, b.lineKeys);
  out[NodeKey{NodeKind::Master, sq_guid{}}].lines = lines(d.masterInserts, d.masterSlotToChain, d.masterLineKeys);
  return out;
}

std::vector<int32_t> AudioEngine::alignLines(const std::vector<LineIdent>& olds, const std::vector<LineIdent>& news) {
  const size_t no = olds.size(), nn = news.size();
  std::vector<int32_t> partner(nn, -1);
  std::vector<bool> taken(no, false);
  auto pair = [&](size_t j, size_t i) {
    partner[j] = static_cast<int32_t>(i);
    taken[i] = true;
  };
  size_t head = 0;
  while (head < no && head < nn && olds[head].key == news[head].key) {
    pair(head, head);
    ++head;
  }
  size_t tail = 0;
  while (tail < no - head && tail < nn - head && olds[no - 1 - tail].key == news[nn - 1 - tail].key) {
    pair(nn - 1 - tail, no - 1 - tail);
    ++tail;
  }
  // In between (moved, added, removed, replaced or several edited lines): the identical identity
  // first, then the same structure; the first unpaired old line each time, so duplicates keep
  // their order.
  std::vector<std::string> oldStructure(no), newStructure(nn);
  for (size_t i = head; i < no - tail; ++i) oldStructure[i] = specStructure(olds[i].key);
  for (size_t j = head; j < nn - tail; ++j) newStructure[j] = specStructure(news[j].key);
  for (int pass = 0; pass < 2; ++pass)
    for (size_t j = head; j < nn - tail; ++j) {
      if (partner[j] >= 0) continue;
      for (size_t i = head; i < no - tail; ++i)
        if (!taken[i] && (pass == 0 ? olds[i].key == news[j].key : oldStructure[i] == newStructure[j])) {
          pair(j, i);
          break;
        }
    }
  return partner;
}

AudioEngine::ParkedState* AudioEngine::findParked(uint64_t id) {
  if (id == 0) return nullptr;
  for (ParkedState& p : parked_)
    if (p.id == id) return &p;
  return nullptr;
}

std::vector<AudioEngine::KeptPluginState> AudioEngine::keptPluginStates() const {
  // liveIdent_ names the lines of the installed graph (brought up to date by mix-only edits), one
  // per "user.inserts" line in order, with the attached kept state of each line that hosts nothing.
  std::vector<KeptPluginState> out;
  for (const auto& [node, ident] : liveIdent_)
    for (size_t s = 0; s < ident.lines.size(); ++s) {
      const LineIdent& l = ident.lines[s];
      if (l.entry >= 0 || l.parked == 0) continue;
      for (const ParkedState& p : parked_)
        if (p.id == l.parked && p.attached) {
          out.push_back(KeptPluginState{node.second, static_cast<int32_t>(s), p.state});
          break;
        }
    }
  return out;
}

void AudioEngine::captureStatesInto(GraphDesc& d, ParkPlan* plan) {
  std::map<NodeKey, NodeIdent> next = identify(d);
  // The newest graph's processors per node, with the lines they stand for (liveIdent_; a graph
  // compiled without one gets a keyless line per processor). Instances are captured lazily.
  struct OldNode {
    std::vector<const AudioProcessor*> procs;  // chain position -> processor
    const AudioProcessor* instrument = nullptr;
    NodeIdent ident;
    std::vector<bool> used;                    // per line: paired with a new line
    bool instrumentUsed = false;
  };
  std::map<NodeKey, OldNode> old;
  auto collect = [&](const NodeKey& key, const auto& proc, const AudioProcessor* instrument) {
    OldNode& n = old[key];
    for (size_t c = 0; c < proc.insertCount(); ++c) n.procs.push_back(proc.insert(c));
    n.instrument = instrument;
    const auto it = liveIdent_.find(key);
    if (it != liveIdent_.end()) {
      n.ident = it->second;
    } else {
      for (size_t c = 0; c < n.procs.size(); ++c) {
        LineIdent l;
        l.entry = static_cast<int32_t>(c);
        n.ident.lines.push_back(std::move(l));
      }
      n.ident.hasInstrument = instrument != nullptr;
    }
    n.used.assign(n.ident.lines.size(), false);
  };
  if (Graph* g = core_->newestGraph()) {
    for (size_t i = 0; i < g->trackCount(); ++i) {
      const TrackProcessor& t = g->track(i);
      collect(NodeKey{NodeKind::Track, t.desc().id}, t, t.instrument());
    }
    for (size_t b = 0; b < g->busCount(); ++b) collect(NodeKey{NodeKind::Bus, g->busId(b)}, g->bus(b), nullptr);
    collect(NodeKey{NodeKind::Master, sq_guid{}}, g->master(), nullptr);
  }
  auto procOf = [](const OldNode& o, const LineIdent& l) -> const AudioProcessor* {
    const size_t c = static_cast<size_t>(l.entry);
    return l.entry >= 0 && c < o.procs.size() ? o.procs[c] : nullptr;
  };

  std::vector<uint64_t> handed;  // kept states this pass handed out (consumed / re-attached)
  auto isHanded = [&](uint64_t id) { return std::find(handed.begin(), handed.end(), id) != handed.end(); };
  // A kept state for a new chain entry: read (render) or consumed (recompile).
  auto takeKept = [&](uint64_t id, PluginState* out) {
    const ParkedState* p = findParked(id);
    if (!p || isHanded(id)) return false;
    *out = p->state;
    handed.push_back(id);
    if (plan) plan->consumed.push_back(id);
    return true;
  };
  // Recompile during an undo / redo only: the newest detached state of the node with this identity
  // that left from this line index (an undo puts a line back exactly where it was) by the opposite
  // move: an undo takes what a commit / redo detached, a redo what an undo detached. The history is
  // a stack, so that is the one of the step being reversed. (Without the direction, "remove B, add
  // B again, undo, undo" would hand B the re-added copy's state: the first undo detaches that copy
  // under the same identity and index, newer than B's own.)
  const bool byUndo = applying_ == DocChange::Undo;
  auto detached = [&](const NodeKey& node, bool instrument, const std::string& key, int32_t at) -> uint64_t {
    if (!plan || (applying_ != DocChange::Undo && applying_ != DocChange::Redo) || key.empty()) return 0;
    uint64_t best = 0;
    for (const ParkedState& p : parked_)
      if (!p.attached && p.byUndo != byUndo && p.instrument == instrument && p.node == node && p.line == key &&
          p.at == at && p.id > best && !isHanded(p.id))
        best = p.id;
    return best;
  };
  // Recompile only: keeps the live state of an old instance (keyless lines: nothing could ask).
  auto keep = [&](const NodeKey& node, bool instrument, bool attached, const std::string& key, int32_t at,
                  const AudioProcessor* proc) -> uint64_t {
    PluginState st;
    if (!plan || key.empty() || !captureInto(proc, &st)) return 0;
    ParkedState p;
    p.id = nextParkId_++;
    p.node = node;
    p.instrument = instrument;
    p.attached = attached;
    p.byUndo = !attached && byUndo;
    p.line = key;
    p.at = at;
    p.state = std::move(st);
    plan->parks.push_back(std::move(p));
    return plan->parks.back().id;
  };

  auto fillChain = [&](const NodeKey& node, const std::vector<ProcessorProto>& protos,
                       const std::vector<ParamList>& params, std::vector<PluginState>& states) {
    states.assign(protos.size(), PluginState{});
    NodeIdent& now = next[node];
    const auto on = old.find(node);
    OldNode* o = on == old.end() ? nullptr : &on->second;
    static const std::vector<LineIdent> kNoLines;
    const std::vector<int32_t> partner = alignLines(o ? o->ident.lines : kNoLines, now.lines);
    std::vector<size_t> index;  // chain position -> inserts[] index
    for (size_t c = 0; c < protos.size(); ++c)
      if (protos[c]) index.push_back(c);
    for (size_t j = 0; j < now.lines.size(); ++j) {
      LineIdent& line = now.lines[j];
      PluginState st;
      bool have = false;
      if (o && partner[j] >= 0) {
        const size_t i = static_cast<size_t>(partner[j]);
        o->used[i] = true;
        const LineIdent& was = o->ident.lines[i];
        const AudioProcessor* proc = procOf(*o, was);
        if (line.entry >= 0) {
          if (proc) have = captureInto(proc, &st);                 // the same insert, recompiled
          else if (was.parked) have = takeKept(was.parked, &st);   // enabled again
        } else if (plan) {
          if (proc) {
            line.parked = keep(node, false, true, line.key, -1, proc);  // bypassed now: kept with its line
          } else if (findParked(was.parked)) {
            line.parked = was.parked;  // still bypassed: the kept state stays with the line
            plan->moves.push_back(ParkMove{was.parked, true, false, line.key, -1});
          }
        }
      } else if (const uint64_t id = detached(node, false, line.key, static_cast<int32_t>(j))) {
        if (line.entry >= 0) {
          have = takeKept(id, &st);
        } else {
          line.parked = id;  // a bypassed line put back: its state waits with it again
          handed.push_back(id);
          plan->moves.push_back(ParkMove{id, true, false, line.key, -1});
        }
      }
      if (!have || line.entry < 0 || static_cast<size_t>(line.entry) >= index.size()) continue;
      const size_t c = index[static_cast<size_t>(line.entry)];
      st.params = c < params.size() ? params[c] : ParamList{};  // the document's values win
      states[c] = std::move(st);
    }
  };
  for (TrackDesc& t : d.tracks) {
    const NodeKey node{NodeKind::Track, t.id};
    fillChain(node, t.inserts, t.insertParams, t.insertStates);
    t.instrumentState = PluginState{};
    if (!t.instrument) continue;
    const auto on = old.find(node);
    OldNode* o = on == old.end() ? nullptr : &on->second;
    PluginState st;
    bool have = false;
    if (o && o->instrument && specStructure(o->ident.instrument) == specStructure(t.instrumentKey)) {
      o->instrumentUsed = true;
      have = captureInto(o->instrument, &st);
    } else if (const uint64_t id = detached(node, true, t.instrumentKey, -1)) {
      have = takeKept(id, &st);
    }
    if (!have) continue;
    st.params = t.instrumentParams;
    t.instrumentState = std::move(st);
  }
  for (BusDesc& b : d.buses) fillChain(NodeKey{NodeKind::Bus, b.id}, b.inserts, b.insertParams, b.insertStates);
  fillChain(NodeKey{NodeKind::Master, sq_guid{}}, d.masterInserts, d.masterInsertParams, d.masterInsertStates);

  if (!plan) return;  // offline render: nothing consumed, nothing kept
  // Old lines without a partner: removed (or their node deleted). Their live states / the states
  // kept for them wait detached for an undo / redo; so does a replaced instrument's.
  for (auto& [node, o] : old) {
    for (size_t i = 0; i < o.ident.lines.size(); ++i) {
      if (o.used[i]) continue;
      const LineIdent& was = o.ident.lines[i];
      const int32_t at = static_cast<int32_t>(i);
      if (const AudioProcessor* proc = procOf(o, was))
        keep(node, false, false, was.key, at, proc);
      else if (findParked(was.parked))
        plan->moves.push_back(ParkMove{was.parked, false, byUndo, was.key, at});
    }
    if (o.instrument && !o.instrumentUsed) keep(node, true, false, o.ident.instrument, -1, o.instrument);
  }
  plan->ident = std::move(next);
}

void AudioEngine::commitParkPlan(ParkPlan&& plan) {
  liveIdent_ = std::move(plan.ident);
  for (const uint64_t id : plan.consumed) {
    auto gone = std::remove_if(parked_.begin(), parked_.end(), [id](const ParkedState& p) { return p.id == id; });
    parked_.erase(gone, parked_.end());
  }
  for (const ParkMove& m : plan.moves)
    if (ParkedState* p = findParked(m.id)) {
      p->attached = m.attached;
      p->byUndo = m.byUndo;
      p->line = m.line;
      p->at = m.at;
    }
  for (ParkedState& p : plan.parks) parked_.push_back(std::move(p));
  // At most kMaxParked entries and kMaxParkedBytes of state (a few sampler states can be large).
  // Detached states go first, oldest first: each waits only for an undo / redo that may never
  // come, while an attached one is needed whenever its line (still in the document) is enabled
  // again; attached states go, oldest first, only when no detached one is left. A line whose kept
  // state went simply has none any more.
  size_t count = parked_.size(), bytes = 0;
  for (const ParkedState& p : parked_) bytes += p.state.bytes.size();
  std::vector<bool> drop(parked_.size(), false);
  for (const bool attached : {false, true})
    for (size_t k = 0; k < parked_.size() && (count > kMaxParked || bytes > kMaxParkedBytes); ++k)
      if (parked_[k].attached == attached) {
        drop[k] = true;
        --count;
        bytes -= parked_[k].state.bytes.size();
      }
  if (count == parked_.size()) return;
  size_t kept = 0;
  for (size_t k = 0; k < parked_.size(); ++k)
    if (!drop[k]) {
      if (kept != k) parked_[kept] = std::move(parked_[k]);
      ++kept;
    }
  parked_.erase(parked_.begin() + static_cast<std::ptrdiff_t>(kept), parked_.end());
}

void AudioEngine::refreshIdentity(const GraphDesc& d) {
  std::map<NodeKey, NodeIdent> next = identify(d);
  for (auto& [node, now] : next) {
    const auto it = liveIdent_.find(node);
    if (it == liveIdent_.end()) continue;
    const std::vector<LineIdent>& was = it->second.lines;
    for (size_t i = 0; i < now.lines.size() && i < was.size(); ++i) {
      if (now.lines[i].entry >= 0) continue;
      ParkedState* p = findParked(was[i].parked);
      if (!p) continue;
      now.lines[i].parked = p->id;
      p->line = now.lines[i].key;  // a value edit of the bypassed line: its kept state follows it
    }
  }
  liveIdent_ = std::move(next);
}

bool AudioEngine::insertsReordered(const GraphDesc& next) const {
  for (const auto& [node, now] : identify(next)) {
    const auto it = liveIdent_.find(node);
    if (it == liveIdent_.end()) continue;
    std::vector<std::string> a, b;
    for (const LineIdent& l : it->second.lines) a.push_back(l.key);
    for (const LineIdent& l : now.lines) b.push_back(l.key);
    if (a == b) continue;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a == b) return true;
  }
  return false;
}

sq_result AudioEngine::rebuildFromDocument(bool allowMixOnly) {
  // notePluginValues holds for this update only (applyMixChanges reads it; a recompile makes new
  // instances, which take their values from the spec).
  struct ForgetHeld {
    AudioEngine* e;
    ~ForgetHeld() { e->forgetPluginValues(); }
  } forgetHeld{this};
  if (doc_) {
    GraphDesc d;
    DocBuildResult info;
    const sq_result r = buildGraphDesc(doc_.get(), reg_, media_, device_ ? sampleRate_ : 0.0, &d, &info, &inserts_, &instruments_);
    if (SQ_FAILED(r)) return r;
    // Same structure as the installed graph: apply the edit as mixer commands (no recompile, no
    // new plugin instances, no reset of delay lines / fader ramps / plugin state). Inserts moved
    // among equal-structure lines are recompiled instead (their states move with their lines).
    if (allowMixOnly && haveDesc_ && !structureKey_.empty() && info.structureKey == structureKey_ &&
        !insertsReordered(d) && applyMixChanges(d)) {
      buildWarnings_ = std::move(info.warnings);
      for (size_t i = 0; i < d.tracks.size() && i < desc_.tracks.size(); ++i) d.tracks[i].liveMidi = desc_.tracks[i].liveMidi;
      desc_ = std::move(d);
      refreshIdentity(desc_);  // same lines, new values / state text
      updateLiveTargets(true);  // arm state may have changed
      return SQ_OK;
    }
    buildWarnings_ = std::move(info.warnings);
    desc_ = std::move(d);
    haveDesc_ = true;
    const sq_result c = compileAndInstall();
    structureKey_ = SQ_SUCCEEDED(c) ? std::move(info.structureKey) : std::string();
    return c;
  }
  structureKey_.clear();
  return compileAndInstall();
}

namespace {
bool sameValue(float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); }
bool sameLanes(const std::vector<AutoLane>& a, const std::vector<AutoLane>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const AutoLane& x = a[i];
    const AutoLane& y = b[i];
    if (x.target != y.target || x.slot != y.slot || x.param != y.param || !sameValue(x.staticValue, y.staticValue) ||
        x.lane.target != y.lane.target || x.lane.points != y.lane.points)
      return false;
  }
  return true;
}
bool sameAutomation(const AutomationData* a, const AutomationData* b) {
  static const AutomationData kEmpty;
  if (!a) a = &kEmpty;
  if (!b) b = &kEmpty;
  auto strips = [](const std::vector<std::vector<AutoLane>>& x, const std::vector<std::vector<AutoLane>>& y) {
    const size_t n = std::max(x.size(), y.size());
    static const std::vector<AutoLane> none;
    for (size_t i = 0; i < n; ++i)
      if (!sameLanes(i < x.size() ? x[i] : none, i < y.size() ? y[i] : none)) return false;
    return true;
  };
  return strips(a->tracks, b->tracks) && strips(a->buses, b->buses);
}
// Parameter differences of one processor (old -> next spec values). false = not expressible
// (a value went back to an unknown default).
bool paramDiff(const ParamList& oldP, const ParamList& newP, const ProcessorProto& nextProto, std::vector<std::pair<uint32_t, double>>* out) {
  for (const auto& [id, v] : newP) {
    bool same = false;
    for (const auto& [oid, ov] : oldP)
      if (oid == id) same = ov == v;
    if (!same) out->emplace_back(id, v);
  }
  for (const auto& [oid, ov] : oldP) {
    bool present = false;
    for (const auto& [id, v] : newP) present = present || id == oid;
    if (present) continue;
    if (!nextProto || oid >= nextProto->parameterCount()) return false;
    out->emplace_back(oid, static_cast<double>(nextProto->parameter(oid)));
  }
  return true;
}
}  // namespace

// Posts the differences between desc_ (what the installed graph was built from, plus the commands
// already applied) and `next` (same structure key): mixer values, insert/instrument parameters,
// send levels, live-MIDI routing and automation. false = not expressible as commands (or the
// command queue is full): the caller recompiles instead.
bool AudioEngine::applyMixChanges(const GraphDesc& next) {
  if (next.tracks.size() != desc_.tracks.size() || next.buses.size() != desc_.buses.size()) return false;
  if (next.masterInserts.size() != desc_.masterInserts.size()) return false;
  std::vector<Command> cmds;
  const uint64_t gen = core_->currentGeneration();
  auto add = [&](CmdType type, int64_t index, double value, int64_t a = 0, int64_t b = 0) {
    Command c;
    c.type = type;
    c.index = static_cast<int32_t>(index);
    c.generation = gen;
    c.value = value;
    c.a = a;
    c.b = b;
    cmds.push_back(c);
  };
  // A changed value the main-chain instance already holds (notePluginValues: the plugin reported
  // it) is not sent back to it: a track insert's command goes to its fast-lane copy only, any
  // other is dropped.
  auto held = [&](const NodeKey& node, int64_t k, uint32_t id, double v) {
    auto it = heldValues_.find({node, static_cast<int32_t>(k)});
    if (it == heldValues_.end()) return false;
    for (const auto& [hid, hv] : it->second)
      if (hid == id) return hv == v;
    return false;
  };
  auto params = [&](CmdType type, int64_t index, const NodeKey& node, const std::vector<ParamList>& oldL,
                    const std::vector<ParamList>& newL, const std::vector<ProcessorProto>& protos) {
    if (oldL.size() != newL.size()) return false;
    for (size_t k = 0; k < newL.size(); ++k) {
      std::vector<std::pair<uint32_t, double>> diff;
      if (!paramDiff(oldL[k], newL[k], k < protos.size() ? protos[k] : nullptr, &diff)) return false;
      for (const auto& [id, v] : diff) {
        if (held(node, static_cast<int64_t>(k), id, v)) {
          if (type != CmdType::TrackParam) continue;
          add(type, index, v, static_cast<int64_t>(k), id);
          cmds.back().fastLaneOnly = true;
          continue;
        }
        add(type, index, v, static_cast<int64_t>(k), id);
      }
    }
    return true;
  };
  for (size_t i = 0; i < next.tracks.size(); ++i) {
    const TrackDesc& o = desc_.tracks[i];
    const TrackDesc& n = next.tracks[i];
    if (!(o.id == n.id) || o.sends.size() != n.sends.size()) return false;
    if (o.volume != n.volume) add(CmdType::TrackVolume, static_cast<int64_t>(i), n.volume);
    if (o.pan != n.pan) add(CmdType::TrackPan, static_cast<int64_t>(i), n.pan);
    if (o.mute != n.mute) add(CmdType::TrackMute, static_cast<int64_t>(i), n.mute ? 1.0 : 0.0);
    if (o.solo != n.solo) add(CmdType::TrackSolo, static_cast<int64_t>(i), n.solo ? 1.0 : 0.0);
    if (monitorOverride_.find(n.id) == monitorOverride_.end() && o.monitor != n.monitor)
      add(CmdType::TrackMonitor, static_cast<int64_t>(i), n.monitor ? 1.0 : 0.0);
    const NodeKey trackNode{NodeKind::Track, n.id};
    if (!params(CmdType::TrackParam, static_cast<int64_t>(i), trackNode, o.insertParams, n.insertParams, n.inserts))
      return false;
    if (n.instrument) {
      std::vector<std::pair<uint32_t, double>> diff;
      if (!paramDiff(o.instrumentParams, n.instrumentParams, n.instrument, &diff)) return false;
      for (const auto& [id, v] : diff)  // an instrument has no fast-lane copy
        if (!held(trackNode, -1, id, v)) add(CmdType::TrackParam, static_cast<int64_t>(i), v, -1, id);
    }
    for (size_t k = 0; k < n.sends.size(); ++k)
      if (o.sends[k].level != n.sends[k].level) add(CmdType::TrackSendLevel, static_cast<int64_t>(i), n.sends[k].level, static_cast<int64_t>(k));
  }
  for (size_t b = 0; b < next.buses.size(); ++b) {
    const BusDesc& o = desc_.buses[b];
    const BusDesc& n = next.buses[b];
    if (!(o.id == n.id) || o.sends.size() != n.sends.size()) return false;
    if (o.volume != n.volume) add(CmdType::BusVolume, static_cast<int64_t>(b), n.volume);
    if (o.mute != n.mute) add(CmdType::BusMute, static_cast<int64_t>(b), n.mute ? 1.0 : 0.0);
    if (o.pan != n.pan) add(CmdType::BusPan, static_cast<int64_t>(b), n.pan);
    if (o.solo != n.solo) add(CmdType::BusSolo, static_cast<int64_t>(b), n.solo ? 1.0 : 0.0);
    const NodeKey busNode{NodeKind::Bus, n.id};
    if (!params(CmdType::BusParam, static_cast<int64_t>(b), busNode, o.insertParams, n.insertParams, n.inserts))
      return false;
    for (size_t k = 0; k < n.sends.size(); ++k)
      if (o.sends[k].level != n.sends[k].level) add(CmdType::BusSendLevel, static_cast<int64_t>(b), n.sends[k].level, static_cast<int64_t>(k));
  }
  if (!params(CmdType::MasterParam, -1, NodeKey{NodeKind::Master, sq_guid{}}, desc_.masterInsertParams,
              next.masterInsertParams, next.masterInserts))
    return false;
  if (desc_.masterVolume != next.masterVolume) {
    Command c;
    c.type = CmdType::MasterVolume;
    c.value = next.masterVolume;
    cmds.push_back(c);
  }
  if (desc_.masterPan != next.masterPan) {
    Command c;
    c.type = CmdType::MasterPan;
    c.value = next.masterPan;
    cmds.push_back(c);
  }
  if (desc_.masterMute != next.masterMute) {
    Command c;
    c.type = CmdType::MasterMute;
    c.value = next.masterMute ? 1.0 : 0.0;
    cmds.push_back(c);
  }
  const bool autoChanged = !sameAutomation(desc_.automation.get(), next.automation.get());
  if (cmds.size() + (autoChanged ? 1 : 0) + next.tracks.size() >= 900) return false;  // a burst: recompile instead
  for (const Command& c : cmds)
    if (!core_->post(c)) return false;  // queue full: a recompile installs the new values anyway
  if (autoChanged) {
    core_->collectGarbage();  // keeps the automation retire queue drained (it never overflows)
    auto* data = next.automation ? new AutomationData(*next.automation) : new AutomationData();
    // The swap restores every automated parameter to its lane's static value (the spec value): one
    // whose main-chain instance already holds that value (notePluginValues, as `held` above) is
    // restored on the fast-lane copy only, by neither the old lanes' restore nor the new lanes'.
    auto markHeld = [&](std::vector<std::vector<AutoLane>>& strips, size_t i, const NodeKey& node) {
      if (i >= strips.size()) return;
      for (AutoLane& l : strips[i]) {
        if ((l.target != AutoTarget::Insert && l.target != AutoTarget::Instrument) || std::isnan(l.staticValue)) continue;
        const auto it = heldValues_.find({node, l.target == AutoTarget::Instrument ? -1 : l.slot});
        if (it == heldValues_.end()) continue;
        for (const auto& [hid, hv] : it->second)
          if (hid == l.param) l.mainHolds = static_cast<float>(hv) == l.staticValue;
      }
    };
    if (!heldValues_.empty()) {
      for (size_t i = 0; i < next.tracks.size(); ++i) markHeld(data->tracks, i, NodeKey{NodeKind::Track, next.tracks[i].id});
      for (size_t b = 0; b < next.buses.size(); ++b) markHeld(data->buses, b, NodeKey{NodeKind::Bus, next.buses[b].id});
    }
    Command c;
    c.type = CmdType::SetAutomation;
    c.generation = gen;
    c.ptr = data;
    if (!core_->post(c)) {
      delete data;
      return false;
    }
  }
  return true;
}

bool AudioEngine::wantsLiveMidi(const TrackDesc& t) const {
  if (!t.instrument) return false;
  bool anyArmed = false;
  for (const TrackDesc& x : desc_.tracks) {
    if (!x.instrument) continue;
    bool armed = x.armed;
    auto a = armOverride_.find(x.id);
    if (a != armOverride_.end()) armed = a->second;
    anyArmed = anyArmed || armed;
  }
  if (anyArmed) {
    bool armed = t.armed;
    auto a = armOverride_.find(t.id);
    if (a != armOverride_.end()) armed = a->second;
    return armed;
  }
  return !sq_guid_is_null(&liveTarget_) && t.id == liveTarget_;
}

void AudioEngine::updateLiveTargets(bool post) {
  const uint64_t gen = core_->currentGeneration();
  for (size_t i = 0; i < desc_.tracks.size(); ++i) {
    const bool want = wantsLiveMidi(desc_.tracks[i]);
    if (want == desc_.tracks[i].liveMidi) continue;
    desc_.tracks[i].liveMidi = want;
    if (!post) continue;
    Command c;
    c.type = CmdType::TrackLiveMidi;
    c.index = static_cast<int32_t>(i);
    c.generation = gen;
    c.value = want ? 1.0 : 0.0;
    core_->post(c);
  }
}

void AudioEngine::setLiveMidiTarget(const sq_guid& trackId) {
  liveTarget_ = trackId;
  updateLiveTargets(true);
}

std::vector<sq_guid> AudioEngine::liveMidiTracks() const {
  std::vector<sq_guid> out;
  for (const TrackDesc& t : desc_.tracks)
    if (t.liveMidi) out.push_back(t.id);
  return out;
}

sq_result AudioEngine::setGraphDesc(GraphDesc desc) {
  detachDocument();
  structureKey_.clear();
  desc_ = std::move(desc);
  haveDesc_ = true;
  return compileAndInstall();
}

void AudioEngine::onDocumentChanged(DocChange change) {
  // A commit clears the document's redo history: the states an undo detached (each waits for the
  // redo of the step it undid) can never be asked for again.
  if (change == DocChange::Commit) {
    auto gone = std::remove_if(parked_.begin(), parked_.end(), [](const ParkedState& p) { return !p.attached && p.byUndo; });
    parked_.erase(gone, parked_.end());
  }
  applying_ = change;
  rebuildFromDocument(true);
  applying_ = DocChange::Other;
}

void AudioEngine::notify(const char* topic) {
  std::vector<sq::Ptr<IObserver>> obs = observers_;
  for (auto& o : obs) o->onNotify(static_cast<IAudioEngine*>(this), topic, nullptr);
}

// ---- device ----
sq_result AudioEngine::openDevice(const sq_guid* driverClassId, const char* deviceId, const sq_audio_device_config* request) {
  if (!driverClassId || !reg_) return SQ_E_INVALIDARG;
  sq::Ptr<IAudioDriver> drv;
  sq_result r = reg_->createInstance(driverClassId, &IAudioDriver::iid, drv.putVoid());
  if (SQ_FAILED(r)) return r;
  std::string id = deviceId ? deviceId : "";
  if (id.empty()) {
    // default device: first with the default-output flag, else the first one
    drv->refreshDevices();
    const uint32_t n = drv->getDeviceCount();
    for (uint32_t i = 0; i < n; ++i) {
      sq_audio_device_info di{};
      di.struct_size = sizeof di;
      if (SQ_FAILED(drv->getDeviceInfo(i, &di))) continue;
      if (id.empty() || (di.flags & SQ_AUDIO_DEVICE_DEFAULT_OUTPUT)) id = di.id;
      if (di.flags & SQ_AUDIO_DEVICE_DEFAULT_OUTPUT) break;
    }
  }
  sq::Ptr<IAudioDevice> dev;
  r = drv->openDevice(id.c_str(), dev.put());
  if (SQ_FAILED(r)) return r;
  r = openDeviceObject(dev.get(), request);
  if (SQ_SUCCEEDED(r)) {
    driver_ = drv;
    deviceReset_->store(0);
    deviceListener_ = sq::Ptr<IAudioDeviceListener>::adopt(new DeviceListener(deviceReset_, deviceListChanged_));
    if (SQ_FAILED(driver_->addListener(deviceListener_.get()))) deviceListener_ = nullptr;  // idle() still polls isRunning()
    driverClass_ = *driverClassId;
    deviceId_ = id;
    std::memset(deviceIdBuf_, 0, sizeof deviceIdBuf_);
    std::memcpy(deviceIdBuf_, id.c_str(), std::min(id.size(), sizeof deviceIdBuf_ - 1));
  }
  return r;
}

sq_result AudioEngine::openDeviceObject(IAudioDevice* device, const sq_audio_device_config* request) {
  if (!device) return SQ_E_INVALIDARG;
  if (request) {
    // Keep an owned copy (channel lists included) so idle() can reopen after a device reset.
    const sq_audio_device_config in = *request;
    std::vector<uint32_t> ins, outs;
    if (in.input_channels) ins.assign(in.input_channels, in.input_channels + in.input_channel_count);
    if (in.output_channels) outs.assign(in.output_channels, in.output_channels + in.output_channel_count);
    request_ = in;
    requestIns_ = std::move(ins);
    requestOuts_ = std::move(outs);
    request_.input_channels = requestIns_.empty() ? nullptr : requestIns_.data();
    request_.input_channel_count = static_cast<uint32_t>(requestIns_.size());
    request_.output_channels = requestOuts_.empty() ? nullptr : requestOuts_.data();
    request_.output_channel_count = static_cast<uint32_t>(requestOuts_.size());
    haveRequest_ = true;
    request = &request_;
  } else {
    haveRequest_ = false;
  }
  closeDevice();
  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  device->getInfo(&info);
  sq_audio_device_config req{};
  std::vector<uint32_t> ins, outs;
  if (request) {
    req = *request;
  } else {
    req.struct_size = sizeof req;
    req.sample_rate = settings_ ? settings_->getDouble("Devices.SampleRate", 0.0) : 0.0;
    int64_t block = settings_ ? settings_->getInt("Devices.BlockSize", 0) : 0;
    if (block <= 0 && settings_) block = settings_->getInt("Engine.BlockSize", 0);
    req.block_frames = block > 0 ? static_cast<uint32_t>(block) : 0;
    for (uint32_t c = 0; c < std::min<uint32_t>(info.max_input_channels, kMaxChannels); ++c) ins.push_back(c);
    for (uint32_t c = 0; c < std::min<uint32_t>(info.max_output_channels, 2); ++c) outs.push_back(c);
    req.input_channels = ins.empty() ? nullptr : ins.data();
    req.input_channel_count = static_cast<uint32_t>(ins.size());
    req.output_channels = outs.empty() ? nullptr : outs.data();
    req.output_channel_count = static_cast<uint32_t>(outs.size());
  }
  sq_audio_device_config actual{};
  actual.struct_size = sizeof actual;
  sq_result r = device->configure(&req, &actual);
  if (SQ_FAILED(r)) return r;
  sq_stream_format in{}, out{};
  in.struct_size = out.struct_size = sizeof in;
  r = device->getStreamFormat(&in, &out);
  if (SQ_FAILED(r)) return r;
  sq_latency_info lat{};
  lat.struct_size = sizeof lat;
  device->getLatency(&lat);
  sampleRate_ = out.sample_rate > 0 ? out.sample_rate : 48000.0;
  blockFrames_ = out.max_block_frames ? out.max_block_frames : 256;
  inChannels_ = in.channels;
  outChannels_ = out.channels ? out.channels : 2;
  devInLatency_ = lat.input_frames;
  devOutLatency_ = lat.output_frames;
  core_->setDeviceLatency(devInLatency_, devOutLatency_);
  device_ = sq::Ptr<IAudioDevice>(device);
  r = rebuild();
  if (SQ_FAILED(r)) {
    device_ = nullptr;
    return r;
  }
  r = device->start(core_.get());
  if (SQ_FAILED(r)) {
    device_ = nullptr;
    return r;
  }
  notify("engine.device");
  notify("engine.latency");
  return SQ_OK;
}

void AudioEngine::dropDriverListener() {
  if (driver_ && deviceListener_) driver_->removeListener(deviceListener_.get());
  deviceListener_ = nullptr;
  driver_ = nullptr;
  deviceReset_->store(0);
}

sq_result AudioEngine::closeDevice() {
  if (!device_) {
    if (recordingActive_) endRecording();
    dropDriverListener();
    return SQ_S_FALSE;
  }
  // Stop the device first: stop() waits for the RT thread and calls onStop(), so a recording
  // ended below never waits for callbacks that a dead device will not deliver.
  device_->stop();
  // A driver whose stream died may not call onStop() from stop(); the RT thread is gone either
  // way (stop() returned), so take the core off the running path ourselves.
  if (core_->running()) core_->onStop();
  if (recordingActive_) endRecording();
  dropDriverListener();
  device_ = nullptr;
  deviceId_.clear();
  std::memset(deviceIdBuf_, 0, sizeof deviceIdBuf_);
  driverClass_ = sq_guid{};
  core_->collectGarbage();
  notify("engine.device");
  return SQ_OK;
}

sq_result AudioEngine::getStatus(sq_engine_status* out) {
  if (!out) return SQ_E_INVALIDARG;
  const uint32_t ss = out->struct_size;
  std::memset(out, 0, sizeof *out);
  out->struct_size = ss;
  if (device_) out->flags |= SQ_ENGINE_DEVICE_OPEN;
  if (core_->running() && (!device_ || device_->isRunning())) out->flags |= SQ_ENGINE_RUNNING;
  if (lowLatency_) out->flags |= SQ_ENGINE_LOW_LATENCY;
  if (activeRenders_->load() > 0) out->flags |= SQ_ENGINE_OFFLINE;
  out->sample_rate = sampleRate_;
  out->block_frames = blockFrames_;
  out->cpu_load = core_->cpuLoad();
  out->xruns = core_->xruns();
  out->driver_class = driverClass_;
  std::memcpy(out->device_id, deviceIdBuf_, sizeof out->device_id);
  return SQ_OK;
}

sq_result AudioEngine::setDocument(IDocument* doc) {
  // Re-binding the document already bound (the shell re-binds the active song after File.Open of
  // an open file, a failed import, a background document closing) is no switch: the rebuild below
  // carries every live state and the kept ones stay. doc_ holds a reference, so another document
  // cannot have the same address.
  const bool same = doc != nullptr && doc_.get() == doc;
  detachDocument();
  if (!same) {
    // Another song (or none): the live plugin states belong to the previous one. No carry, no
    // capture at all (File.Close does not saveState every plugin), and the kept states go too.
    carryNext_ = false;
    parked_.clear();
  }
  if (!doc) {
    desc_ = GraphDesc{};
    haveDesc_ = false;
    return compileAndInstall();
  }
  doc_ = sq::Ptr<IDocument>(doc);
  sq::Ptr<ISubject> subj = doc_.as<ISubject>();
  if (subj) {
    docObserver_ = sq::Ptr<IObserver>::adopt(new DocObserver(this));
    subj->addObserver(docObserver_.get());
  }
  return rebuild();
}

// ---- monitoring / latency / meters ----
sq_result AudioEngine::setLowLatencyMonitoring(sq_bool enabled) {
  lowLatency_ = enabled != 0;
  Command c;
  c.type = CmdType::LowLatency;
  c.value = lowLatency_ ? 1.0 : 0.0;
  if (!core_->post(c)) return SQ_E_BUSY;
  notify("engine.latency");
  return SQ_OK;
}

sq_result AudioEngine::setTrackMonitoring(const sq_guid* trackId, sq_bool enabled) {
  if (!trackId) return SQ_E_INVALIDARG;
  monitorOverride_[*trackId] = enabled != 0;
  const int32_t i = trackIndex(*trackId);
  if (i < 0) return SQ_E_NOTFOUND;
  Command c;
  c.type = CmdType::TrackMonitor;
  c.index = i;
  c.generation = core_->currentGeneration();
  c.value = enabled ? 1.0 : 0.0;
  return core_->post(c) ? SQ_OK : SQ_E_BUSY;
}

sq_result AudioEngine::getLatency(sq_engine_latency* out) {
  if (!out) return SQ_E_INVALIDARG;
  out->struct_size = sizeof *out;
  out->device_input_frames = device_ ? devInLatency_ : 0;
  out->device_output_frames = device_ ? devOutLatency_ : 0;
  out->block_frames = blockFrames_;
  out->plugin_delay_frames = graphLatency_;
  out->main_roundtrip_frames = out->device_input_frames + out->device_output_frames + graphLatency_;
  out->fast_lane_roundtrip_frames = out->device_input_frames + out->device_output_frames;
  return SQ_OK;
}

sq_result AudioEngine::getNodeLatency(const sq_guid* nodeId, sq_latency_info* out) {
  if (!nodeId || !out) return SQ_E_INVALIDARG;
  auto it = nodeLatency_.find(*nodeId);
  if (it == nodeLatency_.end()) return SQ_E_NOTFOUND;
  out->struct_size = sizeof *out;
  out->input_frames = device_ ? devInLatency_ : 0;
  out->output_frames = device_ ? devOutLatency_ : 0;
  out->process_frames = it->second.process;
  return SQ_OK;
}

sq_result AudioEngine::getMeter(const sq_guid* trackId, sq_meter_value* out, uint32_t capacity, uint32_t* count) {
  if (!out && capacity) return SQ_E_INVALIDARG;
  // Wait-free: bounded scan of the append-only directory (no lock, no allocation).
  const sq_guid key = trackId ? *trackId : sq_guid{};
  const uint32_t entries = meterCount_.load(std::memory_order_acquire);
  int32_t slot = -1;
  uint32_t channels = 0;
  for (uint32_t i = 0; i < entries; ++i) {
    if (meterTable_[i].id == key) {
      slot = meterTable_[i].slot;
      channels = meterTable_[i].channels.load(std::memory_order_acquire);
      break;
    }
  }
  if (slot < 0) return SQ_E_NOTFOUND;
  const uint32_t n = std::min<uint32_t>({channels, capacity, MeterBank::kChannels});
  for (uint32_t c = 0; c < n; ++c) core_->meters().read(static_cast<uint32_t>(slot), c, &out[c].peak, &out[c].rms);
  if (count) *count = n;
  return SQ_OK;
}

// ---- C++ track controls ----
sq_result AudioEngine::setMix64(bool enabled) {
  mix64_ = enabled;
  return compileAndInstall();
}

static sq_result postTrack(EngineCore& core, CmdType type, int32_t index, double value) {
  if (index < 0) return SQ_E_NOTFOUND;
  Command c;
  c.type = type;
  c.index = index;
  c.generation = core.currentGeneration();
  c.value = value;
  return core.post(c) ? SQ_OK : SQ_E_BUSY;
}

sq_result AudioEngine::setTrackVolume(const sq_guid& id, float v) {
  const int32_t i = trackIndex(id);
  if (i >= 0) desc_.tracks[static_cast<size_t>(i)].volume = v;
  return postTrack(*core_, CmdType::TrackVolume, i, v);
}
sq_result AudioEngine::setTrackPan(const sq_guid& id, float p) {
  const int32_t i = trackIndex(id);
  if (i >= 0) desc_.tracks[static_cast<size_t>(i)].pan = p;
  return postTrack(*core_, CmdType::TrackPan, i, p);
}
sq_result AudioEngine::setTrackMute(const sq_guid& id, bool m) {
  const int32_t i = trackIndex(id);
  if (i >= 0) desc_.tracks[static_cast<size_t>(i)].mute = m;
  return postTrack(*core_, CmdType::TrackMute, i, m ? 1.0 : 0.0);
}
sq_result AudioEngine::setTrackSolo(const sq_guid& id, bool s) {
  const int32_t i = trackIndex(id);
  if (i >= 0) desc_.tracks[static_cast<size_t>(i)].solo = s;
  return postTrack(*core_, CmdType::TrackSolo, i, s ? 1.0 : 0.0);
}
sq_result AudioEngine::setTrackArmed(const sq_guid& id, bool armed) {
  armOverride_[id] = armed;
  const int32_t i = trackIndex(id);
  if (i < 0) return SQ_E_NOTFOUND;
  desc_.tracks[static_cast<size_t>(i)].armed = armed;
  updateLiveTargets(true);
  return SQ_OK;
}

int64_t AudioEngine::ppqToFrame(sq_ppq_time ppq) const noexcept {
  return static_cast<int64_t>(std::llround(tempo_.ppqToFrame(ppq)));
}
sq_ppq_time AudioEngine::frameToPpq(int64_t frame) const noexcept { return tempo_.frameToPpq(frame); }

std::string AudioEngine::mediaFolder() const {
  std::string dir;
  if (settings_) dir = sq::getString(settings_, "Media.Folder");
  if (dir.empty() && doc_) {
    char buf[1024];
    uint32_t need = 0;
    if (doc_->getPath(buf, sizeof buf, &need) == SQ_OK && buf[0]) dir = sq::joinPath(dirOf(buf), "Media");
  }
  if (dir.empty()) dir = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "SquizzyStudio"), "Media");
  sq_fs_create_dirs(dir.c_str());
  return dir;
}

// ---- transport ----
sq_result AudioEngine::play() {
  Command c;
  c.type = CmdType::Play;
  if (!core_->post(c)) return SQ_E_BUSY;  // command queue full (audio thread stalled)
  notify("transport.state");
  return SQ_OK;
}

sq_result AudioEngine::stop() {
  if (recordingActive_) endRecording();
  Command c;
  c.type = CmdType::Stop;
  if (!core_->post(c)) return SQ_E_BUSY;
  notify("transport.state");
  return SQ_OK;
}

sq_result AudioEngine::record(sq_bool enable) {
  sq_result r;
  if (enable) {
    if (recordingActive_) return SQ_S_FALSE;
    r = beginRecording();
  } else {
    if (!recordingActive_) return SQ_S_FALSE;
    r = endRecording();
  }
  notify("transport.state");
  return r;
}

sq_result AudioEngine::locate(sq_ppq_time position) {
  if (!std::isfinite(position)) return SQ_E_INVALIDARG;
  Command c;
  c.type = CmdType::Locate;
  c.a = ppqToFrame(position);
  if (!core_->post(c)) return SQ_E_BUSY;
  notify("transport.state");
  return SQ_OK;
}

sq_result AudioEngine::setLoop(sq_ppq_time start, sq_ppq_time end, sq_bool enabled) {
  if (!std::isfinite(start) || !std::isfinite(end)) return SQ_E_INVALIDARG;
  if (enabled && end <= start) return SQ_E_INVALIDARG;
  Command c;
  c.type = CmdType::Loop;
  c.a = ppqToFrame(start);
  c.b = ppqToFrame(end);
  c.value = enabled ? 1.0 : 0.0;
  if (enabled && c.b <= c.a) return SQ_E_INVALIDARG;  // shorter than one frame
  if (!core_->post(c)) return SQ_E_BUSY;
  notify("transport.state");
  // The requested loop (main thread; the transport snapshot lags until the audio thread ran):
  // the app saves it in the song (document properties) from this notification.
  auto args = sq::makeAttributes();
  args->setDouble("start", start);
  args->setDouble("end", end);
  args->setBool("enabled", enabled ? SQ_TRUE : SQ_FALSE);
  std::vector<sq::Ptr<IObserver>> obs = observers_;
  for (auto& o : obs) o->onNotify(static_cast<IAudioEngine*>(this), "transport.loop", args.get());
  return SQ_OK;
}

sq_result AudioEngine::setMetronome(sq_bool enabled) {
  Command c;
  c.type = CmdType::Metronome;
  c.value = enabled ? 1.0 : 0.0;
  if (!core_->post(c)) return SQ_E_BUSY;
  notify("transport.state");
  return SQ_OK;
}

sq_result AudioEngine::getTransportState(sq_transport_state* out) {
  if (!out) return SQ_E_INVALIDARG;
  const TransportSnapshot s = core_->transport();
  out->struct_size = sizeof *out;
  out->flags = s.flags;
  out->position = s.ppq;
  out->seconds = s.sampleRate > 0 ? static_cast<double>(s.frame) / s.sampleRate : 0.0;
  out->frames = s.frame;
  out->bpm = s.bpm;
  out->numerator = s.numerator;
  out->denominator = s.denominator;
  out->loop_start = s.loopStartPpq;
  out->loop_end = s.loopEndPpq;
  return SQ_OK;
}

// ---- recording ----
sq_result AudioEngine::beginRecording() {
  auto session = std::make_unique<RecordSession>();
  midiRecTracks_.clear();
  capturedMidi_.clear();
  const std::string dir = mediaFolder();
  std::string ext = settings_ ? sq::getString(settings_, "Media.RecordFormat", "wav") : "wav";
  for (auto& t : desc_.tracks) {
    bool armed = t.armed;
    auto a = armOverride_.find(t.id);
    if (a != armOverride_.end()) armed = a->second;
    if (!armed) continue;
    if (t.instrument) {  // instrument tracks record the live MIDI, not the device input
      midiRecTracks_.push_back(t.id);
      continue;
    }
    if (t.inputChannels.empty()) continue;
    const std::string path = uniquePath(dir, sanitize(t.name) + "_take", ext);
    sq::Ptr<IAudioFileWriter> w;
    sq_result r = openRenderWriter(reg_, path, ext, sampleRate_, t.channels, SQ_SAMPLE_F32, nullptr, &w);
    if (SQ_FAILED(r)) return r;
    const size_t ring = static_cast<size_t>(sampleRate_ * 4.0);
    session->addStream(std::make_unique<RecordStream>(t.id, t.channels, t.inputChannels, std::move(w), path, ring));
  }
  if (session->streamCount() == 0 && midiRecTracks_.empty()) return SQ_S_FALSE;
  if (session->streamCount() > 0) core_->startRecording(std::move(session));
  recordingActive_ = true;
  std::vector<CapturedMidi> drop;
  core_->drainCapturedMidi(drop);  // events of an earlier capture that arrived late
  Command m;
  m.type = CmdType::MidiCapture;
  m.value = midiRecTracks_.empty() ? 0.0 : 1.0;
  core_->post(m);
  Command c;
  c.type = CmdType::Record;
  c.value = 1.0;
  core_->post(c);
  return SQ_OK;
}

sq_result AudioEngine::endRecording() {
  if (!recordingActive_) return SQ_S_FALSE;
  recordingActive_ = false;
  midiRecEnd_ = core_->transport().frame;
  Command c;
  c.type = CmdType::Record;
  c.value = 0.0;
  core_->post(c);
  std::vector<RecordedTake> takes;
  const bool audio = core_->stopRecording(&takes);
  if (!audio) core_->waitForCallback(250);  // the live events before the stop have been captured
  Command m;
  m.type = CmdType::MidiCapture;
  m.value = 0.0;
  core_->post(m);
  core_->drainCapturedMidi(capturedMidi_);
  midiRecStart_ = core_->recordStartFrame();
  lastMidi_ = capturedMidi_;
  capturedMidi_.clear();
  lastTakes_ = std::move(takes);
  if (!audio && midiRecTracks_.empty()) return SQ_S_FALSE;
  const sq_result r = commitTakes(lastTakes_);
  midiRecTracks_.clear();
  return r;
}

// One MIDI clip per armed instrument track from the captured live events: from the record start
// to the record stop (or the last event), notes paired into note events with lengths.
void AudioEngine::appendMidiClips(IEditor* ed, bool* any) {
  *any = false;
  if (midiRecTracks_.empty() || lastMidi_.empty()) return;
  int64_t start = midiRecStart_ >= 0 ? midiRecStart_ : lastMidi_.front().frame;
  int64_t end = midiRecEnd_;
  for (const CapturedMidi& e : lastMidi_) {
    start = std::min(start, e.frame);
    end = std::max(end, e.frame + 1);
  }
  if (start < 0) start = 0;
  const double s = frameToPpq(start);
  double len = frameToPpq(end) - s;
  if (!(len > 0)) len = 1.0;
  constexpr double kMinLen = 1.0 / 256.0;
  std::vector<sq_clip_event> events;
  std::vector<bool> used(lastMidi_.size(), false);
  for (size_t i = 0; i < lastMidi_.size(); ++i) {
    const CapturedMidi& e = lastMidi_[i];
    const uint8_t st = e.bytes[0] & 0xF0u;
    sq_clip_event ce{};
    ce.struct_size = sizeof ce;
    ce.position = std::max(0.0, frameToPpq(e.frame) - s);
    ce.bytes[0] = e.bytes[0];
    ce.bytes[1] = e.bytes[1];
    ce.bytes[2] = e.bytes[2];
    ce.size = e.size;
    if (st == 0x80 || (st == 0x90 && e.bytes[2] == 0)) continue;  // consumed by its note-on (or orphaned)
    if (st == 0x90) {
      double offPpq = s + len;
      for (size_t k = i + 1; k < lastMidi_.size(); ++k) {
        const CapturedMidi& o = lastMidi_[k];
        const uint8_t ost = o.bytes[0] & 0xF0u;
        if (used[k] || (o.bytes[0] & 0x0Fu) != (e.bytes[0] & 0x0Fu) || o.bytes[1] != e.bytes[1]) continue;
        if (ost == 0x80 || (ost == 0x90 && o.bytes[2] == 0)) {
          used[k] = true;
          offPpq = frameToPpq(o.frame);
          break;
        }
      }
      ce.length = std::max(kMinLen, offPpq - s - ce.position);
      if (ce.position + ce.length > len) len = ce.position + ce.length;
    }
    events.push_back(ce);
  }
  if (events.empty()) return;
  for (const sq_guid& track : midiRecTracks_) {
    auto p = sq::makeAttributes();
    sq_guid t = track;
    p->setGuid("track", &t);
    p->setInt("kind", SQ_CLIP_MIDI);
    p->setDouble("start", s);
    p->setDouble("length", len);
    p->setString("name", "Recorded MIDI");
    auto res = sq::makeAttributes();
    if (SQ_FAILED(ed->execute(&doc::kAddClip, p.get(), res.get()))) continue;
    sq_guid clip{};
    if (SQ_FAILED(res->getGuid("node", &clip))) continue;
    auto q = sq::makeAttributes();
    q->setGuid("clip", &clip);
    q->setBlob("events", events.data(), static_cast<uint32_t>(events.size() * sizeof(sq_clip_event)));
    if (SQ_SUCCEEDED(ed->execute(&doc::kSetClipEvents, q.get(), nullptr))) *any = true;
  }
}

sq_result AudioEngine::commitTakes(const std::vector<RecordedTake>& takes) {
  if (!doc_) return SQ_OK;
  sq::Ptr<IEditor> ed;
  if (SQ_FAILED(doc_->getEditor(ed.put())) || !ed) return SQ_E_FAIL;
  bool any = false;
  for (auto& t : takes) any = any || (t.result == SQ_OK && t.frames > 0);
  any = any || (!midiRecTracks_.empty() && !lastMidi_.empty());
  if (!any) return SQ_S_FALSE;
  ed->beginTransaction("Record");
  bool midiAny = false;
  appendMidiClips(ed.get(), &midiAny);
  const double sr = sampleRate_ > 0 ? sampleRate_ : 48000.0;
  for (auto& t : takes) {
    if (t.result != SQ_OK || t.frames <= 0) continue;
    int64_t start = t.startFrame;
    double offsetSec = 0.0;
    if (start < 0) {
      offsetSec = static_cast<double>(-start) / sr;
      start = 0;
    }
    const int64_t end = t.startFrame + t.frames;
    if (end <= start) continue;
    const double s = frameToPpq(start), e = frameToPpq(end);
    auto p = sq::makeAttributes();
    p->setGuid("track", &t.trackId);
    p->setInt("kind", SQ_CLIP_AUDIO);
    p->setDouble("start", s);
    p->setDouble("length", e - s);
    p->setString("source", t.path.c_str());
    p->setDouble("source_offset", offsetSec);
    const sq_result r = ed->execute(&doc::kAddClip, p.get(), nullptr);
    if (SQ_FAILED(r)) {
      ed->cancelTransaction();
      return r;
    }
  }
  return ed->commitTransaction();
}

// ---- rendering ----
sq_result AudioEngine::startRender(const sq_render_request* request, IRenderObserver* observer, IRenderTask** out) {
  if (!request || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
  const int32_t kind = request->kind;
  if (kind != SQ_RENDER_MIXDOWN && kind != SQ_RENDER_STEMS && kind != SQ_RENDER_BOUNCE_IN_PLACE && kind != kRenderClick)
    return SQ_E_INVALIDARG;
  if (request->flags & SQ_RENDER_REALTIME) return SQ_E_UNSUPPORTED;  // device-clocked render: not implemented
  if (!std::isfinite(request->start) || !std::isfinite(request->end) || request->end <= request->start) return SQ_E_INVALIDARG;
  if ((request->flags & SQ_RENDER_TAIL) && !(request->tail_seconds >= 0.0 && request->tail_seconds <= 3600.0))
    return SQ_E_INVALIDARG;  // negative / NaN / absurd tails (would wrap to a ~25 h render)
  if (request->track_count > 0 && !request->track_ids) return SQ_E_INVALIDARG;
  GraphDesc d = desc_;
  if (device_) d.sampleRate = sampleRate_;
  if (request->sample_rate > 0 && std::fabs(request->sample_rate - d.sampleRate) > 0.5) return SQ_E_UNSUPPORTED;  // no SRC yet
  for (auto& t : d.tracks) t.meterSlot = -1;
  for (auto& b : d.buses) b.meterSlot = -1;
  d.masterMeterSlot = -1;

  RenderJob job;
  job.kind = kind;
  job.flags = request->flags;
  job.start = ppqToFrame(request->start);
  job.end = ppqToFrame(request->end);
  if (job.end <= job.start) return SQ_E_INVALIDARG;  // range shorter than one frame
  job.tailFrames = (request->flags & SQ_RENDER_TAIL) ? static_cast<uint32_t>(std::llround(request->tail_seconds * d.sampleRate)) : 0;
  job.mix64 = mix64_;
  job.outputChannels = request->channels ? request->channels : (outChannels_ ? outChannels_ : 2);
  if (job.outputChannels > kMaxChannels) return SQ_E_INVALIDARG;
  for (uint32_t i = 0; request->track_ids && i < request->track_count; ++i) {
    const int32_t idx = trackIndex(request->track_ids[i]);
    if (idx < 0) return SQ_E_NOTFOUND;
    job.tracks.push_back(idx);
  }
  if (kind == SQ_RENDER_STEMS && job.tracks.empty())
    for (size_t i = 0; i < d.tracks.size(); ++i) job.tracks.push_back(static_cast<int32_t>(i));
  if (kind == SQ_RENDER_BOUNCE_IN_PLACE && job.tracks.empty()) return SQ_E_INVALIDARG;

  const std::string ext = request->file_extension && request->file_extension[0] ? request->file_extension : "wav";
  struct Out {
    int32_t track;
    uint32_t channels;
    std::string path;
  };
  std::vector<Out> plan;
  if (kind == SQ_RENDER_MIXDOWN || kind == kRenderClick) {
    if (!request->output) return SQ_E_INVALIDARG;
    plan.push_back(Out{-1, job.outputChannels, request->output});
  } else {
    std::string dir = request->output ? request->output : (kind == SQ_RENDER_BOUNCE_IN_PLACE ? mediaFolder() : std::string());
    if (dir.empty()) return SQ_E_INVALIDARG;
    sq_fs_create_dirs(dir.c_str());
    for (int32_t t : job.tracks) {
      const TrackDesc& td = d.tracks[static_cast<size_t>(t)];
      const bool pre = kind == SQ_RENDER_BOUNCE_IN_PLACE || (request->flags & SQ_RENDER_PRE_FADER);
      const uint32_t ch = pre ? td.channels : (td.output < 0 ? job.outputChannels : d.buses[static_cast<size_t>(td.output)].channels);
      const std::string stem = sanitize(td.name.empty() ? "track" + std::to_string(t + 1) : td.name);
      std::string path = kind == SQ_RENDER_BOUNCE_IN_PLACE ? uniquePath(dir, stem + "_bounce", ext)
                                                           : sq::joinPath(dir, stem + "." + ext);
      plan.push_back(Out{t, ch, path});
    }
  }
  // The offline instances start from the live plugin states like a recompile would (an export
  // sounds like playback); nothing is consumed or kept. The click render hosts no plugins.
  if (kind != kRenderClick) captureRenderStates(d);
  // Plugin inserts are instantiated/activated by compile and released by ~Graph: both happen on
  // this (main) thread. The worker only runs process(); idle() destroys the graphs afterwards.
  auto graphs = std::make_shared<OfflineGraphSet>();
  {
    const sq_result cr = compileRenderGraphs(d, job, &graphs->graphs);
    if (SQ_FAILED(cr)) return cr;
  }
  IClassRegistry* reg = reg_;
  sq::Ptr<IAttributes> options(request->codec_options);
  const sq_sample_format fmt = request->sample_format;
  const double sampleRate = d.sampleRate;
  std::shared_ptr<std::atomic<int32_t>> active = activeRenders_;
  auto work = [reg, sampleRate, job, plan, ext, options, fmt, active, graphs](RenderTask& task) -> sq_result {
    struct Done {
      OfflineGraphSet& g;
      ~Done() { g.done.store(true, std::memory_order_release); }  // hand the graphs back to idle()
    } done{*graphs};
    active->fetch_add(1);
    std::vector<RenderOutput> outs;
    sq_result r = SQ_OK;
    for (auto& p : plan) {
      RenderOutput o;
      o.track = p.track;
      o.channels = p.channels;
      o.path = p.path;
      r = openRenderWriter(reg, p.path, ext, sampleRate, p.channels, fmt, options.get(), &o.writer);
      if (SQ_FAILED(r)) break;
      outs.push_back(std::move(o));
    }
    if (SQ_SUCCEEDED(r))
      r = renderOffline(graphs->graphs, job, outs, [&task](double p) { task.reportProgress(p); }, &task.cancelFlag());
    if (SQ_SUCCEEDED(r)) {
      for (auto& o : outs) {
        r = o.writer->finalize();
        if (SQ_FAILED(r)) break;
        task.addOutputPath(o.path);
      }
    }
    outs.clear();  // unfinalized writers abort
    active->fetch_sub(1);
    return r;
  };
  sq::Ptr<RenderTask> task = sq::Ptr<RenderTask>::adopt(
      new RenderTask(std::move(work), sq::Ptr<IRenderObserver>(observer), kind == SQ_RENDER_BOUNCE_IN_PLACE));
  if (kind == SQ_RENDER_BOUNCE_IN_PLACE) {
    BouncePlan bp;
    for (int32_t t : job.tracks) bp.tracks.push_back(desc_.tracks[static_cast<size_t>(t)].id);
    for (auto& p : plan) bp.paths.push_back(p.path);
    bp.start = request->start;
    bp.end = request->end;
    bp.endWithTail = frameToPpq(job.end + job.tailFrames);
    bouncePlans_[task.get()] = bp;
    mainThreadTasks_.push_back(task);
  }
  offlineGraphs_.push_back(OfflineEntry{graphs, task});
  task->startWorker();
  *out = task.detach();
  return SQ_OK;
}

void AudioEngine::reapOfflineGraphs(bool all, uint32_t waitMs) {
  const uint64_t deadline = sq_time_monotonic_ns() + static_cast<uint64_t>(waitMs) * 1000000ull;
  for (size_t i = 0; i < offlineGraphs_.size();) {
    OfflineEntry& e = offlineGraphs_[i];
    if (all && !e.graphs->done.load(std::memory_order_acquire)) {
      if (e.task) e.task->cancel();
      while (!e.graphs->done.load(std::memory_order_acquire) && sq_time_monotonic_ns() < deadline) sq_thread_sleep_ms(1);
    }
    if (e.graphs->done.load(std::memory_order_acquire)) {
      // Main thread: deactivate + release the render's plugin instances here.
      e.graphs->graphs.main.reset();
      e.graphs->graphs.analysis.reset();
      offlineGraphs_.erase(offlineGraphs_.begin() + static_cast<std::ptrdiff_t>(i));
    } else if (all) {
      // Worker did not stop in time (a plugin stuck in process()): never free under it. The
      // worker's own reference keeps the graphs; they die with the task (logged, should not happen).
      sq_log_printf(SQ_LOG_WARN, "engine", "offline render did not stop; its graph is released by the worker");
      offlineGraphs_.erase(offlineGraphs_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

sq_result AudioEngine::applyBounce(RenderTask& task) {
  auto it = bouncePlans_.find(&task);
  if (it == bouncePlans_.end()) return SQ_E_NOTFOUND;
  const BouncePlan plan = it->second;
  bouncePlans_.erase(it);
  if (!doc_) return SQ_OK;  // files rendered; nothing to replace without a document
  sq::Ptr<IEditor> ed;
  if (SQ_FAILED(doc_->getEditor(ed.put())) || !ed) return SQ_E_FAIL;
  ed->beginTransaction("Bounce in place");
  auto failed = [&](sq_result r) {
    ed->cancelTransaction();
    return r;
  };
  for (size_t k = 0; k < plan.tracks.size(); ++k) {
    sq::Ptr<INode> node;
    if (SQ_FAILED(doc_->findNode(&plan.tracks[k], node.put())) || !node) return failed(SQ_E_NOTFOUND);
    sq::Ptr<ITrack> track = node.as<ITrack>();
    if (!track) return failed(SQ_E_NOTFOUND);
    sq_track_info ti{};
    ti.struct_size = sizeof ti;
    track->getTrackInfo(&ti);
    // snapshot overlapping clips first: splitting changes indices
    struct C {
      sq_guid id;
      double s, e;
    };
    std::vector<C> clips;
    for (uint32_t c = 0; c < ti.clip_count; ++c) {
      sq::Ptr<IClip> clip;
      if (SQ_FAILED(track->getClip(c, clip.put())) || !clip) continue;
      sq_clip_info ci{};
      ci.struct_size = sizeof ci;
      clip->getClipInfo(&ci);
      const double s = ci.start, e = ci.start + ci.length;
      if (e <= plan.start || s >= plan.end) continue;
      C x{};
      clip->getId(&x.id);
      x.s = s;
      x.e = e;
      clips.push_back(x);
    }
    for (auto& c : clips) {
      sq_guid target = c.id;
      if (c.s < plan.start) {
        auto p = sq::makeAttributes();
        p->setGuid("node", &target);
        p->setDouble("at", plan.start);
        auto res = sq::makeAttributes();
        const sq_result r = ed->execute(&doc::kSplitClip, p.get(), res.get());
        if (SQ_FAILED(r)) return failed(r);
        if (SQ_FAILED(res->getGuid("node", &target))) return failed(SQ_E_FAIL);
      }
      if (c.e > plan.end) {
        auto p = sq::makeAttributes();
        p->setGuid("node", &target);
        p->setDouble("at", plan.end);
        const sq_result r = ed->execute(&doc::kSplitClip, p.get(), nullptr);
        if (SQ_FAILED(r)) return failed(r);
      }
      auto p = sq::makeAttributes();
      p->setGuid("node", &target);
      const sq_result r = ed->execute(&doc::kRemoveNode, p.get(), nullptr);
      if (SQ_FAILED(r)) return failed(r);
    }
    auto p = sq::makeAttributes();
    p->setGuid("track", &plan.tracks[k]);
    p->setInt("kind", SQ_CLIP_AUDIO);
    p->setDouble("start", plan.start);
    p->setDouble("length", plan.endWithTail - plan.start);
    p->setString("source", plan.paths[k].c_str());
    p->setDouble("source_offset", 0.0);
    const sq_result r = ed->execute(&doc::kAddClip, p.get(), nullptr);
    if (SQ_FAILED(r)) return failed(r);
  }
  return ed->commitTransaction();
}

void AudioEngine::checkDevice() {
  if (deviceListChanged_->exchange(false)) notify("engine.devicelist");
  if (!device_ || recovering_) {
    deviceReset_->store(0);
    return;
  }
  int32_t reason = deviceReset_->exchange(0);
  // Drivers that do not notify (or a notification lost in a race) are caught by polling.
  if (reason == 0 && core_->running() && !device_->isRunning()) reason = SQ_AUDIO_RESET_DEVICE_LOST;
  if (reason == 0) return;
  recovering_ = true;
  const sq_guid cls = driverClass_;
  const std::string id = deviceId_;
  const bool haveReq = haveRequest_;
  sq_audio_device_config rq = request_;
  const std::vector<uint32_t> ins = requestIns_, outs = requestOuts_;
  rq.input_channels = ins.empty() ? nullptr : ins.data();
  rq.output_channels = outs.empty() ? nullptr : outs.data();
  closeDevice();  // stop() -> onStop(): the core leaves the running path; notifies engine.device
  ++deviceRecoveries_;
  sq_result r = SQ_E_NOTFOUND;
  if (!(cls == sq_guid{})) {
    r = openDevice(&cls, id.c_str(), haveReq ? &rq : nullptr);
    if (SQ_FAILED(r) && reason == SQ_AUDIO_RESET_DEVICE_LOST) r = openDevice(&cls, "", haveReq ? &rq : nullptr);  // driver default
  }
  lastRecoveryResult_ = r;
  if (SQ_FAILED(r))
    sq_log_printf(SQ_LOG_WARN, "engine", "audio device '%s' lost (reason %d); reopen failed (%d), engine closed", id.c_str(),
                  static_cast<int>(reason), static_cast<int>(r));
  else
    sq_log_printf(SQ_LOG_INFO, "engine", "audio device '%s' reset (reason %d); reopened '%s'", id.c_str(), static_cast<int>(reason),
                  deviceId_.c_str());
  recovering_ = false;
}

void AudioEngine::idle() {
  checkDevice();
  core_->collectGarbage();
  if (recordingActive_) core_->drainCapturedMidi(capturedMidi_);
  reapOfflineGraphs(false, 0);
  for (size_t i = 0; i < mainThreadTasks_.size();) {
    RenderTask& t = *mainThreadTasks_[i];
    if (t.awaitingMainThread()) {
      const sq_result r = applyBounce(t);
      t.completeOnMainThread(r);
    }
    if (t.getResult() != SQ_E_BUSY) {
      bouncePlans_.erase(&t);
      mainThreadTasks_.erase(mainThreadTasks_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

// ---- ISubject ----
sq_result AudioEngine::addObserver(IObserver* observer) {
  if (!observer) return SQ_E_INVALIDARG;
  for (auto& o : observers_)
    if (o.get() == observer) return SQ_S_FALSE;
  observers_.emplace_back(observer);
  return SQ_OK;
}

sq_result AudioEngine::removeObserver(IObserver* observer) {
  for (auto it = observers_.begin(); it != observers_.end(); ++it)
    if (it->get() == observer) {
      observers_.erase(it);
      return SQ_OK;
    }
  return SQ_E_NOTFOUND;
}

}  // namespace sq::engine::audio
