// audiodriver-alsa through the real loader. One CTest entry per case (see CMakeLists.txt):
//   [enum]      driver info + enumeration + bad ids rejected (always runs)
//   [playback]  ALSA "null" PCM, playback only: ~0.5 s of clock-paced callbacks of the
//               configured block size, xrun counter, stop() lifecycle
//   [capture]   "null" PCM, capture only: callbacks carry input blocks
//   [duplex]    "null" PCM, inputs + outputs on one thread with the same period
//   [default]   start() without configure(), odd block size negotiation, restart after stop
// The streaming cases need only libasound and its configuration (no sound card): they are
// Skipped (exit 77) when libasound or the "null" PCM is not available.
#include <atomic>
#include <cstring>
#include <string>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/audiodevice.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
const sq_guid kAlsaDriverClassId = sq::guid("{6542CA4E-3454-49ED-AEAF-4C52D15988B1}");

struct Cb final : sq::Object<Cb, sq::IAudioCallback> {
  std::atomic<int> calls{0}, inputBlocks{0}, wrongSize{0};
  std::atomic<uint32_t> outChannels{0}, inChannels{0};
  std::atomic<int64_t> lastTime{-1};
  std::atomic<bool> timeGap{false};
  uint32_t expectFrames = 0;
  std::atomic<bool> started{false}, stopped{false};
  sq_result SQ_CALL onStart(const sq_stream_format* in, const sq_stream_format* out, const sq_latency_info*) override {
    expectFrames = out->channels ? out->max_block_frames : in->max_block_frames;
    started = true;
    return SQ_OK;
  }
  void SQ_CALL process(const sq_audio_io* io) override {
    uint32_t frames = 0;
    if (io->output) {
      outChannels = io->output->channel_count;
      frames = io->output->frames;
      for (uint32_t c = 0; c < io->output->channel_count; ++c)
        for (uint32_t i = 0; i < io->output->frames; ++i) io->output->channels[c][i] = 0.25f;  // exercise conversion
    }
    if (io->input && io->input->channel_count > 0) {
      inChannels = io->input->channel_count;
      frames = io->input->frames;
      inputBlocks.fetch_add(1, std::memory_order_relaxed);
    }
    if (frames != expectFrames) wrongSize.fetch_add(1);
    const int64_t prev = lastTime.exchange(io->device_time);
    if (prev >= 0 && io->device_time != prev + int64_t(frames) && !(io->flags & SQ_AUDIO_IO_XRUN)) timeGap = true;
    calls.fetch_add(1, std::memory_order_relaxed);
  }
  void SQ_CALL onStop() override { stopped = true; }
};

struct Fixture {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  sq::Ptr<sq::IAudioDriver> drv;
  Fixture() {
    std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("audiodriver-alsa") + sq_dylib_suffix());
    REQUIRE(loader.load(mod).ok());
    drv = sq::createInstance<sq::IAudioDriver>(&reg, kAlsaDriverClassId);
    REQUIRE(drv);
    REQUIRE_OK(drv->refreshDevices());
  }
  ~Fixture() {
    drv = nullptr;
    loader.unload("audiodriver-alsa");
  }
  sq::Ptr<sq::IAudioDevice> openNull() {
    sq::Ptr<sq::IAudioDevice> dev;
    const sq_result r = drv->openDevice("alsa:null", dev.put());
    if (r == SQ_E_UNSUPPORTED) SQ_SKIP("libasound.so.2 not loadable");
    if (SQ_FAILED(r)) SQ_SKIP("ALSA 'null' PCM cannot be opened (no ALSA configuration?)");
    return dev;
  }
};

sq_audio_device_config request(const uint32_t* ins, uint32_t nin, const uint32_t* outs, uint32_t nout, uint32_t block) {
  sq_audio_device_config req{};
  req.struct_size = sizeof req;
  req.sample_rate = 48000;
  req.block_frames = block;
  req.input_channels = nin ? ins : nullptr;
  req.input_channel_count = nin;
  req.output_channels = nout ? outs : nullptr;
  req.output_channel_count = nout;
  return req;
}

