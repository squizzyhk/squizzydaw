#include "sq/formats/core/zip.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <limits>
#include <new>
#include <set>

#include "miniz.h"

namespace sq::formats {

bool ZipArchive::add(std::string_view name, std::string data) {
  if (!isSafeRelativePath(name)) return false;
  for (auto& e : entries) {
    if (e.name == name) {
      e.data = std::move(data);
      return true;
    }
  }
  entries.push_back(FileEntry{std::string(name), std::move(data)});
  return true;
}

const std::string* ZipArchive::find(std::string_view name) const {
  for (const auto& e : entries)
    if (e.name == name) return &e.data;
  return nullptr;
}

std::vector<const FileEntry*> ZipArchive::withPrefix(std::string_view prefix) const {
  std::vector<const FileEntry*> r;
  for (const auto& e : entries)
    if (e.name.compare(0, prefix.size(), prefix) == 0) r.push_back(&e);
  return r;
}

Status zipWrite(const ZipArchive& zip, std::string& out, int level) {
  mz_zip_archive z;
  std::memset(&z, 0, sizeof z);
  if (!mz_zip_writer_init_heap(&z, 0, 0)) return Status::error(SQ_E_OUTOFMEMORY, "zip: cannot initialise writer");
  if (level < 0) level = 0;
  if (level > 10) level = 10;
  MZ_TIME_T stamp = static_cast<MZ_TIME_T>(1704067200);  // fixed: reproducible archives
  std::set<std::string> seen;
  for (const auto& e : zip.entries) {
    if (!isSafeRelativePath(e.name) || !seen.insert(e.name).second) {
      mz_zip_writer_end(&z);
      return Status::error(SQ_E_INVALIDARG, "zip: invalid or duplicate member name '" + e.name + "'");
    }
    if (!mz_zip_writer_add_mem_ex_v2(&z, e.name.c_str(), e.data.data(), e.data.size(), nullptr, 0,
                                     static_cast<mz_uint>(level), 0, 0, &stamp, nullptr, 0, nullptr, 0)) {
      std::string err = mz_zip_get_error_string(mz_zip_get_last_error(&z));
      mz_zip_writer_end(&z);
      return Status::error(SQ_E_FAIL, "zip: cannot add '" + e.name + "': " + err);
    }
  }
  void* buf = nullptr;
  size_t size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&z, &buf, &size)) {
    mz_zip_writer_end(&z);
    return Status::error(SQ_E_FAIL, "zip: cannot finalise archive");
  }
  out.assign(static_cast<const char*>(buf), size);
  mz_free(buf);  // finalize hands the heap buffer to the caller
  mz_zip_writer_end(&z);
  return {};
}

