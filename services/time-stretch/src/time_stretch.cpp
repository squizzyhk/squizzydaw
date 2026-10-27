// time-stretch service: ITimeStretcher (interfaces/dsp.h) over the WSOLA / phase-vocoder core.
//
// Settings:  Services.time-stretch.Algorithm = auto | wsola | pv   (read at configure())
// Behaviour notes (beyond dsp.h):
//  - Output frame 0 corresponds to input frame 0. getLatencyFrames() is the input lookahead the
//    algorithm needs before output can be pulled (prime with that many frames for realtime use).
//  - push() returns SQ_E_BUFFER_TOO_SMALL (nothing consumed) if the input FIFO is full; feed at
//    most getRequiredInput() frames per pull to never hit it.
//  - pull() returns SQ_S_FALSE with *produced == 0 once flush()ed and fully drained.
//  - SQ_STRETCH_PRESERVE_FORMANTS is accepted but not implemented (no envelope correction yet).
#include <cstring>

#include "sq/sdk.hpp"
#include "stretch_core.hpp"

namespace {

// Every channel pointer that will be touched is present (RT-safe: no allocation, no locks).
bool channelsPresent(const sq_audio_block* b) {
  if (!b->frames) return true;
  if (!b->channels) return false;
  for (uint32_t c = 0; c < b->channel_count; ++c)
    if (!b->channels[c]) return false;
  return true;
}

sq::stretch::Algorithm algorithmFromSettings() {
  sq::IHostContext* h = sq::host();
  if (!h) return sq::stretch::Algorithm::Auto;
  char buf[32] = {};
  uint32_t needed = 0;
  if (h->getSettings()->getString("Services.time-stretch.Algorithm", buf, sizeof buf, &needed) != SQ_OK)
    return sq::stretch::Algorithm::Auto;
  if (std::strcmp(buf, "wsola") == 0) return sq::stretch::Algorithm::Wsola;
  if (std::strcmp(buf, "pv") == 0) return sq::stretch::Algorithm::PhaseVocoder;
  return sq::stretch::Algorithm::Auto;
}

class TimeStretcher final : public sq::Object<TimeStretcher, sq::ITimeStretcher> {
 public:
  sq_result SQ_CALL configure(const sq_stretch_config* config) override {
    if (!config || config->struct_size < sizeof(sq_stretch_config)) return SQ_E_INVALIDARG;
    sq::stretch::Settings s;
    s.sampleRate = config->sample_rate;
    s.channels = config->channels;
    s.maxBlock = config->max_block_frames;
    s.quality = config->quality;
    s.flags = config->flags;
    s.maxTimeRatio = config->max_time_ratio > 0.0 ? config->max_time_ratio : 4.0;
    s.algorithm = algorithmFromSettings();
    try {
      if (!core_.configure(s)) return SQ_E_INVALIDARG;
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL setRatios(double timeRatio, double pitchSemitones) override {
    if (!core_.configured()) return SQ_E_FAIL;
    if (!(timeRatio > 0.0)) return SQ_E_INVALIDARG;
    core_.setRatios(timeRatio, pitchSemitones);
    return SQ_OK;
  }
  uint32_t SQ_CALL getLatencyFrames() override { return core_.latencyFrames(); }
  uint32_t SQ_CALL getRequiredInput(uint32_t outputFrames) override { return core_.requiredInput(outputFrames); }
  sq_result SQ_CALL push(const sq_audio_block* input) override {
    if (!core_.configured()) return SQ_E_FAIL;
    if (!input || !channelsPresent(input) || input->channel_count != core_.settings().channels)
      return SQ_E_INVALIDARG;
    if (core_.flushed()) return SQ_E_CLOSED;
    return core_.push(input->channels, input->frames) ? SQ_OK : SQ_E_BUFFER_TOO_SMALL;
  }
  sq_result SQ_CALL pull(sq_audio_block* output, uint32_t* produced) override {
    if (produced) *produced = 0;
    if (!core_.configured()) return SQ_E_FAIL;
    if (!output || !produced || !channelsPresent(output) ||
        output->channel_count != core_.settings().channels)
      return SQ_E_INVALIDARG;
    *produced = core_.pull(output->channels, output->frames);
    return (*produced == 0 && core_.drained()) ? SQ_S_FALSE : SQ_OK;
  }
  sq_result SQ_CALL flush() override {
    if (!core_.configured()) return SQ_E_FAIL;
    core_.flush();
    return SQ_OK;
  }
  void SQ_CALL reset() override { core_.reset(); }

 private:
  sq::stretch::Stretcher core_;
};

sq_result onInit(sq::IHostContext* host) {
  sq::ISettings* s = host->getSettings();
  if (!s->has("Services.time-stretch.Algorithm")) s->setString("Services.time-stretch.Algorithm", "auto");
  return SQ_OK;
}

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<TimeStretcher>(sq::dsp::kTimeStretchClassId, sq::dsp::kTimeStretchCategory,
                                  "Squizzy Time Stretch (WSOLA / phase vocoder)"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('T', 'S', 'T', 'R'),
    .name = "time-stretch",
    .description = "Time stretch and pitch shift (WSOLA + phase vocoder)",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = nullptr,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
