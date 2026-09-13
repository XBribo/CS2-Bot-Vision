#pragma once
#include <string>
namespace cs2bv::config {
// Applies defaults, then loads or creates the startup configuration.
void LoadStartupConfig(const std::string& gamedataPath);
}
