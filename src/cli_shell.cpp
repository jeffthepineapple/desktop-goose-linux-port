#include "cli_shell.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

#include "cli_registry.h"
#include "cli_visuals.h"
#include "command_socket.h"
#include "linenoise.h"

namespace {

// Extra commands that only exist inside the shell.
constexpr const char* kShellBuiltins[] = {"exit", "cls"};

// --- Tokenizing ---------------------------------------------------------------

std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;
    for (char c : line) {
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (!inQuotes && std::isspace((unsigned char)c)) {
            if (!current.empty()) tokens.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

// --- Dynamic vocabulary (completion sources) -----------------------------------

// Cached daemon responses; refetched after a short TTL or any mutating command.
struct VocabCache {
    std::string status;    // goose ids + setting keys
    std::string skinList;  // looks + items
    std::string ruleList;  // rule ids
    double fetchedAt = -1.0;
};

VocabCache g_vocab;

double MonotonicSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void InvalidateVocab() { g_vocab.fetchedAt = -1.0; }

const VocabCache& Vocab() {
    const double now = MonotonicSeconds();
    if (g_vocab.fetchedAt < 0.0 || now - g_vocab.fetchedAt > 3.0) {
        g_vocab = VocabCache{};
        CommandSocket_Send({"status"}, &g_vocab.status, nullptr);
        CommandSocket_Send({"skins", "list"}, &g_vocab.skinList, nullptr);
        CommandSocket_Send({"rules", "list"}, &g_vocab.ruleList, nullptr);
        g_vocab.fetchedAt = now;
    }
    return g_vocab;
}

// Collect the text between `prefix` and the next `stop` char on every matching
// line of a key=value response.
std::vector<std::string> LinesBetween(const std::string& text,
                                      const std::string& prefix, char stop) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.rfind(prefix, 0) != 0) continue;
        const size_t stopAt = line.find(stop, prefix.size());
        if (stopAt == std::string::npos) continue;
        out.push_back(line.substr(prefix.size(), stopAt - prefix.size()));
    }
    return out;
}

std::vector<std::string> GooseIds() {
    // status lines: goose.<id>.name=...
    std::vector<std::string> ids;
    for (const std::string& tail : LinesBetween(Vocab().status, "goose.", '.')) {
        if (std::find(ids.begin(), ids.end(), tail) == ids.end()) ids.push_back(tail);
    }
    return ids;
}

std::vector<std::string> SettingKeys() {
    // status lines: <key>=<value>, excluding structured prefixes.
    std::vector<std::string> keys;
    size_t pos = 0;
    const std::string& text = Vocab().status;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        static const char* kNonSettings[] = {
            "running", "goose_count", "config_path", "asset_root", "meme_count",
            "note_count", "frozen", "rule_count",
        };
        bool skip = key.rfind("goose.", 0) == 0 || key.rfind("ram_", 0) == 0;
        for (const char* blocked : kNonSettings) skip = skip || key == blocked;
        if (!skip) keys.push_back(key);
    }
    return keys;
}

std::vector<std::string> Looks() {
    std::vector<std::string> looks = LinesBetween(Vocab().skinList, "preset.", '=');
    for (std::string& id : LinesBetween(Vocab().skinList, "profile.", '=')) {
        looks.push_back(std::move(id));
    }
    return looks;
}

std::vector<std::string> Items(const std::string& slot) {
    std::vector<std::string> items;
    if (slot.empty() || slot == "hat") {
        items = LinesBetween(Vocab().skinList, "hat.", '=');
    }
    if (slot.empty() || slot == "glasses") {
        for (std::string& id : LinesBetween(Vocab().skinList, "glasses.", '=')) {
            items.push_back(std::move(id));
        }
    }
    items.push_back("none");
    return items;
}

std::vector<std::string> RuleIds() {
    // rules list lines: rule.<id> target=...
    return LinesBetween(Vocab().ruleList, "rule.", ' ');
}

