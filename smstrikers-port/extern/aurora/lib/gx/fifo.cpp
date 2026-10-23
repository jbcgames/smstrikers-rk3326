#include "fifo.hpp"
#include "command_processor.hpp"
#include "../internal.hpp"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
}

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void patch_u32(const uint32_t offset, const uint32_t val) {
  ASSERT(!detail::sInDisplayList && offset + sizeof(uint32_t) <= detail::sBufferSize,
         "fifo::patch_u32: invalid patch offset {} (buffer size {})", offset, detail::sBufferSize);
  const auto out = bswap(val);
  std::memcpy(detail::sBufferData + offset, &out, sizeof(out));
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

void drain() {
  if (detail::sBufferSize == 0) {
    return;
  }
  static int s_drain_c = 0;
  if (++s_drain_c >= 30) {
    s_drain_c = 0;
    static int s_debug_fifo = -1;
    if (s_debug_fifo < 0) {
      const char* v = std::getenv("STRIKERS_DEBUG_PERF");
      s_debug_fifo = (v && std::strcmp(v, "1") == 0) ? 1 : 0;
    }
    if (s_debug_fifo) {
      std::fprintf(stdout, "[FIFO-DEBUG] buffer size: %u bytes (%u KB)\n", detail::sBufferSize, detail::sBufferSize / 1024);
      std::fflush(stdout);
    }
  }
  process(detail::sBufferData, detail::sBufferSize, true);
  detail::sBufferSize = 0;
}

namespace {
std::thread s_worker;
std::mutex s_mutex;
std::condition_variable s_cv;
std::function<void()> s_job;
bool s_busy = false;
bool s_stop = false;
FrameBuffer s_spare;

void worker_main() {
  while (true) {
    std::function<void()> job;
    {
      std::unique_lock lock{s_mutex};
      s_cv.wait(lock, [] { return s_job != nullptr || s_stop; });
      if (s_job == nullptr) {
        break;
      }
      job = std::move(s_job);
      s_job = nullptr;
    }
    job();
    {
      std::lock_guard lock{s_mutex};
      s_busy = false;
    }
    s_cv.notify_all();
  }
}
} // namespace

bool threaded() noexcept {
  static const bool on = [] {
    const char* e = std::getenv("AURORA_FIFO_THREAD");
    const bool v = e == nullptr || *e != '0';
    if (v) {
      Log.info("FIFO processing on its own thread");
    }
    return v;
  }();
  return on;
}

FrameBuffer take_frame() {
  FrameBuffer frame{detail::sBufferData, detail::sBufferSize, detail::sBufferCapacity};
  if (s_spare.data == nullptr) {
    s_spare.capacity = detail::sBufferCapacity;
    s_spare.data = static_cast<uint8_t*>(malloc(s_spare.capacity));
  }
  detail::sBufferData = s_spare.data;
  detail::sBufferCapacity = s_spare.capacity;
  detail::sBufferSize = 0;
  s_spare = {};
  return frame;
}

void recycle_frame(const FrameBuffer& frame) {
  std::lock_guard lock{s_mutex};
  s_spare = {frame.data, 0, frame.capacity};
}

void post_frame(std::function<void()> job) {
  if (!s_worker.joinable()) {
    s_worker = std::thread(worker_main);
  }
  {
    std::lock_guard lock{s_mutex};
    s_job = std::move(job);
    s_busy = true;
  }
  s_cv.notify_all();
}

void wait_idle() noexcept {
  if (!threaded()) {
    return;
  }
  std::unique_lock lock{s_mutex};
  s_cv.wait(lock, [] { return !s_busy; });
}

void stop_thread() noexcept {
  if (!s_worker.joinable()) {
    return;
  }
  wait_idle();
  {
    std::lock_guard lock{s_mutex};
    s_stop = true;
  }
  s_cv.notify_all();
  s_worker.join();
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
void clear_buffer() { detail::sBufferSize = 0; }

} // namespace aurora::gx::fifo

extern "C" void aurora_gx_wait_idle(void) { aurora::gx::fifo::wait_idle(); }
extern "C" int aurora_gx_worker_active(void) { return aurora::gx::fifo::threaded() ? 1 : 0; }
