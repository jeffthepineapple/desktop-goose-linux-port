// ===========================
// world.h
// ===========================
#ifndef WORLD_H
#define WORLD_H

#include <list>
#include <string>
#include <deque>
#include <vector>
#include <gtk/gtk.h>
#include "goose.h"
#include "items.h"

struct MonitorInfo {
    int x, y, width, height;
    GdkMonitor* monitor;
    GtkWindow* window = nullptr;
    GtkWidget* canvas = nullptr;
};

struct Footprint {
    Vector2 pos;
    float dir;
    double timeSpawned;
    float lifetime;
};

extern std::vector<Goose> g_geese;
extern std::list<MonitorInfo> g_monitors;
extern std::list<DroppedItem> g_droppedItems;
extern std::list<Footprint> g_footprints;
extern int g_nextId;
extern int g_screenWidth;
extern int g_screenHeight;
extern int g_selectedGooseId;
extern GtkWidget* g_entryNote;
extern std::deque<std::string> g_uiLog;
extern int g_cursorGrabberId; // id of goose currently dragging the cursor, -1 = none
extern int g_windowDragGooseId; // id of goose currently dragging a window, -1 = none
extern int g_heldGooseId; // id of goose currently held by the cursor, -1 = none
extern bool g_suppressOverlayForCapture;

extern bool g_frozen; // when true, all goose updates and rules are paused

// --- Custom rule engine ---
// A rule forces matching geese to perform an action, either once or on a repeating interval.
enum class RuleAction { Wander, FetchMeme, FetchNote, FetchText, Chase, DragWindow, YeetWindow };

struct GooseRule {
    int id = 0;                          // stable rule identifier
    int target = -1;                     // goose id, or -1 for all geese
    RuleAction action = RuleAction::Wander;
    std::string text;                    // payload for FetchText
    double interval = 0.0;               // seconds between fires; <= 0 means fire once
    double nextFire = 0.0;               // absolute time of next fire
    bool fired = false;                  // set when a one-shot rule has run
};

extern std::vector<GooseRule> g_rules;
extern int g_nextRuleId;

void UiLogPush(const std::string& s);
Goose* GetGooseById(int id);

#endif // WORLD_H
