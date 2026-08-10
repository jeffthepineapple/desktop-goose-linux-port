#pragma once

#include <string>
#include <vector>

class HyprlandBackend;

struct HyprlandMonitor {
    int id;
    std::string name;
    int x, y, width, height;
    double scale;
    int reservedTop = 0;
    int reservedBottom = 0;
    int reservedLeft = 0;
    int reservedRight = 0;
    int activeWorkspaceId = -1;
};

struct EdgeWindow {
    std::string address;
    int x, y, width, height;
    std::string title;
    std::string cls;
    int monitorId = -1;
    int workspaceId = -1;
    bool floating = false;
};

class EdgeDetector {
public:
    void Tick(HyprlandBackend* backend);

    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled);

    const std::vector<HyprlandMonitor>& Monitors() const { return m_monitors; }
    const std::vector<EdgeWindow>& EdgeWindows() const { return m_edgeWindows; }

private:
    static bool IsWindowAtEdge(int wx, int wy, int ww, int wh,
                               const HyprlandMonitor& m, int threshold = 15);

    bool m_enabled = false;
    std::vector<HyprlandMonitor> m_monitors;
    std::vector<EdgeWindow> m_edgeWindows;
};

extern EdgeDetector g_edgeDetector;
