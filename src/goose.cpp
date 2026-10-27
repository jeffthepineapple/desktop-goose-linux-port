#include "goose.h"
#include "goose_traits.h"
#include "config.h"     // g_config
#include "assets.h"     // g_assets
#include "world.h"      // g_droppedItems
#include "cursor_backend.h" // g_backendManager
#include "hyprland.h"        // HyprlandBackend (window drag)
#include "niri.h"            // NiriBackend (window drag)
#include "edge_detector.h"
#include "window_capture.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <cstdint>
#include <stdexcept>
#include <unistd.h>

// =========================================================
// CONSTANTS
// =========================================================

static const float OUTLINE_GRAY[] = { 0.82f, 0.82f, 0.82f };
static const float ORANGE[]       = { 1.0f, 0.64f, 0.0f };
// Beak tuning
static const float BEAK_LEN = 12.0f; // much larger beak
static const float BEAK_WID = 9.0f;

// ✅ NEW: Desktop Goose-ish face anchoring (keeps beak/eyes attached to head)
static const float BEAK_BASE_OFFSET = 4.0f;   // from neckHead forward
static const float HEAD1_OFFSET     = 2.0f;   // short round head
static const float HEAD2_OFFSET     = 4.0f;

// ✅ NEW: honk timing
static const double HONK_IDLE_MIN   = 6.0;
static const double HONK_IDLE_MAX   = 14.0;
static const double HONK_MIN_GAP    = 0.60;   // global anti-spam gap
static const double HONK_CHASE_CD   = 1.80;
static const double HONK_FETCH_CD   = 1.20;
static const double HONK_GENERIC_CD = 0.90;
static const double kGooseYeetSpinTime = 0.8; // hammer-throw spin before release


// ✅ NEW: Predicted cursor for backends that can't read global position (or for Tier 3 drag logic)
// We initialize to center for lack of better info.
static Vector2 s_predictedCursor = { -1.0f, -1.0f };

// tiny helper
static inline double Rand01() { return (double)(rand() % 1000) / 1000.0; }
static Vector2 PickGooseYeetLanding(Vector2 from, int w, int h);
static Vector2 GetSnatchForward(float dirDegrees, const Vector2& isoScale) {
    Vector2 rawFwd = Vector2::FromAngleDegrees(dirDegrees);
    Vector2 fwd{ rawFwd.x * isoScale.x, rawFwd.y * isoScale.y };
    if (Vector2::Length(fwd) < 1e-4f) return { 1.0f, 0.0f };
    return Vector2::Normalize(fwd);
}

Vector2 Goose::GetPredictedCursor() {
    return s_predictedCursor;
}

// =========================================================
// CONSTRUCTOR
// =========================================================

Goose::Goose(int _id, const std::string& _name, int screenW, int screenH) : id(_id), name(_name) {
    pos.x = rand() % (screenW - 100) + 50;
    pos.y = rand() % (screenH - 100) + 50;

    const GooseTraits traits = GooseTraits_Next();
    traitSeed = traits.seed;
    attackMouseBias = traits.attackMouseBias;
    memeFetchBias = traits.memeFetchBias;
    noteFetchBias = traits.noteFetchBias;

    // Initialize from global defaults
    cursorChaseEnabled = g_config.cursorChaseEnabled;
    cursorChaseChance  = g_config.cursorChaseChance;
    snatchDuration     = g_config.snatchDuration;
    mudEnabled         = g_config.mudEnabled;
    mudChance          = g_config.mudChance;
    mudLifetime        = g_config.mudLifetime;
    windowDragEnabled  = g_config.windowDragEnabled;
    windowDragChance   = g_config.windowDragChance;
    windowYeetChance   = g_config.windowYeetChance;

    PickNewTarget(screenW, screenH);
}

// =========================================================
// PHYSICS: DRAGGED ITEM SPRING
// =========================================================

void Goose::UpdateDrag(double dt) {
    if (!heldItem) return;
    // A yeeted window is a free projectile; it must not snap back to the beak.
    if (dragIsYeet && dragPhase == DRAG_YEET_FLIGHT) return;

    // Attach RIGIDLY to BEAK TIP (like original Desktop Goose)
    Vector2 beakTip = GetBeakTipWorld();

    if (!dragInit) {
        dragPos = beakTip;
        dragVel = {0,0};
        dragRot = 0.0f;
        dragRotVel = 0.0f;
        dragInit = true;
    }

    // RIGID position attachment - item is always exactly at beak tip
    Vector2 prevPos = dragPos;
    dragPos = beakTip;
    
    // Calculate velocity for rotation effect (but don't use for position)
    dragVel = (dragPos - prevPos) / (float)std::max(dt, 0.001);

    // Rotation follows movement direction with a smooth lag for visual polish
    float targetRot = 0.0f;
    float velMag = Vector2::Length(dragVel);
    if (velMag > 10.0f) {
        // Item rotates based on movement direction
        targetRot = std::atan2(dragVel.y, dragVel.x) + (float)PI / 2.0f;
    }
    
    // Smooth rotation interpolation
    float angDiff = targetRot - dragRot;
    while (angDiff > (float)PI)  angDiff -= (float)(2.0 * PI);
    while (angDiff < (float)-PI) angDiff += (float)(2.0 * PI);
    
    // Faster rotation smoothing for tighter feel
    dragRot += angDiff * std::min(1.0f, (float)dt * 12.0f);

    // Safety: if any values go non-finite, reset drag state
    if (!std::isfinite(dragPos.x) || !std::isfinite(dragPos.y) || !std::isfinite(dragRot)) {
        dragPos = beakTip;
        dragVel = {0,0};
        dragRot = 0.0f;
        dragRotVel = 0.0f;
        dragInit = false;
    }
}

// =========================================================
// PHYSICS: YEETED WINDOW PROJECTILE
// =========================================================

// Integrates the launched window screenshot in device space: gravity, air drag,
// erratic spin, and lossy bounces off the screen edges. Returns true once the
// projectile has settled and the real window should reappear where it landed.
bool Goose::UpdateYeetFlight(double dt, int w, int h) {
    if (!heldItem) return true;

    const float step = (float)std::clamp(dt, 0.0, 0.05);
    const float gravity = 2600.0f;
    const float airDrag = 0.6f;
    const float restitution = 0.45f;
    const float groundFriction = 0.7f;

    const float halfW = heldItem->w * 0.5f * g_config.globalScale;
    const float halfH = heldItem->h * 0.5f * g_config.globalScale;
    const float minX = halfW;
    const float maxX = std::max(minX, (float)w - halfW);
    const float minY = halfH;
    const float maxY = std::max(minY, (float)h - halfH);

    yeetVel.y += gravity * step;
    yeetVel = yeetVel - yeetVel * (airDrag * step);
    yeetPos = yeetPos + yeetVel * step;
    dragRot += dragRotVel * step;
    dragRotVel -= dragRotVel * (0.9f * step);

    bool onGround = false;
    if (yeetPos.x < minX || yeetPos.x > maxX) {
        yeetPos.x = std::clamp(yeetPos.x, minX, maxX);
        yeetVel.x = -yeetVel.x * restitution;
        dragRotVel = -dragRotVel * 0.8f;
        ++yeetBounces;
    }
    if (yeetPos.y < minY) {
        yeetPos.y = minY;
        yeetVel.y = -yeetVel.y * restitution;
        ++yeetBounces;
    } else if (yeetPos.y >= maxY) {
        yeetPos.y = maxY;
        onGround = true;
        if (yeetVel.y > 0.0f) {
            yeetVel.y = -yeetVel.y * restitution;
            yeetVel.x *= groundFriction;
            dragRotVel *= 0.55f;
            ++yeetBounces;
        }
    }

    if (!std::isfinite(yeetPos.x) || !std::isfinite(yeetPos.y) ||
        !std::isfinite(dragRot)) {
        yeetPos = { std::clamp(pos.x, minX, maxX), maxY };
        yeetVel = {0, 0};
        dragRot = 0.0f;
        dragRotVel = 0.0f;
        return true;
    }

    // Render position tracks the projectile; DrawHeldItem works in world space.
    dragPos = DeviceToWorld(yeetPos);

    const bool settled = onGround && Vector2::Length(yeetVel) < 90.0f;
    return settled || yeetBounces >= 6;
}

void Goose::SetWindowDestinationFromImageCenter(Vector2 centerDevice) {
    dragImageDestX = (int)std::lround(centerDevice.x);
    dragImageDestY = (int)std::lround(centerDevice.y);
    dragWindowDestX =
        (int)std::lround(centerDevice.x) - dragWindowOrigW / 2;
    dragWindowDestY =
        (int)std::lround(centerDevice.y) - dragWindowOrigH / 2;

    for (const auto& monitor : g_edgeDetector.Monitors()) {
        if (monitor.id != dragWindowMonitorId) continue;
        const int pad = g_config.windowDragEdgePadding;
        const int usableLeft = monitor.x + monitor.reservedLeft + pad;
        const int usableTop = monitor.y + monitor.reservedTop + pad;
        const int usableRight =
            monitor.x + monitor.width - monitor.reservedRight - pad;
        const int usableBottom =
            monitor.y + monitor.height - monitor.reservedBottom - pad;
        dragWindowDestX = std::clamp(
            dragWindowDestX, usableLeft,
            std::max(usableLeft, usableRight - dragWindowOrigW));
        dragWindowDestY = std::clamp(
            dragWindowDestY, usableTop,
            std::max(usableTop, usableBottom - dragWindowOrigH));
        break;
    }
}


// =========================================================
// UPDATE (AI + MOVEMENT)
// =========================================================

