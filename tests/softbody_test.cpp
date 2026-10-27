// Behavioural physics regressions for SoftBody, driven only through the public
// API. Each check asserts a numeric physical invariant (impact speed, momentum,
// area recovery, steady stretch, boundedness, rotation covariance) rather than
// wiring or implementation-specific constants.
#include <algorithm>
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <vector>
#include <cstdio>
#include <string>

#include "softbody.h"

namespace {

int g_failures = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

std::string Fmt(const std::string& label, double value) {
    return label + " = " + std::to_string(value);
}

constexpr float kDt = 1.0f / 60.0f;
constexpr float kGravity = 1400.0f;

// Free body: no gravity, no damping, no friction, no walls, one substep.
SoftBody MakeFreeBody() {
    SoftBody body;
    body.gravity = {0.0f, 0.0f};
    body.gravityScale = 1.0f;
    body.linearDamping = 0.0f;
    body.friction = 0.0f;
    body.substeps = 1;
    body.iterations = 6;
    body.collideBounds = false;
    return body;
}

// Free body with a right wall at x = 100 (the tall y range keeps y clear).
SoftBody WallBody() {
    SoftBody body = MakeFreeBody();
    body.SetBounds(0.0f, 0.0f, 100.0f, 1000.0f);
    body.collideBounds = true;
    return body;
}

// Mass = 1 / invMass. Pinned points (invMass 0) are excluded.
Vector2 MassCenter(const SoftBody& body) {
    Vector2 sum{};
    float total = 0.0f;
    for (const SoftPoint& p : body.points) {
        if (p.invMass <= 0.0f) continue;
        const float m = 1.0f / p.invMass;
        sum += p.pos * m;
        total += m;
    }
    return sum / total;
}

Vector2 MassVelocity(const SoftBody& body) {
    Vector2 sum{};
    float total = 0.0f;
    for (const SoftPoint& p : body.points) {
        if (p.invMass <= 0.0f) continue;
        const float m = 1.0f / p.invMass;
        sum += p.vel * m;
        total += m;
    }
    return sum / total;
}

// Relative area error of the first area constraint, |A - rest| / rest.
double AreaError(const SoftBody& body) {
    const SoftArea& area = body.areas[0];
    const int n = (int)area.points.size();
    double area2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const Vector2& a = body.points[area.points[i]].pos;
        const Vector2& b = body.points[area.points[(i + 1) % n]].pos;
        area2 += (double)a.x * b.y - (double)b.x * a.y;
    }
    const double current = std::fabs(area2) * 0.5;
    return std::fabs(current - area.rest) / area.rest;
}

Vector2 Rotate(Vector2 v, float theta) {
    const float c = std::cos(theta);
    const float s = std::sin(theta);
    return { v.x * c - v.y * s, v.x * s + v.y * c };
}

// ---------------------------------------------------------------------------
// 1. Wall contact
// ---------------------------------------------------------------------------

// A point crossing the wall mid-step rebounds with restitution times the
// incoming speed, then leaves the wall at that speed.
void TestWallBounceUsesIncomingSpeed() {
    const float dt = 0.01f;
    struct Case { float incoming; float restitution; };
    const Case cases[] = { {200.0f, 0.5f}, {50.0f, 0.5f}, {200.0f, 0.8f} };
    for (const Case& c : cases) {
        SoftBody body = WallBody();
        body.restitution = c.restitution;
        body.substeps = 1;
        // Start short of the wall so the impact lands inside this step.
        const int p = body.AddPoint({100.0f - c.incoming * dt * 0.5f, 500.0f});
        body.points[p].vel = {c.incoming, 0.0f};

        body.Step(dt);
        const float expectedVx = -c.restitution * c.incoming;
        const std::string tag = "bounce(in=" + std::to_string(c.incoming) +
                                ",e=" + std::to_string(c.restitution) + ") ";
        Expect(std::fabs(body.points[p].vel.x - expectedVx) < 1.0f,
               tag + Fmt("vx", body.points[p].vel.x));
        Expect(std::fabs(body.points[p].pos.x - 100.0f) < 1e-3f,
               tag + Fmt("impact x", body.points[p].pos.x));

        // Next step: no contact, so it travels away at the rebound speed.
        body.Step(dt);
        const float expectedX = 100.0f + expectedVx * dt;
        Expect(std::fabs(body.points[p].pos.x - expectedX) < 0.05f,
               tag + Fmt("leave x", body.points[p].pos.x));
        Expect(std::fabs(body.points[p].vel.x - expectedVx) < 1.0f,
               tag + Fmt("leave vx", body.points[p].vel.x));
    }
}

