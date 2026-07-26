#pragma once

#include <string>
#include <vector>

class HyprlandBackend;

struct HyprlandMonitor {
    int id;
    std::string name;
    int x, y, width, height;
    double scale;
};

struct EdgeWindow {
    std::string address;
    int x, y, width, height;
    std::string title;
    std::string cls;
};

class EdgeDetector {
public:
    void Tick(double currentTime, HyprlandBackend* backend,
              const std::vector<HyprlandMonitor>& monitors);

    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled);

    const std::vector<EdgeWindow>& EdgeWindows() const { return m_edgeWindows; }

private:
    static bool IsWindowAtEdge(int wx, int wy, int ww, int wh,
                               const HyprlandMonitor& m, int threshold = 5);

    bool m_enabled = false;
    double m_lastTickTime = -1.0;
    double m_tickInterval = 0.5;
    std::vector<EdgeWindow> m_edgeWindows;
};

extern EdgeDetector g_edgeDetector;
