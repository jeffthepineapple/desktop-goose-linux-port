#include "softbody.h"

#include <algorithm>
#include <cmath>

namespace {
// Calibrate existing 0..1 controls at a 180 Hz reference step. Compliance
// stays constant as frame rate, substeps and iteration count change.
float Compliance(float stiffness) {
    const float k = Clamp(stiffness, 0.0001f, 1.0f);
    constexpr float referenceStep = 1.0f / 180.0f;
    return referenceStep * referenceStep * (1.0f - k) / k;
}

float Cross(Vector2 a, Vector2 b) {
    return a.x * b.y - a.y * b.x;
}
}

// =========================================================
// CONSTRUCTION HELPERS
// =========================================================

int SoftBody::AddPoint(Vector2 pos, float invMass) {
    SoftPoint p;
    p.pos = pos;
    p.invMass = std::max(0.0f, invMass);
    points.push_back(p);
    return (int)points.size() - 1;
}

void SoftBody::AddSpring(int a, int b, float stiffness) {
    if (a < 0 || b < 0) return;
    if (a >= (int)points.size() || b >= (int)points.size()) return;
    if (a == b) return;

    SoftSpring s;
    s.a = a;
    s.b = b;
    s.rest = Vector2::Distance(points[a].pos, points[b].pos);
    s.stiffness = Clamp(stiffness, 0.0f, 1.0f);
    if (s.rest < 1e-3f) return;
    springs.push_back(s);
}

void SoftBody::AddArea(std::vector<int> indices, float stiffness) {
    if (indices.size() < 3) return;
    for (int i : indices) {
        if (i < 0 || i >= (int)points.size()) return;
    }

    float area2 = 0.0f;
    const Vector2 origin = points[indices[0]].pos;
    for (size_t i = 0; i < indices.size(); ++i) {
        area2 += Cross(points[indices[i]].pos - origin,
                       points[indices[(i + 1) % indices.size()]].pos - origin);
    }

    SoftArea area;
    area.points = std::move(indices);
    area.rest = area2 * 0.5f;
    area.stiffness = Clamp(stiffness, 0.0f, 1.0f);
    if (std::fabs(area.rest) < 1e-3f) return;
    area.gradients.resize(area.points.size());
    areas.push_back(std::move(area));
}

void SoftBody::AddMemory(const std::vector<int>& indices, float stiffness) {
    if (indices.size() < 2) return;

    SoftMemory memory;
    memory.points = indices;
    memory.rest.reserve(indices.size());
    for (int i : indices) {
        if (i < 0 || i >= (int)points.size()) return;
        memory.rest.push_back(points[i].pos);
    }
    memory.stiffness = Clamp(stiffness, 0.0f, 1.0f);
    memories.push_back(std::move(memory));
}

// =========================================================
// QUERIES
// =========================================================

Vector2 SoftBody::Center() const {
    return CenterOfRange(0, (int)points.size());
}

Vector2 SoftBody::CenterOfRange(int begin, int count) const {
    Vector2 center{};
    if (count <= 0) return center;
    int used = 0;
    for (int i = begin; i < begin + count && i < (int)points.size(); ++i) {
        if (i < 0) continue;
        center += points[i].pos;
        ++used;
    }
    if (used == 0) return center;
    return center / (float)used;
}

Vector2 SoftBody::AverageVelocity() const {
    Vector2 average{};
    if (points.empty()) return average;
    for (const auto& p : points) {
        if (p.invMass > 0.0f) average += p.vel;
    }
    return average / (float)points.size();
}

float SoftBody::AverageSpeed() const {
    if (points.empty()) return 0.0f;
    float total = 0.0f;
    for (const auto& p : points) total += Vector2::Length(p.vel);
    return total / (float)points.size();
}

void SoftBody::ResetVelocities() {
    for (auto& p : points) p.vel = {0.0f, 0.0f};
}

void SoftBody::Translate(Vector2 delta) {
    for (auto& p : points) {
        p.pos += delta;
        p.stepStart += delta;
    }
    attachTarget += delta;
    previousAttachTarget += delta;
}

