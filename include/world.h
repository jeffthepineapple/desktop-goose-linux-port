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
};

struct Footprint {
    Vector2 pos;
    float dir;
    double timeSpawned;
    float lifetime;
};

extern std::list<Goose> g_geese;
extern std::list<MonitorInfo> g_monitors;
extern std::list<DroppedItem> g_droppedItems;
extern std::list<Footprint> g_footprints;
extern int g_nextId;
extern int g_screenWidth;
extern int g_screenHeight;
extern int g_selectedGooseId;
extern GtkWidget* g_entryNote;
extern std::deque<std::string> g_uiLog;
extern std::vector<GtkWidget*> g_overlayCanvases;
extern int g_cursorGrabberId; // id of goose currently dragging the cursor, -1 = none

void UiLogPush(const std::string& s);
Goose* GetGooseById(int id);

#endif // WORLD_H