void Goose::Update(double dt, double time, int w, int h) {

    HonkState& hs = m_honk;
    if (!hs.initialized) {
        hs.initialized = true;
        hs.nextIdle = time + HONK_IDLE_MIN + Rand01() * (HONK_IDLE_MAX - HONK_IDLE_MIN);
    }

    auto TryHonk = [&](double cd, double& lastBucket) {
        if ((time - hs.lastAny) < HONK_MIN_GAP) return false;
        if ((time - lastBucket) < cd) return false;
        g_assets.Honk();
        lastBucket = time;
        hs.lastAny = time;
        return true;
    };

    // --- HELD: the cursor has the goose. The soft body is simulated while
    // the rest of the AI stays suspended. ---
    if (state == HELD) {
        if (!softActive) {
            // Defensive: HELD is always entered together with the soft body.
            FinishSoftBody(w, h);
            return;
        }
        // Held by a goose that no longer has us in its beak (cancelled,
        // removed, or grabbed by the cursor itself): fall free.
        if (heldByGooseId != -1) {
            const Goose* attacker = GetGooseById(heldByGooseId);
            if (!attacker || attacker->state != YEET_GOOSE ||
                attacker->yeetVictimId != id) {
                DropFromGoose();
            }
        }
        UpdateSoftBody(dt, time, w, h);
        // An indignant honk while struggling, on the normal cooldown.
        if (ragdoll.squirmDrive > 0.8f) TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
        return;
    }
    // --- FLUNG: the goose was dropped or yeeted. Gravity, drag and wall
    // bounces own it until it slows down and gets back on its feet. ---
    if (state == FLUNG) {
        if (!softActive) {
            // Defensive: FLUNG is always entered together with the soft body,
            // but if that ever desyncs the goose must not stay frozen.
            FinishSoftBody(w, h);
            return;
        }
        UpdateSoftBody(dt, time, w, h);
        return;
    }

    // --- SAFETY: Item holding consistency ---
    // If we have an item, we should be RETURNING (unless specifically forced elsewhere)
    if (heldItem != nullptr && state == WANDER) {
        state = RETURNING;
        // Assign target if none was set
        if (target.x < 0 || target.x > w || target.y < 0 || target.y > h) {
            target = { (float)(rand() % (std::max(1, w - 400)) + 200),
                       (float)(rand() % (std::max(1, h - 400)) + 200) };
        }
    }

    // ✅ NEW: idle honk schedule (Desktop Goose vibe)
    if (state == WANDER && time >= hs.nextIdle) {
        // not always; occasional is funnier
        if ((rand() % 3) == 0) {
            // treat as generic bucket
            TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
        }
        hs.nextIdle = time + HONK_IDLE_MIN + Rand01() * (HONK_IDLE_MAX - HONK_IDLE_MIN);
    }

    // --- CHASE_CURSOR: target follows cursor ---
    if (state == CHASE_CURSOR) {
        // If another goose already has the cursor, abort chase immediately.
        if (g_cursorGrabberId != -1 && g_cursorGrabberId != id) {
            state = WANDER;
            PickNewTarget(w, h);
        } else {
            Vector2 cursorPos = g_backendManager.GetCursorPos(time);
            s_predictedCursor = cursorPos;
            
            if (cursorPos.x >= 0 && cursorPos.y >= 0) {
                target = cursorPos; // Update target every frame to follow mouse
            } else {
                // If we can't read cursor, check if we have a predicted position from a recent snatch?
                // Or just abort if we lost tracking.
                // For Tier 3 (Rel only), we might blindly chase 'center' or similar?
                // For now, abort to safe state.
                state = WANDER;
                PickNewTarget(w, h);
            }
        }
    }

    // --- SNATCH_CURSOR: keep running in a small circle behind while holding cursor ---
    if (state == SNATCH_CURSOR) {

        Vector2 fwd = GetSnatchForward(dir, ISO_SCALE);
        Vector2 right{ -fwd.y, fwd.x };

        // Keep the pull point relative to the side/front the pointer came from,
        // but bias it behind the goose so it still reads as a drag.
        float lateralBias = Clamp(snatchOffset.y, -snatchPullDistance * 0.75f, snatchPullDistance * 0.75f);
        float forwardBias = Clamp(snatchOffset.x * 0.25f, -snatchPullDistance * 0.35f, snatchPullDistance * 0.15f);
        Vector2 endpoint = pos - fwd * snatchPullDistance + right * lateralBias + fwd * forwardBias;
        endpoint.x = std::clamp(endpoint.x, 0.0f, (float)std::max(0, w - 1));
        endpoint.y = std::clamp(endpoint.y, 0.0f, (float)std::max(0, h - 1));
        target = endpoint;

        if (time - snatchStartTime > snatchDuration) {
            // Restore walk timing
            stepTime = 0.2f;
            // release global cursor grab
            if (g_cursorGrabberId == id) g_cursorGrabberId = -1;
            state = WANDER;
            PickNewTarget(w, h);

            // ✅ UPDATED: treat as generic honk (don’t spam)
            TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
        }
    }

    // --- CHASE_GOOSE / YEET_GOOSE: run down another goose, then fling it ---
    if (state == CHASE_GOOSE) {
        Goose* victim = GetGooseById(yeetVictimId);
        if (!victim || !victim->CanBeGooseYeeted() || time - gooseYeetStart > 8.0) {
            ReleaseYeetVictim();
            PickNewTarget(w, h);
        } else {
            // Aim a little ahead of a walking victim instead of tail-chasing.
            const float lead = Clamp(Vector2::Distance(pos, victim->pos) /
                                         std::max(currentSpeed, 1.0f), 0.0f, 0.4f);
            target = victim->pos + victim->vel * lead;
        }
    } else if (state == YEET_GOOSE) {
        // Plant the feet; the windup only turns and coils the neck.
        target = pos;
        vel = { 0.0f, 0.0f };
        const double elapsed = time - gooseYeetStart;
        if (!gooseYeetLaunched) {
            Goose* victim = GetGooseById(yeetVictimId);
            if (!victim || victim->state != HELD || victim->heldByGooseId != id) {
                ReleaseYeetVictim();
                PickNewTarget(w, h);
            } else if (elapsed < kGooseYeetSpinTime) {
                // Hammer throw: spin faster and faster with the victim in the
                // beak, coiling the neck, then whip it out to face the aim.
                const float u = (float)(elapsed / kGooseYeetSpinTime);
                dir = std::remainder(gooseYeetSpinFrom + gooseYeetSpinTotal * u * u, 360.0f);
                yeetHeadDrive = u < 0.6f ? Lerp(0.35f, 0.0f, u / 0.6f)
                                         : Lerp(0.0f, 1.0f, (u - 0.6f) / 0.4f);
            } else {
                dir = std::remainder(gooseYeetSpinFrom + gooseYeetSpinTotal, 360.0f);
                // The spin swung the victim around; keep the throw at least
                // the minimum distance from where it actually is.
                if (Vector2::Distance(victim->pos, gooseYeetLanding) <
                    (float)std::max(0, g_config.gooseYeetMinDistance)) {
                    gooseYeetLanding = PickGooseYeetLanding(victim->pos, w, h);
                }
                victim->FlingFromGoose(gooseYeetLanding);
                gooseYeetLaunched = true;
                gooseYeetStart = time;
                yeetHeadDrive = 1.0f;
                TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
                UiLogPush("Goose " + std::to_string(id) + " yeeted goose " +
                          std::to_string(victim->id));
            }
        } else if (elapsed > 0.3) {
            // Follow-through finished.
            ReleaseYeetVictim();
            PickNewTarget(w, h);
        }
    }

    // --- DRAG_WINDOW: capture, hide, carry, and restore a real window ---
    if (state == DRAG_WINDOW) {
        if (NiriBackend* nb = dynamic_cast<NiriBackend*>(
                g_backendManager.GetActiveBackend())) {
            UpdateNiriDrag(nb, dt, time, w, h);
        } else {
        CursorBackend* rawBackend = g_backendManager.GetActiveBackend();
        HyprlandBackend* hBackend = dynamic_cast<HyprlandBackend*>(rawBackend);

        auto abortWindowDrag = [&](const std::string& reason) {
            if (!reason.empty()) {
                std::cerr << "[Goose] Window drag aborted: " << reason << '\n';
                UiLogPush("Goose " + std::to_string(id) + " window drag aborted: " + reason);
            }
            RestoreDraggedWindowNow(false);
            state = WANDER;
            PickNewTarget(w, h);
        };

        if (!hBackend || dragWindowAddr.empty()) {
            abortWindowDrag("Hyprland window is unavailable");
        } else switch (dragPhase) {

        case DRAG_APPROACH: {
            if (hBackend->GetWindowByAddress(dragWindowAddr).address.empty()) {
                abortWindowDrag("target window disappeared");
                break;
            }
            float dist = Vector2::Distance(pos, target);
            if (dist < 30.0f || (time - dragWindowStartTime) > 2.0) {
                g_suppressOverlayForCapture = true;
                for (const auto& monitor : g_monitors) {
                    if (monitor.canvas) gtk_widget_queue_draw(monitor.canvas);
                }
                dragWindowStartTime = time;
                dragCaptureReadyTime = time + 0.05;
                dragPhase = DRAG_CAPTURE_WAIT;
            }
            break;
        }

        case DRAG_CAPTURE_WAIT: {
            if (time < dragCaptureReadyTime) break;

            HyprlandBackend::Window window =
                hBackend->GetWindowByAddress(dragWindowAddr);
            if (window.address.empty()) {
                abortWindowDrag("target window disappeared before capture");
                break;
            }

            uint32_t toplevelHandle = 0;
            try {
                const unsigned long long parsed =
                    std::stoull(dragWindowAddr, nullptr, 0);
                if (parsed == 0) throw std::out_of_range("handle");
                // Version 1 of Hyprland's export protocol transports the
                // compositor pointer as a uint32_t, so its public handle is
                // the low 32 bits of the address reported by hyprctl.
                toplevelHandle = static_cast<uint32_t>(parsed);
                if (toplevelHandle == 0) throw std::out_of_range("handle");
            } catch (const std::exception&) {
                abortWindowDrag("invalid Hyprland window handle");
                break;
            }

            std::string captureError;
            GdkPixbuf* capture =
                CaptureHyprlandToplevel(toplevelHandle, &captureError);
            if (!capture) {
                abortWindowDrag(captureError.empty() ? "window capture failed"
                                                      : captureError);
                break;
            }

            std::string itemError;
            ItemData* capturedItem =
                g_assets.CreateTransientMemeItem(capture, &itemError);
            g_object_unref(capture);
            if (!capturedItem) {
                abortWindowDrag(itemError.empty() ? "captured image normalization failed"
                                                   : itemError);
                break;
            }

            heldItem = capturedItem;
            dragPhaseAttempt = 0;
            dragPhase = DRAG_PREPARE_WINDOW;
            break;
        }

        case DRAG_PREPARE_WINDOW: {
            HyprlandBackend::Window window =
                hBackend->GetWindowByAddress(dragWindowAddr);
            if (window.address.empty()) {
                abortWindowDrag("target window disappeared while preparing");
                break;
            }

            if (!dragWindowWasTiled) {
                dragPhaseAttempt = 0;
                dragPhase = DRAG_HIDE_WINDOW;
                break;
            }

            if (dragPhaseAttempt == 0) {
                hBackend->ToggleFloating(dragWindowAddr);
                dragPhaseAttempt = 1;
                break;
            }

            if (window.floating &&
                window.width == dragWindowOrigW &&
                window.height == dragWindowOrigH) {
                dragPhaseAttempt = 0;
                dragPhase = DRAG_HIDE_WINDOW;
                break;
            }
            if (window.floating) {
                hBackend->ResizeWindowToExact(
                    dragWindowAddr, dragWindowOrigW, dragWindowOrigH);
            }

            if (++dragPhaseAttempt > 15) {
                abortWindowDrag("timed out preparing tiled window");
            }
            break;
        }

        case DRAG_HIDE_WINDOW: {
            HyprlandBackend::Window window =
                hBackend->GetWindowByAddress(dragWindowAddr);
            if (window.address.empty()) {
                abortWindowDrag("target window disappeared while hiding");
                break;
            }

            if (dragPhaseAttempt > 0 &&
                window.workspaceId != dragWindowOrigWorkspaceId) {
                dragWindowWasHidden = true;
                g_suppressOverlayForCapture = false;
                for (const auto& monitor : g_monitors) {
                    if (monitor.canvas) gtk_widget_queue_draw(monitor.canvas);
                }
                dragInit = false;
                dragPos = GetBeakTipWorld();
                dragPhaseAttempt = 0;
                if (dragIsYeet) {
                    // Plant the feet: the goose throws from where it stands.
                    target = pos;
                    yeetWindupStart = time;
                    yeetBounces = 0;
                    dragPhase = DRAG_YEET_WINDUP;
                    break;
                }
                target = {
                    (float)(dragWindowDestX + dragWindowOrigW / 2),
                    (float)(dragWindowDestY + dragWindowOrigH / 2)
                };
                dragPhase = DRAG_CARRY;
                break;
            }

            hBackend->MoveWindowToWorkspace(
                dragWindowAddr, dragWindowHiddenWorkspace);
            if (++dragPhaseAttempt > 15) {
                abortWindowDrag("timed out hiding target window");
            }
            break;
        }

        case DRAG_CARRY:
            if (hBackend->GetWindowByAddress(dragWindowAddr).address.empty()) {
                abortWindowDrag("target window closed during carry");
            }
            break;

        case DRAG_YEET_WINDUP: {
            if (hBackend->GetWindowByAddress(dragWindowAddr).address.empty()) {
                abortWindowDrag("target window closed before the yeet");
                break;
            }

            // Stand still and aim across the window the goose walked up to.
            target = pos;
            Vector2 launchDir = Vector2::Normalize(Vector2{
                (float)(dragWindowOrigX + dragWindowOrigW / 2) - pos.x,
                (float)(dragWindowOrigY + dragWindowOrigH / 2) - pos.y});
            if (!std::isfinite(launchDir.x) || !std::isfinite(launchDir.y) ||
                Vector2::Length(launchDir) < 0.5f) {
                launchDir = Vector2::FromAngleDegrees(dir);
            }
            dir = std::atan2(launchDir.y, launchDir.x) * RAD_TO_DEG;

            const double windup = time - yeetWindupStart;
            const double pullBack = 0.33;
            const double snap = 0.55;
            if (windup < pullBack) {
                yeetHeadDrive = Lerp(0.35f, 0.0f, (float)(windup / pullBack));
                break;
            }
            if (windup < snap) {
                yeetHeadDrive =
                    Lerp(0.0f, 1.0f, (float)((windup - pullBack) / (snap - pullBack)));
                break;
            }

            // Headbutt connects: launch the screenshot off the beak.
            yeetPos = WorldToDevice(GetBeakTipWorld());
            const float launchSpeed = 1100.0f + (float)(rand() % 500);
            const float lift = 650.0f + (float)(rand() % 350);
            yeetVel = { launchDir.x * launchSpeed,
                        launchDir.y * launchSpeed * 0.35f - lift };
            dragRotVel = ((rand() % 2) ? 1.0f : -1.0f) *
                         (7.0f + (float)(rand() % 700) / 100.0f);
            yeetHeadDrive = 1.0f;
            yeetBounces = 0;
            dragPhaseAttempt = 0;
            dragPhase = DRAG_YEET_FLIGHT;
            TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
            UiLogPush("Goose " + std::to_string(id) + " yeeted a window");
            break;
        }

        case DRAG_YEET_FLIGHT: {
            if (hBackend->GetWindowByAddress(dragWindowAddr).address.empty()) {
                abortWindowDrag("target window closed mid-flight");
                break;
            }

            target = pos;
            if (time - yeetWindupStart > 0.75) yeetHeadDrive = -1.0f;
            if (!UpdateYeetFlight(dt, w, h)) break;

            SetWindowDestinationFromImageCenter(yeetPos);
            dragRestoreToDestination = true;
            yeetHeadDrive = -1.0f;
            dragPhaseAttempt = 0;
            dragPhase = DRAG_RESTORE_WORKSPACE;
            break;
        }

        case DRAG_RESTORE_WORKSPACE: {
            if (hBackend->GetWindowByAddress(dragWindowAddr).address.empty()) {
                abortWindowDrag("target window closed during restore");
                break;
            }

            std::string workspaceTarget =
                std::to_string(dragWindowOrigWorkspaceId);
            for (const auto& workspace : hBackend->GetWorkspaces()) {
                if (workspace.name == dragWindowOrigWorkspace) {
                    workspaceTarget = dragWindowOrigWorkspace;
                    break;
                }
            }
            hBackend->MoveWindowToWorkspace(dragWindowAddr, workspaceTarget);
            dragPhaseAttempt = 0;
            dragPhase = DRAG_RESTORE_GEOMETRY;
            break;
        }

        case DRAG_RESTORE_GEOMETRY: {
            HyprlandBackend::Window window =
                hBackend->GetWindowByAddress(dragWindowAddr);
            if (window.address.empty()) {
                abortWindowDrag("target window closed during geometry restore");
                break;
            }

            if (window.workspaceId == dragWindowOrigWorkspaceId) {
                if (window.x == dragWindowDestX &&
                    window.y == dragWindowDestY &&
                    window.width == dragWindowOrigW &&
                    window.height == dragWindowOrigH) {
                    dragPhaseAttempt = 0;
                    dragPhase = DRAG_RETILE;
                    break;
                }
                hBackend->ResizeWindowToExact(
                    dragWindowAddr, dragWindowOrigW, dragWindowOrigH);
                hBackend->MoveWindowToExact(
                    dragWindowAddr, dragWindowDestX, dragWindowDestY);
            }

            if (++dragPhaseAttempt > 15) {
                abortWindowDrag("timed out restoring window geometry");
            }
            break;
        }

        case DRAG_RETILE: {
            HyprlandBackend::Window restored =
                hBackend->GetWindowByAddress(dragWindowAddr);
            if (restored.address.empty()) {
                abortWindowDrag("target window closed before retiling");
                break;
            }

            if (dragWindowWasTiled) {
                if (dragPhaseAttempt == 0) {
                    const int imageX = dragImageDestX;
                    const int imageY = dragImageDestY;
                    double bestDistance = 1e30;
                    dragRetileTargetAddr.clear();

                    for (const auto& candidate : hBackend->GetWindows()) {
                        if (candidate.address == dragWindowAddr ||
                            candidate.floating ||
                            candidate.workspaceId != dragWindowOrigWorkspaceId ||
                            candidate.monitorId != dragWindowMonitorId) {
                            continue;
                        }

                        const int candidateRight =
                            candidate.x + candidate.width;
                        const int candidateBottom =
                            candidate.y + candidate.height;
                        const int dx = imageX < candidate.x
                            ? candidate.x - imageX
                            : imageX > candidateRight
                                ? imageX - candidateRight : 0;
                        const int dy = imageY < candidate.y
                            ? candidate.y - imageY
                            : imageY > candidateBottom
                                ? imageY - candidateBottom : 0;
                        const double distance =
                            (double)dx * dx + (double)dy * dy;
                        if (distance < bestDistance) {
                            bestDistance = distance;
                            dragRetileTargetAddr = candidate.address;
                        }
                    }

                    // Hyprland inserts a re-tiled client relative to focus.
                    // Focus this client, tile it, then swap it into the tile
                    // underneath the screenshot's final center.
                    hBackend->FocusWindow(dragWindowAddr);
                    hBackend->ToggleFloating(dragWindowAddr);
                    dragPhaseAttempt = 1;
                    break;
                }

                if (restored.floating) {
                    if (++dragPhaseAttempt > 15) {
                        abortWindowDrag("timed out retiling target window");
                    }
                    break;
                }

                if (dragPhaseAttempt < 100) {
                    if (!dragRetileTargetAddr.empty()) {
                        HyprlandBackend::Window targetWindow =
                            hBackend->GetWindowByAddress(dragRetileTargetAddr);
                        if (!targetWindow.address.empty() &&
                            !targetWindow.floating) {
                            hBackend->FocusWindow(dragWindowAddr);
                            hBackend->SendDispatch(
                                "swapwindow",
                                "address:" + dragRetileTargetAddr);
                        }
                    }
                    dragPhaseAttempt = 100;
                    break;
                }
            }

            std::string focusAddress = dragWindowWasFocused
                ? dragWindowAddr
                : dragOriginalFocusAddr;
            if (!focusAddress.empty() &&
                !hBackend->GetWindowByAddress(focusAddress).address.empty()) {
                hBackend->FocusWindow(focusAddress);
            }
            const bool wasYeet = dragIsYeet;
            ResetWindowDragState();
            state = WANDER;
            PickNewTarget(w, h);
            TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
            UiLogPush("Goose " + std::to_string(id) +
                      (wasYeet ? " finished yeeting a window"
                               : " finished dragging a window"));
            break;
        }
        }
        }
    }

    // --- Normal state machine ---
    Vector2 btPoint = WorldToDevice(GetBeakTipWorld()); // Calculate once per frame
    bool reached = false;
    bool caughtGoose = false; // separate: a WANDER decision may enter CHASE_GOOSE below

    if (state == WANDER) {
        reached = (Vector2::Distance(pos, target) < 20.0f);
    } else if (state == CHASE_CURSOR) {
        // Special check: did we catch the mouse?
        float catchThreshold = std::max(22.0f * g_config.globalScale, 15.0f);
        if (Vector2::Distance(btPoint, target) < catchThreshold) {
            if (g_cursorGrabberId == -1) {
                g_cursorGrabberId = id;
                state = SNATCH_CURSOR;
                snatchStartTime = time;
                {
                    Vector2 catchFwd = GetSnatchForward(dir, ISO_SCALE);
                    Vector2 catchRight{ -catchFwd.y, catchFwd.x };
                    Vector2 cursorDelta = target - pos;
                    snatchOffset.x = Clamp(Dot(cursorDelta, catchFwd), -120.0f, 120.0f);
                    snatchOffset.y = Clamp(Dot(cursorDelta, catchRight), -120.0f, 120.0f);
                }
                snatchAngle = 0.0f;
                snatchRadius = 40.0f + (rand() % 80);
                snatchAngularSpeed = ((rand() % 2) ? 1.0f : -1.0f) * (1.5f + (rand() % 200) / 100.0f);
                currentSpeed = g_config.baseRunSpeed * 1.25f;
                stepTime = 0.12f;
                TryHonk(HONK_CHASE_CD, hs.lastChase);
            } else {
                state = WANDER;
                PickNewTarget(w, h);
            }
        }
    } else if (state == FETCHING) {
        // The pickup point is intentionally off-screen. Body proximity is more
        // reliable here than beak proximity because edge clamping can keep the
        // animated beak from ever touching the hidden target exactly.
        float beakThreshold = std::max(30.0f * g_config.globalScale, 25.0f);
        bool reachedPickupEdge = (target.x < 0.0f && pos.x <= 10.0f) ||
                                 (target.x > (float)w && pos.x >= (float)w - 10.0f) ||
                                 (target.y < 0.0f && pos.y <= 10.0f) ||
                                 (target.y > (float)h && pos.y >= (float)h - 10.0f);
        reached = Vector2::Distance(btPoint, target) < beakThreshold ||
                  Vector2::Distance(pos, target) < 90.0f ||
                  reachedPickupEdge;
    } else if (state == RETURNING ||
               (state == DRAG_WINDOW && dragPhase == DRAG_CARRY)) {
        // Carried items use visual contact and a larger drop threshold.
        float threshold = std::max(60.0f * g_config.globalScale, 50.0f);
        reached = (Vector2::Distance(btPoint, target) < threshold);
    } else if (state == CHASE_GOOSE) {
        // Catch when the beak touches the victim's body, or the two bodies
        // meet (the victim shuffles away from close geese).
        if (const Goose* victim = GetGooseById(yeetVictimId)) {
            const float scale = g_config.globalScale;
            // A standing goose's torso sits 14 world px above its feet
            // (UpdateRig); derived from pos so it never reads a stale rig.
            const Vector2 victimBody = victim->pos + Vector2{ 0.0f, -14.0f * scale };
            caughtGoose = Vector2::Distance(btPoint, victimBody) < std::max(34.0f * scale, 26.0f) ||
                          Vector2::Distance(pos, victim->pos) < 45.0f * scale + 10.0f;
        }
    } else {
        reached = false;
    }

    if (reached) {
        if (state == WANDER) {
            bool chased = false;

            // Only allow new cursor chases when nobody is currently snatching.
            if (g_cursorGrabberId == -1 && cursorChaseEnabled && (g_backendManager.GetActiveBackend()->Caps() & CAP_GET_POS)) {
                int totalChance = cursorChaseChance + attackMouseBias;
                if (totalChance < 0) totalChance = 0;
                if (totalChance > 100) totalChance = 100;
                if ((rand() % 100) < totalChance) {
                    state = CHASE_CURSOR;
                    Vector2 cursorPos = g_backendManager.GetCursorPos(time);
                    if (cursorPos.x >= 0 && cursorPos.y >= 0) target = cursorPos; // Stay in Device space

                    // ✅ UPDATED: chase honk cooldown
                    TryHonk(HONK_CHASE_CD, hs.lastChase);

                    chased = true;
                }
            }

            // Window interaction chance (Hyprland/niri, only if not chasing).
            // Both entry points own candidate refresh, filtering, and reservation.
            if (!chased && windowDragEnabled && g_config.windowDragEnabled &&
                (rand() % 100) < windowDragChance) {
                chased = ((rand() % 100) < windowYeetChance)
                             ? ForceWindowYeet(w, h)
                             : ForceWindowDrag(w, h);
            }

            // Goose-on-goose yeet: pick on the nearest available goose.
            if (!chased && g_config.gooseYeetEnabled &&
                (rand() % 100) < g_config.gooseYeetChance) {
                chased = ForceGooseYeet(-1, w, h);
            }

            if (!chased) {
                // Fetch chance influenced by per-goose meme/note biases
                int memeProb = g_config.memesEnabled ? memeFetchBias : 0;
                int noteProb = noteFetchBias;
                int trigger = 5 + memeProb + noteProb; // base + biases
                if (trigger > 100) trigger = 100;

                // Only allow fetching if not too many geese are already fetching
                int fetchCount = 0;
                for (auto& other : g_geese) if (other.state == FETCHING) fetchCount++;

                if (fetchCount < 3 && (rand() % 100) < trigger) {
                    int total = memeProb + noteProb;
                    if (total <= 0) {
                        if (rand() % 2 == 0) ForceFetch(0, w, h);
                        else ForceFetch(1, w, h);
                    } else {
                        int pick = rand() % total;
                        if (pick < memeProb) ForceFetch(0, w, h);
                        else ForceFetch(1, w, h);
                    }
                } else {
                    PickNewTarget(w, h);

                    // --- Heist AI: target existing on-screen items ---
                    if (g_config.memesEnabled && (rand() % 100) < 20 && !g_droppedItems.empty()) {
                        auto it = g_droppedItems.begin();
                        std::advance(it, rand() % g_droppedItems.size());
                        // Target an approach point just outside the item's edge so the
                        // goose doesn't attempt to drive its body into the item's hitbox.
                        Vector2 centerDevice = it->pos + Vector2{ (float)it->data->w * 0.5f * g_config.globalScale,
                                                                 (float)it->data->h * 0.5f * g_config.globalScale };
                        // vector from goose screen position to item center
                        Vector2 gooseScreen = WorldToDevice(pos);
                        Vector2 toCenter = centerDevice - gooseScreen;
                        float len = Vector2::Length(toCenter);
                        Vector2 dir = (len < 1e-4f) ? Vector2{0.0f, -1.0f} : Vector2{ toCenter.x / len, toCenter.y / len };
                        float halfDim = std::max(it->data->w, it->data->h) * 0.5f * g_config.globalScale;
                        float margin = 8.0f * g_config.globalScale;
                        Vector2 approachDevice = centerDevice - dir * (halfDim + margin);
                        target = approachDevice; // Stay in Device space
                    }

                    // ✅ UPDATED: keep your random honk, but gate it
                    if (rand() % 15 == 0) {
                        TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
                    }
                }
            }
        }
        else if (state == FETCHING) {
            // Safety: delete old item if any (leaks otherwise)
            if (heldItem) {
                delete heldItem;
                heldItem = nullptr;
            }

            if (!forcedText.empty()) {
                heldItem = g_assets.CreateTextItem(forcedText);
            } else if (!forcedMemePath.empty()) {
                heldItem = g_assets.CreateMemeItem(forcedMemePath);
            } else {
                if (forceItemFetch == 0) {
                    heldItem = g_assets.GetRandomMeme();
                } else if (forceItemFetch == 1) {
                    heldItem = g_assets.GetRandomText();
                } else if (forceItemFetch == 2) {
                    heldItem = g_assets.GetFortune();
                    if (!heldItem) heldItem = g_assets.GetRandomText();
                } else {
                    heldItem = (rand() % 2 == 0) ? g_assets.GetRandomMeme() : g_assets.GetRandomText();
                }
            }

            forceItemFetch = -1;
            forcedText.clear();
            forcedMemePath.clear();

            if (heldItem) {
                state = RETURNING;
                // Place items anywhere on screen with a large margin
                target = { (float)(rand() % (std::max(1, w - 400)) + 200),
                           (float)(rand() % (std::max(1, h - 400)) + 200) };

                // ✅ UPDATED: fetch honk cooldown
                TryHonk(HONK_FETCH_CD, hs.lastFetch);
            } else {
                // Failed to get an item (no files?), just wander
                state = WANDER;
                PickNewTarget(w, h);
            }
        }
        else if (state == DRAG_WINDOW && dragPhase == DRAG_CARRY) {
            if (heldItem) {
                const float halfHeight =
                    heldItem->h * 0.5f * g_config.globalScale;
                const Vector2 imageCenter = btPoint + Vector2{
                    -std::sin(dragRot) * halfHeight,
                     std::cos(dragRot) * halfHeight
                };
                SetWindowDestinationFromImageCenter(imageCenter);
            }
            dragRestoreToDestination = true;
            dragPhaseAttempt = 0;
            dragPhase = DRAG_RESTORE_WORKSPACE;
        }
        else if (state == RETURNING) {
            if (heldItem) {
                DroppedItem drop;
                drop.data = heldItem;
                
                // Use the current beak tip position for dropping
                drop.pos = btPoint - Vector2{ (float)heldItem->w * 0.5f * g_config.globalScale, 
                                             (float)heldItem->h * 0.5f * g_config.globalScale };
                drop.rotation = dragRot;
                drop.timeDropped = time;

                if (std::isfinite(drop.pos.x) && std::isfinite(drop.pos.y) && std::isfinite(drop.rotation)) {
                    // Clamp drop to visible bounds. If multi-monitor support is
                    // disabled, constrain to primary overlay size `g_screenWidth`/`g_screenHeight`.
                    float minX = 0.0f, minY = 0.0f;
                    float maxX = (float)g_screenWidth - heldItem->w * g_config.globalScale;
                    float maxY = (float)g_screenHeight - heldItem->h * g_config.globalScale;

                    if (!g_config.multiMonitorEnabled) {
                        // Prefer primary monitor bounds (fallback to screen globals)
                        // (If monitor querying is added later, replace these.)
                    }

                    if (drop.pos.x < minX) drop.pos.x = minX;
                    if (drop.pos.y < minY) drop.pos.y = minY;
                    if (drop.pos.x > maxX) drop.pos.x = maxX;
                    if (drop.pos.y > maxY) drop.pos.y = maxY;

                    g_droppedItems.push_back(drop);
                    UiLogPush("Goose " + std::to_string(id) + " dropped item at (" + std::to_string((int)drop.pos.x) + "," + std::to_string((int)drop.pos.y) + ")");
                } else {
                    std::cerr << "[Goose] ERROR: Dropped item position invalid for Goose " << id << "! Discarding item." << std::endl;
                    delete heldItem;
                }

                heldItem = nullptr;
                dragInit = false;

                // ✅ UPDATED: fetch honk cooldown
                TryHonk(HONK_FETCH_CD, hs.lastFetch);
            }
            state = WANDER;
            PickNewTarget(w, h);
            UiLogPush("Goose " + std::to_string(id) + " returning to WANDER");
        }
    }

    if (caughtGoose && state == CHASE_GOOSE) {
        Goose* victim = GetGooseById(yeetVictimId);
        if (victim && victim->GrabByGoose(id, btPoint)) {
            state = YEET_GOOSE;
            gooseYeetStart = time;
            gooseYeetLaunched = false;
            gooseYeetLanding = PickGooseYeetLanding(victim->pos, w, h);
            // Spin one full extra turn in a random direction and end facing
            // the landing spot.
            const Vector2 aim = gooseYeetLanding - pos;
            const float toAim = std::remainder(std::atan2(aim.y, aim.x) * RAD_TO_DEG - dir, 360.0f);
            const bool clockwise = rand() % 2;
            const float offset = clockwise ? (toAim < 0.0f ? toAim + 360.0f : toAim)
                                           : (toAim > 0.0f ? toAim - 360.0f : toAim);
            gooseYeetSpinFrom = dir;
            gooseYeetSpinTotal = (clockwise ? 360.0f : -360.0f) + offset;
            target = pos;
            vel = { 0.0f, 0.0f };
            TryHonk(HONK_CHASE_CD, hs.lastChase);
            UiLogPush("Goose " + std::to_string(id) + " grabbed goose " +
                      std::to_string(victim->id));
        } else {
            ReleaseYeetVictim();
            PickNewTarget(w, h);
        }
    }

    // --- Passive Grab (Contact Snatching) ---
    // If the goose's beak touches any dropped item while wandering or fetching, it snatches it.
    if (heldItem == nullptr && (state == WANDER || state == FETCHING)) {
        Vector2 btPoint = WorldToDevice(GetBeakTipWorld());
        for (auto it = g_droppedItems.begin(); it != g_droppedItems.end(); ++it) {
            // Find the visual center of the dropped item
            Vector2 itemCenter = it->pos + Vector2{ (float)it->data->w * 0.5f, (float)it->data->h * 0.5f } * g_config.globalScale;
            
            // Interaction radius scales with goose size (28px base)
            // ✅ ADDED: Only snatch if item has been on the ground for > 2.0 seconds
            // This prevents the goose from instantly re-snatching an item it just dropped.
            if (Vector2::Distance(btPoint, itemCenter) < 28.0f * g_config.globalScale) {
                if ((time - it->timeDropped) > 2.0) {
                    heldItem = it->data;
                    g_droppedItems.erase(it);
                    
                    state = RETURNING;
                    // Pick a new place to bring the stolen loot
                    target = { (float)(rand() % (std::max(1, w - 300)) + 150),
                               (float)(rand() % (std::max(1, h - 300)) + 150) };
                    
                    TryHonk(HONK_FETCH_CD, hs.lastFetch);
                    UiLogPush("Goose " + std::to_string(id) + " snatched a dropped item!");
                    break; 
                }
            }
        }
    }

    // --- Movement physics (Steering AI) ---
    Vector2 diff;
    if (state == WANDER || state == SNATCH_CURSOR) {
        diff = target - pos;
    } else {
        diff = target - pos; // Simple homing: beak naturally leads
    }

    Vector2 moveDir = Vector2::Normalize(diff);
    float dist = Vector2::Length(diff);

    float tSpeed = (dist > 300 || state == FETCHING || state == CHASE_CURSOR || state == SNATCH_CURSOR || state == RETURNING || state == DRAG_WINDOW || state == CHASE_GOOSE)
        ? g_config.baseRunSpeed
        : g_config.baseWalkSpeed;
    if (state == CHASE_GOOSE) {
        // Rocket after the victim: 1.2x run speed ramping to 3x in 0.8 s.
        const float ramp = Clamp((float)((time - gooseYeetStart) / 0.8), 0.0f, 1.0f);
        tSpeed = g_config.baseRunSpeed * (1.2f + 1.8f * ramp);
    }

    currentSpeed = Lerp(currentSpeed, tSpeed, state == CHASE_GOOSE ? 0.2f : 0.05f);

    Vector2 steerForce{0, 0};

    // 1. SEEK / ARRIVE
    Vector2 desiredVel = moveDir * currentSpeed;
    // Slow down as we arrive
    float arrivalRadius = 50.0f;
    if (dist < arrivalRadius) {
        desiredVel = desiredVel * (dist / arrivalRadius);
    }
    // A chasing goose homes hard so its super speed turns into a catch.
    steerForce += (desiredVel - vel) * (state == CHASE_GOOSE ? 6.0f : 2.0f);

    // 2. PARABOLIC DRIFT (Tangential Force)
    // Curvature creates an arc. We use a force perpendicular to current velocity.
    // Not while chasing a goose: at super speed the arc becomes an orbit.
    if (Vector2::Length(vel) > 10.0f && state != CHASE_GOOSE) {
        Vector2 normalizedVel = Vector2::Normalize(vel);
        Vector2 tangent = { -normalizedVel.y, normalizedVel.x }; // Perpendicular
        // Apply tangential force based on current parabolicCurvature
        // Also fade out curvature as we get closer to target to ensure we land
        float curveFade = std::min(1.0f, dist / 200.0f);
        steerForce += tangent * (parabolicCurvature * currentSpeed * 0.8f * curveFade);
    }

    // 3. SEPARATION: steer away from nearby geese
    if (state == WANDER || state == FETCHING) {
        for (auto& other : g_geese) {
            if (other.id == id) continue;
            float d = Vector2::Distance(pos, other.pos);
            if (d > 0.1f && d < 70.0f) {
                float strength = (70.0f - d) / 70.0f;
                Vector2 away = Vector2::Normalize(pos - other.pos);
                steerForce += away * (strength * maxForce * 1.5f);
            }
        }
    }

    // 4. CONTEXT-AWARE EDGE AVOIDANCE
    // Fetches must reach the edge, and a goose rocketing at its victim would
    // otherwise veer away from anyone standing near one.
    if (state != FETCHING && state != CHASE_GOOSE) {
        // Look ahead (whisker) to see if we're heading for an edge
        float lookAhead = currentSpeed * 0.4f + 30.0f;
        Vector2 probePos = pos + Vector2::Normalize(vel) * lookAhead;
        
        float margin = 40.0f;
        Vector2 avoidance{0, 0};
        
        if (probePos.x < margin) avoidance.x = currentSpeed;
        else if (probePos.x > (float)w - margin) avoidance.x = -currentSpeed;
        if (probePos.y < margin) avoidance.y = currentSpeed;
        else if (probePos.y > (float)h - margin) avoidance.y = -currentSpeed;

        if (Vector2::Length(avoidance) > 0.1f) {
            steerForce += (avoidance - vel) * 3.0f;
        }
    }

    // Limit and apply force (a chasing goose gets far more grip so the super
    // speed is reached, and turned, almost immediately)
    const float forceCap = state == CHASE_GOOSE ? maxForce * 8.0f : maxForce;
    float forceMag = Vector2::Length(steerForce);
    if (forceMag > forceCap) {
        steerForce = steerForce * (forceCap / forceMag);
    }

    acceleration = steerForce;
    vel = vel + acceleration * (float)dt;

    // Limit speed to maxSpeed (currentSpeed)
    float speed = Vector2::Length(vel);
    if (speed > currentSpeed && speed > 1e-4) {
        vel = vel * (currentSpeed / speed);
    }
    
    pos = pos + vel * (float)dt;

    // --- SCREEN CLAMPING: prevent geese from wandering off-screen ---
    float minX = 0, minY = 0, maxX = (float)w, maxY = (float)h;
    

    if (state == FETCHING) {
        minX -= 50.0f; maxX += 50.0f; minY -= 50.0f; maxY += 50.0f;
    } else {
        minX += 20.0f; maxX -= 20.0f; minY += 20.0f; maxY -= 20.0f;
    }
    // Explicit clamping with bounce logic for SNATCH_CURSOR and RETURNING (dragging items)
    // We also include FETCHING here to prevent geese from wandering too far off-screen.
    if (pos.x < minX) {
        pos.x = minX + 1.0f;
        if ((state == SNATCH_CURSOR || state == RETURNING ||
             (state == DRAG_WINDOW && dragPhase == DRAG_CARRY) ||
             state == FETCHING) && vel.x < 0) {
            vel.x = std::abs(vel.x) + 50.0f; // force bounce away
        }
    } else if (pos.x > maxX) {
        pos.x = maxX - 1.0f;
        if ((state == SNATCH_CURSOR || state == RETURNING ||
             (state == DRAG_WINDOW && dragPhase == DRAG_CARRY) ||
             state == FETCHING) && vel.x > 0) {
            vel.x = -std::abs(vel.x) - 50.0f;
        }
    }

    if (pos.y < minY) {
        pos.y = minY + 1.0f;
        if ((state == SNATCH_CURSOR || state == RETURNING ||
             (state == DRAG_WINDOW && dragPhase == DRAG_CARRY) ||
             state == FETCHING) && vel.y < 0) {
            vel.y = std::abs(vel.y) + 50.0f;
        }
    } else if (pos.y > maxY) {
        pos.y = maxY - 1.0f;
        if ((state == SNATCH_CURSOR || state == RETURNING ||
             (state == DRAG_WINDOW && dragPhase == DRAG_CARRY) ||
             state == FETCHING) && vel.y > 0) {
            vel.y = -std::abs(vel.y) - 50.0f;
        }
    }
    CursorBackend* backend = g_backendManager.GetActiveBackend();
    bool canMoveAbs = (backend->Caps() & CAP_MOVE_ABS);
    bool canMoveRel = (backend->Caps() & CAP_MOVE_REL);


    // Smooth rotation
    // (A yeeting goose is aimed explicitly by its windup instead.)
    if (Vector2::Length(vel) > 1.0f && state != YEET_GOOSE) {
        Vector2 curDirVec = Vector2::FromAngleDegrees(dir);
        Vector2 targetDirVec = Vector2::Normalize(vel);

        // Pulling look: face the dragged item or the current cursor anchor.
        if (state == RETURNING ||
            (state == DRAG_WINDOW && dragPhase == DRAG_CARRY)) {
            targetDirVec = targetDirVec * -1.0f;
        } else if (state == SNATCH_CURSOR) {
            Vector2 toCursor = s_predictedCursor - pos;
            if (Vector2::Length(toCursor) > 2.0f) targetDirVec = Vector2::Normalize(toCursor);
            else targetDirVec = targetDirVec * -1.0f;
        }

        Vector2 blend = Vector2::Lerp(curDirVec, targetDirVec, 0.15f);
        dir = std::atan2(blend.y, blend.x) * RAD_TO_DEG;
    }

    // Final pose for this frame
    UpdateRig();
    SolveFeet(time);
    UpdateDrag(dt);

    // The victim's grab point rides this frame's beak tip.
    if (state == YEET_GOOSE && !gooseYeetLaunched) {
        Goose* victim = GetGooseById(yeetVictimId);
        if (victim && victim->heldByGooseId == id) {
            victim->MoveGrab(WorldToDevice(GetBeakTipWorld()));
        }
    }

    if (state == SNATCH_CURSOR) {
        Vector2 bt = WorldToDevice(GetBeakTipWorld());
        bt.x = std::clamp(bt.x, 0.0f, (float)std::max(0, w - 1));
        bt.y = std::clamp(bt.y, 0.0f, (float)std::max(0, h - 1));
        
        if (canMoveAbs) {
            // Tier 1/2: Warp cursor to beak
            backend->MoveCursorAbs(std::lround(bt.x), std::lround(bt.y));
            s_predictedCursor = bt;
        } else if (canMoveRel) {
            // Tier 3: Drag cursor toward beak using relative moves
            // We assume the cursor is at `s_predictedCursor`.
            Vector2 delta = bt - s_predictedCursor;
            
            // Limit delta to avoid crazy jumps if prediction desyncs
            float maxStep = 500.0f * (float)dt; 
            if (Vector2::Length(delta) > maxStep) {
                delta = Vector2::Normalize(delta) * maxStep;
            }

            if (std::abs(delta.x) >= 1.0f || std::abs(delta.y) >= 1.0f) {
                backend->MoveCursorRel((int)delta.x, (int)delta.y);
                s_predictedCursor = s_predictedCursor + delta;
            }
        }
    }

    // --- FINAL SAFETY CHECKS ---
    
    // 1. Cap velocity/speed to prevent explosions
    if (Vector2::Length(vel) > 2000.0f) vel = Vector2::Normalize(vel) * 2000.0f;
    if (!std::isfinite(currentSpeed)) currentSpeed = 0.0f;
    if (currentSpeed > 2000.0f) currentSpeed = 2000.0f;

    // 2. NaN/Inf Check - if ANY physics state broke, force a total reset for this goose
    bool bad = !std::isfinite(pos.x) || !std::isfinite(pos.y) || 
               !std::isfinite(vel.x) || !std::isfinite(vel.y) ||
               !std::isfinite(dir) ||
               !std::isfinite(dragPos.x) || !std::isfinite(dragPos.y) ||
               !std::isfinite(dragRot) || !std::isfinite(dragRotVel) ||
               !std::isfinite(rig.neckBase.x) || !std::isfinite(rig.neckBase.y);

    if (bad) {
        std::cerr << "[Goose] NaN/Inf detected for ID " << id << "! Emergency total physics reset." << std::endl;
        std::cerr << "       pos:(" << pos.x << "," << pos.y << ") vel:(" << vel.x << "," << vel.y << ") dir:" << dir << std::endl;
        std::cerr << "       drag:(" << dragPos.x << "," << dragPos.y << ") rot:" << dragRot << std::endl;

        if (state == DRAG_WINDOW || !dragWindowAddr.empty()) {
            RestoreDraggedWindowNow(false);
        } else if (heldItem) {
            delete heldItem;
            heldItem = nullptr;
        }
        ReleaseYeetVictim();
        heldByGooseId = -1;
        if (softActive) {
            softActive = false;
            softThrown = false;
            softBody = SoftBody{};
            if (g_heldGooseId == id) g_heldGooseId = -1;
            ragdoll = RagdollState{};
        }
        pos.x = (float)w / 2.0f;
        pos.y = (float)h / 2.0f;
        vel = {0, 0};
        dir = 0.0f;
        currentSpeed = g_config.baseWalkSpeed;
        dragPos = pos;
        dragVel = {0,0};
        dragRot = 0.0f;
        dragRotVel = 0.0f;
        dragInit = false;
        state = WANDER;
        
        UiLogPush("Emergency Physics Reset: Goose " + std::to_string(id));
    }
}

