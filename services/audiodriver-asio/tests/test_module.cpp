// audiodriver-asio through the real loader. One CTest entry per case (see CMakeLists.txt):
//   [module]  loads, registers the AudioDriver class, status interface; without the SDK: zero
//             devices, a reason naming SQ_ASIO_SDK_DIR, openDevice -> SQ_E_UNSUPPORTED;
//             with the SDK: one device per valid registry entry. Always runs.
//   [stream]  hardware: opens the first ASIO driver, 2 outputs, >= 3 callbacks, stop. Reported
//             SKIPPED (exit 77) when the host is not compiled in or no driver is installed /
//             loadable on this machine - never passed vacuously.
#include <atomic>
#include <cstring>
#include <string>

#include "asio_registry.hpp"
#include "sq/app/module_loader.hpp"
#include "sq/audiodriver-asio/asio_status.h"
#include "sq/interfaces/audiodevice.h"
#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {

struct Cb final : sq::Object<Cb, sq::IAudioCallback> {
  std::atomic<int> calls{0};
  std::atomic<bool> started{false}, stopped{false};
  sq_result SQ_CALL onStart(const sq_stream_format*, const sq_stream_format*, const sq_latency_info*) override {
    started = true;
    return SQ_OK;
  }
  void SQ_CALL process(const sq_audio_io* io) override {
    for (uint32_t c = 0; io->output && c < io->output->channel_count; ++c)
      std::memset(io->output->channels[c], 0, sizeof(float) * io->output->frames);
    calls.fetch_add(1, std::memory_order_relaxed);
  }
  void SQ_CALL onStop() override { stopped = true; }
};

struct ResetListener final : sq::Object<ResetListener, sq::IAudioDeviceListener> {
  std::atomic<int> resets{0};
  void SQ_CALL onDeviceListChanged() override {}
  void SQ_CALL onDeviceReset(sq::IAudioDevice*, int32_t) override { resets++; }
};

struct Fixture {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host{settings, reg};
  sq::app::ModuleLoader loader{host, reg};
  sq::Ptr<sq::IAudioDriver> drv;
  Fixture() {
    const std::string mod =
        sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"), std::string("audiodriver-asio") + sq_dylib_suffix());
    REQUIRE(loader.load(mod).ok());
    drv = sq::createInstance<sq::IAudioDriver>(&reg, sq::audiodevice::kAsioDriverClassId);
    REQUIRE(drv);
    REQUIRE_OK(drv->refreshDevices());
  }
  ~Fixture() {
    drv = nullptr;
    loader.unload("audiodriver-asio");
  }
  std::string status() {
    auto st = drv.as<sq::IAsioDriverStatus>();
    if (!st) return {};
    uint32_t need = 0;
    st->getStatusText(nullptr, 0, &need);
    std::string s(need, '\0');
    if (need == 0 || SQ_FAILED(st->getStatusText(s.data(), need, &need))) return {};
    s.resize(need - 1);
    return s;
  }
};

}  // namespace

