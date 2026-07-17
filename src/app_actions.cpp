#include "app_actions.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "glib.h"
#include "assets.h"
#include "config.h"
#include "cosmetics.h"
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

static bool ParseGooseId(const std::string& text, int* idOut) {
    if (text.empty()) return false;
    int id = -1;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, id);
    if (result.ec != std::errc() || result.ptr != end || id < 0) return false;
    if (idOut) *idOut = id;
    return true;
}

static Goose* FindCommandGoose(const std::string& text, std::string* errorOut) {
    int id = -1;
    if (!ParseGooseId(text, &id)) {
        if (errorOut) *errorOut = "invalid goose id: " + text;
        return nullptr;
    }
    Goose* goose = GetGooseById(id);
    if (!goose && errorOut) *errorOut = "goose not found: " + std::to_string(id);
    return goose;
}

static void QueueOverlayRedraw() {
    for (const MonitorInfo& monitor : g_monitors) {
        if (monitor.canvas) gtk_widget_queue_draw(monitor.canvas);
    }
}

static std::string FormatSkinState(const Goose& goose, bool includeOk) {
    std::ostringstream out;
    if (includeOk) out << "ok\n";
    out << "goose_id=" << goose.id << "\n";
    out << "goose_name=" << goose.name << "\n";
    out << "profile=" << Cosmetics_MatchingProfile(goose.skin) << "\n";
    out << "hat=" << goose.skin.hat << "\n";
    out << "glasses=" << goose.skin.glasses << "\n";
    return out.str();
}

static std::string GetSkinCatalog() {
    Cosmetics_Initialize();
    std::ostringstream out;
    out << "skins_config=" << Cosmetics_ConfigPath() << "\n";
    for (const CosmeticItem& item : Cosmetics_Items()) {
        out << Cosmetics_SlotName(item.slot) << "." << item.id << "=" << item.label << "\n";
    }
    for (const SkinProfile& profile : Cosmetics_BuiltInProfiles()) {
        out << "preset." << profile.id << "=" << profile.label << "|"
            << profile.skin.hat << "|" << profile.skin.glasses << "\n";
    }
    for (const SkinProfile& profile : Cosmetics_CustomProfiles()) {
        out << "profile." << profile.id << "=" << profile.label << "|"
            << profile.skin.hat << "|" << profile.skin.glasses << "\n";
    }
    return out.str();
}

static std::string HandleSkinsCommand(const std::vector<std::string>& args) {
    Cosmetics_Initialize();
    if (args.size() == 1) return GetSkinCatalog();
    if (args[1] == "list") {
        return args.size() == 2
            ? GetSkinCatalog()
            : "error usage: skins list\n";
    }

    if (args[1] == "show") {
        if (args.size() != 3) return "error usage: skins show <goose-id>\n";
        std::string error;
        Goose* goose = FindCommandGoose(args[2], &error);
        return goose ? FormatSkinState(*goose, false) : "error " + error + "\n";
    }

    if (args[1] == "equip") {
        if (args.size() != 4) return "error usage: skins equip <goose-id> <look>\n";
        std::string error;
        Goose* goose = FindCommandGoose(args[2], &error);
        if (!goose) return "error " + error + "\n";
        if (!Cosmetics_ApplyProfile(goose->skin, args[3], &error)) {
            return "error " + error + "\n";
        }
        QueueOverlayRedraw();
        UiLogPush("Equipped " + goose->name + " with skin " + args[3]);
        return FormatSkinState(*goose, true);
    }

    if (args[1] == "set") {
        if (args.size() != 5) {
            return "error usage: skins set <goose-id> <hat|glasses> <item|none>\n";
        }
        std::string error;
        Goose* goose = FindCommandGoose(args[2], &error);
        if (!goose) return "error " + error + "\n";
        CosmeticSlot slot;
        if (!Cosmetics_ParseSlot(args[3], &slot)) {
            return "error unknown skin slot: " + args[3] + " (use hat or glasses)\n";
        }
        if (!Cosmetics_SetItem(goose->skin, slot, args[4], &error)) {
            return "error " + error + "\n";
        }
        QueueOverlayRedraw();
        UiLogPush("Set " + goose->name + " " + args[3] + "=" + args[4]);
        return FormatSkinState(*goose, true);
    }

    if (args[1] == "save") {
        if (args.size() != 4) return "error usage: skins save <goose-id> <look>\n";
        std::string error;
        Goose* goose = FindCommandGoose(args[2], &error);
        if (!goose) return "error " + error + "\n";
        if (!Cosmetics_SaveProfile(args[3], goose->skin, &error)) {
            return "error " + error + "\n";
        }
        UiLogPush("Saved skin " + args[3]);
        return FormatSkinState(*goose, true);
    }

    if (args[1] == "delete") {
        if (args.size() != 3) return "error usage: skins delete <look>\n";
        std::string error;
        if (!Cosmetics_DeleteProfile(args[2], &error)) return "error " + error + "\n";
        UiLogPush("Deleted skin " + args[2]);
        return "ok deleted=" + args[2] + "\n";
    }

    return "error usage: skins [list|show|equip|set|save|delete]\n";
}


