// audiodriver-asio: device + driver logic against a fake backend (no SDK, no hardware).
// The test thread plays the driver: it calls the sink's bufferSwitch / message callbacks.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asio_convert.hpp"
#include "asio_driver.hpp"
#include "sq/audiodriver-asio/asio_status.h"
#include "sq/sdk.hpp"
#include "sqtest.hpp"

using namespace sq::asio;

namespace {

struct FakeState {
  int32_t inputs = 2, outputs = 2;
  std::vector<SampleType> inTypes{SampleType::Int16LSB, SampleType::Int16LSB};
  std::vector<SampleType> outTypes{SampleType::Int32MSB, SampleType::Int24LSB};
  BufferSizes sizes{32, 2048, 256, -1};
  std::vector<double> rates{44100, 48000};
  double rate = 48000;
  int32_t latIn = 100, latOut = 200;
  bool outputReadySupported = true;
  bool failCreate = false;
  int32_t refuseAbove = 0;  // > 0: createBuffers refuses (without allocating) blocks larger than this
  // observed
  std::atomic<int32_t> maxRequestedBlock{0};
  BackendSink* sink = nullptr;
  std::vector<ChannelRequest> reqs;
  int32_t block = 0;
  std::vector<std::vector<uint8_t>> mem;  // 2 halves per channel
  std::atomic<int> starts{0}, stops{0}, disposes{0}, outputReadyCalls{0}, panels{0};
  std::atomic<bool> destroyed{false};
};

class FakeBackend final : public Backend {
 public:
  explicit FakeBackend(std::shared_ptr<FakeState> s) : s_(std::move(s)) {}
  ~FakeBackend() override { s_->destroyed = true; }
  std::string driverName() override { return "Fake Driver"; }
  sq_result getChannels(int32_t& i, int32_t& o) override {
    i = s_->inputs;
    o = s_->outputs;
    return SQ_OK;
  }
  sq_result getLatencies(int32_t& i, int32_t& o) override {
    i = s_->latIn;
    o = s_->latOut;
    return SQ_OK;
  }
  sq_result getBufferSize(BufferSizes& out) override {
    out = s_->sizes;
    return SQ_OK;
  }
  sq_result canSampleRate(double r) override {
    for (double x : s_->rates)
      if (x == r) return SQ_OK;
    return SQ_E_UNSUPPORTED;
  }
  sq_result getSampleRate(double& r) override {
    r = s_->rate;
    return SQ_OK;
  }
  sq_result setSampleRate(double r) override {
    if (canSampleRate(r) != SQ_OK) return SQ_E_UNSUPPORTED;
    s_->rate = r;
    return SQ_OK;
  }
  sq_result getChannelInfo(bool input, int32_t ch, ChannelInfo& out) override {
    const auto& types = input ? s_->inTypes : s_->outTypes;
    if (ch < 0 || ch >= static_cast<int32_t>(types.size())) return SQ_E_INVALIDARG;
    out.type = types[static_cast<size_t>(ch)];
    out.active = false;
    std::snprintf(out.name, sizeof out.name, "%s", (input && ch == 0) ? "" : (input ? "Line In" : "Main Out"));
    return SQ_OK;
  }
  sq_result createBuffers(const ChannelRequest* reqs, int32_t count, int32_t block, ChannelBuffers* out,
                          BackendSink* sink) override {
    if (s_->failCreate) return SQ_E_FAIL;
    if (block > s_->maxRequestedBlock.load()) s_->maxRequestedBlock = block;
    if (s_->refuseAbove > 0 && block > s_->refuseAbove) return SQ_E_INVALIDARG;
    if (s_->sizes.granularity == -1 && (block & (block - 1)) != 0) return SQ_E_INVALIDARG;
    s_->reqs.assign(reqs, reqs + count);
    s_->block = block;
    s_->sink = sink;
    s_->mem.assign(static_cast<size_t>(count) * 2, {});
    for (int32_t k = 0; k < count; ++k) {
      const auto& types = reqs[k].input ? s_->inTypes : s_->outTypes;
      const uint32_t b = bytesPerSample(types[static_cast<size_t>(reqs[k].channel)]);
      for (int h = 0; h < 2; ++h) {
        auto& m = s_->mem[static_cast<size_t>(k) * 2 + h];
        m.assign(static_cast<size_t>(block) * b, 0xAB);  // garbage until the host writes silence
        out[k].half[h] = m.data();
      }
    }
    return SQ_OK;
  }
  sq_result disposeBuffers() override {
    s_->disposes++;
    s_->sink = nullptr;
    return SQ_OK;
  }
  sq_result start() override {
    s_->starts++;
    return SQ_OK;
  }
  sq_result stop() override {
    s_->stops++;
    return SQ_OK;
  }
  bool supportsOutputReady() override { return s_->outputReadySupported; }
  void outputReady() noexcept override { s_->outputReadyCalls++; }
  sq_result controlPanel() override {
    s_->panels++;
    return SQ_OK;
  }
  std::string lastError() override { return "fake error"; }

