#include "binary_cache.hpp"

#include "../fs_helper.hpp"
#include "../internal.hpp"
#include "../sqlite_utils.hpp"

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#include <fmt/format.h>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aurora::gl {
namespace {
Module Log("aurora::gl::binary_cache");

// Master switch (no env vars -- standing rule). kForceBinaryCacheOnMesa lets a developer
// exercise the machinery on the desktop Mesa stack despite the format-deserializer hazard.
constexpr bool kProgramBinaryCache = true;
constexpr bool kForceBinaryCacheOnMesa = false;

constexpr int kSchemaVersion = 1;
constexpr size_t kMaxCacheBytes = 48 * 1024 * 1024;
// Two consecutive boots that both crash inside the glProgramBinary loading window -> the cache
// is genuinely poison on this driver; disable it permanently for this db (source-only, safe).
constexpr int kMaxCrashes = 2;

constexpr const char* kDbName = "program_binary_cache.db";
constexpr const char* kSentinelName = "program_binary_cache.loading";

struct Entry {
  GLenum format = 0;
  std::vector<uint8_t> blob;
};

struct WriteOp {
  enum class Kind { Upsert, Erase, ResetPoison } kind = Kind::Upsert;
  uint64_t key = 0;
  GLenum format = 0;
  std::vector<uint8_t> blob;
};

bool g_enabled = false;

std::mutex g_mapMutex;
std::unordered_map<uint64_t, Entry> g_map;

sqlite3* g_db = nullptr;
std::string g_sentinelPath;

std::thread g_writerThread;
std::mutex g_writerMutex;
std::condition_variable g_writerCv;
std::deque<WriteOp> g_writeQueue;
bool g_writerStop = false;

std::atomic<uint32_t> g_hits{0};
std::atomic<uint32_t> g_misses{0};

std::string gl_string(GLenum name) {
  const auto* s = reinterpret_cast<const char*>(gl.GetString(name));
  return s != nullptr ? std::string{s} : std::string{};
}

bool renderer_is_mesa() {
  auto contains = [](std::string s, const char* needle) {
    for (auto& c : s) {
      c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    return s.find(needle) != std::string::npos;
  };
  const auto vendor = gl_string(GL_VENDOR);
  const auto renderer = gl_string(GL_RENDERER);
  return contains(vendor, "mesa") || contains(renderer, "mesa") || contains(renderer, "llvmpipe") ||
         contains(renderer, "gallium") || contains(renderer, "softpipe");
}

// Whole-cache validity fingerprint: any change in the driver, its GLSL compiler, or its binary
// format list invalidates every stored blob. libmali's GL_VERSION carries the driver revision,
// so a firmware update naturally trips this.
std::string compute_fingerprint() {
  std::string fp = gl_string(GL_VENDOR) + "|" + gl_string(GL_RENDERER) + "|" + gl_string(GL_VERSION) + "|" +
                   gl_string(GL_SHADING_LANGUAGE_VERSION) + "|formats=";
  GLint numFormats = 0;
  gl.GetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &numFormats);
  if (numFormats > 0) {
    std::vector<GLint> formats(static_cast<size_t>(numFormats), 0);
    gl.GetIntegerv(GL_PROGRAM_BINARY_FORMATS, formats.data());
    for (const auto f : formats) {
      fmt::format_to(std::back_inserter(fp), "{:x},", static_cast<uint32_t>(f));
    }
  }
  fmt::format_to(std::back_inserter(fp), "|schema={}", kSchemaVersion);
  return fp;
}

// -- meta table helpers (main thread only, at init) -------------------------------------------

std::string meta_get(const char* key) {
  std::string value;
  const auto sql = fmt::format("SELECT v FROM meta WHERE k = '{}'", key);
  sqlite::exec(g_db, sql.c_str(), [&value](int argc, char** argv, char**) {
    if (argc > 0 && argv[0] != nullptr) {
      value = argv[0];
    }
  });
  return value;
}

void meta_set(const char* key, const std::string& value) {
  auto* stmt = static_cast<sqlite3_stmt*>(nullptr);
  if (sqlite3_prepare_v2(g_db, "INSERT INTO meta(k, v) VALUES(?, ?) "
                               "ON CONFLICT(k) DO UPDATE SET v = excluded.v",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    Log.error("meta_set prepare failed: {}", sqlite3_errmsg(g_db));
    return;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    Log.error("meta_set step failed: {}", sqlite3_errmsg(g_db));
  }
  sqlite3_finalize(stmt);
}

void wipe_programs() { sqlite::exec(g_db, "DELETE FROM programs"); }

bool ensure_schema() {
  if (sqlite::exec(g_db, "PRAGMA journal_mode=TRUNCATE; PRAGMA synchronous=NORMAL;") != SQLITE_OK) {
    Log.error("Failed to set pragmas: {}", sqlite3_errmsg(g_db));
    return false;
  }
  // key is UNIQUE but NOT the rowid, so the implicit rowid stays insertion order -> FIFO eviction.
  const auto* schema = R"(
CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);
CREATE TABLE IF NOT EXISTS programs(
  key INTEGER NOT NULL UNIQUE,
  format INTEGER NOT NULL,
  binary BLOB NOT NULL
);)";
  if (sqlite::exec(g_db, schema) != SQLITE_OK) {
    Log.error("Failed to create schema: {}", sqlite3_errmsg(g_db));
    return false;
  }
  return true;
}

