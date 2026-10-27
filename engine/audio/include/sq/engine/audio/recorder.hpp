// Squizzy Studio engine/audio - recording to disk.
//
// The audio thread pushes each recorded track's input into a lock-free FrameRing
// (RecordStream::push, RT). A DiskWriter thread (normal priority, NOT realtime) drains the rings
// and writes through codec IAudioFileWriter objects. A full ring is a dropout: the block is
// dropped and counted in overflowFrames(); the writer never blocks the audio thread.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sq/sdk.hpp"
#include "sq/interfaces/codec.h"
#include "sq/engine/audio/rt.hpp"

namespace sq::engine::audio {

class RecordStream {
 public:
  // writer: opened IAudioFileWriter (channels == channels). ringFrames: ring capacity.
  RecordStream(sq_guid trackId, uint32_t channels, std::vector<uint32_t> inputChannels, sq::Ptr<IAudioFileWriter> writer,
               std::string path, size_t ringFrames);

  // RT: append one block. startFrame = timeline position of the block's first frame (only the
  // first push of a take latches it).
  bool push(const float* const* deviceInput, uint32_t deviceInputChannels, uint32_t frames, int64_t startFrame) noexcept;

  // Writer thread: drain up to the whole ring. Returns frames written (or <0 on write error).
  int64_t drain(std::vector<float>& interleaved, std::vector<float>& planar);

  const sq_guid& trackId() const noexcept { return trackId_; }
  uint32_t channels() const noexcept { return channels_; }
  const std::vector<uint32_t>& inputChannels() const noexcept { return inputs_; }
  const std::string& path() const noexcept { return path_; }
  int64_t startFrame() const noexcept { return start_.load(); }
  bool started() const noexcept { return started_.load(); }
  uint64_t framesPushed() const noexcept { return pushed_.load(); }
  uint64_t framesWritten() const noexcept { return written_.load(); }
  uint64_t overflowFrames() const noexcept { return overflow_.load(); }
  sq_result writeError() const noexcept { return error_.load(); }
  IAudioFileWriter* writer() const noexcept { return writer_.get(); }

 private:
  sq_guid trackId_;
  uint32_t channels_;
  std::vector<uint32_t> inputs_;
  sq::Ptr<IAudioFileWriter> writer_;
  std::string path_;
  FrameRing ring_;
  std::atomic<int64_t> start_{0};
  std::atomic<bool> started_{false};
  std::atomic<uint64_t> pushed_{0};
  std::atomic<uint64_t> written_{0};
  std::atomic<uint64_t> overflow_{0};
  std::atomic<sq_result> error_{SQ_OK};
};

struct RecordedTake {
  sq_guid trackId{};
  std::string path;
  int64_t startFrame = 0;   // timeline frame (latency compensated)
  int64_t frames = 0;
  uint64_t overflowFrames = 0;
  sq_result result = SQ_OK;
};

// One recording pass: the streams + the disk writer thread.
class RecordSession {
 public:
  explicit RecordSession(uint32_t pollMs = 2) : pollMs_(pollMs) {}
  ~RecordSession();
  void addStream(std::unique_ptr<RecordStream> s) { streams_.push_back(std::move(s)); }
  size_t streamCount() const noexcept { return streams_.size(); }
  RecordStream& stream(size_t i) noexcept { return *streams_[i]; }
  void start();  // starts the writer thread
  // Stops the writer after draining everything; finalizes (or aborts on error) every writer.
  std::vector<RecordedTake> finish();

 private:
  void run();
  std::vector<std::unique_ptr<RecordStream>> streams_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  uint32_t pollMs_;
  bool finished_ = false;
};

}  // namespace sq::engine::audio
