// audiodriver-pulse through the real loader.
//   [enum]    driver info, the default device, bad ids rejected (always runs)
//   [stream]  pulse:default playback for ~0.5 s; Skipped (77) without a reachable server
#include <atomic>
#include <cstring>
#include <string>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/audiodevice.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
const sq_guid kPulseDriverClassId = sq::guid("{10AEBF82-F73D-4072-8710-4325D313567D}");

struct Cb final : sq::Object<Cb, sq::IAudioCallback> {
  std::atomic<int> calls{0};
  sq_result SQ_CALL onStart(const sq_stream_format*, const sq_stream_format*, const sq_latency_info*) override { return SQ_OK; }
  void SQ_CALL process(const sq_audio_io* io) override {
    for (uint32_t c = 0; c < io->output->channel_count; ++c) std::memset(io->output->channels[c], 0, sizeof(float) * io->output->frames);
    calls.fetch_add(1);
  }
  void SQ_CALL onStop() override {}
};

struct Fixture {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  sq::Ptr<sq::IAudioDriver> drv;
  Fixture() {
    std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("audiodriver-pulse") + sq_dylib_suffix());
    REQUIRE(loader.load(mod).ok());
    drv = sq::createInstance<sq::IAudioDriver>(&reg, kPulseDriverClassId);
    REQUIRE(drv);
    REQUIRE_OK(drv->refreshDevices());
  }
  ~Fixture() {
    drv = nullptr;
    loader.unload("audiodriver-pulse");
  }
};
}  // namespace

SQ_TEST("audiodriver-pulse [enum]: driver info, default device, bad ids") {
  Fixture f;
  sq_audio_driver_info di{};
  di.struct_size = sizeof di;
  REQUIRE_OK(f.drv->getDriverInfo(&di));
  CHECK_EQ(std::string(di.name), std::string("Pulse"));
  const uint32_t n = f.drv->getDeviceCount();
  std::printf("pulse: %u device(s)\n", n);
  CHECK(n <= 1u);
  if (n == 1) {
    sq_audio_device_info info{};
    info.struct_size = sizeof info;
    REQUIRE_OK(f.drv->getDeviceInfo(0, &info));
    CHECK_EQ(std::string(info.id), std::string("pulse:default"));
    CHECK(info.flags & SQ_AUDIO_DEVICE_DEFAULT_OUTPUT);
    sq::Ptr<sq::IAudioDevice> dev;
    REQUIRE_OK(f.drv->openDevice("pulse:default", dev.put()));  // no server contact yet
    sq_stream_format in{}, out{};
    in.struct_size = out.struct_size = sizeof in;
    CHECK_EQ(dev->getStreamFormat(&in, &out), SQ_S_FALSE);
    CHECK_EQ(out.channels, 2u);
  }
  sq::Ptr<sq::IAudioDevice> dev;
  CHECK(f.drv->openDevice("alsa:default", dev.put()) < 0);
  CHECK(f.drv->openDevice("pulse:", dev.put()) < 0);
}

SQ_TEST("audiodriver-pulse [stream]: default sink, ~0.5 s of playback callbacks") {
  Fixture f;
  if (f.drv->getDeviceCount() == 0) SQ_SKIP("libpulse-simple.so.0 not loadable");
  sq::Ptr<sq::IAudioDevice> dev;
  REQUIRE_OK(f.drv->openDevice("pulse:default", dev.put()));
  const uint32_t outs[2] = {0, 1};
  sq_audio_device_config req{};
  req.struct_size = sizeof req;
  req.sample_rate = 48000;
  req.block_frames = 256;
  req.output_channels = outs;
  req.output_channel_count = 2;
  sq_audio_device_config got{};
  got.struct_size = sizeof got;
  const sq_result r = dev->configure(&req, &got);
  if (SQ_FAILED(r)) SQ_SKIP("no PulseAudio/PipeWire server reachable");
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  sq_thread_sleep_ms(500);
  REQUIRE_OK(dev->stop());
  std::printf("pulse: %d callbacks\n", cb->calls.load());
  CHECK(cb->calls.load() >= 20);
}
