#pragma once

#include <string>

namespace cs2bv::gamedata {
// Computes <plugin>/gamedata.json from <plugin>/bin/<platform>/<binary>.
// Does not check whether the file exists.
std::string ComputePath();
} // namespace cs2bv::gamedata
