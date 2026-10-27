// link-ableton: local timeline math, the wait-free publisher, and RT properties of capture().
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#include "backend.hpp"
#include "sqtest.hpp"
#include "timeline.hpp"

// ---- allocation counter (this test binary only) -------------------------------------------
namespace {
std::atomic<long> g_allocs{0};
}
void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace sq::tempo;

namespace {
constexpr uint64_t kSec = 1000000000ull;
bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
}  // namespace

SQ_TEST("tempo: beats advance at bpm/60 per second from the origin") {
  Timeline tl;
  tl.bpm = 120.0;
  tl.timeOrigin = 10 * kSec;
  CHECK(approx(beatAt(tl, 10 * kSec), 0.0));
  CHECK(approx(beatAt(tl, 11 * kSec), 2.0));
  CHECK(approx(beatAt(tl, 9 * kSec), -2.0));  // before the origin
  CHECK(approx(beatAt(tl, 10 * kSec + kSec / 4), 0.5));
  CHECK_EQ(timeAtBeat(tl, 2.0), 11 * kSec);
  CHECK_EQ(timeAtBeat(tl, -2.0), 9 * kSec);
  CHECK_EQ(timeAtBeat(tl, -1000.0), uint64_t(0));  // clamped at host time 0
}

SQ_TEST("tempo: a tempo change keeps the beat continuous") {
  Timeline tl;
  tl.bpm = 120.0;
  tl.timeOrigin = 0;
  Timeline t2 = withTempo(tl, 60.0, 4 * kSec);  // beat 8 at t=4 s
  CHECK(approx(beatAt(t2, 4 * kSec), 8.0));
  CHECK(approx(beatAt(t2, 5 * kSec), 9.0));  // now one beat per second
  CHECK(approx(t2.bpm, 60.0));
  Timeline t3 = withTempo(t2, 5000.0, 5 * kSec);
  CHECK(approx(t3.bpm, 999.0));  // clamped
  CHECK(approx(withTempo(t2, 1.0, 0).bpm, 20.0));
  CHECK(approx(clampTempo(std::nan("")), 120.0));
}

SQ_TEST("tempo: phase wraps into [0, quantum)") {
  CHECK(approx(phaseOf(0.0, 4.0), 0.0));
  CHECK(approx(phaseOf(5.5, 4.0), 1.5));
  CHECK(approx(phaseOf(-0.5, 4.0), 3.5));
  CHECK(approx(phaseOf(8.0, 4.0), 0.0));
  CHECK(approx(phaseOf(3.0, 0.0), 0.0));
  for (int i = -1000; i < 1000; ++i) {
    double p = phaseOf(i * 0.37, 3.0);
    CHECK(p >= 0.0);
    CHECK(p < 3.0);
  }
  CHECK(approx(clampQuantum(0.25), 1.0));
  CHECK(approx(clampQuantum(100.0), 64.0));
}

SQ_TEST("tempo: play state records when it changed") {
  Timeline tl;
  Timeline a = withPlaying(tl, true, 7 * kSec);
  CHECK(a.playing);
  CHECK_EQ(a.playTime, 7 * kSec);
  Timeline b = withPlaying(a, true, 9 * kSec);  // no change: time kept
  CHECK_EQ(b.playTime, 7 * kSec);
  Timeline c = withPlaying(b, false, 10 * kSec);
  CHECK(!c.playing);
  CHECK_EQ(c.playTime, 10 * kSec);
}

SQ_TEST("tempo: local backend projects state and capture never allocates") {
  LocalBackend be(100 * kSec, 90.0);
  be.setQuantum(3.0);
  be.setPlaying(true, 100 * kSec);
  sq_tempo_sync_state st{};
  long before = g_allocs.load();
  bool ok = true;
  for (int i = 0; i < 10000; ++i) ok = be.capture(100 * kSec + static_cast<uint64_t>(i) * 1000000ull, st) && ok;
  long after = g_allocs.load();
  CHECK(ok);
  CHECK_EQ(after - before, 0L);
  CHECK_EQ(st.struct_size, uint32_t(sizeof st));
  REQUIRE(be.capture(102 * kSec, st));
  CHECK(approx(st.bpm, 90.0));
  CHECK(approx(st.beat, 3.0));  // 2 s at 90 bpm
  CHECK(approx(st.phase, 0.0, 1e-9));
  CHECK(approx(st.quantum, 3.0));
  CHECK_EQ(st.playing, SQ_TRUE);
  CHECK_EQ(st.peers, 0u);
  CHECK_EQ(st.host_time_ns, 102 * kSec);
  be.setTempo(180.0, 102 * kSec);
  REQUIRE(be.capture(103 * kSec, st));
  CHECK(approx(st.beat, 6.0));
  CHECK(approx(st.phase, 0.0, 1e-9));
}

SQ_TEST("tempo: readers never observe a torn timeline while a writer hammers it") {
  // Every published timeline satisfies bpm == beatOrigin + 1 == quantum + 2 == timeOrigin (as
  // doubles) so a mix of two versions is detectable.
  StatePublisher pub;
  {
    Timeline t0;
    t0.bpm = 100;
    t0.beatOrigin = 99;
    t0.quantum = 98;
    t0.timeOrigin = 100;
    pub.write(t0);
  }
  std::atomic<bool> stop{false};
  std::atomic<long> torn{0}, reads{0}, failed{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r)
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        Timeline t;
        if (!pub.read(t)) {
          failed.fetch_add(1);
          continue;
        }
        reads.fetch_add(1);
        if (t.beatOrigin != t.bpm - 1 || t.quantum != t.bpm - 2 || static_cast<double>(t.timeOrigin) != t.bpm)
          torn.fetch_add(1);
      }
    });
  for (int i = 0; i < 300000; ++i) {
    Timeline t;
    t.bpm = 100.0 + i;
    t.beatOrigin = t.bpm - 1;
    t.quantum = t.bpm - 2;
    t.timeOrigin = static_cast<uint64_t>(t.bpm);
    pub.write(t);
  }
  stop.store(true);
  for (auto& th : readers) th.join();
  CHECK_EQ(torn.load(), 0L);
  CHECK(reads.load() > 0);
  // Bounded retries may give up under this artificial hammering; they must stay rare.
  CHECK(failed.load() * 100 <= reads.load() + 100);
  Timeline last;
  REQUIRE(pub.read(last));
  CHECK(approx(last.bpm, 100.0 + 299999));
}
