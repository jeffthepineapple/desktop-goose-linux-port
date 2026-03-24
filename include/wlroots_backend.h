#pragma once
#include "cursor_backend.h"
#include <cstdint>

struct WlrootsImpl;

class WlrootsBackend : public CursorBackend {
public:
    WlrootsBackend();
    ~WlrootsBackend() override;

    std::string Name() const override { return "Wayland (wlroots)"; }
    uint32_t Caps() const override { return CAP_MOVE_REL | CAP_MOVE_ABS; }
    
    bool Init() override;
    
    // Core operations
    void MoveCursorAbs(int x, int y) override;
    void MoveCursorRel(int dx, int dy) override;
    
private:
    std::unique_ptr<WlrootsImpl> m_impl;
};
