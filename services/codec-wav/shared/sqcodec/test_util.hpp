// Squizzy Studio codec services - shared test helpers (tests only; links sq::appcore + sq::test).
#pragma once

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sq/app/class_registry.hpp"
#include "sq/app/host_context.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/app/settings.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/cpp/bytestream.hpp"
#include "sq/interfaces/codec.h"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"
#include "sqcodec/common.hpp"
#include "sqtest.hpp"

namespace sqct {

using Channels = std::vector<std::vector<float>>;

// Loads bin/services/<name> through the real loader and creates its ICodecService.
struct CodecEnv {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  std::unique_ptr<sq::app::HostContext> host;
  std::unique_ptr<sq::app::ModuleLoader> loader;
  sq::Ptr<sq::ICodecService> svc;
  bool ok = false;

  CodecEnv(const char* module, const sq_guid& classId) {
    host = std::make_unique<sq::app::HostContext>(settings, reg);
    loader = std::make_unique<sq::app::ModuleLoader>(*host, reg);
    std::string path = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string(module) + sq_dylib_suffix());
    if (!loader->load(path).ok()) return;
    svc = sq::createInstance<sq::ICodecService>(&reg, classId);
    ok = static_cast<bool>(svc);
  }
  ~CodecEnv() {
    svc = nullptr;
    loader.reset();
  }
  sq::Ptr<sq::IAudioFileReader> reader() {
    sq::Ptr<sq::IAudioFileReader> r;
    svc->createReader(r.put());
    return r;
  }
  sq::Ptr<sq::IAudioFileWriter> writer(const char* ext) {
    sq::Ptr<sq::IAudioFileWriter> w;
    svc->createWriter(ext, w.put());
    return w;
  }
};

inline sq_audio_file_format format(double sr, uint32_t ch, sq_sample_format f, uint32_t bits = 0) {
  sq_audio_file_format fmt{};
  fmt.struct_size = sizeof fmt;
  fmt.sample_rate = sr;
  fmt.channels = ch;
  fmt.sample_format = f;
  fmt.bits_per_sample = bits;
  fmt.frame_count = -1;
  return fmt;
}

// Deterministic multi-tone test signal, amplitude <= 0.9.
inline Channels signal(uint32_t ch, uint32_t frames, double sr = 48000.0) {
  Channels c(ch, std::vector<float>(frames));
  for (uint32_t k = 0; k < ch; ++k)
    for (uint32_t i = 0; i < frames; ++i) {
      double t = i / sr;
      c[k][i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * (220.0 * (k + 1)) * t) +
                                   0.3 * std::sin(2 * 3.14159265358979 * 1234.5 * t + k) +
                                   0.1 * std::sin(2 * 3.14159265358979 * 7000.0 * t));
    }
  return c;
}

inline sq_result writeAll(sq::IAudioFileWriter* w, const Channels& c, uint32_t block = 1000) {
  const uint32_t frames = c.empty() ? 0 : static_cast<uint32_t>(c[0].size());
  std::vector<float*> ptrs(c.size());
  for (uint32_t at = 0; at < frames; at += block) {
    uint32_t n = std::min(block, frames - at);
    for (size_t k = 0; k < c.size(); ++k) ptrs[k] = const_cast<float*>(c[k].data()) + at;
    sq_audio_block b{ptrs.data(), static_cast<uint32_t>(c.size()), n, at};
    sq_result r = w->write(&b);
    if (SQ_FAILED(r)) return r;
  }
  return SQ_OK;
}

// Reads up to maxFrames frames from the current position.
inline Channels readFrames(sq::IAudioFileReader* r, uint32_t ch, uint64_t maxFrames, uint32_t block = 777) {
  Channels out(ch);
  std::vector<std::vector<float>> tmp(ch, std::vector<float>(block));
  std::vector<float*> ptrs(ch);
  for (uint32_t k = 0; k < ch; ++k) ptrs[k] = tmp[k].data();
  uint64_t total = 0;
  while (total < maxFrames) {
    uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(block, maxFrames - total));
    sq_audio_block b{ptrs.data(), ch, want, 0};
    uint32_t got = 0;
    if (SQ_FAILED(r->read(&b, &got)) || got == 0) break;
    for (uint32_t k = 0; k < ch; ++k) out[k].insert(out[k].end(), tmp[k].begin(), tmp[k].begin() + got);
    total += got;
    if (got < want) break;
  }
  return out;
}
inline Channels readAll(sq::IAudioFileReader* r, uint32_t ch) { return readFrames(r, ch, ~0ull); }

inline double maxDiff(const Channels& a, const Channels& b, size_t offB = 0, size_t n = ~size_t(0)) {
  double m = 0;
  for (size_t k = 0; k < a.size() && k < b.size(); ++k) {
    size_t len = std::min(a[k].size(), b[k].size() > offB ? b[k].size() - offB : 0);
    len = std::min(len, n);
    for (size_t i = 0; i < len; ++i) m = std::max(m, std::fabs(static_cast<double>(a[k][i]) - b[k][i + offB]));
  }
  return m;
}

inline std::string metaKey(sq::IAudioFileReader* r, uint32_t i) {
  char buf[256];
  uint32_t need = 0;
  if (r->getMetadataKey(i, buf, sizeof buf, &need) != SQ_OK) return {};
  return buf;
}
inline std::vector<uint8_t> metaValue(sq::IAudioFileReader* r, const char* key) {
  uint32_t need = 0;
  if (r->getMetadata(key, nullptr, 0, &need) != SQ_E_BUFFER_TOO_SMALL) return {};
  std::vector<uint8_t> v(need);
  if (r->getMetadata(key, v.data(), need, &need) != SQ_OK) return {};
  return v;
}
inline std::string metaText(sq::IAudioFileReader* r, const char* key) {
  auto v = metaValue(r, key);
  return std::string(v.begin(), v.end());
}