// Load every row (oldest first) into g_map, evicting the oldest entries when over the byte cap.
size_t load_map_and_evict() {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(g_db, "SELECT key, format, binary FROM programs ORDER BY rowid ASC", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    Log.error("Failed to prepare load statement: {}", sqlite3_errmsg(g_db));
    return 0;
  }

  std::vector<uint64_t> order; // rowid order, for FIFO eviction
  size_t totalBytes = 0;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const auto key = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    const auto format = static_cast<GLenum>(sqlite3_column_int64(stmt, 1));
    const auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
    const int size = sqlite3_column_bytes(stmt, 2);
    if (blob == nullptr || size <= 0) {
      continue;
    }
    Entry entry{.format = format, .blob = std::vector<uint8_t>(blob, blob + size)};
    g_map.emplace(key, std::move(entry));
    order.push_back(key);
    totalBytes += static_cast<size_t>(size);
  }
  sqlite3_finalize(stmt);

  if (totalBytes > kMaxCacheBytes) {
    size_t evicted = 0;
    sqlite::Transaction tx(g_db, Log, true);
    for (const auto key : order) {
      if (totalBytes <= kMaxCacheBytes) {
        break;
      }
      const auto it = g_map.find(key);
      if (it == g_map.end()) {
        continue;
      }
      totalBytes -= it->second.blob.size();
      g_map.erase(it);
      const auto del = fmt::format("DELETE FROM programs WHERE key = {}", static_cast<int64_t>(key));
      sqlite::exec(g_db, del.c_str());
      ++evicted;
    }
    if (tx) {
      tx.commit();
    }
    Log.info("[binary-cache] evicted {} oldest entries to fit the {} MiB cap", evicted, kMaxCacheBytes >> 20);
  }
  return g_map.size();
}

void writer_thread() {
  while (true) {
    std::deque<WriteOp> batch;
    {
      std::unique_lock lock{g_writerMutex};
      g_writerCv.wait(lock, [] { return g_writerStop || !g_writeQueue.empty(); });
      if (g_writerStop && g_writeQueue.empty()) {
        return;
      }
      batch.swap(g_writeQueue);
    }

    sqlite::Transaction tx(g_db, Log, true);
    if (!tx) {
      Log.error("Failed to begin write transaction; dropping {} ops", batch.size());
      continue;
    }
    bool ok = true;
    for (const auto& op : batch) {
      if (op.kind == WriteOp::Kind::ResetPoison) {
        meta_set("crash_count", "0");
        continue;
      }
      if (op.kind == WriteOp::Kind::Erase) {
        const auto del = fmt::format("DELETE FROM programs WHERE key = {}", static_cast<int64_t>(op.key));
        if (sqlite::exec(g_db, del.c_str()) != SQLITE_OK) {
          ok = false;
          break;
        }
        continue;
      }
      // Upsert.
      sqlite3_stmt* stmt = nullptr;
      if (sqlite3_prepare_v2(g_db,
                             "INSERT INTO programs(key, format, binary) VALUES(?, ?, ?) "
                             "ON CONFLICT(key) DO UPDATE SET format = excluded.format, binary = excluded.binary",
                             -1, &stmt, nullptr) != SQLITE_OK) {
        ok = false;
        break;
      }
      sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(op.key));
      sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(op.format));
      sqlite3_bind_blob64(stmt, 3, op.blob.data(), op.blob.size(), SQLITE_TRANSIENT);
      const int rc = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      if (rc != SQLITE_DONE) {
        ok = false;
        break;
      }
    }
    if (ok) {
      tx.commit();
    } else {
      Log.error("Write transaction failed: {}", sqlite3_errmsg(g_db));
    }
  }
}

