#pragma once
#include "wlroots_backend.h"

#include <string>
#include <vector>

// niri (https://github.com/YaLTeR/niri) is a scrollable-tiling Wayland
// compositor built on Smithay. It is not wlroots-based, but it implements the
// wlr-virtual-pointer protocol, so cursor movement reuses the wlroots virtual
// pointer path (inherited from WlrootsBackend). Window/monitor awareness comes
// from niri's own line-delimited JSON IPC exposed at $NIRI_SOCKET.
//
// niri does not expose the pointer position over IPC and does not implement a
// screen-copy-of-a-single-toplevel protocol like Hyprland, so cursor-chase and
// the screenshot-carry window drag remain unavailable here (both are gated on
// the relevant capabilities / on the Hyprland backend specifically).
class NiriBackend : public WlrootsBackend {
public:
    std::string Name() const override { return "niri IPC + virtual pointer"; }

    bool Init() override;

    // Neutral window-info interface (CursorBackend override), backed by niri IPC.
    bool SupportsWindowInfo() const override { return true; }
    std::vector<BackendMonitor> GetWindowMonitors() override;
    std::vector<BackendWindow> GetWindowList() override;


    // --- Window control (used by the drag/yeet behaviour) ---------------
    // niri IPC is synchronous and supports absolute floating placement, so
    // these are direct one-shot actions with no polling.
    struct WindowRect {
        bool valid = false;
        int x = 0, y = 0, w = 0, h = 0; // global logical geometry
        std::string output;             // output the window is on
        int outX = 0, outY = 0;         // output logical origin
        bool floating = false;
    };
    WindowRect FindWindow(const std::string& id);
    bool SetFloating(const std::string& id, bool floating);
    bool MoveFloating(const std::string& id, int localX, int localY); // relative to output working area
    bool SetSize(const std::string& id, int wpx, int hpx);
private:
    // Sends a single JSON request line and reads the single-line JSON reply.
    bool SendAction(const std::string& actionBody);
    bool Query(const std::string& request, std::string* response);

    std::string m_socketPath;
};