 private:
  std::shared_ptr<FakeState> s_;
};

class FakeRegistry final : public RegistryView {
 public:
  bool listSubkeys(std::vector<std::string>& out) override {
    out = {"Fake B", "Fake A"};
    return true;
  }
  bool readString(const std::string& k, const char* v, std::string& out) override {
    if (std::strcmp(v, "CLSID") == 0) {
      out = k == "Fake A" ? "{A0000000-0000-0000-0000-000000000001}" : "{B0000000-0000-0000-0000-000000000002}";
      return true;
    }
    if (std::strcmp(v, "Description") == 0) {
      out = k + " Device";
      return true;
    }
    return false;
  }
};

// Callback: output[c] = input[c] (or a constant when no input), records everything.
struct Cb final : sq::Object<Cb, sq::IAudioCallback> {
  std::atomic<int> starts{0}, stops{0}, calls{0}, xrunFlags{0}, afterStop{0};
  std::atomic<bool> stopped{false};
  sq_stream_format in{}, out{};
  sq_latency_info lat{};
  std::vector<sq_sample_time> times;
  float constant = 0.25f;
  sq_result SQ_CALL onStart(const sq_stream_format* i, const sq_stream_format* o, const sq_latency_info* l) override {
    in = *i;
    out = *o;
    lat = *l;
    starts++;
    stopped = false;
    return SQ_OK;
  }
  void SQ_CALL process(const sq_audio_io* io) override {
    if (stopped) afterStop++;
    calls++;
    if (io->flags & SQ_AUDIO_IO_XRUN) xrunFlags++;
    if (times.size() < 64) times.push_back(io->device_time);
    for (uint32_t c = 0; c < io->output->channel_count; ++c) {
      for (uint32_t f = 0; f < io->output->frames; ++f)
        io->output->channels[c][f] =
            (io->input && c < io->input->channel_count) ? io->input->channels[c][f] : constant;
    }
  }
  void SQ_CALL onStop() override {
    stopped = true;
    stops++;
  }
};

struct Listener final : sq::Object<Listener, sq::IAudioDeviceListener> {
  std::atomic<int> listChanged{0};
  std::atomic<int> resets[4] = {};
  void SQ_CALL onDeviceListChanged() override { listChanged++; }
  void SQ_CALL onDeviceReset(sq::IAudioDevice*, int32_t reason) override {
    if (reason > 0 && reason < 4) resets[reason]++;
  }
  bool waitFor(int reason, int count) {
    for (int i = 0; i < 200; ++i) {
      if (resets[reason].load() >= count) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
};

struct Fixture {
  std::shared_ptr<FakeState> st = std::make_shared<FakeState>();
  sq::Ptr<sq::IAudioDriver> drv;
  std::atomic<int> loads{0};
  bool failLoad = false;
  explicit Fixture(bool withHost = true) {
    DriverConfig cfg;
    cfg.registry = [] { return std::unique_ptr<RegistryView>(new FakeRegistry()); };
    if (withHost) {
      cfg.backends = [this](const DriverEntry&, std::string* err) -> std::unique_ptr<Backend> {
        loads++;
        if (failLoad) {
          *err = "simulated load failure";
          return nullptr;
        }
        return std::make_unique<FakeBackend>(st);
      };
    } else {
      cfg.unavailableReason = "Host not compiled in (test).";
    }
    REQUIRE_OK(createDriverWith(std::move(cfg), &sq::IAudioDriver::iid, drv.putVoid()));
    REQUIRE_OK(drv->refreshDevices());
  }
  sq::Ptr<sq::IAudioDevice> open(const char* id = nullptr) {
    sq::Ptr<sq::IAudioDevice> d;
    REQUIRE_OK(drv->openDevice(id, d.put()));
    return d;
  }
};

sq_result configure(sq::IAudioDevice* d, std::vector<uint32_t> ins, std::vector<uint32_t> outs, double rate = 0,
                    uint32_t block = 0, sq_audio_device_config* actual = nullptr) {
  sq_audio_device_config req{}, got{};
  req.struct_size = got.struct_size = sizeof req;
  req.sample_rate = rate;
  req.block_frames = block;
  req.input_channels = ins.empty() ? nullptr : ins.data();
  req.input_channel_count = static_cast<uint32_t>(ins.size());
  req.output_channels = outs.empty() ? nullptr : outs.data();
  req.output_channel_count = static_cast<uint32_t>(outs.size());
  const sq_result r = d->configure(&req, &got);
  if (actual) *actual = got;
  return r;
}

void putI16(std::vector<uint8_t>& m, uint32_t frame, int16_t v) { std::memcpy(&m[frame * 2], &v, 2); }

}  // namespace

SQ_TEST("asio device: routing of driver messages") {
  CHECK_EQ(routeMessage(msg::kSelectorSupported, msg::kResetRequest).result, 1);
  CHECK_EQ(routeMessage(msg::kSelectorSupported, msg::kResyncRequest).result, 1);
  CHECK_EQ(routeMessage(msg::kSelectorSupported, msg::kSupportsTimeInfo).result, 1);
  CHECK_EQ(routeMessage(msg::kSelectorSupported, msg::kSupportsTimeCode).result, 0);
  CHECK_EQ(routeMessage(msg::kSelectorSupported, 999).result, 0);
  CHECK_EQ(routeMessage(msg::kEngineVersion, 0).result, kHostEngineVersion);
  CHECK_EQ(routeMessage(msg::kResetRequest, 0).resetReason, SQ_AUDIO_RESET_REQUESTED);
  CHECK(!routeMessage(msg::kResetRequest, 0).xrun);
  CHECK_EQ(routeMessage(msg::kResyncRequest, 0).resetReason, SQ_AUDIO_RESET_REQUESTED);
  CHECK(routeMessage(msg::kResyncRequest, 0).xrun);
  CHECK_EQ(routeMessage(msg::kBufferSizeChange, 0).resetReason, SQ_AUDIO_RESET_FORMAT_CHANGED);
  CHECK_EQ(routeMessage(msg::kLatenciesChanged, 0).resetReason, SQ_AUDIO_RESET_FORMAT_CHANGED);
  CHECK_EQ(routeMessage(msg::kSupportsTimeInfo, 0).result, 1);
  CHECK(routeMessage(msg::kOverload, 0).xrun);
  CHECK_EQ(routeMessage(msg::kOverload, 0).resetReason, 0);
  CHECK_EQ(routeMessage(12345, 0).result, 0);
}

SQ_TEST("asio device: block size negotiation") {
  const BufferSizes pow2{32, 2048, 256, -1};
  CHECK_EQ(chooseBlockSize(pow2, 0), 256u);
  CHECK_EQ(chooseBlockSize(pow2, 100), 128u);
  CHECK_EQ(chooseBlockSize(pow2, 96), 128u);  // tie 64/128 -> larger
  CHECK_EQ(chooseBlockSize(pow2, 64), 64u);
  CHECK_EQ(chooseBlockSize(pow2, 1), 32u);
  CHECK_EQ(chooseBlockSize(pow2, 100000), 2048u);
  const BufferSizes step{64, 1024, 192, 32};
  CHECK_EQ(chooseBlockSize(step, 100), 96u);
  CHECK_EQ(chooseBlockSize(step, 112), 128u);
  CHECK_EQ(chooseBlockSize(step, 5000), 1024u);
  const BufferSizes oddStep{100, 250, 100, 60};  // 100,160,220 (250 not on the grid)
  CHECK_EQ(chooseBlockSize(oddStep, 250), 220u);
  const BufferSizes fixed{256, 256, 256, 0};
  CHECK_EQ(chooseBlockSize(fixed, 64), 256u);
  const BufferSizes any{16, 4096, 512, 0};
  CHECK_EQ(chooseBlockSize(any, 300), 300u);

  CHECK(supportedBlockSizes(pow2) == (std::vector<uint32_t>{32, 64, 128, 256, 512, 1024, 2048}));
  CHECK(supportedBlockSizes(step).size() == 31u);
  CHECK(supportedBlockSizes(fixed) == (std::vector<uint32_t>{256}));
  const BufferSizes fine{17, 4000, 300, 1};
  const auto list = supportedBlockSizes(fine);
  CHECK(list.front() == 17u && list.back() == 4000u);
  CHECK(std::find(list.begin(), list.end(), 300u) != list.end());
  CHECK(std::find(list.begin(), list.end(), 256u) != list.end());
  CHECK(list.size() < 40u);
}

SQ_TEST("asio driver: enumeration, info before/after open, ids, single open") {
  Fixture f;
  sq_audio_driver_info di{};
  di.struct_size = sizeof di;
  REQUIRE_OK(f.drv->getDriverInfo(&di));
  CHECK_EQ(std::string(di.name), std::string("ASIO"));
  REQUIRE_EQ(f.drv->getDeviceCount(), 2u);
  sq_audio_device_info info{};
  info.struct_size = sizeof info;
  REQUIRE_OK(f.drv->getDeviceInfo(0, &info));
  CHECK_EQ(std::string(info.name), std::string("Fake A Device"));
  CHECK_EQ(std::string(info.id), std::string("asio:{A0000000-0000-0000-0000-000000000001}"));
  CHECK_EQ(info.max_output_channels, 0u);  // unknown until opened
  CHECK(f.drv->getDeviceInfo(2, &info) < 0);

  sq::Ptr<sq::IAudioDevice> none;
  CHECK_EQ(f.drv->openDevice("asio:{00000000-0000-0000-0000-000000000000}", none.put()), SQ_E_NOTFOUND);

  {
    auto dev = f.open("asio:{A0000000-0000-0000-0000-000000000001}");
    sq_audio_device_info d{};
    d.struct_size = sizeof d;
    REQUIRE_OK(dev->getInfo(&d));
    CHECK_EQ(d.max_input_channels, 2u);
    CHECK_EQ(d.max_output_channels, 2u);
    CHECK_EQ(d.default_sample_rate, 48000.0);
    CHECK_EQ(d.default_block_frames, 256u);
    CHECK_EQ(d.min_block_frames, 32u);
    CHECK_EQ(d.max_block_frames, 2048u);
    CHECK((d.flags & (SQ_AUDIO_DEVICE_INPUT | SQ_AUDIO_DEVICE_OUTPUT | SQ_AUDIO_DEVICE_CONTROL_PANEL)) ==
          (SQ_AUDIO_DEVICE_INPUT | SQ_AUDIO_DEVICE_OUTPUT | SQ_AUDIO_DEVICE_CONTROL_PANEL));
    CHECK_EQ(openDeviceCount(), 1u);
    sq::Ptr<sq::IAudioDevice> second;
    CHECK_EQ(f.drv->openDevice("asio:{B0000000-0000-0000-0000-000000000002}", second.put()), SQ_E_BUSY);
    // the driver now knows the opened device's channel counts
    REQUIRE_OK(f.drv->getDeviceInfo(0, &info));
    CHECK_EQ(info.max_output_channels, 2u);
    CHECK_OK(dev->showControlPanel(nullptr));
    CHECK_EQ(f.st->panels.load(), 1);
  }
  CHECK(f.st->destroyed.load());
  CHECK_EQ(openDeviceCount(), 0u);
  auto def = f.open(nullptr);  // default = first in the list
  sq_audio_device_info d{};
  d.struct_size = sizeof d;
  REQUIRE_OK(def->getInfo(&d));
  CHECK_EQ(std::string(d.id), std::string("asio:{A0000000-0000-0000-0000-000000000001}"));
  def = nullptr;

  f.failLoad = true;
  sq::Ptr<sq::IAudioDevice> bad;
  CHECK_EQ(f.drv->openDevice("asio:{B0000000-0000-0000-0000-000000000002}", bad.put()), SQ_E_FAIL);
  CHECK_EQ(openDeviceCount(), 0u);

  auto st = f.drv.as<sq::IAsioDriverStatus>();
  REQUIRE(st);
  CHECK(st->isHostCompiled());
  CHECK_EQ(st->getInstalledDriverCount(), 2u);
  char buf[512];
  uint32_t need = 0;
  REQUIRE_OK(st->getStatusText(buf, sizeof buf, &need));
  CHECK(std::string(buf).find("2 ASIO driver(s) available") != std::string::npos);
}

SQ_TEST("asio driver: without the host compiled in -> zero devices with a reason") {
  Fixture f(false);
  CHECK_EQ(f.drv->getDeviceCount(), 0u);
  sq::Ptr<sq::IAudioDevice> d;
  CHECK_EQ(f.drv->openDevice("asio:{A0000000-0000-0000-0000-000000000001}", d.put()), SQ_E_UNSUPPORTED);
  CHECK_EQ(f.loads.load(), 0);
  auto st = f.drv.as<sq::IAsioDriverStatus>();
  REQUIRE(st);
  CHECK(!st->isHostCompiled());
  CHECK_EQ(st->getInstalledDriverCount(), 2u);
  uint32_t need = 0;
  CHECK_EQ(st->getStatusText(nullptr, 0, &need), SQ_E_BUFFER_TOO_SMALL);
  std::string text(need, '\0');
  REQUIRE_OK(st->getStatusText(text.data(), need, &need));
  CHECK(text.find("Host not compiled in (test).") == 0);
  CHECK(text.find("2 ASIO driver(s)") != std::string::npos);
}

SQ_TEST("asio device: configure negotiates rate, block size, channels and formats") {
  Fixture f;
  auto dev = f.open();
  sq_audio_device_config got{};
  // exact request
  CHECK_EQ(configure(dev.get(), {0}, {0, 1}, 44100, 128, &got), SQ_OK);
  CHECK_EQ(got.sample_rate, 44100.0);
  CHECK_EQ(got.block_frames, 128u);
  CHECK_EQ(got.input_channel_count, 1u);
  CHECK_EQ(got.output_channel_count, 2u);
  CHECK_EQ(got.device_format, SQ_SAMPLE_I32);
  CHECK_EQ(f.st->rate, 44100.0);
  CHECK_EQ(f.st->block, 128);
  REQUIRE_EQ(f.st->reqs.size(), size_t{3});
  CHECK(f.st->reqs[0].input && f.st->reqs[0].channel == 0);
  CHECK(!f.st->reqs[2].input && f.st->reqs[2].channel == 1);
  // output halves were cleared to silence
  CHECK(f.st->mem[2][0] == 0 && f.st->mem[3][0] == 0 && f.st->mem[5].back() == 0);

  // unsupported rate and off-grid block: granted values differ -> SQ_S_FALSE
  CHECK_EQ(configure(dev.get(), {}, {1}, 96000, 100, &got), SQ_S_FALSE);
  CHECK_EQ(got.sample_rate, 44100.0);
  CHECK_EQ(got.block_frames, 128u);
  CHECK_EQ(got.device_format, SQ_SAMPLE_I24);
  CHECK(f.st->disposes.load() >= 1);  // previous buffers released before re-creating

  sq_stream_format in{}, out{};
  in.struct_size = out.struct_size = sizeof in;
  REQUIRE_OK(dev->getStreamFormat(&in, &out));
  CHECK_EQ(in.channels, 0u);
  CHECK_EQ(out.channels, 1u);
  CHECK_EQ(out.max_block_frames, 128u);
  CHECK_EQ(out.format, SQ_SAMPLE_F32);
  sq_latency_info lat{};
  lat.struct_size = sizeof lat;
  REQUIRE_OK(dev->getLatency(&lat));
  CHECK_EQ(lat.input_frames, 100u);
  CHECK_EQ(lat.output_frames, 200u);
  CHECK_EQ(lat.process_frames, 0u);

  // invalid requests
  CHECK_EQ(configure(dev.get(), {2}, {0}), SQ_E_INVALIDARG);     // no such input
  CHECK_EQ(configure(dev.get(), {}, {0, 0}), SQ_E_INVALIDARG);   // duplicate
  CHECK_EQ(configure(dev.get(), {}, {}), SQ_E_INVALIDARG);       // nothing to open
  f.st->outTypes[1] = static_cast<SampleType>(32);              // a non-PCM type
  CHECK_EQ(configure(dev.get(), {}, {0, 1}), SQ_E_UNSUPPORTED);

  // rates / block sizes / channel names
  double rates[8];
  uint32_t n = 0;
  REQUIRE_OK(dev->getSupportedSampleRates(rates, 8, &n));
  REQUIRE_EQ(n, 2u);
  CHECK(rates[0] == 44100.0 && rates[1] == 48000.0);
  uint32_t sizes[16];
  REQUIRE_OK(dev->getSupportedBlockSizes(sizes, 16, &n));
  CHECK_EQ(n, 7u);
  char name[64];
  uint32_t need = 0;
  REQUIRE_OK(dev->getChannelName(SQ_FALSE, 1, name, sizeof name, &need));
  CHECK_EQ(std::string(name), std::string("Main Out"));
  REQUIRE_OK(dev->getChannelName(SQ_TRUE, 0, name, sizeof name, &need));
  CHECK_EQ(std::string(name), std::string("In 1"));  // driver gave no name
  CHECK_EQ(dev->getChannelName(SQ_TRUE, 5, name, sizeof name, &need), SQ_E_NOTFOUND);
}

SQ_TEST("asio device: buffer switch converts input and output bit-exact, both halves") {
  Fixture f;
  auto dev = f.open();
  REQUIRE_OK(configure(dev.get(), {0, 1}, {0, 1}, 0, 64));
  REQUIRE(f.st->sink != nullptr);
  // mem layout: [in0 h0, in0 h1, in1 h0, in1 h1, out0 h0, out0 h1, out1 h0, out1 h1]
  for (uint32_t i = 0; i < 64; ++i) {
    putI16(f.st->mem[0], i, static_cast<int16_t>(i * 512 - 16384));  // in0 half 0
    putI16(f.st->mem[1], i, static_cast<int16_t>(-static_cast<int>(i) * 3));  // in0 half 1
    putI16(f.st->mem[2], i, 0x4000);                                  // in1 half 0: 0.5
    putI16(f.st->mem[3], i, static_cast<int16_t>(0x8000));            // in1 half 1: -1.0
  }
  // not started: the callback writes silence
  std::memset(f.st->mem[4].data(), 0x77, f.st->mem[4].size());
  f.st->sink->bufferSwitch(0, true, false, 0, false, 0);
  CHECK(f.st->mem[4][0] == 0 && f.st->mem[4].back() == 0);

  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  CHECK(dev->isRunning());
  CHECK_EQ(f.st->starts.load(), 1);
  CHECK_EQ(cb->starts.load(), 1);
  CHECK_EQ(cb->in.channels, 2u);
  CHECK_EQ(cb->out.channels, 2u);
  CHECK_EQ(cb->out.max_block_frames, 64u);
  CHECK_EQ(cb->lat.output_frames, 200u);

  f.st->sink->bufferSwitch(0, true, true, 1000, true, 5);
  f.st->sink->bufferSwitch(1, true, true, 1064, true, 6);
  CHECK_EQ(cb->calls.load(), 2);
  REQUIRE_EQ(cb->times.size(), size_t{2});
  CHECK_EQ(cb->times[0], 0);
  CHECK_EQ(cb->times[1], 64);
  CHECK_EQ(f.st->outputReadyCalls.load(), 2);
  CHECK_EQ(dev->getXrunCount(), 0ull);

  // out0 = Int32MSB of in0; out1 = Int24LSB of in1. Int16 -> float -> Int32 is exact (<< 16).
  uint32_t bad = 0;
  for (uint32_t i = 0; i < 64; ++i) {
    for (int h = 0; h < 2; ++h) {
      const int16_t v = h == 0 ? static_cast<int16_t>(i * 512 - 16384) : static_cast<int16_t>(-static_cast<int>(i) * 3);
      const uint32_t want = static_cast<uint32_t>(static_cast<int32_t>(v) * 65536);
      const uint8_t* p = &f.st->mem[4 + h][i * 4];
      const uint32_t got = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
      if (got != want) ++bad;
      const uint8_t* q = &f.st->mem[6 + h][i * 3];
      const uint32_t want24 = h == 0 ? 0x400000u : 0x800000u;
      if ((q[0] | (uint32_t{q[1]} << 8) | (uint32_t{q[2]} << 16)) != want24) ++bad;
    }
  }
  CHECK_EQ(bad, 0u);

  // a sample-position jump is a dropout: counted and flagged on that buffer
  f.st->sink->bufferSwitch(0, true, true, 1064 + 64 + 64, true, 7);
  CHECK_EQ(dev->getXrunCount(), 1ull);
  CHECK_EQ(cb->xrunFlags.load(), 1);

  REQUIRE_OK(dev->stop());
  CHECK(!dev->isRunning());
  CHECK_EQ(f.st->stops.load(), 1);
  CHECK_EQ(cb->stops.load(), 1);
  const int calls = cb->calls.load();
  f.st->sink->bufferSwitch(1, true, false, 0, false, 0);  // late callback after stop: silence only
  CHECK_EQ(cb->calls.load(), calls);
  CHECK(f.st->mem[5][0] == 0 && f.st->mem[5].back() == 0);
  CHECK_EQ(dev->stop(), SQ_S_FALSE);
}

SQ_TEST("asio device: reset / resync / buffer-size / rate messages reach the listener") {
  Fixture f;
  auto lis = sq::Ptr<Listener>::adopt(new Listener());
  REQUIRE_OK(f.drv->addListener(lis.get()));
  CHECK_EQ(f.drv->addListener(lis.get()), SQ_S_FALSE);
  auto dev = f.open();
  REQUIRE_OK(configure(dev.get(), {}, {0, 1}));
  BackendSink* s = f.st->sink;
  REQUIRE(s != nullptr);
  CHECK_EQ(s->message(msg::kSelectorSupported, msg::kResetRequest), 1);
  CHECK_EQ(s->message(msg::kEngineVersion, 0), kHostEngineVersion);

  CHECK_EQ(s->message(msg::kResetRequest, 0), 1);
  CHECK(lis->waitFor(SQ_AUDIO_RESET_REQUESTED, 1));

  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  CHECK_EQ(s->message(msg::kResyncRequest, 0), 1);
  CHECK(lis->waitFor(SQ_AUDIO_RESET_REQUESTED, 2));
  CHECK_EQ(dev->getXrunCount(), 1ull);
  s->bufferSwitch(0, true, false, 0, false, 0);
  CHECK_EQ(cb->xrunFlags.load(), 1);  // the buffer after a resync is flagged
  s->bufferSwitch(1, true, false, 0, false, 0);
  CHECK_EQ(cb->xrunFlags.load(), 1);  // once

  CHECK_EQ(s->message(msg::kBufferSizeChange, 512), 1);
  CHECK(lis->waitFor(SQ_AUDIO_RESET_FORMAT_CHANGED, 1));
  s->sampleRateChanged(48000.0);  // unchanged: no notification
  s->sampleRateChanged(96000.0);
  CHECK(lis->waitFor(SQ_AUDIO_RESET_FORMAT_CHANGED, 2));
  CHECK_EQ(s->message(msg::kLatenciesChanged, 0), 1);
  CHECK(lis->waitFor(SQ_AUDIO_RESET_FORMAT_CHANGED, 3));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_EQ(lis->resets[SQ_AUDIO_RESET_FORMAT_CHANGED].load(), 3);
  CHECK_EQ(lis->resets[SQ_AUDIO_RESET_REQUESTED].load(), 2);
  CHECK_EQ(lis->resets[SQ_AUDIO_RESET_DEVICE_LOST].load(), 0);
  REQUIRE_OK(dev->stop());

  REQUIRE_OK(f.drv->removeListener(lis.get()));
  CHECK_EQ(f.drv->removeListener(lis.get()), SQ_E_NOTFOUND);
  CHECK_EQ(s->message(msg::kResetRequest, 0), 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_EQ(lis->resets[SQ_AUDIO_RESET_REQUESTED].load(), 2);
}

SQ_TEST("asio device: stop() while a driver thread streams never calls process() after onStop") {
  Fixture f;
  f.st->outputReadySupported = false;
  auto dev = f.open();
  REQUIRE_OK(configure(dev.get(), {0}, {0}, 0, 32));
  BackendSink* s = f.st->sink;
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  std::atomic<bool> quit{false};
  std::thread rt([&] {
    int32_t h = 0;
    while (!quit.load()) {
      s->bufferSwitch(h, true, false, 0, false, 0);
      h ^= 1;
      std::this_thread::yield();
    }
  });
  for (int round = 0; round < 20; ++round) {
    REQUIRE_OK(dev->start(cb.get()));
    for (int i = 0; i < 2000 && cb->calls.load() < (round + 1) * 5; ++i) std::this_thread::sleep_for(std::chrono::microseconds(200));
    REQUIRE_OK(dev->stop());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  quit = true;
  rt.join();
  CHECK_EQ(cb->starts.load(), 20);
  CHECK_EQ(cb->stops.load(), 20);
  CHECK(cb->calls.load() >= 100);
  CHECK_EQ(cb->afterStop.load(), 0);
  CHECK_EQ(f.st->outputReadyCalls.load(), 0);
}

SQ_TEST("asio device: start without configure uses the first two outputs") {
  Fixture f;
  auto dev = f.open();
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  CHECK_EQ(cb->out.channels, 2u);
  CHECK_EQ(cb->in.channels, 0u);
  CHECK_EQ(cb->out.max_block_frames, 256u);
  f.st->sink->bufferSwitch(0, true, false, 0, false, 0);
  // constant 0.25 -> Int32MSB 0x20000000
  CHECK(f.st->mem[0][0] == 0x20 && f.st->mem[0][1] == 0 && f.st->mem[0][3] == 0);
  CHECK_EQ(dev->configure(nullptr, nullptr), SQ_E_INVALIDARG);
  sq_audio_device_config req{}, got{};
  req.struct_size = got.struct_size = sizeof req;
  CHECK_EQ(dev->configure(&req, &got), SQ_E_BUSY);  // only while stopped
  REQUIRE_OK(dev->stop());
  dev = nullptr;  // releasing the device disposes buffers and unloads the driver
  CHECK(f.st->destroyed.load());
}

// ---- adversarial cases (verifier) ---------------------------------------------------------------

namespace {
// Owns the only reference to the device and drops it from inside onDeviceReset.
struct Releaser final : sq::Object<Releaser, sq::IAudioDeviceListener> {
  sq::IAudioDevice* dev = nullptr;  // owned reference
  std::atomic<int> calls{0};
  void SQ_CALL onDeviceListChanged() override {}
  void SQ_CALL onDeviceReset(sq::IAudioDevice*, int32_t) override {
    calls++;
    if (dev) {
      sq::IAudioDevice* d = dev;
      dev = nullptr;
      d->release();  // last reference: the device is destroyed on the notifier thread
    }
  }
};
// Records whether it was ever handed a device that had already been destroyed.
struct Observer final : sq::Object<Observer, sq::IAudioDeviceListener> {
  std::shared_ptr<FakeState> st;
  std::atomic<int> calls{0}, afterDestroy{0};
  void SQ_CALL onDeviceListChanged() override {}
  void SQ_CALL onDeviceReset(sq::IAudioDevice*, int32_t) override {
    calls++;
    if (st->destroyed.load()) afterDestroy++;
  }
};
}  // namespace

SQ_TEST("asio device: listener dropping the last device reference inside onDeviceReset is safe") {
  for (int round = 0; round < 10; ++round) {
    Fixture f;
    auto rel = sq::Ptr<Releaser>::adopt(new Releaser());
    auto obs = sq::Ptr<Observer>::adopt(new Observer());
    obs->st = f.st;
    REQUIRE_OK(f.drv->addListener(rel.get()));
    REQUIRE_OK(f.drv->addListener(obs.get()));
    {
      auto dev = f.open();
      REQUIRE_OK(configure(dev.get(), {}, {0, 1}));
      BackendSink* s = f.st->sink;
      REQUIRE(s != nullptr);
      rel->dev = dev.get();
      rel->dev->addRef();
      dev = nullptr;  // the listener now holds the only reference
      // two reasons in one notifier batch: FORMAT_CHANGED is delivered first, then REQUESTED
      s->message(msg::kBufferSizeChange, 0);
      s->message(msg::kResetRequest, 0);
    }
    for (int i = 0; i < 400 && !f.st->destroyed.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(f.st->destroyed.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(obs->afterDestroy.load(), 0);  // nobody is handed a dead device
    CHECK_EQ(rel->calls.load(), 1);         // delivery stops once the device is gone
    CHECK_EQ(openDeviceCount(), 0u);
    f.drv->removeListener(rel.get());
    f.drv->removeListener(obs.get());
    auto again = f.open();  // the process-wide single-open slot was released
    CHECK(again);
  }
}

SQ_TEST("asio device: absurd driver buffer sizes are rejected before buffers are created") {
  Fixture f;
  f.st->sizes = BufferSizes{0, 0, 1 << 30, 0};  // malformed: min/max invalid, preferred 1G frames
  f.st->refuseAbove = 1 << 20;                    // the fake refuses instead of allocating
  auto dev = f.open();
  sq_audio_device_config got{};
  const sq_result r = configure(dev.get(), {}, {0, 1}, 0, 0, &got);
  CHECK(SQ_FAILED(r));
  CHECK(f.st->maxRequestedBlock.load() <= 65536);  // the host never asked for a giant block
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  CHECK(SQ_FAILED(dev->start(cb.get())));
  CHECK(!dev->isRunning());
  CHECK_EQ(cb->starts.load(), 0);
  // a sane driver again: the device recovers
  f.st->sizes = BufferSizes{32, 2048, 256, -1};
  CHECK_EQ(configure(dev.get(), {}, {0, 1}, 0, 0, &got), SQ_OK);
  CHECK_EQ(got.block_frames, 256u);
  // a huge but well-formed range is capped too
  f.st->sizes = BufferSizes{64, 1 << 30, 1 << 30, 0};
  CHECK(SQ_SUCCEEDED(configure(dev.get(), {}, {0}, 0, 0, &got)));
  CHECK(got.block_frames <= 65536u);
  CHECK(f.st->maxRequestedBlock.load() <= 65536);
}

SQ_TEST("asio device: latency and sample-rate changes are visible to the re-query after the notification") {
  Fixture f;
  auto lis = sq::Ptr<Listener>::adopt(new Listener());
  REQUIRE_OK(f.drv->addListener(lis.get()));
  auto dev = f.open();
  REQUIRE_OK(configure(dev.get(), {0}, {0}));
  BackendSink* s = f.st->sink;
  REQUIRE(s != nullptr);
  sq_latency_info lat{};
  lat.struct_size = sizeof lat;
  REQUIRE_OK(dev->getLatency(&lat));
  CHECK_EQ(lat.output_frames, 200u);
  f.st->latIn = 333;
  f.st->latOut = 444;
  CHECK_EQ(s->message(msg::kLatenciesChanged, 0), 1);
  CHECK(lis->waitFor(SQ_AUDIO_RESET_FORMAT_CHANGED, 1));
  REQUIRE_OK(dev->getLatency(&lat));
  CHECK_EQ(lat.input_frames, 333u);
  CHECK_EQ(lat.output_frames, 444u);

  f.st->rate = 44100;
  s->sampleRateChanged(44100.0);
  CHECK(lis->waitFor(SQ_AUDIO_RESET_FORMAT_CHANGED, 2));
  sq_stream_format in{}, out{};
  in.struct_size = out.struct_size = sizeof in;
  REQUIRE_OK(dev->getStreamFormat(&in, &out));
  CHECK_EQ(out.sample_rate, 44100.0);
  CHECK_EQ(in.sample_rate, 44100.0);
  f.drv->removeListener(lis.get());
}

SQ_TEST("asio device: out-of-range buffer index and garbage message selectors are harmless") {
  Fixture f;
  auto dev = f.open();
  REQUIRE_OK(configure(dev.get(), {0}, {0}, 0, 32));
  BackendSink* s = f.st->sink;
  auto cb = sq::Ptr<Cb>::adopt(new Cb());
  REQUIRE_OK(dev->start(cb.get()));
  s->bufferSwitch(-1, true, false, 0, false, 0);
  s->bufferSwitch(7, false, false, 0, false, 0);
  s->bufferSwitch(INT32_MIN, true, false, 0, false, 0);
  CHECK_EQ(cb->calls.load(), 3);
  for (int32_t sel : {0, -1, 9, 14, 16, 1000, INT32_MIN, INT32_MAX}) CHECK_EQ(s->message(sel, 0), 0);
  CHECK_EQ(s->message(msg::kSelectorSupported, INT32_MIN), 0);
  REQUIRE_OK(dev->stop());
}
