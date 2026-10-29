#pragma once
#include "wlroots_backend.h"

#include <string>
#include <vector>
#include <memory>

class NiriCursorTracker;

// niri (https://github.com/YaLTeR/niri) is a scrollable-tiling Wayland
// compositor built on Smithay. It is not wlroots-based, but it implements the
// wlr-virtual-pointer protocol, so cursor movement reuses the wlroots virtual
// pointer path (inherited from WlrootsBackend). Window/monitor awareness comes
// from niri's own line-delimited JSON IPC exposed at $NIRI_SOCKET.
//
// niri does not expose the pointer position over IPC. Pointer position is read
// from the ext-image-copy-capture pointer cursor session (niri_cursor.h) when
// the compositor offers it; CAP_GET_POS is advertised only while that tracker
// is active. Window snapshots use niri's screenshot-window action.
class NiriBackend : public WlrootsBackend {
public:
    NiriBackend();
    std::string Name() const override { return "niri IPC + virtual pointer"; }

    bool Init() override;
    ~NiriBackend() override;

    uint32_t Caps() const override;
    Vector2 GetCursorPos() override;

    // Neutral window-info interface (CursorBackend override), backed by niri IPC.
    bool SupportsWindowInfo() const override { return true; }
    std::vector<BackendMonitor> GetWindowMonitors() override;
    std::vector<BackendWindow> GetWindowList() override;


    // --- Window control (used by the drag/yeet behaviour) ---------------
    // IPC actions acknowledge requests; callers observe the resulting state.
    struct WindowRect {
        bool valid = false;
        int x = 0, y = 0, w = 0, h = 0; // global logical geometry
        std::string output;             // output the window is on
        int outX = 0, outY = 0;         // output logical origin
        bool floating = false;
        long workspaceId = -1;
    };
    WindowRect FindWindow(const std::string& id);
    bool SetFloating(const std::string& id, bool floating);
    bool MoveFloating(const std::string& id, int localX, int localY); // relative to the full output
    bool SetSize(const std::string& id, int wpx, int hpx);
    bool HideWindow(const std::string& id, long workspaceId);
    bool RestoreWindowWorkspace(const std::string& id, long workspaceId);
private:
    // Sends a single JSON request line and reads the single-line JSON reply.
    bool SendAction(const std::string& actionBody);
    bool Query(const std::string& request, std::string* response);
    // Creates the niri cursor tracker from IPC outputs. Failure only disables
    // GetCursorPos; the virtual pointer path stays usable.
    bool InitCursorTracking();

    std::string m_socketPath;
    std::string m_protectedWorkspaceName;
    // Cursor-position tracker (ext-image-copy-capture); null if never created.
    std::unique_ptr<NiriCursorTracker> m_cursor;
};
