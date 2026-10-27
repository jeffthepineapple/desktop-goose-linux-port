#undef NDEBUG
#include <cassert>
#include <cmath>
#include <vector>
#include "config.h"
#include "goose.h"
#include "world.h"

// Contract test for pickup + yeet: grabbing starts the soft body, dragging
// deforms it, flicking and releasing throws it (FLUNG), the solver keeps every
// point finite and on the desktop through the whole flight, and the goose
// eventually stands back up into WANDER.
static void TestSoftBodyPickupAndYeet() {
    g_config.globalScale = 1.0f;
    g_config.ragdollGravity = 1400.0f;
    g_config.ragdollDamping = 2.4f;
    g_config.ragdollSquirmStrength = 0.0f;
    g_config.softbodyStiffness = 1.0f;
    g_config.softbodyPressure = 0.5f;
    g_config.softbodyShapeMemory = 0.45f;
    g_config.softbodyGrabStiffness = 0.4f;
    g_config.softbodyAirDrag = 0.5f;
    g_config.softbodyBounce = 0.45f;
    g_config.softbodyFriction = 0.35f;
    g_config.softbodyYeetSpeed = 420.0f;
    g_config.softbodyIterations = 4;
    g_config.softbodySubsteps = 3;

    Goose goose(0, "Test Goose", 800, 600);
    goose.pos = {300.0f, 300.0f};
    goose.dir = 90.0f;
    goose.rig.body = {300.0f, 286.0f};
    goose.rig.underbody = {300.0f, 291.0f};
    goose.rig.neckBase = {300.0f, 270.0f};
    goose.rig.neckHead = {300.0f, 250.0f};
    goose.rig.head1 = {300.0f, 248.0f};
    goose.rig.head2 = {300.0f, 246.0f};
    goose.rig.lFoot.currentPos = {295.0f, 300.0f};
    goose.rig.rFoot.currentPos = {305.0f, 300.0f};

    const Vector2 bodyPoint{300.0f, 286.0f};
    assert(goose.ContainsGrabPoint(bodyPoint));
    assert(goose.ContainsGrabPoint({300.0f, 250.0f}));
    assert(!goose.ContainsGrabPoint({380.0f, 300.0f}));

    assert(goose.GrabAt(bodyPoint));
    assert(goose.IsHeld());
    assert(goose.IsSoftActive());

    // Offscreen draw smoke test: the deformed pose must render through the
    // soft path without emitting cairo errors.
    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 800, 600);
    cairo_t* cr = cairo_create(surface);

    const size_t pointCount = goose.Soft().points.size();
    assert(pointCount >= 16);
    std::vector<Vector2> initial;
    initial.reserve(pointCount);
    for (const auto& p : goose.Soft().points) {
        assert(std::isfinite(p.pos.x) && std::isfinite(p.pos.y));
        initial.push_back(p.pos);
    }

    // Drag the cursor up and across the screen over a few frames: the body
    // must visibly deform while staying finite.
    double t = 0.0;
    goose.MoveGrab(bodyPoint);
    goose.Update(1.0 / 60.0, t, 800, 600);
    for (int i = 1; i <= 30; ++i) {
        t += 1.0 / 60.0;
        goose.MoveGrab({bodyPoint.x + i * 10.0f, bodyPoint.y - i * 6.0f});
        goose.Update(1.0 / 60.0, t, 800, 600);
    }
    assert(goose.IsHeld());
    assert(goose.IsSoftActive());
    assert(goose.Soft().IsFinite());
    goose.Draw(cr);
    assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

    float maxMove = 0.0f;
    for (size_t i = 0; i < pointCount; ++i) {
        maxMove = std::max(
            maxMove, Vector2::Distance(initial[i], goose.Soft().points[i].pos));
    }
    assert(maxMove > 10.0f);

    // Flick hard, then release: the release speed must classify as a yeet.
    goose.MoveGrab({bodyPoint.x + 400.0f, bodyPoint.y - 180.0f});
    t += 1.0 / 60.0;
    goose.Update(1.0 / 60.0, t, 800, 600);
    assert(goose.IsHeld());
    goose.ReleaseGrab(800, 600);
    assert(!goose.IsHeld());
    assert(goose.state == FLUNG);
    assert(goose.IsSoftActive());
    t += 1.0 / 60.0;
    goose.Update(1.0 / 60.0, t, 800, 600);
    goose.Draw(cr);
    assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

    // Whole flight: never NaN, never off the desktop, and eventually back up.
    bool recovered = false;
    for (int i = 0; i < 900; ++i) {
        t += 1.0 / 60.0;
        goose.Update(1.0 / 60.0, t, 800, 600);
        if (!goose.IsSoftActive()) {
            recovered = true;
            break;
        }
        assert(goose.Soft().IsFinite());
        for (const auto& p : goose.Soft().points) {
            assert(p.pos.x > -1.0f && p.pos.x < 801.0f);
            assert(p.pos.y > -1.0f && p.pos.y < 601.0f);
        }
    }
    assert(recovered);
    assert(goose.state == WANDER);
    assert(goose.pos.x > 0.0f && goose.pos.x < 800.0f);
    assert(goose.pos.y > 0.0f && goose.pos.y < 600.0f);
    goose.Draw(cr);
    assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS);

    // A slow, gentle release must not throw the goose: it re-forms roughly in
    // place and stands back up.
    assert(goose.GrabAt(goose.pos));
    const Vector2 hold = goose.pos;
    goose.MoveGrab(hold);
    t += 1.0 / 60.0;
    goose.Update(1.0 / 60.0, t, 800, 600);
    for (int i = 1; i <= 10; ++i) {
        t += 1.0 / 60.0;
        goose.MoveGrab({ hold.x + i * 0.5f, hold.y });
        goose.Update(1.0 / 60.0, t, 800, 600);
    }
    goose.ReleaseGrab(800, 600);
    assert(!goose.IsHeld());
    assert(goose.state == FLUNG);
    bool stood = false;
    for (int i = 0; i < 180; ++i) {
        t += 1.0 / 60.0;
        goose.Update(1.0 / 60.0, t, 800, 600);
        if (!goose.IsSoftActive()) {
            stood = true;
            break;
        }
    }
    assert(stood);
    assert(goose.state == WANDER);
    assert(Vector2::Distance(hold, goose.pos) < 100.0f);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// A cursor can reverse faster than any muscle responds. Across low frame rates
