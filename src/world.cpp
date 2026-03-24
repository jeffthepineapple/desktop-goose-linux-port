// ===========================
// world.cpp
// ===========================
#include "world.h"

std::deque<std::string> g_uiLog;
static const size_t UI_LOG_MAX = 12;

void UiLogPush(const std::string& s) {
    g_uiLog.push_back(s);
    if (g_uiLog.size() > UI_LOG_MAX) g_uiLog.pop_front();
}

std::list<Goose> g_geese;
std::list<MonitorInfo> g_monitors;
std::list<DroppedItem> g_droppedItems;
std::list<Footprint> g_footprints;
int g_nextId = 0;
int g_screenWidth = 1920;
int g_screenHeight = 1080;
int g_selectedGooseId = 0;
GtkWidget* g_entryNote = nullptr;
int g_cursorGrabberId = -1;

Goose* GetGooseById(int id) {
    for (auto& g : g_geese) {
        if (g.id == id) return &g;
    }
    return nullptr;
}
