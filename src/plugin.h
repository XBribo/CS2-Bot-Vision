#pragma once

#include <ISmmPlugin.h>

class ISchemaSystem;
extern ISchemaSystem* g_schemaSystem;

namespace cs2bv {

class BotVisionPlugin : public ISmmPlugin
{
  public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
    bool Unload(char* error, size_t maxlen) override;

    const char* GetAuthor() override;
    const char* GetName() override;
    const char* GetDescription() override;
    const char* GetURL() override;
    const char* GetLicense() override;
    const char* GetVersion() override;
    const char* GetDate() override;
    const char* GetLogTag() override;
};

extern BotVisionPlugin g_plugin;

} // namespace cs2bv

PLUGIN_GLOBALVARS();
