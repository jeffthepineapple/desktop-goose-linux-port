#include "x11_backend.h"
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <iostream>
#include <cstdlib>

struct X11Backend::Impl {
    Display* display = nullptr;
    Window root = 0;
};

X11Backend::X11Backend() = default;
X11Backend::~X11Backend() {
    if (m_impl && m_impl->display) {
        XCloseDisplay(m_impl->display);
    }
}

bool X11Backend::Init() {
    // Only attempt if explicitly X11 or autodetection allows
    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if (sessionType && std::string(sessionType) == "wayland") {
        // Technically XWayland exists, but we usually want native Wayland backends if possible.
        // However, if we don't have a Wayland backend yet, X11 backend might work via XWayland 
        // (though moving cursor globally on XWayland is often restricted or doesn't affect real compositor cursor).
        // For now, let's allow it to try if DISPLAY is set, but it might fail or be limited.
    }

    Display* d = XOpenDisplay(nullptr);
    if (!d) return false;

    // Check for XTest extension
    int event_base, error_base, major_version, minor_version;
    if (!XTestQueryExtension(d, &event_base, &error_base, &major_version, &minor_version)) {
        std::cerr << "X11Backend: XTest extension not available." << std::endl;
        XCloseDisplay(d);
        return false;
    }

    m_impl = std::make_unique<Impl>();
    m_impl->display = d;
    m_impl->root = DefaultRootWindow(d);
    
    return true;
}

Vector2 X11Backend::GetCursorPos() {
    if (!m_impl || !m_impl->display) return {-1.0f, -1.0f};

    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;

    if (XQueryPointer(m_impl->display, m_impl->root, &root_ret, &child_ret, 
                      &root_x, &root_y, &win_x, &win_y, &mask)) {
        return {(float)root_x, (float)root_y};
    }
    return {-1.0f, -1.0f};
}

void X11Backend::MoveCursorAbs(int x, int y) {
    if (!m_impl || !m_impl->display) return;

    // XTestFakeMotionEvent is generally more reliable for "input simulation" than XWarpPointer,
    // though XWarpPointer is simpler. XTest is preferred for automation.
    XTestFakeMotionEvent(m_impl->display, -1, x, y, CurrentTime);
    XFlush(m_impl->display);
}

void X11Backend::MoveCursorRel(int dx, int dy) {
    if (!m_impl || !m_impl->display) return;
    
    XTestFakeRelativeMotionEvent(m_impl->display, dx, dy, CurrentTime);
    XFlush(m_impl->display);
}
