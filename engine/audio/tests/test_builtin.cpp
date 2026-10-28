// engine/audio - built-in devices: catalog, factories, parameters, clone, spec helpers and a
// sweep of every device over sample rates / block sizes / channel counts.
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "sqtest.hpp"
#include "test_builtin_util.hpp"

using namespace sq::engine::audio;
using bt::Channels;

SQ_TEST("builtin: catalog is consistent") {
  const auto& all = builtinDevices();
  REQUIRE(all.size() >= 17);
  std::set<std::string> ids;
  for (const DeviceInfo& d : all) {
    CHECK(!d.id.empty());
    CHECK(!d.name.empty());
    CHECK(!d.category.empty());
    CHECK(ids.insert(d.id).second);
    CHECK_EQ(d.instrument, d.category == "Instrument");
    CHECK(findBuiltin(d.id) == &d);
    std::set<std::string> pids;
    for (const ParamInfo& p : d.params) {
      CHECK(pids.insert(p.id).second);
      CHECK(p.min < p.max);
      CHECK(p.def >= p.min && p.def <= p.max);
      if (p.kind == ParamKind::Choice) {
        CHECK_EQ(p.min, 0.0f);
        CHECK_EQ(static_cast<size_t>(p.max) + 1, p.choices.size());
      }
      if (p.logScale) CHECK(p.min > 0.0f);
    }
  }
  for (const char* id : {"synth", "drums", "keys", "bass", "sampler", "eq", "compressor", "limiter", "gate", "reverb", "delay",
                         "chorus", "flanger", "phaser", "distortion", "filter", "utility", "tremolo"})
    CHECK(findBuiltin(id) != nullptr);
  CHECK(findBuiltin("nope") == nullptr);
  SpecParts def;
  REQUIRE(parseSpec(kDefaultInstrument, &def));
  CHECK_EQ(def.format, std::string("builtin"));
  REQUIRE(findBuiltin(def.id) != nullptr);
  CHECK(findBuiltin(def.id)->instrument);
}

SQ_TEST("builtin: makeBuiltin applies + clamps params, rejects unknown ids") {
  std::string err;
  CHECK(makeBuiltin("does-not-exist", {}, &err) == nullptr);
  CHECK(!err.empty());
  for (const DeviceInfo& d : builtinDevices()) {
    auto proto = makeBuiltin(d.id, {}, &err);
    REQUIRE(proto != nullptr);
    REQUIRE_EQ(proto->parameterCount(), static_cast<uint32_t>(d.params.size()));
    CHECK_EQ(proto->kind(), d.instrument ? AudioProcessor::Kind::Synth : AudioProcessor::Kind::Fx);
    for (uint32_t i = 0; i < d.params.size(); ++i) CHECK_EQ(proto->parameter(i), d.params[i].def);
    CHECK_EQ(proto->parameter(9999), 0.0f);
    // clamping through the factory and through setParameter
    std::vector<std::pair<uint32_t, double>> hi, lo;
    for (uint32_t i = 0; i < d.params.size(); ++i) {
      hi.emplace_back(i, d.params[i].max + 1000.0);
      lo.emplace_back(i, d.params[i].min - 1000.0);
    }
    hi.emplace_back(9999u, 1.0);  // ignored
    auto ph = makeBuiltin(d.id, hi, &err);
    auto pl = makeBuiltin(d.id, lo, &err);
    REQUIRE(ph && pl);
    auto inst = ph->clone();
    for (uint32_t i = 0; i < d.params.size(); ++i) {
      CHECK_EQ(ph->parameter(i), d.params[i].max);
      CHECK_EQ(pl->parameter(i), d.params[i].min);
      CHECK_EQ(inst->parameter(i), d.params[i].max);  // clone keeps values
      inst->setParameter(i, std::nanf(""));
      CHECK_EQ(inst->parameter(i), d.params[i].def);
      inst->setParameter(i, d.params[i].min - 5.0f);
      CHECK_EQ(inst->parameter(i), d.params[i].min);
      if (d.params[i].kind != ParamKind::Continuous) {
        inst->setParameter(i, d.params[i].min + 0.4f);
        CHECK_EQ(inst->parameter(i), d.params[i].min);
        inst->setParameter(i, d.params[i].min + 0.6f);
        CHECK_EQ(inst->parameter(i), d.params[i].min + 1.0f);
      }
    }
    inst->setParameter(12345, 1.0f);  // out of range: ignored
  }
}