// A point resting on the wall must not gain bounce energy: with no forces it
// stays put with zero velocity, and under a constant push into the wall its
// rebound speed stays within one frame's gravity impulse.
void TestRestingContactGainsNoBounceEnergy() {
    {
        SoftBody body = WallBody();
        body.restitution = 0.5f;
        const int p = body.AddPoint({100.0f, 500.0f});
        for (int i = 0; i < 100; ++i) body.Step(kDt);
        const SoftPoint& q = body.points[p];
        Expect(std::fabs(q.pos.x - 100.0f) < 1e-3f, Fmt("idle x", q.pos.x));
        Expect(std::fabs(q.vel.x) < 1e-3f, Fmt("idle vx", q.vel.x));
        Expect(std::fabs(q.vel.y) < 1e-3f, Fmt("idle vy", q.vel.y));
    }
    {
        SoftBody body = WallBody();
        body.restitution = 0.5f;
        body.friction = 0.35f;
        body.gravity = {kGravity, 0.0f}; // pushes into the wall at x = 100
        body.substeps = 3;
        const int p = body.AddPoint({100.0f, 500.0f});

        float maxLateSpeed = 0.0f;
        float minLateX = 1e30f;
        float maxX = -1e30f;
        for (int i = 0; i < 600; ++i) {
            body.Step(kDt);
            const SoftPoint& q = body.points[p];
            maxX = std::max(maxX, q.pos.x);
            if (i >= 300) {
                maxLateSpeed = std::max(maxLateSpeed, std::fabs(q.vel.x));
                minLateX = std::min(minLateX, q.pos.x);
            }
        }
        Expect(maxX <= 100.0f + 1e-3f, Fmt("pressed max x", maxX));
        Expect(minLateX > 99.5f, Fmt("pressed min late x", minLateX));
        Expect(maxLateSpeed <= kGravity * kDt,
               Fmt("pressed late |vx|", maxLateSpeed));
    }
}