// =========================================================
// SOFT BODY (cursor pickup + yeet)
// =========================================================
namespace {

float DistanceToSegment(Vector2 point, Vector2 a, Vector2 b) {
    const Vector2 segment = b - a;
    const float lengthSquared = Dot(segment, segment);
    if (lengthSquared < 1e-6f) return Vector2::Distance(point, a);
    const float t = Clamp(Dot(point - a, segment) / lengthSquared, 0.0f, 1.0f);
    return Vector2::Distance(point, a + segment * t);
}

// Closed Catmull-Rom spline through the torso ring. The soft body is drawn as
// one smooth blob so deformation reads as flesh, not as a polygon.
void PathClosedSpline(cairo_t* cr, const Vector2* pts, int n) {
    if (n < 3) return;
    cairo_move_to(cr, pts[0].x, pts[0].y);
    for (int i = 0; i < n; ++i) {
        const Vector2& p0 = pts[(i - 1 + n) % n];
        const Vector2& p1 = pts[i];
        const Vector2& p2 = pts[(i + 1) % n];
        const Vector2& p3 = pts[(i + 2) % n];
        const Vector2 c1 = p1 + (p2 - p0) * (1.0f / 6.0f);
        const Vector2 c2 = p2 - (p3 - p1) * (1.0f / 6.0f);
        cairo_curve_to(cr, c1.x, c1.y, c2.x, c2.y, p2.x, p2.y);
    }
    cairo_close_path(cr);
}

// Inward offset of a closed polygon, used for the body's white fill.
void InsetPolygon(const Vector2* pts, int n, float inset, Vector2* out) {
    float area2 = 0.0f;
    for (int i = 0; i < n; ++i) {
        const Vector2& a = pts[i];
        const Vector2& b = pts[(i + 1) % n];
        area2 += a.x * b.y - b.x * a.y;
    }
    const float wind = (area2 >= 0.0f) ? 1.0f : -1.0f;

    for (int i = 0; i < n; ++i) {
        Vector2 tangent = pts[(i + 1) % n] - pts[(i - 1 + n) % n];
        if (Vector2::Length(tangent) < 1e-4f) {
            out[i] = pts[i];
            continue;
        }
        tangent = Vector2::Normalize(tangent);
        const Vector2 outward{ tangent.y * wind, -tangent.x * wind };
        out[i] = pts[i] - outward * inset;
    }
}

} // namespace

