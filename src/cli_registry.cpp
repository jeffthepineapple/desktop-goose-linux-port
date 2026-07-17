#include "cli_registry.h"

#include <algorithm>

namespace {

constexpr CliComplete N = CliComplete::None;

const std::vector<CliCommandSpec> kRegistry = {
    // name       sub       page        args                              summary                                  example                        offline local  argComplete
    {"start",    nullptr,  "daemon",   "[name] [--foreground]",          "Launch the flock",                       "start \"Pip\"",              true,  true,  {N, N, N}},
    {"shell",    nullptr,  "daemon",   "",                               "Open the interactive goose shell",       nullptr,                      false, true,  {N, N, N}},
    {"status",   nullptr,  "daemon",   "",                               "Inspect runtime state",                  nullptr,                      false, false, {N, N, N}},
    {"help",     nullptr,  "daemon",   "[page|command|all]",             "Show this help",                         "help skins",                 true,  true,  {N, N, N}},
    {"quit",     nullptr,  "daemon",   "",                               "Clear every goose and stop",             nullptr,                      false, false, {N, N, N}},

    {"spawn",    nullptr,  "flock",    "[name]",                         "Add a goose",                            "spawn \"Pip\"",              false, false, {N, N, N}},
    {"clear",    nullptr,  "flock",    "",                               "Remove every goose",                     nullptr,                      false, false, {N, N, N}},
    {"freeze",   nullptr,  "flock",    "[on|off|toggle]",                "Freeze or unfreeze every goose",         nullptr,                      false, false, {CliComplete::OnOffToggle, N, N}},
    {"ram",      nullptr,  "flock",    "",                               "Show memory telemetry",                  nullptr,                      false, false, {N, N, N}},

    {"skins",    "list",   "skins",    "",                               "List items and saved looks",             nullptr,                      true,  false, {N, N, N}},
    {"skins",    "show",   "skins",    "<id>",                           "Inspect one goose's outfit",             nullptr,                      false, false, {CliComplete::GooseId, N, N}},
    {"skins",    "equip",  "skins",    "<id> <look>",                    "Equip a preset or saved look",           "skins equip 0 party",        false, false, {CliComplete::GooseId, CliComplete::Look, N}},
    {"skins",    "set",    "skins",    "<id> <hat|glasses> <item|none>", "Mix hat and glasses",                    "skins set 0 glasses shades", false, false, {CliComplete::GooseId, CliComplete::Slot, CliComplete::Item}},
    {"skins",    "save",   "skins",    "<id> <look>",                    "Save the current outfit as a look",      nullptr,                      false, false, {CliComplete::GooseId, N, N}},
    {"skins",    "delete", "skins",    "<look>",                         "Delete a saved look",                    nullptr,                      true,  false, {CliComplete::Look, N, N}},

    {"settings", "list",   "settings", "",                               "List every setting",                     nullptr,                      true,  false, {N, N, N}},
    {"settings", "get",    "settings", "<key>",                          "Read one value",                         "settings get walk_speed",    true,  false, {CliComplete::SettingKey, N, N}},
    {"settings", "set",    "settings", "<key> <value>",                  "Change one value",                       "settings set mud_chance 30", true,  false, {CliComplete::SettingKey, N, N}},

    {"rules",    "list",   "rules",    "",                               "List active rules",                      nullptr,                      false, false, {N, N, N}},
    {"rules",    "add",    "rules",    "<id|all> <action> [interval] [text]", "Schedule wander|meme|note|chase|text", "rules add all meme 30",   false, false, {CliComplete::GooseId, CliComplete::RuleAction, N}},
    {"rules",    "remove", "rules",    "<rule-id>",                      "Delete one rule",                        nullptr,                      false, false, {CliComplete::RuleId, N, N}},
    {"rules",    "clear",  "rules",    "",                               "Delete every rule",                      nullptr,                      false, false, {N, N, N}},

    {"force",    "set",    "force",    "<id> <wander|meme|note|chase>",   "Immediately set one goose's behavior",   "force set 0 wander",       false, false, {CliComplete::GooseId, CliComplete::ForceBehavior, N}},
};

// Damerau-free edit distance; small strings, small table, good enough for typos.
size_t EditDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

} // namespace

const std::vector<CliCommandSpec>& Cli_Registry() {
    return kRegistry;
}

std::vector<std::string> Cli_Pages() {
    std::vector<std::string> pages;
    for (const CliCommandSpec& spec : kRegistry) {
        if (std::find(pages.begin(), pages.end(), spec.page) == pages.end()) {
            pages.push_back(spec.page);
        }
    }
    return pages;
}

bool Cli_IsControlCommand(const std::string& name) {
    for (const CliCommandSpec& spec : kRegistry) {
        if (name == spec.name) return true;
    }
    return false;
}

const CliCommandSpec* Cli_FindSpec(const std::string& name, const std::string& sub) {
    const CliCommandSpec* fallback = nullptr;
    for (const CliCommandSpec& spec : kRegistry) {
        if (name != spec.name) continue;
        if (!spec.sub) return &spec; // bare command; sub token is an argument
        if (sub == spec.sub) return &spec;
        // Bare invocation of a grouped command resolves to its "list" form.
        if (sub.empty() && std::string(spec.sub) == "list") fallback = &spec;
    }
    return fallback;
}

bool Cli_WorksOffline(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    const CliCommandSpec* spec =
        Cli_FindSpec(args.front(), args.size() > 1 ? args[1] : "");
    return spec && spec->offline;
}

std::vector<std::string> Cli_Subcommands(const std::string& name) {
    std::vector<std::string> subs;
    for (const CliCommandSpec& spec : kRegistry) {
        if (name == spec.name && spec.sub) subs.push_back(spec.sub);
    }
    return subs;
}

std::string Cli_Suggest(const std::string& name) {
    std::string best;
    size_t bestDistance = 3; // anything further is noise
    for (const CliCommandSpec& spec : kRegistry) {
        if (best == spec.name) continue;
        const size_t distance = EditDistance(name, spec.name);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = spec.name;
        }
    }
    return best;
}

std::string Cli_Usage(const CliCommandSpec& spec) {
    std::string usage = spec.name;
    if (spec.sub) usage += std::string(" ") + spec.sub;
    if (spec.args[0] != '\0') usage += std::string(" ") + spec.args;
    return usage;
}