namespace {

struct Sink {
  std::string* data;
  uint64_t declared;      // central directory uncompressed size
  uint64_t compressed;
  uint64_t entryLimit;
  uint64_t totalLeft;     // budget left for the whole archive
  const ZipLimits* limits;
  const char* failure = nullptr;
  bool oom = false;
};

// miniz write callback: grows the member as real bytes arrive. Returning less than n aborts.
size_t sinkWrite(void* opaque, mz_uint64 ofs, const void* buf, size_t n) {
  auto* s = static_cast<Sink*>(opaque);
  const uint64_t have = s->data->size();
  if (ofs != have) {
    s->failure = "non-sequential data";
    return 0;
  }
  const uint64_t next = have + n;
  if (next > s->declared) {
    s->failure = "member inflates beyond its declared size";
    return 0;
  }
  if (next > s->entryLimit) {
    s->failure = "member too large";
    return 0;
  }
  if (next > s->totalLeft) {
    s->failure = "archive expands beyond the size limit";
    return 0;
  }
  if (s->limits->maxRatio && next > s->limits->ratioFloor &&
      next / (s->compressed ? s->compressed : 1) > s->limits->maxRatio) {
    s->failure = "compression ratio too high (zip bomb?)";
    return 0;
  }
  try {
    s->data->append(static_cast<const char*>(buf), n);
  } catch (...) {  // no exception may cross miniz (C)
    s->oom = true;
    s->failure = "out of memory";
    return 0;
  }
  return n;
}

Status zipReadImpl(std::string_view bytes, ZipArchive& out, const ZipLimits& limits) {
  out.entries.clear();
  mz_zip_archive z;
  std::memset(&z, 0, sizeof z);
  if (!mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0))
    return Status::error(SQ_E_PARSE, std::string("zip: not a valid archive (") +
                                         mz_zip_get_error_string(mz_zip_get_last_error(&z)) + ")");
  struct Closer {
    mz_zip_archive* z;
    ~Closer() { mz_zip_reader_end(z); }
  } closer{&z};
  mz_uint n = mz_zip_reader_get_num_files(&z);
  if (n > limits.maxEntries) return Status::error(SQ_E_PARSE, "zip: too many members");
  uint64_t total = 0;
  std::set<std::string> seen;
  for (mz_uint i = 0; i < n; ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&z, i, &st)) return Status::error(SQ_E_PARSE, "zip: corrupt central directory");
    if (st.m_is_directory) continue;
    std::string name = st.m_filename;
    if (!isSafeRelativePath(name)) return Status::error(SQ_E_PARSE, "zip: unsafe member name '" + name + "'");
    if (!seen.insert(name).second) return Status::error(SQ_E_PARSE, "zip: duplicate member '" + name + "'");
    if (st.m_is_encrypted || !st.m_is_supported)
      return Status::error(SQ_E_UNSUPPORTED, "zip: encrypted or unsupported member '" + name + "'");
    // Declared sizes are only a first filter: the sink enforces every limit on the real output.
    if (st.m_uncomp_size > limits.maxEntrySize) return Status::error(SQ_E_PARSE, "zip: member too large '" + name + "'");
    if (total + st.m_uncomp_size > limits.maxTotalSize) return Status::error(SQ_E_PARSE, "zip: archive expands beyond the size limit");
    if (st.m_uncomp_size > std::numeric_limits<size_t>::max()) return Status::error(SQ_E_PARSE, "zip: member too large");
    if (limits.maxRatio && st.m_uncomp_size > limits.ratioFloor &&
        st.m_uncomp_size / (st.m_comp_size ? st.m_comp_size : 1) > limits.maxRatio)
      return Status::error(SQ_E_PARSE, "zip: compression ratio too high for '" + name + "' (zip bomb?)");
    std::string data;
    // Pre-reserve at most what the compressed bytes can plausibly produce; the rest grows on demand.
    data.reserve(static_cast<size_t>(std::min<uint64_t>(st.m_uncomp_size, std::max<uint64_t>(64 * 1024, st.m_comp_size * 4))));
    Sink sink{&data, st.m_uncomp_size, st.m_comp_size, limits.maxEntrySize, limits.maxTotalSize - total, &limits};
    if (st.m_uncomp_size > 0 && !mz_zip_reader_extract_to_callback(&z, i, &sinkWrite, &sink, 0)) {
      if (sink.oom) return Status::error(SQ_E_OUTOFMEMORY, "zip: out of memory extracting '" + name + "'");
      return Status::error(SQ_E_PARSE, "zip: cannot extract '" + name + "' (" +
                                           (sink.failure ? std::string(sink.failure)
                                                         : std::string(mz_zip_get_error_string(mz_zip_get_last_error(&z)))) +
                                           ")");
    }
    if (data.size() != st.m_uncomp_size) return Status::error(SQ_E_PARSE, "zip: member '" + name + "' is shorter than declared");
    total += data.size();
    data.shrink_to_fit();
    out.entries.push_back(FileEntry{std::move(name), std::move(data)});
  }
  return {};
}

}  // namespace

Status zipRead(std::string_view bytes, ZipArchive& out, const ZipLimits& limits) {
  try {
    Status st = zipReadImpl(bytes, out, limits);
    if (!st) out.entries.clear();
    return st;
  } catch (const std::bad_alloc&) {
    out.entries.clear();
    return Status::error(SQ_E_OUTOFMEMORY, "zip: out of memory");
  } catch (...) {
    out.entries.clear();
    return Status::error(SQ_E_FAIL, "zip: unexpected error");
  }
}

