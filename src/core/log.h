#pragma once

#include <cstddef>

namespace cs2bv::log {
enum class Level
{
    Debug,
    Info,
    Warn,
    Error
};

// Opens the console and file sinks before plugin initialization.
bool Init(const char* baseDir, char* error, size_t maxlen);
// Flushes and releases the plugin logger after hook cleanup.
void Close();
// Formats existing printf-style diagnostics for the shared logger.
void Write(Level level, const char* format, ...);
} // namespace cs2bv::log

#define BV_LOG_DEBUG(...) ::cs2bv::log::Write(::cs2bv::log::Level::Debug, __VA_ARGS__)
#define BV_LOG_INFO(...)  ::cs2bv::log::Write(::cs2bv::log::Level::Info, __VA_ARGS__)
#define BV_LOG_WARN(...)  ::cs2bv::log::Write(::cs2bv::log::Level::Warn, __VA_ARGS__)
#define BV_LOG_ERROR(...) ::cs2bv::log::Write(::cs2bv::log::Level::Error, __VA_ARGS__)
