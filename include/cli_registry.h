#pragma once

#include <string>
#include <vector>

// What kind of value an argument slot takes; drives shell tab-completion.
enum class CliComplete {
    None,
    OnOffToggle, // on | off | toggle
    SettingKey,  // keys from g_configRegistry
    GooseId,     // goose ids from `status`
    Look,        // preset/profile ids from `skins list`
    Slot,        // hat | glasses
    Item,        // item ids from `skins list` (+ none)
    RuleAction,     // wander | meme | note | chase | text
    ForceBehavior,  // wander | chase | drag | yeet | meme | note | fortune
    ForceSource,    // file for memes; file | text for notes
    RuleId,         // rule ids from `rules list`
};

// One row per invocable command form. Adding a command = adding a row here
// plus a handler in AppActions_HandleCommand; help, validation, and shell
// completion all derive from this table.
struct CliCommandSpec {
    const char* name;    // top-level command, e.g. "skins"
    const char* sub;     // subcommand, or nullptr for a bare command
    const char* page;    // help page this belongs to
    const char* args;    // argument template, e.g. "<id> <look>"
    const char* summary; // one-line description
    const char* example; // full example invocation, or nullptr
    bool offline;        // usable without a running daemon
    bool local;          // handled entirely by the CLI process (never sent)
    CliComplete argComplete[4]; // completion kind per argument slot
};

const std::vector<CliCommandSpec>& Cli_Registry();

// Ordered, de-duplicated page names.
std::vector<std::string> Cli_Pages();

// True when `name` is a known top-level command.
bool Cli_IsControlCommand(const std::string& name);

// Spec for `name` (+ optional subcommand). A bare invocation of a command
// whose first spec is a "list" sub resolves to that spec. nullptr if unknown.
const CliCommandSpec* Cli_FindSpec(const std::string& name, const std::string& sub);

// Whether this exact invocation can run without the daemon.
bool Cli_WorksOffline(const std::vector<std::string>& args);

// Subcommand names registered under `name`.
std::vector<std::string> Cli_Subcommands(const std::string& name);

// Closest known command for a typo, or "" when nothing is close.
std::string Cli_Suggest(const std::string& name);

// "name sub <args>" display string for one spec.
std::string Cli_Usage(const CliCommandSpec& spec);
