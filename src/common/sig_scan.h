// Signature scanning and gamedata helpers

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cs2bv::sig
{
    struct ModuleSegment
    {
        unsigned char *Base = nullptr;
        size_t Size = 0;
    };

    struct ModuleInfo
    {
        unsigned char *Base = nullptr;
        size_t Size = 0;
        std::vector<ModuleSegment> Segments;

        // Reports whether module boundaries were resolved
        explicit operator bool() const { return Base != nullptr && Size != 0; }
    };

    // Read + parse gamedata.json into `out`. Returns false on open/parse error
    bool LoadGamedata(const char *path, nlohmann::json &out);

    std::string FindPlatformSig(const nlohmann::json &gamedata, const std::string &name);

    const char *PlatformName();

    bool ParseSigString(const std::string &sigStr,
                        std::vector<uint8_t> &outBytes,
                        std::vector<bool> &outWild);

    void *FindPatternIn(const ModuleInfo &module,
                        const std::vector<uint8_t> &pattern,
                        const std::vector<bool> &wild);

    // Resolve a module by basename, e.g. server.dll or libserver.so
    ModuleInfo ModuleFromName(const char *moduleName);

    ModuleInfo ModuleFromInterfacePtr(void *interfacePtr);

    void *ResolveSig(const nlohmann::json &gamedata, const ModuleInfo &module,
                     const char *name, char *errorOut, size_t errorOutLen);

    // Read a platform offset from gamedata[name].offsets.<platform>; defVal if absent
    int ResolveOffset(const nlohmann::json &gamedata, const char *name, int defVal);
}
