#include "cursor_backend.h"
#include "hyprland.h"
#include "niri.h"
#include "x11_backend.h"
#include "wlroots_backend.h"
#include <iostream>
#include <cstdlib>


CursorBackendManager g_backendManager;

CursorBackendManager::CursorBackendManager() {
    // Null backend as default to prevent crashes
    class NullBackend : public CursorBackend {
    public:
        std::string Name() const override { return "None"; }
        uint32_t Caps() const override { return CAP_NONE; }
        bool Init() override { return true; }
    };
    static NullBackend nullBackend;
    m_activeBackend = &nullBackend;
}

void CursorBackendManager::RegisterBackend(std::unique_ptr<CursorBackend> backend) {
    m_backends.push_back(std::move(backend));
}

Vector2 CursorBackendManager::GetCursorPos(double frameTime) {
    if (frameTime != m_cursorSampleTime) {
        m_cursorSample = m_activeBackend->GetCursorPos();
        m_cursorSampleTime = frameTime;
    }
    return m_cursorSample;
}

void CursorBackendManager::Init() {
    m_cursorSampleTime = -1.0;
    // Register known backends
    RegisterBackend(std::make_unique<HyprlandBackend>());
    RegisterBackend(std::make_unique<NiriBackend>());
    RegisterBackend(std::make_unique<WlrootsBackend>());
    RegisterBackend(std::make_unique<X11Backend>());

    if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP")) {
        std::cout << "Initializing Cursor Backends (desktop: " << desktop << ")..." << std::endl;
    } else {
        std::cout << "Initializing Cursor Backends..." << std::endl;
    }

    // Detection priority, most specific first:
    //   1. Hyprland  - full IPC (cursor pos + window management)
    //   2. niri      - IPC window info + wlr-virtual-pointer cursor
    //   3. wlroots   - generic wlr-virtual-pointer (sway, river, wayfire, ...)
    //   4. X11       - XTest (also covers XWayland fallback)
    for (auto& backend : m_backends) {
        if (backend->Init()) {
            std::cout << "Selected Cursor Backend: " << backend->Name() << std::endl;
            m_activeBackend = backend.get();
            return;
        }
    }

    std::cerr << "Warning: No suitable cursor backend found!" << std::endl;
}
