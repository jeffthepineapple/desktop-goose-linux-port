#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cstdint>

#include <string>

// Captures the actual Hyprland toplevel surface. Unlike screencopy, this does
// not include the desktop behind transparent parts of the window.
GdkPixbuf* CaptureHyprlandToplevel(uint32_t handle,
                                    std::string* errorOut = nullptr);

// Captures one whole niri window (its own surface, not the screen rectangle
// behind it) by running `niri msg action screenshot-window --id`. niri applies
// the action asynchronously, so this waits for the screenshot file to finish
// writing before decoding it. Temporary files are removed before returning.
GdkPixbuf* CaptureNiriWindow(uint64_t windowId, std::string* errorOut = nullptr);
