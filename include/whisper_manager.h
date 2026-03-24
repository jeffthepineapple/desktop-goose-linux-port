#pragma once
#include <array>
#include <string>

struct WhisperVisualizerState {
    bool running = false;
    float peakLevel = 0.0f;
    std::array<float, 32> bars{{}};
    std::string transcript;
    std::string status;
    double lastUpdateTime = 0.0;
};

void WhisperManager_RequestToggle(bool enable);
WhisperVisualizerState WhisperManager_Snapshot();
bool WhisperManager_IsCapturing();