inline std::vector<uint8_t> streamBytes(sq::IByteStream* s) {
  return static_cast<sq::MemoryStream*>(s)->bytes();
}

inline std::vector<uint8_t> loadFile(const std::string& path) {
  auto f = sq::readFile(path);
  if (!f) return {};
  return std::vector<uint8_t>(f->begin(), f->end());
}

// Developer aid: with SQ_CODEC_DUMP_DIR set, tests drop their encoded files there so they can be
// checked with external tools. No effect otherwise.
inline void dump(const char* name, const std::vector<uint8_t>& bytes) {
  std::string dir = sq::cx::envVar("SQ_CODEC_DUMP_DIR");
  if (dir.empty()) return;
  sq_fs_write_file(sq::joinPath(dir, name).c_str(), bytes.data(), bytes.size());
}

// Exercises a reader on hostile input: open, format, metadata, markers, bounded read, seeks.
inline void hammer(CodecEnv& env, const std::vector<uint8_t>& bytes) {
  auto r = env.reader();
  if (!r) return;
  auto s = sq::makeMemoryStream(bytes.data(), bytes.size(), false);
  if (SQ_FAILED(r->openStream(s.get()))) return;
  sq_audio_file_format f{};
  f.struct_size = sizeof f;
  if (SQ_FAILED(r->getFormat(&f))) return;
  for (uint32_t i = 0; i < r->getMetadataCount() && i < 1000; ++i) {
    std::string k = metaKey(r.get(), i);
    (void)metaValue(r.get(), k.c_str());
  }
  for (uint32_t i = 0; i < r->getMarkerCount() && i < 1000; ++i) {
    sq_audio_file_marker m{};
    m.struct_size = sizeof m;
    r->getMarker(i, &m);
  }
  if (f.channels == 0 || f.channels > 64) return;
  (void)readFrames(r.get(), f.channels, 200000, 4096);
  if (f.frame_count > 0) {
    r->seek(f.frame_count / 2);
    (void)readFrames(r.get(), f.channels, 5000);
  }
  r->seek(0);
  (void)readFrames(r.get(), f.channels, 3000);
}

// Truncations at many points, random byte flips, random header garbage. Must not crash.
inline void fuzz(CodecEnv& env, const std::vector<uint8_t>& valid, int iterations = 300, uint32_t seed = 12345) {
  uint32_t st = seed;
  auto rnd = [&]() { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
  const size_t n = valid.size();
  for (size_t cut : {size_t(0), size_t(1), size_t(4), size_t(11), size_t(12), size_t(20), size_t(36), size_t(44), size_t(64), n / 3, n / 2, n - 1})
    if (cut <= n) hammer(env, std::vector<uint8_t>(valid.begin(), valid.begin() + static_cast<ptrdiff_t>(cut)));
  const bool verbose = !sq::cx::envVar("SQ_CODEC_FUZZ_VERBOSE").empty();
  const int scale = std::max(1, std::atoi(sq::cx::envVar("SQ_CODEC_FUZZ_SCALE").c_str()));  // longer local runs
  iterations *= scale;
  for (int it = 0; it < iterations; ++it) {
    if (verbose) { std::printf("fuzz iteration %d\n", it); std::fflush(stdout); }
    std::vector<uint8_t> b = valid;
    const int mode = it % 4;
    const size_t head = std::min<size_t>(n, 512);
    int flips = 1 + static_cast<int>(rnd() % 8);
    for (int f = 0; f < flips && n; ++f) {
      size_t at = (mode == 0 || !head) ? rnd() % n : rnd() % head;  // concentrate on headers
      b[at] = static_cast<uint8_t>(mode == 2 ? 0xFF : rnd());
    }
    if (mode == 3 && n > 16) b.resize(16 + rnd() % (n - 16));
    if (verbose) dump("fuzz_last.bin", b);
    hammer(env, b);
  }
  std::vector<uint8_t> garbage(4096);
  for (int it = 0; it < 20; ++it) {
    for (auto& x : garbage) x = static_cast<uint8_t>(rnd());
    // keep a plausible magic so the parser goes deep
    if (n >= 16) std::copy(valid.begin(), valid.begin() + 16, garbage.begin());
    hammer(env, garbage);
  }
}

// Sample-accurate seek: reads at random positions must equal the sequential decode.
inline bool seekMatches(sq::IAudioFileReader* r, const Channels& full, int probes, double tol, uint32_t seed = 99) {
  uint32_t st = seed;
  auto rnd = [&]() { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
  const uint32_t ch = static_cast<uint32_t>(full.size());
  const size_t frames = full[0].size();
  for (int i = 0; i < probes; ++i) {
    size_t pos = rnd() % (frames - 600);
    if (SQ_FAILED(r->seek(static_cast<sq_sample_time>(pos)))) return false;
    sq_sample_time t = -1;
    r->tell(&t);
    if (t != static_cast<sq_sample_time>(pos)) return false;
    Channels got = readFrames(r, ch, 500, 128);
    if (got[0].size() != 500) return false;
    if (maxDiff(got, full, pos, 500) > tol) {
      std::printf("seek mismatch at %zu: %g\n", pos, maxDiff(got, full, pos, 500));
      return false;
    }
  }
  return true;
}

}  // namespace sqct