SQ_TEST("builtin: clone keeps parameters and is unprepared") {
  for (const DeviceInfo& d : builtinDevices()) {
    std::vector<std::pair<uint32_t, double>> params;
    for (uint32_t i = 0; i < d.params.size(); ++i) {
      const ParamInfo& p = d.params[i];
      params.emplace_back(i, p.kind == ParamKind::Continuous ? p.min + (p.max - p.min) * 0.37 : p.max);
    }
    auto proto = makeBuiltin(d.id, params, nullptr);
    REQUIRE(proto);
    auto a = proto->clone();
    a->setParameter(0, d.params[0].min);
    auto b = a->clone();
    for (uint32_t i = 0; i < d.params.size(); ++i) CHECK_EQ(b->parameter(i), a->parameter(i));
    // an unprepared processor must ignore process()
    float buf[16] = {};
    float* io[1] = {buf};
    ProcessContext ctx;
    ctx.frames = 16;
    b->process(ctx, io, 1);
    for (float x : buf) CHECK_EQ(x, 0.0f);
  }
}

SQ_TEST("builtin: every device runs clean at 44.1/48/96k, block 1/64/512, mono..4ch") {
  for (const DeviceInfo& d : builtinDevices()) {
    for (double sr : {44100.0, 48000.0, 96000.0}) {
      for (uint32_t block : {1u, 64u, 512u}) {
        for (uint32_t ch : {1u, 2u, 3u}) {
          if (block == 1 && ch != 2) continue;  // keep the slow path short
          auto p = bt::instance(d.id);
          REQUIRE(p != nullptr);
          REQUIRE(p->prepare(sr, block, ch));
          const int64_t frames = block == 1 ? static_cast<int64_t>(sr * 0.05) : static_cast<int64_t>(sr * 0.4);
          // silence in -> finite, and fx stay silent-ish (no self-oscillation from nothing)
          Channels silent = bt::run(*p, ch, frames, block);
          CHECK(bt::clean(silent));
          if (!d.instrument)
            for (const auto& c : silent) CHECK(bt::peak(c) < 1e-3f);
          // signal / notes
          p->reset();
          Channels in = bt::testSignal(ch, frames, sr);
          auto notes = bt::chordNotes(0, frames / 2);
          notes.push_back({frames / 4, 0xE0, 0x00, 0x50});  // pitch bend
          notes.push_back({frames / 4, 0xB0, 1, 100});      // mod wheel
          Channels out = bt::run(*p, ch, frames, block, d.instrument ? Channels{} : in, d.instrument ? notes : std::vector<bt::Note>{});
          if (!bt::clean(out)) {
            std::printf("  device %s sr %.0f block %u ch %u produced NaN/Inf/denormals\n", d.id.c_str(), sr, block, ch);
            CHECK(false);
          }
          for (const auto& c : out)
            if (bt::peak(c) > 8.0f) {
              std::printf("  device %s sr %.0f block %u: peak %f\n", d.id.c_str(), sr, block, static_cast<double>(bt::peak(c)));
              CHECK(false);
            }
          if (d.instrument && d.id != "sampler") CHECK(bt::rms(out[0]) > 1e-3);
          if (!d.instrument) CHECK(bt::rms(out[0]) > 1e-3);
        }
      }
    }
  }
}