void enqueue(WriteOp op) {
  {
    std::lock_guard lock{g_writerMutex};
    g_writeQueue.emplace_back(std::move(op));
  }
  g_writerCv.notify_one();
}

void remove_sentinel() {
  if (g_sentinelPath.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove(g_sentinelPath, ec);
}

void write_sentinel() {
  if (g_sentinelPath.empty()) {
    return;
  }
  std::error_code ec;
  // Content is irrelevant; presence is the signal. Use an SDL-free direct write.
  if (FILE* f = std::fopen(g_sentinelPath.c_str(), "wb")) {
    std::fputc('1', f);
    std::fclose(f);
  }
}
} // namespace

void binary_cache_initialize() {
  g_enabled = false;
  g_hits = 0;
  g_misses = 0;

  if (!kProgramBinaryCache) {
    return;
  }
  if (gl.GetProgramBinary == nullptr || gl.ProgramBinary == nullptr) {
    Log.info("[binary-cache] driver has no glGetProgramBinary/glProgramBinary; source compile only");
    return;
  }
  GLint numFormats = 0;
  gl.GetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &numFormats);
  if (numFormats <= 0) {
    Log.info("[binary-cache] GL_NUM_PROGRAM_BINARY_FORMATS=0; source compile only");
    return;
  }
  if (renderer_is_mesa() && !kForceBinaryCacheOnMesa) {
    Log.info("[binary-cache] Mesa renderer ({}); using its own disk cache, program-binary cache off",
             gl_string(GL_RENDERER));
    return;
  }
  if (g_config.cachePath == nullptr) {
    return;
  }

  const auto dir = std::filesystem::path{g_config.cachePath};
  g_sentinelPath = fs_path_to_string(dir / kSentinelName);
  const auto dbPath = fs_path_to_string(dir / kDbName);

  if (sqlite3_open(dbPath.c_str(), &g_db) != SQLITE_OK) {
    Log.error("Failed to open {}: {}", dbPath, sqlite3_errmsg(g_db));
    sqlite3_close(g_db);
    g_db = nullptr;
    return;
  }
  if (!ensure_schema()) {
    sqlite3_close(g_db);
    g_db = nullptr;
    return;
  }

  const auto currentFp = compute_fingerprint();
  const auto storedFp = meta_get("fingerprint");
  if (storedFp != currentFp) {
    // Driver / build / format change: full reset, including any accumulated poison state.
    wipe_programs();
    meta_set("fingerprint", currentFp);
    meta_set("crash_count", "0");
    meta_set("disabled", "0");
    Log.info("[binary-cache] driver fingerprint changed; cache reset");
  }

  if (meta_get("disabled") == "1") {
    Log.warn("[binary-cache] permanently disabled for this driver after repeated crashes; source compile only");
    sqlite3_close(g_db);
    g_db = nullptr;
    return;
  }

  int crashCount = 0;
  try {
    crashCount = std::stoi(meta_get("crash_count"));
  } catch (...) {
    crashCount = 0;
  }

  size_t loaded = 0;
  const bool sentinelPresent = !g_sentinelPath.empty() && std::filesystem::exists(g_sentinelPath);
  if (sentinelPresent) {
    // The previous run loaded cached binaries and died before the precompile drained -- i.e. it
    // crashed feeding one back through glProgramBinary. Discard the set and count the crash.
    ++crashCount;
    meta_set("crash_count", std::to_string(crashCount));
    remove_sentinel();
    wipe_programs();
    Log.error("[binary-cache] crash sentinel present (count {}/{}); wiped cached program binaries", crashCount,
              kMaxCrashes);
    if (crashCount >= kMaxCrashes) {
      meta_set("disabled", "1");
      Log.error("[binary-cache] disabling program-binary cache permanently for this driver");
      sqlite3_close(g_db);
      g_db = nullptr;
      return;
    }
    // Leave g_map empty this session; recompile from source and rebuild the cache.
  } else {
    loaded = load_map_and_evict();
  }

  // Arm the crash sentinel only when there are binaries that the boot precompile will feed through
  // glProgramBinary -- that is the only window a bad blob can crash the driver.
  if (loaded > 0) {
    write_sentinel();
  }

  g_writerStop = false;
  g_writerThread = std::thread(writer_thread);
  g_enabled = true;
  Log.info("[binary-cache] enabled: {} cached program binaries loaded ({})", loaded, gl_string(GL_RENDERER));
}

