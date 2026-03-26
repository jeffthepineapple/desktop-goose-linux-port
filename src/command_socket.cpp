#include "command_socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "glib.h"

namespace fs = std::filesystem;

namespace {

struct PendingRequest {
    std::vector<std::string> args;
    std::string response;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

CommandHandler g_commandHandler;
std::thread g_serverThread;
std::atomic<bool> g_serverRunning{false};
int g_serverFd = -1;

std::string EscapeField(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size());
    for (char c : input) {
        if (c == '\\' || c == '\t' || c == '\n') escaped.push_back('\\');
        escaped.push_back(c == '\n' ? 'n' : c);
    }
    return escaped;
}

std::string SerializeArgs(const std::vector<std::string>& args) {
    std::string line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) line.push_back('\t');
        line += EscapeField(args[i]);
    }
    line.push_back('\n');
    return line;
}

std::vector<std::string> ParseArgs(const std::string& line) {
    std::vector<std::string> args;
    std::string current;
    bool escaping = false;

    for (char c : line) {
        if (!escaping) {
            if (c == '\\') {
                escaping = true;
                continue;
            }
            if (c == '\t') {
                args.push_back(current);
                current.clear();
                continue;
            }
            if (c == '\n' || c == '\r') continue;
            current.push_back(c);
            continue;
        }

        current.push_back(c == 'n' ? '\n' : c);
        escaping = false;
    }

    args.push_back(current);
    return args;
}

gboolean HandlePendingRequest(gpointer data) {
    PendingRequest* request = static_cast<PendingRequest*>(data);
    if (g_commandHandler) request->response = g_commandHandler(request->args);
    else request->response = "error command handler unavailable\n";

    {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->done = true;
    }
    request->cv.notify_one();
    return G_SOURCE_REMOVE;
}

void WriteAll(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t rc = write(fd, data.data() + written, data.size() - written);
        if (rc <= 0) return;
        written += (size_t)rc;
    }
}

void HandleClientConnection(int clientFd) {
    std::string requestData;
    char buffer[512];
    while (true) {
        const ssize_t rc = read(clientFd, buffer, sizeof(buffer));
        if (rc <= 0) break;
        requestData.append(buffer, (size_t)rc);
    }

    PendingRequest request;
    request.args = ParseArgs(requestData);
    g_main_context_invoke(nullptr, HandlePendingRequest, &request);

    std::unique_lock<std::mutex> lock(request.mutex);
    request.cv.wait(lock, [&request]() { return request.done; });
    lock.unlock();

    WriteAll(clientFd, request.response);
}

std::string SocketPathForRuntime() {
    if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR")) {
        if (*runtimeDir) return std::string(runtimeDir) + "/desktop-goose.sock";
    }
    return "/tmp/desktop-goose-" + std::to_string(getuid()) + ".sock";
}

} // namespace

std::string CommandSocket_GetPath() {
    return SocketPathForRuntime();
}

bool CommandSocket_StartServer(CommandHandler handler, std::string* errorOut) {
    if (g_serverRunning.load()) return true;

    const std::string socketPath = CommandSocket_GetPath();
    sockaddr_un addr{};
    if (socketPath.size() >= sizeof(addr.sun_path)) {
        if (errorOut) *errorOut = "Socket path is too long";
        return false;
    }

    std::error_code ec;
    if (fs::path(socketPath).has_parent_path()) {
        fs::create_directories(fs::path(socketPath).parent_path(), ec);
    }

    g_serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_serverFd < 0) {
        if (errorOut) *errorOut = "Unable to create command socket";
        return false;
    }

    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    unlink(socketPath.c_str());
    if (bind(g_serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errorOut) *errorOut = "Unable to bind command socket";
        close(g_serverFd);
        g_serverFd = -1;
        return false;
    }

    if (listen(g_serverFd, 8) != 0) {
        if (errorOut) *errorOut = "Unable to listen on command socket";
        close(g_serverFd);
        g_serverFd = -1;
        unlink(socketPath.c_str());
        return false;
    }

    g_commandHandler = std::move(handler);
    g_serverRunning.store(true);
    g_serverThread = std::thread([socketPath]() {
        while (g_serverRunning.load()) {
            const int clientFd = accept(g_serverFd, nullptr, nullptr);
            if (clientFd < 0) {
                if (!g_serverRunning.load()) break;
                continue;
            }

            HandleClientConnection(clientFd);
            close(clientFd);
        }

        unlink(socketPath.c_str());
    });

    return true;
}

void CommandSocket_StopServer() {
    if (!g_serverRunning.exchange(false)) return;

    if (g_serverFd >= 0) {
        close(g_serverFd);
        g_serverFd = -1;
    }

    if (g_serverThread.joinable()) {
        g_serverThread.join();
    }
}

bool CommandSocket_Send(const std::vector<std::string>& args, std::string* responseOut, std::string* errorOut) {
    const std::string socketPath = CommandSocket_GetPath();
    sockaddr_un addr{};
    if (socketPath.size() >= sizeof(addr.sun_path)) {
        if (errorOut) *errorOut = "Socket path is too long";
        return false;
    }

    const int clientFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (clientFd < 0) {
        if (errorOut) *errorOut = "Unable to create client socket";
        return false;
    }

    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(clientFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errorOut) *errorOut = "Desktop Goose is not running";
        close(clientFd);
        return false;
    }

    WriteAll(clientFd, SerializeArgs(args));
    shutdown(clientFd, SHUT_WR);

    std::string response;
    char buffer[512];
    while (true) {
        const ssize_t rc = read(clientFd, buffer, sizeof(buffer));
        if (rc <= 0) break;
        response.append(buffer, (size_t)rc);
    }

    close(clientFd);
    if (responseOut) *responseOut = response;
    return true;
}
