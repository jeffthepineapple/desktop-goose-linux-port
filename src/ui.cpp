// ui.cpp
#include "ui.h"
#include "world.h"
#include "config.h"
#include "assets.h"
#include "goose.h"
#include <algorithm>
#include <iostream>
#include <vector>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <string>
#include <pango/pangocairo.h>
#include <chrono>
#include "cursor_backend.h"

namespace fs = std::filesystem;

// Manual replacement for gdk_cairo_set_source_pixbuf
static void set_source_pixbuf_manual(cairo_t *cr, GdkPixbuf *pixbuf, double x, double y) {
    if (!pixbuf) return;
    gint width = gdk_pixbuf_get_width(pixbuf);
    gint height = gdk_pixbuf_get_height(pixbuf);
    gint stride = gdk_pixbuf_get_rowstride(pixbuf);
    guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

    // GdkPixbuf is typically RGB (3 bytes) or RGBA (4 bytes, non-premultiplied).
    // Cairo RGB24/ARGB32 both expect 4 bytes per pixel.
    cairo_format_t format = CAIRO_FORMAT_ARGB32;
    int minStride = cairo_format_stride_for_width(format, width);
    
    if (stride < minStride) {
        // This is where the "invalid value for stride" crash happens.
        // If we reach here, the pixbuf is 3-bytes per pixel and Cairo won't touch it.
        return;
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        pixels, format, width, height, stride
    );

    cairo_set_source_surface(cr, surface, x, y);
    cairo_surface_destroy(surface);
}

// Optional: keep the debug log bounded so it doesn't grow forever.
static constexpr size_t UI_LOG_MAX = 60;

static void UiLogTrim() {
    while (g_uiLog.size() > UI_LOG_MAX) g_uiLog.pop_front();
}

// UI widget for random-bias toggle
static GtkWidget* g_chkRandomizeBias = nullptr;
static GtkWidget* g_entryGooseName = nullptr;
static GtkWidget* g_labelMudChanceVal = nullptr;
static GtkWidget* g_labelMudLifetimeVal = nullptr;

// Per-goose UI widgets (sliders)
static GtkWidget* g_mouseSlider = nullptr;
static GtkWidget* g_noteSlider = nullptr;
static GtkWidget* g_memeSlider = nullptr;
// Slider value labels
static GtkWidget* g_labelScaleVal = nullptr;
static GtkWidget* g_labelWalkVal = nullptr;
static GtkWidget* g_labelRunVal = nullptr;
static GtkWidget* g_labelMouseVal = nullptr;
static GtkWidget* g_labelNoteVal = nullptr;
static GtkWidget* g_labelMemeVal = nullptr;
static GtkWidget* g_labelSnatchVal = nullptr;

// Extra per-goose controls
static GtkWidget* g_chkGooseMudEnabled = nullptr;
static GtkWidget* g_chkGooseCursorEnabled = nullptr;
static GtkWidget* g_sliderGooseMudChance = nullptr, * g_labelGooseMudChance = nullptr;
static GtkWidget* g_sliderGooseMudLife = nullptr,   * g_labelGooseMudLife = nullptr;
static GtkWidget* g_sliderGooseCursorChance = nullptr, * g_labelGooseCursorChance = nullptr;
static GtkWidget* g_sliderGooseSnatchDur = nullptr,     * g_labelGooseSnatchDur = nullptr;

// Forward decl: used by per-goose UI callbacks below.
static void RefreshSelectedGooseUi();

static void ApplyDefaultsToGoose(Goose* g) {
    if (!g) return;
    g->mudEnabled = g_config.mudEnabled;
    g->mudChance = g_config.mudChance;
    g->mudLifetime = g_config.mudLifetime;
    g->cursorChaseEnabled = g_config.cursorChaseEnabled;
    g->cursorChaseChance = g_config.cursorChaseChance;
    g->snatchDuration = g_config.snatchDuration;
}

static void cb_goose_copy_defaults(GtkButton*, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    ApplyDefaultsToGoose(g);
    RefreshSelectedGooseUi();
}

static void cb_goose_apply_to_all(GtkButton*, gpointer) {
    Goose* src = GetGooseById(g_selectedGooseId);
    if (!src) return;
    for (auto& g : g_geese) {
        g.mudEnabled = src->mudEnabled;
        g.mudChance = src->mudChance;
        g.mudLifetime = src->mudLifetime;
        g.cursorChaseEnabled = src->cursorChaseEnabled;
        g.cursorChaseChance = src->cursorChaseChance;
        g.snatchDuration = src->snatchDuration;
    }
    RefreshSelectedGooseUi();
}

// Selected-goose info label (for a quick read)
static GtkWidget* g_labelSelectedInfo = nullptr;

// Debug overlay options (UI-only; not persisted in config.ini)
static bool g_debugOverlayVerbose = false;
static bool g_debugOverlaySelectedOnly = false;

static void RefreshSelectedGooseUi();

// --- Input region ------------------------------------------------------------
// Make overlay click-through except where geese/items are drawn.
// Now handles drawing relative to the specific monitor this window belongs to.
void UpdateInputRegion(GtkWindow* window, const MonitorInfo& m) {
    cairo_region_t* region = cairo_region_create();

    // Respect multi-monitor toggle for input as well
    if (!g_config.multiMonitorEnabled && (m.x != 0 || m.y != 0)) {
        GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(window));
        if (s) gdk_surface_set_input_region(s, region);
        cairo_region_destroy(region);
        return;
    }

    for (auto& goose : g_geese) {
        // Calculate relative position to this monitor
        float localX = goose.pos.x - m.x;
        float localY = goose.pos.y - m.y;
        
        // Only add to region if it's within/near this monitor's bounds
        if (localX > -100 && localX < m.width + 100 && localY > -100 && localY < m.height + 100) {
            cairo_rectangle_int_t r = { (int)localX - 30, (int)localY - 30, 60, 60 };
            cairo_region_union_rectangle(region, &r);
        }
    }

    for (auto& item : g_droppedItems) {
        float localX = item.pos.x - m.x;
        float localY = item.pos.y - m.y;
        if (localX > -500 && localX < m.width + 500 && localY > -500 && localY < m.height + 500) {
            cairo_rectangle_int_t r = { (int)localX, (int)localY, (int)(item.data->w * g_config.globalScale), (int)(item.data->h * g_config.globalScale) };
            cairo_region_union_rectangle(region, &r);
        }
    }

    GdkSurface* s = gtk_native_get_surface(GTK_NATIVE(window));
    if (s) {
        gdk_surface_set_input_region(s, region);
    }
    cairo_region_destroy(region);
}

