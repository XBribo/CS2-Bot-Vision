#include "core/config.h"
#include "core/log.h"
#include "features/vision/BotVision.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
namespace cs2bv::config {
namespace {
constexpr int kDefaultSmokeMode = 0;
constexpr float kDefaultDensityThreshold = 0.23F;
constexpr float kDefaultHeRadius = 250.0F;
constexpr float kDefaultHeDuration = 5.0F;
constexpr bool kDefaultBulletHolesEnabled = true;
constexpr float kDefaultBulletRadius = 20.0F;
constexpr float kDefaultShotgunRadius = 80.0F;
constexpr float kDefaultBulletDuration = 1.0F;
constexpr double kMaximumFixedPointValue = (static_cast<double>(std::numeric_limits<int>::max()) / 1000.0) - 1.0;

// Builds the packaged startup configuration
nlohmann::json BuildDefaultConfig()
{
    return { { "smoke", { { "mode", kDefaultSmokeMode }, { "density_threshold", kDefaultDensityThreshold } } },
             { "he_holes", { { "radius", kDefaultHeRadius }, { "duration", kDefaultHeDuration } } },
             { "bullet_holes",
               { { "enabled", kDefaultBulletHolesEnabled },
                 { "radius", kDefaultBulletRadius },
                 { "shotgun_radius", kDefaultShotgunRadius },
                 { "duration", kDefaultBulletDuration } } } };
}

// Reports one invalid startup setting
void WarnInvalidSetting(const char* setting)
{
    char message[192];
    std::snprintf(message, sizeof(message), "[BotVision] WARN: invalid config setting '%s'; using default\n", setting);
    BV_LOG_INFO("%s", message);
}

// Applies one nonnegative fixed-point startup setting
void ApplyNonNegativeFloat(const nlohmann::json& section, const char* key, const char* setting, void (*setter)(float))
{
    if (!section.contains(key)) return;

    const auto& value = section[key];
    if (!value.is_number())
    {
        WarnInvalidSetting(setting);
        return;
    }

    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0.0 || number > kMaximumFixedPointValue)
    {
        WarnInvalidSetting(setting);
        return;
    }
    setter(static_cast<float>(number));
}

// Applies validated startup settings over the active defaults
void ApplyStartupConfig(const nlohmann::json& config)
{
    if (config.contains("smoke"))
    {
        const auto& smoke = config["smoke"];
        if (!smoke.is_object())
        {
            WarnInvalidSetting("smoke");
        }
        else
        {
            if (smoke.contains("mode"))
            {
                const auto& mode = smoke["mode"];
                if (mode.is_number_integer() && (mode == 0 || mode == 1))
                {
                    bot_vision::SetSmokeMode(mode.get<int>());
                }
                else
                {
                    WarnInvalidSetting("smoke.mode");
                }
            }
            ApplyNonNegativeFloat(smoke, "density_threshold", "smoke.density_threshold", bot_vision::SetDensityThreshold);
        }
    }

    if (config.contains("he_holes"))
    {
        const auto& heHoles = config["he_holes"];
        if (!heHoles.is_object())
        {
            WarnInvalidSetting("he_holes");
        }
        else
        {
            ApplyNonNegativeFloat(heHoles, "radius", "he_holes.radius", bot_vision::SetHeRadius);
            ApplyNonNegativeFloat(heHoles, "duration", "he_holes.duration", bot_vision::SetHeDuration);
        }
    }

    if (config.contains("bullet_holes"))
    {
        const auto& bulletHoles = config["bullet_holes"];
        if (!bulletHoles.is_object())
        {
            WarnInvalidSetting("bullet_holes");
        }
        else
        {
            if (bulletHoles.contains("enabled"))
            {
                if (bulletHoles["enabled"].is_boolean()) bot_vision::SetBulletHolesEnabled(bulletHoles["enabled"].get<bool>());
                else
                    WarnInvalidSetting("bullet_holes.enabled");
            }
            ApplyNonNegativeFloat(bulletHoles, "radius", "bullet_holes.radius", bot_vision::SetBulletRadius);
            ApplyNonNegativeFloat(bulletHoles, "shotgun_radius", "bullet_holes.shotgun_radius", bot_vision::SetBulletRadiusShotgun);
            ApplyNonNegativeFloat(bulletHoles, "duration", "bullet_holes.duration", bot_vision::SetBulletDuration);
        }
    }
}

} // namespace

// Loads config.json beside gamedata.json and creates it when missing
void LoadStartupConfig(const std::string& gamedataPath)
{
    const nlohmann::json defaults = BuildDefaultConfig();
    ApplyStartupConfig(defaults);

    std::filesystem::path configPath(gamedataPath);
    configPath.replace_filename("config.json");
    std::ifstream configFile(configPath, std::ios::binary);
    if (!configFile.is_open())
    {
        std::ofstream defaultFile(configPath, std::ios::trunc);
        if (defaultFile.is_open())
        {
            defaultFile << defaults.dump(4) << '\n';
        }
        else
        {
            BV_LOG_WARN("%s", "[BotVision] WARN: config.json missing and could not be created; using defaults\n");
        }
        return;
    }

    const std::string configText((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
    const nlohmann::json config = nlohmann::json::parse(configText, nullptr, false);
    if (config.is_discarded() || !config.is_object())
    {
        BV_LOG_WARN("%s", "[BotVision] WARN: config.json parse error; using defaults\n");
        return;
    }
    ApplyStartupConfig(config);
}



}
