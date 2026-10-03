#include <chrono>
#include <ctime>
#include <mutex>

#include "internal.hpp"
#include <aurora/time.hpp>
#include <dolphin/os.h>

namespace chrono = std::chrono;
using namespace std::literals::chrono_literals;

using SystemDuration = chrono::system_clock::duration;
using SystemTime = chrono::time_point<chrono::system_clock>;
using LocalTime = chrono::local_time<SystemDuration>;

namespace {
// GCN epoch: 2000-01-01 00:00:00 UTC = 946684800 seconds after Unix epoch
constexpr SystemTime kGcnEpochUnix{946684800s};
std::once_flag s_gameEpochOnce;
OSTime s_gameEpochOffset;

std::tm utc_time(std::time_t ticks) {
  std::tm result{};
#if defined(_WIN32)
  const errno_t error = gmtime_s(&result, &ticks);
  AURORA_ASSERT(error == 0, "gmtime_s failed");
#else
  const std::tm* converted = gmtime_r(&ticks, &result);
  AURORA_ASSERT(converted != nullptr, "gmtime_r failed");
#endif
  return result;
}

std::time_t utc_ticks(std::tm* time) {
#if defined(_WIN32)
  return _mkgmtime(time);
#else
  return timegm(time);
#endif
}

LocalTime system_time_to_local_time(SystemTime time) {
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
  return chrono::zoned_time(chrono::current_zone(), time).get_local_time();
#else
  // libstdc++ 10 / older libc++ lack the C++20 calendar & timezone API, so use
  // POSIX localtime_r and encode the wall-clock reading as seconds since epoch
  // (as if UTC). timegm gives exactly that encoding.
  const auto wholeSeconds = chrono::floor<chrono::seconds>(time);
  const auto fractionalSeconds = chrono::duration_cast<SystemDuration>(time - wholeSeconds);
  const std::time_t wallClock = chrono::system_clock::to_time_t(time);
  std::tm localTm{};

#if defined(_WIN32)
  const errno_t result = localtime_s(&localTm, &wallClock);
  AURORA_ASSERT(result == 0, "localtime_s failed in system_time_to_local_time");
#else
  const std::tm* result = localtime_r(&wallClock, &localTm);
  AURORA_ASSERT(result != nullptr, "localtime_r failed in system_time_to_local_time");
#endif

  const std::time_t localTicks = utc_ticks(&localTm);
  return LocalTime{chrono::duration_cast<SystemDuration>(chrono::seconds{localTicks}) + fractionalSeconds};
#endif
}

SystemTime local_time_to_system_time(LocalTime time) {
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
  return chrono::zoned_time(chrono::current_zone(), time).get_sys_time();
#else
  // Inverse of the fallback in system_time_to_local_time: decode the
  // wall-clock reading (encoded as-if-UTC) and apply the local timezone with
  // mktime. Ambiguous only for the one hour at fall-back DST transitions.
  const auto wholeSeconds = chrono::floor<chrono::seconds>(time.time_since_epoch());
  const auto fractionalSeconds = chrono::duration_cast<SystemDuration>(time.time_since_epoch() - wholeSeconds);
  const std::time_t localTicks = static_cast<std::time_t>(wholeSeconds.count());

  std::tm localTm = utc_time(localTicks);
  localTm.tm_isdst = -1;

  const std::time_t utcTime = mktime(&localTm);
  AURORA_ASSERT(utcTime != -1, "mktime failed in local_time_to_system_time");

  return chrono::system_clock::from_time_t(utcTime) + fractionalSeconds;
#endif
}

template <typename Duration>
OSTime duration_to_ticks(const Duration duration) {
  const auto seconds = chrono::floor<chrono::seconds>(duration);
  const auto nanoseconds = chrono::duration_cast<chrono::nanoseconds>(duration - seconds);
  return seconds.count() * static_cast<OSTime>(OS_TIMER_CLOCK) +
         nanoseconds.count() * static_cast<OSTime>(OS_TIMER_CLOCK) / 1000000000LL;
}

void initialize_game_epoch() {
  s_gameEpochOffset = OSGetSystemTime() - duration_to_ticks(aurora::time::game_clock::now().time_since_epoch());
}
} // namespace

OSTick OSGetTick() { return OSGetTime() & 0xFFFFFFFF; }

OSTime OSGetTime() {
  std::call_once(s_gameEpochOnce, initialize_game_epoch);
  return s_gameEpochOffset + duration_to_ticks(aurora::time::game_clock::now().time_since_epoch());
}

OSTime OSGetNativeTime() { return duration_to_ticks(aurora::time::native_clock::now().time_since_epoch()); }

void AuroraInitClock() {
  std::call_once(s_gameEpochOnce, initialize_game_epoch);
  if (OSBaseAddress == 0) {
    return;
  }

  __OSBusClock = OS_TIMER_CLOCK * OS_TIMER_CLOCK_DIVIDER;
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
  // We assume that all input times (ticks) are in UTC, relative to GCN epoch
  // So convert that to the local time
  const LocalTime local =
      system_time_to_local_time(SystemTime{chrono::microseconds{OSTicksToMicroseconds(ticks)} + kGcnEpochUnix});

  // The local time is a wall-clock reading encoded as seconds since epoch
  // (as if UTC), so decode the components with gmtime.
  const auto localSeconds = chrono::floor<chrono::seconds>(local.time_since_epoch());
  const auto localSubseconds = chrono::duration_cast<chrono::microseconds>(local.time_since_epoch() - localSeconds);
  const std::time_t localTicks = static_cast<std::time_t>(localSeconds.count());
  const std::tm localTm = utc_time(localTicks);

  td->sec = localTm.tm_sec;
  td->min = localTm.tm_min;
  td->hour = localTm.tm_hour;
  td->mday = localTm.tm_mday;
  td->mon = localTm.tm_mon;
  td->year = localTm.tm_year + 1900;
  td->wday = localTm.tm_wday;
  td->yday = localTm.tm_yday;

  td->msec = static_cast<int>(chrono::duration_cast<chrono::milliseconds>(localSubseconds).count());
  td->usec = static_cast<int>(localSubseconds.count() - td->msec * 1000);

  AURORA_ASSERT(0 <= td->usec, "0 <= td->usec");
  AURORA_ASSERT(0 <= td->msec, "0 <= td->msec");
  AURORA_ASSERT(0 <= td->sec, "0 <= td->sec");
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* td) {
  std::tm localTm{};
  localTm.tm_year = td->year - 1900;
  localTm.tm_mon = td->mon;
  localTm.tm_mday = td->mday;
  localTm.tm_hour = td->hour;
  localTm.tm_min = td->min;
  localTm.tm_sec = td->sec;
  localTm.tm_isdst = -1;

  // Build the wall-clock reading (seconds since epoch, as if UTC), then hand
  // it to local_time_to_system_time to apply the local timezone.
  const std::time_t localTicks = utc_ticks(&localTm);
  AURORA_ASSERT(localTicks != -1, "UTC time conversion failed in OSCalendarTimeToTicks");

  const LocalTime local{chrono::duration_cast<SystemDuration>(chrono::seconds{localTicks}) +
                        chrono::milliseconds{td->msec} + chrono::microseconds{td->usec}};

  const SystemTime sys = local_time_to_system_time(local);

  return OSMicrosecondsToTicks(chrono::duration_cast<chrono::microseconds>(sys - kGcnEpochUnix).count());
}

OSTime OSGetSystemTime() { return duration_to_ticks(aurora::time::wall_clock::now() - kGcnEpochUnix); }
