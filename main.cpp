#include <gtk/gtk.h>
#include "ui.h"
#include "world.h"
#include "config.h"
#include "cursor_backend.h"

static void on_activate(GtkApplication* app) {
    Config_InitRegistry();
    setup_overlay_window(app);
    activate_control_panel(app);
    g_backendManager.Init();
    ShowInitialNamePrompt(app);
}

int main(int argc, char** argv) {
    GtkApplication* app = gtk_application_new("com.goose.wayland", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
