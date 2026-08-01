// Runtime Source 2 schema field resolver

#include "schema_resolver.h"

#include <schemasystem/schemasystem.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

#include <cstring>
#include <string>
#include <unordered_map>

namespace cs2bv::schema {
using CreateIfaceFn = void* (*)(const char*, int*);

namespace {
ISchemaSystem* g_schemaSystem = nullptr;
std::unordered_map<std::string, int> g_offsetCache;

#if !defined(_WIN32)
// Returns the final component of a Unix path
const char* BaseName(const char* path)
{
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

struct FindModuleContext
{
    const char* name = nullptr;
    const char* path = nullptr;
};

// Finds the full path of one loaded ELF module
int FindModuleCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* context = static_cast<FindModuleContext*>(data);
    if (info->dlpi_name && std::strcmp(BaseName(info->dlpi_name), context->name) == 0)
    {
        context->path = info->dlpi_name;
        return 1;
    }
    return 0;
}

// Opens one already loaded ELF module without loading another copy
void* OpenLoadedModule(const char* moduleName)
{
    void* module = dlopen(moduleName, RTLD_NOW | RTLD_NOLOAD);
    if (module) return module;

    FindModuleContext context{};
    context.name = moduleName;
    dl_iterate_phdr(FindModuleCallback, &context);
    return context.path && context.path[0] ? dlopen(context.path, RTLD_NOW | RTLD_NOLOAD) : nullptr;
}
#endif

// Finds one class in the server or global schema scope
CSchemaClassInfo* FindClass(const char* className)
{
#if defined(_WIN32)
    static constexpr const char* kServerScopes[] = { "server.dll", "libserver.so" };
#else
    static constexpr const char* kServerScopes[] = { "libserver.so", "server.dll" };
#endif
    for (const char* scopeName : kServerScopes)
    {
        CSchemaSystemTypeScope* scope = g_schemaSystem->FindTypeScopeForModule(scopeName, nullptr);
        if (!scope) continue;
        if (CSchemaClassInfo* classInfo = scope->FindDeclaredClass(className).Get()) return classInfo;
    }

    CSchemaSystemTypeScope* globalScope = g_schemaSystem->GlobalTypeScope();
    return globalScope ? globalScope->FindDeclaredClass(className).Get() : nullptr;
}

// Finds a field recursively through the schema inheritance tree
int FindFieldOffset(const CSchemaClassInfo* classInfo, const char* fieldName, int depth)
{
    if (!classInfo || !fieldName || depth > 32) return -1;

    for (uint16 index = 0; index < classInfo->m_nFieldCount; ++index)
    {
        const SchemaClassFieldData_t& field = classInfo->m_pFields[index];
        if (field.m_pszName && std::strcmp(field.m_pszName, fieldName) == 0) return field.m_nSingleInheritanceOffset;
    }

    if (!classInfo->m_pBaseClasses) return -1;
    for (uint8 index = 0; index < classInfo->m_nBaseClassCount; ++index)
    {
        const SchemaBaseClassInfoData_t& baseClass = classInfo->m_pBaseClasses[index];
        const int fieldOffset = FindFieldOffset(baseClass.m_pClass, fieldName, depth + 1);
        if (fieldOffset >= 0) return static_cast<int>(baseClass.m_nOffset) + fieldOffset;
    }
    return -1;
}
} // namespace

// Resolves ISchemaSystem from the already loaded schemasystem module
bool Init()
{
    if (g_schemaSystem) return true;

#if defined(_WIN32)
    HMODULE module = GetModuleHandleA("schemasystem.dll");
    if (!module) return false;
    auto createInterface = reinterpret_cast<CreateIfaceFn>(GetProcAddress(module, "CreateInterface"));
#else
    void* module = OpenLoadedModule("libschemasystem.so");
    if (!module) return false;
    auto createInterface = reinterpret_cast<CreateIfaceFn>(dlsym(module, "CreateInterface"));
#endif
    if (!createInterface) return false;

    g_schemaSystem = static_cast<ISchemaSystem*>(createInterface(SCHEMASYSTEM_INTERFACE_VERSION, nullptr));
    return g_schemaSystem != nullptr;
}

// Resolves and caches one field offset from the live server schema
int GetFieldOffset(const char* className, const char* fieldName)
{
    if (!g_schemaSystem || !className || !fieldName) return -1;

    const std::string key = std::string(className) + "::" + fieldName;
    const auto cached = g_offsetCache.find(key);
    if (cached != g_offsetCache.end()) return cached->second;

    const int offset = FindFieldOffset(FindClass(className), fieldName, 0);
    g_offsetCache.emplace(key, offset);
    return offset;
}
} // namespace cs2bv::schema
