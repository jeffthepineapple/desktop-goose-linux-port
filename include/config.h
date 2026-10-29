#ifndef CONFIG_H
#define CONFIG_H

#include <vector>
#include <string>

enum ConfigType { CFG_BOOL, CFG_INT, CFG_FLOAT, CFG_STRING };

struct ConfigOption {
    const char* section;
    const char* key;
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
    float globalScale = 1.0f;
    bool audioEnabled = true;
    bool memesEnabled = true;
    bool fortuneEnabled = false;   // pull some note text from the `fortune` program when available
    int fortuneChance = 50;        // 0..100 chance a fetched note uses fortune instead of a bundled note
    float baseWalkSpeed = 180.0f;
    float baseRunSpeed = 480.0f;

    // Cursor chase/snatch (requires cursor-position tracking)
    bool cursorChaseEnabled = true;
    int cursorChaseChance = 3;     // Percentage chance per wander cycle
    float snatchDuration = 3.0f;   // Seconds to hold cursor
    bool multiMonitorEnabled = true;

    // Mud tracking
    bool mudEnabled = true;
    int mudChance = 15;        // 0..100
    float mudLifetime = 15.0f; // seconds

    // Cursor pickup ragdoll physics
    float ragdollGravity = 1400.0f;
    float ragdollDamping = 2.4f;
    float ragdollSquirmStrength = 1.0f;

    // Soft-body pickup/yeet physics
    float softbodyStiffness = 1.0f;      // spring stiffness scale 0.25..2
    float softbodyPressure = 0.5f;       // area preservation 0..1
    float softbodyShapeMemory = 0.45f;   // pull back to the rest pose 0..1
    float softbodyGrabStiffness = 0.4f;  // cursor pull per sub-step 0.05..1
    float softbodyAirDrag = 0.5f;        // extra velocity damping in flight
    float softbodyBounce = 0.45f;        // wall restitution 0..0.95
    float softbodyFriction = 0.35f;      // contact friction 0..1
    float softbodyYeetSpeed = 420.0f;    // px/s flick that counts as a throw
    int softbodyIterations = 4;          // constraint iterations 1..12
    int softbodySubsteps = 3;            // physics substeps 1..6

    // Edge detection debug visuals (requires compositor window information)
    bool highlightEdgeWindows = false;

    // Window drag (Hyprland and niri)
    bool windowDragEnabled = true;
    int windowDragChance = 3;       // 0..100 percentage per wander cycle
    int windowDragEdgePadding = 40;       // px edge padding for destination bounds
    int windowYeetChance = 35;      // 0..100 share of window grabs that become yeets

    // Goose-on-goose yeet
    bool gooseYeetEnabled = true;
    int gooseYeetChance = 4;          // 0..100 percentage per wander cycle
    int gooseYeetMinDistance = 400;   // px the throw's aim point must clear
};

extern Config g_config;
extern double g_time;
extern std::vector<ConfigOption> g_configRegistry;

void Config_InitRegistry();
std::string Config_GetPath();
const ConfigOption* Config_FindOptionByKey(const std::string& key);
const ConfigOption* Config_FindOptionByName(const std::string& keyOrLabel);
bool Config_GetValueByKey(const std::string& key, std::string* valueOut, std::string* errorOut = nullptr);
bool Config_SetValueByKey(const std::string& key, const std::string& value, std::string* errorOut = nullptr);
bool Config_SaveNow(std::string* errorOut = nullptr);

#endif // CONFIG_H