bool SoftBody::IsFinite() const {
    for (const auto& p : points) {
        if (!std::isfinite(p.pos.x) || !std::isfinite(p.pos.y) ||
            !std::isfinite(p.vel.x) || !std::isfinite(p.vel.y)) {
            return false;
        }
    }
    return true;
}

// =========================================================
// CONSTRAINTS
// =========================================================

void SoftBody::SolveAttach(float dt, Vector2 target) {
    if (attachedPoint < 0 || attachedPoint >= (int)points.size()) return;
    SoftPoint& p = points[attachedPoint];
    if (p.invMass <= 0.0f || attachStiffness <= 0.0f) return;
    const float alpha = Compliance(attachStiffness) / (dt * dt);
    const Vector2 impulse = (target - p.pos - attachLambda * alpha) /
                            (p.invMass + alpha);
    attachLambda += impulse;
    p.Correct(impulse * p.invMass);
}

void SoftBody::SolveSprings(float dt) {
    for (auto& s : springs) {
        if (s.stiffness <= 0.0f) continue;
        SoftPoint& a = points[s.a];
        SoftPoint& b = points[s.b];
        const Vector2 delta = b.pos - a.pos;
        const float dist = Vector2::Length(delta);
        const float weightSum = a.invMass + b.invMass;
        if (dist < 1e-4f || weightSum <= 0.0f) continue;
        const float alpha = Compliance(s.stiffness) / (dt * dt);
        const float impulse = (s.rest - dist - alpha * s.lambda) /
                              (weightSum + alpha);
        s.lambda += impulse;
        const Vector2 correction = delta * (impulse / dist);
        a.Correct(correction * -a.invMass);
        b.Correct(correction * b.invMass);
    }
}

void SoftBody::SolveAreas(float dt) {
    for (auto& area : areas) {
        const int n = (int)area.points.size();
        if (n < 3 || area.stiffness <= 0.0f) continue;
        float area2 = 0.0f;
        float weightSum = 0.0f;
        const Vector2 origin = points[area.points[0]].pos;
        for (int i = 0; i < n; ++i) {
            const Vector2 a = points[area.points[i]].pos - origin;
            const Vector2 b = points[area.points[(i + 1) % n]].pos - origin;
            const Vector2 prev = points[area.points[(i - 1 + n) % n]].pos;
            const Vector2 tangent = points[area.points[(i + 1) % n]].pos - prev;
            area2 += Cross(a, b);
            const Vector2 gradient{tangent.y * 0.5f, -tangent.x * 0.5f};
            area.gradients[i] = gradient;
            weightSum += points[area.points[i]].invMass * Dot(gradient, gradient);
        }
        if (weightSum < 1e-6f) continue;
        const float alpha = Compliance(area.stiffness) * std::fabs(area.rest) /
                            (dt * dt);
        const float impulse = (area.rest - area2 * 0.5f - alpha * area.lambda) /
                              (weightSum + alpha);
        area.lambda += impulse;
        // Simultaneous, mass-weighted pressure forces preserve center of mass.
        // Signed area resists inversion instead of inflating an inside-out ring.
        for (int i = 0; i < n; ++i) {
            SoftPoint& p = points[area.points[i]];
            p.Correct(area.gradients[i] * (impulse * p.invMass));
        }
    }
}