float Goose::GrabRadius() const {
    return 56.0f * std::max(0.35f, g_config.globalScale);
}

bool Goose::ContainsGrabPoint(Vector2 devicePos) const {
    const float scale = std::max(0.35f, g_config.globalScale);
    const Vector2 point = pos + (devicePos - pos) / scale;
    const Vector2 rawFwd = Vector2::FromAngleDegrees(dir);
    const Vector2 fwd{ rawFwd.x * ISO_SCALE.x, rawFwd.y * ISO_SCALE.y };

    const Vector2 bodyFront = rig.body + fwd * 13.0f;
    const Vector2 bodyBack = rig.body - fwd * 13.0f;
    if (DistanceToSegment(point, bodyBack, bodyFront) <= 15.0f) return true;
    if (DistanceToSegment(point, rig.neckBase, rig.neckHead) <= 9.5f) return true;
    if (DistanceToSegment(point, rig.neckHead, rig.head2) <= 10.5f) return true;
    return false;
}

// Build the soft-body skeleton from the current rig pose. Called on grab so
// the goose starts deforming from wherever it actually is.
void Goose::BuildSoftBody() {
    softBody = SoftBody{};
    softBody.gravity = { 0.0f, g_config.ragdollGravity };
    softBody.linearDamping = std::max(0.2f, g_config.ragdollDamping * 0.35f);
    softBody.restitution = Clamp(g_config.softbodyBounce, 0.0f, 0.95f);
    softBody.friction = Clamp(g_config.softbodyFriction, 0.0f, 1.0f);
    softBody.attachStiffness = Clamp(g_config.softbodyGrabStiffness, 0.05f, 1.0f);
    softBody.iterations = std::clamp(g_config.softbodyIterations, 1, 12);
    softBody.substeps = std::clamp(g_config.softbodySubsteps, 1, 6);
    softBody.maxSpeed = 7000.0f;

    // Spring stiffness is deliberately partial: each iteration only fixes a
    // fraction of the length error, so the body holds its shape but visibly
    // stretches and squashes under real loads instead of behaving rigid.
    const float stiff = Clamp(g_config.softbodyStiffness * 0.35f, 0.05f, 0.7f);
    const Vector2 rawFwd = Vector2::FromAngleDegrees(dir);
    const Vector2 fwdVec{ rawFwd.x * ISO_SCALE.x, rawFwd.y * ISO_SCALE.y };
    const float fwdLen = std::max(0.2f, Vector2::Length(fwdVec));
    const Vector2 axis = Vector2::Normalize(fwdVec);
    const Vector2 perp{ -axis.y, axis.x };

    // Torso: an ellipse ring standing in for the body capsule. rx follows the
    // isometric foreshortening so a goose facing away is as compact on screen
    // as the drawn body is.
    const float rx = 11.0f * fwdLen + 9.0f;
    const float ry = 12.0f;
    const Vector2 bodyCenter = rig.body;
    for (int i = 0; i < SB_RING_COUNT; ++i) {
        const float a = (float)i / (float)SB_RING_COUNT * 2.0f * (float)PI;
        const Vector2 p =
            bodyCenter + axis * (rx * std::cos(a)) + perp * (ry * std::sin(a));
        softBody.AddPoint(p, 0.55f);
    }

    // Neck, head, beak and feet start exactly where the drawn rig has them.
    softBody.AddPoint(rig.neckBase, 0.85f); // SB_NECK_BASE
    softBody.AddPoint(rig.neckHead, 0.75f); // SB_NECK_HEAD
    softBody.AddPoint(rig.head2, 0.75f);    // SB_HEAD

    Vector2 headDir = rig.head2 - rig.neckHead;
    headDir = (Vector2::Length(headDir) > 1e-3f) ? Vector2::Normalize(headDir) : axis;
    const Vector2 beakBase = rig.neckHead + headDir * BEAK_BASE_OFFSET;
    softBody.AddPoint(beakBase + headDir * BEAK_LEN, 1.1f); // SB_BEAK_TIP
    softBody.AddPoint(rig.lFoot.currentPos, 1.3f);          // SB_FOOT_L
    softBody.AddPoint(rig.rFoot.currentPos, 1.3f);          // SB_FOOT_R

    // Structure: torso perimeter, one copy of each diameter, and shear braces.
    for (int i = 0; i < SB_RING_COUNT; ++i) {
        softBody.AddSpring(SB_RING + i, SB_RING + (i + 1) % SB_RING_COUNT,
                           1.0f * stiff);
        if (i < SB_RING_COUNT / 2) {
            softBody.AddSpring(SB_RING + i, SB_RING + i + SB_RING_COUNT / 2,
                               0.75f * stiff);
        }
        softBody.AddSpring(SB_RING + i, SB_RING + (i + 2) % SB_RING_COUNT,
                           0.5f * stiff);
    }

    // Head and beak springs stay firm so they keep their length. The neck
    // links are softer so the neck can bend and trail behind the torso.
    softBody.AddSpring(SB_NECK_BASE, SB_NECK_HEAD, 0.55f * stiff);
    softBody.AddSpring(SB_NECK_HEAD, SB_HEAD, 1.0f * stiff);
    softBody.AddSpring(SB_HEAD, SB_BEAK_TIP, 1.0f * stiff);
    softBody.AddSpring(SB_NECK_BASE, SB_HEAD, 0.3f * stiff);
    softBody.AddSpring(SB_NECK_HEAD, SB_BEAK_TIP, 0.4f * stiff);

    // Hang the neck and feet off the nearest torso points.
    auto anchorToRing = [&](int point, int neighbours, float springStiffness) {
        std::vector<int> anchors;
        std::vector<unsigned char> used(SB_RING_COUNT, 0);
        for (int pick = 0; pick < neighbours; ++pick) {
            int best = -1;
            float bestDist = 0.0f;
            for (int i = 0; i < SB_RING_COUNT; ++i) {
                if (used[i]) continue;
                const float d = Vector2::Distance(softBody.points[point].pos,
                                                  softBody.points[SB_RING + i].pos);
                if (best < 0 || d < bestDist) { best = i; bestDist = d; }
            }
            if (best < 0) break;
            used[best] = 1;
            softBody.AddSpring(point, SB_RING + best, springStiffness * stiff);
            anchors.push_back(SB_RING + best);
        }
        return anchors;
    };
    // Loose anchors: the neck and feet may lag behind the torso and swing.
    const std::vector<int> neckAnchors = anchorToRing(SB_NECK_BASE, 3, 0.5f);
    // Legs are pendulums: each foot hangs from the single nearest torso point
    // on one soft link, so it can swing freely and stretch under gravity and
    // spin. (Two anchors would fix the foot's position like a rigid strut.)
    // The foot starts only a few pixels from that point, so the link gets a
    // real leg length rather than tucking the foot into the body.
    constexpr float kLegLength = 12.0f;
    auto hangLeg = [&](int foot) {
        int hip = SB_RING;
        for (int i = 1; i < SB_RING_COUNT; ++i) {
            if (Vector2::Distance(softBody.points[foot].pos, softBody.points[SB_RING + i].pos) <
                Vector2::Distance(softBody.points[foot].pos, softBody.points[hip].pos)) {
                hip = SB_RING + i;
            }
        }
        SoftSpring leg;
        leg.a = hip;
        leg.b = foot;
        leg.rest = std::max(kLegLength, Vector2::Distance(softBody.points[foot].pos,
                                                          softBody.points[hip].pos));
        leg.stiffness = Clamp(0.25f * stiff, 0.0f, 1.0f);
        softBody.springs.push_back(leg);
        return hip;
    };
    softHip[0] = hangLeg(SB_FOOT_L);
    softHip[1] = hangLeg(SB_FOOT_R);

    // Skin and connective tissue stretch, but never tear off under a flick.
    // Skull/beak links are firmer than the neck and torso. A foot sits only a
    // few pixels from its anchors, so a ratio limit would pin it in place;
    // legs instead get a fixed reach in world pixels.
    constexpr float kLegReach = 30.0f;
    for (auto& spring : softBody.springs) {
        const bool headLink = spring.a >= SB_NECK_HEAD && spring.a <= SB_BEAK_TIP &&
                              spring.b >= SB_NECK_HEAD && spring.b <= SB_BEAK_TIP;
        const bool footLink = spring.a >= SB_FOOT_L || spring.b >= SB_FOOT_L;
        spring.maxStretch = headLink ? 1.15f
                          : footLink ? (spring.rest + kLegReach) / spring.rest
                                     : 1.5f;
    }

    // Pressure: keep the torso inflated while it is stretched and squeezed.
    {
        std::vector<int> ring;
        ring.reserve(SB_RING_COUNT);
        for (int i = 0; i < SB_RING_COUNT; ++i) ring.push_back(SB_RING + i);
        softBody.AddArea(ring, Clamp(g_config.softbodyPressure, 0.0f, 1.0f));
    }

    // Shape memory is local, so a limb can swing without dragging the whole
    // goose along. The torso keeps its oval, the head and beak hold their
    // shape, and the neck keeps a weak rest pose with the torso patch it hangs
    // from. Feet get no pose memory so the legs stay free to stretch and swing.
    const float memory = Clamp(g_config.softbodyShapeMemory, 0.0f, 0.9f);
    if (memory > 0.0f) {
        std::vector<int> ring;
        ring.reserve(SB_RING_COUNT);
        for (int i = 0; i < SB_RING_COUNT; ++i) ring.push_back(SB_RING + i);
        softBody.AddMemory(ring, memory);

        softBody.AddMemory({ SB_NECK_HEAD, SB_HEAD, SB_BEAK_TIP }, memory);

        auto limbMemory = [&](int limb, const std::vector<int>& anchors, float scale) {
            std::vector<int> cluster{ limb };
            cluster.insert(cluster.end(), anchors.begin(), anchors.end());
            softBody.AddMemory(cluster, memory * scale);
        };
        limbMemory(SB_NECK_BASE, neckAnchors, 0.3f);
    }
}

