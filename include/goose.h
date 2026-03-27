// ===========================
// goose.h
// ===========================
#ifndef GOOSE_H
#define GOOSE_H

#include <string>
#include <gtk/gtk.h>
#include "goose_math.h"
#include "assets.h"

enum GooseState { WANDER, FETCHING, RETURNING, CHASE_CURSOR, SNATCH_CURSOR };

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

class Goose {
public:
    int id;
    std::string name;
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

    // Honk timing (legacy fields; current impl uses internal map state in goose.cpp)
    double nextIdleHonkTime = 0.0;
    double lastChaseHonkTime = 0.0;

    Goose(int _id, const std::string& _name, int screenW, int screenH);

    void Update(double dt, double time, int scrW, int scrH);
    void ForceFetch(int type, int w, int h);
    void ForceFetchText(const std::string& text, int w, int h);
    void ForceWander(int w, int h);
    void Draw(cairo_t* cr);

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
    void UpdateDrag(double dt);
    void StartFetch(int w, int h);
    void DrawHeldItem(cairo_t* cr);
    void DrawEyes(cairo_t* cr, Vector2 fwd);
    void PickNewTarget(int w, int h);
    Vector2 GetFootHome(float angleOffset);
    void SolveFeet(double time);
    void UpdateRig();
    void DrawEllipse(cairo_t* cr, Vector2 p, int rx, int ry, float r, float g, float b, float a=1.0);
    void DrawLine(cairo_t* cr, Vector2 a, Vector2 b, float w, const float color[]);
    void DrawLine(cairo_t* cr, Vector2 a, Vector2 b, float w, float r, float g, float bl);
};

#endif // GOOSE_H