void SoftBody::SolveMemories() {
    for (auto& memory : memories) {
        const int n = (int)memory.points.size();
        if (n < 2 || (int)memory.rest.size() != n) continue;

        // Best-fit rigid transform (2D Kabsch) from the captured rest pose to
        // the current pose. Letting the fit rotate freely means a tumbling
        // goose is not fighting its own shape memory.
        const Vector2 restOrigin = memory.rest[0];
        const Vector2 currentOrigin = points[memory.points[0]].pos;
        // Double-precision fit prevents a tiny centroid residual from becoming
        // a net force when velocity recovery divides it by the substep time.
        double restX = 0.0, restY = 0.0, currentX = 0.0, currentY = 0.0;
        double massSum = 0.0;
        for (int i = 0; i < n; ++i) {
            const SoftPoint& p = points[memory.points[i]];
            // Pins are handled by structural constraints, not a free rigid fit.
            if (p.invMass <= 0.0f) { massSum = 0.0; break; }
            const double mass = 1.0 / p.invMass;
            restX += (double(memory.rest[i].x) - restOrigin.x) * mass;
            restY += (double(memory.rest[i].y) - restOrigin.y) * mass;
            currentX += (double(p.pos.x) - currentOrigin.x) * mass;
            currentY += (double(p.pos.y) - currentOrigin.y) * mass;
            massSum += mass;
        }
        if (massSum <= 0.0) continue;
        restX /= massSum; restY /= massSum;
        currentX /= massSum; currentY /= massSum;

        double dot = 0.0, cross = 0.0;
        for (int i = 0; i < n; ++i) {
            const SoftPoint& p = points[memory.points[i]];
            const double qx = double(memory.rest[i].x) - restOrigin.x - restX;
            const double qy = double(memory.rest[i].y) - restOrigin.y - restY;
            const double cx = double(p.pos.x) - currentOrigin.x - currentX;
            const double cy = double(p.pos.y) - currentOrigin.y - currentY;
            dot += (qx * cx + qy * cy) / p.invMass;
            cross += (qx * cy - qy * cx) / p.invMass;
        }
        const double norm = std::hypot(dot, cross);
        const double cosA = norm > 1e-9 ? dot / norm : 1.0;
        const double sinA = norm > 1e-9 ? cross / norm : 0.0;
        for (int i = 0; i < n; ++i) {
            SoftPoint& p = points[memory.points[i]];
            const double qx = double(memory.rest[i].x) - restOrigin.x - restX;
            const double qy = double(memory.rest[i].y) - restOrigin.y - restY;
            p.Correct({
                float((currentX + qx * cosA - qy * sinA -
                       (double(p.pos.x) - currentOrigin.x)) * memory.solverStiffness),
                float((currentY + qx * sinA + qy * cosA -
                       (double(p.pos.y) - currentOrigin.y)) * memory.solverStiffness)});
        }
    }
}

void SoftBody::ApplyBounds() {
    for (auto& p : points) {
        if (p.invMass <= 0.0f) continue;
        if (p.pos.x <= boundMinX) {
            p.pos.x = boundMinX;
            p.contactX = -1;
        } else if (p.pos.x >= boundMaxX) {
            p.pos.x = boundMaxX;
            p.contactX = 1;
        }
        if (p.pos.y <= boundMinY) {
            p.pos.y = boundMinY;
            p.contactY = -1;
        } else if (p.pos.y >= boundMaxY) {
            p.pos.y = boundMaxY;
            p.contactY = 1;
        }
    }
}

void SoftBody::RecoverVelocities(float dt) {
    for (auto& p : points) {
        if (p.invMass <= 0.0f) {
            p.vel = {0.0f, 0.0f};
            continue;
        }
        // Differencing rounded screen coordinates injects momentum every
        // frame. Recover from the actual constraint impulses instead.
        Vector2 v = p.vel + p.correction / dt;
        auto contact = [&](float normal, float incoming, float& perpendicular,
                           float& tangent) {
            const float closing = incoming * normal;
            // Do not reflect velocity that already points away from the wall.
            const float bounce = closing > 20.0f
                ? Clamp(restitution, 0.0f, 1.0f) : 0.0f;
            const float impulse = std::max(0.0f, closing) * (1.0f + bounce);
            perpendicular = std::min(perpendicular * normal,
                                      -std::max(0.0f, closing) * bounce) * normal;
            const float frictionImpulse = Clamp(friction, 0.0f, 1.0f) * impulse;
            tangent = std::copysign(std::max(0.0f, std::fabs(tangent) -
                                            frictionImpulse), tangent);
        };
        if (p.contactX && (p.pos.x <= boundMinX || p.pos.x >= boundMaxX))
            contact((float)p.contactX, p.vel.x, v.x, v.y);
        if (p.contactY && (p.pos.y <= boundMinY || p.pos.y >= boundMaxY))
            contact((float)p.contactY, p.vel.y, v.y, v.x);
        const float speed = Vector2::Length(v);
        if (speed > maxSpeed) v = v * (maxSpeed / speed);
        p.vel = v;
    }
}

