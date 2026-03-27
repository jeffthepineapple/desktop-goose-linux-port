#include "goose.h"
#include "config.h"     // g_config
#include "assets.h"     // g_assets
#include "world.h"      // g_droppedItems
#include "cursor_backend.h" // g_backendManager
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <unordered_map>   // ✅ NEW (honk timing state)

// =========================================================
// CONSTANTS
// =========================================================

static const float OUTLINE_GRAY[] = { 0.82f, 0.82f, 0.82f };
static const float ORANGE[]       = { 1.0f, 0.64f, 0.0f };
// Beak tuning
static const float BEAK_LEN = 12.0f; // much larger beak
static const float BEAK_WID = 16.0f;

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

// per-goose honk state stored here (no header changes)
struct HonkState {
    bool   init = false;
    double nextIdleHonk = 0.0;
    double lastAny = -1e9;
    double lastChase = -1e9;
    double lastFetch = -1e9;
    double lastGeneric = -1e9;
};
static std::unordered_map<int, HonkState> s_honk;

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

    // Default random biases (matches ui.cpp spawns)
    attackMouseBias = rand() % 51; // 0..50
    memeFetchBias = rand() % 61;   // 0..60
    noteFetchBias = rand() % 41;   // 0..40

    // Initialize from global defaults
    cursorChaseEnabled = g_config.cursorChaseEnabled;
    cursorChaseChance  = g_config.cursorChaseChance;
    snatchDuration     = g_config.snatchDuration;
    mudEnabled         = g_config.mudEnabled;
    mudChance          = g_config.mudChance;
    mudLifetime        = g_config.mudLifetime;

    PickNewTarget(screenW, screenH);
}

// =========================================================
// PHYSICS: DRAGGED ITEM SPRING
// =========================================================

