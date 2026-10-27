// Squizzy Studio engine/audio - record streams + disk writer thread.
#include "sq/engine/audio/recorder.hpp"

#include "sq/platform.h"

namespace sq::engine::audio {

RecordStream::RecordStream(sq_guid trackId, uint32_t channels, std::vector<uint32_t> inputChannels,
                           sq::Ptr<IAudioFileWriter> writer, std::string path, size_t ringFrames)
    : trackId_(trackId),
      channels_(channels ? channels : 1),
      inputs_(std::move(inputChannels)),
      writer_(std::move(writer)),
      path_(std::move(path)),
      ring_(channels ? channels : 1, ringFrames) {}

bool RecordStream::push(const float* const* deviceInput, uint32_t deviceInputChannels, uint32_t frames, int64_t startFrame) noexcept {
  const float* src[kMaxChannels];
  for (uint32_t c = 0; c < channels_ && c < kMaxChannels; ++c) {
    src[c] = nullptr;
    if (inputs_.empty()) continue;
    const uint32_t idx = inputs_[c < inputs_.size() ? c : inputs_.size() - 1];
    if (deviceInput && idx < deviceInputChannels) src[c] = deviceInput[idx];
  }
  if (!started_.load(std::memory_order_relaxed)) {
    start_.store(startFrame, std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);
  }
  if (!ring_.write(src, channels_, frames)) {
    overflow_.fetch_add(frames, std::memory_order_relaxed);
    return false;
  }
  pushed_.fetch_add(frames, std::memory_order_relaxed);
  return true;
}

int64_t RecordStream::drain(std::vector<float>& interleaved, std::vector<float>& planar) {
  constexpr size_t kChunk = 4096;
  if (interleaved.size() < kChunk * channels_) interleaved.resize(kChunk * channels_);
  if (planar.size() < kChunk * channels_) planar.resize(kChunk * channels_);
  int64_t total = 0;
  for (;;) {
    const size_t n = ring_.read(interleaved.data(), kChunk);
    if (n == 0) break;
    float* ptrs[kMaxChannels];
    for (uint32_t c = 0; c < channels_; ++c) {
      ptrs[c] = &planar[c * kChunk];
      for (size_t i = 0; i < n; ++i) ptrs[c][i] = interleaved[i * channels_ + c];
    }
    if (writer_ && error_.load() == SQ_OK) {
      sq_audio_block blk{ptrs, channels_, static_cast<uint32_t>(n), 0};
      const sq_result r = writer_->write(&blk);
      if (SQ_FAILED(r)) error_.store(r);
    }
    written_.fetch_add(n);
    total += static_cast<int64_t>(n);
  }
  return error_.load() == SQ_OK ? total : -1;
}

RecordSession::~RecordSession() {
  if (!finished_) finish();
}

void RecordSession::start() {
  if (thread_.joinable()) return;
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
}

void RecordSession::run() {
  sq_thread_set_current_name("DiskWriter");
  std::vector<float> inter, planar;
  while (!stop_.load()) {
    for (auto& s : streams_) s->drain(inter, planar);
    sq_thread_sleep_ms(pollMs_);
  }
  for (auto& s : streams_) s->drain(inter, planar);
}

std::vector<RecordedTake> RecordSession::finish() {
  std::vector<RecordedTake> takes;
  if (finished_) return takes;
  finished_ = true;
  stop_.store(true);
  if (thread_.joinable()) thread_.join();
  else {
    std::vector<float> inter, planar;
    for (auto& s : streams_) s->drain(inter, planar);
  }
  for (auto& s : streams_) {
    RecordedTake t;
    t.trackId = s->trackId();
    t.path = s->path();
    t.startFrame = s->startFrame();
    t.frames = static_cast<int64_t>(s->framesWritten());
    t.overflowFrames = s->overflowFrames();
    t.result = s->writeError();
    if (s->writer()) {
      if (t.result == SQ_OK && t.frames > 0) {
        const sq_result r = s->writer()->finalize();
        if (SQ_FAILED(r)) t.result = r;
      }
      // a writer released without finalize() aborts (and deletes its file)
    }
    takes.push_back(t);
  }
  return takes;
}

}  // namespace sq::engine::audio
