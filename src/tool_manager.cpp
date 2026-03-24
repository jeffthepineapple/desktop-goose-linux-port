#include "tool_manager.h"
#include "config.h"
#include <iostream>
#include <glib.h>

ToolManager& ToolManager::Instance() {
    static ToolManager inst;
    return inst;
}

ToolManager::ToolManager() {}

bool ToolManager::Init() {
    // Nothing heavy to init for now
    return true;
}

ToolStatus ToolManager::GetStatus() {
    std::lock_guard<std::mutex> l(m_mutex);
    return m_status;
}

static void spawn_command(const std::string& cmd, std::string& outErr) {
    GError* err = nullptr;
    gboolean ok = g_spawn_command_line_async(cmd.c_str(), &err);
    if (!ok) {
        if (err) {
            outErr = err->message ? err->message : "unknown";
            g_error_free(err);
        } else outErr = "failed to spawn";
    }
}

bool ToolManager::HandleIntent(const SpeechIntent& intent) {
    std::string lower = intent.intent;
    if (lower == "open_calculator") {
        std::string cmd = g_config.geminiApiKey.size() ? g_config.geminiApiKey : ""; // placeholder to use config if repurposed
        // prefer configured calculator
        std::string calc = g_config.geminiApiKey.empty() ? std::string() : std::string();
        if (!g_config.geminiApiKey.empty()) {
            // nothing to do - geminiApiKey is not a calculator; ignore
        }

        std::string err;
        // try preferred calculator from config if present
        if (!g_config.preferredCalculator.empty()) {
            spawn_command(g_config.preferredCalculator, err);
        } else {
            spawn_command("gnome-calculator", err);
            if (!err.empty()) spawn_command("galculator", err);
            if (!err.empty()) spawn_command("xcalc", err);
        }

        std::lock_guard<std::mutex> l(m_mutex);
        if (!err.empty()) {
            m_status.lastError = err;
            return false;
        }
        m_status.isCalculatorRunning = true;
        return true;
    } else if (lower == "open_browser" || lower == "search_web") {
        std::string target = "https://www.google.com";
        if (lower == "search_web") {
            auto it = intent.entities.find("query");
            if (it != intent.entities.end()) {
                std::string q = it->second;
                // naive url-encode spaces
                for (auto& c : q) if (c == ' ') c = '+';
                target = "https://www.google.com/search?q=" + q;
            }
        }

        std::string cmd = "xdg-open '" + target + "'";
        std::string err;
        spawn_command(cmd, err);
        std::lock_guard<std::mutex> l(m_mutex);
        if (!err.empty()) {
            m_status.lastError = err;
            return false;
        }
        m_status.isBrowserOpen = true;
        return true;
    } else if (lower == "status") {
        // no-op; UI can call GetStatus to show state
        return true;
    }

    return false; // not handled
}

void ToolManager::OnOpenCalculatorButton() {
    SpeechIntent si;
    si.intent = "open_calculator";
    si.transcript = "(button) open calculator";
    HandleIntent(si);
}

void ToolManager::OnShowAssignmentsButton() {
    std::lock_guard<std::mutex> l(m_mutex);
    // For now, populate lastAssignmentSummary from config lastAssignmentSummary
    m_status.lastAssignmentSummary = g_config.lastAssignmentSummary;
}

void ToolManager::OnSearchWeb(const std::string& query) {
    SpeechIntent si;
    si.intent = "search_web";
    si.entities["query"] = query;
    HandleIntent(si);
}
