
#pragma once // NOLINT(portability-avoid-pragma-once)

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>


namespace cs2bv::modules {
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




bool ParseSigString(const std::string& sigStr, std::vector<uint8_t>& outBytes, std::vector<bool>& outWild);

void* FindPatternIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild);

// Finds every pattern match in the selected module segments
std::vector<void*> FindPatternMatchesIn(const ModuleInfo& module, const std::vector<uint8_t>& pattern, const std::vector<bool>& wild);

// Resolve a module by basename, e.g. server.dll or libserver.so
ModuleInfo ModuleFromName(const char* moduleName);

// Resolves executable code ranges from a loaded module
ModuleInfo ModuleCodeFromName(const char* moduleName);

ModuleInfo ModuleFromInterfacePtr(void* interfacePtr);

// Resolves a polymorphic class vtable from loaded module RTTI
void** ResolveVirtualTable(const ModuleInfo& module, const char* className, char* errorOut, size_t errorOutLen);

// Checks whether an address belongs to executable image memory
bool IsExecutableAddress(const void* address);


} // namespace cs2bv::modules
