#pragma once
#include "goose_math.h" // Vector2

#include "cursor_backend.h"

class HyprlandBackend : public CursorBackend {
public:
    std::string Name() const override { return "Hyprland IPC"; }
    uint32_t Caps() const override { return CAP_GET_POS | CAP_MOVE_ABS; }

    bool Init() override;
    Vector2 GetCursorPos() override;
    void MoveCursorAbs(int x, int y) override;

    struct Monitor {
        int id;
        std::string name;
        int x, y, width, height;
        double scale;
        int reserved[4] = {0, 0, 0, 0}; // [top, bottom, left, right] from Hyprland IPC
    };

    struct Window {
        std::string address;
        int x, y, width, height;
        int monitorId;
        std::string title;
        std::string cls;
    };

    // New methods for getting Hyprland info
    std::vector<Monitor> GetMonitors();
    std::vector<Window> GetWindows();
    bool SetWindowBorderColor(const std::string& windowAddress, const std::string& color);
    bool ResetWindowBorder(const std::string& windowAddress);


private:
    bool SendCommand(const std::string& command, std::string* response);
    std::string m_socketPath;
};
