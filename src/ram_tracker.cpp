// ram_tracker.cpp
#include "ram_tracker.h"
#include "world.h"
#include <glib.h>
#include <fstream>
#include <sstream>
#include <string>

static guint g_ram_timer_id = 0;

static gboolean ram_tracker_tick(gpointer user_data) {
    std::ifstream f("/proc/self/status");
    std::string line;
    long rss_kb = -1;
    long vms_kb = -1;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line);
            std::string key, value, unit;
            if (iss >> key >> value >> unit) {
                try { rss_kb = std::stol(value); } catch(...) { }
            }
        } else if (line.rfind("VmSize:", 0) == 0) {
            std::istringstream iss(line);
            std::string key, value, unit;
            if (iss >> key >> value >> unit) {
                try { vms_kb = std::stol(value); } catch(...) { }
            }
        }
    }

    char buf[128];
    if (rss_kb >= 0 && vms_kb >= 0) {
        snprintf(buf, sizeof(buf), "RAM: RSS=%ld KiB VSZ=%ld KiB", rss_kb, vms_kb);
    } else if (rss_kb >= 0) {
        snprintf(buf, sizeof(buf), "RAM: RSS=%ld KiB", rss_kb);
    } else {
        snprintf(buf, sizeof(buf), "RAM: (unknown)");
    }

    UiLogPush(std::string(buf));
    return G_SOURCE_CONTINUE;
}

void RamTracker_Init() {
    if (g_ram_timer_id == 0) {
        g_ram_timer_id = g_timeout_add_seconds(1, ram_tracker_tick, NULL);
    }
}

void RamTracker_Shutdown() {
    if (g_ram_timer_id != 0) {
        g_source_remove(g_ram_timer_id);
        g_ram_timer_id = 0;
    }
}