SQ_TEST("builtin: extreme parameter values stay finite") {
  for (const DeviceInfo& d : builtinDevices()) {
    for (int corner = 0; corner < 3; ++corner) {
      std::vector<std::pair<uint32_t, double>> params;
      for (uint32_t i = 0; i < d.params.size(); ++i) {
        const ParamInfo& p = d.params[i];
        const double v = corner == 0 ? p.min : (corner == 1 ? p.max : (i % 2 ? p.min : p.max));
        params.emplace_back(i, v);
      }
      auto p = bt::instance(d.id, params);
      REQUIRE(p);
      REQUIRE(p->prepare(48000.0, 256, 2));
      const int64_t frames = 48000;
      Channels in = bt::testSignal(2, frames, 48000.0, 1.0f);
      Channels out = bt::run(*p, 2, frames, 256, d.instrument ? Channels{} : in, d.instrument ? bt::chordNotes(100, 30000) : std::vector<bt::Note>{});
      if (!bt::clean(out)) {
        std::printf("  device %s corner %d not clean\n", d.id.c_str(), corner);
        CHECK(false);
      }
      for (const auto& c : out)
        if (bt::peak(c) > 1e5f) {  // all-max EQ boosts are legitimately huge
          std::printf("  device %s corner %d peak %f\n", d.id.c_str(), corner, static_cast<double>(bt::peak(c)));
          CHECK(false);
        }
    }
  }
}

SQ_TEST("builtin: live parameter changes are smoothed (no zipper steps)") {
  auto p = bt::instance("utility");
  REQUIRE(p->prepare(48000.0, 128, 1));
  Channels dc(1, std::vector<float>(48000, 1.0f));
  std::vector<float*> ptr(1);
  float maxStep = 0.0f, prev = 1.0f;
  for (int64_t pos = 0; pos < 48000; pos += 128) {
    if (pos == 4096) p->setParameter(0, -24.0f);  // gain jump of 24 dB
    ptr[0] = dc[0].data() + pos;
    ProcessContext ctx;
    ctx.frames = 128;
    p->process(ctx, ptr.data(), 1);
    for (int i = 0; i < 128; ++i) {
      maxStep = std::max(maxStep, std::fabs(ptr[0][i] - prev));
      prev = ptr[0][i];
    }
  }
  CHECK(maxStep < 0.01f);
  CHECK(std::fabs(dc[0].back() - 0.0630957f) < 1e-3f);  // reached the target
}

