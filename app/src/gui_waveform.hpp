// Squizzy Studio GUI - waveform peak cache for the arrange view (and any other panel that wants
// to draw audio files).
//
// Every media file is decoded ONCE on a background worker thread through the codec registry
// (category "Codec", the same path the engine uses) into multi-resolution min/max peaks
// (16-, 256- and 4096-frame buckets per channel). The UI thread asks for a file with request()
// (starts loading it the first time) and, once ready, reads column peaks through the returned
// PeakData without locking. Files are keyed by path.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sq/interfaces/codec.h"

namespace sq::app::gui {

struct AudioFileInfo {
  double sampleRate = 0;
  uint32_t channels = 0;
  int64_t frames = 0;
  double seconds() const { return sampleRate > 0 ? static_cast<double>(frames) / sampleRate : 0.0; }
};

// Lower-case extension without the dot ("" if none).
std::string fileExtension(const std::string& path);
bool isAudioFileExtension(const std::string& ext);
bool isMidiFileExtension(const std::string& ext);
// Opens the file with a codec reader and reads its format (no decoding). Main or worker thread.
bool probeAudioFile(IClassRegistry* reg, const std::string& path, AudioFileInfo* out);

// Decoded peaks of one file. Immutable once state() == Ready.
class PeakData {
 public:
  enum State { Loading = 0, Ready = 1, Failed = 2 };
  State state() const { return static_cast<State>(state_.load(std::memory_order_acquire)); }
  const AudioFileInfo& info() const { return info_; }
  // Min/max of channel `ch` over source frames [f0, f1) (ch >= channels: all channels merged).
  // Picks the coarsest resolution that still has at least one bucket per range. False if not
  // ready or the range is outside the file.
  bool range(uint32_t ch, double f0, double f1, float* mn, float* mx) const;

 private:
  friend class PeakCache;
  struct Level {
    uint32_t bucket = 0;      // frames per bucket
    size_t count = 0;         // buckets
    std::vector<float> data;  // [bucket][channel][min, max]
  };
  std::atomic<int> state_{Loading};
  AudioFileInfo info_;
  std::vector<Level> levels_;  // finest first
};

class PeakCache {
 public:
  explicit PeakCache(IClassRegistry* reg);
  ~PeakCache();
  PeakCache(const PeakCache&) = delete;
  PeakCache& operator=(const PeakCache&) = delete;

  // The file's peaks (loading starts on the first request). Never null for a non-empty path.
  std::shared_ptr<const PeakData> request(const std::string& path);
  // True while files are queued or decoding (the view keeps repainting to pick them up).
  bool busy() const { return pending_.load(std::memory_order_acquire) > 0; }

 private:
  void workerMain();
  void decode(const std::string& path, PeakData& d);

  IClassRegistry* reg_;
  std::map<std::string, std::shared_ptr<PeakData>> entries_;  // UI thread only
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::pair<std::string, std::shared_ptr<PeakData>>> queue_;  // guarded by m_
  std::atomic<int> pending_{0};
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

}  // namespace sq::app::gui