bool Goose::GrabAt(Vector2 devicePos) {
    if (g_heldGooseId != -1 && g_heldGooseId != id) return false;
    if (!ContainsGrabPoint(devicePos)) return false;
    BeginHold(devicePos);
    g_heldGooseId = id;
    UiLogPush("Goose " + std::to_string(id) + " picked up");
    return true;
}

bool Goose::CanBeGooseYeeted() const {
    if (softActive || g_heldGooseId == id) return false;
    // Off-screen fetches and window/cursor carries are left alone.
    return state == WANDER || state == RETURNING || state == CHASE_CURSOR;
}

bool Goose::GrabByGoose(int attackerId, Vector2 beakDevice) {
    if (attackerId == id || !CanBeGooseYeeted()) return false;
    BeginHold(beakDevice);
    heldByGooseId = attackerId;
    return true;
}

// Shared by cursor and goose grabs: drop the current behavior and hand the
// pose to the soft body, attached at the point nearest the grab.
void Goose::BeginHold(Vector2 devicePos) {
    const Vector2 carryVel = vel; // walking momentum carries into the body
    CancelCurrentBehavior();
    UpdateRig(); // the soft body is built from the freshest drawn pose

    BuildSoftBody();
    for (auto& p : softBody.points) p.vel = carryVel;
    const Vector2 worldPos = DeviceToWorld(devicePos);
    int nearest = -1;
    float nearestDist = 0.0f;
    for (int i = 0; i < (int)softBody.points.size(); ++i) {
        const float d = Vector2::Distance(softBody.points[i].pos, worldPos);
        if (nearest < 0 || d < nearestDist) { nearest = i; nearestDist = d; }
    }
    softBody.attachedPoint = nearest;
    softBody.attachTarget = worldPos;

    ragdoll = RagdollState{};
    ragdoll.pivot = devicePos;
    ragdoll.nextSquirm = g_time + 0.35 + Rand01() * 1.1;

    softActive = true;
    softThrown = false;
    softFlightElapsed = 0.0;
    softSettleDeadline = 0.0;
    softCursorVel = { 0.0f, 0.0f };
    softLastTarget = worldPos;
    softTargetPrimed = false;
    heldByGooseId = -1;

    vel = { 0, 0 };
    acceleration = { 0, 0 };
    currentSpeed = 0.0f;
    state = HELD;
}

void Goose::MoveGrab(Vector2 devicePos) {
    if (state != HELD || !softActive) return;
    // Only record the target here; velocity is derived in UpdateSoftBody on
    // the fixed frame clock. Pointer events arrive at an irregular rate, so
    // differentiating them directly would be pure noise.
    ragdoll.pivot = devicePos;
    softBody.attachTarget = DeviceToWorld(devicePos);
}

void Goose::ReleaseGrab(int w, int h) {
    if (g_heldGooseId == id) g_heldGooseId = -1;
    if (state != HELD) return;

    const float throwSpeed = Vector2::Length(softCursorVel);
    const bool thrown = throwSpeed >= g_config.softbodyYeetSpeed;
    if (thrown) {
        // Hand the flick's momentum to the body so the throw really carries;
        // the grabbed point already moved with the cursor, this adds the
        // whip-crack the rest of the body inherits.
        for (auto& p : softBody.points) p.vel += softCursorVel * 0.15f;
    }
    EnterFlight(thrown);
    UiLogPush(thrown ? "Goose " + std::to_string(id) + " yeeted at " +
                           std::to_string((int)throwSpeed) + " px/s"
                     : "Goose " + std::to_string(id) + " dropped");
}

void Goose::FlingFromGoose(Vector2 landingDevice) {
    if (state != HELD || heldByGooseId == -1) return;
    EnterFlight(true); // sets the flight gravity and air drag solved below

    // Every point gets the same launch velocity, so the whole body follows
    // the torso's trajectory. Solve x(T) = goal under the solver's exact
    // linear drag k and gravity g:
    //   x(T) = x0 + v0 * E + g * (T - E) / k,   E = (1 - e^(-kT)) / k.
    const Vector2 start = softBody.CenterOfRange(SB_RING, SB_RING_COUNT);
    const Vector2 goal = DeviceToWorld(landingDevice);
    const float k = softBody.linearDamping;
    const float g = softBody.gravity.y * softBody.gravityScale;
    auto launchFor = [&](float flightTime) {
        const float e = k > 1e-4f ? -std::expm1(-k * flightTime) / k : flightTime;
        const float fall = k > 1e-4f ? (flightTime - e) / k
                                     : 0.5f * flightTime * flightTime;
        return (goal - start - Vector2{ 0.0f, g * fall }) / e;
    };
    // Highest point of the arc (screen y grows downward), same drag model.
    auto apexFor = [&](float vy) {
        if (vy >= 0.0f || g <= 0.0f) return start.y;
        if (k <= 1e-4f) return start.y - vy * vy / (2.0f * g);
        const float terminal = g / k;
        const float riseTime = std::log((terminal - vy) / terminal) / k;
        return start.y + terminal * riseTime +
               (vy - terminal) * -std::expm1(-k * riseTime) / k;
    };

    // Long throws get a longer, higher arc, but never into the top of the
    // screen: a ceiling bounce would wreck the aim, so flatten it instead.
    const float distance = Vector2::Distance(WorldToDevice(start), landingDevice);
    const float ceiling = softBody.boundMinY + 24.0f;
    float flightTime = Clamp(0.35f + distance / 1800.0f, 0.45f, 1.0f);
    Vector2 launch = launchFor(flightTime);
    for (int i = 0; i < 12 && flightTime > 0.25f && apexFor(launch.y) < ceiling; ++i) {
        flightTime *= 0.85f;
        launch = launchFor(flightTime);
    }
    // The safety cap is in world units, which grow as the scale shrinks; a
    // solved, screen-bounded throw must not be clipped short by it.
    softBody.maxSpeed = std::max(softBody.maxSpeed, Vector2::Length(launch) * 1.05f);
    for (auto& p : softBody.points) p.vel = launch;
}

void Goose::DropFromGoose() {
    if (state != HELD || heldByGooseId == -1) return;
    EnterFlight(false);
}

// Leave HELD: a throw flies under gravity and bounces; a gentle drop stays
// weightless and re-forms where it was let go.
void Goose::EnterFlight(bool thrown) {
    softBody.attachedPoint = -1;
    softFlightElapsed = 0.0;
    heldByGooseId = -1;
    softThrown = thrown;
    if (thrown) {
        softBody.gravityScale = 1.0f;
        softBody.collideBounds = true;
        softBody.linearDamping = std::max(
            0.1f, g_config.ragdollDamping * 0.2f + g_config.softbodyAirDrag);
    } else {
        softBody.gravityScale = 0.0f;
        softBody.collideBounds = false;
        softBody.linearDamping = std::max(0.5f, g_config.ragdollDamping);
        for (auto& memory : softBody.memories) {
            memory.stiffness = Clamp(memory.stiffness * 2.5f, 0.0f, 0.9f);
        }
        softSettleDeadline = 0.7;
    }
    state = FLUNG;
    ragdoll.pivotVel = { 0.0f, 0.0f };
    currentSpeed = 0.0f;
    vel = { 0, 0 };
    acceleration = { 0, 0 };
}

// One physics frame of the soft body. Owns the goose while HELD or FLUNG:
// cursor differentiation, struggle impulses, then the solver, then the
// settle/stand-up transition.
void Goose::UpdateSoftBody(double dt, double time, int w, int h) {
    if (!softActive || (int)softBody.points.size() < (int)SB_COUNT) return;
    const float step = (float)std::clamp(dt, 1e-4, 0.05);

    // Desktop bounds in world space (the space the points live in).
    const Vector2 topLeft = DeviceToWorld({ 0.0f, 0.0f });
    const Vector2 bottomRight = DeviceToWorld(
        { (float)std::max(1, w - 1), (float)std::max(1, h - 1) });
    softBody.SetBounds(std::min(topLeft.x, bottomRight.x),
                       std::min(topLeft.y, bottomRight.y),
                       std::max(topLeft.x, bottomRight.x),
                       std::max(topLeft.y, bottomRight.y));

    if (state == HELD) {
        // Differentiate the cursor on the fixed frame clock, not on irregular
        // pointer events; a teleporting sample must not fling the body.
        if (softTargetPrimed) {
            const Vector2 rawVel = (softBody.attachTarget - softLastTarget) / step;
            const float mag = Vector2::Length(rawVel);
            const float kMax = 12000.0f;
            const Vector2 clampedVel = (mag > kMax) ? rawVel * (kMax / mag) : rawVel;
            const float smooth = 1.0f - std::exp(-18.0f * step);
            softCursorVel = softCursorVel * (1.0f - smooth) + clampedVel * smooth;
        } else {
            softCursorVel = { 0.0f, 0.0f };
        }
        softLastTarget = softBody.attachTarget;
        softTargetPrimed = true;
        ragdoll.pivotVel = softCursorVel;

        // Struggle: periodic muscular impulses through the neck and legs.
        if (time >= ragdoll.nextSquirm) {
            const float strength = std::max(0.0f, g_config.ragdollSquirmStrength);
            const float kick = (25.0f + (float)(rand() % 55)) * strength;
            auto kickPoint = [&](int index) {
                if (index < 0 || index >= (int)softBody.points.size()) return;
                const float a = (float)(rand() % 628) * 0.01f;
                softBody.points[index].vel += Vector2{ std::cos(a), std::sin(a) } * kick;
            };
            kickPoint(SB_FOOT_L);
            kickPoint(SB_FOOT_R);
            if (rand() % 2) {
                kickPoint(SB_NECK_HEAD);
            } else {
                kickPoint(SB_HEAD);
            }
            ragdoll.squirmDrive =
                (0.55f + (float)(rand() % 45) / 100.0f) * std::min(1.0f, strength);
            ragdoll.nextSquirm = time + 0.45 + (double)(rand() % 150) * 0.01;
        }
    }
    ragdoll.squirmDrive = std::max(0.0f, ragdoll.squirmDrive - (float)dt * 1.4f);

    softBody.Step(step);

    // World space is scaled about `pos`, and UpdateRigFromSoft moves `pos` to
    // the torso center. Re-anchoring alone would slide every point on screen
    // by (pos' - pos)(1 - scale) each frame, so the cursor target runs away
    // from the body (lag and a whipping swing at scales below 1). Translate
    // the body so its device-space pose is unchanged by the new anchor.
    const float scale = g_config.globalScale;
    if (scale >= 0.01f && std::fabs(scale - 1.0f) > 1e-4f) {
        const Vector2 center = softBody.CenterOfRange(SB_RING, SB_RING_COUNT);
        const Vector2 anchorMove = (center - pos) * scale;
        const Vector2 shift = anchorMove * (1.0f - 1.0f / scale);
        softBody.Translate(shift);
        softLastTarget += shift;
    }
    UpdateRigFromSoft();

    if (state == FLUNG) {
        softFlightElapsed += dt;
        const float speed = softBody.AverageSpeed();
        const bool slow = speed < (softThrown ? 75.0f : 55.0f);
        const bool grounded = std::any_of(
            softBody.points.begin(), softBody.points.end(),
            [&](const SoftPoint& p) { return p.pos.y >= softBody.boundMaxY - 0.5f; });
        const double minAge = softThrown ? 0.4 : 0.25;
        // Low speed at the top of a throw is not a landing. Only get up after
        // ground contact; gentle drops still recover in place.
        if ((softFlightElapsed > minAge && slow && (!softThrown || grounded)) ||
            (!softThrown && softSettleDeadline > 0.0 &&
             softFlightElapsed > softSettleDeadline) ||
            softFlightElapsed > 6.0) {
            FinishSoftBody(w, h);
        }
    }
}

