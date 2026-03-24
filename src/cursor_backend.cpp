#include "cursor_backend.h"
#include "hyprland.h"
#include "x11_backend.h"
#include "wlroots_backend.h"
#include <algorithm>
#include <iostream>

// Defined here for now, eventually will include specific backend headers
// We will register them in Init()

CursorBackendManager g_backendManager;

CursorBackendManager::CursorBackendManager() {
    // Null backend as default to prevent crashes
    class NullBackend : public CursorBackend {
    public:
        std::string Name() const override { return "None"; }
        uint32_t Caps() const override { return CAP_NONE; }
        bool Init() override { return true; }
    };
    // Don't register null backend in the list, just set as fallback active
    static auto nullBackend = std::make_shared<NullBackend>();
    m_activeBackend = nullBackend.get();
}

void CursorBackendManager::RegisterBackend(std::shared_ptr<CursorBackend> backend) {
    m_backends.push_back(backend);
}

void CursorBackendManager::Init() {
    // Register known backends
    RegisterBackend(std::make_shared<HyprlandBackend>());
    RegisterBackend(std::make_shared<WlrootsBackend>());
    RegisterBackend(std::make_shared<X11Backend>());

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
