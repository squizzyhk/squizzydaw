// video-engine through the real module loader.
// The fixture is synthesised here: a 30-frame, 30 fps, 128x96 H.264 MP4 written with the Media
// Foundation sink writer. Frame i has a top half with red = 8*i and a blue bottom half, so the
// decoded content identifies the frame and the orientation. Without an H.264 encoder (e.g. an
// N edition without the Media Feature Pack) the decode cases are reported SKIPPED.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/video.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

namespace {

constexpr uint32_t kW = 128, kH = 96, kFrames = 30, kFps = 30;

struct Fixture {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  bool loaded = false;
  explicit Fixture(const char* ffmpegDir = nullptr) {
    if (ffmpegDir) settings.setString("Services.video-engine.FfmpegDir", ffmpegDir);
    std::string path = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"),
                                    std::string("video-engine") + sq_dylib_suffix());
    loaded = loader.load(path).ok();
  }
  sq::Ptr<sq::IVideoEngine> engine() {
    sq::Ptr<sq::IVideoEngine> e;
    host.queryService(&sq::IVideoEngine::iid, e.putVoid());
    return e;
  }
};

std::string fixtureDir() {
  std::string d = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-video");
  sq_fs_create_dirs(d.c_str());
  return d;
}

#if defined(_WIN32)
// Writes the synthetic clip. Returns "" on success, else why it could not be made.
std::string writeFixture(const std::string& path) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return "CoInitializeEx failed";
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return "MFStartup failed";
  std::string why;
  IMFSinkWriter* writer = nullptr;
  IMFMediaType* outType = nullptr;
  IMFMediaType* inType = nullptr;
  DWORD stream = 0;
  int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring wpath(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), n);
  do {
    if (FAILED(MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, nullptr, &writer))) {
      why = "MFCreateSinkWriterFromURL failed";
      break;
    }
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, 2000000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, kW, kH);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(writer->AddStream(outType, &stream))) {
      why = "no H.264 encoder available";
      break;
    }
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, kW * 4);  // top-down
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, kW, kH);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(writer->SetInputMediaType(stream, inType, nullptr))) {
      why = "encoder does not accept RGB32 input";
      break;
    }
    if (FAILED(writer->BeginWriting())) {
      why = "BeginWriting failed";
      break;
    }
    const LONGLONG dur = 10000000LL / kFps;
    for (uint32_t i = 0; i < kFrames && why.empty(); ++i) {
      IMFMediaBuffer* buf = nullptr;
      IMFSample* sample = nullptr;
      MFCreateMemoryBuffer(kW * kH * 4, &buf);
      BYTE* p = nullptr;
      buf->Lock(&p, nullptr, nullptr);
      for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
          BYTE* px = p + (y * kW + x) * 4;
          if (y < kH / 2) {  // top: red ramps with the frame index, green fixed
            px[0] = 40;
            px[1] = 200;
            px[2] = static_cast<BYTE>(8 * i);
          } else {  // bottom: blue
            px[0] = 230;
            px[1] = 30;
            px[2] = 30;
          }
          px[3] = 0;
        }
      buf->Unlock();
      buf->SetCurrentLength(kW * kH * 4);
      MFCreateSample(&sample);
      sample->AddBuffer(buf);
      sample->SetSampleTime(dur * i);
      sample->SetSampleDuration(dur);
      if (FAILED(writer->WriteSample(stream, sample))) why = "WriteSample failed";
      sample->Release();
      buf->Release();
    }
    if (why.empty() && FAILED(writer->Finalize())) why = "Finalize failed";
  } while (false);
  if (inType) inType->Release();
  if (outType) outType->Release();
  if (writer) writer->Release();
  MFShutdown();
  CoUninitialize();
  return why;
}
#endif

struct Pixel {
  int b, g, r;
};
Pixel at(const std::vector<uint8_t>& f, uint32_t x, uint32_t y) {
  const uint8_t* p = f.data() + (y * kW + x) * 4;
  return {p[0], p[1], p[2]};
}