void SoftBody::DampSprings(float dt) {
    // Dissipate only relative stretch velocity. Translation and rigid rotation
    // survive; tissue jiggle dies away without making a throw feel underwater.
    const float damping = 1.0f - std::exp(-6.0f * dt);
    for (const auto& spring : springs) {
        if (spring.stiffness <= 0.0f) continue;
        SoftPoint& a = points[spring.a];
        SoftPoint& b = points[spring.b];
        const Vector2 delta = b.pos - a.pos;
        const float length = Vector2::Length(delta);
        const float weights = a.invMass + b.invMass;
        if (length < 1e-4f || weights <= 0.0f) continue;
        const Vector2 normal = delta / length;
        const Vector2 impulse = normal *
            (Dot(b.vel - a.vel, normal) * damping / weights);
        a.vel += impulse * a.invMass;
        b.vel = b.vel - impulse * b.invMass;
    }
}

// =========================================================
// STEP
// =========================================================

void SoftBody::LimitStrain() {
    bool limited = false;
    // Project upper length limits, not rigid rest lengths: ordinary tissue
    // remains compliant, but connective links cannot stretch without bound.
    for (int pass = 0; pass < 12; ++pass) {
        float largestError = 0.0f;
        for (const auto& spring : springs) {
            if (spring.maxStretch <= 0.0f) continue;
            SoftPoint& a = points[spring.a];
            SoftPoint& b = points[spring.b];
            const Vector2 delta = b.pos - a.pos;
            const float length = Vector2::Length(delta);
            const float limit = spring.rest * spring.maxStretch;
            const float weights = a.invMass + b.invMass;
            if (length <= limit || weights <= 0.0f) continue;
            limited = true;
            largestError = std::max(largestError, (length - limit) / limit);
            const Vector2 correction = delta * ((length - limit) / (length * weights));
            a.Correct(correction * a.invMass);
            b.Correct(correction * -b.invMass);
        }
        if (collideBounds) ApplyBounds();
        if (largestError < 0.001f) break;
    }
    if (!limited) return;

    // Residual stretch must not cancel translation or rotation. Contract the
    // solved pose about its mass center, rather than rewinding its motion.
    // This also stays inside the convex screen bounds already applied above.
    double scaleSquared = 1.0;
    for (const auto& spring : springs) {
        if (spring.maxStretch <= 0.0f) continue;
        const Vector2 delta = points[spring.b].pos - points[spring.a].pos;
        const double lengthSquared = double(delta.x) * delta.x + double(delta.y) * delta.y;
        const double limit = double(spring.rest) * spring.maxStretch;
        if (lengthSquared > limit * limit)
            scaleSquared = std::min(scaleSquared, limit * limit / lengthSquared);
    }
    if (scaleSquared >= 1.0) return;

    double mass = 0.0, centerX = 0.0, centerY = 0.0;
    bool pinned = false;
    for (const auto& p : points) {
        if (p.invMass <= 0.0f) { pinned = true; break; }
        const double weight = 1.0 / p.invMass;
        mass += weight;
        centerX += double(p.pos.x) * weight;
        centerY += double(p.pos.y) * weight;
    }
    if (!pinned && mass > 0.0) {
        centerX /= mass;
        centerY /= mass;
        const double contraction = 1.0 - std::sqrt(scaleSquared) * (1.0 - 1e-6);
        for (auto& p : points) {
            p.Correct({float((centerX - p.pos.x) * contraction),
                       float((centerY - p.pos.y) * contraction)});
        }
        return;
    }

    // Fixed world anchors cannot translate or contract with the body. For
    // pinned systems, clip only the remaining motion from the feasible pose;
    // convex distance and screen bounds keep every fixed connection valid.
    double fraction = 1.0;
    for (const auto& spring : springs) {
        if (spring.maxStretch <= 0.0f) continue;
        const SoftPoint& a = points[spring.a];
        const SoftPoint& b = points[spring.b];
        const Vector2 old = b.stepStart - a.stepStart;
        const Vector2 next = b.pos - a.pos;
        const double limit = double(spring.rest) * spring.maxStretch;
        if (double(next.x) * next.x + double(next.y) * next.y <= limit * limit) continue;
        const Vector2 delta = next - old;
        const double qa = double(delta.x) * delta.x + double(delta.y) * delta.y;
        if (qa < 1e-12) continue;
        const double qb = 2.0 * (double(old.x) * delta.x + double(old.y) * delta.y);
        const double qc = std::min(0.0, double(old.x) * old.x +
                                        double(old.y) * old.y - limit * limit);
        const double root = (-qb + std::sqrt(std::max(0.0, qb * qb - 4.0 * qa * qc))) /
                            (2.0 * qa);
        fraction = std::min(fraction, std::clamp(root, 0.0, 1.0));
    }
    if (fraction < 1.0) {
        for (auto& p : points) {
            if (p.invMass > 0.0f)
                p.Correct((p.stepStart - p.pos) * float(1.0 - fraction));
        }
    }
}

