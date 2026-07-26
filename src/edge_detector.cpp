#include "edge_detector.h"

#include "hyprland.h"
#include <iostream>

EdgeDetector g_edgeDetector;

void EdgeDetector::SetEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) {
        m_edgeWindows.clear();
    }
}

bool EdgeDetector::IsWindowAtEdge(int wx, int wy, int ww, int wh,
                                  const HyprlandMonitor& m, int threshold) {
    if (wx + ww < m.x || wx > m.x + m.width) return false;
    if (wy + wh < m.y || wy > m.y + m.height) return false;

    const int rightEdge = m.x + m.width;
    const int bottomEdge = m.y + m.height;

    if (wx <= m.x + threshold) return true;
    if (wx + ww >= rightEdge - threshold) return true;
    if (wy <= m.y + threshold) return true;
    if (wy + wh >= bottomEdge - threshold) return true;

    return false;
}

void EdgeDetector::Tick(double currentTime, HyprlandBackend* backend,
                        const std::vector<HyprlandMonitor>& monitors) {
    if (!m_enabled || !backend) {
        if (!m_enabled) m_edgeWindows.clear();
        return;
    }
    if (currentTime - m_lastTickTime < m_tickInterval) return;
    m_lastTickTime = currentTime;

    std::cerr << "[edge] Tick: querying IPC... backend=" << backend->Name()
              << " monitors=" << monitors.size() << "\n";
    std::vector<HyprlandBackend::Window> rawWindows = backend->GetWindows();
    std::cerr << "[edge] Got " << rawWindows.size() << " windows from IPC\n";

    m_edgeWindows.clear();
    for (const auto& rw : rawWindows) {
        for (const auto& mon : monitors) {
            if (IsWindowAtEdge(rw.x, rw.y, rw.width, rw.height, mon)) {
                m_edgeWindows.push_back({rw.address, rw.x, rw.y, rw.width, rw.height,
                                         rw.title, rw.cls});
                std::cerr << "[edge] " << rw.title << " (" << rw.cls << ") at ("
                          << rw.x << "," << rw.y << " " << rw.width << "x" << rw.height
                          << ") mon=[" << mon.x << "," << mon.y << " "
                          << mon.width << "x" << mon.height << "]\n";
                break;
            }
        }
    }
    if (!m_edgeWindows.empty()) {
        std::cerr << "[edge] " << m_edgeWindows.size() << " windows at edges\n";
    }
}