// Shared decode checks for one backend on the synthetic clip.
void checkDecode(Fixture& f, const char* backend) {
  std::string clip = sq::joinPath(fixtureDir(), std::string("ramp-") + backend + ".mp4");
  sq_fs_remove(clip.c_str());
#if defined(_WIN32)
  std::string why = writeFixture(clip);
#else
  std::string why = "the fixture writer needs Media Foundation";
#endif
  if (!why.empty()) SQ_SKIP("cannot synthesise the H.264 fixture: " + why);

  auto eng = f.engine();
  REQUIRE(eng);
  sq::Ptr<sq::IVideoReader> rd;
  REQUIRE_OK(eng->openFile(clip.c_str(), backend, rd.put()));
  REQUIRE(rd);
  sq_video_info info{};
  REQUIRE_OK(rd->getInfo(&info));
  CHECK_EQ(info.width, kW);
  CHECK_EQ(info.height, kH);
  CHECK_EQ(info.pixel_format, uint32_t(SQ_PIXEL_BGRA8));
  CHECK_EQ(std::string(info.codec), std::string("h264"));
  CHECK_EQ(std::string(info.backend), std::string(backend));
  CHECK_EQ(info.frame_rate_num * 1u, kFps * info.frame_rate_den);
  CHECK(std::llabs(info.duration_ns - 1000000000LL) < 50000000LL);
  CHECK_EQ(info.has_audio, SQ_FALSE);

  sq_video_frame fr{};
  CHECK_EQ(rd->readFrame(nullptr, 0, &fr), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(fr.width, kW);
  CHECK_EQ(fr.stride, kW * 4);

  std::vector<uint8_t> buf(kW * kH * 4);
  std::vector<int64_t> stamps;
  int badColor = 0;
  for (;;) {
    sq_result r = rd->readFrame(buf.data(), static_cast<uint32_t>(buf.size()), &fr);
    if (r == SQ_S_FALSE) break;
    REQUIRE_OK(r);
    CHECK_EQ(fr.index, uint64_t(stamps.size()));
    stamps.push_back(fr.timestamp_ns);
    Pixel top = at(buf, kW / 2, kH / 4), bottom = at(buf, kW / 2, 3 * kH / 4);
    const int expectRed = 8 * static_cast<int>(stamps.size() - 1);
    if (std::abs(top.r - expectRed) > 30 || top.g < 150 || bottom.b < 180 || bottom.r > 80) ++badColor;
    CHECK_EQ(buf[3], uint8_t(255));
  }
  CHECK_EQ(stamps.size(), size_t(kFrames));
  CHECK_EQ(badColor, 0);  // right frame order and right orientation (top is not the blue half)
  for (size_t i = 0; i < stamps.size(); ++i)
    CHECK(std::llabs(stamps[i] - static_cast<int64_t>(i) * 1000000000LL / kFps) < 2000000LL);
  CHECK_EQ(rd->readFrame(buf.data(), static_cast<uint32_t>(buf.size()), &fr), SQ_S_FALSE);

  // Seek into the middle of frame 15 (500 ms .. 533 ms): the first frame back is frame 15.
  REQUIRE_OK(rd->seek(510000000LL));
  REQUIRE_OK(rd->readFrame(buf.data(), static_cast<uint32_t>(buf.size()), &fr));
  CHECK_EQ(fr.index, uint64_t(0));
  CHECK(std::llabs(fr.timestamp_ns - 500000000LL) < 2000000LL);
  CHECK(std::abs(at(buf, kW / 2, kH / 4).r - 8 * 15) <= 30);
  REQUIRE_OK(rd->readFrame(buf.data(), static_cast<uint32_t>(buf.size()), &fr));
  CHECK(std::llabs(fr.timestamp_ns - 533333333LL) < 2000000LL);
  // Back to the start, and past the end.
  REQUIRE_OK(rd->seek(0));
  REQUIRE_OK(rd->readFrame(buf.data(), static_cast<uint32_t>(buf.size()), &fr));
  CHECK(fr.timestamp_ns < 2000000LL);
  REQUIRE_OK(rd->seek(5000000000LL));
  CHECK_EQ(rd->readFrame(buf.data(), static_cast<uint32_t>(buf.size()), &fr), SQ_S_FALSE);

  // While a reader is alive the module is busy (unload rolled back); afterwards it unloads.
  CHECK_EQ(f.loader.unload("video-engine"), SQ_E_BUSY);
  REQUIRE_OK(rd->seek(0));
  CHECK_OK(rd->readFrame(buf.data(), static_cast<uint32_t>(buf.size()), &fr));  // still decoding after the rollback
  rd.reset();
  eng.reset();
  CHECK_OK(f.loader.unload("video-engine"));
  sq_fs_remove(clip.c_str());
}

}  // namespace

