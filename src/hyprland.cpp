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

static bool SendHyprCommand(const std::string& sockPath, const std::string& cmd,
                            std::string* out) {
    if (out) out->clear();
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

bool HyprlandBackend::Init() {
    m_socketPath = GetSocketPath();
    return !m_socketPath.empty() && std::filesystem::exists(m_socketPath);
}

bool HyprlandBackend::SendCommand(const std::string& command, std::string* response) {
    if (SendHyprCommand(m_socketPath, command, response)) return true;

    m_socketPath = GetSocketPath();
    return SendHyprCommand(m_socketPath, command, response);
}

Vector2 HyprlandBackend::GetCursorPos() {

    std::string resp;
    if (!SendCommand("j/cursorpos", &resp)) return {-1.0f, -1.0f};

    double x = -1, y = -1;
    if (!ExtractJsonNumber(resp, "x", &x) || !ExtractJsonNumber(resp, "y", &y)) {
        return {-1.0f, -1.0f};
    }
    return {(float)x, (float)y};
}

void HyprlandBackend::MoveCursorAbs(int x, int y) {

    // Dispatcher: movecursor takes absolute x y
    std::string cmd = "dispatch movecursor " + std::to_string(x) + " " + std::to_string(y);
    (void)SendCommand(cmd, nullptr);
}

static bool ExtractJsonInteger(const std::string& s, const char* key, int* outVal) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    p++;

    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;

    char* end = nullptr;
    const char* start = s.c_str() + p;
    long v = std::strtol(start, &end, 10);
    if (end == start) return false;

    *outVal = static_cast<int>(v);
    return true;
}

static bool ExtractJsonString(const std::string& s, const char* key, std::string* outVal) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    p++;

    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;

    if (p >= s.size() || s[p] != '"') return false; // Expecting a string enclosed in quotes
    p++; // Skip the opening quote

    size_t end_p = s.find('"', p);
    if (end_p == std::string::npos) return false;

    *outVal = s.substr(p, end_p - p);
    return true;
}

std::vector<HyprlandBackend::Monitor> HyprlandBackend::GetMonitors() {
    std::vector<Monitor> monitors;
    std::string resp;
    if (!SendCommand("monitors -j", &resp)) {
        return monitors;
    }

    // Very basic JSON array parsing
    size_t pos = 0;
    while ((pos = resp.find("{", pos)) != std::string::npos) {
        size_t end_pos = resp.find("}", pos);
        if (end_pos == std::string::npos) break;

        std::string monitor_json = resp.substr(pos, end_pos - pos + 1);
        Monitor m;
        if (ExtractJsonInteger(monitor_json, "id", &m.id) &&
            ExtractJsonString(monitor_json, "name", &m.name) &&
            ExtractJsonInteger(monitor_json, "x", &m.x) &&
            ExtractJsonInteger(monitor_json, "y", &m.y) &&
            ExtractJsonInteger(monitor_json, "width", &m.width) &&
            ExtractJsonInteger(monitor_json, "height", &m.height) &&
            ExtractJsonNumber(monitor_json, "scale", &m.scale)) {
            monitors.push_back(m);
        }
        pos = end_pos + 1;
    }
    return monitors;
}

std::vector<HyprlandBackend::Window> HyprlandBackend::GetWindows() {
    std::vector<Window> windows;
    std::string resp;
    if (!SendCommand("clients -j", &resp)) {
        return windows;
    }

    // Very basic JSON array parsing
    size_t pos = 0;
    while ((pos = resp.find("{", pos)) != std::string::npos) {
        size_t end_pos = resp.find("}", pos);
        if (end_pos == std::string::npos) break;

        std::string window_json = resp.substr(pos, end_pos - pos + 1);
        Window w;
        if (ExtractJsonString(window_json, "address", &w.address) &&
            ExtractJsonInteger(window_json, "monitor", &w.monitorId) &&
            ExtractJsonString(window_json, "title", &w.title) &&
            ExtractJsonString(window_json, "class", &w.cls)) {
            // Extract 'at' array [x, y]
            size_t at_start = window_json.find("\"at\":[", pos);
            if (at_start != std::string::npos) {
                at_start += std::string("\"at\":[").length();
                size_t at_comma = window_json.find(",", at_start);
                if (at_comma != std::string::npos) {
                    w.x = std::stoi(window_json.substr(at_start, at_comma - at_start));
                    size_t at_end = window_json.find("]", at_comma);
                    if (at_end != std::string::npos) {
                        w.y = std::stoi(window_json.substr(at_comma + 1, at_end - at_comma - 1));
                    }
                }
            }
            // Extract 'size' array [width, height]
            size_t size_start = window_json.find("\"size\":[", pos);
            if (size_start != std::string::npos) {
                size_start += std::string("\"size\":[").length();
                size_t size_comma = window_json.find(",", size_start);
                if (size_comma != std::string::npos) {
                    w.width = std::stoi(window_json.substr(size_start, size_comma - size_start));
                    size_t size_end = window_json.find("]", size_comma);
                    if (size_end != std::string::npos) {
                        w.height = std::stoi(window_json.substr(size_comma + 1, size_end - size_comma - 1));
                    }
                }
            }
            windows.push_back(w);
        }
        pos = end_pos + 1;
    }
    return windows;
}

bool HyprlandBackend::SetWindowBorderColor(const std::string& windowAddress, const std::string& color) {
    // Example: hyprctl keyword windowrule bordercolor 0xff[color_hex_alpha],address:0x[windowAddress]
    // The color string should be in RRGGBBAA format (e.g., FF0000FF for red)
    std::string command = "keyword windowrule bordercolor 0x" + color + ",address:" + windowAddress;
    return SendCommand(command, nullptr);
}

bool HyprlandBackend::ResetWindowBorder(const std::string& windowAddress) {
    // To reset, we can set the border color to a default or remove the rule.
    // Hyprland doesn't have a direct 'remove windowrule' command via 'keyword'.
    // The easiest way is to set it back to the default Hyprland border color.
    // Assuming default is black (000000FF) or using a known default from config.
    // For now, let's assume setting it to 00000000 (transparent) effectively 'removes' it visually.
    // Or set to the default border color of the system/theme.
    // A better approach would be to read the original border color if possible.
    // For simplicity, let's set it to transparent black, effectively hiding the custom border.
    std::string command = "keyword windowrule bordercolor 0x00000000,address:" + windowAddress;
    return SendCommand(command, nullptr);
}
