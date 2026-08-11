#include "niri.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal, string-aware JSON scanning.
//
// niri emits a single JSON line per reply. Rather than pull in a JSON library,
// we reuse the same lightweight approach as the Hyprland backend, but made
// string-aware so window titles containing braces/brackets/quotes don't corrupt
// structural matching.
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kNpos = std::string::npos;

// Returns the index of the delimiter that closes the one at `open`
// (open/close are e.g. '{'/'}' or '['/']'), skipping over quoted strings.
size_t MatchDelim(const std::string& s, size_t open, char openCh, char closeCh) {
    int depth = 0;
    bool inStr = false;
    for (size_t i = open; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == openCh) {
            ++depth;
        } else if (c == closeCh) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return kNpos;
}

// Collects every top-level `{...}` object substring found within [from, to).
// Works for both JSON arrays (`[{...},{...}]`) and maps (`{"k":{...},...}`),
// since in both cases the immediate children we care about are objects.
std::vector<std::string> CollectObjects(const std::string& s, size_t from, size_t to) {
    std::vector<std::string> out;
    bool inStr = false;
    for (size_t i = from; i < to && i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') {
            size_t e = MatchDelim(s, i, '{', '}');
            if (e == kNpos || e >= to) break;
            out.push_back(s.substr(i, e - i + 1));
            i = e;
        }
    }
    return out;
}

// Finds `"key"` used as an object key and returns the position of the first
// non-whitespace character of its value.
bool FindValue(const std::string& s, const char* key, size_t* valPos) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = 0;
    while ((p = s.find(k, p)) != kNpos) {
        size_t c = p + k.size();
        while (c < s.size() && (s[c] == ' ' || s[c] == '\t')) ++c;
        if (c < s.size() && s[c] == ':') {
            ++c;
            while (c < s.size() && (s[c] == ' ' || s[c] == '\t')) ++c;
            *valPos = c;
            return true;
        }
        p += k.size();
    }
    return false;
}

bool IsNullAt(const std::string& s, size_t p) {
    return s.compare(p, 4, "null") == 0;
}

bool ExtractInt(const std::string& s, const char* key, long* out) {
    size_t p;
    if (!FindValue(s, key, &p) || IsNullAt(s, p)) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str() + p, &end, 10);
    if (end == s.c_str() + p) return false;
    *out = v;
    return true;
}

bool ExtractDouble(const std::string& s, const char* key, double* out) {
    size_t p;
    if (!FindValue(s, key, &p) || IsNullAt(s, p)) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    *out = v;
    return true;
}

bool ExtractBool(const std::string& s, const char* key, bool* out) {
    size_t p;
    if (!FindValue(s, key, &p)) return false;
    if (s.compare(p, 4, "true") == 0) { *out = true; return true; }
    if (s.compare(p, 5, "false") == 0) { *out = false; return true; }
    return false;
}

bool ExtractString(const std::string& s, const char* key, std::string* out) {
    size_t p;
    if (!FindValue(s, key, &p) || IsNullAt(s, p) || p >= s.size() || s[p] != '"') return false;
    ++p; // opening quote
    std::string v;
    for (size_t i = p; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\') { if (i + 1 < s.size()) v.push_back(s[++i]); continue; }
        if (c == '"') { *out = std::move(v); return true; }
        v.push_back(c);
    }
    return false;
}

// Extracts the substring of a nested object value: `"key":{...}` -> `{...}`.
bool ExtractObject(const std::string& s, const char* key, std::string* out) {
    size_t p;
    if (!FindValue(s, key, &p) || p >= s.size() || s[p] != '{') return false;
    size_t e = MatchDelim(s, p, '{', '}');
    if (e == kNpos) return false;
    *out = s.substr(p, e - p + 1);
    return true;
}

// Extracts the first two numbers of a `"key":[a,b,...]` array. Returns false on
// null or absent.
bool ExtractPair(const std::string& s, const char* key, double* a, double* b) {
    size_t p;
    if (!FindValue(s, key, &p) || p >= s.size() || s[p] != '[') return false;
    char* end = nullptr;
    double v0 = std::strtod(s.c_str() + p + 1, &end);
    if (end == s.c_str() + p + 1) return false;
    const char* comma = std::strchr(end, ',');
    if (!comma) return false;
    char* end2 = nullptr;
    double v1 = std::strtod(comma + 1, &end2);
    if (end2 == comma + 1) return false;
    *a = v0;
    *b = v1;
    return true;
}

