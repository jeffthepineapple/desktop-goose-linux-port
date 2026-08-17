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


// ✅ NEW: Predicted cursor for backends that can't read global position (or for Tier 3 drag logic)
// We initialize to center for lack of better info.
static Vector2 s_predictedCursor = { -1.0f, -1.0f };

// tiny helper
static inline double Rand01() { return (double)(rand() % 1000) / 1000.0; }
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

    // --- HELD: the cursor has the goose. It dangles and squirms, but no
    // movement, steering, fetching, chasing or window work is allowed. ---
    if (state == HELD) {
        UpdateRagdoll(dt, time);
        UpdateRig();
        // An indignant honk while struggling, on the normal cooldown.
        if (ragdoll.squirmDrive > 0.8f) TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
        return;
    }
    if (ragdoll.release > 0.002f) {
        // Just dropped: keep the limbs settling while the walk cycle resumes.
        UpdateRagdoll(dt, time);
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

    float tSpeed = (dist > 300 || state == FETCHING || state == CHASE_CURSOR || state == SNATCH_CURSOR || state == RETURNING || state == DRAG_WINDOW)
        ? g_config.baseRunSpeed
        : g_config.baseWalkSpeed;

    currentSpeed = Lerp(currentSpeed, tSpeed, 0.05f);

    Vector2 steerForce{0, 0};

    // 1. SEEK / ARRIVE
    Vector2 desiredVel = moveDir * currentSpeed;
    // Slow down as we arrive
    float arrivalRadius = 50.0f;
    if (dist < arrivalRadius) {
        desiredVel = desiredVel * (dist / arrivalRadius);
    }
    steerForce += (desiredVel - vel) * 2.0f;

    // 2. PARABOLIC DRIFT (Tangential Force)
    // Curvature creates an arc. We use a force perpendicular to current velocity.
    if (Vector2::Length(vel) > 10.0f) {
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
    // Only apply avoidance if NOT fetching (let them reach edge items)
    if (state != FETCHING) {
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

    // Limit and apply force
    float forceMag = Vector2::Length(steerForce);
    if (forceMag > maxForce) {
        steerForce = steerForce * (maxForce / forceMag);
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
    if (Vector2::Length(vel) > 1.0f) {
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
// RAGDOLL (cursor pickup)
// =========================================================
namespace {

// Pixel-space gravity. Tuned so a ~22px body link swings at roughly 1 Hz,
// which reads as "a bird dangling" rather than "a pendulum in a clock".
constexpr float RAG_GRAVITY = 1400.0f;

// One damped, driven pendulum link.
//   ang    - angle from straight-down, radians
//   len    - link length in px (shorter = faster swing)
//   spring - muscle stiffness pulling back to `rest` (0 = pure ragdoll)
//   damp   - velocity damping
// gEff already includes the pseudo-force from the accelerating grab point.
void IntegrateLink(float& ang, float& vel, Vector2 gEff, float len,
                   float spring, float rest, float damp, float limit,
                   float dt) {
    const Vector2 u{ std::sin(ang), std::cos(ang) }; // link direction, ang=0 -> down
    // Torque per unit mass about the pivot: u x gEff.
    const float cross = u.x * gEff.y - u.y * gEff.x;
    float acc = -(cross / std::max(len, 1.0f)) - spring * (ang - rest) - damp * vel;
    vel += acc * dt;
    ang += vel * dt;
    if (ang > limit)  { ang = limit;  if (vel > 0) vel = -vel * 0.35f; }
    if (ang < -limit) { ang = -limit; if (vel < 0) vel = -vel * 0.35f; }
    if (!std::isfinite(ang) || !std::isfinite(vel)) { ang = 0.0f; vel = 0.0f; }
}

} // namespace

float Goose::GrabRadius() const {
    return 34.0f * std::max(0.35f, g_config.globalScale);
}

bool Goose::GrabAt(Vector2 devicePos) {
    if (g_heldGooseId != -1 && g_heldGooseId != id) return false;
    if (Vector2::Distance(devicePos, pos) > GrabRadius()) return false;

    // Drop whatever the goose was in the middle of; a held goose does nothing.
    CancelCurrentBehavior();

    ragdoll = RagdollState{};
    ragdoll.pivot = devicePos;
    ragdoll.grabOffset = pos - devicePos;
    ragdoll.primed = false;
    ragdoll.nextSquirm = g_time + 0.35 + Rand01() * 1.1;
    ragdoll.release = 0.0f;

    vel = {0, 0};
    acceleration = {0, 0};
    currentSpeed = 0.0f;
    state = HELD;
    g_heldGooseId = id;
    UiLogPush("Goose " + std::to_string(id) + " picked up");
    return true;
}

void Goose::MoveGrab(Vector2 devicePos) {
    if (state != HELD) return;
    // Only record the target here; velocity/acceleration are derived in
    // UpdateRagdoll against the fixed frame step. Pointer events arrive at an
    // irregular rate, so differentiating them directly would be pure noise.
    ragdoll.pivot = devicePos;
}

void Goose::ReleaseGrab(int w, int h) {
    if (g_heldGooseId == id) g_heldGooseId = -1;
    if (state != HELD) return;

    ragdoll.release = 1.0f;
    ragdoll.primed = false;
    ragdoll.accel = {0, 0};
    ragdoll.pivotVel = {0, 0};
    vel = {0, 0};
    acceleration = {0, 0};
    currentSpeed = 0.0f;
    // Feet are wherever the dangle left them; snap them under the body so the
    // walk cycle doesn't start by lerping in from a splayed pose.
    rig.lFoot.currentPos = GetFootHome(-90.0f);
    rig.rFoot.currentPos = GetFootHome(90.0f);
    rig.lFoot.moveStartTime = -1.0;
    rig.rFoot.moveStartTime = -1.0;
    state = WANDER;
    PickNewTarget(w, h);
    UiLogPush("Goose " + std::to_string(id) + " dropped");
}

void Goose::UpdateRagdoll(double dt, double time) {
    const float step = (float)std::max(dt, 1e-4);

    if (state == HELD) {
        // Differentiate the pivot on the fixed frame clock, not on the
        // irregular pointer-event clock.
        if (ragdoll.primed) {
            const Vector2 newVel = (ragdoll.pivot - ragdoll.lastPivot) / step;
            Vector2 rawAcc = (newVel - ragdoll.pivotVel) / step;
            // Clamp before smoothing: a single teleporting pointer sample
            // would otherwise fling the whole chain into its angle limits.
            const float mag = Vector2::Length(rawAcc);
            const float kMaxAcc = 26000.0f;
            if (mag > kMaxAcc) rawAcc = rawAcc * (kMaxAcc / mag);
            const float smooth = 0.35f;
            ragdoll.accel = ragdoll.accel * (1.0f - smooth) + rawAcc * smooth;
            ragdoll.pivotVel = newVel;
        } else {
            ragdoll.pivotVel = {0, 0};
            ragdoll.accel = {0, 0};
        }
        ragdoll.lastPivot = ragdoll.pivot;
        ragdoll.primed = true;

        pos = ragdoll.pivot + ragdoll.grabOffset;
    } else {
        // Settling: no drive, just gravity and damping.
        ragdoll.accel = ragdoll.accel * 0.85f;
    }

    // Settle blend after release.
    if (state != HELD) {
        ragdoll.release = std::max(0.0f, ragdoll.release - (float)dt * 2.6f);
    }

    // Effective gravity in the accelerating frame of the grab point.
    const Vector2 gEff{ -ragdoll.accel.x, RAG_GRAVITY - ragdoll.accel.y };

    // Struggle: periodic muscular impulses into the neck and legs.
    if (state == HELD && time >= ragdoll.nextSquirm) {
        const float kick = 6.0f + Rand01() * 9.0f;
        const float sign = (rand() % 2) ? 1.0f : -1.0f;
        ragdoll.neckVel += sign * kick;
        ragdoll.legLVel -= sign * kick * (0.6f + Rand01() * 0.8f);
        ragdoll.legRVel += sign * kick * (0.6f + Rand01() * 0.8f);
        ragdoll.bodyVel += sign * kick * 0.12f;
        ragdoll.squirmDrive = 0.55f + Rand01() * 0.45f;
        ragdoll.nextSquirm = time + 0.5 + Rand01() * 2.4;
    }
    ragdoll.squirmDrive = std::max(0.0f, ragdoll.squirmDrive - (float)dt * 1.4f);

    // Body swings about the grab point; neck and legs hang off the body, so
    // their rest angle is the body's angle carried into their local frame.
    IntegrateLink(ragdoll.bodyAng, ragdoll.bodyVel, gEff, 26.0f,
                  1.5f, 0.0f, 2.4f, 1.15f, step);
    IntegrateLink(ragdoll.neckAng, ragdoll.neckVel, gEff, 15.0f,
                  9.0f, -ragdoll.bodyAng * 0.5f, 3.1f, 0.85f, step);
    IntegrateLink(ragdoll.legLAng, ragdoll.legLVel, gEff, 9.0f,
                  4.0f, -ragdoll.bodyAng, 2.2f, 1.0f, step);
    IntegrateLink(ragdoll.legRAng, ragdoll.legRVel, gEff, 9.0f,
                  4.0f, -ragdoll.bodyAng, 2.2f, 1.0f, step);
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

    // --- Ragdoll pose overlay ---------------------------------------------
    // Head chain droops toward gravity and lags behind the swing; the amount
    // fades out over the settle window after the goose is dropped.
    const float ragAmount = (state == HELD) ? 1.0f
                          : Clamp(ragdoll.release, 0.0f, 1.0f);
    if (ragAmount > 0.002f) {
        // Struggling lifts the head; a limp goose lets it hang forward.
        const float droop = (1.0f - Clamp(ragdoll.squirmDrive, 0.0f, 1.0f)) * 0.85f;
        const float side = (fwd.x >= 0.0f) ? 1.0f : -1.0f;
        ragdoll.headRot = (ragdoll.neckAng + droop * side) * ragAmount;

        const float deg = ragdoll.headRot * RAD_TO_DEG;
        rig.neckHead = rig.neckBase + (rig.neckHead - rig.neckBase).Rotate(deg);
        rig.head1    = rig.neckBase + (rig.head1    - rig.neckBase).Rotate(deg);
        rig.head2    = rig.neckBase + (rig.head2    - rig.neckBase).Rotate(deg);
    } else {
        ragdoll.headRot = 0.0f;
    }

    if (state == HELD) {
        // Legs dangle from the hip instead of stepping.
        const Vector2 hip = rig.underbody;
        const float legLen = 13.0f;
        Vector2 rawSide = Vector2::FromAngleDegrees(dir + 90.0f);
        Vector2 lateral{ rawSide.x * ISO_SCALE.x, rawSide.y * ISO_SCALE.y };
        rig.lFoot.currentPos = hip - lateral * 3.5f +
            Vector2{ std::sin(ragdoll.legLAng), std::cos(ragdoll.legLAng) } * legLen;
        rig.rFoot.currentPos = hip + lateral * 3.5f +
            Vector2{ std::sin(ragdoll.legRAng), std::cos(ragdoll.legRAng) } * legLen;
        rig.lFoot.moveStartTime = -1.0;
        rig.rFoot.moveStartTime = -1.0;
    }
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
        pango_layout_set_text(layout, heldItem->Text().c_str(), -1);
        pango_layout_set_width(layout, (heldItem->w - 10) * PANGO_SCALE);
        cairo_move_to(cr, 5, 5);
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

    // Ragdoll: the whole goose swings about the point the cursor holds. Doing
    // it here means the beak, eyes, cosmetics and carried item all inherit the
    // rotation for free.
    const float ragAmount = (state == HELD) ? 1.0f
                          : Clamp(ragdoll.release, 0.0f, 1.0f);
    const bool ragdolling = ragAmount > 0.002f;
    if (ragdolling) {
        const Vector2 pivot = (state == HELD) ? ragdoll.pivot : pos;
        cairo_translate(cr, pivot.x, pivot.y);
        cairo_rotate(cr, ragdoll.bodyAng * ragAmount);
        cairo_translate(cr, -pivot.x, -pivot.y);
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
    // Lifted geese cast a smaller, fainter shadow.
    const float shadowScale = 1.0f - 0.5f * ragAmount;
    DrawEllipse(cr, pos + Vector2{2, 10}, (int)(20 * shadowScale),
                (int)(15 * shadowScale), 0, 0, 0, 0.3f * (1.0f - 0.55f * ragAmount));

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

    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, OUTLINE_GRAY[0], OUTLINE_GRAY[1], OUTLINE_GRAY[2]);
    DrawLine(cr, bodyFront, bodyBack, 24);
    DrawLine(cr, rig.neckBase, rig.neckHead, 15);
    DrawLine(cr, rig.neckHead, rig.head1, 17);
    DrawLine(cr, rig.head1, rig.head2, 12);
    DrawLine(cr, underFront, underBack, 15);

    // The head chain is rotated by the ragdoll, so the beak has to follow it.
    const Vector2 fwdHead = ragdolling
        ? fwd.Rotate(ragdoll.headRot * RAD_TO_DEG) : fwd;
    Vector2 beakBase = rig.neckHead + fwdHead * BEAK_BASE_OFFSET;
    Vector2 beakTip = beakBase + fwdHead * BEAK_LEN;

    // Draw early so the white fill occludes the beak when facing away.
    cairo_set_source_rgb(cr, ORANGE[0], ORANGE[1], ORANGE[2]);
    DrawLine(cr, beakBase, beakTip, BEAK_WID);

    cairo_set_source_rgb(cr, 1, 1, 1);
    DrawLine(cr, bodyFront, bodyBack, 22);
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