std::vector<std::string> KindCandidates(CliComplete kind,
                                        const std::vector<std::string>& tokens,
                                        size_t tokenIndex) {
    switch (kind) {
        case CliComplete::OnOffToggle: return {"on", "off", "toggle"};
        case CliComplete::Slot:        return {"hat", "glasses"};
        case CliComplete::RuleAction:  return {"wander", "meme", "note", "chase", "text"};
        case CliComplete::ForceBehavior: return {"wander", "meme", "note", "chase"};
        case CliComplete::ForceSource: {
            if (tokens.size() > 3 && tokens[3] == "note") return {"file", "text"};
            if (tokens.size() > 3 && tokens[3] == "meme") return {"file"};
            return {};
        }
        case CliComplete::SettingKey:  return SettingKeys();
        case CliComplete::Look:        return Looks();
        case CliComplete::RuleId:      return RuleIds();
        case CliComplete::GooseId: {
            std::vector<std::string> ids = GooseIds();
            if (tokens[0] == "rules") ids.push_back("all"); // rules add <id|all>
            return ids;
        }
        case CliComplete::Item:
            return Items(tokenIndex > 0 ? tokens[tokenIndex - 1] : "");
        case CliComplete::None: return {};
    }
    return {};
}

// Candidates for the token at `index`, given the tokens typed so far.
std::vector<std::string> CandidatesFor(const std::vector<std::string>& tokens,
                                       size_t index) {
    if (index == 0) {
        std::vector<std::string> names;
        for (const CliCommandSpec& spec : Cli_Registry()) {
            if (spec.name == std::string("shell") || spec.name == std::string("start")) {
                continue; // meaningless inside the shell
            }
            if (names.empty() || names.back() != spec.name) names.push_back(spec.name);
        }
        for (const char* builtin : kShellBuiltins) names.push_back(builtin);
        return names;
    }

    const std::string& name = tokens[0];
    if (name == "help" && index == 1) {
        std::vector<std::string> topics = Cli_Pages();
        for (const CliCommandSpec& spec : Cli_Registry()) {
            if (std::find(topics.begin(), topics.end(), spec.name) == topics.end()) {
                topics.push_back(spec.name);
            }
        }
        topics.push_back("all");
        return topics;
    }

    const std::vector<std::string> subs = Cli_Subcommands(name);
    if (!subs.empty() && index == 1) return subs;

    const CliCommandSpec* spec =
        Cli_FindSpec(name, subs.empty() ? "" : (tokens.size() > 1 ? tokens[1] : ""));
    if (!spec) return {};
    const size_t argIndex = index - (spec->sub ? 2 : 1);
    if (argIndex >= 4) return {};
    return KindCandidates(spec->argComplete[argIndex], tokens, index);
}

void CompletionCallback(const char* buf, linenoiseCompletions* lc) {
    const std::string line(buf);
    const std::vector<std::string> tokens = Tokenize(line);
    const bool newToken =
        line.empty() || std::isspace((unsigned char)line.back());
    const size_t index = newToken ? tokens.size() : tokens.size() - 1;
    const std::string current = newToken ? "" : tokens.back();
    const std::string prefix = line.substr(0, line.size() - current.size());

    for (const std::string& candidate : CandidatesFor(tokens, index)) {
        if (candidate.rfind(current, 0) != 0) continue;
        linenoiseAddCompletion(lc, (prefix + candidate).c_str());
    }
}

char* HintsCallback(const char* buf, int* color, int* bold) {
    static std::string hint;
    const std::string line(buf);
    if (line.empty() || !std::isspace((unsigned char)line.back())) return nullptr;

    const std::vector<std::string> tokens = Tokenize(line);
    if (tokens.empty() || !Cli_IsControlCommand(tokens[0])) return nullptr;

    const std::vector<std::string> subs = Cli_Subcommands(tokens[0]);
    hint.clear();
    if (tokens.size() == 1 && !subs.empty()) {
        for (const std::string& sub : subs) {
            if (!hint.empty()) hint += "|";
            hint += sub;
        }
    } else if (tokens.size() <= 2) {
        const CliCommandSpec* spec =
            Cli_FindSpec(tokens[0], tokens.size() > 1 ? tokens[1] : "");
        if (spec && (tokens.size() == 1 || spec->sub)) hint = spec->args;
    }
    if (hint.empty()) return nullptr;
    *color = 90; // dim gray
    *bold = 0;
    return hint.data();
}

