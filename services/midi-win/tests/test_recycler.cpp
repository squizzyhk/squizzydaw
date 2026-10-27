// midi-win: BufferRecycler, the worker that re-queues WinMM sysex buffers so MidiInProc never
// calls a multimedia API (regression for re-adding inside the callback and for a re-add racing
// the port's teardown).
#include <atomic>
#include <thread>

#include "../rt_util.hpp"
#include "sqtest.hpp"

namespace {
struct Probe {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> onPosterThread{0};
  std::atomic<uint64_t> afterShutdown{0};
  std::atomic<bool> shutDown{false};
  std::thread::id poster;
};
void readd(void* user, uint32_t) {
  auto* p = static_cast<Probe*>(user);
  if (std::this_thread::get_id() == p->poster) p->onPosterThread.fetch_add(1);
  if (p->shutDown.load()) p->afterShutdown.fetch_add(1);
  p->calls.fetch_add(1);
}
}  // namespace

SQ_TEST("midi-win recycler: posted buffers are re-added on the worker, never on the posting thread") {
  Probe p;
  sq::midiwin::BufferRecycler r(&readd, &p);
  REQUIRE(r.start());
  std::thread cb([&] {
    p.poster = std::this_thread::get_id();
    for (uint32_t i = 0; i < 1000; ++i) {
      r.post(i & 3u);
      if ((i & 31u) == 0) std::this_thread::yield();
    }
  });
  cb.join();
  for (int i = 0; i < 2000 && p.calls.load() < 1000; ++i) sq_thread_sleep_ms(1);
  r.shutdown();
  CHECK(p.calls.load() > 0);
  CHECK_EQ(p.onPosterThread.load(), uint64_t(0));
}

SQ_TEST("midi-win recycler: no re-add runs after shutdown() returned, even with posts racing it") {
  for (int round = 0; round < 200; ++round) {
    Probe p;
    sq::midiwin::BufferRecycler r(&readd, &p);
    REQUIRE(r.start());
    std::atomic<bool> stop{false};
    std::thread cb([&] {
      p.poster = std::this_thread::get_id();
      while (!stop.load()) r.post(1);
    });
    if (round & 1) sq_thread_sleep_ms(1);
    r.shutdown();
    p.shutDown.store(true);
    const uint64_t n = p.calls.load();
    for (int i = 0; i < 1000; ++i) r.post(2);  // ignored: closing
    sq_thread_sleep_ms(round == 0 ? 60 : 0);   // longer than the worker's poll period once
    stop.store(true);
    cb.join();
    CHECK_EQ(p.afterShutdown.load(), uint64_t(0));
    CHECK_EQ(p.calls.load(), n);
    CHECK(r.closing());
    CHECK(!r.start());
  }
}
