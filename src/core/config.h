#pragma once
#include <string>
namespace cs2bv::config {
// Checks the stored milli-unit range.
bool IsValidFixedPointValue(double value);

// Applies defaults, then loads or creates the startup configuration.
void LoadStartupConfig(const std::string& gamedataPath);
}
