#include "cursor_backend.h"
#include "hyprland.h"
#include "x11_backend.h"
#include "wlroots_backend.h"
#include <iostream>


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
    RegisterBackend(std::make_unique<WlrootsBackend>());
    RegisterBackend(std::make_unique<X11Backend>());

    std::cout << "Initializing Cursor Backends..." << std::endl;

    // Detection priority: 
    // 1. Check environment hints
    // 2. Try Init() on backends in order

    // Simple priority for now: Hyprland first (since it's specific), then X11 (general)
    for (auto& backend : m_backends) {
        if (backend->Init()) {
            std::cout << "Selected Cursor Backend: " << backend->Name() << std::endl;
            m_activeBackend = backend.get();
            return;
        }
    }

    std::cerr << "Warning: No suitable cursor backend found!" << std::endl;
}
