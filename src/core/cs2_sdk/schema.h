// Runtime Source 2 schema field resolver

#pragma once // NOLINT(portability-avoid-pragma-once)

class ISchemaSystem;
extern ISchemaSystem* g_schemaSystem;

namespace cs2bv::schema {
// Uses the ISchemaSystem acquired from the engine factory.
bool Init();

// Returns a server field offset, including inherited fields, or -1
int GetFieldOffset(const char* className, const char* fieldName);
} // namespace cs2bv::schema