void Goose::UpdateDrag(double dt) {
    if (!heldItem) return;

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
// UPDATE (AI + MOVEMENT)
// =========================================================

void Goose::Update(double dt, double time, int w, int h) {

    // ✅ NEW: honk timing init + helper (no header edits)
    HonkState& hs = s_honk[id];
    if (!hs.init) {
        hs.init = true;
        hs.lastAny = -1e9;
        hs.lastChase = -1e9;
        hs.lastFetch = -1e9;
        hs.lastGeneric = -1e9;
        hs.nextIdleHonk = time + HONK_IDLE_MIN + Rand01() * (HONK_IDLE_MAX - HONK_IDLE_MIN);
    }

    auto TryHonk = [&](double cd, double& lastBucket) {
        if ((time - hs.lastAny) < HONK_MIN_GAP) return false;
        if ((time - lastBucket) < cd) return false;
        g_assets.Honk();
        lastBucket = time;
        hs.lastAny = time;
        return true;
    };

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
    if (state == WANDER && time >= hs.nextIdleHonk) {
        // not always; occasional is funnier
        if ((rand() % 3) == 0) {
            // treat as generic bucket
            TryHonk(HONK_GENERIC_CD, hs.lastGeneric);
        }
        hs.nextIdleHonk = time + HONK_IDLE_MIN + Rand01() * (HONK_IDLE_MAX - HONK_IDLE_MIN);
    }

    // --- CHASE_CURSOR: target follows cursor ---
    if (state == CHASE_CURSOR) {
        // If another goose already has the cursor, abort chase immediately.
        if (g_cursorGrabberId != -1 && g_cursorGrabberId != id) {
            state = WANDER;
            PickNewTarget(w, h);
        } else {
            // Use active backend
            CursorBackend* backend = g_backendManager.GetActiveBackend();
            Vector2 cursorPos = backend->GetCursorPos();
            
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
        // Canonical Beak Tip Point Sync: Warp cursor to the exact visual tip.
        // We use std::lround for frame-perfect alignment without "shimmering" or 1px jitter.
        Vector2 bt = WorldToDevice(GetBeakTipWorld());
        // Clamp cursor so we never push into invalid monitor edges when multi-monitor is disabled.
        // (w/h are the active bounds passed in from UI.)
        bt.x = std::clamp(bt.x, 0.0f, (float)std::max(0, w - 1));
        bt.y = std::clamp(bt.y, 0.0f, (float)std::max(0, h - 1));
        g_backendManager.GetActiveBackend()->MoveCursorAbs(std::lround(bt.x), std::lround(bt.y));

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
    } else {
        // FETCHING, RETURNING etc. use visual contact point (Beak Tip in device space)
        // Use a much larger threshold for RETURNING to ensure items get dropped easily
        float threshold = (state == RETURNING) ? std::max(60.0f * g_config.globalScale, 50.0f)
                                               : std::max(30.0f * g_config.globalScale, 25.0f);
        reached = (Vector2::Distance(btPoint, target) < threshold);
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
                    Vector2 cursorPos = g_backendManager.GetActiveBackend()->GetCursorPos();
                    if (cursorPos.x >= 0 && cursorPos.y >= 0) target = cursorPos; // Stay in Device space

                    // ✅ UPDATED: chase honk cooldown
                    TryHonk(HONK_CHASE_CD, hs.lastChase);

                    chased = true;
                }
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
            } else {
                if (forceItemFetch == 0) {
                    heldItem = g_assets.GetRandomMeme();
                } else if (forceItemFetch == 1) {
                    heldItem = g_assets.GetRandomText();
                } else {
                    heldItem = (rand() % 2 == 0) ? g_assets.GetRandomMeme() : g_assets.GetRandomText();
                }
            }

            forceItemFetch = -1;
            forcedText.clear();

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

    float tSpeed = (dist > 300 || state == FETCHING || state == CHASE_CURSOR || state == SNATCH_CURSOR || state == RETURNING)
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
        
        // Multi-monitor aware boundaries
        float bMinX = 0, bMinY = 0, bMaxX = (float)w, bMaxY = (float)h;
        if (!g_config.multiMonitorEnabled && !g_monitors.empty()) {
            for (auto& m : g_monitors) {
                if (m.x == 0 && m.y == 0) {
                    bMaxX = m.width; bMaxY = m.height; break;
                }
            }
        }

        if (probePos.x < bMinX + margin) avoidance.x = currentSpeed;
        else if (probePos.x > bMaxX - margin) avoidance.x = -currentSpeed;
        if (probePos.y < bMinY + margin) avoidance.y = currentSpeed;
        else if (probePos.y > bMaxY - margin) avoidance.y = -currentSpeed;

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
    
    // Multi-monitor aware clamping
    if (!g_config.multiMonitorEnabled && !g_monitors.empty()) {
        for (auto& m : g_monitors) {
            if (m.x == 0 && m.y == 0) {
                maxX = (float)m.width; maxY = (float)m.height; break;
            }
        }
    }

    if (state == FETCHING) {
        minX -= 50.0f; maxX += 50.0f; minY -= 50.0f; maxY += 50.0f;
    } else {
        minX += 20.0f; maxX -= 20.0f; minY += 20.0f; maxY -= 20.0f;
    }
    // Explicit clamping with bounce logic for SNATCH_CURSOR and RETURNING (dragging items)
    // We also include FETCHING here to prevent geese from wandering too far off-screen.
    float bounceWall = 5.0f; // extra nudge to prevent sticking
    if (pos.x < minX) {
        pos.x = minX + 1.0f;
        if ((state == SNATCH_CURSOR || state == RETURNING || state == FETCHING) && vel.x < 0) {
            vel.x = std::abs(vel.x) + 50.0f; // force bounce away
        }
    } else if (pos.x > maxX) {
        pos.x = maxX - 1.0f;
        if ((state == SNATCH_CURSOR || state == RETURNING || state == FETCHING) && vel.x > 0) {
            vel.x = -std::abs(vel.x) - 50.0f;
        }
    }

    if (pos.y < minY) {
        pos.y = minY + 1.0f;
        if ((state == SNATCH_CURSOR || state == RETURNING || state == FETCHING) && vel.y < 0) {
            vel.y = std::abs(vel.y) + 50.0f;
        }
    } else if (pos.y > maxY) {
        pos.y = maxY - 1.0f;
        if ((state == SNATCH_CURSOR || state == RETURNING || state == FETCHING) && vel.y > 0) {
            vel.y = -std::abs(vel.y) - 50.0f;
        }
    }
    CursorBackend* backend = g_backendManager.GetActiveBackend();
    bool canGetPos  = (backend->Caps() & CAP_GET_POS);
    bool canMoveAbs = (backend->Caps() & CAP_MOVE_ABS);
    bool canMoveRel = (backend->Caps() & CAP_MOVE_REL);

    // Sync predicted cursor if we can read the real one
    if (canGetPos) {
        Vector2 realPos = backend->GetCursorPos();
        if (realPos.x >= 0) s_predictedCursor = realPos;
    } else {
        // Initialize if invalid
        if (s_predictedCursor.x < 0) s_predictedCursor = { (float)w/2, (float)h/2 };
    }

    // Smooth rotation
    if (Vector2::Length(vel) > 1.0f) {
        Vector2 curDirVec = Vector2::FromAngleDegrees(dir);
        Vector2 targetDirVec = Vector2::Normalize(vel);

        // Pulling look: face the dragged item or the current cursor anchor.
        if (state == RETURNING) {
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
        heldItem = nullptr;
        state = WANDER;
        
        UiLogPush("Emergency Physics Reset: Goose " + std::to_string(id));
    }
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

    int targetState = (currentSpeed >= 150.0f) ? 1 : 0;
    rig.neckLerp = Lerp(rig.neckLerp, (float)targetState, 0.1f);

    float neckH   = Lerp(20, 10, rig.neckLerp);
    float neckExt = Lerp(3, 16, rig.neckLerp);

    // Separate neck base slightly when facing away
    rig.neckBase = rig.body + fwd * 15.0f + up * (2.0f * back);

    // Key "no-pill" trick: push head slightly forward when facing away
    rig.neckHead = rig.neckBase + fwd * (neckExt + 2.0f * back) + up * neckH;

    // ✅ UPDATED: shorten head (round, original-like), do NOT tie to BEAK_LEN
    rig.head1 = rig.neckHead + fwd * HEAD1_OFFSET;
    rig.head2 = rig.neckHead + fwd * HEAD2_OFFSET;
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

void Goose::DrawEyes(cairo_t* cr, Vector2 fwd) {
    Vector2 rawSide = Vector2::FromAngleDegrees(dir + 90.0f);
    Vector2 side{ rawSide.x * ISO_SCALE.x, rawSide.y * ISO_SCALE.y };
    Vector2 up{ 0, -1 };

    float facing = Dot(Vector2::Normalize(fwd), Vector2{0, 1});
    float back   = Clamp(-facing, 0.0f, 1.0f);

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
    cairo_translate(cr, -heldItem->w / 2, 0);

    if (heldItem->type == ItemData::MEME && heldItem->pixbuf) {
        GdkPixbuf* pb = heldItem->pixbuf;
        int width = gdk_pixbuf_get_width(pb);
        int height = gdk_pixbuf_get_height(pb);
        int stride = gdk_pixbuf_get_rowstride(pb);
        cairo_format_t format = CAIRO_FORMAT_ARGB32;
        int minStride = cairo_format_stride_for_width(format, width);

        if (stride >= minStride) {
            cairo_surface_t *surface = cairo_image_surface_create_for_data(
                gdk_pixbuf_get_pixels(pb),
                format,
                width,
                height,
                stride
            );
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_paint(cr);
            cairo_surface_destroy(surface);
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

    Vector2 rawFwd = Vector2::FromAngleDegrees(dir);
    Vector2 fwd{ rawFwd.x * ISO_SCALE.x, rawFwd.y * ISO_SCALE.y };

    // facing (for item behind/forward)
    float facing = Dot(Vector2::Normalize(fwd), Vector2{0, 1});
    float back = Clamp(-facing, 0.0f, 1.0f);
    bool facingBack = (back > 0.55f);

    // draw held item behind if facing away
    if (heldItem && facingBack) DrawHeldItem(cr);

    // shadow
    DrawEllipse(cr, pos + Vector2{2, 10}, 20, 15, 0,0,0, 0.3f);

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

    // outlines
    DrawLine(cr, bodyFront, bodyBack, 24, OUTLINE_GRAY);
    DrawLine(cr, rig.neckBase, rig.neckHead, 15, OUTLINE_GRAY);
    DrawLine(cr, rig.neckHead, rig.head1, 17, OUTLINE_GRAY);
    DrawLine(cr, rig.head1, rig.head2, 12, OUTLINE_GRAY);
    DrawLine(cr, underFront, underBack, 15, OUTLINE_GRAY);

    // ✅ UPDATED: beak anchored to neckHead (prevents sliding), short + round-cap
    float beakBright = 1.0f; // original goose doesn't really dim; occlusion does the job
    float beakW = std::min(BEAK_WID, 9.0f);   // keep your constant, but clamp to original-ish width

    Vector2 beakBase = rig.neckHead + fwd * BEAK_BASE_OFFSET;
    Vector2 beakTip  = GetBeakTipWorld();

    // draw early so fill can occlude when facing away (your original intent)
    DrawLine(cr, beakBase, beakTip, beakW,
             ORANGE[0] * beakBright, ORANGE[1] * beakBright, ORANGE[2] * beakBright);

    // fill
    DrawLine(cr, bodyFront, bodyBack, 22, 1,1,1);
    DrawLine(cr, rig.neckBase, rig.neckHead, 13, 1,1,1);
    DrawLine(cr, rig.neckHead, rig.head1, 15, 1,1,1);
    DrawLine(cr, rig.head1, rig.head2, 10, 1,1,1);

    cairo_restore(cr); // body squash scope

    // eyes
    DrawEyes(cr, fwd);

    // held item front
    if (heldItem && !facingBack) DrawHeldItem(cr);

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

void Goose::DrawLine(cairo_t* cr, Vector2 a, Vector2 b, float w, const float color[]) {
    DrawLine(cr, a, b, w, color[0], color[1], color[2]);
}

void Goose::DrawLine(cairo_t* cr, Vector2 a, Vector2 b, float w,
                     float r, float g, float bl) {
    cairo_set_line_width(cr, w);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, r, g, bl);
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

void Goose::ForceFetch(int type, int w, int h) {
    forceItemFetch = type;
    StartFetch(w, h);
}

void Goose::ForceFetchText(const std::string& text, int w, int h) {
    forceItemFetch = 1;
    forcedText = text;
    StartFetch(w, h);
}

void Goose::ForceWander(int w, int h) {
    state = WANDER;
    heldItem = nullptr;
    dragInit = false;
    forcedText.clear();
    PickNewTarget(w, h);
}
