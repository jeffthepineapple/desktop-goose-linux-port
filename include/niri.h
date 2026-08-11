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

private:
    // Sends a single JSON request line and reads the single-line JSON reply.
    bool Query(const std::string& request, std::string* response);

    std::string m_socketPath;
};
