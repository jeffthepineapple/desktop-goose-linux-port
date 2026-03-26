#pragma once
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

void setup_overlay_window(GtkApplication* app);
struct MonitorInfo;
void UpdateInputRegion(GtkWindow* window, const MonitorInfo& m);
void draw_overlay(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data);
gboolean on_tick(gpointer data);
