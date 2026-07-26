#include "edge_detector.h"

#include "hyprland.h"
#include "config.h"
#include <algorithm>

EdgeDetector g_edgeDetector;

void EdgeDetector::SetEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) {
        m_highlightedAddresses.clear();
    }
}

void EdgeDetector::Toggle() {
    SetEnabled(!m_enabled);
}

bool EdgeDetector::IsWindowAtEdge(const HyprlandWindow& w, const HyprlandMonitor& m,
                                  int threshold) {
    // Window must be on this monitor
    if (w.x + w.width < m.x || w.x > m.x + m.width) return false;
    if (w.y + w.height < m.y || w.y > m.y + m.height) return false;

    // Check if any edge touches a monitor boundary
    const int rightEdge = m.x + m.width;
    const int bottomEdge = m.y + m.height;

    if (w.x <= m.x + threshold) return true;
    if (w.x + w.width >= rightEdge - threshold) return true;
    if (w.y <= m.y + threshold) return true;
    if (w.y + w.height >= bottomEdge - threshold) return true;

    return false;
}

void EdgeDetector::Tick(double currentTime, HyprlandBackend* backend,
                        const std::vector<HyprlandMonitor>& monitors) {
    if (!m_enabled || !backend) return;
    if (currentTime - m_lastTickTime < m_tickInterval) return;
    m_lastTickTime = currentTime;

    std::vector<HyprlandBackend::Window> rawWindows = backend->GetWindows();
    if (rawWindows.empty()) return;

    std::set<std::string> newHighlighted;

    for (const auto& rawWin : rawWindows) {
        // Convert to our Window struct
        HyprlandWindow w;
        w.address = rawWin.address;
        w.x = rawWin.x;
        w.y = rawWin.y;
        w.width = rawWin.width;
        w.height = rawWin.height;
        w.monitorId = rawWin.monitorId;
        w.title = rawWin.title;
        w.cls = rawWin.cls;

        bool atEdge = false;
        for (const auto& mon : monitors) {
            if (IsWindowAtEdge(w, mon)) {
                atEdge = true;
                break;
            }
        }

        if (atEdge) {
            newHighlighted.insert(w.address);
        }
    }

    // Highlight newly detected edge windows
    for (const auto& addr : newHighlighted) {
        if (m_highlightedAddresses.find(addr) == m_highlightedAddresses.end()) {
            backend->SetWindowBorderColor(addr, m_highlightColor);
        }
    }

    // Reset windows that are no longer at edges
    for (const auto& addr : m_highlightedAddresses) {
        if (newHighlighted.find(addr) == newHighlighted.end()) {
            backend->ResetWindowBorder(addr);
        }
    }

    m_highlightedAddresses = std::move(newHighlighted);
}