SQ_TEST("video-engine [core]: Media Foundation decodes frames with timestamps, seeks accurately") {
#if !defined(_WIN32)
  SQ_SKIP("Media Foundation backend is Windows-only");
#else
  Fixture f;
  REQUIRE(f.loaded);
  checkDecode(f, "mediafoundation");
#endif
}

// Separate CTest entry: Skipped unless the module was built with SQ_FFMPEG_DIR (its runtime DLLs
// then come from SQ_TEST_FFMPEG_BIN).
SQ_TEST("video-engine [ffmpeg]: the LGPL ffmpeg backend decodes the same clip") {
#if !defined(SQ_TEST_FFMPEG_BIN)
  SQ_SKIP("ffmpeg backend not built (configure with -DSQ_FFMPEG_DIR=...)");
#else
  Fixture f(SQ_TEST_FFMPEG_BIN);
  REQUIRE(f.loaded);
  auto eng = f.engine();
  REQUIRE(eng);
  bool available = false;
  for (uint32_t i = 0; i < eng->getBackendCount(); ++i) {
    sq_video_backend_info bi{};
    REQUIRE_OK(eng->getBackendInfo(i, &bi));
    if (std::string(bi.name) == "ffmpeg") {
      available = bi.available != SQ_FALSE;
      if (!available) SQ_SKIP(std::string("ffmpeg runtime unavailable: ") + bi.detail);
    }
  }
  REQUIRE(available);
  eng.reset();
  checkDecode(f, "ffmpeg");
#endif
}

SQ_TEST("video-engine [core]: backends, argument and error paths") {
  Fixture f;
  REQUIRE(f.loaded);
  auto eng = f.engine();
  REQUIRE(eng);
  REQUIRE(eng->getBackendCount() >= 1u);
  bool sawFfmpeg = false;
  for (uint32_t i = 0; i < eng->getBackendCount(); ++i) {
    sq_video_backend_info bi{};
    REQUIRE_OK(eng->getBackendInfo(i, &bi));
    CHECK_EQ(bi.struct_size, uint32_t(sizeof bi));
    CHECK(bi.detail[0] != 0);
    if (std::string(bi.name) == "ffmpeg") sawFfmpeg = true;
#if defined(_WIN32)
    if (std::string(bi.name) == "mediafoundation") CHECK_EQ(bi.available, SQ_TRUE);
#endif
  }
  CHECK(sawFfmpeg);  // the seam is always listed, available or not
  sq_video_backend_info bi{};
  CHECK_EQ(eng->getBackendInfo(99, &bi), SQ_E_NOTFOUND);

  sq::Ptr<sq::IVideoReader> r;
  CHECK_EQ(eng->openFile(nullptr, nullptr, r.put()), SQ_E_INVALIDARG);
  CHECK_EQ(eng->openFile("Z:/definitely/missing.mp4", nullptr, r.put()), SQ_E_NOTFOUND);
  // A file that is not media.
  std::string junk = sq::joinPath(fixtureDir(), "junk.mp4");
  REQUIRE_OK(sq::writeFile(junk, std::string(4096, 'x')));
  sq_result jr = eng->openFile(junk.c_str(), nullptr, r.put());
  CHECK(jr == SQ_E_UNSUPPORTED || jr == SQ_E_PARSE);
  CHECK(!r);
  CHECK_EQ(eng->openFile(junk.c_str(), "no-such-backend", r.put()), SQ_E_INVALIDARG);
  eng.reset();
  CHECK_OK(f.loader.unload("video-engine"));
}

