#pragma once

#include <string>
#include <map>
#include <mutex>

struct SpeechIntent {
    std::string intent;            // canonical intent name, e.g. "open_calculator"
    double confidence = 1.0;       // 0.0..1.0
    std::string transcript;        // original raw transcript
    std::string normalized;        // normalized/cleaned text
    std::map<std::string,std::string> entities; // named entities
};

struct ToolStatus {
    bool isCalculatorRunning = false;
    bool isBrowserOpen = false;
    std::string lastAssignmentSummary;
    std::string lastError;
};

class ToolManager {
public:
    static ToolManager& Instance();
    bool Init();
    bool HandleIntent(const SpeechIntent& intent);
    ToolStatus GetStatus();

    // UI callbacks
    void OnOpenCalculatorButton();
    void OnShowAssignmentsButton();
    void OnSearchWeb(const std::string& query);

private:
    ToolManager();
    ToolManager(const ToolManager&) = delete;
    ToolManager& operator=(const ToolManager&) = delete;

    std::mutex m_mutex;
    ToolStatus m_status;
};
