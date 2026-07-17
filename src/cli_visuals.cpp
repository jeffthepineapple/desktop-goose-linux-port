#include "cli_visuals.h"
#include "cli_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>
#include <unistd.h>

namespace {

constexpr const char* RESET = "\033[0m";
constexpr const char* BOLD = "\033[1m";
constexpr const char* ORANGE = "\033[38;5;208m";
constexpr const char* CREAM = "\033[38;5;230m";
constexpr const char* STEEL = "\033[38;5;245m";
constexpr const char* CYAN = "\033[38;5;81m";
constexpr const char* GREEN = "\033[38;5;114m";
constexpr const char* RED = "\033[38;5;203m";

bool IsInteractive() {
    const char* term = std::getenv("TERM");
    return isatty(STDOUT_FILENO) && (!term || std::string(term) != "dumb");
}

bool UseColor() {
    return IsInteractive() && std::getenv("NO_COLOR") == nullptr;
}

std::string Paint(const char* color, const std::string& text, bool bold = false) {
    if (!UseColor()) return text;
    return std::string(bold ? BOLD : "") + color + text + RESET;
}

using Fields = std::vector<std::pair<std::string, std::string>>;

Fields ParseFields(const std::string& response) {
    Fields fields;
    std::istringstream input(response);
    std::string line;
    while (std::getline(input, line)) {
        const size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        fields.emplace_back(line.substr(0, separator), line.substr(separator + 1));
    }
    return fields;
}

std::string GetField(const Fields& fields, const std::string& key,
                     const std::string& fallback = "-") {
    for (const auto& field : fields) {
        if (field.first == key) return field.second;
    }
    return fallback;
}

std::string OnOff(const std::string& value) {
    return value == "1" ? Paint(GREEN, "on") : Paint(STEEL, "off");
}

void PrintRule() {
    std::cout << Paint(STEEL, std::string(68, '-')) << '\n';
}

void PrintHeader(const std::string& title, const std::string& state = "") {
    std::cout << Paint(ORANGE, "GOOSE CONTROL", true)
              << Paint(STEEL, " // ")
              << Paint(CREAM, title, true);
    if (!state.empty()) std::cout << "  " << state;
    std::cout << '\n';
    PrintRule();
}

std::string Shorten(const std::string& value, size_t width) {
    if (value.size() <= width) return value;
    if (width <= 3) return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

std::string Pad(const std::string& value, size_t width) {
    const std::string shortened = Shorten(value, width);
    return shortened + std::string(width > shortened.size() ? width - shortened.size() : 0, ' ');
}

void PrintStatus(const std::string& response) {
    const Fields fields = ParseFields(response);
    const std::string count = GetField(fields, "goose_count", "0");
    PrintHeader("STATUS", Paint(GREEN, "ONLINE"));

    std::cout << Paint(ORANGE, "FLOCK", true) << "  "
              << Paint(CREAM, count, true) << " active"
              << "    " << Paint(ORANGE, "ASSETS", true) << "  "
              << GetField(fields, "meme_count", "0") << " memes / "
              << GetField(fields, "note_count", "0") << " notes"
              << "    " << Paint(ORANGE, "MEM", true) << "  "
              << GetField(fields, "ram_rss_mb") << " MB\n\n";

    struct GooseRow {
        std::string name = "-";
        std::string profile = "custom";
        std::string hat = "none";
        std::string glasses = "none";
        std::string behavior = "wander";
        std::string traitSeed = "0";
        std::string traitAttack = "0";
        std::string traitMeme = "0";
        std::string traitNote = "0";
    };
    std::map<int, GooseRow> geese;
    for (const auto& field : fields) {
        if (field.first.rfind("goose.", 0) != 0) continue;
        const size_t propertyDot = field.first.find('.', 6);
        if (propertyDot == std::string::npos) continue;
        const std::string idText = field.first.substr(6, propertyDot - 6);
        char* end = nullptr;
        const long id = std::strtol(idText.c_str(), &end, 10);
        if (!end || *end != '\0') continue;
        const std::string property = field.first.substr(propertyDot + 1);
        GooseRow& row = geese[(int)id];
        if (property == "name") row.name = field.second;
        else if (property == "skin") row.profile = field.second;
        else if (property == "hat") row.hat = field.second;
        else if (property == "glasses") row.glasses = field.second;
        else if (property == "behavior") row.behavior = field.second;
        else if (property == "trait_seed") row.traitSeed = field.second;
        else if (property == "trait_attack") row.traitAttack = field.second;
        else if (property == "trait_meme") row.traitMeme = field.second;
        else if (property == "trait_note") row.traitNote = field.second;
    }

    std::cout << Paint(CYAN, "FLOCK MANIFEST", true) << '\n';
    if (geese.empty()) {
        std::cout << "  No geese airborne. Try: CppGoose spawn \"Pip\"\n";
    } else {
        std::cout << Paint(STEEL, "  ID  NAME                 SKIN          HAT          GLASSES") << '\n';
        for (const auto& entry : geese) {
            const GooseRow& goose = entry.second;
            const std::string id = "#" + std::to_string(entry.first);
            std::cout << "  " << Paint(ORANGE, Pad(id, 4), true)
                      << Pad(goose.name, 21)
                      << Pad(goose.profile, 14)
                      << Pad(goose.hat, 13)
                      << Shorten(goose.glasses, 12) << '\n'
                      << Paint(STEEL, "      behavior " + goose.behavior +
                                             "  traits attack:" + goose.traitAttack +
                                             " meme:" + goose.traitMeme +
                                             " note:" + goose.traitNote +
                                             "  seed:" + Shorten(goose.traitSeed, 12))
                      << '\n';
        }
    }

    std::cout << '\n' << Paint(CYAN, "BEHAVIOR", true) << '\n'
              << "  chase " << OnOff(GetField(fields, "cursor_chase_enabled", "0"))
              << " @ " << GetField(fields, "cursor_chase_chance", "0") << "%"
              << "    mud " << OnOff(GetField(fields, "mud_enabled", "0"))
              << " @ " << GetField(fields, "mud_chance", "0") << "%"
              << "    audio " << OnOff(GetField(fields, "audio_enabled", "0"))
              << "    monitors " << OnOff(GetField(fields, "multi_monitor_enabled", "0"))
              << '\n';

    std::cout << '\n' << Paint(CYAN, "FILES", true) << '\n'
              << "  config  " << GetField(fields, "config_path") << '\n'
              << "  assets  " << GetField(fields, "asset_root") << '\n'
              << '\n' << Paint(STEEL, "  More: CppGoose settings | CppGoose skins") << '\n';
}

void PrintRam(const std::string& response) {
    const Fields fields = ParseFields(response);
    PrintHeader("MEMORY TELEMETRY");
    std::cout << "  " << Paint(CYAN, "CURRENT", true) << "  "
              << GetField(fields, "ram_rss_mb") << " MB\n"
              << "  " << Paint(CYAN, "PEAK", true) << "     "
              << GetField(fields, "ram_peak_mb") << " MB\n"
              << "  " << Paint(CYAN, "VIRTUAL", true) << "  "
              << GetField(fields, "ram_virtual_mb") << " MB\n";
}

void PrintSettings(const std::string& response) {
    PrintHeader("SETTINGS");
    std::istringstream input(response);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("Config:", 0) == 0) {
            std::cout << Paint(STEEL, line) << '\n';
        } else if (line.rfind("Key", 0) == 0) {
            std::cout << Paint(CYAN, line, true) << '\n';
        } else if (!line.empty() && line.find_first_not_of('-') == std::string::npos) {
            std::cout << Paint(STEEL, line) << '\n';
        } else {
            std::cout << line << '\n';
        }
    }
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::istringstream input(value);
    std::string part;
    while (std::getline(input, part, delimiter)) parts.push_back(part);
    return parts;
}

