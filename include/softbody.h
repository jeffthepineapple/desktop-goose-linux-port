// ===========================
// softbody.h
// ===========================
#ifndef SOFTBODY_H
#define SOFTBODY_H

#include <vector>
#include "goose_math.h"

// Generic 2D soft body for the desktop goose.
//
// The body is a set of point masses wired together with distance springs,
// optional area (pressure) constraints and optional shape memories. It is
// solved with compliant position-based dynamics: spring and pressure compliance
// use seconds, not per-frame correction fractions. Mass-weighted shape matching
// and internal damping retain elastic follow-through without damping rigid
// rotation. Contact impulses use incoming velocity, not clamped displacement.

struct SoftPoint {
    Vector2 pos{};
    Vector2 vel{};
    Vector2 correction{}; // constraint displacement, separate from world-position rounding
    Vector2 stepStart{};  // feasible pose before this substep's motion
    float invMass = 1.0f;  // 0 pins the point
    signed char contactX = 0, contactY = 0; // outward wall normals this substep

    void Correct(Vector2 delta) {
        pos += delta;
        correction += delta;
    }
};

struct SoftSpring {
    int a = 0;
    int b = 0;
    float rest = 0.0f;
    float stiffness = 1.0f; // 0..1 material stiffness, independent of iterations
    float lambda = 0.0f;    // accumulated XPBD impulse within a substep
    float maxStretch = 0.0f; // optional anatomical length/rest limit; 0 disables
};

struct SoftArea {
    std::vector<int> points; // ordered polygon
    float rest = 0.0f;       // signed target area in px^2 (preserves winding)
    float stiffness = 0.5f;
    float lambda = 0.0f;
    std::vector<Vector2> gradients; // scratch storage, allocated at construction
};

struct SoftMemory {          // soft shape matching toward a captured pose
    std::vector<int> points;
    std::vector<Vector2> rest;
    float stiffness = 0.3f;  // 0..1
    float solverStiffness = 0.0f;
};

class SoftBody {
public:
    std::vector<SoftPoint> points;
    std::vector<SoftSpring> springs;
    std::vector<SoftArea> areas;
    std::vector<SoftMemory> memories;

    Vector2 gravity{0.0f, 1400.0f};
    float gravityScale = 1.0f;  // 0 = weightless (in-place recovery)
    float linearDamping = 1.0f; // 1/s velocity damping
    float restitution = 0.45f;  // wall bounce
    float friction = 0.35f;     // Coulomb contact friction coefficient
    float maxSpeed = 6000.0f;
    int iterations = 6;
    int substeps = 3;

    // Desktop bounds in the same space as the points.
    float boundMinX = 0.0f, boundMinY = 0.0f;
    float boundMaxX = 1920.0f, boundMaxY = 1080.0f;
    bool collideBounds = true;

    // Cursor hold: one point is pulled toward a target each sub-step.
    int attachedPoint = -1;
    Vector2 attachTarget{};
    float attachStiffness = 0.45f; // 0..1 compliant cursor spring

    int AddPoint(Vector2 pos, float invMass = 1.0f);
    void AddSpring(int a, int b, float stiffness = 1.0f);
    void AddArea(std::vector<int> points, float stiffness = 0.5f);
    void AddMemory(const std::vector<int>& points, float stiffness = 0.3f);

    void SetBounds(float minX, float minY, float maxX, float maxY) {
        boundMinX = minX;
        boundMinY = minY;
        boundMaxX = maxX;
        boundMaxY = maxY;
    }

    int PointCount() const { return (int)points.size(); }
    Vector2 Center() const;
    Vector2 CenterOfRange(int begin, int count) const;
    Vector2 AverageVelocity() const;
    float AverageSpeed() const;
    void ResetVelocities();
    bool IsFinite() const;
    // Rigidly move the whole body and its hold target; velocities, constraint
    // state and the shape are unchanged.
    void Translate(Vector2 delta);

    void Step(float dt);

private:
    Vector2 attachLambda{};
    Vector2 previousAttachTarget{};
    int previousAttachedPoint = -1;

    void SolveAttach(float dt, Vector2 target);
    void SolveSprings(float dt);
    void SolveAreas(float dt);
    void SolveMemories();
    void ApplyBounds();
    void RecoverVelocities(float dt);
    void DampSprings(float dt);
    void LimitStrain();
};

#endif // SOFTBODY_H
