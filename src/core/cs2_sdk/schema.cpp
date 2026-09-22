// Runtime Source 2 schema field resolver

#include "core/cs2_sdk/schema.h"

#include <schemasystem/schemasystem.h>
#include <schemasystem/schematypes.h>

#include <cstring>
#include <string>
#include <unordered_map>

ISchemaSystem* g_schemaSystem = nullptr;

namespace cs2bv::schema {
namespace {
std::unordered_map<std::string, int> g_offsetCache; // NOLINT(bugprone-throwing-static-initialization)

// Finds one class in the server or global schema scope
CSchemaClassInfo* FindClass(const char* className)
{
#ifdef _WIN32
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
int FindFieldOffset(const CSchemaClassInfo* classInfo, const char* fieldName, int depth) // NOLINT(misc-no-recursion)
{
    if (!classInfo || !fieldName || depth > 32) return -1;

    for (uint16_t index = 0; index < classInfo->m_nFieldCount; ++index)
    {
        const SchemaClassFieldData_t& field = classInfo->m_pFields[index];
        if (field.m_pszName && std::strcmp(field.m_pszName, fieldName) == 0) return field.m_nSingleInheritanceOffset;
    }

    if (!classInfo->m_pBaseClasses) return -1;
    for (uint8_t index = 0; index < classInfo->m_nBaseClassCount; ++index)
    {
        const SchemaBaseClassInfoData_t& baseClass = classInfo->m_pBaseClasses[index];
        const int fieldOffset = FindFieldOffset(baseClass.m_pClass, fieldName, depth + 1);
        if (fieldOffset >= 0) return static_cast<int>(baseClass.m_nOffset) + fieldOffset;
    }
    return -1;
}
} // namespace

// Uses the ISchemaSystem acquired from the engine factory.
bool Init()
{
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
