#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <string>

struct WindowCaptureRegion {
    std::string outputName;
    int outputX, outputY;
    int x, y, width, height;  // Output-local logical coordinates.
};

// Returns a newly referenced pixbuf, owned by the caller.
GdkPixbuf* CaptureWaylandRegion(const WindowCaptureRegion& region,
                                std::string* errorOut = nullptr);
