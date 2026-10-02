#pragma once
namespace eventlog {
template <class... T>
inline void log(const char*, const char*, T...) {}
inline void flush_tick() {}
}  // namespace eventlog
