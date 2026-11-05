#include "core/config.h"
#include "core/log.h"
#include "features/vision/BotVision.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
namespace cs2bv::config {
namespace {
constexpr int kDefaultSmokeMode = 0;
constexpr float kDefaultDensityThreshold = 0.23F;
constexpr bool kDefaultBulletHolesEnabled = true;

nlohmann::json BuildDefaultConfig()
{
    return { { "smoke", { { "mode", kDefaultSmokeMode }, { "density_threshold", kDefaultDensityThreshold } } },
             { "bullet_holes", { { "enabled", kDefaultBulletHolesEnabled } } } };
}

void WarnInvalidSetting(const char* setting) { BV_LOG_WARN("Invalid config: %s; using default", setting); }

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
    if (!IsValidFixedPointValue(number))
    {
        WarnInvalidSetting(setting);
        return;
    }
    setter(static_cast<float>(number));
}

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
            BV_LOG_WARN("he_holes radius/duration are fixed (250 units, 5 seconds)");
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
            if (bulletHoles.contains("radius") || bulletHoles.contains("shotgun_radius") || bulletHoles.contains("duration"))
                BV_LOG_WARN("bullet hole geometry/lifetime use fixed parameters (20/80 units, 1 second)");
            if (bulletHoles.contains("enabled"))
            {
                if (bulletHoles["enabled"].is_boolean()) bot_vision::SetBulletHolesEnabled(bulletHoles["enabled"].get<bool>());
                else
                    WarnInvalidSetting("bullet_holes.enabled");
            }
        }
    }
}

} // namespace

bool IsValidFixedPointValue(double value)
{
    const double maximum = static_cast<double>(std::numeric_limits<int>::max());
    if (!std::isfinite(value) || value < 0.0 || value > maximum / 1000.0) return false;
    return static_cast<double>(static_cast<float>(value) * 1000.0F) <= maximum;
}

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
            BV_LOG_WARN("Cannot create config.json; using defaults");
        }
        return;
    }

    const std::string configText((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
    const nlohmann::json config = nlohmann::json::parse(configText, nullptr, false);
    if (config.is_discarded() || !config.is_object())
    {
        BV_LOG_WARN("Invalid config.json; using defaults");
        return;
    }
    ApplyStartupConfig(config);
}

} // namespace cs2bv::config
