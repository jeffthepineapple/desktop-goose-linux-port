#pragma once

#include <set>
#include <string>
#include <vector>

class HyprlandBackend;

struct HyprlandMonitor {
    int id;
    std::string name;
    int x, y, width, height;
    double scale;
};

struct HyprlandWindow {
    std::string address;
    int x, y, width, height;
    int monitorId;
    std::string title;
    std::string cls;
};

class EdgeDetector {
public:
    void Tick(double currentTime, HyprlandBackend* backend,
              const std::vector<HyprlandMonitor>& monitors);

    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool enabled);
    void Toggle();

    const std::string& HighlightColor() const { return m_highlightColor; }
    void SetHighlightColor(const std::string& color) { m_highlightColor = color; }

    int HighlightedCount() const { return (int)m_highlightedAddresses.size(); }

private:
    static bool IsWindowAtEdge(const HyprlandWindow& w, const HyprlandMonitor& m,
                               int threshold = 5);

    bool m_enabled = false;
    double m_lastTickTime = -1.0;
    double m_tickInterval = 0.5;
    std::string m_highlightColor = "FF0000FF";
    std::set<std::string> m_highlightedAddresses;
};

extern EdgeDetector g_edgeDetector;
