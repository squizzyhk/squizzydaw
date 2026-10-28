// engine/audio - built-in devices never allocate (or free) inside process()/setParameter()/reset().
// Same approach as test_rtsafety.cpp: global operator new/delete are replaced and every call
// made while rt::inRealtimeContext() is true is counted.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "sqtest.hpp"
#include "test_builtin_util.hpp"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // free() pairs with the malloc() above
#endif

namespace {
std::atomic<uint64_t> g_rtAllocs{0};
inline void noteAlloc() noexcept {
  if (sq::engine::audio::rt::inRealtimeContext()) g_rtAllocs.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

void* operator new(std::size_t n) {
  noteAlloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  noteAlloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  noteAlloc();
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  noteAlloc();
  return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept {
  if (p) noteAlloc();
  std::free(p);
}
void operator delete[](void* p) noexcept {
  if (p) noteAlloc();
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  if (p) noteAlloc();
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  if (p) noteAlloc();
  std::free(p);
}

using namespace sq::engine::audio;

SQ_TEST("builtin rt: process/setParameter/reset do not allocate") {
  auto media = std::make_shared<MediaBuffer>();
  media->channels = 2;
  media->frames = 20000;
  media->data.assign(2, std::vector<float>(20000, 0.25f));
  for (const DeviceInfo& d : builtinDevices()) {
    ProcessorProto proto = d.id == "sampler" ? makeBuiltinWithMedia(d.id, media, {{7, 1.0}}, nullptr) : makeBuiltin(d.id, {}, nullptr);
    REQUIRE(proto);
    auto p = proto->clone();
    const uint32_t ch = 2, block = 256;
    REQUIRE(p->prepare(48000.0, block, ch));
    std::vector<std::vector<float>> buf(ch, std::vector<float>(block));
    std::vector<float*> ptr(ch);
    for (uint32_t c = 0; c < ch; ++c) ptr[c] = buf[c].data();
    std::vector<sq_midi_event> ev(32);
    uint32_t rng = 1;
    const uint64_t before = g_rtAllocs.load();
    {
      rt::Scope scope;
      for (int b = 0; b < 200; ++b) {
        for (uint32_t c = 0; c < ch; ++c)
          for (uint32_t i = 0; i < block; ++i) {
            rng = rng * 1664525u + 1013904223u;
            buf[c][i] = static_cast<float>(static_cast<int32_t>(rng)) * (0.5f / 2147483648.0f);
          }
        uint32_t n = 0;
        if (d.instrument) {
          const uint8_t note = static_cast<uint8_t>(36 + (b * 7) % 40);
          ev[n++] = bt::midi(3, 0x90, note, 100);
          ev[n++] = bt::midi(10, 0x90, static_cast<uint8_t>(note + 4), 90);
          if (b % 3 == 0) ev[n++] = bt::midi(100, 0x80, static_cast<uint8_t>(note - 7), 0);
          if (b % 10 == 0) ev[n++] = bt::midi(120, 0xB0, 64, static_cast<uint8_t>(b % 20 == 0 ? 127 : 0));
          if (b % 17 == 0) ev[n++] = bt::midi(130, 0xE0, 0, static_cast<uint8_t>(b % 128));
          if (b % 50 == 49) ev[n++] = bt::midi(200, 0xB0, 123, 0);
        }
        ProcessContext ctx;
        ctx.frames = b % 5 == 0 ? block / 3 : block;
        ctx.midi = n ? ev.data() : nullptr;
        ctx.midiCount = n;
        p->process(ctx, ptr.data(), ch);
        // live parameter automation between blocks
        const ParamInfo& pi = d.params[static_cast<size_t>(b) % d.params.size()];
        p->setParameter(static_cast<uint32_t>(b) % p->parameterCount(), pi.min + (pi.max - pi.min) * static_cast<float>((b * 37) % 100) / 100.0f);
        if (b == 150) p->reset();
        (void)p->latency();
        (void)p->tail();
        (void)p->parameter(0);
      }
    }
    const uint64_t allocs = g_rtAllocs.load() - before;
    if (allocs) std::printf("  %s allocated %llu times on the RT thread\n", d.id.c_str(), static_cast<unsigned long long>(allocs));
    CHECK_EQ(allocs, uint64_t(0));
  }
}
