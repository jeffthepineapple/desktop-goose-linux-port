#pragma once
#include "goose_math.h" // Vector2

#include "cursor_backend.h"

class HyprlandBackend : public CursorBackend {
public:
    std::string Name() const override { return "Hyprland IPC"; }
    uint32_t Caps() const override { return CAP_GET_POS | CAP_MOVE_ABS; }

    bool Init() override;
    Vector2 GetCursorPos() override;
    void MoveCursorAbs(int x, int y) override;
    
    // Internal helper remains available if needed, but usually hidden
    static bool isAvailable(); 
};