// --- History -------------------------------------------------------------------

std::string HistoryPath() {
    std::string dir;
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) {
        dir = state;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        dir = std::string(home) + "/.local/state";
    } else {
        return "";
    }
    dir += "/cppgoose";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return "";
    return dir + "/history";
}

// --- Command execution -----------------------------------------------------------

void HandleHelp(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        Cli_PrintHelp();
    } else if (args[1] == "all") {
        Cli_PrintHelpAll();
    } else if (!Cli_PrintHelpTopic(args[1])) {
        Cli_PrintNotice(false, "No help for '" + args[1] + "'. Try 'help'.");
    }
}

bool ConfirmQuit() {
    char* raw = linenoise("Stop the daemon and remove every goose? [y/N] ");
    if (!raw) return false;
    const bool yes = raw[0] == 'y' || raw[0] == 'Y';
    linenoiseFree(raw);
    return yes;
}

// Returns false when the shell should exit.
bool ExecuteLine(const std::string& line) {
    const std::vector<std::string> args = Tokenize(line);
    if (args.empty()) return true;
    const std::string& command = args.front();

    if (command == "exit") return false;
    if (command == "cls") {
        linenoiseClearScreen();
        return true;
    }
    if (command == "help") {
        HandleHelp(args);
        return true;
    }
    if (command == "shell") {
        Cli_PrintNotice(true, "Already in the goose shell.");
        return true;
    }
    if (command == "start") {
        Cli_PrintNotice(true, "Desktop Goose is already running. Use spawn to add a goose.");
        return true;
    }
    if (!Cli_IsControlCommand(command)) {
        const std::string suggestion = Cli_Suggest(command);
        Cli_PrintNotice(false, "Unknown command '" + command + "'." +
                        (suggestion.empty() ? " Try 'help'."
                                            : " Did you mean '" + suggestion + "'?"));
        return true;
    }
    if (command == "quit" && !ConfirmQuit()) return true;

    std::string response;
    std::string error;
    if (!CommandSocket_Send(args, &response, &error)) {
        Cli_PrintNotice(false, error);
        Cli_PrintNotice(false, "The daemon may have stopped. Leave with 'exit', restart with 'CppGoose start'.");
        return true;
    }
    Cli_PrintResponse(args, response);
    InvalidateVocab();
    return command != "quit";
}

} // namespace

int Cli_RunShell() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        Cli_PrintNotice(false, "The goose shell needs an interactive terminal.");
        return 1;
    }
    {
        std::string response;
        if (!CommandSocket_Send({"status"}, &response, nullptr)) {
            Cli_PrintNotice(false, "Desktop Goose is not running. Start it with 'CppGoose start'.");
            return 1;
        }
    }

    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(CompletionCallback);
    linenoiseSetHintsCallback(HintsCallback);
    linenoiseHistorySetMaxLen(500);
    const std::string historyPath = HistoryPath();
    if (!historyPath.empty()) linenoiseHistoryLoad(historyPath.c_str());

    std::cout << "Goose shell. TAB completes, 'help' shows pages, 'exit' leaves (goose keeps running).\n";

    bool keepGoing = true;
    while (keepGoing) {
        char* raw = linenoise("goose> ");
        if (!raw) break; // Ctrl-D / Ctrl-C
        const std::string line(raw);
        linenoiseFree(raw);
        if (Tokenize(line).empty()) continue;
        linenoiseHistoryAdd(line.c_str());
        if (!historyPath.empty()) linenoiseHistorySave(historyPath.c_str());
        keepGoing = ExecuteLine(line);
    }
    return 0;
}