// Returns the [from, to) bounds of the array value for `"key":[...]`.
bool FindArrayBounds(const std::string& s, const char* key, size_t* from, size_t* to) {
    size_t p;
    if (!FindValue(s, key, &p) || p >= s.size() || s[p] != '[') return false;
    size_t e = MatchDelim(s, p, '[', ']');
    if (e == kNpos) return false;
    *from = p + 1;
    *to = e;
    return true;
}

// Returns the interior bounds of the map value for `"key":{...}`.
bool FindObjectBounds(const std::string& s, const char* key, size_t* from, size_t* to) {
    size_t p;
    if (!FindValue(s, key, &p) || p >= s.size() || s[p] != '{') return false;
    size_t e = MatchDelim(s, p, '{', '}');
    if (e == kNpos) return false;
    *from = p + 1;
    *to = e;
    return true;
}

struct NiriOutput {
    std::string name;
    double x = 0, y = 0, w = 0, h = 0, scale = 1.0;
};

struct NiriWorkspace {
    long id = -1;
    std::string output;
    bool isActive = false;
};

int RoundToInt(double v) { return static_cast<int>(std::lround(v)); }

} // namespace

// ---------------------------------------------------------------------------
// Socket transport.
// ---------------------------------------------------------------------------
bool NiriBackend::Query(const std::string& request, std::string* response) {
    if (response) response->clear();
    if (m_socketPath.empty()) return false;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    // niri expects a JSON request on a single line; flushing and shutting down
    // the write end signals the request is complete.
    std::string line = request + "\n";
    ssize_t sent = ::send(fd, line.c_str(), line.size(), 0);
    if (sent < 0) {
        ::close(fd);
        return false;
    }
    ::shutdown(fd, SHUT_WR);

    if (response) {
        char buf[8192];
        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            response->append(buf, buf + n);
        }
    } else {
        char buf[256];
        while (::recv(fd, buf, sizeof(buf), 0) > 0) {}
    }

    ::close(fd);
    return response ? !response->empty() : true;
}