// --- Drawing ----------------------------------------------------------------
static void draw_debug_overlay(cairo_t* cr) {
    // Textual debug overlay should only be shown when the Debug toggle is enabled.
    // Visual debug markers are controlled separately by `g_config.debugVisuals`.
    if (!g_config.debugToTerminal) return;

    UiLogTrim();

    cairo_save(cr);

    const int pad = 8;
    const int x = 20;
    const int y = 20;

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc = pango_font_description_from_string("Monospace 11");
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    std::vector<std::string> lines;
    char buf[256];
    CursorBackend* backend = g_backendManager.GetActiveBackend();
    bool canGetPos = backend->Caps() & CAP_GET_POS;
    Vector2 cursor = canGetPos ? backend->GetCursorPos() : Vector2{-1, -1};

    snprintf(buf, sizeof(buf), "Goose Debug | geese:%zu  items:%zu  t:%.2f", g_geese.size(), g_droppedItems.size(), g_time);
    lines.emplace_back(buf);

    snprintf(buf, sizeof(buf), "Config | scale:%.2f  walk:%.0f  run:%.0f  memes:%s  cursorDefaults:%s (%d%%)  snatch:%.1fs",
             g_config.globalScale,
             g_config.baseWalkSpeed,
             g_config.baseRunSpeed,
             g_config.memesEnabled ? "on" : "off",
             g_config.cursorChaseEnabled ? "on" : "off",
             g_config.cursorChaseChance,
             g_config.snatchDuration);
    lines.emplace_back(buf);

    snprintf(buf, sizeof(buf), "Cursor | %s  pos:(%.0f,%.0f)  pred:(%.0f,%.0f)  grabber:%d  selected:%d",
             backend->Name().c_str(),
             cursor.x, cursor.y,
             Goose::GetPredictedCursor().x, Goose::GetPredictedCursor().y,
             g_cursorGrabberId,
             g_selectedGooseId);
    lines.emplace_back(buf);

    const bool selectedOnly = g_debugOverlaySelectedOnly && (GetGooseById(g_selectedGooseId) != nullptr);

    for (auto& goose : g_geese) {
        if (selectedOnly && goose.id != g_selectedGooseId) continue;

        const char* stateStr;
        switch (goose.state) {
            case WANDER: stateStr = "WANDER"; break;
            case FETCHING: stateStr = "FETCHING"; break;
            case RETURNING: stateStr = "RETURNING"; break;
            case CHASE_CURSOR: stateStr = "CHASE"; break;
            case SNATCH_CURSOR: stateStr = "SNATCH"; break;
            default: stateStr = "???"; break;
        }

        const float vmag = (float)std::sqrt(goose.vel.x * goose.vel.x + goose.vel.y * goose.vel.y);
        const char* heldStr = goose.heldItem ? (goose.heldItem->type == ItemData::MEME ? "MEME" : "NOTE") : "none";
        const bool isGrabber = (g_cursorGrabberId == goose.id);
        const float distToTarget = Vector2::Distance(goose.pos, goose.target);

        // Check for NaN/Inf
        const bool posNaN = std::isnan(goose.pos.x) || std::isnan(goose.pos.y);
        const bool velNaN = std::isnan(goose.vel.x) || std::isnan(goose.vel.y);
        const char* warnStr = (posNaN || velNaN) ? " !!NaN!!" : "";

        snprintf(buf, sizeof(buf), "ID %d%s%s | %s  pos:(%.1f,%.1f)  vel:(%.1f,%.1f)=%.1f  dir:%.0f  spd:%.1f  held:%s",
                 goose.id, isGrabber ? "*" : "", warnStr,
                 stateStr,
                 goose.pos.x, goose.pos.y,
                 goose.vel.x, goose.vel.y, vmag,
                 goose.dir,
                 goose.currentSpeed,
                 heldStr);
        lines.emplace_back(buf);

        snprintf(buf, sizeof(buf), "    target:(%.1f,%.1f)  dist:%.1f  biases mouse:%d%% meme:%d%% note:%d%%",
                 goose.target.x, goose.target.y, distToTarget,
                 goose.attackMouseBias, goose.memeFetchBias, goose.noteFetchBias);
        lines.emplace_back(buf);

        if (g_debugOverlayVerbose) {
            // Drag physics
            const float dragVmag = (float)std::sqrt(goose.dragVel.x * goose.dragVel.x + goose.dragVel.y * goose.dragVel.y);
            snprintf(buf, sizeof(buf), "    drag init:%s  pos:(%.1f,%.1f)  vel:(%.1f,%.1f)=%.1f  rot:%.2frad  rotVel:%.2f",
                     goose.dragInit ? "Y" : "N",
                     goose.dragPos.x, goose.dragPos.y,
                     goose.dragVel.x, goose.dragVel.y, dragVmag,
                     goose.dragRot, goose.dragRotVel);
            lines.emplace_back(buf);

            // Rig/body positions
            snprintf(buf, sizeof(buf), "    rig body:(%.1f,%.1f)  neckBase:(%.1f,%.1f)  neckHead:(%.1f,%.1f)  neckLerp:%.2f",
                     goose.rig.body.x, goose.rig.body.y,
                     goose.rig.neckBase.x, goose.rig.neckBase.y,
                     goose.rig.neckHead.x, goose.rig.neckHead.y,
                     goose.rig.neckLerp);
            lines.emplace_back(buf);

            // Feet positions
            snprintf(buf, sizeof(buf), "    feet L:(%.1f,%.1f) move:%s  R:(%.1f,%.1f) move:%s  stepTime:%.2f",
                     goose.rig.lFoot.currentPos.x, goose.rig.lFoot.currentPos.y,
                     goose.rig.lFoot.moveStartTime >= 0 ? "Y" : "N",
                     goose.rig.rFoot.currentPos.x, goose.rig.rFoot.currentPos.y,
                     goose.rig.rFoot.moveStartTime >= 0 ? "Y" : "N",
                     goose.stepTime);
            lines.emplace_back(buf);

            if (goose.state == SNATCH_CURSOR) {
                const double heldFor = g_time - goose.snatchStartTime;
                snprintf(buf, sizeof(buf), "    snatch t:%.2fs  pull:%.0f  off:(%.0f,%.0f)  circle r:%.0f  w:%.2f  a:%.2f",
                         heldFor,
                         goose.snatchPullDistance,
                         goose.snatchOffset.x, goose.snatchOffset.y,
                         goose.snatchRadius,
                         goose.snatchAngularSpeed,
                         goose.snatchAngle);
                lines.emplace_back(buf);
            }
        }
    }

    // Rendering stats
    snprintf(buf, sizeof(buf), "Render | screen:%dx%d  droppedItems:%zu", g_screenWidth, g_screenHeight, g_droppedItems.size());
    lines.emplace_back(buf);

    for (const auto& l : g_uiLog) lines.emplace_back(l);

    int maxw = 0;
    int totalh = 0;
    for (const auto& l : lines) {
        pango_layout_set_text(layout, l.c_str(), -1);
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        if (pw > maxw) maxw = pw;
        totalh += ph;
    }

    int boxW = maxw + pad * 2;
    int boxH = totalh + pad * 2;

    // Clamp to application size with margins
    int maxBoxW = g_screenWidth - 40;
    if (boxW > maxBoxW) boxW = maxBoxW;
    int maxBoxH = g_screenHeight - 40;
    if (boxH > maxBoxH) boxH = maxBoxH;

    // Background
    cairo_set_source_rgba(cr, 0.05, 0.05, 0.05, 0.75);
    cairo_rectangle(cr, x, y, boxW, boxH);
    cairo_fill(cr);

    // Render lines
    int cy = y + pad;
    for (const auto& l : lines) {
        pango_layout_set_text(layout, l.c_str(), -1);
        cairo_move_to(cr, x + pad, cy);
        cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
        pango_cairo_show_layout(cr, layout);
        int pw = 0, ph = 0;
        pango_layout_get_pixel_size(layout, &pw, &ph);
        cy += ph;
        if (cy > y + boxH - pad) break; // prevent overflow
    }

    g_object_unref(layout);
    cairo_restore(cr);
}