// Wall friction slows sliding motion along the wall but must never reverse it.
void TestWallFrictionNeverReversesTangentialMotion() {
    const float dt = 0.01f;
    const float frictions[] = { 0.0f, 0.35f, 0.9f };
    const float tangential[] = { 300.0f, -300.0f };
    for (float friction : frictions) {
        for (float vy : tangential) {
            SoftBody body = WallBody();
            body.restitution = 0.5f;
            body.friction = friction;
            const int p = body.AddPoint({99.0f, 500.0f});
            body.points[p].vel = {200.0f, vy};

            body.Step(dt);
            const SoftPoint& q = body.points[p];
            const std::string tag = "friction(" + std::to_string(friction) +
                                    ",vy=" + std::to_string(vy) + ") ";
            Expect(std::fabs(q.vel.x + 100.0f) < 1.0f, tag + Fmt("vx", q.vel.x));
            Expect(q.vel.y * vy >= 0.0f, tag + Fmt("vy reversed", q.vel.y));
            Expect(std::fabs(q.vel.y) <= std::fabs(vy) + 1e-3f,
                   tag + Fmt("|vy| grew", q.vel.y));
            if (friction == 0.0f) {
                Expect(std::fabs(q.vel.y - vy) < 1.0f,
                       tag + Fmt("frictionless vy", q.vel.y));
            } else {
                Expect(std::fabs(q.vel.y) < std::fabs(vy) - 1.0f,
                       tag + Fmt("friction did not slow |vy|", q.vel.y));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 2. Unequal-mass spring
// ---------------------------------------------------------------------------

// A free, deformed spring between unequal masses is an isolated system: the
// mass-weighted centre must keep moving at its initial velocity.
void TestUnequalMassSpringConservesMassWeightedCenter() {
    SoftBody body = MakeFreeBody();
    body.substeps = 3;
    const int a = body.AddPoint({500.0f, 500.0f}, 1.0f);   // mass 1
    const int b = body.AddPoint({560.0f, 500.0f}, 0.25f);  // mass 4
    body.AddSpring(a, b, 0.8f);
    body.points[b].pos = {575.0f, 520.0f};                 // stretched, sheared
    body.points[a].vel = {0.0f, 100.0f};
    body.points[b].vel = {0.0f, -50.0f};

    const Vector2 com0 = MassCenter(body);
    const Vector2 vcom = MassVelocity(body);
    float maxDrift = 0.0f;
    for (int step = 1; step <= 300; ++step) {
        body.Step(kDt);
        const float t = step * kDt;
        const Vector2 expected = com0 + vcom * t;
        maxDrift = std::max(maxDrift, Vector2::Distance(MassCenter(body), expected));
    }
    Expect(maxDrift < 0.05f, Fmt("spring COM drift px", maxDrift));
}

// A single spring has one constraint, so the position correction splits in
// inverse-mass proportion: the light point moves four times as far as the
// heavy one (mass 1 vs mass 4).
void TestUnequalMassSpringSharesCorrectionByInverseMass() {
    SoftBody body = MakeFreeBody();
    const int a = body.AddPoint({500.0f, 500.0f}, 1.0f);
    const int b = body.AddPoint({560.0f, 500.0f}, 0.25f);
    body.AddSpring(a, b, 0.8f);
    body.points[b].pos = {580.0f, 500.0f}; // 20 px stretch, at rest

    const Vector2 a0 = body.points[a].pos;
    const Vector2 b0 = body.points[b].pos;
    body.Step(kDt);
    const Vector2 da = body.points[a].pos - a0;
    const Vector2 db = body.points[b].pos - b0;
    const float ratio = Vector2::Length(da) / Vector2::Length(db);
    Expect(da.x > 0.0f && db.x < 0.0f, Fmt("spring pull direction da.x", da.x));
    Expect(ratio > 3.9f && ratio < 4.1f, Fmt("light/heavy displacement ratio", ratio));
}

// ---------------------------------------------------------------------------
// 3. Area (pressure) constraint
// ---------------------------------------------------------------------------

// Square 100x100 with unequal masses. The rest area is captured before the
// vertex is pushed out, so the body starts 15% too large.
SoftBody MakeAreaBody(float cornerInvMass, float damping) {
    SoftBody body = MakeFreeBody();
    body.linearDamping = damping;
    body.substeps = 3;
    body.AddPoint({400.0f, 400.0f}, cornerInvMass);
    body.AddPoint({500.0f, 400.0f}, 0.5f);
    body.AddPoint({500.0f, 500.0f}, 0.25f);
    body.AddPoint({400.0f, 500.0f}, 1.0f);
    body.AddArea({0, 1, 2, 3}, 0.8f); // rest = 10000
    body.points[2].pos = {540.0f, 490.0f};
    return body;
}

// With no external forces the area correction must not move the centre of
// mass, and the body must still have its area back once it settles.
void TestAreaRecoveryKeepsMassWeightedCenter() {
    {
        SoftBody body = MakeAreaBody(1.0f, 0.0f);
        Expect(AreaError(body) > 0.10, Fmt("initial area error", AreaError(body)));
        const Vector2 com0 = MassCenter(body);
        float maxDrift = 0.0f;
        for (int i = 0; i < 300; ++i) {
            body.Step(kDt);
            maxDrift = std::max(maxDrift, Vector2::Distance(MassCenter(body), com0));
        }
        Expect(maxDrift < 0.05f, Fmt("area COM drift px", maxDrift));
    }
    {
        SoftBody body = MakeAreaBody(1.0f, 3.0f);
        for (int i = 0; i < 600; ++i) body.Step(kDt);
        Expect(AreaError(body) < 0.03, Fmt("settled area error", AreaError(body)));
        Expect(body.IsFinite(), "free area settled finite");
    }
}

// A pinned vertex is an external anchor: it must not move at all, and the area
// must still recover with the remaining vertices.
void TestPinnedAreaRecoversAndKeepsPinFixed() {
    SoftBody body = MakeAreaBody(0.0f, 3.0f);
    const Vector2 pin0 = body.points[0].pos;
    for (int i = 0; i < 600; ++i) body.Step(kDt);
    const Vector2 pin = body.points[0].pos;
    Expect(Vector2::Distance(pin, pin0) < 1e-4f, Fmt("pin drift px", Vector2::Distance(pin, pin0)));
    Expect(AreaError(body) < 0.03, Fmt("pinned settled area error", AreaError(body)));
    Expect(body.IsFinite(), "pinned area settled finite");
}

// ---------------------------------------------------------------------------
// 4. Steady suspension and repeated pulls
// ---------------------------------------------------------------------------

// Pinned anchor at y=100 with a hanging mass on a spring of rest length 100.
SoftBody MakeSuspension(float stiffness) {
    SoftBody body;
    body.gravity = {0.0f, kGravity};
    body.gravityScale = 1.0f;
    body.linearDamping = 2.0f;
    body.friction = 0.0f;
    body.collideBounds = false;
    const int anchor = body.AddPoint({500.0f, 100.0f}, 0.0f);
    const int mass = body.AddPoint({500.0f, 200.0f}, 1.0f);
    body.AddSpring(anchor, mass, stiffness);
    return body;
}

struct Settled {
    float stretch;
    float speed;
};

Settled SettleSuspension(float fps, int substeps, int iterations) {
    SoftBody body = MakeSuspension(0.8f);
    body.substeps = substeps;
    body.iterations = iterations;
    const float dt = 1.0f / fps;
    const int steps = (int)std::lround(8.0f * fps);
    for (int i = 0; i < steps; ++i) body.Step(dt);
    const SoftPoint& mass = body.points[1];
    return { Vector2::Distance(body.points[0].pos, mass.pos) - 100.0f,
             Vector2::Length(mass.vel) };
}

// Gravity-loaded spring must reach the same steady stretch whatever the frame
// rate, substep count or iteration count.
void TestSuspensionStretchIndependentOfTimestep() {
    const float fpsList[] = { 30.0f, 60.0f, 144.0f };
    const int substepList[] = { 1, 3, 6 };
    const int iterationList[] = { 1, 6 };

    float minStretch = 1e30f;
    float maxStretch = -1e30f;
    double sum = 0.0;
    int count = 0;
    for (float fps : fpsList) {
        for (int substeps : substepList) {
            for (int iterations : iterationList) {
                const Settled s = SettleSuspension(fps, substeps, iterations);
                const std::string tag = "suspension(fps=" + std::to_string(fps) +
                    ",sub=" + std::to_string(substeps) +
                    ",it=" + std::to_string(iterations) + ") ";
                Expect(s.stretch > 0.0f, tag + Fmt("stretch", s.stretch));
                Expect(s.speed < 1.0f, tag + Fmt("unsettled speed", s.speed));
                minStretch = std::min(minStretch, s.stretch);
                maxStretch = std::max(maxStretch, s.stretch);
                sum += s.stretch;
                ++count;
            }
        }
    }
    const float mean = (float)(sum / count);
    Expect(maxStretch - minStretch <= 0.05f * mean,
           Fmt("stretch spread px", maxStretch - minStretch));
}

// Repeated cursor pulls left and right must keep the body finite and the
// spring from stretching beyond twice its rest length.
void TestRepeatedPullsStayFiniteAndBounded() {
    SoftBody body = MakeSuspension(0.8f);
    body.attachedPoint = 1;
    body.attachStiffness = 0.45f;
    bool allFinite = true;
    float maxRatio = 0.0f;
    for (int cycle = 0; cycle < 10; ++cycle) {
        body.attachTarget = {cycle % 2 == 0 ? 350.0f : 650.0f, 200.0f};
        for (int i = 0; i < 60; ++i) {
            body.Step(kDt);
            allFinite = allFinite && body.IsFinite();
            const float length = Vector2::Distance(body.points[0].pos, body.points[1].pos);
            maxRatio = std::max(maxRatio, std::fabs(length - 100.0f) / 100.0f);
        }
    }
    Expect(allFinite, "pulled body finite");
    Expect(maxRatio < 1.0f, Fmt("max spring strain ratio", maxRatio));
}

// ---------------------------------------------------------------------------
// 5. Shape memory
// ---------------------------------------------------------------------------

// Quad with unequal masses and a shape memory, captured at angle theta. A
// rigidly rotated copy must evolve as the rotated original, and the shape
// memory must not move the mass-weighted centre.
SoftBody MakeMemoryScene(float theta) {
    const Vector2 offset{300.0f, 300.0f};
    const Vector2 base[4] = { {0.0f, 0.0f}, {100.0f, 0.0f},
                              {100.0f, 60.0f}, {0.0f, 60.0f} };
    const float invMass[4] = { 1.0f, 0.5f, 2.0f, 1.0f };

    SoftBody body = MakeFreeBody();
    body.substeps = 3;
    std::vector<int> ids;
    for (int i = 0; i < 4; ++i) {
        ids.push_back(body.AddPoint(Rotate(base[i], theta) + offset, invMass[i]));
    }
    body.AddMemory(ids, 0.5f);
    body.points[2].pos = body.points[2].pos + Rotate({15.0f, -10.0f}, theta);
    body.points[1].vel = Rotate({0.0f, 80.0f}, theta);
    return body;
}

void TestShapeMemoryIsRotationCovariantWithUnequalMasses() {
    const float theta = 0.7f;
    const Vector2 offset{300.0f, 300.0f};
    SoftBody plain = MakeMemoryScene(0.0f);
    SoftBody turned = MakeMemoryScene(theta);

    const Vector2 com0 = MassCenter(plain);
    const Vector2 vcom = MassVelocity(plain);
    float maxDrift = 0.0f;
    for (int step = 1; step <= 200; ++step) {
        plain.Step(kDt);
        turned.Step(kDt);
        const float t = step * kDt;
        maxDrift = std::max(maxDrift,
            Vector2::Distance(MassCenter(plain), com0 + vcom * t));
    }
    Expect(maxDrift < 0.05f, Fmt("memory COM drift px", maxDrift));

    float maxMismatch = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const Vector2 expected = Rotate(plain.points[i].pos - offset, theta);
        const Vector2 actual = turned.points[i].pos - offset;
        maxMismatch = std::max(maxMismatch, Vector2::Distance(expected, actual));
    }
    Expect(maxMismatch < 0.05f, Fmt("rotation covariance px", maxMismatch));
}

// Reaching a tissue length cap must remove stretch, not the whole frame's
// translation. A taut chain under a fast pull used to roll every point back
// to its previous pose, including momentum shared by the whole body.
void TestStrainLimitsPreserveTranslation() {
    SoftBody body = MakeFreeBody();
    for (int i = 0; i < 8; ++i) {
        body.AddPoint({500.0f + i * 10.0f, 500.0f}, 1.0f / (i + 1));
        if (i > 0) {
            body.AddSpring(i - 1, i, 0.0f);
            body.springs.back().maxStretch = 1.5f;
        }
    }
    for (int i = 0; i < 8; ++i) {
        body.points[i].pos.x = 500.0f + i * 15.0f;
        body.points[i].vel = {1000.0f, 200.0f};
    }
    body.points.back().vel.x += 2000.0f;
    const Vector2 center = MassCenter(body);
    const Vector2 velocity = MassVelocity(body);
    body.Step(kDt);
    const float drift = Vector2::Distance(MassCenter(body), center + velocity * kDt);
    Expect(drift < 0.01f, Fmt("strain-limited translation error px", drift));
    Expect(Vector2::Distance(MassVelocity(body), velocity) < 0.1f,
           Fmt("strain-limited momentum error", Vector2::Distance(MassVelocity(body), velocity)));
    for (const auto& spring : body.springs) {
        Expect(Vector2::Distance(body.points[spring.a].pos, body.points[spring.b].pos) <=
                   spring.rest * spring.maxStretch + 0.001f,
               "taut chain stays connected while translating");
    }
}

} // namespace

int main() {
    TestWallBounceUsesIncomingSpeed();
    TestRestingContactGainsNoBounceEnergy();
    TestWallFrictionNeverReversesTangentialMotion();
    TestUnequalMassSpringConservesMassWeightedCenter();
    TestUnequalMassSpringSharesCorrectionByInverseMass();
    TestAreaRecoveryKeepsMassWeightedCenter();
    TestPinnedAreaRecoversAndKeepsPinFixed();
    TestSuspensionStretchIndependentOfTimestep();
    TestRepeatedPullsStayFiniteAndBounded();
    TestShapeMemoryIsRotationCovariantWithUnequalMasses();
    TestStrainLimitsPreserveTranslation();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d softbody physics check(s) failed\n", g_failures);
    } else {
        std::printf("softbody physics contracts passed\n");
    }
    assert(g_failures == 0);
    return g_failures == 0 ? 0 : 1;
}
