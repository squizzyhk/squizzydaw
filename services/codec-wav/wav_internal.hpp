// codec-wav internals: reader / writer classes shared by the module translation units.
#pragma once

#include <string>
#include <vector>

#include "sqcodec/common.hpp"
#include "sqcodec/riff_markers.hpp"

namespace sq::wav {

inline constexpr uint32_t kMaxChannels = 1024;
inline constexpr size_t kBextFixed = 602;  // bext v1/v2 fixed part before the coding history

struct InfoMap { const char* tag; const char* id; };
inline constexpr InfoMap kInfoMap[] = {
    {"title", "INAM"}, {"artist", "IART"}, {"album", "IPRD"}, {"genre", "IGNR"}, {"date", "ICRD"},
    {"comment", "ICMT"}, {"track", "ITRK"}, {"copyright", "ICOP"}, {"encoder", "ISFT"}, {"bpm", "IBPM"},
};
const char* infoTagForId(const char* id);
const char* infoIdFor(const std::string& tag);

enum class Enc { Int, IntLE, U8, F32, F64 };
enum class Kind { Wav, Aiff, Aifc };

class Reader final : public cx::ReaderBase<Reader> {
 public:
  sq_result SQ_CALL open(const char* path) override;
  sq_result SQ_CALL openStream(IByteStream* stream) override;
  sq_result SQ_CALL getFormat(sq_audio_file_format* out) override;
  sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* framesRead) override;
  sq_result SQ_CALL seek(sq_sample_time frame) override;
  sq_result SQ_CALL tell(sq_sample_time* frame) override;

 private:
  sq_result attach(Ptr<IByteStream> s);
  sq_result parse();
  sq_result parseRiff(bool rf64);
  sq_result parseFmt(const uint8_t* p, size_t n);
  void parseInfo(const uint8_t* p, size_t n);
  void parseBext(const uint8_t* p, size_t n);
  sq_result parseAiff(bool aifc);
  sq_result parseComm(const uint8_t* p, size_t n, bool aifc, uint32_t& frames);
  void parseAiffMarkers(const std::vector<uint8_t>& mark, const std::vector<uint8_t>& inst);
  bool loadChunk(int64_t off, uint64_t size, std::vector<uint8_t>& out);
  void convert(const uint8_t* src, uint32_t n, sq_audio_block* dst, uint32_t at) const;

  Ptr<IByteStream> stream_;
  int64_t fileSize_ = 0;
  char container_[16] = {};
  bool bigEndian_ = false;
  Enc enc_ = Enc::Int;
  uint32_t channels_ = 0, bytes_ = 0, validBits_ = 0, blockAlign_ = 0;
  double sampleRate_ = 0;
  int64_t dataOffset_ = 0;
  uint64_t dataSize_ = 0;
  sq_sample_time frames_ = 0, pos_ = 0;
  std::vector<uint8_t> buf_;
};

class Writer final : public Object<Writer, IAudioFileWriter> {
 public:
  explicit Writer(Kind k) : kind_(k) {}
  sq_result SQ_CALL open(const char* path, const sq_audio_file_format* format, IAttributes* options) override;
  sq_result SQ_CALL openStream(IByteStream* stream, const sq_audio_file_format* format, IAttributes* options) override;
  sq_result SQ_CALL setMetadata(const char* key, const void* data, uint32_t size) override;
  sq_result SQ_CALL addMarker(const sq_audio_file_marker* marker) override;
  sq_result SQ_CALL write(const sq_audio_block* src) override;
  sq_result SQ_CALL finalize() override;

 private:
  struct Chunk {
    std::string key;  // identity for in-place updates of pre-data chunks
    char id[4];
    std::vector<uint8_t> data;
  };
  struct Emitted {
    std::string key;
    int64_t offset;  // chunk header offset
    std::vector<uint8_t> data;
  };
  sq_result setup(const sq_audio_file_format* f, IAttributes* options);
  sq_result begin();
  sq_result emitChunk(const char* id, const std::vector<uint8_t>& data);
  std::vector<Chunk> finalChunks();
  void wavDerived(std::vector<Chunk>& cs);
  void aiffDerived(std::vector<Chunk>& cs);
  sq_result finishWav(int64_t end);
  sq_result finishAiff(int64_t end);
  uint32_t channelMask() const;

  Kind kind_;
  cx::WriterTarget out_;
  bool configured_ = false, started_ = false, finalized_ = false, failed_ = false;
  // format
  uint32_t channels_ = 0, bytes_ = 0, validBits_ = 0, blockAlign_ = 0;
  double sampleRate_ = 0;
  Enc enc_ = Enc::Int;
  int rf64Mode_ = -1;  // -1 auto, 0 never, 1 always
  cx::Dither dither_;
  // state
  cx::MetaStore meta_;
  std::vector<sq_audio_file_marker> markers_;
  std::vector<Emitted> pre_;
  int64_t dataHeader_ = 0, dataStart_ = 0, factOffset_ = -1, commOffset_ = 0;
  uint64_t dataBytes_ = 0;
  uint64_t frames_ = 0;
  std::vector<uint8_t> buf_;
};

}  // namespace sq::wav
