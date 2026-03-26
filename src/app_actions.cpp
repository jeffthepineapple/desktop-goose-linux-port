#include "app_actions.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include "glib.h"
#include "config.h"
#include "world.h"

static GtkApplication* g_appActionsApp = nullptr;

void AppActions_SetApplication(GtkApplication* app) {
    g_appActionsApp = app;
}

Goose* AppActions_SpawnGoose(const std::string& requestedName) {
    std::string name = requestedName;
    if (name.empty()) {
        name = "Goose " + std::to_string(g_nextId);
    }

    g_geese.emplace_back(g_nextId++, name, g_screenWidth, g_screenHeight);
    UiLogPush("Spawned " + name);
    return &g_geese.back();
}

void AppActions_EnsureInitialGoose() {
    if (!g_geese.empty()) return;
    AppActions_SpawnGoose("");
}

void AppActions_ClearGeese() {
    for (auto& item : g_droppedItems) {
        delete item.data;
    }
    g_droppedItems.clear();
    g_footprints.clear();
    g_geese.clear();
    g_cursorGrabberId = -1;
    g_selectedGooseId = 0;
    g_nextId = 0;
    for (GtkWidget* canvas : g_overlayCanvases) {
        if (canvas) gtk_widget_queue_draw(canvas);
    }
    UiLogPush("Cleared all geese.");
}

static gboolean QuitAfterClearFrame(gpointer data) {
    GtkApplication* app = static_cast<GtkApplication*>(data);
    if (app) g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

void AppActions_Quit() {
    if (!g_appActionsApp) return;
    g_timeout_add(50, QuitAfterClearFrame, g_appActionsApp);
}

static std::string GetRamUsageReport() {
    std::ifstream file("/proc/self/status");
    std::string line;
    long rssKb = -1;
    long hwmKb = -1;
    long vmKb = -1;

    while (std::getline(file, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream(line.substr(6)) >> rssKb;
        } else if (line.rfind("VmHWM:", 0) == 0) {
            std::istringstream(line.substr(6)) >> hwmKb;
        } else if (line.rfind("VmSize:", 0) == 0) {
            std::istringstream(line.substr(7)) >> vmKb;
        }
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    if (rssKb >= 0) out << "ram_rss_mb=" << (rssKb / 1024.0) << "\n";
    if (hwmKb >= 0) out << "ram_peak_mb=" << (hwmKb / 1024.0) << "\n";
    if (vmKb >= 0) out << "ram_virtual_mb=" << (vmKb / 1024.0) << "\n";
    return out.str();
}

std::string AppActions_GetStatus() {
    std::ostringstream out;
    out << "running=1\n";
    out << "goose_count=" << g_geese.size() << "\n";
    out << "config_path=" << Config_GetPath() << "\n";
    out << GetRamUsageReport();

    for (const auto& opt : g_configRegistry) {
        std::string value;
        Config_GetValueByKey(opt.key, &value, nullptr);
        out << opt.key << "=" << value << "\n";
    }

    return out.str();
}

std::string AppActions_HandleCommand(const std::vector<std::string>& args) {
    if (args.empty()) return "error missing command\n";

    const std::string& command = args.front();
    if (command == "spawn") {
        Goose* goose = AppActions_SpawnGoose(args.size() > 1 ? args[1] : "");
        return "ok id=" + std::to_string(goose ? goose->id : -1) + "\n";
    }

    if (command == "clear") {
        AppActions_ClearGeese();
        return "ok\n";
    }

    if (command == "status") {
        return AppActions_GetStatus();
    }

    if (command == "ram") {
        return GetRamUsageReport();
    }

    if (command == "quit") {
        AppActions_ClearGeese();
        AppActions_Quit();
        return "ok cleared and quitting\n";
    }

    return "error unknown command: " + command + "\n";
}
