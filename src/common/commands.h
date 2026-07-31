#pragma once

class IVEngineServer2;
class CCommandContext;

namespace cs2bv::commands {

extern IVEngineServer2* g_pEngine;

// Registers plugin console commands
void Register();

// Releases plugin console command state
void Unregister();

// Prints formatted text to the invoking client or server console
void PrintToCaller(const CCommandContext& context, const char* fmt, ...);

} // namespace cs2bv::commands
