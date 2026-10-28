// Squizzy Studio GUI - waveform peak cache (see gui_waveform.hpp).
#include "gui_waveform.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "sq/sdk.hpp"

namespace sq::app::gui {

namespace {
constexpr uint32_t kLevels[] = {16, 256, 4096};
constexpr uint32_t kChunk = 8192;
constexpr uint32_t kMaxChannels = 32;

sq::Ptr<IAudioFileReader> openReader(IClassRegistry* reg, const std::string& path, sq_audio_file_format* fmt) {
  const std::string ext = fileExtension(path);
  if (!reg || ext.empty()) return {};
  auto codec = sq::Ptr<ICodecService>::adopt(codec::findCodec(reg, ext.c_str(), nullptr, SQ_CODEC_READ));
  if (!codec) return {};
  sq::Ptr<IAudioFileReader> reader;
  if (SQ_FAILED(codec->createReader(reader.put())) || !reader) return {};
  if (SQ_FAILED(reader->open(path.c_str()))) return {};
  *fmt = sq_audio_file_format{};
  fmt->struct_size = sizeof *fmt;
  if (SQ_FAILED(reader->getFormat(fmt)) || fmt->channels == 0 || fmt->channels > kMaxChannels || !(fmt->sample_rate > 0)) return {};
  return reader;
}
}  // namespace

std::string fileExtension(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string e = path.substr(dot + 1);
  for (char& ch : e) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
  return e;
}

bool isAudioFileExtension(const std::string& ext) {
  static const char* kExt[] = {"wav", "wave", "flac", "ogg", "oga", "opus", "mp3", "aif", "aiff", "aifc", "m4a", "aac", "rf64", "w64"};
  for (const char* e : kExt)
    if (ext == e) return true;
  return false;
}

bool isMidiFileExtension(const std::string& ext) { return ext == "mid" || ext == "midi" || ext == "smf"; }

bool probeAudioFile(IClassRegistry* reg, const std::string& path, AudioFileInfo* out) {
  sq_audio_file_format fmt{};
  auto r = openReader(reg, path, &fmt);
  if (!r) return false;
  if (out) {
    out->sampleRate = fmt.sample_rate;
    out->channels = fmt.channels;
    out->frames = fmt.frame_count;
    if (out->frames < 0) {  // unknown length (stream): count by reading
      int64_t n = 0;
      std::vector<float> tmp(static_cast<size_t>(kChunk) * fmt.channels);
      std::vector<float*> ptrs(fmt.channels);
      for (uint32_t c = 0; c < fmt.channels; ++c) ptrs[c] = &tmp[static_cast<size_t>(c) * kChunk];
      for (;;) {
        sq_audio_block blk{ptrs.data(), fmt.channels, kChunk, 0};
        uint32_t got = 0;
        if (SQ_FAILED(r->read(&blk, &got))) break;
        n += got;
        if (got < kChunk) break;
      }
      out->frames = n;
    }
  }
  return true;
}

// ---- PeakData ------------------------------------------------------------------------------------
bool PeakData::range(uint32_t ch, double f0, double f1, float* mn, float* mx) const {
  if (state() != Ready || levels_.empty() || info_.channels == 0) return false;
  if (f1 <= 0 || f0 >= static_cast<double>(info_.frames)) return false;
  f0 = std::max(0.0, f0);
  f1 = std::min(static_cast<double>(info_.frames), std::max(f1, f0 + 1.0));
  const double span = f1 - f0;
  const Level* lv = &levels_.front();
  for (const Level& l : levels_)
    if (static_cast<double>(l.bucket) <= span) lv = &l;
  if (lv->count == 0) return false;
  const auto b = static_cast<double>(lv->bucket);
  size_t i0 = static_cast<size_t>(std::floor(f0 / b));
  size_t i1 = static_cast<size_t>(std::ceil(f1 / b));
  i0 = std::min(i0, lv->count - 1);
  i1 = std::clamp(i1, i0 + 1, lv->count);
  const uint32_t nch = info_.channels;
  const uint32_t c0 = ch < nch ? ch : 0, c1 = ch < nch ? ch + 1 : nch;
  float lo = 1e9f, hi = -1e9f;
  for (size_t i = i0; i < i1; ++i) {
    const float* p = &lv->data[(i * nch + c0) * 2];
    for (uint32_t c = c0; c < c1; ++c, p += 2) {
      lo = std::min(lo, p[0]);
      hi = std::max(hi, p[1]);
    }
  }
  *mn = lo;
  *mx = hi;
  return true;
}

// ---- PeakCache -----------------------------------------------------------------------------------
PeakCache::PeakCache(IClassRegistry* reg) : reg_(reg) { worker_ = std::thread([this] { workerMain(); }); }

PeakCache::~PeakCache() {
  {
    std::lock_guard<std::mutex> lk(m_);
    stop_.store(true);
    queue_.clear();
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::shared_ptr<const PeakData> PeakCache::request(const std::string& path) {
  if (path.empty()) return {};
  auto it = entries_.find(path);
  if (it != entries_.end()) return it->second;
  auto d = std::make_shared<PeakData>();
  entries_.emplace(path, d);
  pending_.fetch_add(1);
  {
    std::lock_guard<std::mutex> lk(m_);
    queue_.emplace_back(path, d);
  }
  cv_.notify_one();
  return d;
}

void PeakCache::workerMain() {
  for (;;) {
    std::pair<std::string, std::shared_ptr<PeakData>> job;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [this] { return stop_.load() || !queue_.empty(); });
      if (stop_.load()) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    decode(job.first, *job.second);
    pending_.fetch_sub(1);
  }
}

void PeakCache::decode(const std::string& path, PeakData& d) {
  sq_audio_file_format fmt{};
  auto reader = openReader(reg_, path, &fmt);
  if (!reader) {
    d.state_.store(PeakData::Failed, std::memory_order_release);
    return;
  }
  const uint32_t nch = fmt.channels;
  d.info_.sampleRate = fmt.sample_rate;
  d.info_.channels = nch;
  d.levels_.resize(std::size(kLevels));
  for (size_t l = 0; l < std::size(kLevels); ++l) {
    d.levels_[l].bucket = kLevels[l];
    if (fmt.frame_count > 0) d.levels_[l].data.reserve(static_cast<size_t>(fmt.frame_count / kLevels[l] + 1) * nch * 2);
  }
  // Finest level straight from the samples; coarser levels by merging finer buckets.
  std::vector<float> tmp(static_cast<size_t>(kChunk) * nch);
  std::vector<float*> ptrs(nch);
  for (uint32_t c = 0; c < nch; ++c) ptrs[c] = &tmp[static_cast<size_t>(c) * kChunk];
  std::vector<float> acc(static_cast<size_t>(nch) * 2);
  uint32_t inBucket = 0;
  int64_t total = 0;
  auto resetAcc = [&] {
    for (uint32_t c = 0; c < nch; ++c) {
      acc[c * 2] = 1e9f;
      acc[c * 2 + 1] = -1e9f;
    }
  };
  resetAcc();
  auto& fine = d.levels_[0].data;
  for (;;) {
    if (stop_.load(std::memory_order_relaxed)) {
      d.state_.store(PeakData::Failed, std::memory_order_release);
      return;
    }
    sq_audio_block blk{ptrs.data(), nch, kChunk, 0};
    uint32_t got = 0;
    if (SQ_FAILED(reader->read(&blk, &got))) break;
    for (uint32_t i = 0; i < got; ++i) {
      for (uint32_t c = 0; c < nch; ++c) {
        const float v = ptrs[c][i];
        acc[c * 2] = std::min(acc[c * 2], v);
        acc[c * 2 + 1] = std::max(acc[c * 2 + 1], v);
      }
      if (++inBucket == kLevels[0]) {
        fine.insert(fine.end(), acc.begin(), acc.end());
        resetAcc();
        inBucket = 0;
      }
    }
    total += got;
    if (got < kChunk) break;
  }
  if (inBucket > 0) fine.insert(fine.end(), acc.begin(), acc.end());
  d.levels_[0].count = fine.size() / (static_cast<size_t>(nch) * 2);
  for (size_t l = 1; l < d.levels_.size(); ++l) {
    const auto& src = d.levels_[l - 1];
    auto& dst = d.levels_[l];
    const size_t ratio = dst.bucket / src.bucket;
    dst.count = (src.count + ratio - 1) / ratio;
    dst.data.assign(dst.count * nch * 2, 0.0f);
    for (size_t b = 0; b < dst.count; ++b) {
      for (uint32_t c = 0; c < nch; ++c) {
        float lo = 1e9f, hi = -1e9f;
        for (size_t k = b * ratio; k < std::min(src.count, (b + 1) * ratio); ++k) {
          lo = std::min(lo, src.data[(k * nch + c) * 2]);
          hi = std::max(hi, src.data[(k * nch + c) * 2 + 1]);
        }
        dst.data[(b * nch + c) * 2] = lo;
        dst.data[(b * nch + c) * 2 + 1] = hi;
      }
    }
  }
  d.info_.frames = total;
  d.state_.store(total > 0 ? PeakData::Ready : PeakData::Failed, std::memory_order_release);
}

}  // namespace sq::app::gui