static bool ParseRuleAction(const std::string& text, RuleAction* out) {
    if (text == "wander") *out = RuleAction::Wander;
    else if (text == "meme" || text == "fetch-meme") *out = RuleAction::FetchMeme;
    else if (text == "note" || text == "fetch-note") *out = RuleAction::FetchNote;
    else if (text == "text" || text == "say") *out = RuleAction::FetchText;
    else if (text == "chase" || text == "chase-cursor") *out = RuleAction::Chase;
    else return false;
    return true;
}

static const char* RuleActionName(RuleAction action) {
    switch (action) {
        case RuleAction::Wander:    return "wander";
        case RuleAction::FetchMeme: return "meme";
        case RuleAction::FetchNote: return "note";
        case RuleAction::FetchText: return "text";
        case RuleAction::Chase:     return "chase";
    }
    return "?";
}

static void ApplyRuleAction(Goose& goose, const GooseRule& rule, int w, int h) {
    switch (rule.action) {
        case RuleAction::Wander:    goose.ForceWander(w, h); break;
        case RuleAction::FetchMeme: goose.ForceFetch(0, w, h); break;
        case RuleAction::FetchNote: goose.ForceFetch(1, w, h); break;
        case RuleAction::FetchText: goose.ForceFetchText(rule.text, w, h); break;
        case RuleAction::Chase:     goose.ForceChase(w, h); break;
    }
}

void Rules_Tick(double time, int w, int h) {
    for (auto& rule : g_rules) {
        if (rule.fired || time < rule.nextFire) continue;

        if (rule.target < 0) {
            for (auto& goose : g_geese) ApplyRuleAction(goose, rule, w, h);
        } else if (Goose* goose = GetGooseById(rule.target)) {
            ApplyRuleAction(*goose, rule, w, h);
        }

        if (rule.interval > 0.0) rule.nextFire = time + rule.interval;
        else rule.fired = true; // one-shot
    }

    g_rules.erase(std::remove_if(g_rules.begin(), g_rules.end(),
                                 [](const GooseRule& r) { return r.fired; }),
                  g_rules.end());
}

static std::string HandleFreezeCommand(const std::vector<std::string>& args) {
    if (args.size() > 2) return "error usage: freeze [on|off|toggle]\n";

    if (args.size() == 2) {
        const std::string& mode = args[1];
        if (mode == "on") g_frozen = true;
        else if (mode == "off") g_frozen = false;
        else if (mode == "toggle") g_frozen = !g_frozen;
        else return "error usage: freeze [on|off|toggle]\n";
    } else {
        g_frozen = true;
    }

    UiLogPush(g_frozen ? "Froze all geese" : "Unfroze all geese");
    return std::string("ok frozen=") + (g_frozen ? "1" : "0") + "\n";
}

static std::string FormatRuleLine(const GooseRule& rule) {
    std::ostringstream out;
    out << "rule." << rule.id << " target="
        << (rule.target < 0 ? "all" : std::to_string(rule.target))
        << " action=" << RuleActionName(rule.action);
    if (rule.action == RuleAction::FetchText) out << " text=\"" << rule.text << "\"";
    if (rule.interval > 0.0) out << " every=" << rule.interval << "s";
    else out << " once";
    out << "\n";
    return out.str();
}

static std::string GetRulesList() {
    if (g_rules.empty()) return "ok rules=0\n";
    std::ostringstream out;
    out << "ok rules=" << g_rules.size() << "\n";
    for (const GooseRule& rule : g_rules) out << FormatRuleLine(rule);
    return out.str();
}

