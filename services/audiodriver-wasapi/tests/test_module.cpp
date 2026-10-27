// audiodriver-wasapi through the real loader. One CTest entry per case (see CMakeLists.txt):
//   [enum]      driver info + enumeration (always runs)
//   [shared]    default output, shared mode: configure -> start -> >= 3 callbacks -> stop
//   [exclusive] default output, exclusive mode (SQ_AUDIO_CONFIG_EXCLUSIVE): same, >= 3 callbacks
//   [capture]   an input endpoint, capture only: >= 3 callbacks carrying input blocks
// A case that cannot run on this machine (no endpoint, exclusive mode refused by the device or
// policy, microphone access denied) is reported SKIPPED with the reason: CTest shows it as
// "Skipped", never as passed. Every other failure (configure/start errors on an existing
// endpoint, too few callbacks) fails the case.
#include <atomic>
#include <cstring>
#include <string>

#include "sq/app/module_loader.hpp"
#include "sq/interfaces/audiodevice.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
struct Cb final : sq::Object<Cb, sq::IAudioCallback> {
  std::atomic<int> calls{0}, inputBlocks{0};
  std::atomic<bool> started{false}, stopped{false};
  sq_result SQ_CALL onStart(const sq_stream_format*, const sq_stream_format*, const sq_latency_info*) override {
    started = true;
    return SQ_OK;
  }
  void SQ_CALL process(const sq_audio_io* io) override {
    for (uint32_t c = 0; io->output && c < io->output->channel_count; ++c)
      std::memset(io->output->channels[c], 0, sizeof(float) * io->output->frames);
    if (io->input && io->input->channel_count > 0 && io->input->frames > 0) inputBlocks.fetch_add(1, std::memory_order_relaxed);
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
    std::string mod = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("audiodriver-wasapi") + sq_dylib_suffix());
    REQUIRE(loader.load(mod).ok());
    drv = sq::createInstance<sq::IAudioDriver>(&reg, sq::audiodevice::kWasapiDriverClassId);
    REQUIRE(drv);
    REQUIRE_OK(drv->refreshDevices());
  }
  ~Fixture() {
    drv = nullptr;
    loader.unload("audiodriver-wasapi");
  }
  // First device with `flag` (default output / default input), else the first with channels.
  std::string find(uint32_t defaultFlag, bool input) {
    std::string any;
    sq_audio_device_info info{};
    info.struct_size = sizeof info;
    for (uint32_t i = 0; i < drv->getDeviceCount(); ++i) {
      if (SQ_FAILED(drv->getDeviceInfo(i, &info))) continue;
      const uint32_t ch = input ? info.max_input_channels : info.max_output_channels;
      if (ch == 0) continue;
      if (info.flags & defaultFlag) return info.id;
      if (any.empty()) any = info.id;
    }
    return any;
  }
};

// Streams until >= 3 callbacks (2 s budget) and checks the lifecycle.
void streamThree(sq::IAudioDevice* dev, bool wantInput) {
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  for (int i = 0; i < 200 && cb->calls.load() < 3; ++i) sq_thread_sleep_ms(10);
  CHECK(dev->isRunning());
  REQUIRE_OK(dev->stop());
  CHECK(cb->started.load());
  CHECK(cb->stopped.load());
  std::printf("wasapi: %d callbacks, %d with input\n", cb->calls.load(), cb->inputBlocks.load());
  CHECK(cb->calls.load() >= 3);
  if (wantInput) CHECK(cb->inputBlocks.load() >= 3);
  CHECK(!dev->isRunning());
}

void printConfig(const char* what, sq::IAudioDevice* dev) {
  sq_stream_format in{}, out{};
  in.struct_size = out.struct_size = sizeof in;
  dev->getStreamFormat(&in, &out);
  std::printf("wasapi %s: %.0f Hz, %u frames, out %u ch, in %u ch\n", what, out.channels ? out.sample_rate : in.sample_rate,
              out.channels ? out.max_block_frames : in.max_block_frames, out.channels, in.channels);
}
}  // namespace

