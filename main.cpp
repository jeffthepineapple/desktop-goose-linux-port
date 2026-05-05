#include <gtk/gtk.h>
#include <glib-unix.h>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <string>
#include <vector>
#include <unistd.h>
#include "app_actions.h"
#include "command_socket.h"
#include "ui.h"
#include "world.h"
#include "config.h"
#include "cursor_backend.h"

static std::string g_initialGooseName;

static void on_activate(GtkApplication* app) {
    static bool initialized = false;
    AppActions_SetApplication(app);

    if (initialized) return;

    Config_InitRegistry();
    setup_overlay_window(app);
    g_backendManager.Init();

    std::string error;
    if (!CommandSocket_StartServer(AppActions_HandleCommand, &error) && !error.empty()) {
        std::cerr << error << std::endl;
    }
    if (!g_initialGooseName.empty()) {
        AppActions_SpawnGoose(g_initialGooseName);
    } else {
        AppActions_EnsureInitialGoose();
    }
    initialized = true;
}

static bool IsControlCommand(const std::string& command) {
    return command == "start" ||
           command == "spawn" ||
           command == "clear" ||
           command == "ram" ||
           command == "status" ||
           command == "quit";
}

static bool IsRunning() {
    std::string response;
    return CommandSocket_Send({"status"}, &response, nullptr);
}

static int DaemonizeProcess() {
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork background process" << std::endl;
        return 1;
    }

    if (pid > 0) {
        std::cout << "Desktop Goose started in background" << std::endl;
        return 0;
    }

    if (setsid() < 0) _exit(1);
    signal(SIGHUP, SIG_IGN); // suppress SIGHUP during second fork; overridden after exec

    pid = fork();
    if (pid < 0) _exit(1);
    if (pid > 0) _exit(0);

    if (chdir("/") != 0) {
        // Keep going even if the cwd cannot be changed.
    }

    const int devNull = open("/dev/null", O_RDWR);
    if (devNull >= 0) {
        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        if (devNull > STDERR_FILENO) close(devNull);
    }

    return -1;
}

static int HandleCliCommand(int argc, char** argv, int* appArgc) {
    if (argc <= 1) {
        if (IsRunning()) {
            std::cout << "Desktop Goose is already running" << std::endl;
            return 0;
        }

        return DaemonizeProcess();
    }

    const std::string command = argv[1];
    if (command == "--foreground") {
        *appArgc = 1;
        return -1;
    }

    if (command == "--help" || command == "help") {
        std::cout
            << "Desktop Goose commands:\n"
            << "  CppGoose\n"
            << "  CppGoose start [name]\n"
            << "  CppGoose start [name] --foreground\n"
            << "  CppGoose spawn [name]\n"
            << "  CppGoose clear\n"
            << "  CppGoose ram\n"
            << "  CppGoose status\n"
            << "  CppGoose quit\n"
            << "\n"
            << "Examples:\n"
            << "  CppGoose start \"Pip\"      Start with a named goose\n"
            << "  CppGoose spawn \"Pip\"      Add a named goose to a running daemon\n";
        return 0;
    }

    if (!IsControlCommand(command)) return -1;

    if (command == "start") {
        bool foreground = false;
        std::string requestedName;

        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--foreground") {
                foreground = true;
            } else if (requestedName.empty()) {
                requestedName = arg;
            } else {
                std::cerr << "Usage: CppGoose start [name] [--foreground]" << std::endl;
                return 1;
            }
        }

        g_initialGooseName = requestedName;

        if (foreground) {
            *appArgc = 1;
            return -1;
        }

        if (IsRunning()) {
            if (!requestedName.empty()) {
                std::string response;
                std::string error;
                if (!CommandSocket_Send({"spawn", requestedName}, &response, &error)) {
                    std::cerr << error << std::endl;
                    return 1;
                }
                if (!response.empty()) std::cout << response;
                return 0;
            }

            std::cout << "Desktop Goose is already running" << std::endl;
            return 0;
        }

        return DaemonizeProcess();
    }

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    std::string response;
    std::string error;
    if (!CommandSocket_Send(args, &response, &error)) {
        std::cerr << error << std::endl;
        return 1;
    }

    if (!response.empty()) std::cout << response;
    return 0;
}

int main(int argc, char** argv) {
    char* runArgv[] = { argv[0], nullptr };
    int runArgc = 1;

    const int cliStatus = HandleCliCommand(argc, argv, &runArgc);
    if (cliStatus >= 0) return cliStatus;

    GtkApplication* app = gtk_application_new("com.goose.wayland", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    auto quit_cb = [](gpointer data) -> gboolean {
        GtkApplication* a = GTK_APPLICATION(data);
        GList* wins = g_list_copy(gtk_application_get_windows(a));
        for (GList* l = wins; l; l = l->next)
            gtk_window_destroy(GTK_WINDOW(l->data));
        g_list_free(wins);
        g_application_quit(G_APPLICATION(data));
        return G_SOURCE_REMOVE;
    };
    for (int sig : { SIGTERM, SIGHUP, SIGINT })
        g_unix_signal_add(sig, quit_cb, app);

    int status = g_application_run(G_APPLICATION(app), runArgc, runArgv);
    CommandSocket_StopServer();
    g_object_unref(app);
    return status;
}