static std::string HandleRulesCommand(const std::vector<std::string>& args) {
    if (args.size() == 1 || args[1] == "list") {
        if (args.size() > 2) return "error usage: rules list\n";
        return GetRulesList();
    }

    if (args[1] == "clear") {
        if (args.size() != 2) return "error usage: rules clear\n";
        size_t count = g_rules.size();
        g_rules.clear();
        UiLogPush("Cleared all rules");
        return "ok cleared=" + std::to_string(count) + "\n";
    }

    if (args[1] == "remove") {
        if (args.size() != 3) return "error usage: rules remove <rule-id>\n";
        int ruleId = -1;
        if (!ParseGooseId(args[2], &ruleId)) return "error invalid rule id: " + args[2] + "\n";
        auto before = g_rules.size();
        g_rules.erase(std::remove_if(g_rules.begin(), g_rules.end(),
                                     [ruleId](const GooseRule& r) { return r.id == ruleId; }),
                      g_rules.end());
        if (g_rules.size() == before) return "error rule not found: " + args[2] + "\n";
        UiLogPush("Removed rule " + args[2]);
        return "ok removed=" + args[2] + "\n";
    }

    if (args[1] == "add") {
        // rules add <goose-id|all> <action> [interval-seconds] [text...]
        if (args.size() < 4) {
            return "error usage: rules add <goose-id|all> <wander|meme|note|chase|text> [interval] [text]\n";
        }

        int target = -1;
        if (args[2] != "all" && !ParseGooseId(args[2], &target)) {
            return "error invalid goose id: " + args[2] + " (use a goose id or 'all')\n";
        }
        if (target >= 0 && !GetGooseById(target)) {
            return "error goose not found: " + args[2] + "\n";
        }

        RuleAction action;
        if (!ParseRuleAction(args[3], &action)) {
            return "error unknown action: " + args[3] + " (wander|meme|note|chase|text)\n";
        }

        double interval = 0.0;
        size_t textStart = 4;
        if (args.size() > 4) {
            const std::string& maybeInterval = args[4];
            double parsed = 0.0;
            const char* begin = maybeInterval.data();
            const char* end = begin + maybeInterval.size();
            auto result = std::from_chars(begin, end, parsed);
            if (result.ec == std::errc() && result.ptr == end) {
                if (parsed < 0.0) return "error interval must be >= 0\n";
                interval = parsed;
                textStart = 5;
            }
        }

        std::string text;
        for (size_t i = textStart; i < args.size(); ++i) {
            if (!text.empty()) text += " ";
            text += args[i];
        }
        if (action == RuleAction::FetchText && text.empty()) {
            return "error text action requires a message: rules add <target> text [interval] <message>\n";
        }

        GooseRule rule;
        rule.id = g_nextRuleId++;
        rule.target = target;
        rule.action = action;
        rule.text = text;
        rule.interval = interval;
        // Repeating rules first fire after one interval; one-shot rules fire on the next tick.
        rule.nextFire = (interval > 0.0) ? g_time + interval : g_time;
        g_rules.push_back(rule);

        UiLogPush("Added rule " + std::to_string(rule.id) + " (" + RuleActionName(action) + ")");
        return "ok " + FormatRuleLine(rule);
    }

    return "error usage: rules [list|add|remove|clear]\n";
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
    out << "frozen=" << (g_frozen ? 1 : 0) << "\n";
    out << "rule_count=" << g_rules.size() << "\n";

    for (const auto& opt : g_configRegistry) {
        std::string value;
        Config_GetValueByKey(opt.key, &value, nullptr);
        out << opt.key << "=" << value << "\n";
    }

    for (const auto& goose : g_geese) {
        out << "goose." << goose.id << ".name=" << goose.name << "\n";
        out << "goose." << goose.id << ".skin="
            << Cosmetics_MatchingProfile(goose.skin) << "\n";
        out << "goose." << goose.id << ".hat=" << goose.skin.hat << "\n";
        out << "goose." << goose.id << ".glasses=" << goose.skin.glasses << "\n";
    }

    return out.str();
}

// Command handler table. Adding a daemon command = one CliCommandSpec row in
// cli_registry.cpp plus one entry here.
using CommandHandlerFn = std::string (*)(const std::vector<std::string>&);

static std::string HandleSpawnCommand(const std::vector<std::string>& args) {
    Goose* goose = AppActions_SpawnGoose(args.size() > 1 ? args[1] : "");
    return "ok id=" + std::to_string(goose ? goose->id : -1) + "\n";
}

static std::string HandleClearCommand(const std::vector<std::string>&) {
    AppActions_ClearGeese();
    return "ok\n";
}

static std::string HandleStatusCommand(const std::vector<std::string>&) {
    return AppActions_GetStatus();
}

static std::string HandleRamCommand(const std::vector<std::string>&) {
    return GetRamUsageReport();
}

static std::string HandleQuitCommand(const std::vector<std::string>&) {
    AppActions_ClearGeese();
    AppActions_Quit();
    return "ok cleared and quitting\n";
}

std::string AppActions_HandleCommand(const std::vector<std::string>& args) {
    static const std::map<std::string, CommandHandlerFn> handlers = {
        {"spawn",    HandleSpawnCommand},
        {"clear",    HandleClearCommand},
        {"status",   HandleStatusCommand},
        {"settings", HandleSettingsCommand},
        {"skins",    HandleSkinsCommand},
        {"ram",      HandleRamCommand},
        {"freeze",   HandleFreezeCommand},
        {"rules",    HandleRulesCommand},
        {"quit",     HandleQuitCommand},
    };

    if (args.empty()) return "error missing command\n";

    const auto handler = handlers.find(args.front());
    if (handler == handlers.end()) {
        return "error unknown command: " + args.front() + "\n";
    }
    return handler->second(args);
}