bool NiriBackend::Init() {
    const char* sock = std::getenv("NIRI_SOCKET");
    if (!sock || !*sock) return false;
    if (!std::filesystem::exists(sock)) return false;
    m_socketPath = sock;

    // Cursor movement relies on wlr-virtual-pointer, which niri implements.
    // Reuse the wlroots virtual-pointer path; if it fails we are not usable.
    if (!WlrootsBackend::Init()) {
        std::cerr << "NiriBackend: niri detected but wlr-virtual-pointer is unavailable." << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Window/monitor awareness via niri IPC.
// ---------------------------------------------------------------------------
namespace {

// Fetches enabled outputs sorted by name so a given output maps to a stable
// synthetic monitor id across separate IPC calls (niri serializes outputs as an
// unordered map).
std::vector<NiriOutput> ParseOutputs(const std::string& resp) {
    std::vector<NiriOutput> outs;
    size_t from, to;
    if (!FindObjectBounds(resp, "Outputs", &from, &to)) return outs;

    for (const std::string& obj : CollectObjects(resp, from, to)) {
        NiriOutput o;
        std::string logical;
        if (!ExtractString(obj, "name", &o.name)) continue;
        if (!ExtractObject(obj, "logical", &logical)) continue; // disabled output
        ExtractDouble(logical, "x", &o.x);
        ExtractDouble(logical, "y", &o.y);
        ExtractDouble(logical, "width", &o.w);
        ExtractDouble(logical, "height", &o.h);
        ExtractDouble(logical, "scale", &o.scale);
        outs.push_back(std::move(o));
    }
    std::sort(outs.begin(), outs.end(),
              [](const NiriOutput& a, const NiriOutput& b) { return a.name < b.name; });
    return outs;
}

std::vector<NiriWorkspace> ParseWorkspaces(const std::string& resp) {
    std::vector<NiriWorkspace> wss;
    size_t from, to;
    if (!FindArrayBounds(resp, "Workspaces", &from, &to)) return wss;
    for (const std::string& obj : CollectObjects(resp, from, to)) {
        NiriWorkspace ws;
        if (!ExtractInt(obj, "id", &ws.id)) continue;
        ExtractString(obj, "output", &ws.output);
        ExtractBool(obj, "is_active", &ws.isActive);
        wss.push_back(std::move(ws));
    }
    return wss;
}

} // namespace

std::vector<BackendMonitor> NiriBackend::GetWindowMonitors() {
    std::vector<BackendMonitor> out;

    std::string outResp, wsResp;
    if (!Query("\"Outputs\"", &outResp)) return out;
    Query("\"Workspaces\"", &wsResp);

    std::vector<NiriOutput> outs = ParseOutputs(outResp);
    std::vector<NiriWorkspace> wss = ParseWorkspaces(wsResp);

    for (size_t i = 0; i < outs.size(); ++i) {
        const NiriOutput& o = outs[i];
        BackendMonitor m;
        m.id = static_cast<int>(i);
        m.name = o.name;
        m.x = RoundToInt(o.x);
        m.y = RoundToInt(o.y);
        m.width = RoundToInt(o.w);
        m.height = RoundToInt(o.h);
        m.scale = o.scale;
        // niri does not report layer-shell exclusive zones over IPC; leave
        // reserved at zero.
        for (const NiriWorkspace& ws : wss) {
            if (ws.isActive && ws.output == o.name) {
                m.activeWorkspaceId = static_cast<int>(ws.id);
                break;
            }
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<BackendWindow> NiriBackend::GetWindowList() {
    std::vector<BackendWindow> out;

    std::string outResp, wsResp, winResp;
    if (!Query("\"Outputs\"", &outResp)) return out;
    Query("\"Workspaces\"", &wsResp);
    if (!Query("\"Windows\"", &winResp)) return out;

    std::vector<NiriOutput> outs = ParseOutputs(outResp);
    std::vector<NiriWorkspace> wss = ParseWorkspaces(wsResp);

    // output name -> synthetic monitor id (sorted index) and geometry origin.
    std::map<std::string, int> outputId;
    std::map<std::string, const NiriOutput*> outputByName;
    for (size_t i = 0; i < outs.size(); ++i) {
        outputId[outs[i].name] = static_cast<int>(i);
        outputByName[outs[i].name] = &outs[i];
    }
    // workspace id -> output name.
    std::map<long, std::string> wsOutput;
    for (const NiriWorkspace& ws : wss) wsOutput[ws.id] = ws.output;

    size_t from, to;
    if (!FindArrayBounds(winResp, "Windows", &from, &to)) return out;

    for (const std::string& obj : CollectObjects(winResp, from, to)) {
        long id = -1, workspaceId = -1;
        if (!ExtractInt(obj, "id", &id)) continue;
        ExtractInt(obj, "workspace_id", &workspaceId);

        std::string layout;
        if (!ExtractObject(obj, "layout", &layout)) continue;

        double tx, ty;
        // Only windows in the current workspace view carry a tile position; the
        // rest are off-screen (other columns/workspaces) and not edge-relevant.
        if (!ExtractPair(layout, "tile_pos_in_workspace_view", &tx, &ty)) continue;

        double wsz0 = 0, wsz1 = 0;
        ExtractPair(layout, "window_size", &wsz0, &wsz1);
        double ox = 0, oy = 0;
        ExtractPair(layout, "window_offset_in_tile", &ox, &oy);

        // Resolve the window's output through its workspace.
        auto wsIt = wsOutput.find(workspaceId);
        const NiriOutput* mon = nullptr;
        int monId = -1;
        if (wsIt != wsOutput.end()) {
            auto oIt = outputByName.find(wsIt->second);
            if (oIt != outputByName.end()) {
                mon = oIt->second;
                monId = outputId[wsIt->second];
            }
        }

        double baseX = mon ? mon->x : 0.0;
        double baseY = mon ? mon->y : 0.0;

        BackendWindow w;
        w.id = std::to_string(id);
        w.x = RoundToInt(baseX + tx + ox);
        w.y = RoundToInt(baseY + ty + oy);
        w.width = RoundToInt(wsz0);
        w.height = RoundToInt(wsz1);
        w.monitorId = monId;
        w.workspaceId = static_cast<int>(workspaceId);
        ExtractString(obj, "title", &w.title);
        ExtractString(obj, "app_id", &w.cls);
        ExtractBool(obj, "is_floating", &w.floating);
        out.push_back(std::move(w));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Window control. niri actions are synchronous: the reply arrives once the
// action is applied, so no polling is needed.
// ---------------------------------------------------------------------------
bool NiriBackend::SendAction(const std::string& actionBody) {
    std::string resp;
    if (!Query("{\"Action\":" + actionBody + "}", &resp)) return false;
    return resp.find("\"Ok\"") != std::string::npos;
}

NiriBackend::WindowRect NiriBackend::FindWindow(const std::string& id) {
    WindowRect r;
    long wantId = std::strtol(id.c_str(), nullptr, 10);

    std::string outResp, wsResp, winResp;
    if (!Query("\"Outputs\"", &outResp)) return r;
    Query("\"Workspaces\"", &wsResp);
    if (!Query("\"Windows\"", &winResp)) return r;

    std::vector<NiriOutput> outs = ParseOutputs(outResp);
    std::vector<NiriWorkspace> wss = ParseWorkspaces(wsResp);
    std::map<std::string, const NiriOutput*> outputByName;
    for (const auto& o : outs) outputByName[o.name] = &o;
    std::map<long, std::string> wsOutput;
    for (const NiriWorkspace& ws : wss) wsOutput[ws.id] = ws.output;

    size_t from, to;
    if (!FindArrayBounds(winResp, "Windows", &from, &to)) return r;
    for (const std::string& obj : CollectObjects(winResp, from, to)) {
        long wid = -1;
        if (!ExtractInt(obj, "id", &wid) || wid != wantId) continue;

        r.valid = true;
        ExtractBool(obj, "is_floating", &r.floating);
        long workspaceId = -1;
        ExtractInt(obj, "workspace_id", &workspaceId);
        auto wsIt = wsOutput.find(workspaceId);
        const NiriOutput* o = nullptr;
        if (wsIt != wsOutput.end()) {
            auto oIt = outputByName.find(wsIt->second);
            if (oIt != outputByName.end()) o = oIt->second;
        }
        if (o) {
            r.output = o->name;
            r.outX = RoundToInt(o->x);
            r.outY = RoundToInt(o->y);
        }
        std::string layout;
        if (ExtractObject(obj, "layout", &layout)) {
            double wsz0 = 0, wsz1 = 0, tx = 0, ty = 0, ox = 0, oy = 0;
            ExtractPair(layout, "window_size", &wsz0, &wsz1);
            r.w = RoundToInt(wsz0);
            r.h = RoundToInt(wsz1);
            if (ExtractPair(layout, "tile_pos_in_workspace_view", &tx, &ty)) {
                ExtractPair(layout, "window_offset_in_tile", &ox, &oy);
                r.x = RoundToInt(o ? o->x + tx + ox : tx + ox);
                r.y = RoundToInt(o ? o->y + ty + oy : ty + oy);
            }
        }
        break;
    }
    return r;
}

bool NiriBackend::SetFloating(const std::string& id, bool floating) {
    const char* action = floating ? "MoveWindowToFloating" : "MoveWindowToTiling";
    return SendAction(std::string("{\"") + action + "\":{\"id\":" + id + "}}");
}

bool NiriBackend::MoveFloating(const std::string& id, int localX, int localY) {
    // ponytail: SetFixed is relative to the output working area, which excludes
    // struts/bars; a top bar offsets placement by its height. Good enough; add
    // working-area origin from the layer-shell/logical delta if it ever matters.
    return SendAction("{\"MoveFloatingWindow\":{\"id\":" + id +
                      ",\"x\":{\"SetFixed\":" + std::to_string((double)localX) +
                      "},\"y\":{\"SetFixed\":" + std::to_string((double)localY) + "}}}");
}

bool NiriBackend::SetSize(const std::string& id, int wpx, int hpx) {
    bool ok = SendAction("{\"SetWindowWidth\":{\"id\":" + id +
                         ",\"change\":{\"SetFixed\":" + std::to_string(wpx) + "}}}");
    ok = SendAction("{\"SetWindowHeight\":{\"id\":" + id +
                    ",\"change\":{\"SetFixed\":" + std::to_string(hpx) + "}}}") && ok;
    return ok;
}
