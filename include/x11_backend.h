#pragma once
#include "cursor_backend.h"
#include <cstdint>

class X11Backend : public CursorBackend {
public:
    X11Backend();
    ~X11Backend() override;

    std::string Name() const override { return "X11 (XTest)"; }
    uint32_t Caps() const override { return CAP_GET_POS | CAP_MOVE_ABS | CAP_CLICK; }
    
    bool Init() override;
    
    // Core operations
    Vector2 GetCursorPos() override;
    void MoveCursorAbs(int x, int y) override;
    void MoveCursorRel(int dx, int dy) override;
    
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
