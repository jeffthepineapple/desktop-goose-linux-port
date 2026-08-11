#pragma once
#include "goose_math.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// Neutral, backend-agnostic window/monitor descriptions used by consumers
// (e.g. the edge detector) so window awareness is not tied to any single
// compositor. Backends that can enumerate windows fill these in.
struct BackendMonitor {
    int id = -1;
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    double scale = 1.0;
    int reserved[4] = {0, 0, 0, 0}; // [top, bottom, left, right]
    int activeWorkspaceId = -1;
};

struct BackendWindow {
    std::string id; // Opaque handle: Hyprland address, niri numeric id, etc.
    int x = 0, y = 0, width = 0, height = 0;
    int monitorId = -1;
    int workspaceId = -1;
    std::string title;
    std::string cls;
    bool floating = false;
};

// Capabilities bitflags
enum CursorCaps {
    CAP_NONE        = 0,
    CAP_GET_POS     = 1 << 0, // Can read global cursor position
    CAP_MOVE_ABS    = 1 << 1, // Can move cursor to absolute position
    CAP_MOVE_REL    = 1 << 2, // Can move cursor relatively
    CAP_CLICK       = 1 << 3  // Can emit click events (future)
};

// Abstract base class for all backends
class CursorBackend {
public:
    virtual ~CursorBackend() = default;

    virtual std::string Name() const = 0;
    virtual uint32_t Caps() const = 0;
    virtual bool Init() = 0; // Return true if successfully initialized/detected

    // Core operations (no-op if cap missing)
    virtual Vector2 GetCursorPos() { return {-1.0f, -1.0f}; }
    virtual void MoveCursorAbs(int x, int y) {}
    virtual void MoveCursorRel(int dx, int dy) {}

    // Optional window awareness. Backends able to enumerate the compositor's
    // windows/monitors (via IPC or a protocol) override these. Consumers must
    // check SupportsWindowInfo() before relying on the data.
    virtual bool SupportsWindowInfo() const { return false; }
    virtual std::vector<BackendMonitor> GetWindowMonitors() { return {}; }
    virtual std::vector<BackendWindow> GetWindowList() { return {}; }
};

// Manager to handle selection and global access
class CursorBackendManager {
public:
    CursorBackendManager();
    ~CursorBackendManager() = default;

    void Init();
    CursorBackend* GetActiveBackend() { return m_activeBackend; }
    Vector2 GetCursorPos(double frameTime);

private:
    void RegisterBackend(std::unique_ptr<CursorBackend> backend);

    std::vector<std::unique_ptr<CursorBackend>> m_backends;
    CursorBackend* m_activeBackend = nullptr;
    double m_cursorSampleTime = -1.0;
    Vector2 m_cursorSample{-1.0f, -1.0f};
};

extern CursorBackendManager g_backendManager;
