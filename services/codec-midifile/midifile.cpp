// codec-midifile service: Standard MIDI File reader/writer (sq/interfaces/midi.h, category "MidiFile").
#include <cstring>
#include <string>
#include <vector>

#include "smf.hpp"
#include "sq/interfaces/midi.h"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"

namespace {

constexpr uint64_t kMaxFileBytes = uint64_t(512) << 20;

class MidiFileReader final : public sq::Object<MidiFileReader, sq::IMidiFileReader> {
 public:
  sq_result SQ_CALL open(const char* path) override {
    if (!path || !*path) return SQ_E_INVALIDARG;
    opened_ = false;
    if (!sq_fs_is_file(path)) return SQ_E_NOTFOUND;
    uint64_t sz = 0;
    if (sq_fs_file_size(path, &sz) == SQ_OK && sz > kMaxFileBytes) return SQ_E_UNSUPPORTED;
    sq::Buffer b;
    if (sq_fs_read_file(path, b.out()) != SQ_OK) return SQ_E_IO;
    return load(reinterpret_cast<const uint8_t*>(b.data()), b.size());
  }

  sq_result SQ_CALL openStream(sq::IByteStream* stream) override {
    if (!stream) return SQ_E_INVALIDARG;
    opened_ = false;
    try {
      std::vector<uint8_t> bytes;
      uint8_t chunk[65536];
      for (;;) {
        uint32_t got = 0;
        sq_result r = stream->read(chunk, sizeof chunk, &got);
        if (SQ_FAILED(r)) return r;
        bytes.insert(bytes.end(), chunk, chunk + got);
        if (bytes.size() > kMaxFileBytes) return SQ_E_UNSUPPORTED;
        if (got < sizeof chunk) break;
      }
      return load(bytes.data(), bytes.size());
    } catch (const std::bad_alloc&) {
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL getInfo(sq_midi_file_info* out) override {
    if (!out || out->struct_size < sizeof(sq_midi_file_info)) return SQ_E_INVALIDARG;
    if (!opened_) return SQ_E_CLOSED;
    const uint16_t div = file_.division;
    out->format = file_.format == 0 ? 0u : 1u;
    out->track_count = uint32_t(file_.tracks.size());
    if (div & 0x8000u) {
      out->ticks_per_quarter = 0;
      out->smpte_fps = -int32_t(int8_t(div >> 8));
      out->ticks_per_frame = div & 0xFFu;
    } else {
      out->ticks_per_quarter = div;
      out->smpte_fps = 0;
      out->ticks_per_frame = 0;
    }
    return SQ_OK;
  }

  uint32_t SQ_CALL getEventCount(uint32_t track) override {
    if (!opened_ || track >= file_.tracks.size()) return 0;
    return uint32_t(file_.tracks[track].events.size());
  }

  sq_result SQ_CALL getEvent(uint32_t track, uint32_t index, sq_midi_file_event* out) override {
    if (!out || out->struct_size < sizeof(sq_midi_file_event)) return SQ_E_INVALIDARG;
    if (!opened_) return SQ_E_CLOSED;
    if (track >= file_.tracks.size()) return SQ_E_NOTFOUND;
    const auto& evs = file_.tracks[track].events;
    if (index >= evs.size()) return SQ_E_NOTFOUND;
    const sq::smf::Event& e = evs[index];
    out->tick = e.tick;
    out->status = e.status;
    out->data1 = e.data1;
    out->data2 = e.data2;
    out->meta_type = e.metaType;
    out->size = e.size;
    out->data = e.size ? file_.payload.data() + e.offset : nullptr;
    return SQ_OK;
  }

  sq_result SQ_CALL getTrackName(uint32_t track, char* buf, uint32_t bufSize, uint32_t* needed) override {
    if (!opened_) return SQ_E_CLOSED;
    if (track >= file_.tracks.size()) return SQ_E_NOTFOUND;
    const std::string& n = file_.tracks[track].name;
    const uint32_t need = uint32_t(n.size() + 1);
    if (needed) *needed = need;
    if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
    std::memcpy(buf, n.c_str(), need);
    return SQ_OK;
  }

 private:
  sq_result load(const uint8_t* data, size_t size) {
    try {
      std::string err;
      sq_result r = sq::smf::parse(data, size, file_, &err);
      if (SQ_FAILED(r)) {
        sq::logf(SQ_LOG_DEBUG, "SMF parse failed: %s", err.c_str());
        file_ = sq::smf::File{};
        return r;
      }
      opened_ = true;
      return SQ_OK;
    } catch (const std::bad_alloc&) {
      file_ = sq::smf::File{};
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      file_ = sq::smf::File{};
      return SQ_E_FAIL;
    }
  }

  sq::smf::File file_;
  bool opened_ = false;
};

class MidiFileWriter final : public sq::Object<MidiFileWriter, sq::IMidiFileWriter> {
 public:
  sq_result SQ_CALL create(const char* path, uint32_t format, uint32_t ticksPerQuarter) override {
    if (!path || !*path) return SQ_E_INVALIDARG;
    sq_result r = writer_.begin(format, ticksPerQuarter);
    if (SQ_FAILED(r)) return r;
    path_ = path;
    stream_ = nullptr;
    return SQ_OK;
  }

  sq_result SQ_CALL createStream(sq::IByteStream* stream, uint32_t format, uint32_t ticksPerQuarter) override {
    if (!stream) return SQ_E_INVALIDARG;
    sq_result r = writer_.begin(format, ticksPerQuarter);
    if (SQ_FAILED(r)) return r;
    path_.clear();
    stream_ = sq::Ptr<sq::IByteStream>(stream);
    return SQ_OK;
  }

  sq_result SQ_CALL beginTrack() override { return guard([&] { return writer_.beginTrack(); }); }

  sq_result SQ_CALL addEvent(const sq_midi_file_event* e) override {
    if (!e || e->struct_size < sizeof(sq_midi_file_event)) return SQ_E_INVALIDARG;
    return guard([&] { return writer_.addEvent(e->tick, e->status, e->data1, e->data2, e->meta_type, e->data, e->size); });
  }

  sq_result SQ_CALL endTrack() override { return guard([&] { return writer_.endTrack(); }); }

  sq_result SQ_CALL finish() override {
    if (!writer_.started()) return SQ_E_FAIL;
    return guard([&]() -> sq_result {
      std::vector<uint8_t> bytes;
      sq_result r = writer_.finish(bytes);
      if (SQ_FAILED(r)) return r;
      if (stream_) {
        size_t off = 0;
        while (off < bytes.size()) {
          const size_t left = bytes.size() - off;
          const uint32_t n = left > 0x40000000u ? 0x40000000u : uint32_t(left);
          uint32_t written = 0;
          r = stream_->write(bytes.data() + off, n, &written);
          if (SQ_FAILED(r)) return r;
          if (written == 0) return SQ_E_IO;
          off += written;
        }
        stream_->flush();
        stream_ = nullptr;
        return SQ_OK;
      }
      if (sq_fs_write_file(path_.c_str(), bytes.data(), bytes.size()) != SQ_OK) return SQ_E_IO;
      return SQ_OK;
    });
  }

 private:
  template <class F>
  static sq_result guard(F&& f) noexcept {
    try {
      return f();
    } catch (const std::bad_alloc&) {
      return SQ_E_OUTOFMEMORY;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq::smf::Writer writer_;
  std::string path_;
  sq::Ptr<sq::IByteStream> stream_;
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<MidiFileReader>(sq::midi::kMidiFileReaderClassId, sq::midi::kFileCategory, "Standard MIDI File Reader"),
    sq::classEntry<MidiFileWriter>(sq::midi::kMidiFileWriterClassId, sq::midi::kFileCategory, "Standard MIDI File Writer"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('S', 'M', 'F', 'C'),
    .name = "codec-midifile",
    .description = "Standard MIDI File (type 0/1) reader and writer",
    .classes = kClasses,
    .onInit = nullptr,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
