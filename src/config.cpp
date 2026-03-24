#include <iostream>
#include <fstream>
#include <sstream>
#include "config.h"

Config g_config;
double g_time = 0.0;
std::vector<ConfigOption> g_configRegistry;

static void Config_Save() {
    std::ofstream file("config.ini");
    if (!file.is_open()) return;

    for (const auto& opt : g_configRegistry) {
        file << opt.label << "=";
        if (opt.type == CFG_BOOL) file << (*(bool*)opt.ptr ? "1" : "0");
        else if (opt.type == CFG_INT) file << *(int*)opt.ptr;
        else if (opt.type == CFG_FLOAT) file << *(float*)opt.ptr;
        else if (opt.type == CFG_STRING) file << *(std::string*)opt.ptr;
        file << "\n";
    }
}

static void Config_Load() {
    std::ifstream file("config.ini");
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        for (auto& opt : g_configRegistry) {
            if (std::string(opt.label) == key) {
                if (opt.type == CFG_BOOL) *(bool*)opt.ptr = (val == "1");
                else if (opt.type == CFG_INT) *(int*)opt.ptr = std::stoi(val);
                else if (opt.type == CFG_FLOAT) *(float*)opt.ptr = std::stof(val);
                else if (opt.type == CFG_STRING) *(std::string*)opt.ptr = val;
            }
        }
    }
}

static void OnConfigChange() {
    Config_Save();
}

void Config_InitRegistry() {
    g_configRegistry.clear();

    // SECTION: Movement & Scale
    g_configRegistry.push_back({"Movement", "Global Scale", CFG_FLOAT, &g_config.globalScale, 0.5f, 3.0f, 0.05f, "", OnConfigChange});
    g_configRegistry.push_back({"Movement", "Walk Speed", CFG_FLOAT, &g_config.baseWalkSpeed, 20.0f, 300.0f, 5.0f, "px", OnConfigChange});
    g_configRegistry.push_back({"Movement", "Run Speed", CFG_FLOAT, &g_config.baseRunSpeed, 100.0f, 800.0f, 10.0f, "px", OnConfigChange});

    // SECTION: Behavior
    g_configRegistry.push_back({"Behavior", "Allow Memes/Notes", CFG_BOOL, &g_config.memesEnabled, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Behavior", "Multi-Monitor Support", CFG_BOOL, &g_config.multiMonitorEnabled, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Behavior", "Audio (Honks)", CFG_BOOL, &g_config.audioEnabled, 0, 1, 1, "", OnConfigChange});

    // SECTION: Cursor
    g_configRegistry.push_back({"Cursor", "Default: Cursor Chase", CFG_BOOL, &g_config.cursorChaseEnabled, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Cursor", "Default: Chase Chance", CFG_INT, &g_config.cursorChaseChance, 0, 100, 1, "%", OnConfigChange});
    g_configRegistry.push_back({"Cursor", "Default: Snatch Duration", CFG_FLOAT, &g_config.snatchDuration, 0.5f, 10.0f, 0.5f, "s", OnConfigChange});

    // SECTION: Mud
    g_configRegistry.push_back({"Mud", "Default: Enable Mud", CFG_BOOL, &g_config.mudEnabled, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Mud", "Default: Mud Chance", CFG_INT, &g_config.mudChance, 0, 100, 1, "%", OnConfigChange});
    g_configRegistry.push_back({"Mud", "Default: Footprint Life", CFG_FLOAT, &g_config.mudLifetime, 5.0f, 120.0f, 1.0f, "s", OnConfigChange});

    // SECTION: Debug
    g_configRegistry.push_back({"Debug", "Show Overlays", CFG_BOOL, &g_config.debugVisuals, 0, 1, 1, "", OnConfigChange});
    g_configRegistry.push_back({"Debug", "Log to Terminal", CFG_BOOL, &g_config.debugToTerminal, 0, 1, 1, "", OnConfigChange});

    Config_Load();
}