void PrintSkinCatalog(const std::string& response) {
    const Fields fields = ParseFields(response);
    PrintHeader("SKIN LOCKER");

    auto printGroup = [&](const std::string& prefix, const std::string& title) {
        std::cout << Paint(CYAN, title, true) << '\n';
        bool found = false;
        for (const auto& field : fields) {
            if (field.first.rfind(prefix, 0) != 0) continue;
            found = true;
            const std::string id = field.first.substr(prefix.size());
            const std::vector<std::string> parts = Split(field.second, '|');
            std::cout << "  " << Paint(ORANGE, Pad(id, 18), true)
                      << (parts.empty() ? "" : parts[0]);
            if (parts.size() >= 3) {
                std::cout << Paint(STEEL, "  [hat: " + parts[1] + ", glasses: " + parts[2] + "]");
            }
            std::cout << '\n';
        }
        if (!found) std::cout << Paint(STEEL, "  none saved") << '\n';
        std::cout << '\n';
    };

    printGroup("hat.", "HATS");
    printGroup("glasses.", "GLASSES");
    printGroup("preset.", "BUILT-IN LOOKS");
    printGroup("profile.", "SAVED LOOKS");
    std::cout << Paint(STEEL, "  Equip: CppGoose skins equip <goose-id> <look>") << '\n'
              << Paint(STEEL, "  Mix:   CppGoose skins set <goose-id> <hat|glasses> <item|none>") << '\n'
              << Paint(STEEL, "  File:  " + GetField(fields, "skins_config")) << '\n';
}