// The simulation is the source of truth while it is active; this copies the
// deformed skeleton back into the rig so cosmetics, eyes, beak helpers and the
// window-drag code keep seeing a coherent goose.
void Goose::UpdateRigFromSoft() {
    if ((int)softBody.points.size() < (int)SB_COUNT) return;
    const SoftPoint* pts = softBody.points.data();

    const Vector2 ringCenter = softBody.CenterOfRange(SB_RING, SB_RING_COUNT);
    const Vector2 tail = pts[SB_RING + SB_RING_COUNT / 2].pos;
    const Vector2 nose = pts[SB_RING].pos;
    const Vector2 spine = nose - tail;
    if (Vector2::Length(spine) > 1e-3f) {
        const Vector2 raw{ spine.x / std::max(1e-3f, ISO_SCALE.x),
                           spine.y / std::max(1e-3f, ISO_SCALE.y) };
        if (Vector2::Length(raw) > 1e-4f) {
            dir = std::atan2(raw.y, raw.x) * RAD_TO_DEG;
        }
    }

    rig.body = ringCenter;
    rig.underbody = ringCenter + Vector2{ 0.0f, 5.0f };
    rig.neckBase = pts[SB_NECK_BASE].pos;
    rig.neckHead = pts[SB_NECK_HEAD].pos;
    rig.head2 = pts[SB_HEAD].pos;
    rig.head1 = Vector2::Lerp(rig.neckHead, rig.head2, 0.5f);
    rig.lFoot.currentPos = pts[SB_FOOT_L].pos;
    rig.rFoot.currentPos = pts[SB_FOOT_R].pos;
    rig.lFoot.moveStartTime = -1.0;
    rig.rFoot.moveStartTime = -1.0;
    pos = ringCenter;
}

// Hand control back to the walk AI once the flop has settled.
void Goose::FinishSoftBody(int w, int h) {
    if (!softActive && state != HELD && state != FLUNG) return;
    softActive = false;
    softThrown = false;
    softBody.attachedPoint = -1;
    softCursorVel = { 0.0f, 0.0f };
    softTargetPrimed = false;
    softFlightElapsed = 0.0;
    softSettleDeadline = 0.0;
    if (state == HELD || state == FLUNG) state = WANDER;
    if (g_heldGooseId == id) g_heldGooseId = -1;

    vel = { 0, 0 };
    acceleration = { 0, 0 };
    currentSpeed = 0.0f;
    ragdoll = RagdollState{};

    // Feet snap under the body so the walk cycle does not stride in from
    // wherever the flop left them.
    rig.lFoot.currentPos = GetFootHome(-90.0f);
    rig.rFoot.currentPos = GetFootHome(90.0f);
    rig.lFoot.moveStartTime = -1.0;
    rig.rFoot.moveStartTime = -1.0;

    if (!std::isfinite(pos.x) || !std::isfinite(pos.y)) {
        pos = { (float)std::max(1, w) * 0.5f, (float)std::max(1, h) * 0.5f };
    }
    pos.x = Clamp(pos.x, 20.0f, (float)std::max(40, w - 20));
    pos.y = Clamp(pos.y, 20.0f, (float)std::max(40, h - 20));
    PickNewTarget(w, h);
    UiLogPush("Goose " + std::to_string(id) + " got back up");
}

// Soft pose rendering. The torso is one smooth blob; neck, head and beak are
// stroked through the simulated points in the same widths and order as the
// normal rig so the goose still reads as the desktop goose.
void Goose::DrawSoft(cairo_t* cr) {
    if ((int)softBody.points.size() < (int)SB_COUNT) return;
    const SoftPoint* pts = softBody.points.data();

    Vector2 headFwd = pts[SB_HEAD].pos - pts[SB_NECK_HEAD].pos;
    headFwd = (Vector2::Length(headFwd) > 1e-3f)
        ? Vector2::Normalize(headFwd) : Vector2::FromAngleDegrees(dir);
    const float back = Clamp(-headFwd.y, 0.0f, 1.0f);

    // shadow
    const float shadowScale = softThrown ? 0.7f : 0.85f;
    DrawEllipse(cr, pos + Vector2{ 2, 10 }, (int)(20 * shadowScale),
                (int)(15 * shadowScale), 0, 0, 0,
                softThrown ? 0.14f : 0.22f);

    // Draw legs beneath the torso so swinging feet remain visibly connected.
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, ORANGE[0], ORANGE[1], ORANGE[2]);
    DrawLine(cr, pts[softHip[0]].pos, pts[SB_FOOT_L].pos, 3.5f);
    DrawLine(cr, pts[softHip[1]].pos, pts[SB_FOOT_R].pos, 3.5f);
    DrawEllipse(cr, pts[SB_FOOT_L].pos, 4, 4, ORANGE[0], ORANGE[1], ORANGE[2]);
    DrawEllipse(cr, pts[SB_FOOT_R].pos, 4, 4, ORANGE[0], ORANGE[1], ORANGE[2]);

    Vector2 ring[SB_RING_COUNT];
    for (int i = 0; i < SB_RING_COUNT; ++i) ring[i] = pts[SB_RING + i].pos;
    Vector2 inner[SB_RING_COUNT];
    InsetPolygon(ring, SB_RING_COUNT, 1.8f, inner);

    const Vector2 headMid =
        Vector2::Lerp(pts[SB_NECK_HEAD].pos, pts[SB_HEAD].pos, 0.5f);

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, OUTLINE_GRAY[0], OUTLINE_GRAY[1], OUTLINE_GRAY[2]);
    PathClosedSpline(cr, ring, SB_RING_COUNT);
    cairo_fill(cr);
    DrawLine(cr, rig.body, pts[SB_NECK_BASE].pos, 15);
    DrawLine(cr, pts[SB_NECK_BASE].pos, pts[SB_NECK_HEAD].pos, 15);
    DrawLine(cr, pts[SB_NECK_HEAD].pos, headMid, 17);
    DrawLine(cr, headMid, pts[SB_HEAD].pos, 12);

    // Beak first, so the white head occludes it when the goose faces away.
    const Vector2 beakBase = pts[SB_NECK_HEAD].pos + headFwd * BEAK_BASE_OFFSET;
    cairo_set_source_rgb(cr, ORANGE[0], ORANGE[1], ORANGE[2]);
    DrawLine(cr, beakBase, pts[SB_BEAK_TIP].pos, BEAK_WID);

    cairo_set_source_rgb(cr, 1, 1, 1);
    PathClosedSpline(cr, inner, SB_RING_COUNT);
    cairo_fill(cr);
    DrawLine(cr, rig.body, pts[SB_NECK_BASE].pos, 13);
    DrawLine(cr, pts[SB_NECK_BASE].pos, pts[SB_NECK_HEAD].pos, 13);
    DrawLine(cr, pts[SB_NECK_HEAD].pos, headMid, 15);
    DrawLine(cr, headMid, pts[SB_HEAD].pos, 10);

    // Eyes and cosmetics ride the simulated head instead of the walk rig.
    const Vector2 savedNeckHead = rig.neckHead;
    rig.neckHead = Vector2::Lerp(pts[SB_NECK_HEAD].pos, pts[SB_HEAD].pos, 0.3f);
    DrawEyes(cr, headFwd, back);
    rig.neckHead = savedNeckHead;

    const Vector2 side{ -headFwd.y, headFwd.x };
    const Vector2 cosmeticsHead =
        Vector2::Lerp(pts[SB_NECK_HEAD].pos, pts[SB_HEAD].pos, 0.55f);
    Cosmetics_Draw(cr, skin, { cosmeticsHead, headFwd, side, back });
}

// =========================================================
// RIG (NO-PILL FIXES LIVE HERE)
// =========================================================

void Goose::UpdateRig() {
    Vector2 rawFwd = Vector2::FromAngleDegrees(dir);

    // isometric projection
    Vector2 fwd{ rawFwd.x * ISO_SCALE.x, rawFwd.y * ISO_SCALE.y };
    Vector2 up{ 0, -1 };

    // facing: +1 toward camera (down screen), -1 away (up screen)
    float facing = Dot(Vector2::Normalize(fwd), Vector2{0, 1});
    float back = Clamp(-facing, 0.0f, 1.0f);

    rig.underbody = pos + up * 9.0f;
    rig.body      = pos + up * 14.0f;

    if (yeetHeadDrive >= 0.0f) {
        // The yeet windup drives the neck directly: coil back, then whip forward.
        rig.neckLerp = std::clamp(yeetHeadDrive, 0.0f, 1.0f);
    } else {
        int targetState = (currentSpeed >= 150.0f) ? 1 : 0;
        rig.neckLerp = Lerp(rig.neckLerp, (float)targetState, 0.1f);
    }

    float neckH   = Lerp(20, 10, rig.neckLerp);
    float neckExt = Lerp(3, 16, rig.neckLerp);

    // Separate neck base slightly when facing away
    rig.neckBase = rig.body + fwd * 15.0f + up * (2.0f * back);

    // Key "no-pill" trick: push head slightly forward when facing away
    rig.neckHead = rig.neckBase + fwd * (neckExt + 2.0f * back) + up * neckH;

    // ✅ UPDATED: shorten head (round, original-like), do NOT tie to BEAK_LEN
    rig.head1 = rig.neckHead + fwd * HEAD1_OFFSET;
    rig.head2 = rig.neckHead + fwd * HEAD2_OFFSET;

    // Pickup poses are owned by the soft body; the walk rig above is only for
    // the normal states.
}

// =========================================================
// FEET (IK-ish stepping)
// =========================================================

Vector2 Goose::GetFootHome(float angleOffset) {
    float ang = dir + angleOffset;
    Vector2 raw = Vector2::FromAngleDegrees(ang);
    Vector2 side{ raw.x * ISO_SCALE.x, raw.y * ISO_SCALE.y };
    return pos + side * 4.0f; // Reduced from 6.0f
}

void Goose::SolveFeet(double time) {
    Vector2 lHome = GetFootHome(-90.0f);
    Vector2 rHome = GetFootHome( 90.0f);

    // Init
    if (rig.lFoot.currentPos.x == 0 && rig.lFoot.currentPos.y == 0) {
        rig.lFoot.currentPos = lHome;
        rig.rFoot.currentPos = rHome;
    }

    // Step tuning based on current speed
    float speed = std::max(0.0f, currentSpeed);
    float denom = std::max(1.0f, (g_config.baseRunSpeed - g_config.baseWalkSpeed));
    float speed01 = Clamp((speed - g_config.baseWalkSpeed) / denom, 0.0f, 1.0f);
    float stepTrigger = Lerp(5.0f, 9.0f, speed01); // Reduced from 7.0-12.0
    float overshoot = Lerp(1.5f, 4.0f, speed01);   // Reduced from 2.0-8.0
    float baseDur = Lerp(0.16f, 0.085f, speed01);
    float liftAmt = Lerp(3.0f, 7.0f, speed01);

    auto UpdateFoot = [&](FootState& f, Vector2 home) {
        if (f.moveStartTime < 0) {
            float dist = Vector2::Distance(f.currentPos, home);

            // If we got extremely far behind (teleport / big speed spike), snap.
            if (dist > 90.0f) {
                f.currentPos = home;
                f.moveStartTime = -1.0;
                return;
            }

            if (dist > stepTrigger) {
                // Only one foot should start moving at a time to avoid
                // both feet hopping simultaneously.
                if (rig.lFoot.moveStartTime >= 0.0 || rig.rFoot.moveStartTime >= 0.0)
                    return;

                f.moveOrigin = f.currentPos;
                f.moveDir = Vector2::Normalize(home - f.currentPos);
                f.moveStartTime = time;

                // Shorten step duration as speed increases; cap the duration so
                // fast walking doesn't make feet slow-lerp.
                float distFactor = Clamp(dist / 22.0f, 0.9f, 1.45f);
                f.moveDuration = Clamp(baseDur * distFactor, 0.055f, 0.18f);
            }
        } else {
            Vector2 target = home + f.moveDir * overshoot;
            float p = (float)(time - f.moveStartTime) / std::max(0.001f, f.moveDuration);

            if (p >= 1.0f) {
                f.currentPos = home;
                f.moveStartTime = -1;
                g_assets.Pat();

                // Mud tracking chance
                if (mudEnabled && (rand() % 100) < mudChance) {
                    Footprint fp;
                    fp.pos = home;
                    fp.dir = dir + ((&f == &rig.lFoot) ? -15.0f : 15.0f); // slight rotate per foot
                    fp.timeSpawned = time;
                    fp.lifetime = mudLifetime;
                    g_footprints.push_back(fp);
                }
            } else {
                float e = CubicEaseInOut(p);
                Vector2 base = Vector2::Lerp(f.moveOrigin, target, e);
                // Simple lift arc (0 at endpoints), makes stepping feel snappier.
                float lift = std::sin((float)PI * p) * liftAmt;
                f.currentPos = base + Vector2{0.0f, -lift};
            }
        }
    };

    UpdateFoot(rig.lFoot, lHome);
    UpdateFoot(rig.rFoot, rHome);
}

// =========================================================
// RENDERING
// =========================================================

void Goose::DrawEyes(cairo_t* cr, Vector2 fwd, float back) {
    Vector2 rawSide = Vector2::FromAngleDegrees(dir + 90.0f);
    Vector2 side{ rawSide.x * ISO_SCALE.x, rawSide.y * ISO_SCALE.y };
    Vector2 up{ 0, -1 };


    // eyes stay visible, compress when facing away
    float eyeSep  = Lerp(5.0f, 2.8f, back);
    float eyeLift = Lerp(0.0f, 1.5f, back);

    // ✅ UPDATED: eyes locked to head (no forward drift)
    Vector2 center = rig.neckHead + up * (3.0f + eyeLift);

    // When facing strongly away, show a single centered eye-dot to avoid
    // visual overlap; otherwise draw the two eyes separated by eyeSep.
    if (back > 0.82f) {
        DrawEllipse(cr, center, 2, 2, 0,0,0);
    } else {
        DrawEllipse(cr, center - side * eyeSep, 2, 2, 0,0,0);
        DrawEllipse(cr, center + side * eyeSep, 2, 2, 0,0,0);
    }
}

void Goose::DrawHeldItem(cairo_t* cr) {
    if (!heldItem) return;

    cairo_save(cr);

    // Use dragPos directly (it is in world coordinates). 
    // Since we are already inside the goose's master scale transform in Draw(),
    // Cairo will automatically handle the (pos + (dragPos-pos)*scale) mapping.
    cairo_translate(cr, dragPos.x, dragPos.y);

    cairo_rotate(cr, dragRot);
    // A tumbling window spins about its middle; a carried one hangs off the beak.
    const bool tumbling = dragIsYeet && dragPhase == DRAG_YEET_FLIGHT;
    cairo_translate(cr, -heldItem->w / 2, tumbling ? -heldItem->h / 2 : 0);

    if (heldItem->type == ItemData::MEME) {
        if (cairo_surface_t* surface = heldItem->Surface()) {
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_paint(cr);
        }
    } else if (heldItem->type == ItemData::TEXT) {
        // Notepad look
        cairo_set_source_rgb(cr, 1, 1, 0.9);
        cairo_rectangle(cr, 0, 0, heldItem->w, heldItem->h);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);

        PangoLayout* layout = pango_cairo_create_layout(cr);
        PangoFontDescription* desc = pango_font_description_from_string(kNoteFontDescription);
        pango_layout_set_font_description(layout, desc);
        pango_font_description_free(desc);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_text(layout, heldItem->Text().c_str(), -1);
        pango_layout_set_width(layout, (heldItem->w - kNotePadding) * PANGO_SCALE);
        cairo_move_to(cr, kNotePadding / 2, kNotePadding / 2);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    cairo_restore(cr);
}

