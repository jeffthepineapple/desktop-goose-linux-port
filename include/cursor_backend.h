#pragma once
#include "goose_math.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

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
};

// Manager to handle selection and global access
class CursorBackendManager {
public:
    CursorBackendManager();
    ~CursorBackendManager() = default;

    void Init();
    CursorBackend* GetActiveBackend() { return m_activeBackend; }

private:
    void RegisterBackend(std::shared_ptr<CursorBackend> backend);
    
    std::vector<std::shared_ptr<CursorBackend>> m_backends;
    CursorBackend* m_activeBackend = nullptr;
};

extern CursorBackendManager g_backendManager;