void PrintSkinState(const std::string& response) {
    const Fields fields = ParseFields(response);
    const std::string id = GetField(fields, "goose_id");
    PrintHeader("GOOSE OUTFIT", Paint(ORANGE, "#" + id, true));
    std::cout << "  " << Paint(CYAN, "GOOSE", true) << "     " << GetField(fields, "goose_name") << '\n'
              << "  " << Paint(CYAN, "LOOK", true) << "      " << GetField(fields, "profile", "custom") << '\n'
              << "  " << Paint(CYAN, "HAT", true) << "       " << GetField(fields, "hat", "none") << '\n'
              << "  " << Paint(CYAN, "GLASSES", true) << "   " << GetField(fields, "glasses", "none") << '\n';
}

void PrintSuccess(const std::string& response) {
    const Fields fields = ParseFields(response);
    std::string summary = response;
    const size_t newline = summary.find('\n');
    if (newline != std::string::npos) summary.resize(newline);
    if (summary.rfind("ok ", 0) == 0) summary.erase(0, 3);
    if (summary == "ok") summary = "Command completed";
    PrintHeader("COMMAND", Paint(GREEN, "OK"));
    std::cout << "  " << Paint(GREEN, "[ok]", true) << " " << summary << '\n';
    for (const auto& field : fields) {
        if (field.first == "ok" || field.first.rfind("ok ", 0) == 0) continue;
        std::cout << "  " << Paint(CYAN, field.first) << "  " << field.second << '\n';
    }
}

std::string UpperCopy(std::string value) {
    for (char& c : value) c = (char)std::toupper((unsigned char)c);
    return value;
}

// Longest usage string so summaries align across every page.
size_t HelpUsageWidth() {
    size_t width = 0;
    for (const CliCommandSpec& spec : Cli_Registry()) {
        width = std::max(width, Cli_Usage(spec).size());
    }
    return width + 2;
}

void PrintHelpPageBody(const std::string& page) {
    const size_t width = HelpUsageWidth();
    for (const CliCommandSpec& spec : Cli_Registry()) {
        if (page != spec.page) continue;
        std::cout << "  " << Paint(CREAM, Pad(Cli_Usage(spec), width), true)
                  << Paint(STEEL, spec.summary) << '\n';
    }
    for (const CliCommandSpec& spec : Cli_Registry()) {
        if (page != spec.page || !spec.example) continue;
        std::cout << Paint(STEEL, "  e.g. CppGoose " + std::string(spec.example)) << '\n';
    }
}

} // namespace

void Cli_PrintHelp() {
    PrintHeader("HELP");
    for (const std::string& page : Cli_Pages()) {
        std::string commands;
        for (const CliCommandSpec& spec : Cli_Registry()) {
            if (page != spec.page) continue;
            if (!commands.empty()) commands += Paint(STEEL, " · ");
            commands += spec.sub ? spec.sub : spec.name;
        }
        std::cout << "  " << Paint(ORANGE, Pad(page, 10), true) << commands << '\n';
    }
    PrintRule();
    std::cout << "  " << Paint(CREAM, Pad("help <page|command>", 21), true)
              << Paint(STEEL, "details, e.g. CppGoose help skins") << '\n'
              << "  " << Paint(CREAM, Pad("help all", 21), true)
              << Paint(STEEL, "the full reference") << '\n';
}

