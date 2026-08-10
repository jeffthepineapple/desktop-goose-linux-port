#include "hyprland.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
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

static bool ExtractJsonBool(const std::string& s, const char* key, bool* outVal) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (s.compare(p, 4, "true") == 0) {
        *outVal = true;
        return true;
    }
    if (s.compare(p, 5, "false") == 0) {
        *outVal = false;
        return true;
    }
    return false;
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
    if (!SendCommand("j/monitors", &resp)) {
        std::cerr << "[hyprland] GetMonitors: SendCommand failed\n";
        return monitors;
    }

    // Robust JSON object extraction: find matching braces with nesting
    size_t pos = 0;
    while (true) {
        size_t obj_start = resp.find('{', pos);
        if (obj_start == std::string::npos) break;

        // Find matching closing brace
        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (depth > 0 && obj_end < resp.size()) {
            if (resp[obj_end] == '{') depth++;
            else if (resp[obj_end] == '}') depth--;
            if (depth > 0) obj_end++;
        }
        if (depth != 0) break;

        std::string monitor_json = resp.substr(obj_start, obj_end - obj_start + 1);
        Monitor m;
        if (ExtractJsonInteger(monitor_json, "id", &m.id) &&
            ExtractJsonString(monitor_json, "name", &m.name) &&
            ExtractJsonInteger(monitor_json, "x", &m.x) &&
            ExtractJsonInteger(monitor_json, "y", &m.y) &&
            ExtractJsonInteger(monitor_json, "width", &m.width) &&
            ExtractJsonInteger(monitor_json, "height", &m.height) &&
            ExtractJsonNumber(monitor_json, "scale", &m.scale)) {
            // Parse reserved array: [top, bottom, left, right]
            size_t rp = monitor_json.find("\"reserved\"");
            if (rp != std::string::npos) {
                rp = monitor_json.find('[', rp);
                if (rp != std::string::npos) {
                    ++rp;
                    for (int ri = 0; ri < 4; ++ri) {
                        while (rp < monitor_json.size() &&
                               (monitor_json[rp] == ' ' || monitor_json[rp] == ',' ||
                                monitor_json[rp] == '[' || monitor_json[rp] == '\t'))
                            ++rp;
                        if (rp >= monitor_json.size()) break;
                        char* end = nullptr;
                        long v = std::strtol(monitor_json.c_str() + rp, &end, 10);
                        if (end == monitor_json.c_str() + rp) break;
                        m.reserved[ri] = static_cast<int>(v);
                        rp = end - monitor_json.c_str();
                    }
                }
            }
            size_t active_ws_start = monitor_json.find("\"activeWorkspace\"");
            if (active_ws_start != std::string::npos) {
                size_t active_ws_object = monitor_json.find('{', active_ws_start);
                if (active_ws_object != std::string::npos) {
                    size_t active_ws_end = monitor_json.find('}', active_ws_object);
                    if (active_ws_end != std::string::npos) {
                        std::string active_ws_json =
                            monitor_json.substr(active_ws_object, active_ws_end - active_ws_object + 1);
                        ExtractJsonInteger(active_ws_json, "id", &m.activeWorkspaceId);
                    }
                }
            }
            monitors.push_back(m);
        }
        pos = obj_end + 1;
    }
    return monitors;
}

