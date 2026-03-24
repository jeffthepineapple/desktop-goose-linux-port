#include "hyprland.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <filesystem>

static std::string GetSocketPath() {
    const char* his = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!his || !*his) return {};

    // Newer docs prefer $XDG_RUNTIME_DIR/hypr/<HIS>/.socket.sock, but older setups may use /tmp/hypr/...
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string base = (xdg && *xdg) ? std::string(xdg) : std::string("/tmp");

    std::string p1 = base + "/hypr/" + his + "/.socket.sock";
    if (std::filesystem::exists(p1)) return p1;

    // Fallback (common on older configs)
    std::string p2 = std::string("/tmp") + "/hypr/" + his + "/.socket.sock";
    if (std::filesystem::exists(p2)) return p2;

    return p1; // best guess
}

static bool SendHyprCommand(const std::string& cmd, std::string* out) {
    if (out) out->clear();

    const std::string sockPath = GetSocketPath();
    if (sockPath.empty()) return false;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sockPath.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    // Hyprland IPC expects raw text commands (no newline required)
    ssize_t sent = ::send(fd, cmd.c_str(), cmd.size(), 0);
    if (sent < 0) {
        ::close(fd);
        return false;
    }

    // Tell compositor we’re done writing; then read the reply
    ::shutdown(fd, SHUT_WR);

    if (out) {
        char buf[4096];
        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out->append(buf, buf + n);
        }
    } else {
        // Drain quickly anyway
        char buf[256];
        while (::recv(fd, buf, sizeof(buf), 0) > 0) {}
    }

    ::close(fd);
    return true;
}

static bool ExtractJsonNumber(const std::string& s, const char* key, double* outVal) {
    // super small parser: find "key", then ':', then parse number
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    p++;

    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;

    char* end = nullptr;
    const char* start = s.c_str() + p;
    double v = std::strtod(start, &end);
    if (end == start) return false;

    *outVal = v;
    return true;
}

bool HyprlandBackend::isAvailable() {
    const std::string sockPath = GetSocketPath();
    return !sockPath.empty() && std::filesystem::exists(sockPath);
}

bool HyprlandBackend::Init() {
    return isAvailable();
}

Vector2 HyprlandBackend::GetCursorPos() {
    if (!isAvailable()) return {-1.0f, -1.0f};

    std::string resp;
    if (!SendHyprCommand("j/cursorpos", &resp)) return {-1.0f, -1.0f};

    double x = -1, y = -1;
    if (!ExtractJsonNumber(resp, "x", &x) || !ExtractJsonNumber(resp, "y", &y)) {
        return {-1.0f, -1.0f};
    }
    return {(float)x, (float)y};
}

void HyprlandBackend::MoveCursorAbs(int x, int y) {
    if (!isAvailable()) return;

    // Dispatcher: movecursor takes absolute x y
    std::string cmd = "dispatch movecursor " + std::to_string(x) + " " + std::to_string(y);
    (void)SendHyprCommand(cmd, nullptr);
}
