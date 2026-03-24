#pragma once
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>

void activate_control_panel(GtkApplication* app);
void setup_overlay_window(GtkApplication* app);
void UpdateInputRegion(GtkWindow* window);
void draw_overlay(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data);
gboolean on_tick(gpointer data);
void ShowInitialNamePrompt(GtkApplication* app);
