#ifndef CONFIG_H
#define CONFIG_H

#include <vector>
#include <string>

enum ConfigType { CFG_BOOL, CFG_INT, CFG_FLOAT, CFG_STRING };

struct ConfigOption {
    const char* section;
    const char* label;
    ConfigType type;
    void* ptr;
    float min;
    float max;
    float step;
    const char* suffix; // e.g. "%", "s", "px"
    void (*onChange)() = nullptr;
};

struct Config {
    bool debugToTerminal = false;
    bool debugVisuals = false;
    bool geminiDebug = false;
    bool geminiLogRawResponses = false;
    float globalScale = 1.0f;
    bool audioEnabled = true;
    bool memesEnabled = true;
    bool whisperEnabled = false;
    float whisperVolumeNormalization = 0.8f;
    float baseWalkSpeed = 180.0f;
    float baseRunSpeed = 480.0f;

    // Cursor chase/snatch (Hyprland only)
    bool cursorChaseEnabled = true;
    int cursorChaseChance = 3;     // Percentage chance per wander cycle
    float snatchDuration = 3.0f;   // Seconds to hold cursor
    bool multiMonitorEnabled = true;

    // Mud tracking
    bool mudEnabled = true;
    int mudChance = 15;        // 0..100
    float mudLifetime = 15.0f; // seconds

    // AI
    std::string geminiApiKey = "";
    // Tool preferences
    std::string preferredCalculator = ""; // program name or full command
    std::string preferredBrowser = "";    // program name or full command
    bool toolsEnabled = true;
    std::string lastAssignmentSummary = "";
};

extern Config g_config;
extern double g_time;
extern std::vector<ConfigOption> g_configRegistry;

void Config_InitRegistry();

#endif // CONFIG_H
