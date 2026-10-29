#pragma once
#include "goose_math.h"

#include <memory>
#include <string>
#include <vector>

// One niri output the cursor tracker should follow. Coordinates are niri IPC
// logical geometry (global logical space, same space as Hyprland cursorpos).
struct NiriCursorOutput {
    std::string name;
    double x = 0, y = 0, w = 0, h = 0, scale = 1;
};

// Tracks the global pointer position using the ext-image-copy-capture
// pointer cursor session, one session per niri output. Creates no surfaces
// and takes no input grab, so pointer focus is untouched. Event dispatch is
// nonblocking and driven by Pump().
class NiriCursorTracker {
public:
    // Implementation detail shared with the protocol callbacks in niri_cursor.cpp.
    struct Impl;

    NiriCursorTracker();
    ~NiriCursorTracker();
    NiriCursorTracker(const NiriCursorTracker&) = delete;
    NiriCursorTracker& operator=(const NiriCursorTracker&) = delete;

    // Connects and binds the protocol. Returns true when at least one output
    // delivered cursor constraints. On failure Reason() explains why.
    bool Init(const std::vector<NiriCursorOutput>& outputs);

    // True while the connection is alive and at least one output session is
    // usable.
    bool Active() const;
    bool NeedsRefresh() const;
    const std::string& Reason() const { return m_reason; }

    // Nonblocking dispatch of any pending compositor events.
    void Pump();

    // Global logical cursor position, or {-1,-1} when the cursor is not on a
    // tracked output (or is unknown).
    Vector2 Position() const;

private:
    std::unique_ptr<Impl> m_impl;
    std::string m_reason;
};
