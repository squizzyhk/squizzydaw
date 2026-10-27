#include <cstring>
#include <string>
#include <thread>

#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
std::string uniqueName(const char* base) {
  sq_guid g{};
  sq_guid_generate(&g);
  return std::string(base) + "-" + std::to_string(sq_process_id()) + "-" + std::to_string(g.data1);
}
}  // namespace

SQ_TEST("ipc: named pipe request/response roundtrip") {
  std::string name = uniqueName("pipe");
  sq_pipe_server* server = nullptr;
  REQUIRE_OK(sq_pipe_listen(name.c_str(), &server));
  sq_pipe_server* dup = nullptr;
  CHECK_EQ(sq_pipe_listen(name.c_str(), &dup), SQ_E_EXISTS);

  sq_result clientResult = SQ_E_FAIL;
  std::string clientGot;
  std::thread client([&] {
    sq_pipe* c = nullptr;
    clientResult = sq_pipe_connect(name.c_str(), 5000, &c);
    if (clientResult != SQ_OK) return;
    const char msg[] = "hello-scanner";
    uint32_t len = sizeof msg - 1;
    sq_pipe_write(c, &len, sizeof len);
    sq_pipe_write(c, msg, len);
    char reply[16] = {};
    if (sq_pipe_read_exact(c, reply, 3, 5000) == SQ_OK) clientGot.assign(reply, 3);
    sq_pipe_close(c);
  });

  sq_pipe* conn = nullptr;
  REQUIRE_OK(sq_pipe_accept(server, 5000, &conn));
  uint32_t len = 0;
  REQUIRE_OK(sq_pipe_read_exact(conn, &len, sizeof len, 5000));
  REQUIRE_EQ(len, 13u);
  std::string body(len, '\0');
  REQUIRE_OK(sq_pipe_read_exact(conn, body.data(), len, 5000));
  CHECK_EQ(body, std::string("hello-scanner"));
  CHECK_OK(sq_pipe_write(conn, "ack", 3));
  client.join();
  CHECK_OK(clientResult);
  CHECK_EQ(clientGot, std::string("ack"));

  // Peer closed: read reports CLOSED, not a hang.
  char b;
  size_t got = 0;
  CHECK_EQ(sq_pipe_read(conn, &b, 1, &got, 2000), SQ_E_CLOSED);
  sq_pipe_close(conn);
  sq_pipe_server_close(server);
}

SQ_TEST("ipc: pipe accept and read time out") {
  std::string name = uniqueName("pipe-timeout");
  sq_pipe_server* server = nullptr;
  REQUIRE_OK(sq_pipe_listen(name.c_str(), &server));
  sq_pipe* conn = nullptr;
  uint64_t t0 = sq_time_monotonic_ns();
  CHECK_EQ(sq_pipe_accept(server, 100, &conn), SQ_E_TIMEOUT);
  CHECK((sq_time_monotonic_ns() - t0) >= 80'000'000ull);

  // A connected peer that never writes -> read timeout, then the pipe is still usable.
  sq_pipe* client = nullptr;
  std::thread t([&] { sq_pipe_connect(name.c_str(), 5000, &client); });
  REQUIRE_OK(sq_pipe_accept(server, 5000, &conn));
  t.join();
  REQUIRE(client != nullptr);
  char buf[4];
  size_t got = 0;
  CHECK_EQ(sq_pipe_read(conn, buf, sizeof buf, &got, 100), SQ_E_TIMEOUT);
  CHECK_OK(sq_pipe_write(client, "x", 1));
  CHECK_OK(sq_pipe_read(conn, buf, sizeof buf, &got, 2000));
  CHECK_EQ(got, size_t(1));
  sq_pipe_close(client);
  sq_pipe_close(conn);
  sq_pipe_server_close(server);
  CHECK_EQ(sq_pipe_connect(uniqueName("nobody").c_str(), 50, &client), SQ_E_TIMEOUT);
  CHECK_EQ(sq_pipe_listen("bad name!", &server), SQ_E_INVALIDARG);
}

SQ_TEST("ipc: shared memory create/open share the same bytes") {
  std::string name = uniqueName("shm");
  sq_shm* a = nullptr;
  REQUIRE_OK(sq_shm_create(name.c_str(), 4096, &a));
  sq_shm* dup = nullptr;
  CHECK_EQ(sq_shm_create(name.c_str(), 4096, &dup), SQ_E_EXISTS);
  CHECK_EQ(sq_shm_size(a), size_t(4096));
  auto* pa = static_cast<unsigned char*>(sq_shm_data(a));
  CHECK_EQ(int(pa[0]), 0);
  CHECK_EQ(int(pa[4095]), 0);
  std::strcpy(reinterpret_cast<char*>(pa), "shared-state");
  pa[4095] = 0x5A;

  sq_shm* b = nullptr;
  REQUIRE_OK(sq_shm_open(name.c_str(), &b));
  CHECK_EQ(sq_shm_size(b), size_t(4096));
  auto* pb = static_cast<unsigned char*>(sq_shm_data(b));
  CHECK(pa != pb);  // distinct mappings
  CHECK_EQ(std::string(reinterpret_cast<char*>(pb)), std::string("shared-state"));
  CHECK_EQ(int(pb[4095]), 0x5A);
  pb[1] = 'H';
  CHECK_EQ(int(pa[1]), int('H'));
  sq_shm_close(b);
  sq_shm_close(a);
  sq_shm* gone = nullptr;
  CHECK(sq_shm_open(uniqueName("never").c_str(), &gone) != SQ_OK);
}

SQ_TEST("ipc: named semaphore post/wait across handles + timeout") {
  std::string name = uniqueName("sem");
  sq_sem* s = nullptr;
  REQUIRE_OK(sq_sem_create(name.c_str(), 0, 10, &s));
  sq_sem* other = nullptr;
  REQUIRE_OK(sq_sem_open(name.c_str(), &other));
  CHECK_EQ(sq_sem_wait(s, 50), SQ_E_TIMEOUT);
  CHECK_OK(sq_sem_post(other, 2));
  CHECK_OK(sq_sem_wait(s, 1000));
  CHECK_OK(sq_sem_wait(s, 1000));
  CHECK_EQ(sq_sem_wait(s, 0), SQ_E_TIMEOUT);

  std::thread poster([&] {
    sq_thread_sleep_ms(30);
    sq_sem_post(other, 1);
  });
  CHECK_OK(sq_sem_wait(s, 5000));
  poster.join();
  sq_sem_close(other);
  sq_sem_close(s);

  sq_sem* local = nullptr;
  REQUIRE_OK(sq_sem_create(nullptr, 1, 1, &local));
  CHECK_OK(sq_sem_wait(local, 0));
  CHECK_EQ(sq_sem_wait(local, 0), SQ_E_TIMEOUT);
  sq_sem_close(local);
}