std::vector<HyprlandBackend::Window> HyprlandBackend::GetWindows() {
    std::vector<Window> windows;
    std::string resp;
    if (!SendCommand("j/clients", &resp)) {
        std::cerr << "[hyprland] GetWindows: SendCommand failed\n";
        return windows;
    }

    // Robust JSON object extraction: find matching braces with nesting
    size_t pos = 0;
    while (true) {
        size_t obj_start = resp.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (depth > 0 && obj_end < resp.size()) {
            if (resp[obj_end] == '{') depth++;
            else if (resp[obj_end] == '}') depth--;
            if (depth > 0) obj_end++;
        }
        if (depth != 0) break;

        std::string window_json = resp.substr(obj_start, obj_end - obj_start + 1);
        Window w;
        if (ExtractJsonString(window_json, "address", &w.address) &&
            ExtractJsonInteger(window_json, "monitor", &w.monitorId) &&
            ExtractJsonString(window_json, "title", &w.title) &&
            ExtractJsonString(window_json, "class", &w.cls)) {
            // Parse workspace id from nested workspace object
            size_t ws_start = window_json.find("\"workspace\"");
            if (ws_start != std::string::npos) {
                size_t ws_obj = window_json.find('{', ws_start);
                if (ws_obj != std::string::npos) {
                    size_t ws_end = window_json.find('}', ws_obj);
                    if (ws_end != std::string::npos) {
                        std::string ws_json = window_json.substr(ws_obj, ws_end - ws_obj + 1);
                        ExtractJsonInteger(ws_json, "id", &w.workspaceId);
                    }
                }
            }
            ExtractJsonBool(window_json, "floating", &w.floating);
            // Extract 'at' array [x, y]
            size_t at_start = window_json.find("\"at\"");
            if (at_start != std::string::npos) {
                at_start = window_json.find('[', at_start);
                if (at_start != std::string::npos) {
                    at_start++;
                    size_t at_comma = window_json.find(',', at_start);
                    if (at_comma != std::string::npos) {
                        w.x = std::stoi(window_json.substr(at_start, at_comma - at_start));
                        size_t at_end = window_json.find(']', at_comma);
                        if (at_end != std::string::npos) {
                            w.y = std::stoi(window_json.substr(at_comma + 1, at_end - at_comma - 1));
                        }
                    }
                }
            }
            // Extract 'size' array [width, height]
            size_t size_start = window_json.find("\"size\"");
            if (size_start != std::string::npos) {
                size_start = window_json.find('[', size_start);
                if (size_start != std::string::npos) {
                    size_start++;
                    size_t size_comma = window_json.find(',', size_start);
                    if (size_comma != std::string::npos) {
                        w.width = std::stoi(window_json.substr(size_start, size_comma - size_start));
                        size_t size_end = window_json.find(']', size_comma);
                        if (size_end != std::string::npos) {
                            w.height = std::stoi(window_json.substr(size_comma + 1, size_end - size_comma - 1));
                        }
                    }
                }
            }
            windows.push_back(w);
        }
        pos = obj_end + 1;
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
    std::string command = "keyword windowrule bordercolor 0x00000000,address:" + windowAddress;
    return SendCommand(command, nullptr);
}

bool HyprlandBackend::MoveWindowTo(const std::string& address, int x, int y) {
    // Get current position, compute delta, then move relatively
    std::vector<Window> wins = GetWindows();
    for (const auto& w : wins) {
        if (w.address == address) {
            int dx = x - w.x;
            int dy = y - w.y;
            if (!FocusWindow(address)) return false;
            std::string cmd = "dispatch movewindow pixel " + std::to_string(dx) + " " + std::to_string(dy);
            return SendCommand(cmd, nullptr);
        }
    }
    return false;
}

bool HyprlandBackend::FocusWindow(const std::string& address) {
    std::string cmd = "dispatch focuswindow address:" + address;
    return SendCommand(cmd, nullptr);
}

bool HyprlandBackend::SendDispatch(const std::string& dispatcher, const std::string& args) {
    std::string cmd = "dispatch " + dispatcher + " " + args;
    return SendCommand(cmd, nullptr);
}


HyprlandBackend::Monitor HyprlandBackend::GetMonitorById(int id) {
    auto monitors = GetMonitors();
    for (auto& m : monitors) {
        if (m.id == id) return m;
    }
    if (!monitors.empty()) return monitors[0];
    return Monitor{};
}

HyprlandBackend::Window HyprlandBackend::GetWindowByAddress(const std::string& address) {
    auto windows = GetWindows();
    for (auto& w : windows) {
        if (w.address == address) return w;
    }
    return Window{};
}

std::string HyprlandBackend::GetActiveWindowAddress() {
    std::string response;
    std::string address;
    if (!SendCommand("j/activewindow", &response) ||
        !ExtractJsonString(response, "address", &address)) {
        return {};
    }
    return address;
}

HyprlandBackend::Workspace HyprlandBackend::GetActiveWorkspace() {
    Workspace ws{};
    std::string resp;
    if (!SendCommand("j/activeworkspace", &resp)) return ws;
    ExtractJsonInteger(resp, "id", &ws.id);
    ExtractJsonString(resp, "name", &ws.name);
    ExtractJsonInteger(resp, "monitorID", &ws.monitorID);
    int specialInt = 0;
    if (ExtractJsonInteger(resp, "special", &specialInt)) ws.special = (specialInt != 0);
    return ws;
}

std::vector<HyprlandBackend::Workspace> HyprlandBackend::GetWorkspaces() {
    std::vector<Workspace> workspaces;
    std::string resp;
    if (!SendCommand("j/workspaces", &resp)) return workspaces;

    size_t pos = 0;
    while (true) {
        size_t obj_start = resp.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (depth > 0 && obj_end < resp.size()) {
            if (resp[obj_end] == '{') depth++;
            else if (resp[obj_end] == '}') depth--;
            if (depth > 0) obj_end++;
        }
        if (depth != 0) break;

        std::string ws_json = resp.substr(obj_start, obj_end - obj_start + 1);
        Workspace ws;
        if (ExtractJsonInteger(ws_json, "id", &ws.id) &&
            ExtractJsonString(ws_json, "name", &ws.name) &&
            ExtractJsonInteger(ws_json, "monitorID", &ws.monitorID)) {
            ExtractJsonBool(ws_json, "special", &ws.special);
            workspaces.push_back(ws);
        }
        pos = obj_end + 1;
    }
    return workspaces;
}

bool HyprlandBackend::MoveWindowToExact(const std::string& address, int x, int y) {
    std::string cmd = "dispatch movewindowpixel exact " + std::to_string(x) + " " + std::to_string(y) + ",address:" + address;
    return SendCommand(cmd, nullptr);
}

bool HyprlandBackend::ResizeWindowToExact(const std::string& address, int w, int h) {
    std::string cmd = "dispatch resizewindowpixel exact " + std::to_string(w) + " " + std::to_string(h) + ",address:" + address;
    return SendCommand(cmd, nullptr);
}

bool HyprlandBackend::ToggleFloating(const std::string& address) {
    return SendDispatch("togglefloating", "address:" + address);
}

bool HyprlandBackend::MoveWindowToWorkspace(const std::string& address, const std::string& workspaceName) {
    std::string cmd = "dispatch movetoworkspacesilent " + workspaceName + ",address:" + address;
    return SendCommand(cmd, nullptr);
}