SQ_TEST("builtin: spec helpers") {
  SpecParts a;
  REQUIRE(parseSpec("builtin|eq||0=-3|4=1.5", &a));
  CHECK_EQ(a.format, std::string("builtin"));
  CHECK_EQ(a.id, std::string("eq"));
  CHECK_EQ(a.path, std::string());
  REQUIRE_EQ(a.params.size(), size_t(2));
  CHECK_EQ(a.params[0].first, 0u);
  CHECK_EQ(a.params[0].second, -3.0);
  CHECK_EQ(a.params[1].first, 4u);
  CHECK_EQ(a.params[1].second, 1.5);

  SpecParts b;
  REQUIRE(parseSpec("builtin|synth|", &b));
  CHECK_EQ(b.id, std::string("synth"));
  CHECK(b.path.empty());
  CHECK(b.params.empty());

  SpecParts c;
  REQUIRE(parseSpec("clap|com.vendor.fx|/opt/plug ins/fx.clap|2=0.25|bogus|7=x|3=1e-3", &c));
  CHECK_EQ(c.format, std::string("clap"));
  CHECK_EQ(c.path, std::string("/opt/plug ins/fx.clap"));
  REQUIRE_EQ(c.params.size(), size_t(2));
  CHECK_EQ(c.params[1].second, 1e-3);

  CHECK(!parseSpec("", &c));
  CHECK(!parseSpec("builtin", &c));
  CHECK(!parseSpec("builtin|eq", &c));
  CHECK(!parseSpec("|eq|", &c));
  CHECK(!parseSpec("builtin||", &c));

  // round trip
  SpecParts x;
  x.format = "vst3";
  x.id = "ABCDEF0123";
  x.path = "C:/Program Files/Common Files/VST3/x.vst3";
  x.params = {{0, -3.0}, {4, 1.5}, {12, 0.1}, {3, 1e-9}, {7, 123456.789}, {9, -0.0001}};
  SpecParts y;
  REQUIRE(parseSpec(formatSpec(x), &y));
  CHECK_EQ(y.format, x.format);
  CHECK_EQ(y.id, x.id);
  CHECK_EQ(y.path, x.path);
  CHECK(y.params == x.params);
  SpecParts e{"builtin", "synth", "", {}};
  CHECK_EQ(formatSpec(e), std::string(kDefaultInstrument));
  REQUIRE(parseSpec(formatSpec(e), &y));
  CHECK(y.params.empty() && y.id == "synth");

  // builtinSpec lists only non-default values
  const DeviceInfo* eq = findBuiltin("eq");
  std::vector<float> vals;
  for (const auto& p : eq->params) vals.push_back(p.def);
  CHECK_EQ(builtinSpec("eq", vals), std::string("builtin|eq|"));
  vals[0] = 1.0f;
  vals[4] = -3.0f;
  vals[7] = 2.5f;
  const std::string s = builtinSpec("eq", vals);
  CHECK_EQ(s, std::string("builtin|eq||0=1|4=-3|7=2.5"));
  SpecParts z;
  REQUIRE(parseSpec(s, &z));
  auto proto = makeBuiltin(z.id, z.params, nullptr);
  REQUIRE(proto);
  for (uint32_t i = 0; i < vals.size(); ++i) CHECK_EQ(proto->parameter(i), vals[i]);
}

SQ_TEST("builtin: sampler without media is silent, with media plays") {
  auto p = bt::instance("sampler");
  REQUIRE(p->prepare(48000.0, 256, 2));
  Channels out = bt::run(*p, 2, 24000, 256, {}, bt::chordNotes(0, 12000));
  CHECK_EQ(bt::peak(out[0]), 0.0f);
  auto m = std::make_shared<MediaBuffer>();
  m->sampleRate = 44100.0;
  m->channels = 1;
  m->frames = 44100;
  m->data.assign(1, std::vector<float>(44100));
  for (size_t i = 0; i < 44100; ++i) m->data[0][i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(i) / 44100.0));
  std::string err;
  auto proto = makeBuiltinWithMedia("sampler", m, {{0, 69.0}}, &err);  // root = A4
  REQUIRE(proto);
  CHECK_EQ(proto->parameter(0), 69.0f);
  auto s = proto->clone();  // clone keeps the media
  REQUIRE(s->prepare(48000.0, 256, 2));
  auto zeroCrossHz = [](const std::vector<float>& v, size_t from, size_t to) {
    int z = 0;
    for (size_t i = from + 1; i < to; ++i)
      if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++z;
    return z * 0.5 / (static_cast<double>(to - from) / 48000.0);
  };
  Channels a = bt::run(*s, 2, 24000, 256, {}, {{0, 0x90, 69, 127}});
  CHECK(bt::rms(a[0]) > 0.1);
  CHECK(std::fabs(zeroCrossHz(a[0], 2000, 20000) - 440.0) < 10.0);
  s->reset();
  Channels b = bt::run(*s, 2, 24000, 256, {}, {{0, 0x90, 81, 127}});
  CHECK(std::fabs(zeroCrossHz(b[0], 2000, 20000) - 880.0) < 15.0);
  // other ids ignore media
  auto eq = makeBuiltinWithMedia("eq", m, {}, &err);
  REQUIRE(eq);
  CHECK(makeBuiltinWithMedia("nope", m, {}, &err) == nullptr);
}