SQ_TEST("audiodriver-asio [module]: loads, class registered, devices match the build's licence seam") {
  Fixture f;
  sq_class_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(f.reg.findClass(&sq::audiodevice::kAsioDriverClassId, &ci));
  CHECK_EQ(std::string(ci.category), std::string(sq::audiodevice::kCategory));

  sq_audio_driver_info di{};
  di.struct_size = sizeof di;
  REQUIRE_OK(f.drv->getDriverInfo(&di));
  CHECK_EQ(std::string(di.name), std::string("ASIO"));

  auto st = f.drv.as<sq::IAsioDriverStatus>();
  REQUIRE(st);
  const std::string text = f.status();
  CHECK_EQ(st->isHostCompiled() != 0, SQ_TEST_EXPECT_ASIO_HOST != 0);
  CHECK(!text.empty());
  std::printf("asio status: %s\n", text.c_str());

  auto view = sq::asio::openSystemRegistry();
  const auto installed = sq::asio::enumerateDrivers(*view);
  CHECK_EQ(st->getInstalledDriverCount(), static_cast<uint32_t>(installed.drivers.size()));

  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  if (!st->isHostCompiled()) {
    CHECK_EQ(f.drv->getDeviceCount(), 0u);
    CHECK(f.drv->getDeviceInfo(0, &info) < 0);
    CHECK(text.find("SQ_ASIO_SDK_DIR") != std::string::npos);
    sq::Ptr<sq::IAudioDevice> dev;
    const std::string id = installed.drivers.empty() ? std::string("asio:{00000000-0000-0000-0000-000000000000}")
                                                     : installed.drivers.front().id;
    CHECK_EQ(f.drv->openDevice(id.c_str(), dev.put()), SQ_E_UNSUPPORTED);
    CHECK(!dev);
  } else {
    CHECK_EQ(f.drv->getDeviceCount(), static_cast<uint32_t>(installed.drivers.size()));
    for (uint32_t i = 0; i < f.drv->getDeviceCount(); ++i) {
      REQUIRE_OK(f.drv->getDeviceInfo(i, &info));
      CHECK(std::string(info.id).rfind("asio:{", 0) == 0);
    }
    sq::Ptr<sq::IAudioDevice> dev;
    CHECK_EQ(f.drv->openDevice("asio:{00000000-0000-0000-0000-000000000000}", dev.put()), SQ_E_NOTFOUND);
  }
}

SQ_TEST("audiodriver-asio [stream]: first driver, 2 outputs, >= 3 callbacks") {
  Fixture f;
  auto st = f.drv.as<sq::IAsioDriverStatus>();
  REQUIRE(st);
  if (!st->isHostCompiled()) SQ_SKIP("ASIO host not compiled in (configure with -DSQ_ASIO_SDK_DIR)");
  if (f.drv->getDeviceCount() == 0) SQ_SKIP("no ASIO driver installed on this machine");
  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(f.drv->getDeviceInfo(0, &info));
  sq::Ptr<sq::IAudioDevice> dev;
  const sq_result ro = f.drv->openDevice(info.id, dev.put());
  if (SQ_FAILED(ro)) SQ_SKIP("the first ASIO driver could not be loaded (hardware absent or in use)");
  REQUIRE_OK(dev->getInfo(&info));
  if (info.max_output_channels == 0) SQ_SKIP("the first ASIO driver reports no outputs");
  uint32_t outs[2] = {0, 1};
  sq_audio_device_config req{}, got{};
  req.struct_size = got.struct_size = sizeof req;
  req.output_channels = outs;
  req.output_channel_count = info.max_output_channels >= 2 ? 2 : 1;
  REQUIRE(SQ_SUCCEEDED(dev->configure(&req, &got)));
  auto lis = sq::Ptr<ResetListener>::adopt(new ResetListener());
  REQUIRE_OK(f.drv->addListener(lis.get()));
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  for (int i = 0; i < 300 && cb->calls.load() < 3; ++i) sq_thread_sleep_ms(10);
  CHECK(dev->isRunning());
  REQUIRE_OK(dev->stop());
  f.drv->removeListener(lis.get());
  // A driver whose hardware stream cannot run (endpoint busy, wrapped device missing) starts,
  // delivers nothing and asks the host for a reset: that is this machine, not a host failure.
  if (cb->calls.load() == 0 && lis->resets.load() > 0)
    SQ_SKIP("the driver started but requested a reset without delivering buffers (its device is unavailable)");
  std::printf("asio: %d callbacks at %.0f Hz / %u frames\n", cb->calls.load(), got.sample_rate, got.block_frames);
  CHECK(cb->started.load());
  CHECK(cb->stopped.load());
  CHECK(cb->calls.load() >= 3);
  CHECK(!dev->isRunning());
}