void draw_overlay(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
    MonitorInfo* m = (MonitorInfo*)data;
    if (!m || cairo_status(cr) != CAIRO_STATUS_SUCCESS) return;

    // Respect multi-monitor toggle: if disabled, only draw on the primary monitor (0,0)
    if (!g_config.multiMonitorEnabled && (m->x != 0 || m->y != 0)) {
        // Clear surface just in case
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
    }

    cairo_save(cr); 

    // Translation for multi-monitor: move everything by negative monitor offset
    // so world coordinates are drawn correctly in monitor-local window.
    cairo_translate(cr, -m->x, -m->y);

    // Draw mud footprints
    for (auto& fp : g_footprints) {
        double age = g_time - fp.timeSpawned;
        float alpha = 0.5f;
        float life = (fp.lifetime > 0.0f) ? fp.lifetime : g_config.mudLifetime;
        float fadeStart = life * 0.7f;
        float fadeDur = life - fadeStart;
        if (age > fadeStart) alpha = 0.5f * (1.0f - (float)(age - fadeStart) / fadeDur);
        if (alpha <= 0) continue;

        cairo_save(cr);
        cairo_translate(cr, fp.pos.x, fp.pos.y);
        cairo_scale(cr, g_config.globalScale, g_config.globalScale);
        cairo_rotate(cr, fp.dir * G_PI / 180.0);
        
        // Use a brown-ish mud color
        cairo_set_source_rgba(cr, 0.45, 0.25, 0.1, alpha);
        
        // Simple foot shape from three ellipses
        cairo_save(cr);
        cairo_scale(cr, 6, 8);
        cairo_arc(cr, 0, 0, 1.0, 0, 2*G_PI);
        cairo_fill(cr);
        cairo_restore(cr);

        cairo_save(cr);
        cairo_translate(cr, -3, -5);
        cairo_scale(cr, 3, 5);
        cairo_arc(cr, 0, 0, 1.0, 0, 2*G_PI);
        cairo_fill(cr);
        cairo_restore(cr);

        cairo_save(cr);
        cairo_translate(cr, 3, -5);
        cairo_scale(cr, 3, 5);
        cairo_arc(cr, 0, 0, 1.0, 0, 2*G_PI);
        cairo_fill(cr);
        cairo_restore(cr);

        cairo_restore(cr);
    }

    // Draw Dropped Items
    for (auto& item : g_droppedItems) {
        if (!std::isfinite(item.pos.x) || !std::isfinite(item.pos.y) || !std::isfinite(item.rotation)) {
            continue; // Skip corrupted items
        }

        cairo_save(cr);
        float s = g_config.globalScale;
        // Center the coordinate system on the item, applying global scale
        cairo_translate(cr, item.pos.x + item.data->w * 0.5f * s, item.pos.y + item.data->h * 0.5f * s);
        cairo_scale(cr, s, s);
        cairo_rotate(cr, item.rotation);
        
        if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
            std::cerr << "[UI] Cairo error after item transform: " << cairo_status_to_string(cairo_status(cr)) << std::endl;
            cairo_restore(cr);
            continue;
        }

        // Draw from top-left relative to scaled center
        cairo_translate(cr, -item.data->w * 0.5f, -item.data->h * 0.5f);

        if (item.data->type == ItemData::MEME && item.data->pixbuf) {
            set_source_pixbuf_manual(cr, item.data->pixbuf, 0, 0);
            cairo_paint(cr);
        } else if (item.data->type == ItemData::TEXT) {
            cairo_set_source_rgb(cr, 1, 1, 0.85);
            cairo_rectangle(cr, 0, 0, item.data->w, item.data->h);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);

            PangoLayout* layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, item.data->textContent.c_str(), -1);
            pango_layout_set_width(layout, (item.data->w - 10) * PANGO_SCALE);
            cairo_move_to(cr, 5, 5);
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
        }
        cairo_restore(cr);

        // Visual debug for items: draw a point at their center
        if (g_config.debugVisuals) {
            cairo_save(cr);
            Vector2 center = item.pos + Vector2{(float)item.data->w * 0.5f, (float)item.data->h * 0.5f} * s;
            cairo_set_source_rgba(cr, 1, 0.5, 0, 0.8); // Orange point
            cairo_arc(cr, center.x, center.y, 4, 0, G_PI * 2);
            cairo_fill(cr);
            
            // Label for time left
            char timeBuf[16];
            snprintf(timeBuf, sizeof(timeBuf), "%.1fs", 15.0 - (g_time - item.timeDropped));
            cairo_move_to(cr, center.x + 8, center.y + 8);
            PangoLayout* tLayout = pango_cairo_create_layout(cr);
            pango_layout_set_text(tLayout, timeBuf, -1);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
            pango_cairo_show_layout(cr, tLayout);
            g_object_unref(tLayout);
            cairo_restore(cr);
        }
    }

    // Draw Geese
    for (auto& g : g_geese) {
        if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) break;
        g.Draw(cr);

        // Minecraft-style name tag
        if (!g.name.empty()) {
            cairo_save(cr);
            PangoLayout* layout = pango_cairo_create_layout(cr);
            std::string tagText = "[#" + std::to_string(g.id) + "] " + g.name;
            pango_layout_set_text(layout, tagText.c_str(), -1);
            
            PangoFontDescription* desc = pango_font_description_from_string("Sans Bold 10");
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);

            int tw, th;
            pango_layout_get_pixel_size(layout, &tw, &th);

            float tagX = g.pos.x - tw / 2.0f;
            float tagY = g.pos.y - 75.0f * g_config.globalScale; // Position above head

            // Background rectangle (translucent dark)
            cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
            cairo_rectangle(cr, tagX - 4, tagY - 2, tw + 8, th + 4);
            cairo_fill(cr);

            // White text
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_move_to(cr, tagX, tagY);
            pango_cairo_show_layout(cr, layout);

            g_object_unref(layout);
            cairo_restore(cr);
        }

        // Visual debug: highlight all geese when enabled
        if (g_config.debugVisuals) {
            cairo_save(cr);
            
            // 1. Highlight circle around goose
            cairo_set_source_rgba(cr, 1, 1, 0, 0.15);
            cairo_set_line_width(cr, 2.0);
            cairo_arc(cr, g.pos.x, g.pos.y, 40, 0, G_PI * 2);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1, 1, 0, 0.4);
            cairo_stroke(cr);

            // 2. ID label near goose
            char idBuf[16];
            snprintf(idBuf, sizeof(idBuf), "ID %d", g.id);
            cairo_set_source_rgba(cr, 1, 1, 0, 0.9);
            cairo_move_to(cr, g.pos.x + 15, g.pos.y - 25);
            PangoLayout* idLayout = pango_cairo_create_layout(cr);
            pango_layout_set_text(idLayout, idBuf, -1);
            pango_cairo_show_layout(cr, idLayout);
            g_object_unref(idLayout);

            // 3. Target point and line to target
            // Note: Use btPoint if we're in a beak-targeting state, otherwise pos
            Vector2 origin = g.pos;
            if (g.state == FETCHING || g.state == RETURNING || g.state == CHASE_CURSOR) {
                origin = g.WorldToDevice(g.GetBeakTipWorld());
            }

            // Draw line to target (dashed)
            cairo_set_source_rgba(cr, 0, 1, 0, 0.5);
            cairo_set_line_width(cr, 1.5);
            double dashes[] = {4.0, 2.0};
            cairo_set_dash(cr, dashes, 2, 0);
            cairo_move_to(cr, origin.x, origin.y);
            cairo_line_to(cr, g.target.x, g.target.y);
            cairo_stroke(cr);
            cairo_set_dash(cr, NULL, 0, 0); // reset dash

            // Draw target dot
            cairo_set_source_rgba(cr, 0, 1, 0, 0.8);
            if (g.state == RETURNING) cairo_set_source_rgba(cr, 1, 0, 1, 0.8); // Purple for drop point
            if (g.state == FETCHING) cairo_set_source_rgba(cr, 0, 1, 1, 0.8);  // Cyan for fetch point
            
            cairo_arc(cr, g.target.x, g.target.y, 6, 0, G_PI * 2);
            cairo_fill(cr);

            // 4. Threshold circle (Drop Zone / Catch Zone)
            float threshold = std::max(30.0f * g_config.globalScale, 25.0f);
            if (g.state == RETURNING) threshold = std::max(50.0f * g_config.globalScale, 40.0f);
            if (g.state == CHASE_CURSOR) threshold = std::max(22.0f * g_config.globalScale, 15.0f);

            cairo_set_source_rgba(cr, 1, 1, 1, 0.2); // Faint white circle
            cairo_set_line_width(cr, 1.0);
            cairo_arc(cr, g.target.x, g.target.y, threshold, 0, G_PI * 2);
            cairo_stroke(cr);

            // 5. Text info under ID
            const char* stateName = "UNKNOWN";
            switch(g.state) {
                case WANDER: stateName = "WANDER"; break;
                case FETCHING: stateName = "FETCHING"; break;
                case RETURNING: stateName = "RETURNING"; break;
                case CHASE_CURSOR: stateName = "CHASE"; break;
                case SNATCH_CURSOR: stateName = "SNATCH"; break;
            }
            float dist = Vector2::Distance(origin, g.target);
            char infoBuf[64];
            snprintf(infoBuf, sizeof(infoBuf), "%s (d:%.0f/%.0f)", stateName, dist, threshold);
            
            cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
            cairo_move_to(cr, g.pos.x + 15, g.pos.y - 10);
            PangoLayout* infoLayout = pango_cairo_create_layout(cr);
            pango_layout_set_text(infoLayout, infoBuf, -1);
            pango_cairo_show_layout(cr, infoLayout);
            g_object_unref(infoLayout);

            // 6. Draw Beak Tip (btPoint) clearly
            Vector2 bt = g.WorldToDevice(g.GetBeakTipWorld());
            cairo_set_source_rgba(cr, 1, 0.5, 0, 0.8);
            cairo_arc(cr, bt.x, bt.y, 4, 0, G_PI * 2);
            cairo_fill(cr);

            // Enhanced Path Visualization: Predictive Curve Simulation
            Vector2 simPos = origin;
            Vector2 simVel = (Vector2::Length(g.vel) < 1.0f) ? (Vector2::Normalize(g.target - origin) * g.currentSpeed) : g.vel;
            float simDt = 0.1f; // Simulation step
            
            cairo_set_source_rgba(cr, 1, 1, 1, 0.3);
            for (int i = 0; i < 15; i++) {
                Vector2 toTarget = g.target - simPos;
                float d = Vector2::Length(toTarget);
                if (d < 10.0f) break;

                Vector2 desired = Vector2::Normalize(toTarget) * g.currentSpeed;
                Vector2 seek = (desired - simVel) * 2.0f;
                
                Vector2 tangent{ -Vector2::Normalize(simVel).y, Vector2::Normalize(simVel).x };
                float curveFade = std::min(1.0f, d / 200.0f);
                Vector2 curve = tangent * (g.parabolicCurvature * g.currentSpeed * 0.8f * curveFade);
                
                Vector2 force = seek + curve;
                simVel = simVel + force * simDt;
                // Limit speed
                float s = Vector2::Length(simVel);
                if (s > g.currentSpeed) simVel = simVel * (g.currentSpeed / s);
                
                Vector2 nextSimPos = simPos + simVel * simDt;
                cairo_move_to(cr, simPos.x, simPos.y);
                cairo_line_to(cr, nextSimPos.x, nextSimPos.y);
                cairo_stroke(cr);
                
                simPos = nextSimPos;
                
                // Avoid infinite simulation if something goes wrong
                if (Vector2::Distance(simPos, origin) > 2000.0f) break;
            }

            cairo_restore(cr);
        }
    }

    cairo_restore(cr); 

    // Debug overlay (only on the primary monitor window)
    if (m->x == 0 && m->y == 0) {
        draw_debug_overlay(cr);
    }
}