Status zipListNames(std::string_view bytes, std::vector<std::string>& names, uint32_t maxEntries) {
  names.clear();
  try {
    mz_zip_archive z;
    std::memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0)) return Status::error(SQ_E_PARSE, "zip: not a valid archive");
    struct Closer {
      mz_zip_archive* z;
      ~Closer() { mz_zip_reader_end(z); }
    } closer{&z};
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    if (n > maxEntries) return Status::error(SQ_E_PARSE, "zip: too many members");
    for (mz_uint i = 0; i < n; ++i) {
      mz_zip_archive_file_stat st;
      if (!mz_zip_reader_file_stat(&z, i, &st)) return Status::error(SQ_E_PARSE, "zip: corrupt central directory");
      if (!st.m_is_directory) names.emplace_back(st.m_filename);
    }
    return {};
  } catch (...) {
    names.clear();
    return Status::error(SQ_E_OUTOFMEMORY, "zip: out of memory");
  }
}

Status zipWriteFile(const ZipArchive& zip, const std::string& path, int level) {
  std::string bytes;
  SQF_TRY(zipWrite(zip, bytes, level));
  return writeFileBytes(path, bytes);
}

Status zipReadFile(const std::string& path, ZipArchive& out, const ZipLimits& limits) {
  try {
    std::string bytes;
    SQF_TRY(readFileBytes(path, bytes));
    return zipRead(bytes, out, limits);
  } catch (const std::bad_alloc&) {
    out.entries.clear();
    return Status::error(SQ_E_OUTOFMEMORY, "zip: out of memory reading " + path);
  }
}

bool zipProbe(std::string_view head) {
  return head.size() >= 4 && head[0] == 'P' && head[1] == 'K' &&
         ((head[2] == 3 && head[3] == 4) || (head[2] == 5 && head[3] == 6));
}

Status zlibCompress(std::string_view in, std::string& out, int level) {
  if (in.size() > 0xFFFFFFF0u) return Status::error(SQ_E_INVALIDARG, "zlib: input too large");
  mz_ulong cap = mz_compressBound(static_cast<mz_ulong>(in.size()));
  out.assign(static_cast<size_t>(cap), '\0');
  int rc = mz_compress2(reinterpret_cast<unsigned char*>(out.data()), &cap,
                        reinterpret_cast<const unsigned char*>(in.data()), static_cast<mz_ulong>(in.size()), level);
  if (rc != MZ_OK) return Status::error(SQ_E_FAIL, "zlib: compression failed");
  out.resize(static_cast<size_t>(cap));
  return {};
}

Status zlibDecompress(std::string_view in, size_t expectedSize, std::string& out) {
  if (expectedSize > 0xFFFFFFF0u || in.size() > 0xFFFFFFF0u) return Status::error(SQ_E_INVALIDARG, "zlib: input too large");
  // Deflate cannot expand more than ~1032:1: a header claiming more is lying - do not allocate it.
  if (expectedSize / 1100 > in.size() + 64) return Status::error(SQ_E_PARSE, "zlib: declared size impossible for the input");
  try {
    out.assign(expectedSize, '\0');
  } catch (const std::bad_alloc&) {
    return Status::error(SQ_E_OUTOFMEMORY, "zlib: out of memory");
  }
  mz_ulong got = static_cast<mz_ulong>(expectedSize);
  mz_ulong srcLen = static_cast<mz_ulong>(in.size());
  // A one-byte scratch target keeps miniz happy for empty payloads.
  unsigned char scratch = 0;
  unsigned char* dst = expectedSize ? reinterpret_cast<unsigned char*>(out.data()) : &scratch;
  int rc = mz_uncompress2(dst, &got, reinterpret_cast<const unsigned char*>(in.data()), &srcLen);
  if (rc != MZ_OK) return Status::error(SQ_E_PARSE, "zlib: corrupt or truncated stream");
  if (got != expectedSize) return Status::error(SQ_E_PARSE, "zlib: size mismatch");
  if (srcLen != in.size()) return Status::error(SQ_E_PARSE, "zlib: trailing data after stream");
  return {};
}

}  // namespace sq::formats
