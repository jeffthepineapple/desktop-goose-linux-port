// ===========================
// goose.h
// ===========================
#ifndef GOOSE_H
#define GOOSE_H

#include <cstdint>
#include <string>
#include <gtk/gtk.h>
#include "goose_math.h"
#include "assets.h"
#include "cosmetics.h"

enum GooseState { WANDER, FETCHING, RETURNING, CHASE_CURSOR, SNATCH_CURSOR, DRAG_WINDOW, HELD };

enum DragPhase {
    DRAG_APPROACH,
    DRAG_CAPTURE_WAIT,
    DRAG_PREPARE_WINDOW,
    DRAG_HIDE_WINDOW,
    DRAG_CARRY,
    DRAG_YEET_WINDUP,
    DRAG_YEET_FLIGHT,
    DRAG_RESTORE_WORKSPACE,
    DRAG_RESTORE_GEOMETRY,
    DRAG_RETILE
};

struct FootState {
    Vector2 currentPos{};
    Vector2 moveOrigin{};
    Vector2 moveDir{};
    double moveStartTime = -1.0;
    float moveDuration = 0.2f;
};

struct Rig {
    Vector2 underbody, body, neckBase, neckHead, head1, head2;
    float neckLerp = 0;
    FootState lFoot, rFoot;
};

// Ragdoll: the goose hangs from the cursor and swings as a chain of damped
// pendulums (body, neck, each leg). In the accelerating frame of the grab
// point every limb feels an effective gravity of `gravity - cursorAccel`, so
// shaking the cursor injects real torque instead of a canned wiggle anim.
struct RagdollState {
    Vector2 pivot{};      // device-space point the cursor holds
    Vector2 lastPivot{};
    Vector2 grabOffset{}; // pos - pivot captured at grab time
    Vector2 pivotVel{};
    Vector2 accel{};      // smoothed pivot acceleration
    bool primed = false;  // pivot history is valid

    float bodyAng = 0.0f, bodyVel = 0.0f; // radians, 0 = hanging straight down
    float neckAng = 0.0f, neckVel = 0.0f; // relative to body
    float legLAng = 0.0f, legLVel = 0.0f;
    float legRAng = 0.0f, legRVel = 0.0f;

    double nextSquirm = 0.0;
    float squirmDrive = 0.0f; // transient neck extension while struggling
    float headRot = 0.0f;     // radians actually applied to the head chain
    float release = 0.0f;     // 1 -> 0 settle blend after being dropped
};

class Goose {
public:
    int id;
    std::string name;
    GooseSkin skin;
    Vector2 pos{200, 200};
    Vector2 target{500, 500};
    Vector2 vel{};
    Vector2 acceleration{};
    float dir = 90.0f;
    float maxForce = 350.0f;
    float parabolicCurvature = 0.0f; // Multiplier for tangential curve force

    // State
    GooseState state = WANDER;
    ItemData* heldItem = nullptr;
    int forceItemFetch = -1; // -1: Random, 0: Meme, 1: Text
    std::string forcedText;
    std::string forcedMemePath;

    float currentSpeed = 0;
    float stepTime = 0.2f;
    Rig rig;

    const Vector2 ISO_SCALE { 1.3f, 0.4f };

    Vector2 dragPos{};
    Vector2 dragVel{};
    float dragRot = 0.0f; // radians
    float dragRotVel = 0.0f;
    bool dragInit = false;

    // Cursor chase/snatch state
    double snatchStartTime = 0.0;
    Vector2 snatchOffset{};  // Cursor anchor stored in goose-local forward/right space during snatch
    // How far the goose pulls the cursor behind it when snatching
    float snatchPullDistance = 140.0f;
    // Circular snatch motion parameters
    float snatchAngle = 0.0f;           // current angular phase (radians)
    float snatchRadius = 60.0f;         // radius of circular motion in pixels
    float snatchAngularSpeed = 2.5f;    // radians per second

    // Per-goose tendencies (0-100)
    std::uint64_t traitSeed = 0;
    int attackMouseBias = 0; // added to global cursor chase chance
    int noteFetchBias = 0;   // increases chance to fetch notes
    int memeFetchBias = 0;   // increases chance to fetch memes

    // Per-goose dynamic settings
    bool cursorChaseEnabled = true;
    int  cursorChaseChance = 5;
    float snatchDuration = 3.0f;
    bool mudEnabled = true;
    int  mudChance = 15;
    float mudLifetime = 15.0f;