gboolean on_tick(gpointer data) {
    g_time += 1.0 / 60.0;

    for (auto& g : g_geese)
        g.Update(1.0 / 60.0, g_time, g_screenWidth, g_screenHeight);

    g_droppedItems.remove_if([](DroppedItem& i) {
        bool exp = i.isExpired(g_time);
        if (exp) delete i.data;
        return exp;
    });

    g_footprints.remove_if([](Footprint& fp) {
        float life = (fp.lifetime > 0.0f) ? fp.lifetime : g_config.mudLifetime;
        return (g_time - fp.timeSpawned) > life;
    });

    // We pass a window to setup_overlay, but we need to update ALL overlays.
    // However, on_tick is associated with a specific canvas.
    // To keep it simple, we queue draw on all monitors.
    // This is handled by setup_overlay_window which spawns timers or we can use a global signal.
    gtk_widget_queue_draw(GTK_WIDGET(data));
    
    // Find matching MonitorInfo for this canvas to update its input region
    for (auto& mi : g_monitors) {
        // This is a bit hacky but works: find window that contains this canvas
        GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(data));
        if (root && GTK_IS_WINDOW(root)) {
            // Need to match monitor to window. Let's store window in MonitorInfo.
            // Simplified: just update input region for this specific window.
            UpdateInputRegion(GTK_WINDOW(root), mi);
            break; 
        }
    }

    static int tick = 0;
    if ((++tick % 10) == 0) RefreshSelectedGooseUi();
    return G_SOURCE_CONTINUE;
}

// --- Callbacks ---------------------------------------------------------------
void cb_spawn(GtkButton*, gpointer) {
    std::string name = "";
    if (g_entryGooseName) {
        const char* txt = gtk_editable_get_text(GTK_EDITABLE(g_entryGooseName));
        if (txt) name = txt;
        gtk_editable_set_text(GTK_EDITABLE(g_entryGooseName), "");
    }

    if (name.empty()) {
        name = "Goose " + std::to_string(g_nextId);
    }

    g_geese.emplace_back(g_nextId++, name, g_screenWidth, g_screenHeight);
    Goose& g = g_geese.back();
    bool randize = true;
    if (g_chkRandomizeBias) {
        randize = gtk_check_button_get_active(GTK_CHECK_BUTTON(g_chkRandomizeBias));
    }
    if (randize) {
        g.attackMouseBias = rand() % 51; // 0..50 by default
        g.memeFetchBias = rand() % 61;   // 0..60
        g.noteFetchBias = rand() % 41;   // 0..40
    } else {
        g.attackMouseBias = 0;
        g.memeFetchBias = 0;
        g.noteFetchBias = 0;
    }
}
void cb_clear(GtkButton*, gpointer) { g_geese.clear(); g_droppedItems.clear(); g_nextId = 0; }
void cb_clear_log(GtkButton*, gpointer) { g_uiLog.clear(); }

void cb_fetch_meme(GtkButton*, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (g) g->ForceFetch(0, g_screenWidth, g_screenHeight);
}
void cb_fetch_text(GtkButton*, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (g) g->ForceFetch(1, g_screenWidth, g_screenHeight);
}
void cb_fetch_text_custom(GtkButton*, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g || !g_entryNote) return;

    const char* txt = gtk_editable_get_text(GTK_EDITABLE(g_entryNote));
    std::string s = txt ? txt : "";
    if (!s.empty()) g->ForceFetchText(s, g_screenWidth, g_screenHeight);
}

void cb_select_goose(GtkSpinButton* spin, gpointer) {
    g_selectedGooseId = (int)gtk_spin_button_get_value(spin);
    RefreshSelectedGooseUi();
}

