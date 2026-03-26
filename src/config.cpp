#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "config.h"

namespace fs = std::filesystem;

Config g_config;
double g_time = 0.0;
std::vector<ConfigOption> g_configRegistry;

static fs::path ConfigDirPath() {
    if (const char* xdgConfig = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdgConfig) return fs::path(xdgConfig) / "desktop-goose";
    }

    if (const char* home = std::getenv("HOME")) {
        if (*home) return fs::path(home) / ".config" / "desktop-goose";
    }

    return fs::current_path();
}

std::string Config_GetPath() {
    return (ConfigDirPath() / "config.ini").string();
}

static std::string Trim(const std::string& text) {
    const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (begin >= end) return "";
    return std::string(begin, end);
}

static bool ParseBoolValue(const std::string& rawValue, bool* outValue) {
    std::string lowered = rawValue;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });

    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        *outValue = true;
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        *outValue = false;
        return true;
    }
    return false;
}

static std::string OptionValueToString(const ConfigOption& opt) {
    if (opt.type == CFG_BOOL) return *(bool*)opt.ptr ? "1" : "0";
    if (opt.type == CFG_INT) return std::to_string(*(int*)opt.ptr);
    if (opt.type == CFG_FLOAT) {
        std::ostringstream stream;
        stream << *(float*)opt.ptr;
        return stream.str();
    }
    return *(std::string*)opt.ptr;
}

static const ConfigOption* FindOptionByAnyKey(const std::string& key) {
    for (const auto& opt : g_configRegistry) {
        if (key == opt.key || key == opt.label) return &opt;
    }
    return nullptr;
}

const ConfigOption* Config_FindOptionByKey(const std::string& key) {
    for (const auto& opt : g_configRegistry) {
        if (key == opt.key) return &opt;
    }
    return nullptr;
}

static bool AssignValueToOption(const ConfigOption& opt, const std::string& rawValue, std::string* errorOut) {
    try {
        if (opt.type == CFG_BOOL) {
            bool parsed = false;
            if (!ParseBoolValue(Trim(rawValue), &parsed)) {
                if (errorOut) *errorOut = "Expected a boolean value for " + std::string(opt.key);
                return false;
            }
            *(bool*)opt.ptr = parsed;
            return true;
        }

        if (opt.type == CFG_INT) {
            *(int*)opt.ptr = std::stoi(Trim(rawValue));
            return true;
        }

        if (opt.type == CFG_FLOAT) {
            *(float*)opt.ptr = std::stof(Trim(rawValue));
            return true;
        }

        *(std::string*)opt.ptr = rawValue;
        return true;
    } catch (const std::exception&) {
        if (errorOut) *errorOut = "Invalid value for " + std::string(opt.key);
        return false;
    }
}

bool Config_SaveNow(std::string* errorOut) {
    std::error_code ec;
    fs::create_directories(ConfigDirPath(), ec);
    if (ec) {
        if (errorOut) *errorOut = "Unable to create config directory";
        return false;
    }

    std::ofstream file(Config_GetPath());
    if (!file.is_open()) {
        if (errorOut) *errorOut = "Unable to open config file for writing";
        return false;
    }

    for (const auto& opt : g_configRegistry) {
        file << opt.key << "=" << OptionValueToString(opt) << "\n";
    }

    return true;
}

static void Config_Load() {
    std::vector<fs::path> candidatePaths = {
        fs::path(Config_GetPath()),
        fs::current_path() / "config.ini",
    };

    for (const auto& path : candidatePaths) {
        std::ifstream file(path);
        if (!file.is_open()) continue;

        std::string line;
        while (std::getline(file, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            const std::string key = Trim(line.substr(0, eq));
            const std::string value = line.substr(eq + 1);
            const ConfigOption* opt = FindOptionByAnyKey(key);
            if (!opt) continue;

            AssignValueToOption(*opt, value, nullptr);
        }
        return;
    }
}

static void OnConfigChange() {
    Config_SaveNow(nullptr);
}

bool Config_GetValueByKey(const std::string& key, std::string* valueOut, std::string* errorOut) {
    const ConfigOption* opt = Config_FindOptionByKey(key);
    if (!opt) {
        if (errorOut) *errorOut = "Unknown config key: " + key;
        return false;
    }

    if (valueOut) *valueOut = OptionValueToString(*opt);
    return true;
}

bool Config_SetValueByKey(const std::string& key, const std::string& value, std::string* errorOut) {
    const ConfigOption* opt = Config_FindOptionByKey(key);
    if (!opt) {
        if (errorOut) *errorOut = "Unknown config key: " + key;
        return false;
    }

    if (!AssignValueToOption(*opt, value, errorOut)) return false;
    if (opt->onChange) opt->onChange();
    return true;
}

void Config_InitRegistry() {
    g_configRegistry.clear();

    // SECTION: Movement & Scale
    g_configRegistry.push_back({"Movement", "global_scale", "Global Scale", CFG_FLOAT, &g_config.globalScale, 0.5f, 3.0f, 0.05f, "", OnConfigChange});
    g_configRegistry.push_back({"Movement", "walk_speed", "Walk Speed", CFG_FLOAT, &g_config.baseWalkSpeed, 20.0f, 300.0f, 5.0f, "px", OnConfigChange});
    g_configRegistry.push_back({"Movement", "run_speed", "Run Speed", CFG_FLOAT, &g_config.baseRunSpeed, 100.0f, 800.0f, 10.0f, "px", OnConfigChange});

    // SECTION: Behavior
    g_configRegistry.push_back({"Behavior", "memes_enabled", "Allow Memes/Notes", CFG_BOOL, &g_config.memesEnabled, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Behavior", "multi_monitor_enabled", "Multi-Monitor Support", CFG_BOOL, &g_config.multiMonitorEnabled, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Behavior", "audio_enabled", "Audio (Honks)", CFG_BOOL, &g_config.audioEnabled, 0, 1, 1, "", OnConfigChange});

    // SECTION: Cursor
    g_configRegistry.push_back({"Cursor", "cursor_chase_enabled", "Default: Cursor Chase", CFG_BOOL, &g_config.cursorChaseEnabled, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Cursor", "cursor_chase_chance", "Default: Chase Chance", CFG_INT, &g_config.cursorChaseChance, 0, 100, 1, "%", OnConfigChange});
    g_configRegistry.push_back({"Cursor", "snatch_duration", "Default: Snatch Duration", CFG_FLOAT, &g_config.snatchDuration, 0.5f, 10.0f, 0.5f, "s", OnConfigChange});

    // SECTION: Mud
    g_configRegistry.push_back({"Mud", "mud_enabled", "Default: Enable Mud", CFG_BOOL, &g_config.mudEnabled, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Mud", "mud_chance", "Default: Mud Chance", CFG_INT, &g_config.mudChance, 0, 100, 1, "%", OnConfigChange});
    g_configRegistry.push_back({"Mud", "mud_lifetime", "Default: Footprint Life", CFG_FLOAT, &g_config.mudLifetime, 5.0f, 120.0f, 1.0f, "s", OnConfigChange});

    // SECTION: Debug
    g_configRegistry.push_back({"Debug", "debug_visuals", "Show Overlays", CFG_BOOL, &g_config.debugVisuals, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Debug", "debug_terminal", "Log to Terminal", CFG_BOOL, &g_config.debugToTerminal, 0, 1, 1, "", OnConfigChange});

    Config_Load();
    Config_SaveNow(nullptr);
}
