// Threads, pool, TLS, locks, text, filesystem, logging, system.
#include <cstring>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "sq/platform.hpp"
#include "sqtest.hpp"
#include "test_util.hpp"

SQ_TEST("thread: create/join, TLS isolation, priority") {
  sq_tls_key key;
  REQUIRE_OK(sq_tls_alloc(&key));
  int mainVal = 1;
  REQUIRE_OK(sq_tls_set(key, &mainVal));
  struct Ctx {
    sq_tls_key key;
    void* seen;
    uint64_t tid;
  } ctx{key, reinterpret_cast<void*>(1), 0};
  sq_thread* t = nullptr;
  REQUIRE_OK(sq_thread_create(
      [](void* p) {
        auto* c = static_cast<Ctx*>(p);
        c->seen = sq_tls_get(c->key);
        c->tid = sq_thread_current_id();
      },
      &ctx, "sq-test-thread", &t));
  sq_thread_join(t);
  CHECK(ctx.seen == nullptr);
  CHECK(ctx.tid != 0 && ctx.tid != sq_thread_current_id());
  CHECK(sq_tls_get(key) == &mainVal);
  sq_tls_free(key);
  CHECK_OK(sq_thread_set_current_priority(SQ_PRIORITY_NORMAL));
  CHECK(sq_cpu_count() >= 1u);
}

SQ_TEST("thread: worker pool runs every job, wait_idle, destroy drains") {
  sq_pool* pool = nullptr;
  REQUIRE_OK(sq_pool_create(4, "sq-test-pool", &pool));
  CHECK_EQ(sq_pool_thread_count(pool), 4u);
  std::atomic<int> sum{0};
  for (int i = 1; i <= 1000; ++i) {
    struct Job {
      static void run(void* p) { static_cast<std::atomic<int>*>(p)->fetch_add(1); }
    };
    REQUIRE_OK(sq_pool_submit(pool, &Job::run, &sum));
  }
  sq_pool_wait_idle(pool);
  CHECK_EQ(sum.load(), 1000);
  for (int i = 0; i < 100; ++i) sq_pool_submit(pool, [](void* p) { static_cast<std::atomic<int>*>(p)->fetch_add(1); }, &sum);
  sq_pool_destroy(pool);
  CHECK_EQ(sum.load(), 1100);
}

SQ_TEST("locks: mutex, recursive mutex, rwlock, event, spinlock") {
  sq_mutex* m = nullptr;
  REQUIRE_OK(sq_mutex_create(SQ_FALSE, &m));
  int counter = 0;
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; ++i)
    ts.emplace_back([&] {
      for (int k = 0; k < 10000; ++k) {
        sq_mutex_lock(m);
        ++counter;
        sq_mutex_unlock(m);
      }
    });
  for (auto& t : ts) t.join();
  CHECK_EQ(counter, 40000);
  sq_mutex_destroy(m);

  sq_mutex* rm = nullptr;
  REQUIRE_OK(sq_mutex_create(SQ_TRUE, &rm));
  sq_mutex_lock(rm);
  CHECK(sq_mutex_trylock(rm));
  sq_mutex_unlock(rm);
  sq_mutex_unlock(rm);
  sq_mutex_destroy(rm);

  sq_rwlock* rw = nullptr;
  REQUIRE_OK(sq_rwlock_create(&rw));
  sq_rwlock_lock_shared(rw);
  sq_rwlock_lock_shared(rw);
  sq_rwlock_unlock_shared(rw);
  sq_rwlock_unlock_shared(rw);
  sq_rwlock_lock(rw);
  sq_rwlock_unlock(rw);
  sq_rwlock_destroy(rw);

  sq_event* e = nullptr;
  REQUIRE_OK(sq_event_create(SQ_FALSE, SQ_FALSE, &e));
  CHECK_EQ(sq_event_wait(e, 10), SQ_E_TIMEOUT);
  std::thread setter([&] { sq_event_set(e); });
  CHECK_OK(sq_event_wait(e, 5000));
  setter.join();
  CHECK_EQ(sq_event_wait(e, 0), SQ_E_TIMEOUT);  // auto-reset
  sq_event_destroy(e);

  sq_spinlock sl{0};
  int spun = 0;
  std::vector<std::thread> st;
  for (int i = 0; i < 4; ++i)
    st.emplace_back([&] {
      for (int k = 0; k < 5000; ++k) {
        sq_spin_lock(&sl);
        ++spun;
        sq_spin_unlock(&sl);
      }
    });
  for (auto& t : st) t.join();
  CHECK_EQ(spun, 20000);
  volatile int32_t a = 5;
  CHECK_EQ(sq_atomic_add32(&a, 3), 8);
  CHECK(sq_atomic_cas32(&a, 8, 1));
  CHECK_EQ(sq_atomic_load32(&a), 1);
}

