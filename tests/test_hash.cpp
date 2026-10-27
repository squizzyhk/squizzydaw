#include <string>

#include "sq/platform.hpp"
#include "sqtest.hpp"

namespace {
std::string sha(const std::string& s) { return sq::sha256Hex(s); }
}  // namespace

SQ_TEST("hash: CRC32 known vectors + incremental") {
  CHECK_EQ(sq_crc32(0, "", 0), 0u);
  CHECK_EQ(sq_crc32(0, "123456789", 9), 0xCBF43926u);
  CHECK_EQ(sq_crc32(0, "The quick brown fox jumps over the lazy dog", 43), 0x414FA339u);
  uint32_t c = sq_crc32(0, "1234", 4);
  c = sq_crc32(c, "56789", 5);
  CHECK_EQ(c, 0xCBF43926u);
}

SQ_TEST("hash: SHA-256 FIPS vectors") {
  CHECK_EQ(sha(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  CHECK_EQ(sha(std::string(1000000, 'a')), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

SQ_TEST("hash: SHA-256 incremental equals one-shot across block boundaries") {
  std::string data;
  for (int i = 0; i < 1000; ++i) data += static_cast<char>(i * 31 + 7);
  std::string oneShot = sha(data);
  for (size_t step : {1u, 3u, 55u, 56u, 63u, 64u, 65u, 127u}) {
    sq_sha256_ctx c;
    sq_sha256_init(&c);
    for (size_t off = 0; off < data.size(); off += step) sq_sha256_update(&c, data.data() + off, std::min(step, data.size() - off));
    uint8_t d[32];
    sq_sha256_final(&c, d);
    char hex[65];
    sq_hex_encode(d, 32, hex);
    CHECK_EQ(std::string(hex), oneShot);
  }
}

SQ_TEST("hash: FNV-1a 64") {
  CHECK_EQ(sq_fnv1a64("", 0), 0xcbf29ce484222325ull);
  CHECK_EQ(sq_fnv1a64("a", 1), 0xaf63dc4c8601ec8cull);
}