// Streams for ~0.5 s; the null PCM is clock-paced, so the count must be close to real time.
void streamHalfSecond(sq::IAudioDevice* dev, uint32_t block, bool wantInput, bool wantOutput) {
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  CHECK(dev->isRunning());
  sq_thread_sleep_ms(500);
  REQUIRE_OK(dev->stop());
  CHECK(!dev->isRunning());
  CHECK(cb->started.load());
  CHECK(cb->stopped.load());
  const int calls = cb->calls.load();
  const int expected = int(0.5 * 48000.0 / block);  // 93 for 256 frames
  std::printf("alsa null: %d callbacks of %u frames in 0.5 s (expected ~%d), %d with input, %llu xrun(s)\n", calls, block, expected,
              cb->inputBlocks.load(), static_cast<unsigned long long>(dev->getXrunCount()));
  CHECK(calls >= expected / 2);      // streamed
  CHECK(calls <= expected * 2 + 4);  // and paced, not spinning
  CHECK_EQ(cb->wrongSize.load(), 0);
  CHECK(!cb->timeGap.load());
  if (wantOutput) CHECK_EQ(cb->outChannels.load(), 2u);
  if (wantInput) {
    CHECK(cb->inputBlocks.load() >= expected / 2);
    CHECK(cb->inChannels.load() >= 1u);
  }
  CHECK_EQ(dev->stop(), SQ_S_FALSE);  // second stop is a no-op
}
}  // namespace

SQ_TEST("audiodriver-alsa [enum]: loads, driver info, enumeration, bad ids rejected") {
  Fixture f;
  sq_audio_driver_info di{};
  di.struct_size = sizeof di;
  REQUIRE_OK(f.drv->getDriverInfo(&di));
  CHECK_EQ(std::string(di.name), std::string("ALSA"));
  const uint32_t n = f.drv->getDeviceCount();
  std::printf("alsa: %u device(s)\n", n);
  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  bool sawNull = false;
  for (uint32_t i = 0; i < n; ++i) {
    REQUIRE_OK(f.drv->getDeviceInfo(i, &info));
    std::printf("  %s\t%s\tout %u in %u, %.0f Hz, blocks %u..%u\n", info.id, info.name, info.max_output_channels, info.max_input_channels,
                info.default_sample_rate, info.min_block_frames, info.max_block_frames);
    CHECK(std::strncmp(info.id, "alsa:", 5) == 0);
    CHECK(info.max_output_channels + info.max_input_channels > 0);
    CHECK(info.default_sample_rate > 0);
    CHECK(info.min_block_frames <= info.default_block_frames && info.default_block_frames <= info.max_block_frames);
    if (std::strcmp(info.id, "alsa:null") == 0) sawNull = true;
  }
  CHECK(f.drv->getDeviceInfo(n, &info) < 0);
  sq::Ptr<sq::IAudioDevice> dev;
  CHECK(f.drv->openDevice("alsa:no-such-pcm-squizzy", dev.put()) < 0);
  CHECK(f.drv->openDevice("", dev.put()) < 0);
  CHECK(f.drv->openDevice(nullptr, dev.put()) < 0);
  if (n > 0 && !sawNull) std::printf("alsa: note: no 'null' hint listed\n");
}

SQ_TEST("audiodriver-alsa [playback]: null PCM, playback only, ~0.5 s of paced callbacks") {
  Fixture f;
  auto dev = f.openNull();
  const uint32_t outs[2] = {0, 1};
  auto req = request(nullptr, 0, outs, 2, 256);
  sq_audio_device_config got{};
  got.struct_size = sizeof got;
  REQUIRE_OK(dev->configure(&req, &got));
  CHECK_EQ(got.block_frames, 256u);
  CHECK_EQ(got.sample_rate, 48000.0);
  CHECK_EQ(got.output_channel_count, 2u);
  CHECK(got.device_format != SQ_SAMPLE_UNKNOWN);
  sq_latency_info lat{};
  lat.struct_size = sizeof lat;
  REQUIRE_OK(dev->getLatency(&lat));
  CHECK(lat.output_frames >= 256u);
  CHECK_EQ(lat.input_frames, 0u);
  sq_stream_format in{}, out{};
  in.struct_size = out.struct_size = sizeof in;
  REQUIRE_OK(dev->getStreamFormat(&in, &out));
  CHECK_EQ(out.channels, 2u);
  CHECK_EQ(in.channels, 0u);
  CHECK_EQ(out.max_block_frames, 256u);
  streamHalfSecond(dev.get(), 256, false, true);
}

