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
        int activeWorkspaceId = -1;
    };

    struct Window {
        std::string address;
        int x, y, width, height;
        int monitorId;
        int workspaceId = -1;
        std::string title;
        std::string cls;
        bool floating = false;
    };

    struct Workspace {
        int id;
        std::string name;
        int monitorID;
        bool special = false;
    };

    // New methods for getting Hyprland info
    std::vector<Monitor> GetMonitors();
    Monitor GetMonitorById(int id);
    std::vector<Window> GetWindows();
    Window GetWindowByAddress(const std::string& address);
    std::string GetActiveWindowAddress();
    bool SetWindowBorderColor(const std::string& windowAddress, const std::string& color);
    bool ResetWindowBorder(const std::string& windowAddress);

    // Neutral window-info interface (CursorBackend override).
    bool SupportsWindowInfo() const override { return true; }
    std::vector<BackendMonitor> GetWindowMonitors() override;
    std::vector<BackendWindow> GetWindowList() override;

    // Workspace queries
    Workspace GetActiveWorkspace();
    std::vector<Workspace> GetWorkspaces();

    // Window manipulation (Hyprland-only)
    bool MoveWindowTo(const std::string& address, int x, int y);
    bool MoveWindowToExact(const std::string& address, int x, int y);
    bool ResizeWindowToExact(const std::string& address, int w, int h);
    bool ToggleFloating(const std::string& address);
    bool MoveWindowToWorkspace(const std::string& address, const std::string& workspaceName);
    bool FocusWindow(const std::string& address);
    bool SendDispatch(const std::string& dispatcher, const std::string& args);


private:
    bool SendCommand(const std::string& command, std::string* response);
    std::string m_socketPath;
};