bool Cli_PrintHelpTopic(const std::string& topic) {
    const std::vector<std::string> pages = Cli_Pages();
    if (std::find(pages.begin(), pages.end(), topic) != pages.end()) {
        PrintHeader("HELP " + UpperCopy(topic));
        PrintHelpPageBody(topic);
        return true;
    }
    if (Cli_IsControlCommand(topic)) {
        PrintHeader("HELP " + UpperCopy(topic));
        const size_t width = HelpUsageWidth();
        for (const CliCommandSpec& spec : Cli_Registry()) {
            if (topic != spec.name) continue;
            std::cout << "  " << Paint(CREAM, Pad(Cli_Usage(spec), width), true)
                      << Paint(STEEL, spec.summary) << '\n';
        }
        for (const CliCommandSpec& spec : Cli_Registry()) {
            if (topic != spec.name || !spec.example) continue;
            std::cout << Paint(STEEL, "  e.g. CppGoose " + std::string(spec.example)) << '\n';
        }
        return true;
    }
    return false;
}

void Cli_PrintHelpAll() {
    static const char* logo[] = {
        "       _.-.",
        " __.-' ,  \\",
        "'--'-'._   \\",
        "        '.  \\",
        "         )-- \\ __.--._",
        "        /   .'        '--.",
        "       .               _/-._",
        "       :       ____._/   _-'",
        "        '._          _.'-'",
        "           '-._    _.'",
        "               \\_|/",
        "              __|||",
        " snd          >__/"
    };
    for (size_t line = 0; line < sizeof(logo) / sizeof(logo[0]); ++line) {
        std::cout << Paint(ORANGE, Pad(logo[line], 32), true);
        if (line == 1) std::cout << Paint(CREAM, "GOOSE CONTROL", true);
        if (line == 2) std::cout << Paint(STEEL, "desktop companion");
        std::cout << '\n';
    }
    PrintRule();
    for (const std::string& page : Cli_Pages()) {
        std::cout << Paint(CYAN, UpperCopy(page), true) << '\n';
        PrintHelpPageBody(page);
        std::cout << '\n';
    }
    std::cout << Paint(STEEL, "  Compact view: CppGoose help") << '\n';
}

void Cli_PrintNotice(bool success, const std::string& message) {
    std::ostream& output = success ? std::cout : std::cerr;
    if (!IsInteractive()) {
        output << message << '\n';
        return;
    }
    output << Paint(success ? GREEN : RED, success ? "[ok] " : "[error] ", true)
           << message << '\n';
}

int Cli_PrintResponse(const std::vector<std::string>& args, const std::string& response) {
    const bool isError = response.rfind("error ", 0) == 0;
    if (!IsInteractive()) {
        (isError ? std::cerr : std::cout) << response;
        return isError ? 1 : 0;
    }

    if (isError) {
        std::string message = response.substr(6);
        if (!message.empty() && message.back() == '\n') message.pop_back();
        PrintHeader("COMMAND", Paint(RED, "ERROR"));
        std::cerr << "  " << Paint(RED, "[error]", true) << " " << message << '\n'
                  << "  " << Paint(STEEL, "Run CppGoose help for command examples.") << '\n';
        return 1;
    }

    const std::string command = args.empty() ? "" : args.front();
    if (command == "status") PrintStatus(response);
    else if (command == "ram") PrintRam(response);
    else if (command == "settings" && (args.size() == 1 || args[1] == "list")) PrintSettings(response);
    else if (command == "skins" && (args.size() == 1 || args[1] == "list")) PrintSkinCatalog(response);
    else if (command == "skins" && (args.size() > 1 && args[1] == "show")) PrintSkinState(response);
    else if (command == "skins" && response.find("goose_id=") != std::string::npos) PrintSkinState(response);
    else PrintSuccess(response);
    return 0;
}