SQ_TEST("text: UTF-8 <-> UTF-16 incl. surrogates and invalid input") {
  const char* s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8E\xB5";  // a é € 🎵
  uint16_t w[16];
  size_t n = 0;
  REQUIRE_OK(sq_utf8_to_utf16(s, std::strlen(s), w, 16, &n));
  REQUIRE_EQ(n, size_t(5));
  CHECK_EQ(w[0], 'a');
  CHECK_EQ(w[1], 0xE9);
  CHECK_EQ(w[2], 0x20AC);
  CHECK_EQ(w[3], 0xD83C);
  CHECK_EQ(w[4], 0xDFB5);
  char back[32];
  size_t bn = 0;
  REQUIRE_OK(sq_utf16_to_utf8(w, n, back, sizeof back, &bn));
  CHECK_EQ(std::string(back, bn), std::string(s));
  CHECK_EQ(sq_utf8_to_utf16(s, std::strlen(s), w, 3, &n), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(n, size_t(5));
  CHECK(sq_utf8_validate(s, std::strlen(s)));
  CHECK(!sq_utf8_validate("\xC0\xAF", 2));       // overlong
  CHECK(!sq_utf8_validate("\xED\xA0\x80", 3));   // surrogate
  CHECK(!sq_utf8_validate("\xF4\x90\x80\x80", 4));  // > U+10FFFF
  CHECK(!sq_utf8_validate("\xE2\x82", 2));       // truncated
  REQUIRE_OK(sq_utf8_to_utf16("x\xFFy", 3, w, 16, &n));
  CHECK_EQ(n, size_t(3));
  CHECK_EQ(w[1], 0xFFFD);
  uint16_t lone[] = {0xD800, 'z'};
  REQUIRE_OK(sq_utf16_to_utf8(lone, 2, back, sizeof back, &bn));
  CHECK_EQ(std::string(back, bn), std::string("\xEF\xBF\xBDz"));
  CHECK_EQ(sq_ascii_casecmp("Engine", "eNGINE"), 0);
  CHECK(sq_ascii_casecmp("a", "b") < 0);
}

SQ_TEST("fs: dirs, atomic write/read, list, rename, copy, stream file, unicode paths") {
  std::string dir = sqtest::makeTempDir("fs");
  std::string sub = sq::joinPath(dir, "a/b/\xC3\xA9t\xC3\xA9");  // "été"
  REQUIRE_OK(sq_fs_create_dirs(sub.c_str()));
  CHECK_OK(sq_fs_create_dirs(sub.c_str()));
  CHECK(sq_fs_is_dir(sub.c_str()));
  std::string f = sq::joinPath(sub, "data.bin");
  std::string payload = std::string("bin\0ary", 7) + std::string(70000, 'x');
  REQUIRE_OK(sq::writeFile(f, payload));
  CHECK(sq_fs_is_file(f.c_str()));
  uint64_t size = 0;
  CHECK_OK(sq_fs_file_size(f.c_str(), &size));
  CHECK_EQ(size, uint64_t(payload.size()));
  auto read = sq::readFile(f);
  REQUIRE(read.has_value());
  CHECK(*read == payload);
  int64_t mt = 0;
  CHECK_OK(sq_fs_mtime(f.c_str(), &mt));
  CHECK(mt > 1600000000000ll);

  std::string f2 = sq::joinPath(sub, "copy.bin");
  CHECK_OK(sq_fs_copy_file(f.c_str(), f2.c_str(), SQ_FALSE));
  CHECK_EQ(sq_fs_copy_file(f.c_str(), f2.c_str(), SQ_FALSE), SQ_E_EXISTS);
  std::string f3 = sq::joinPath(sub, "moved.bin");
  CHECK_OK(sq_fs_rename(f2.c_str(), f3.c_str()));
  std::set<std::string> names;
  sq_fs_list_dir(
      sub.c_str(),
      [](void* u, const char* name, sq_bool) -> sq_bool {
        static_cast<std::set<std::string>*>(u)->insert(name);
        return SQ_TRUE;
      },
      &names);
  CHECK((names == std::set<std::string>{"data.bin", "moved.bin"}));

  sq_file* sf = nullptr;
  std::string log = sq::joinPath(dir, "stream.txt");
  REQUIRE_OK(sq_file_open(log.c_str(), SQ_FILE_WRITE | SQ_FILE_CREATE | SQ_FILE_TRUNCATE, &sf));
  CHECK_OK(sq_file_write(sf, "hello", 5));
  sq_file_close(sf);
  REQUIRE_OK(sq_file_open(log.c_str(), SQ_FILE_APPEND, &sf));
  CHECK_OK(sq_file_write(sf, " world", 6));
  sq_file_close(sf);
  REQUIRE_OK(sq_file_open(log.c_str(), SQ_FILE_READ, &sf));
  uint64_t pos = 0;
  CHECK_OK(sq_file_seek(sf, 6, SQ_SEEK_SET, &pos));
  char buf[16] = {};
  size_t got = 0;
  CHECK_OK(sq_file_read(sf, buf, sizeof buf, &got));
  CHECK_EQ(std::string(buf, got), std::string("world"));
  sq_file_close(sf);
  CHECK_EQ(sq_file_open(sq::joinPath(dir, "nope").c_str(), SQ_FILE_READ, &sf), SQ_E_NOTFOUND);

  sq::Buffer abs;
  CHECK_OK(sq_fs_absolute(sq::joinPath(dir, "a/../a/b").c_str(), abs.out()));
  CHECK(abs.str().find("..") == std::string::npos);
  CHECK_OK(sq_fs_remove_all(dir.c_str()));
  CHECK(!sq_fs_exists(dir.c_str()));
  CHECK_EQ(sq_fs_remove(dir.c_str()), SQ_E_NOTFOUND);
}

SQ_TEST("system: special dirs, env, exe path, dylib") {
  CHECK(!sq::specialDir(SQ_DIR_EXE).empty());
  CHECK(!sq::specialDir(SQ_DIR_TEMP).empty());
  CHECK(sq::specialDir(SQ_DIR_USER_CONFIG).find("SquizzyStudio") != std::string::npos);
  sq::Buffer exe;
  CHECK_OK(sq_exe_path(exe.out()));
  CHECK(exe.str().find("sqtest_platform") != std::string::npos);
  CHECK_OK(sq_env_set("SQ_TEST_ENV", "v\xC3\xA9"));
  sq::Buffer v;
  CHECK_OK(sq_env_get("SQ_TEST_ENV", v.out()));
  CHECK_EQ(v.str(), std::string("v\xC3\xA9"));
  CHECK_OK(sq_env_set("SQ_TEST_ENV", nullptr));
  CHECK_EQ(sq_env_get("SQ_TEST_ENV", v.out()), SQ_E_NOTFOUND);
  sq_dylib* lib = nullptr;
  char err[256] = {};
  CHECK(sq_dylib_open(sq::joinPath(sqtest::binDir(), "nope.dll").c_str(), &lib, err, sizeof err) != SQ_OK);
  CHECK(err[0] != '\0');
  CHECK(std::string(sq_result_string(SQ_E_ABI_MISMATCH)).find("ABI") != std::string::npos);
}

SQ_TEST("log: level filter and sinks") {
  struct Rec {
    int count = 0;
    std::string last;
  } rec;
  int32_t id = sq_log_add_sink(
      [](void* u, int32_t, const char* tag, const char* msg) {
        auto* r = static_cast<Rec*>(u);
        ++r->count;
        r->last = std::string(tag) + ":" + msg;
      },
      &rec);
  REQUIRE(id > 0);
  sq_log_set_console(SQ_FALSE);
  int32_t old = sq_log_get_level();
  sq_log_set_level(SQ_LOG_WARN);
  sq_log_printf(SQ_LOG_INFO, "t", "dropped %d", 1);
  sq_log_printf(SQ_LOG_ERROR, "t", "kept %d", 2);
  CHECK_EQ(rec.count, 1);
  CHECK_EQ(rec.last, std::string("t:kept 2"));
  sq_log_remove_sink(id);
  sq_log_write(SQ_LOG_ERROR, "t", "after removal");
  CHECK_EQ(rec.count, 1);
  sq_log_set_level(old);
  sq_log_set_console(SQ_TRUE);
  CHECK_EQ(sq_log_level_from_name("WARN"), SQ_LOG_WARN);
  CHECK_EQ(sq_log_level_from_name("nonsense"), -1);
}