    // Window drag settings (copied from global defaults per goose)
    bool windowDragEnabled = true;
    int  windowDragChance = 3;
    int  windowYeetChance = 35;
    std::string dragWindowAddr;
    bool dragWindowWasTiled = false;
    int dragWindowOrigX = 0, dragWindowOrigY = 0;
    int dragWindowOrigW = 0, dragWindowOrigH = 0;
    int dragWindowDestX = 0, dragWindowDestY = 0;
    int dragImageDestX = 0, dragImageDestY = 0;
    double dragWindowStartTime = 0.0;
    DragPhase dragPhase = DRAG_APPROACH;
    int dragPhaseAttempt = 0;
    std::string dragOriginalFocusAddr;
    std::string dragWindowOrigWorkspace;
    std::string dragWindowHiddenWorkspace;
    std::string dragRetileTargetAddr;
    int dragWindowOrigWorkspaceId = -1;
    int dragWindowMonitorId = -1;
    bool dragWindowWasFocused = false;
    bool dragWindowWasHidden = false;
    bool dragRestoreToDestination = false;
    double dragCaptureReadyTime = 0.0;

    // Window yeet: the window screenshot is launched off the goose's head and
    // tumbles ballistically until it settles; the real window reappears there.
    bool dragIsYeet = false;
    Vector2 yeetPos{};              // device-space projectile position
    Vector2 yeetVel{};              // device-space velocity
    double yeetWindupStart = 0.0;
    int yeetBounces = 0;
    float yeetHeadDrive = -1.0f;    // >= 0 overrides the rig's neck extension

    RagdollState ragdoll;

    Goose(int _id, const std::string& _name, int screenW, int screenH);

    void Update(double dt, double time, int scrW, int scrH);
    void ForceFetch(int type, int w, int h);
    void ForceFetchText(const std::string& text, int w, int h);
    void ForceFetchMeme(const std::string& path, int w, int h);
    void ForceWander(int w, int h);
    bool ForceChase(int w, int h);
    bool ForceWindowDrag(int w, int h);
    bool ForceWindowYeet(int w, int h);
    void Draw(cairo_t* cr);

    // --- Ragdoll pickup -------------------------------------------------
    // Grab/Move take device-space (global) cursor coordinates. Only one goose
    // may be held at a time; ownership lives in g_heldGooseId.
    bool GrabAt(Vector2 devicePos);
    void MoveGrab(Vector2 devicePos);
    void ReleaseGrab(int w, int h);
    bool IsHeld() const { return state == HELD; }
    // True while the ragdoll pose still needs to be drawn (held or settling).
    bool IsRagdolling() const { return state == HELD || ragdoll.release > 0.002f; }
    // Device-space hit radius for cursor pickup.
    float GrabRadius() const;

    // Coordinate helpers
    Vector2 GetBeakTipWorld();
    Vector2 WorldToDevice(Vector2 worldPos);
    Vector2 DeviceToWorld(Vector2 devicePos);

    // NEW: pixel-perfect beak tip helpers (max-accuracy drag/snatch)
    Vector2 GetBeakTipDeviceRounded(); // device px, rounded to integers (stored as float)
    Vector2 GetBeakTipAttachWorld();   // world pos that maps exactly to those device px

    // Tier 3 support
    static Vector2 GetPredictedCursor(); // Returns s_predictedCursor

private:
    struct HonkState {
        bool initialized = false;
        double nextIdle = 0.0;
        double lastAny = -1e9;
        double lastChase = -1e9;
        double lastFetch = -1e9;
        double lastGeneric = -1e9;
    };

    HonkState m_honk;
    void UpdateDrag(double dt);
    bool UpdateYeetFlight(double dt, int w, int h);
    void UpdateRagdoll(double dt, double time);
    void UpdateNiriDrag(class NiriBackend* nb, double dt, double time, int w, int h);
    bool BeginWindowInteraction(int w, int h, bool yeet);
    void SetWindowDestinationFromImageCenter(Vector2 centerDevice);
    void CancelCurrentBehavior();
    void RestoreDraggedWindowNow(bool placeAtDestination);
    void ResetWindowDragState();
    void StartFetch(int w, int h);
    void DrawHeldItem(cairo_t* cr);
    void DrawEyes(cairo_t* cr, Vector2 fwd, float back);
    void PickNewTarget(int w, int h);
    Vector2 GetFootHome(float angleOffset);
    void SolveFeet(double time);
    void UpdateRig();
    void DrawEllipse(cairo_t* cr, Vector2 p, int rx, int ry, float r, float g, float b, float a=1.0);
    void DrawLine(cairo_t* cr, Vector2 a, Vector2 b, float width);
};

#endif // GOOSE_H
