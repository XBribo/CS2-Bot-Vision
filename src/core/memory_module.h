
#pragma once // NOLINT(portability-avoid-pragma-once)

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace cs2bv::modules {
#ifdef _WIN32
inline constexpr const char* kRootBin = "/bin/win64/";
inline constexpr const char* kGameBin = "/csgo/bin/win64/";
inline constexpr const char* kModulePrefix = "";
inline constexpr const char* kModuleExtension = ".dll";
#else
inline constexpr const char* kRootBin = "/bin/linuxsteamrt64/";
inline constexpr const char* kGameBin = "/csgo/bin/linuxsteamrt64/";
inline constexpr const char* kModulePrefix = "lib";
inline constexpr const char* kModuleExtension = ".so";
#endif

struct ModuleSegment
{
    unsigned char* base = nullptr;
    size_t size = 0;
};

struct ModuleInfo
{
    unsigned char* base = nullptr;
    size_t size = 0;
    std::vector<ModuleSegment> segments;

    // Reports whether module boundaries were resolved
    explicit operator bool() const { return base != nullptr && size != 0; }
};

class CModule
{
  public:
    // Loads one game module from its explicit game-relative directory.
    CModule(const char* relativeDirectory, const char* moduleName);

    // Reports whether the module image is usable.
    explicit operator bool() const { return static_cast<bool>(m_image); }

    // Returns all mapped load segments used by general signature scans.
    const ModuleInfo& Image() const { return m_image; }

    // Returns only executable load segments used by code-only scans.
    const ModuleInfo& Code() const { return m_code; }

    // Returns the exact path passed to the platform loader.
    const char* Path() const { return m_path.c_str(); }

  private:
    std::string m_path;
    void* m_hModule = nullptr;
    ModuleInfo m_image;
    ModuleInfo m_code;
};

extern CModule* engine;
extern CModule* server;

// Loads the engine and server modules once for signature resolution.
void Initialize();

// Parses hexadecimal bytes and wildcard markers.
bool ParseSigString(const std::string& sigStr, std::vector<uint8_t>& outBytes, std::vector<bool>& outWild);

// Finds the first pattern match across the module segments.
void* FindPatternIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild);

// Finds every pattern match in the selected module segments
std::vector<void*> FindPatternMatchesIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild);

// Resolves a polymorphic class vtable from loaded module RTTI
void** ResolveVirtualTable(const ModuleInfo& module, const char* className, char* errorOut, size_t errorOutLen);

// Checks whether an address belongs to executable image memory
bool IsExecutableAddress(const void* address);


} // namespace cs2bv::modules
