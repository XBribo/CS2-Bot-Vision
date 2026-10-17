#pragma once

#include "core/cs2_sdk/src/core/log.h"

namespace cs2bv {
namespace log = cs2_sdk::log;
}

#define BV_LOG_DEBUG(...) ::cs2bv::log::Write(::cs2bv::log::Level::Debug, __VA_ARGS__)
#define BV_LOG_INFO(...)  ::cs2bv::log::Write(::cs2bv::log::Level::Info, __VA_ARGS__)
#define BV_LOG_WARN(...)  ::cs2bv::log::Write(::cs2bv::log::Level::Warn, __VA_ARGS__)
#define BV_LOG_ERROR(...) ::cs2bv::log::Write(::cs2bv::log::Level::Error, __VA_ARGS__)