static void RefreshSelectedGooseUi() {
    Goose* g = GetGooseById(g_selectedGooseId);
    
    // 1. Update info label
    if (g_labelSelectedInfo) {
        if (!g) {
            gtk_label_set_text(GTK_LABEL(g_labelSelectedInfo), "Selected: (none)");
        } else {
            char nameBuf[128];
            snprintf(nameBuf, sizeof(nameBuf), "Selected: [#%d] \"%s\"", g->id, g->name.c_str());
            gtk_label_set_text(GTK_LABEL(g_labelSelectedInfo), nameBuf);
            
            const char* stateStr;
            switch (g->state) {
                case WANDER: stateStr = "WANDER"; break;
                case FETCHING: stateStr = "FETCHING"; break;
                case RETURNING: stateStr = "RETURNING"; break;
                case CHASE_CURSOR: stateStr = "CHASE"; break;
                case SNATCH_CURSOR: stateStr = "SNATCH"; break;
                default: stateStr = "???"; break;
            }
            const char* heldStr = g->heldItem ? (g->heldItem->type == ItemData::MEME ? "MEME" : "NOTE") : "none";
            char buf[256];
            snprintf(buf, sizeof(buf), "Selected: ID %d | %s | held:%s | pos:(%.0f,%.0f) | cursorGrab:%s",
                     g->id, stateStr, heldStr, g->pos.x, g->pos.y, (g_cursorGrabberId == g->id) ? "Y" : "N");
            gtk_label_set_text(GTK_LABEL(g_labelSelectedInfo), buf);
        }
    }

    // 2. Update sliders
    if (g) {
        if (g_mouseSlider) gtk_range_set_value(GTK_RANGE(g_mouseSlider), g->attackMouseBias);
        if (g_noteSlider) gtk_range_set_value(GTK_RANGE(g_noteSlider), g->noteFetchBias);
        if (g_memeSlider) gtk_range_set_value(GTK_RANGE(g_memeSlider), g->memeFetchBias);
        if (g_labelMouseVal) {
            char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->attackMouseBias);
            gtk_label_set_text(GTK_LABEL(g_labelMouseVal), buf);
        }
        if (g_labelNoteVal) {
            char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->noteFetchBias);
            gtk_label_set_text(GTK_LABEL(g_labelNoteVal), buf);
        }
        if (g_labelMemeVal) {
            char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->memeFetchBias);
            gtk_label_set_text(GTK_LABEL(g_labelMemeVal), buf);
        }

        // Detailed Per-Goose settings
        if (g_chkGooseMudEnabled) gtk_check_button_set_active(GTK_CHECK_BUTTON(g_chkGooseMudEnabled), g->mudEnabled);
        if (g_chkGooseCursorEnabled) gtk_check_button_set_active(GTK_CHECK_BUTTON(g_chkGooseCursorEnabled), g->cursorChaseEnabled);

        if (g_sliderGooseMudChance) gtk_range_set_value(GTK_RANGE(g_sliderGooseMudChance), g->mudChance);
        if (g_labelGooseMudChance) {
            char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->mudChance);
            gtk_label_set_text(GTK_LABEL(g_labelGooseMudChance), buf);
        }

        if (g_sliderGooseMudLife) gtk_range_set_value(GTK_RANGE(g_sliderGooseMudLife), g->mudLifetime);
        if (g_labelGooseMudLife) {
            char buf[32]; snprintf(buf, sizeof(buf), "%.1fs", g->mudLifetime);
            gtk_label_set_text(GTK_LABEL(g_labelGooseMudLife), buf);
        }

        if (g_sliderGooseCursorChance) gtk_range_set_value(GTK_RANGE(g_sliderGooseCursorChance), g->cursorChaseChance);
        if (g_labelGooseCursorChance) {
            char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->cursorChaseChance);
            gtk_label_set_text(GTK_LABEL(g_labelGooseCursorChance), buf);
        }

        if (g_sliderGooseSnatchDur) gtk_range_set_value(GTK_RANGE(g_sliderGooseSnatchDur), g->snatchDuration);
        if (g_labelGooseSnatchDur) {
            char buf[32]; snprintf(buf, sizeof(buf), "%.1fs", g->snatchDuration);
            gtk_label_set_text(GTK_LABEL(g_labelGooseSnatchDur), buf);
        }
    }
}
void cb_action_apply(GtkButton*, gpointer user_data) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;

    GtkWidget* combo = GTK_WIDGET(user_data);
    int idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(combo));

    if (idx == 0) g->ForceWander(g_screenWidth, g_screenHeight);
    else if (idx == 1) g->ForceFetch(0, g_screenWidth, g_screenHeight);
    else if (idx == 2) g->ForceFetch(1, g_screenWidth, g_screenHeight);
}

// Per-goose bias setters
void cb_set_mouse_bias(GtkRange* r, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->attackMouseBias = (int)gtk_range_get_value(r);
    if (g_labelMouseVal) {
        char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->attackMouseBias);
        gtk_label_set_text(GTK_LABEL(g_labelMouseVal), buf);
    }
}

void cb_set_note_bias(GtkRange* r, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->noteFetchBias = (int)gtk_range_get_value(r);
    if (g_labelNoteVal) {
        char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->noteFetchBias);
        gtk_label_set_text(GTK_LABEL(g_labelNoteVal), buf);
    }
}

void cb_set_meme_bias(GtkRange* r, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->memeFetchBias = (int)gtk_range_get_value(r);
    if (g_labelMemeVal) {
        char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->memeFetchBias);
        gtk_label_set_text(GTK_LABEL(g_labelMemeVal), buf);
    }
}

// Per-goose detailed settings
static void cb_goose_mud_toggle(GtkCheckButton* b, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (g) g->mudEnabled = gtk_check_button_get_active(b);
}
static void cb_goose_mud_chance(GtkRange* r, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->mudChance = (int)gtk_range_get_value(r);
    if (g_labelGooseMudChance) {
        char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->mudChance);
        gtk_label_set_text(GTK_LABEL(g_labelGooseMudChance), buf);
    }
}
static void cb_goose_mud_life(GtkRange* r, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->mudLifetime = (float)gtk_range_get_value(r);
    if (g_labelGooseMudLife) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.1fs", g->mudLifetime);
        gtk_label_set_text(GTK_LABEL(g_labelGooseMudLife), buf);
    }
}
static void cb_goose_cursor_toggle(GtkCheckButton* b, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (g) g->cursorChaseEnabled = gtk_check_button_get_active(b);
}
static void cb_goose_cursor_chance(GtkRange* r, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->cursorChaseChance = (int)gtk_range_get_value(r);
    if (g_labelGooseCursorChance) {
        char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g->cursorChaseChance);
        gtk_label_set_text(GTK_LABEL(g_labelGooseCursorChance), buf);
    }
}
static void cb_goose_snatch_dur(GtkRange* r, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->snatchDuration = (float)gtk_range_get_value(r);
    if (g_labelGooseSnatchDur) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.1fs", g->snatchDuration);
        gtk_label_set_text(GTK_LABEL(g_labelGooseSnatchDur), buf);
    }
}

void cb_debug(GtkCheckButton* b, gpointer) {
    bool v = gtk_check_button_get_active(b);
    g_config.debugToTerminal = v;
    g_config.debugVisuals = v;
}
void cb_multi_monitor(GtkCheckButton* b, gpointer) { g_config.multiMonitorEnabled = gtk_check_button_get_active(b); }
void cb_memes(GtkCheckButton* b, gpointer) { g_config.memesEnabled = gtk_check_button_get_active(b); }

void cb_scale(GtkRange* r, gpointer) {
    g_config.globalScale = (float)gtk_range_get_value(r);
    if (g_labelScaleVal) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", g_config.globalScale);
        gtk_label_set_text(GTK_LABEL(g_labelScaleVal), buf);
    }
}

void cb_walk(GtkRange* r, gpointer)  {
    g_config.baseWalkSpeed = (float)gtk_range_get_value(r);
    if (g_labelWalkVal) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", g_config.baseWalkSpeed);
        gtk_label_set_text(GTK_LABEL(g_labelWalkVal), buf);
    }
}

void cb_run(GtkRange* r, gpointer)   {
    g_config.baseRunSpeed = (float)gtk_range_get_value(r);
    if (g_labelRunVal) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", g_config.baseRunSpeed);
        gtk_label_set_text(GTK_LABEL(g_labelRunVal), buf);
    }
}

// Cursor chase/snatch controls
void cb_cursor_toggle(GtkCheckButton* b, gpointer) { g_config.cursorChaseEnabled = gtk_check_button_get_active(b); }
void cb_cursor_chance(GtkSpinButton* spin, gpointer) { g_config.cursorChaseChance = (int)gtk_spin_button_get_value(spin); }
void cb_snatch_duration(GtkRange* r, gpointer) {
    g_config.snatchDuration = (float)gtk_range_get_value(r);
    if (g_labelSnatchVal) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fs", g_config.snatchDuration);
        gtk_label_set_text(GTK_LABEL(g_labelSnatchVal), buf);
    }
}

static void cb_debug_overlay_verbose(GtkCheckButton* b, gpointer) {
    g_debugOverlayVerbose = gtk_check_button_get_active(b);
}