SQ_TEST("audiodriver-wasapi [enum]: loads, driver info, enumeration, bad ids rejected") {
  Fixture f;
  sq_audio_driver_info di{};
  di.struct_size = sizeof di;
  REQUIRE_OK(f.drv->getDriverInfo(&di));
  CHECK_EQ(std::string(di.name), std::string("WASAPI"));
  const uint32_t n = f.drv->getDeviceCount();
  std::printf("wasapi: %u endpoint(s)\n", n);
  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  CHECK(f.drv->getDeviceInfo(n, &info) < 0);
  sq::Ptr<sq::IAudioDevice> dev;
  CHECK(f.drv->openDevice("no-such-device", dev.put()) < 0);
  for (uint32_t i = 0; i < n; ++i) {
    REQUIRE_OK(f.drv->getDeviceInfo(i, &info));
    CHECK(info.id[0] != 0);
  }
}

SQ_TEST("audiodriver-wasapi [shared]: default output, shared mode, >= 3 callbacks") {
  Fixture f;
  const std::string id = f.find(SQ_AUDIO_DEVICE_DEFAULT_OUTPUT, false);
  if (id.empty()) SQ_SKIP("no output endpoint on this machine");
  sq::Ptr<sq::IAudioDevice> dev;
  REQUIRE_OK(f.drv->openDevice(id.c_str(), dev.put()));
  uint32_t outs[2] = {0, 1};
  sq_audio_device_config req{};
  req.struct_size = sizeof req;
  req.output_channels = outs;
  req.output_channel_count = 2;
  sq_audio_device_config got{};
  got.struct_size = sizeof got;
  REQUIRE(SQ_SUCCEEDED(dev->configure(&req, &got)));
  printConfig("shared", dev.get());
  streamThree(dev.get(), false);
}

SQ_TEST("audiodriver-wasapi [exclusive]: default output, exclusive mode, >= 3 callbacks") {
  Fixture f;
  const std::string id = f.find(SQ_AUDIO_DEVICE_DEFAULT_OUTPUT, false);
  if (id.empty()) SQ_SKIP("no output endpoint on this machine");
  sq::Ptr<sq::IAudioDevice> dev;
  REQUIRE_OK(f.drv->openDevice(id.c_str(), dev.put()));
  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(dev->getInfo(&info));
  if (!(info.flags & SQ_AUDIO_DEVICE_EXCLUSIVE)) SQ_SKIP("the default output reports no exclusive-mode format");
  uint32_t outs[2] = {0, 1};
  sq_audio_device_config req{};
  req.struct_size = sizeof req;
  req.output_channels = outs;
  req.output_channel_count = info.max_output_channels >= 2 ? 2 : 1;
  req.flags = SQ_AUDIO_CONFIG_EXCLUSIVE;
  sq_audio_device_config got{};
  got.struct_size = sizeof got;
  const sq_result r = dev->configure(&req, &got);
  if (r == SQ_E_BUSY) SQ_SKIP("exclusive mode refused (endpoint in use or exclusive mode disabled by policy)");
  REQUIRE(SQ_SUCCEEDED(r));
  printConfig("exclusive", dev.get());
  streamThree(dev.get(), false);
}

SQ_TEST("audiodriver-wasapi [capture]: input endpoint, capture only, >= 3 callbacks with input") {
  Fixture f;
  const std::string id = f.find(SQ_AUDIO_DEVICE_DEFAULT_INPUT, true);
  if (id.empty()) SQ_SKIP("no capture endpoint on this machine");
  sq::Ptr<sq::IAudioDevice> dev;
  REQUIRE_OK(f.drv->openDevice(id.c_str(), dev.put()));
  uint32_t ins[1] = {0};
  sq_audio_device_config req{};
  req.struct_size = sizeof req;
  req.input_channels = ins;
  req.input_channel_count = 1;
  sq_audio_device_config got{};
  got.struct_size = sizeof got;
  const sq_result r = dev->configure(&req, &got);
  if (r == SQ_E_ACCESS) SQ_SKIP("microphone access denied by the OS privacy settings");
  if (r == SQ_E_BUSY) SQ_SKIP("capture endpoint in exclusive use by another application");
  REQUIRE(SQ_SUCCEEDED(r));
  printConfig("capture", dev.get());
  streamThree(dev.get(), true);
}