void Goose::Draw(cairo_t* cr) {
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y)) return;

    cairo_save(cr);

    // global scale
    cairo_translate(cr, pos.x, pos.y);
    cairo_scale(cr, g_config.globalScale, g_config.globalScale);
    cairo_translate(cr, -pos.x, -pos.y);

    // While the soft body owns the pose (held, thrown or recovering) it draws
    // itself from the simulated skeleton instead of the walk rig.
    if (softActive) {
        DrawSoft(cr);
        cairo_restore(cr);
        return;
    }

    Vector2 rawFwd = Vector2::FromAngleDegrees(dir);
    Vector2 fwd{ rawFwd.x * ISO_SCALE.x, rawFwd.y * ISO_SCALE.y };

    // facing (for item behind/forward)
    float facing = Dot(Vector2::Normalize(fwd), Vector2{0, 1});
    float back = Clamp(-facing, 0.0f, 1.0f);
    bool facingBack = (back > 0.55f);

    // draw held item behind if facing away
    if (heldItem && facingBack) DrawHeldItem(cr);

    // shadow
    DrawEllipse(cr, pos + Vector2{2, 10}, 20, 15, 0, 0, 0, 0.3f);

    // feet
    DrawEllipse(cr, rig.lFoot.currentPos, 4, 4, ORANGE[0], ORANGE[1], ORANGE[2]);
    DrawEllipse(cr, rig.rFoot.currentPos, 4, 4, ORANGE[0], ORANGE[1], ORANGE[2]);

    // optional body squash when facing away (helps pill-look)

    cairo_save(cr);
    cairo_translate(cr, rig.body.x, rig.body.y);
    cairo_scale(cr, 1.0f, Lerp(1.0f, 0.92f, back));
    cairo_translate(cr, -rig.body.x, -rig.body.y);

    // body segments
    Vector2 bodyFront = rig.body + fwd * 11.0f;
    Vector2 bodyBack  = rig.body - fwd * 11.0f;
    Vector2 underFront= rig.underbody + fwd * 7.0f;
    Vector2 underBack = rig.underbody - fwd * 7.0f;
    const Vector2 bodyControl = rig.body;
    const Vector2 underControl = rig.underbody;

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, OUTLINE_GRAY[0], OUTLINE_GRAY[1], OUTLINE_GRAY[2]);
    DrawBentLine(cr, bodyFront, bodyControl, bodyBack, 24);
    DrawLine(cr, rig.neckBase, rig.neckHead, 15);
    DrawLine(cr, rig.neckHead, rig.head1, 17);
    DrawLine(cr, rig.head1, rig.head2, 12);
    DrawBentLine(cr, underFront, underControl, underBack, 15);

    const Vector2 fwdHead = fwd;
    Vector2 beakBase = rig.neckHead + fwdHead * BEAK_BASE_OFFSET;
    Vector2 beakTip = beakBase + fwdHead * BEAK_LEN;

    // Draw early so the white fill occludes the beak when facing away.
    cairo_set_source_rgb(cr, ORANGE[0], ORANGE[1], ORANGE[2]);
    DrawLine(cr, beakBase, beakTip, BEAK_WID);

    cairo_set_source_rgb(cr, 1, 1, 1);
    DrawBentLine(cr, bodyFront, bodyControl, bodyBack, 22);
    DrawLine(cr, rig.neckBase, rig.neckHead, 13);
    DrawLine(cr, rig.neckHead, rig.head1, 15);
    DrawLine(cr, rig.head1, rig.head2, 10);

    cairo_restore(cr); // body squash scope

    // eyes
    DrawEyes(cr, fwdHead, back);
    Vector2 rawSide = Vector2::FromAngleDegrees(dir + 90.0f);
    Vector2 side{rawSide.x * ISO_SCALE.x, rawSide.y * ISO_SCALE.y};
    Cosmetics_Draw(cr, skin, {rig.neckHead, fwdHead, side, back});

    // held item front
    if (heldItem && !facingBack) DrawHeldItem(cr);

    if (dragIsYeet && state == DRAG_WINDOW &&
        (dragPhase == DRAG_YEET_WINDUP || dragPhase == DRAG_YEET_FLIGHT)) {
        // Start the word just before impact, punch it well past full size, then
        // let it drift and fade like a cartoon sound effect.
        const double burstAge = g_time - yeetWindupStart - 0.42;
        if (burstAge >= 0.0 && burstAge < 1.05) {
            const float age = (float)burstAge;
            float popScale = 1.0f;
            if (age < 0.13f) {
                const float p = age / 0.13f;
                const float overshoot =
                    1.0f + 2.70158f * std::pow(p - 1.0f, 3.0f) +
                    1.70158f * std::pow(p - 1.0f, 2.0f);
                popScale = Lerp(0.12f, 1.65f, overshoot);
            } else if (age < 0.32f) {
                popScale = Lerp(1.65f, 1.05f, (age - 0.13f) / 0.19f);
            } else {
                popScale = Lerp(1.05f, 0.82f, (age - 0.32f) / 0.73f);
            }

            const float fade =
                age < 0.60f ? 1.0f : 1.0f - (age - 0.60f) / 0.45f;
            const float wobble =
                std::sin(age * 24.0f) * 0.10f * (1.0f - age / 1.05f);
            const Vector2 labelPos =
                rig.neckHead + fwd * 20.0f +
                Vector2{0.0f, -48.0f - age * 34.0f};

            cairo_save(cr);
            cairo_translate(cr, labelPos.x, labelPos.y);
            cairo_rotate(cr, wobble);
            cairo_scale(cr, popScale, popScale);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_ITALIC,
                                   CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 72.0);

            constexpr const char* label = "yeet!";
            cairo_text_extents_t extents{};
            cairo_text_extents(cr, label, &extents);
            cairo_move_to(cr,
                          -(extents.width / 2.0 + extents.x_bearing),
                          -(extents.height / 2.0 + extents.y_bearing));
            cairo_text_path(cr, label);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, fade);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.05, 0.05, 0.05, fade);
            cairo_set_line_width(cr, 5.0);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }
    cairo_restore(cr);
}

// =========================================================
// DRAW PRIMITIVES
// =========================================================

void Goose::DrawEllipse(cairo_t* cr, Vector2 p, int rx, int ry,
                        float r, float g, float b, float a) {
    cairo_save(cr);
    cairo_translate(cr, p.x, p.y);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0, 0, 1.0, 0, 2*M_PI);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_fill(cr);
    cairo_restore(cr);
}

void Goose::DrawLine(cairo_t* cr, Vector2 a, Vector2 b, float width) {
    cairo_set_line_width(cr, width);
    cairo_move_to(cr, a.x, a.y);
    cairo_line_to(cr, b.x, b.y);
    cairo_stroke(cr);
}

void Goose::DrawBentLine(cairo_t* cr, Vector2 a, Vector2 control,
                         Vector2 b, float width) {
    const Vector2 firstControl = a + (control - a) * (2.0f / 3.0f);
    const Vector2 secondControl = b + (control - b) * (2.0f / 3.0f);
    cairo_set_line_width(cr, width);
    cairo_move_to(cr, a.x, a.y);
    cairo_curve_to(cr,
                   firstControl.x, firstControl.y,
                   secondControl.x, secondControl.y,
                   b.x, b.y);
    cairo_stroke(cr);
}

// =========================================================
// AI HELPERS
// =========================================================

void Goose::StartFetch(int w, int h) {
    state = FETCHING;

    // Reduced from 100px to 40px so goose stays partially visible
    int side = rand() % 4;
    switch (side) {
        case 0: target = { -40.0f, (float)(rand() % h) }; break;
        case 1: target = { (float)w + 40.0f, (float)(rand() % h) }; break;
        case 2: target = { (float)(rand() % w), -40.0f }; break;
        case 3: target = { (float)(rand() % w), (float)h + 40.0f }; break;
    }

    // Set a random curvature for this path segment
    parabolicCurvature = ((rand() % 200) - 100) / 100.0f; // -1.0 to 1.0
}

void Goose::PickNewTarget(int w, int h) {
    target.x = (float)(rand() % (std::max(1, w - 200)) + 100);
    target.y = (float)(rand() % (std::max(1, h - 200)) + 100);

    // Set a random curvature for this path segment
    parabolicCurvature = ((rand() % 200) - 100) / 100.0f; // -1.0 to 1.0
}

// =========================================================
// COORDINATE HELPERS
// =========================================================

Vector2 Goose::WorldToDevice(Vector2 worldPos) {
    return pos + (worldPos - pos) * g_config.globalScale;
}

Vector2 Goose::DeviceToWorld(Vector2 devicePos) {
    if (g_config.globalScale < 0.01f) return devicePos;
    return pos + (devicePos - pos) / g_config.globalScale;
}

Vector2 Goose::GetBeakTipWorld() {
    Vector2 rawFwd = Vector2::FromAngleDegrees(dir);
    Vector2 fwd{ rawFwd.x * ISO_SCALE.x, rawFwd.y * ISO_SCALE.y };
    Vector2 beakBase = rig.neckHead + fwd * BEAK_BASE_OFFSET;
    return beakBase + fwd * BEAK_LEN;
}

// =========================================================
// UI FORCE COMMANDS
// =========================================================

void Goose::CancelCurrentBehavior() {
    // A force command or a new grab takes the goose back from the soft body.
    if (softActive) {
        softActive = false;
        softThrown = false;
        softBody.attachedPoint = -1;
        softCursorVel = { 0.0f, 0.0f };
        softTargetPrimed = false;
        softFlightElapsed = 0.0;
        softSettleDeadline = 0.0;
        if (g_heldGooseId == id) g_heldGooseId = -1;
        if (state == HELD || state == FLUNG) state = WANDER;
        ragdoll = RagdollState{};
        heldByGooseId = -1;
    }

    // An interrupted attacker lets its victim go rather than leaving it
    // dangling from a beak that no longer exists.
    ReleaseYeetVictim();

    if (state == DRAG_WINDOW || !dragWindowAddr.empty() ||
        g_windowDragGooseId == id) {
        RestoreDraggedWindowNow(false);
    } else if (heldItem) {
        delete heldItem;
        heldItem = nullptr;
    }
    if (g_cursorGrabberId == id) g_cursorGrabberId = -1;
    dragInit = false;
    forceItemFetch = -1;
    forcedText.clear();
    forcedMemePath.clear();
}

void Goose::RestoreDraggedWindowNow(bool placeAtDestination) {
    HyprlandBackend* backend = dynamic_cast<HyprlandBackend*>(
        g_backendManager.GetActiveBackend());
    if (NiriBackend* nb = dynamic_cast<NiriBackend*>(
            g_backendManager.GetActiveBackend())) {
        if (!dragWindowAddr.empty()) {
            NiriBackend::WindowRect r = nb->FindWindow(dragWindowAddr);
            if (r.valid) {
                int px = placeAtDestination ? dragWindowDestX : dragWindowOrigX;
                int py = placeAtDestination ? dragWindowDestY : dragWindowOrigY;
                nb->SetSize(dragWindowAddr, dragWindowOrigW, dragWindowOrigH);
                nb->MoveFloating(dragWindowAddr, px - r.outX, py - r.outY);
                if (dragWindowWasTiled) nb->SetFloating(dragWindowAddr, false);
            }
        }
        ResetWindowDragState();
        return;
    }
    if (backend && !dragWindowAddr.empty()) {
        HyprlandBackend::Window window =
            backend->GetWindowByAddress(dragWindowAddr);
        if (!window.address.empty()) {
            std::string workspaceTarget =
                std::to_string(dragWindowOrigWorkspaceId);
            for (const auto& workspace : backend->GetWorkspaces()) {
                if (workspace.name == dragWindowOrigWorkspace) {
                    workspaceTarget = dragWindowOrigWorkspace;
                    break;
                }
            }
            backend->MoveWindowToWorkspace(dragWindowAddr, workspaceTarget);
            backend->MoveWindowToExact(
                dragWindowAddr,
                placeAtDestination ? dragWindowDestX : dragWindowOrigX,
                placeAtDestination ? dragWindowDestY : dragWindowOrigY);
            backend->ResizeWindowToExact(
                dragWindowAddr, dragWindowOrigW, dragWindowOrigH);
            if (dragWindowWasTiled && window.floating) {
                backend->ToggleFloating(dragWindowAddr);
            }
            if (!dragOriginalFocusAddr.empty() &&
                !backend->GetWindowByAddress(dragOriginalFocusAddr).address.empty()) {
                backend->FocusWindow(dragOriginalFocusAddr);
            }
        }
    }
    ResetWindowDragState();
}

void Goose::ResetWindowDragState() {
    if (heldItem) {
        delete heldItem;
        heldItem = nullptr;
    }
    if (g_windowDragGooseId == id) {
        g_windowDragGooseId = -1;
        g_suppressOverlayForCapture = false;
    }
    dragInit = false;
    dragWindowAddr.clear();
    dragWindowWasTiled = false;
    dragWindowOrigX = dragWindowOrigY = 0;
    dragWindowOrigW = dragWindowOrigH = 0;
    dragWindowDestX = dragWindowDestY = 0;
    dragImageDestX = dragImageDestY = 0;
    dragWindowStartTime = 0.0;
    dragPhase = DRAG_APPROACH;
    dragPhaseAttempt = 0;
    dragOriginalFocusAddr.clear();
    dragWindowOrigWorkspace.clear();
    dragWindowHiddenWorkspace.clear();
    dragRetileTargetAddr.clear();
    dragWindowOrigWorkspaceId = -1;
    dragWindowMonitorId = -1;
    dragWindowWasFocused = false;
    dragWindowWasHidden = false;
    dragRestoreToDestination = false;
    dragCaptureReadyTime = 0.0;
    dragIsYeet = false;
    yeetPos = {0, 0};
    yeetVel = {0, 0};
    yeetWindupStart = 0.0;
    yeetBounces = 0;
    yeetHeadDrive = -1.0f;
    dragRotVel = 0.0f;
}

void Goose::ForceFetch(int type, int w, int h) {
    if (IsHeld()) return; // held geese do nothing
    CancelCurrentBehavior();
    forceItemFetch = type;
    StartFetch(w, h);
}

void Goose::ForceFetchFortune(int w, int h) {
    if (IsHeld()) return;
    CancelCurrentBehavior();
    forceItemFetch = 2;
    StartFetch(w, h);
}

void Goose::ForceFetchText(const std::string& text, int w, int h) {
    if (IsHeld()) return;
    CancelCurrentBehavior();
    forceItemFetch = 1;
    forcedText = text;
    StartFetch(w, h);
}

void Goose::ForceFetchMeme(const std::string& path, int w, int h) {
    if (IsHeld()) return;
    CancelCurrentBehavior();
    forceItemFetch = 0;
    forcedMemePath = path;
    StartFetch(w, h);
}

void Goose::ForceWander(int w, int h) {
    if (IsHeld()) return;
    CancelCurrentBehavior();
    state = WANDER;
    PickNewTarget(w, h);
}

bool Goose::ForceChase(int w, int h) {
    if (IsHeld()) return false;
    CancelCurrentBehavior();
    // Only chase when a backend can read the cursor and nobody else is snatching.
    // Update()'s CHASE_CURSOR handler resolves the live cursor target next frame.
    if (g_cursorGrabberId == -1 &&
        (g_backendManager.GetActiveBackend()->Caps() & CAP_GET_POS)) {
        state = CHASE_CURSOR;
        return true;
    }
    state = WANDER;
    PickNewTarget(w, h);
    return false;
}