// Global toggle callbacks
void cb_audio(GtkCheckButton* b, gpointer) { g_config.audioEnabled = gtk_check_button_get_active(b); }

// Mud tracking callbacks
void cb_mud_toggle(GtkCheckButton* b, gpointer) { g_config.mudEnabled = gtk_check_button_get_active(b); }
void cb_mud_chance(GtkRange* r, gpointer) {
    g_config.mudChance = (int)gtk_range_get_value(r);
    if (g_labelMudChanceVal) {
        char buf[32]; snprintf(buf, sizeof(buf), "%d%%", g_config.mudChance);
        gtk_label_set_text(GTK_LABEL(g_labelMudChanceVal), buf);
    }
}
void cb_mud_lifetime(GtkRange* r, gpointer) {
    g_config.mudLifetime = (float)gtk_range_get_value(r);
    if (g_labelMudLifetimeVal) {
        char buf[32]; snprintf(buf, sizeof(buf), "%.1fs", g_config.mudLifetime);
        gtk_label_set_text(GTK_LABEL(g_labelMudLifetimeVal), buf);
    }
}

static void cb_debug_overlay_selected_only(GtkCheckButton* b, gpointer) {
    g_debugOverlaySelectedOnly = gtk_check_button_get_active(b);
}

static void cb_reset_biases_selected(GtkButton*, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->attackMouseBias = 0;
    g->noteFetchBias = 0;
    g->memeFetchBias = 0;

    // Reset to global defaults
    ApplyDefaultsToGoose(g);

    RefreshSelectedGooseUi();
}

static void cb_randomize_biases_selected(GtkButton*, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) return;
    g->attackMouseBias = rand() % 101;
    g->noteFetchBias = rand() % 101;
    g->memeFetchBias = rand() % 101;

    // Randomize mud/cursor personalities
    g->mudEnabled = (rand() % 2) == 0;
    g->mudChance = rand() % 101;
    g->mudLifetime = 5.0f + (float)(rand() % 56); // 5..60s

    g->cursorChaseEnabled = (rand() % 2) == 0;
    g->cursorChaseChance = rand() % 26; // 0..25%
    g->snatchDuration = 1.0f + (float)(rand() % 10);

    RefreshSelectedGooseUi();
}
void cb_attack_cursor(GtkButton*, gpointer) {
    Goose* g = GetGooseById(g_selectedGooseId);
    if (!g) {
        if (g_geese.empty()) g_geese.emplace_back(g_nextId++, "", g_screenWidth, g_screenHeight);
        g = &g_geese.front();
    }
    g->state = CHASE_CURSOR;
    CursorBackend* backend = g_backendManager.GetActiveBackend();
    if (backend->Caps() & CAP_GET_POS) {
        Vector2 pos = backend->GetCursorPos();
        if (pos.x >= 0) g->target = g->DeviceToWorld(pos);
    }
}

// When the control panel is closed, just hide it instead of quitting the whole
// application. This lets users close/move the window without terminating the app.
static gboolean cb_control_close(GtkWindow* window, gpointer) {
    // Ask the window's application to quit; this will terminate the program.
    GtkApplication* app = gtk_window_get_application(window);
    if (app) {
        g_application_quit(G_APPLICATION(app));
    } else {
        // Fallback: hide the window if we can't find the app
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    }
    return TRUE; // Stop further handling
}

// --- UI helpers --------------------------------------------------------------
static GtkWidget* make_section(const char* title, GtkWidget** out_box) {
    GtkWidget* frame = gtk_frame_new(title);
    gtk_frame_set_label_align(GTK_FRAME(frame), 0.02f);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    gtk_frame_set_child(GTK_FRAME(frame), box);

    if (out_box) *out_box = box;
    return frame;
}

static GtkWidget* make_scale_row(const char* title, double min, double max, double step,
                                 double initial, GCallback on_changed,
                                 GtkWidget** out_scale, GtkWidget** out_value_label,
                                 const char* initial_value_text) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget* lbl = gtk_label_new(title);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), lbl);

    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, step);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_range_set_value(GTK_RANGE(scale), initial);
    if (on_changed) g_signal_connect(scale, "value-changed", on_changed, NULL);
    gtk_box_append(GTK_BOX(row), scale);

    GtkWidget* val = gtk_label_new(initial_value_text ? initial_value_text : "");
    gtk_widget_set_halign(val, GTK_ALIGN_END);
    gtk_widget_set_size_request(val, 56, -1);
    gtk_box_append(GTK_BOX(row), val);

    if (out_scale) *out_scale = scale;
    if (out_value_label) *out_value_label = val;
    return row;
}

// --- Generic Registry Callbacks ----------------------------------------------
static void cb_generic_toggle(GtkCheckButton* btn, gpointer data) {
    ConfigOption* opt = (ConfigOption*)data;
    if (opt->type == CFG_BOOL) {
        *(bool*)opt->ptr = gtk_check_button_get_active(btn);
        if (opt->onChange) opt->onChange();
    }
}

static void cb_generic_spin(GtkSpinButton* spin, gpointer data) {
    ConfigOption* opt = (ConfigOption*)data;
    if (opt->type == CFG_INT) {
        *(int*)opt->ptr = (int)gtk_spin_button_get_value(spin);
        if (opt->onChange) opt->onChange();
    }
}

void PopulateConfigSection(GtkWidget* container, const char* sectionName) {
    for (auto& opt : g_configRegistry) {
        if (std::string(opt.section) != sectionName) continue;

        if (opt.type == CFG_BOOL) {
            GtkWidget* chk = gtk_check_button_new_with_label(opt.label);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), *(bool*)opt.ptr);
            g_signal_connect(chk, "toggled", G_CALLBACK(cb_generic_toggle), &opt);
            gtk_box_append(GTK_BOX(container), chk);
        }
        else if (opt.type == CFG_FLOAT) {
            char valTxt[32];
            snprintf(valTxt, sizeof(valTxt), "%.1f%s", *(float*)opt.ptr, opt.suffix);
            GtkWidget* scale = nullptr;
            GtkWidget* label = nullptr;
            GtkWidget* row = make_scale_row(opt.label, opt.min, opt.max, opt.step, *(float*)opt.ptr, NULL, &scale, &label, valTxt);
            
            struct Sync { ConfigOption* p; GtkWidget* l; };
            Sync* s = new Sync{&opt, label};
            g_signal_connect(scale, "value-changed", G_CALLBACK(+[](GtkRange* r, gpointer d){
                Sync* sc = (Sync*)d;
                float v = (float)gtk_range_get_value(r);
                *(float*)sc->p->ptr = v;
                if (sc->p->onChange) sc->p->onChange();
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1f%s", v, sc->p->suffix);
                gtk_label_set_text(GTK_LABEL(sc->l), buf);
            }), s);
            gtk_box_append(GTK_BOX(container), row);
        }
        else if (opt.type == CFG_INT) {
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_box_append(GTK_BOX(row), gtk_label_new(opt.label));
            GtkWidget* spin = gtk_spin_button_new_with_range(opt.min, opt.max, opt.step);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), *(int*)opt.ptr);
            g_signal_connect(spin, "value-changed", G_CALLBACK(cb_generic_spin), &opt);
            
            gtk_widget_set_hexpand(spin, TRUE);
            gtk_widget_set_halign(spin, GTK_ALIGN_END);
            gtk_box_append(GTK_BOX(row), spin);
            
            if (opt.suffix && strlen(opt.suffix) > 0) {
                gtk_box_append(GTK_BOX(row), gtk_label_new(opt.suffix));
            }
            gtk_box_append(GTK_BOX(container), row);
        }
        else if (opt.type == CFG_STRING) {
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_box_append(GTK_BOX(row), gtk_label_new(opt.label));
            GtkWidget* entry = gtk_entry_new();
            gtk_editable_set_text(GTK_EDITABLE(entry), ((std::string*)opt.ptr)->c_str());
            
            // Password-like masking for API Key
            if (std::string(opt.label).find("Key") != std::string::npos) {
                gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
            }

            g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable* e, gpointer d){
                ConfigOption* o = (ConfigOption*)d;
                *(std::string*)o->ptr = gtk_editable_get_text(e);
                if (o->onChange) o->onChange();
            }), &opt);

            gtk_box_append(GTK_BOX(row), entry);
            gtk_box_append(GTK_BOX(container), row);
        }
    }
}


