#pragma once
#include <string>
namespace cs2bv::config {
bool IsValidFixedPointValue(double value);
bool AreCommandsServerOnly();

void LoadStartupConfig(const std::string& gamedataPath);
} // namespace cs2bv::config
