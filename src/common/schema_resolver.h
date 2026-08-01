// Runtime Source 2 schema field resolver

#pragma once

namespace cs2bv::schema {
// Resolves ISchemaSystem from the loaded schemasystem module
bool Init();

// Returns a server field offset, including inherited fields, or -1
int GetFieldOffset(const char* className, const char* fieldName);
} // namespace cs2bv::schema