// --- Initial Name Prompt -----------------------------------------------------
struct PromptData {
    GtkWidget* dialog;
    GtkWidget* entry;
};

static void cb_initial_prompt_submit(GtkWidget*, gpointer user_data) {
    PromptData* pd = (PromptData*)user_data;
    const char* txt = gtk_editable_get_text(GTK_EDITABLE(pd->entry));
    std::string name = txt ? txt : "";
    
    if (name.empty()) {
        name = "Goose " + std::to_string(g_nextId);
    }

    g_geese.emplace_back(g_nextId++, name, g_screenWidth, g_screenHeight);
    
    gtk_window_destroy(GTK_WINDOW(pd->dialog));
    delete pd;
}

void ShowInitialNamePrompt(GtkApplication* app) {
    GtkWidget* win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "New Goose");
    gtk_window_set_default_size(GTK_WINDOW(win), 280, 120);
    
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 15);
    gtk_widget_set_margin_bottom(box, 15);
    gtk_widget_set_margin_start(box, 15);
    gtk_widget_set_margin_end(box, 15);
    gtk_window_set_child(GTK_WINDOW(win), box);

    gtk_box_append(GTK_BOX(box), gtk_label_new("Enter a name for your first goose:"));
    
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget* btn = gtk_button_new_with_label("Spawn!");
    gtk_widget_set_halign(btn, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), btn);

    PromptData* pd = new PromptData{ win, entry };
    g_signal_connect(btn, "clicked", G_CALLBACK(cb_initial_prompt_submit), pd);
    g_signal_connect(entry, "activate", G_CALLBACK(cb_initial_prompt_submit), pd);

    gtk_window_present(GTK_WINDOW(win));
}

// Size-allocate callback for the control panel root; kept minimal so it
// compiles cleanly and can be extended later to adjust layout on resize.
static void cb_root_size_allocate(GtkWidget* widget, GtkAllocation* allocation, gpointer user_data) {
    (void)user_data;
    // Currently no-op; kept to allow easy extension for dynamic reflow.
    (void)widget; (void)allocation;
}

// --- Control panel -----------------------------------------------------------
void activate_control_panel(GtkApplication* app) {
    GtkWidget* win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Goose Control Panel");
    gtk_window_set_default_size(GTK_WINDOW(win), 360, 680);

    GtkWidget* notebook = gtk_notebook_new();
    gtk_window_set_child(GTK_WINDOW(win), notebook);

    char buf[64];

    // =========================================================
    // PAGE 1: GEESE (Spawning & Selected Goose)
    // =========================================================
    GtkWidget* rootGeese = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* scrollGeese = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrollGeese), rootGeese);
    
    GtkWidget* geeseInner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(geeseInner, 12);
    gtk_widget_set_margin_bottom(geeseInner, 12);
    gtk_widget_set_margin_start(geeseInner, 12);
    gtk_widget_set_margin_end(geeseInner, 12);
    gtk_box_append(GTK_BOX(rootGeese), geeseInner);

    // -- Section: Manage Geese
    GtkWidget* mngBox = nullptr;
    gtk_box_append(GTK_BOX(geeseInner), make_section("Population", &mngBox));
    
    gtk_box_append(GTK_BOX(mngBox), gtk_label_new("New Goose Name:"));
    g_entryGooseName = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_entryGooseName), "Leave blank for random");
    gtk_box_append(GTK_BOX(mngBox), g_entryGooseName);

    GtkWidget* spawnBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* btnSpawn = gtk_button_new_with_label("Spawn Goose");
    g_signal_connect(btnSpawn, "clicked", G_CALLBACK(cb_spawn), NULL);
    gtk_box_append(GTK_BOX(spawnBox), btnSpawn);
    GtkWidget* btnClear = gtk_button_new_with_label("Remove All");
    g_signal_connect(btnClear, "clicked", G_CALLBACK(cb_clear), NULL);
    gtk_box_append(GTK_BOX(spawnBox), btnClear);
    gtk_box_append(GTK_BOX(mngBox), spawnBox);

    g_chkRandomizeBias = gtk_check_button_new_with_label("Randomize tendencies on spawn");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_chkRandomizeBias), TRUE);
    gtk_box_append(GTK_BOX(mngBox), g_chkRandomizeBias);

    // -- Section: Selected Goose
    GtkWidget* selBox = nullptr;
    gtk_box_append(GTK_BOX(geeseInner), make_section("Targeted Goose", &selBox));
    
    GtkWidget* idRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(idRow), gtk_label_new("Goose ID:"));
    GtkWidget* spinGoose = gtk_spin_button_new_with_range(0, 999, 1);
    g_signal_connect(spinGoose, "value-changed", G_CALLBACK(cb_select_goose), NULL);
    gtk_box_append(GTK_BOX(idRow), spinGoose);
    gtk_box_append(GTK_BOX(selBox), idRow);

    g_labelSelectedInfo = gtk_label_new("No goose selected");
    gtk_label_set_wrap(GTK_LABEL(g_labelSelectedInfo), TRUE);
    gtk_box_append(GTK_BOX(selBox), g_labelSelectedInfo);

    gtk_box_append(GTK_BOX(selBox), gtk_label_new("Quick Actions:"));
    GtkWidget* actRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* btnMeme = gtk_button_new_with_label("Fetch Meme");
    g_signal_connect(btnMeme, "clicked", G_CALLBACK(cb_fetch_meme), NULL);
    gtk_box_append(GTK_BOX(actRow), btnMeme);
    GtkWidget* btnNote = gtk_button_new_with_label("Fetch Note");
    g_signal_connect(btnNote, "clicked", G_CALLBACK(cb_fetch_text), NULL);
    gtk_box_append(GTK_BOX(actRow), btnNote);
    gtk_box_append(GTK_BOX(selBox), actRow);

    GtkWidget* btnAtk = gtk_button_new_with_label("Force Cursor Attack");
    g_signal_connect(btnAtk, "clicked", G_CALLBACK(cb_attack_cursor), NULL);
    gtk_box_append(GTK_BOX(selBox), btnAtk);

    gtk_box_append(GTK_BOX(selBox), gtk_label_new("Individual Tendencies:"));
    gtk_box_append(GTK_BOX(selBox), make_scale_row("Attack Mouse", 0, 100, 1, 0, G_CALLBACK(cb_set_mouse_bias), &g_mouseSlider, &g_labelMouseVal, "0%"));
    gtk_box_append(GTK_BOX(selBox), make_scale_row("Fetch Notes", 0, 100, 1, 0, G_CALLBACK(cb_set_note_bias), &g_noteSlider, &g_labelNoteVal, "0%"));
    gtk_box_append(GTK_BOX(selBox), make_scale_row("Fetch Memes", 0, 100, 1, 0, G_CALLBACK(cb_set_meme_bias), &g_memeSlider, &g_labelMemeVal, "0%"));

    gtk_box_append(GTK_BOX(selBox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(selBox), gtk_label_new("Individual Mud Controls:"));
    g_chkGooseMudEnabled = gtk_check_button_new_with_label("Enable Mud for this Goose");
    g_signal_connect(g_chkGooseMudEnabled, "toggled", G_CALLBACK(cb_goose_mud_toggle), NULL);
    gtk_box_append(GTK_BOX(selBox), g_chkGooseMudEnabled);
    gtk_box_append(GTK_BOX(selBox), make_scale_row("Mud Chance", 0, 100, 1, 0, G_CALLBACK(cb_goose_mud_chance), &g_sliderGooseMudChance, &g_labelGooseMudChance, "0%"));
    gtk_box_append(GTK_BOX(selBox), make_scale_row("Mud Lifetime", 5, 120, 1, 0, G_CALLBACK(cb_goose_mud_life), &g_sliderGooseMudLife, &g_labelGooseMudLife, "0s"));

    gtk_box_append(GTK_BOX(selBox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(selBox), gtk_label_new("Individual Cursor Controls:"));
    g_chkGooseCursorEnabled = gtk_check_button_new_with_label("Enable Cursor Chase for this Goose");
    g_signal_connect(g_chkGooseCursorEnabled, "toggled", G_CALLBACK(cb_goose_cursor_toggle), NULL);
    gtk_box_append(GTK_BOX(selBox), g_chkGooseCursorEnabled);
    gtk_box_append(GTK_BOX(selBox), make_scale_row("Base Chance", 0, 100, 1, 0, G_CALLBACK(cb_goose_cursor_chance), &g_sliderGooseCursorChance, &g_labelGooseCursorChance, "0%"));
    gtk_box_append(GTK_BOX(selBox), make_scale_row("Snatch Duration", 0.5, 15, 0.5, 0, G_CALLBACK(cb_goose_snatch_dur), &g_sliderGooseSnatchDur, &g_labelGooseSnatchDur, "0s"));

    GtkWidget* perRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* btnCopyDefaults = gtk_button_new_with_label("Copy Global Defaults");
    g_signal_connect(btnCopyDefaults, "clicked", G_CALLBACK(cb_goose_copy_defaults), NULL);
    gtk_box_append(GTK_BOX(perRow), btnCopyDefaults);
    GtkWidget* btnApplyAll = gtk_button_new_with_label("Apply To All Geese");
    g_signal_connect(btnApplyAll, "clicked", G_CALLBACK(cb_goose_apply_to_all), NULL);
    gtk_box_append(GTK_BOX(perRow), btnApplyAll);
    gtk_box_append(GTK_BOX(selBox), perRow);

    GtkWidget* biasRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* btnRand = gtk_button_new_with_label("Randomize Biases");
    g_signal_connect(btnRand, "clicked", G_CALLBACK(cb_randomize_biases_selected), NULL);
    gtk_box_append(GTK_BOX(biasRow), btnRand);
    GtkWidget* btnReset = gtk_button_new_with_label("Reset Biases");
    g_signal_connect(btnReset, "clicked", G_CALLBACK(cb_reset_biases_selected), NULL);
    gtk_box_append(GTK_BOX(biasRow), btnReset);
    gtk_box_append(GTK_BOX(selBox), biasRow);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrollGeese, gtk_label_new("Geese"));

    // =========================================================
    // PAGE 2: GLOBAL (General Settings & Mud)
    // =========================================================
    GtkWidget* rootGlobal = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* scrollGlobal = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrollGlobal), rootGlobal);
    
    GtkWidget* globalInner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(globalInner, 12);
    gtk_widget_set_margin_bottom(globalInner, 12);
    gtk_widget_set_margin_start(globalInner, 12);
    gtk_widget_set_margin_end(globalInner, 12);
    gtk_box_append(GTK_BOX(rootGlobal), globalInner);

    // -- Sections populated from Registry
    GtkWidget* behBox = nullptr;
    gtk_box_append(GTK_BOX(globalInner), make_section("Goose Behavior", &behBox));
    PopulateConfigSection(behBox, "Behavior");

    GtkWidget* movBox = nullptr;
    gtk_box_append(GTK_BOX(globalInner), make_section("Movement & Scale", &movBox));
    PopulateConfigSection(movBox, "Movement");

    // Mud/Cursor are per-goose now; controls live on the Geese tab.

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scrollGlobal, gtk_label_new("Global"));

    // =========================================================
    // PAGE 3: ENGINE (Diagnostics & Logs)
    // =========================================================
    GtkWidget* rootWhisp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* whInner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(whInner, 12);
    gtk_widget_set_margin_bottom(whInner, 12);
    gtk_widget_set_margin_start(whInner, 12);
    gtk_widget_set_margin_end(whInner, 12);
    gtk_box_append(GTK_BOX(rootWhisp), whInner);

    GtkWidget* dbgBox = nullptr;
    gtk_box_append(GTK_BOX(whInner), make_section("System Diagnostics", &dbgBox));
    PopulateConfigSection(dbgBox, "Debug");

    GtkWidget* btnLog = gtk_button_new_with_label("Clear Runtime Log");
    g_signal_connect(btnLog, "clicked", G_CALLBACK(cb_clear_log), NULL);
    gtk_box_append(GTK_BOX(dbgBox), btnLog);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), rootWhisp, gtk_label_new("Engine"));

    g_signal_connect(win, "close-request", G_CALLBACK(cb_control_close), NULL);
    
    RefreshSelectedGooseUi();
    gtk_window_present(GTK_WINDOW(win));
}

