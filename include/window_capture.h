#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cstdint>

#include <string>

struct WindowCaptureRegion {
    std::string outputName;
    int outputX, outputY;
    int x, y, width, height;  // Output-local logical coordinates.
};

// Returns a newly referenced pixbuf, owned by the caller.
GdkPixbuf* CaptureWaylandRegion(const WindowCaptureRegion& region,
                                std::string* errorOut = nullptr);

// Captures the actual Hyprland toplevel surface. Unlike screencopy, this does
// not include the desktop behind transparent parts of the window.
GdkPixbuf* CaptureHyprlandToplevel(uint32_t handle,
                                    std::string* errorOut = nullptr);