// A goose another attacker is already after is off the table.
static bool IsGooseYeetTarget(int victimId) {
    for (const auto& goose : g_geese) {
        if (goose.yeetVictimId == victimId) return true;
    }
    return false;
}

bool Goose::ForceGooseYeet(int victimId, int w, int h) {
    if (IsHeld()) return false;
    Goose* victim = nullptr;
    float nearest = 0.0f;
    for (auto& other : g_geese) {
        if (other.id == id || (victimId >= 0 && other.id != victimId)) continue;
        if (!other.CanBeGooseYeeted() || IsGooseYeetTarget(other.id)) continue;
        const float d = Vector2::Distance(pos, other.pos);
        if (!victim || d < nearest) { victim = &other; nearest = d; }
    }
    if (!victim) return false;

    CancelCurrentBehavior();
    state = CHASE_GOOSE;
    yeetVictimId = victim->id;
    gooseYeetStart = g_time;
    gooseYeetLaunched = false;
    target = victim->pos;
    UiLogPush("Goose " + std::to_string(id) + " is coming for goose " +
              std::to_string(victim->id));
    return true;
}

void Goose::ReleaseYeetVictim() {
    if (yeetVictimId != -1) {
        if (Goose* victim = GetGooseById(yeetVictimId)) {
            if (victim->heldByGooseId == id) victim->DropFromGoose();
        }
    }
    yeetVictimId = -1;
    gooseYeetLaunched = false;
    if (state == CHASE_GOOSE || state == YEET_GOOSE) {
        yeetHeadDrive = -1.0f;
        state = WANDER;
    }
}

// Any on-screen point whose distance from `from` is at least the configured
// minimum; on a screen too small for that, the farthest corner.
static Vector2 PickGooseYeetLanding(Vector2 from, int w, int h) {
    const float margin = 40.0f;
    const float minX = margin, maxX = std::max(margin, (float)w - margin);
    const float minY = margin, maxY = std::max(margin, (float)h - margin);
    const float minDistance = (float)std::max(0, g_config.gooseYeetMinDistance);
    for (int attempt = 0; attempt < 32; ++attempt) {
        const Vector2 p{ minX + (float)Rand01() * (maxX - minX), minY + (float)Rand01() * (maxY - minY) };
        if (Vector2::Distance(p, from) >= minDistance) return p;
    }
    Vector2 best{ minX, minY };
    for (const Vector2 corner : { Vector2{ minX, minY }, Vector2{ maxX, minY },
                                  Vector2{ minX, maxY }, Vector2{ maxX, maxY } }) {
        if (Vector2::Distance(corner, from) > Vector2::Distance(best, from)) best = corner;
    }
    return best;
}

bool Goose::ForceWindowDrag(int w, int h) {
    return BeginWindowInteraction(w, h, false);
}

bool Goose::ForceWindowYeet(int w, int h) {
    return BeginWindowInteraction(w, h, true);
}

// Shared setup for both window behaviors: pick a visible edge window, snapshot
// its state, reserve it, and walk to its near edge. `yeet` only changes what
// happens once the real window has been hidden behind the screenshot.
bool Goose::BeginWindowInteraction(int w, int h, bool yeet) {
    if (IsHeld()) return false;
    CancelCurrentBehavior();
    CursorBackend* rawBackend = g_backendManager.GetActiveBackend();
    HyprlandBackend* hBackend = dynamic_cast<HyprlandBackend*>(rawBackend);
    bool canControl = hBackend || dynamic_cast<NiriBackend*>(rawBackend);
    if (!canControl || (g_windowDragGooseId != -1 &&
                        g_windowDragGooseId != id)) {
        state = WANDER;
        PickNewTarget(w, h);
        return false;
    }

    g_edgeDetector.SetEnabled(true);
    g_edgeDetector.Tick(rawBackend);

    std::vector<EdgeWindow> candidates;
    for (const auto& edgeWindow : g_edgeDetector.EdgeWindows()) {
        bool reserved = false;
        for (const auto& goose : g_geese) {
            if (goose.id != id &&
                goose.dragWindowAddr == edgeWindow.address) {
                reserved = true;
                break;
            }
        }
        if (!reserved) candidates.push_back(edgeWindow);
    }

    if (candidates.empty()) {
        if (g_windowDragGooseId == id) g_windowDragGooseId = -1;
        state = WANDER;
        PickNewTarget(w, h);
        return false;
    }

    EdgeWindow pick = candidates[rand() % candidates.size()];
    const HyprlandMonitor* mon = nullptr;
    for (const auto& monitor : g_edgeDetector.Monitors()) {
        if (monitor.id == pick.monitorId) {
            mon = &monitor;
            break;
        }
    }
    if (!mon) {
        state = WANDER;
        PickNewTarget(w, h);
        return false;
    }

    dragWindowAddr = pick.address;
    dragWindowWasTiled = !pick.floating;
    dragWindowOrigX = pick.x;
    dragWindowOrigY = pick.y;
    dragWindowOrigW = pick.width;
    dragWindowOrigH = pick.height;
    dragWindowOrigWorkspaceId = pick.workspaceId;
    dragWindowMonitorId = pick.monitorId;
    dragWindowOrigWorkspace = std::to_string(pick.workspaceId);
    dragOriginalFocusAddr.clear();
    dragWindowWasFocused = false;
    dragWindowHiddenWorkspace.clear();
    if (hBackend) {
        // Hyprland restore addresses windows/workspaces by name and juggles a
        // hidden special workspace; niri does none of this.
        for (const auto& workspace : hBackend->GetWorkspaces()) {
            if (workspace.id == pick.workspaceId) {
                dragWindowOrigWorkspace = workspace.name;
                break;
            }
        }
        dragOriginalFocusAddr = hBackend->GetActiveWindowAddress();
        dragWindowWasFocused = dragOriginalFocusAddr == dragWindowAddr;
        dragWindowHiddenWorkspace =
            "special:cppgoose-hidden-" + std::to_string(getpid()) +
            "-" + std::to_string(id);
    }

    int pad = g_config.windowDragEdgePadding;
    int usableLeft = mon->x + mon->reservedLeft + pad;
    int usableTop = mon->y + mon->reservedTop + pad;
    int usableRight = mon->x + mon->width - mon->reservedRight - pad;
    int usableBottom = mon->y + mon->height - mon->reservedBottom - pad;
    int maxX = usableRight - pick.width;
    int maxY = usableBottom - pick.height;
    dragWindowDestX = maxX <= usableLeft
        ? usableLeft
        : usableLeft + rand() % (maxX - usableLeft + 1);
    dragWindowDestY = maxY <= usableTop
        ? usableTop
        : usableTop + rand() % (maxY - usableTop + 1);

    g_windowDragGooseId = id;

    Vector2 windowCenter = {
        (float)(pick.x + pick.width / 2),
        (float)(pick.y + pick.height / 2)
    };
    Vector2 toWindow = Vector2::Normalize(windowCenter - pos);
    if (std::abs(toWindow.x) > std::abs(toWindow.y)) {
        target = {
            toWindow.x > 0 ? (float)pick.x : (float)(pick.x + pick.width),
            windowCenter.y
        };
    } else {
        target = {
            windowCenter.x,
            toWindow.y > 0 ? (float)pick.y : (float)(pick.y + pick.height)
        };
    }

    dragWindowStartTime = g_time;
    dragPhase = DRAG_APPROACH;
    dragPhaseAttempt = 0;
    dragIsYeet = yeet;
    state = DRAG_WINDOW;
    currentSpeed = g_config.baseRunSpeed * 1.1f;
    UiLogPush("Goose " + std::to_string(id) +
              (yeet ? " lining up a yeet on window " : " dragging window ") +
              pick.title);
    return true;
}

// niri window drag/yeet. niri IPC is synchronous and supports absolute
// floating placement, so this needs none of the Hyprland machine's polling,
// float-dance, hidden-workspace juggling, or swapwindow retile.
void Goose::UpdateNiriDrag(NiriBackend* nb, double dt, double time, int w, int h) {
    auto abort = [&](const std::string& why) {
        if (!why.empty()) {
            std::cerr << "[Goose] niri window drag aborted: " << why << '\n';
            UiLogPush("Goose " + std::to_string(id) + " window drag aborted: " + why);
        }
        if (!dragWindowAddr.empty()) {
            NiriBackend::WindowRect r = nb->FindWindow(dragWindowAddr);
            if (r.valid) {
                nb->SetSize(dragWindowAddr, dragWindowOrigW, dragWindowOrigH);
                nb->MoveFloating(dragWindowAddr,
                                 dragWindowOrigX - r.outX, dragWindowOrigY - r.outY);
                if (dragWindowWasTiled) nb->SetFloating(dragWindowAddr, false);
            }
        }
        g_suppressOverlayForCapture = false;
        ResetWindowDragState();
        state = WANDER;
        PickNewTarget(w, h);
    };


    auto TryHonk = [&](double cd, double& lastBucket) {
        if ((time - m_honk.lastAny) < HONK_MIN_GAP) return;
        if ((time - lastBucket) < cd) return;
        g_assets.Honk();
        lastBucket = time;
        m_honk.lastAny = time;
    };
    if (dragWindowAddr.empty()) { abort(""); return; }

    switch (dragPhase) {
    case DRAG_APPROACH: {
        if (!nb->FindWindow(dragWindowAddr).valid) {
            abort("target window disappeared");
            break;
        }
        float dist = Vector2::Distance(pos, target);
        if (dist < 30.0f || (time - dragWindowStartTime) > 2.0) {
            g_suppressOverlayForCapture = true;
            for (const auto& monitor : g_monitors)
                if (monitor.canvas) gtk_widget_queue_draw(monitor.canvas);
            dragWindowStartTime = time;
            dragCaptureReadyTime = time + 0.05;
            dragPhase = DRAG_CAPTURE_WAIT;
        }
        break;
    }

    case DRAG_CAPTURE_WAIT: {
        if (time < dragCaptureReadyTime) break;
        NiriBackend::WindowRect r = nb->FindWindow(dragWindowAddr);
        if (!r.valid) { abort("target window disappeared before capture"); break; }

        WindowCaptureRegion region;
        region.outputName = r.output;
        region.outputX = r.outX;
        region.outputY = r.outY;
        region.x = std::max(0, r.x - r.outX);
        region.y = std::max(0, r.y - r.outY);
        region.width = r.w > 0 ? r.w : dragWindowOrigW;
        region.height = r.h > 0 ? r.h : dragWindowOrigH;

        std::string captureError;
        // ponytail: wlr-screencopy region grab (niri has no toplevel-export),
        // so the image is whatever composited in that rect.
        GdkPixbuf* capture = CaptureWaylandRegion(region, &captureError);
        if (!capture) {
            abort(captureError.empty() ? "window capture failed" : captureError);
            break;
        }

        std::string itemError;
        ItemData* item = g_assets.CreateTransientMemeItem(capture, &itemError);
        g_object_unref(capture);
        if (!item) {
            abort(itemError.empty() ? "captured image normalization failed" : itemError);
            break;
        }
        heldItem = item;
        dragPhase = DRAG_HIDE_WINDOW;
        break;
    }

    case DRAG_HIDE_WINDOW: {
        if (!nb->FindWindow(dragWindowAddr).valid) {
            abort("target window disappeared while hiding");
            break;
        }
        // Float and shove off-screen; the screenshot stands in for the window.
        nb->SetFloating(dragWindowAddr, true);
        nb->MoveFloating(dragWindowAddr, 1000000, 1000000);

        g_suppressOverlayForCapture = false;
        for (const auto& monitor : g_monitors)
            if (monitor.canvas) gtk_widget_queue_draw(monitor.canvas);
        dragInit = false;
        dragPos = GetBeakTipWorld();

        if (dragIsYeet) {
            target = pos;
            yeetWindupStart = time;
            yeetBounces = 0;
            dragPhase = DRAG_YEET_WINDUP;
        } else {
            target = { (float)(dragWindowDestX + dragWindowOrigW / 2),
                       (float)(dragWindowDestY + dragWindowOrigH / 2) };
            dragPhase = DRAG_CARRY;
        }
        break;
    }

    case DRAG_CARRY:
        // Shared arrival code advances to DRAG_RESTORE_WORKSPACE on reach.
        if (!nb->FindWindow(dragWindowAddr).valid) abort("target window closed during carry");
        break;

    case DRAG_YEET_WINDUP: {
        if (!nb->FindWindow(dragWindowAddr).valid) {
            abort("target window closed before the yeet");
            break;
        }
        // ponytail: windup aim mirrors the Hyprland path; duplicated so the
        // Hyprland state machine stays byte-for-byte untouched.
        target = pos;
        Vector2 launchDir = Vector2::Normalize(Vector2{
            (float)(dragWindowOrigX + dragWindowOrigW / 2) - pos.x,
            (float)(dragWindowOrigY + dragWindowOrigH / 2) - pos.y});
        if (!std::isfinite(launchDir.x) || !std::isfinite(launchDir.y) ||
            Vector2::Length(launchDir) < 0.5f) {
            launchDir = Vector2::FromAngleDegrees(dir);
        }
        dir = std::atan2(launchDir.y, launchDir.x) * RAD_TO_DEG;

        const double windup = time - yeetWindupStart;
        const double pullBack = 0.33, snap = 0.55;
        if (windup < pullBack) {
            yeetHeadDrive = Lerp(0.35f, 0.0f, (float)(windup / pullBack));
            break;
        }
        if (windup < snap) {
            yeetHeadDrive = Lerp(0.0f, 1.0f, (float)((windup - pullBack) / (snap - pullBack)));
            break;
        }
        yeetPos = WorldToDevice(GetBeakTipWorld());
        const float launchSpeed = 1100.0f + (float)(rand() % 500);
        const float lift = 650.0f + (float)(rand() % 350);
        yeetVel = { launchDir.x * launchSpeed, launchDir.y * launchSpeed * 0.35f - lift };
        dragRotVel = ((rand() % 2) ? 1.0f : -1.0f) * (7.0f + (float)(rand() % 700) / 100.0f);
        yeetHeadDrive = 1.0f;
        yeetBounces = 0;
        dragPhase = DRAG_YEET_FLIGHT;
        TryHonk(HONK_GENERIC_CD, m_honk.lastGeneric);
        UiLogPush("Goose " + std::to_string(id) + " yeeted a window");
        break;
    }

    case DRAG_YEET_FLIGHT: {
        if (!nb->FindWindow(dragWindowAddr).valid) {
            abort("target window closed mid-flight");
            break;
        }
        target = pos;
        if (time - yeetWindupStart > 0.75) yeetHeadDrive = -1.0f;
        if (!UpdateYeetFlight(dt, w, h)) break;
        SetWindowDestinationFromImageCenter(yeetPos);
        dragRestoreToDestination = true;
        yeetHeadDrive = -1.0f;
        dragPhase = DRAG_RESTORE_WORKSPACE;
        break;
    }

    case DRAG_RESTORE_WORKSPACE:
    default: {
        // One-shot restore: place the (floating) window at the landing point,
        // drop back to tiling if it started tiled.
        NiriBackend::WindowRect r = nb->FindWindow(dragWindowAddr);
        if (r.valid) {
            int px = dragRestoreToDestination ? dragWindowDestX : dragWindowOrigX;
            int py = dragRestoreToDestination ? dragWindowDestY : dragWindowOrigY;
            nb->SetSize(dragWindowAddr, dragWindowOrigW, dragWindowOrigH);
            nb->MoveFloating(dragWindowAddr, px - r.outX, py - r.outY);
            if (dragWindowWasTiled) nb->SetFloating(dragWindowAddr, false);
        }
        const bool wasYeet = dragIsYeet;
        ResetWindowDragState();
        state = WANDER;
        PickNewTarget(w, h);
        TryHonk(HONK_GENERIC_CD, m_honk.lastGeneric);
        UiLogPush("Goose " + std::to_string(id) +
                  (wasYeet ? " finished yeeting a window" : " finished dragging a window"));
        break;
    }
    }
}