// and the softest supported tissue, no connecting link may grow into a detached
// head/foot. Include the release and repeated screen-edge impacts.
static void TestHighVelocityPartsStayAttached() {
    g_config.ragdollSquirmStrength = 0.0f;
    for (int fps : {20, 60, 144}) {
        for (bool softest : {false, true}) {
            g_config.softbodyStiffness = softest ? 0.25f : 1.0f;
            g_config.softbodyPressure = softest ? 0.0f : 0.5f;
            g_config.softbodyShapeMemory = softest ? 0.0f : 0.45f;
            g_config.softbodyIterations = softest ? 1 : 4;
            g_config.softbodySubsteps = softest ? 1 : 3;
            g_config.softbodyBounce = 0.95f;
            Goose goose(20, "Fast Goose", 800, 600);
            goose.pos = {400, 300};
            goose.dir = 0;
            double time = 0.0;
            const double dt = 1.0 / fps;
            goose.Update(dt, time, 800, 600);
            const bool grabbed = goose.GrabAt(goose.rig.neckHead);
            assert(grabbed);
            auto checkConnections = [&] {
                assert(goose.Soft().IsFinite());
                for (const auto& spring : goose.Soft().springs) {
                    const float length = Vector2::Distance(
                        goose.Soft().points[spring.a].pos,
                        goose.Soft().points[spring.b].pos);
                    assert(length <= spring.rest * spring.maxStretch * 1.01f + 0.001f);
                }
            };
            for (int frame = 0; frame < fps * 2; ++frame) {
                goose.MoveGrab({frame % 2 ? 20.0f : 780.0f,
                                frame % 3 ? 20.0f : 580.0f});
                time += dt;
                goose.Update(dt, time, 800, 600);
                checkConnections();
            }
            goose.ReleaseGrab(800, 600);
            for (int frame = 0; frame < fps * 7 && goose.IsSoftActive(); ++frame) {
                time += dt;
                goose.Update(dt, time, 800, 600);
                if (goose.IsSoftActive()) checkConnections();
            }
            assert(!goose.IsSoftActive());
        }
    }
}

// Swinging in circles must feel the same at every global scale. The body lives
// in a space scaled about the goose center; if re-centering moves the body on
// screen, the held point falls far behind the cursor at small scales.
static float MeanSwingLag(float scale) {
    g_config.globalScale = scale;
    g_config.ragdollSquirmStrength = 0.0f;
    g_config.softbodyStiffness = 1.0f;
    g_config.softbodyPressure = 0.5f;
    g_config.softbodyShapeMemory = 0.45f;
    g_config.softbodyGrabStiffness = 0.4f;
    g_config.softbodyIterations = 4;
    g_config.softbodySubsteps = 3;
    const int w = 1920, h = 1080;
    Goose goose(30, "Swing Goose", w, h);
    goose.pos = {960, 540};
    double time = 0.0;
    goose.Update(1.0 / 60.0, time, w, h);
    const Vector2 start = goose.WorldToDevice(goose.rig.neckHead);
    assert(goose.GrabAt(start));
    const Vector2 center{start.x - 150.0f, start.y};
    float total = 0.0f;
    int samples = 0;
    for (int frame = 0; frame < 240; ++frame) {
        const float angle = 2.0f * (float)M_PI * 2.0f * (float)(frame + 1) / 60.0f;
        const Vector2 cursor{center.x + 150.0f * std::cos(angle),
                             center.y + 150.0f * std::sin(angle)};
        goose.MoveGrab(cursor);
        time += 1.0 / 60.0;
        goose.Update(1.0 / 60.0, time, w, h);
        assert(goose.IsHeld());
        if (frame < 60) continue;
        const auto& soft = goose.Soft();
        total += Vector2::Distance(
            goose.WorldToDevice(soft.points[soft.attachedPoint].pos), cursor);
        ++samples;
    }
    g_config.globalScale = 1.0f;
    return total / (float)samples;
}

static void TestSwingLagIndependentOfScale() {
    const float reference = MeanSwingLag(1.0f);
    assert(MeanSwingLag(0.5f) <= reference * 1.5f);
    assert(MeanSwingLag(2.0f) <= reference * 1.5f);
}

int main() {
    TestSoftBodyPickupAndYeet();
    TestHighVelocityPartsStayAttached();
    TestSwingLagIndependentOfScale();
    return 0;
}