// --- Overlay window ----------------------------------------------------------
void setup_overlay_window(GtkApplication* app) {
    ASSET_ROOT = fs::current_path() / ASSET_ROOT_NAME;
    if (!fs::exists(ASSET_ROOT))
        ASSET_ROOT = fs::canonical("/proc/self/exe").parent_path() / ASSET_ROOT_NAME;

    g_assets.Init();

    // Multi-monitor detection
    GdkDisplay* display = gdk_display_get_default();
    GListModel* monitors = gdk_display_get_monitors(display);
    
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    g_monitors.clear();

    for (unsigned int i = 0; i < g_list_model_get_n_items(monitors); i++) {
        GdkMonitor* monitor = (GdkMonitor*)g_list_model_get_item(monitors, i);
        GdkRectangle geom;
        gdk_monitor_get_geometry(monitor, &geom);
        
        MonitorInfo mi;
        mi.x = geom.x;
        mi.y = geom.y;
        mi.width = geom.width;
        mi.height = geom.height;
        mi.monitor = monitor;
        g_monitors.push_back(mi);

        if (mi.x < minX) minX = mi.x;
        if (mi.y < minY) minY = mi.y;
        if (mi.x + mi.width > maxX) maxX = mi.x + mi.width;
        if (mi.y + mi.height > maxY) maxY = mi.y + mi.height;

        // Create overlay for this monitor
        GtkWindow* overlay = GTK_WINDOW(gtk_application_window_new(app));
        gtk_layer_init_for_window(overlay);
        gtk_layer_set_monitor(overlay, monitor);
        gtk_layer_set_layer(overlay, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_anchor(overlay, GTK_LAYER_SHELL_EDGE_TOP, 1);
        gtk_layer_set_anchor(overlay, GTK_LAYER_SHELL_EDGE_BOTTOM, 1);
        gtk_layer_set_anchor(overlay, GTK_LAYER_SHELL_EDGE_LEFT, 1);
        gtk_layer_set_anchor(overlay, GTK_LAYER_SHELL_EDGE_RIGHT, 1);
        gtk_layer_set_keyboard_mode(overlay, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

        GtkWidget* canvas = gtk_drawing_area_new();
        // Pass the MonitorInfo reference
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(canvas), draw_overlay, &g_monitors.back(), NULL);
        gtk_window_set_child(overlay, canvas);

        g_timeout_add(16, on_tick, canvas);
        gtk_window_present(overlay);
    }

    // Set globally unified screen dimensions.
    // If multi-monitor is disabled, clamp world bounds to the primary monitor
    // so geese/cursor logic won't drift into hidden monitors and crash at edges.
    if (!g_config.multiMonitorEnabled && !g_monitors.empty()) {
        const MonitorInfo& primary = g_monitors.front();
        g_screenWidth = primary.width;
        g_screenHeight = primary.height;
    } else {
        g_screenWidth = maxX;
        g_screenHeight = maxY;
    }
    
    // Global style for transparent windows
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, "window { background: transparent; }");
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css), 800);
}
