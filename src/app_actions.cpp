#include "app_actions.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include "glib.h"
#include "assets.h"
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
    for (const auto& monitor : g_monitors) {
        if (monitor.canvas) gtk_widget_queue_draw(monitor.canvas);
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

static const char* ConfigTypeName(ConfigType type) {
    if (type == CFG_BOOL) return "bool";
    if (type == CFG_INT) return "int";
    if (type == CFG_FLOAT) return "float";
    return "string";
}

static std::string GetSettingValue(const ConfigOption& opt) {
    std::string value;
    Config_GetValueByKey(opt.key, &value, nullptr);
    return value;
}

static std::string FormatSettingLine(const ConfigOption& opt) {
    std::ostringstream out;
    out << opt.key << "=" << GetSettingValue(opt)
        << " label=\"" << opt.label << "\""
        << " section=\"" << opt.section << "\""
        << " type=" << ConfigTypeName(opt.type);

    if (opt.type == CFG_INT || opt.type == CFG_FLOAT) {
        out << " min=" << opt.min << " max=" << opt.max << " step=" << opt.step;
    }
    if (opt.suffix && *opt.suffix) out << " suffix=\"" << opt.suffix << "\"";
    out << "\n";
    return out.str();
}

static std::string FormatSettingRange(const ConfigOption& opt) {
    if (opt.type != CFG_INT && opt.type != CFG_FLOAT) return "-";

    std::ostringstream out;
    out << opt.min << ".." << opt.max;
    if (opt.suffix && *opt.suffix) out << " " << opt.suffix;
    return out.str();
}

static std::string GetSettingsList() {
    std::ostringstream out;
    out << "Config: " << Config_GetPath() << "\n\n";
    out << std::left
        << std::setw(24) << "Key"
        << std::setw(10) << "Value"
        << std::setw(10) << "Type"
        << std::setw(16) << "Range"
        << "Label\n";
    out << std::string(82, '-') << "\n";
    for (const auto& opt : g_configRegistry) {
        out << std::left
            << std::setw(24) << opt.key
            << std::setw(10) << GetSettingValue(opt)
            << std::setw(10) << ConfigTypeName(opt.type)
            << std::setw(16) << FormatSettingRange(opt)
            << opt.label << "\n";
    }
    return out.str();
}

static std::string HandleSettingsCommand(const std::vector<std::string>& args) {
    if (args.size() == 1 || args[1] == "list") {
        return GetSettingsList();
    }

    if (args[1] == "get") {
        if (args.size() != 3) return "error usage: settings get <key>\n";

        const ConfigOption* opt = Config_FindOptionByName(args[2]);
        if (!opt) return "error unknown setting: " + args[2] + "\n";
        return FormatSettingLine(*opt);
    }

    if (args[1] == "set") {
        if (args.size() != 4) return "error usage: settings set <key> <value>\n";

        const ConfigOption* opt = Config_FindOptionByName(args[2]);
        if (!opt) return "error unknown setting: " + args[2] + "\n";

        std::string error;
        if (!Config_SetValueByKey(opt->key, args[3], &error)) {
            return "error " + error + "\n";
        }

        std::string value;
        Config_GetValueByKey(opt->key, &value, nullptr);
        UiLogPush("Set " + std::string(opt->key) + "=" + value);
        return "ok " + std::string(opt->key) + "=" + value + "\n";
    }

    return "error usage: settings [list|get|set]\n";
}

std::string AppActions_GetStatus() {
    std::ostringstream out;
    out << "running=1\n";
    out << "goose_count=" << g_geese.size() << "\n";
    out << "config_path=" << Config_GetPath() << "\n";
    out << "asset_root=" << ASSET_ROOT.string() << "\n";
    out << "meme_count=" << g_assets.memePaths.size() << "\n";
    out << "note_count=" << g_assets.textPaths.size() << "\n";
    out << GetRamUsageReport();

    for (const auto& opt : g_configRegistry) {
        std::string value;
        Config_GetValueByKey(opt.key, &value, nullptr);
        out << opt.key << "=" << value << "\n";
    }

    for (const auto& goose : g_geese) {
        out << "goose." << goose.id << ".name=" << goose.name << "\n";
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

    if (command == "settings") {
        return HandleSettingsCommand(args);
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
