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
#include "softbody.h"

// HELD: the cursor (or another goose's beak) owns the goose; the soft body is
// simulated around the attached point and the rest of the AI stays suspended.
// FLUNG: the goose was released (dropped or yeeted) and the soft body is a
// free projectile with gravity, drag and bouncing until it settles.
// CHASE_GOOSE: run at another goose to grab it by the beak.
// YEET_GOOSE: hold the grabbed goose, coil the neck back, then fling it.
enum GooseState { WANDER, FETCHING, RETURNING, CHASE_CURSOR, SNATCH_CURSOR, DRAG_WINDOW, HELD, FLUNG,
                  CHASE_GOOSE, YEET_GOOSE };

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

// Derived pickup/soft-body pose values. The pose itself lives in `softBody`;
// these fields are the small readouts the honk/squirm logic still uses.
struct RagdollState {
    Vector2 pivot{};      // last cursor target while held (device space)
    Vector2 pivotVel{};   // smoothed cursor velocity while held

    double nextSquirm = 0.0;
    float squirmDrive = 0.0f; // 1 right after a struggle impulse
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
    int forceItemFetch = -1; // -1: Random, 0: Meme, 1: Text, 2: Fortune
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

    // Goose-on-goose yeet. The attacker records its victim and the victim
    // records its attacker; each side checks the other every frame, so a
    // cancelled, removed or cursor-stolen partner never leaves a goose stuck.
    int yeetVictimId = -1;          // attacker: goose being chased/held
    int heldByGooseId = -1;         // victim: goose whose beak holds us
    double gooseYeetStart = 0.0;    // attacker: chase start, then windup start
    bool gooseYeetLaunched = false; // attacker: victim already thrown
    Vector2 gooseYeetLanding{};     // attacker: device-space landing aim point
    float gooseYeetSpinFrom = 0.0f;  // attacker: facing when the hammer spin starts
    float gooseYeetSpinTotal = 0.0f; // attacker: signed degrees, ends facing the aim

    RagdollState ragdoll;

    Goose(int _id, const std::string& _name, int screenW, int screenH);

    void Update(double dt, double time, int scrW, int scrH);
    void ForceFetch(int type, int w, int h);
    void ForceFetchText(const std::string& text, int w, int h);
    void ForceFetchMeme(const std::string& path, int w, int h);
    void ForceFetchFortune(int w, int h);
    void ForceWander(int w, int h);
    bool ForceChase(int w, int h);
    bool ForceWindowDrag(int w, int h);
    bool ForceWindowYeet(int w, int h);
    // Run at another goose, grab it by the beak and fling it. victimId -1
    // picks the nearest available goose. Returns false when none is available.
    bool ForceGooseYeet(int victimId, int w, int h);
    // True when this goose can currently be grabbed by another goose.
    bool CanBeGooseYeeted() const;
    void Draw(cairo_t* cr);

    // --- Soft-body pickup and yeet --------------------------------------
    // Grab/Move take device-space (global) cursor coordinates. Only one goose
    // may be held at a time; ownership lives in g_heldGooseId. Releasing with
    // a fast enough cursor flick throws the goose (FLUNG) so it tumbles,
    // bounces and flops back to its feet.
    bool GrabAt(Vector2 devicePos);
    void MoveGrab(Vector2 devicePos);
    void ReleaseGrab(int w, int h);
    // Victim side of a goose yeet: another goose's beak becomes the grab
    // point (device space). The fling solves the launch velocity, including
    // gravity and air drag, so the body arrives at `landingDevice`.
    bool GrabByGoose(int attackerId, Vector2 beakDevice);
    void FlingFromGoose(Vector2 landingDevice);
    void DropFromGoose();
    bool IsHeld() const { return state == HELD; }
    // True while the soft body owns the pose (held, thrown or recovering).
    bool IsSoftActive() const { return softActive; }
    bool IsRagdolling() const { return softActive; }
    // Read-only view of the simulation for tests and debug tooling.
    const SoftBody& Soft() const { return softBody; }
    // Conservative input-region radius plus visible-body hit testing.
    float GrabRadius() const;
    bool ContainsGrabPoint(Vector2 devicePos) const;

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

    // Soft-body skeleton indices. The torso is a ring sampled around the body
    // capsule; neck, head, beak and the two feet hang off it on springs.
    enum SoftIndex {
        SB_RING = 0,
        SB_RING_COUNT = 10,
        SB_NECK_BASE = SB_RING + SB_RING_COUNT, // 10
        SB_NECK_HEAD,                           // 11
        SB_HEAD,                                // 12
        SB_BEAK_TIP,                            // 13
        SB_FOOT_L,                              // 14
        SB_FOOT_R,                              // 15
        SB_COUNT
    };

    SoftBody softBody;
    int softHip[2] = { SB_RING, SB_RING }; // torso points the left/right legs hang from
    bool softActive = false;  // soft body owns the pose (HELD/FLUNG/recover)
    bool softThrown = false;  // the release carried enough speed to fly
    double softFlightElapsed = 0.0;
    double softSettleDeadline = 0.0; // gentle drops re-form by this age
    Vector2 softCursorVel{};         // smoothed cursor velocity while held
    Vector2 softLastTarget{};
    bool softTargetPrimed = false;

    void BuildSoftBody();
    void UpdateSoftBody(double dt, double time, int w, int h);
    void FinishSoftBody(int w, int h);
    void UpdateRigFromSoft();
    void DrawSoft(cairo_t* cr);

    void UpdateDrag(double dt);
    bool UpdateYeetFlight(double dt, int w, int h);
    void UpdateNiriDrag(class NiriBackend* nb, double dt, double time, int w, int h);
    bool BeginWindowInteraction(int w, int h, bool yeet);
    void SetWindowDestinationFromImageCenter(Vector2 centerDevice);
    // Shared hold setup for cursor and goose grabs; the release side enters
    // flight as a throw or a gentle in-place drop.
    void BeginHold(Vector2 devicePos);
    void EnterFlight(bool thrown);
    void ReleaseYeetVictim(); // drop any goose in our beak and leave the yeet states
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
    void DrawBentLine(cairo_t* cr, Vector2 a, Vector2 control, Vector2 b, float width);
};

#endif // GOOSE_H