void binary_cache_shutdown() {
  if (g_writerThread.joinable()) {
    {
      std::lock_guard lock{g_writerMutex};
      g_writerStop = true;
    }
    g_writerCv.notify_one();
    g_writerThread.join();
  }
  g_writerStop = false;
  g_writeQueue.clear();

  // Clean shutdown: the process survived, so the loaded set is trustworthy for next boot.
  remove_sentinel();

  if (g_db != nullptr) {
    sqlite3_close(g_db);
    g_db = nullptr;
  }
  {
    std::lock_guard lock{g_mapMutex};
    g_map.clear();
  }
  g_enabled = false;
}

bool binary_cache_enabled() { return g_enabled; }

uint64_t binary_cache_key(const char* vertexSource, const char* fragmentSource) {
  XXH3_state_t state;
  XXH3_INITSTATE(&state);
  XXH3_64bits_reset(&state);
  XXH3_64bits_update(&state, vertexSource, std::strlen(vertexSource));
  XXH3_64bits_update(&state, "\x1f", 1); // separator so "ab"+"c" != "a"+"bc"
  XXH3_64bits_update(&state, fragmentSource, std::strlen(fragmentSource));
  return static_cast<uint64_t>(XXH3_64bits_digest(&state));
}

bool binary_cache_try_load(uint64_t key, GLuint program) {
  if (!g_enabled) {
    return false;
  }
  Entry entry;
  {
    std::lock_guard lock{g_mapMutex};
    const auto it = g_map.find(key);
    if (it == g_map.end()) {
      ++g_misses;
      return false;
    }
    entry = it->second;
  }

  gl.ProgramBinary(program, entry.format, entry.blob.data(), static_cast<GLsizei>(entry.blob.size()));
  GLint status = GL_FALSE;
  gl.GetProgramiv(program, GL_LINK_STATUS, &status);
  if (status != GL_TRUE) {
    while (gl.GetError() != GL_NO_ERROR) {
      // drain any error the rejected binary raised
    }
    Log.warn("[binary-cache] driver rejected cached binary {:016x}; recompiling from source", key);
    {
      std::lock_guard lock{g_mapMutex};
      g_map.erase(key);
    }
    enqueue(WriteOp{.kind = WriteOp::Kind::Erase, .key = key});
    ++g_misses;
    return false;
  }
  ++g_hits;
  return true;
}

void binary_cache_store(uint64_t key, GLuint program) {
  if (!g_enabled) {
    return;
  }
  {
    std::lock_guard lock{g_mapMutex};
    if (g_map.count(key) != 0) {
      return;
    }
  }

  GLint length = 0;
  gl.GetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
  if (length <= 0) {
    return;
  }
  std::vector<uint8_t> blob(static_cast<size_t>(length));
  GLsizei written = 0;
  GLenum format = 0;
  gl.GetProgramBinary(program, length, &written, &format, blob.data());
  if (written <= 0) {
    while (gl.GetError() != GL_NO_ERROR) {
    }
    return;
  }
  blob.resize(static_cast<size_t>(written));

  {
    std::lock_guard lock{g_mapMutex};
    g_map.insert_or_assign(key, Entry{.format = format, .blob = blob});
  }
  enqueue(WriteOp{.kind = WriteOp::Kind::Upsert, .key = key, .format = format, .blob = std::move(blob)});
}

void binary_cache_precompile_drained() {
  if (!g_enabled) {
    return;
  }
  // Survived the glProgramBinary loading window: disarm the sentinel and clear the poison counter so
  // an unrelated later crash cannot accumulate against the cache.
  remove_sentinel();
  enqueue(WriteOp{.kind = WriteOp::Kind::ResetPoison});
}

uint32_t binary_cache_hits() { return g_hits.load(std::memory_order_relaxed); }
uint32_t binary_cache_misses() { return g_misses.load(std::memory_order_relaxed); }

} // namespace aurora::gl
