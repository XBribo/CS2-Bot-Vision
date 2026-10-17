#include "gamedata.h"
#include "core/cs2_sdk/src/utils/platform.h"

#include <filesystem>

namespace cs2bv::gamedata {
std::string ComputePath()
{
    std::filesystem::path path(cs2_sdk::platform::SelfModulePath());
    if (path.empty()) return "";
    for (int i = 0; i < 3; ++i)
    {
        path = path.parent_path();
        if (path.empty()) return "";
    }
    return (path / "gamedata.json").string();
}
} // namespace cs2bv::gamedata
