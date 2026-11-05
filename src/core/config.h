#pragma once
#include <string>
namespace cs2bv::config {
bool IsValidFixedPointValue(double value);

void LoadStartupConfig(const std::string& gamedataPath);
}