void SoftBody::Step(float dt) {
    if (points.empty()) return;
    if (!(dt > 0.0f)) return;

    const float clamped = std::min(dt, 0.05f);
    const int subs = std::clamp(substeps, 1, 8);
    const float h = clamped / (float)subs;
    const int iters = std::clamp(iterations, 1, 20);
    const float damping = std::exp(-std::max(0.0f, linearDamping) * h);
    const float gravityTime = linearDamping > 1e-6f
        ? -std::expm1(-linearDamping * h) / linearDamping : h;
    const bool attached = attachedPoint >= 0 && attachedPoint < (int)points.size();
    const Vector2 startTarget = attached
        ? (previousAttachedPoint == attachedPoint ? previousAttachTarget
                                                 : points[attachedPoint].pos)
        : attachTarget;
    for (auto& memory : memories) {
        const float k = Clamp(memory.stiffness, 0.0f, 1.0f);
        const float effective = k <= 0.0f ? 0.0f :
            h * h / (h * h + Compliance(k));
        memory.solverStiffness = 1.0f -
            std::pow(1.0f - effective, 1.0f / (float)iters);
    }

    for (int s = 0; s < subs; ++s) {
        for (auto& spring : springs) spring.lambda = 0.0f;
        for (auto& area : areas) area.lambda = 0.0f;
        attachLambda = {};
        const Vector2 target = Vector2::Lerp(startTarget, attachTarget,
                                             (float)(s + 1) / (float)subs);
        // Integrate exact linear drag; keep small constraint displacements
        // separately so subpixel rounding cannot feed back into velocity.
        for (auto& p : points) {
            p.stepStart = p.pos;
            p.correction = {};
            p.contactX = p.contactY = 0;
            if (p.invMass <= 0.0f) continue;
            p.vel = p.vel * damping + gravity * (gravityScale * gravityTime);
            const float speed = Vector2::Length(p.vel);
            if (speed > maxSpeed) p.vel = p.vel * (maxSpeed / speed);
            p.pos += p.vel * h;
        }

        // Couple tissue, hold and contact in the same solve: the cursor must
        // not fight a body that is only clamped after all its constraints.
        for (int i = 0; i < iters; ++i) {
            SolveMemories();
            SolveSprings(h);
            SolveAreas(h);
            SolveAttach(h, target);
            if (collideBounds) ApplyBounds();
        }
        LimitStrain();
        RecoverVelocities(h);
        DampSprings(h);
    }
    previousAttachTarget = attachTarget;
    previousAttachedPoint = attached ? attachedPoint : -1;

    if (!IsFinite()) {
        const Vector2 center{ (boundMinX + boundMaxX) * 0.5f,
                              (boundMinY + boundMaxY) * 0.5f };
        for (auto& p : points) {
            p.pos = center;
            p.correction = {};
            p.vel = {0.0f, 0.0f};
        }
    }
}