SQ_TEST("audiodriver-alsa [capture]: null PCM, capture only, callbacks carry input") {
  Fixture f;
  auto dev = f.openNull();
  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(dev->getInfo(&info));
  if (info.max_input_channels == 0) SQ_SKIP("the null PCM offers no capture here");
  const uint32_t ins[2] = {0, 1};
  auto req = request(ins, 2, nullptr, 0, 128);
  sq_audio_device_config got{};
  got.struct_size = sizeof got;
  REQUIRE_OK(dev->configure(&req, &got));
  CHECK_EQ(got.input_channel_count, 2u);
  sq_latency_info lat{};
  lat.struct_size = sizeof lat;
  REQUIRE_OK(dev->getLatency(&lat));
  CHECK_EQ(lat.input_frames, 128u);
  streamHalfSecond(dev.get(), 128, true, false);
}

SQ_TEST("audiodriver-alsa [duplex]: null PCM, inputs + outputs with the same period") {
  Fixture f;
  auto dev = f.openNull();
  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(dev->getInfo(&info));
  if (info.max_input_channels == 0) SQ_SKIP("the null PCM offers no capture here");
  const uint32_t ins[1] = {0}, outs[2] = {0, 1};
  auto req = request(ins, 1, outs, 2, 256);
  sq_audio_device_config got{};
  got.struct_size = sizeof got;
  REQUIRE_OK(dev->configure(&req, &got));
  CHECK_EQ(got.input_channel_count, 1u);
  CHECK_EQ(got.output_channel_count, 2u);
  streamHalfSecond(dev.get(), 256, true, true);
}

SQ_TEST("audiodriver-alsa [default]: start without configure, odd block size, restart") {
  Fixture f;
  auto dev = f.openNull();
  // Unconfigured: getStreamFormat reports the defaults with SQ_S_FALSE.
  sq_stream_format in{}, out{};
  in.struct_size = out.struct_size = sizeof in;
  CHECK_EQ(dev->getStreamFormat(&in, &out), SQ_S_FALSE);
  CHECK(out.channels >= 1u);
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  for (int i = 0; i < 200 && cb->calls.load() < 3; ++i) sq_thread_sleep_ms(5);
  CHECK(dev->configure(nullptr, nullptr) < 0);
  sq_audio_device_config got{};
  got.struct_size = sizeof got;
  const uint32_t outs[2] = {0, 1};
  auto busy = request(nullptr, 0, outs, 2, 256);
  CHECK_EQ(dev->configure(&busy, &got), SQ_E_BUSY);  // only while stopped
  REQUIRE_OK(dev->stop());
  CHECK(cb->calls.load() >= 3);
  CHECK_EQ(cb->wrongSize.load(), 0);

  // 100 frames: the null PCM grants it exactly; whatever is granted is what the callback sees.
  auto odd = request(nullptr, 0, outs, 2, 100);
  REQUIRE(SQ_SUCCEEDED(dev->configure(&odd, &got)));
  std::printf("alsa null: requested 100 frames, granted %u\n", got.block_frames);
  // Channel indices beyond the device are dropped with SQ_S_FALSE.
  const uint32_t bad[3] = {0, 1, 4096};
  auto partial = request(nullptr, 0, bad, 3, 256);
  CHECK_EQ(dev->configure(&partial, &got), SQ_S_FALSE);
  CHECK_EQ(got.output_channel_count, 2u);
  streamHalfSecond(dev.get(), got.block_frames, false, true);
  // and once more after a full stop
  streamHalfSecond(dev.get(), got.block_frames, false, true);
}
