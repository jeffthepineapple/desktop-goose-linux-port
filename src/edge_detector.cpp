#include "edge_detector.h"

#include "cursor_backend.h"

EdgeDetector g_edgeDetector;

void EdgeDetector::SetEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) {
        m_edgeWindows.clear();
        m_monitors.clear();
    }
}

bool EdgeDetector::IsWindowAtEdge(int wx, int wy, int ww, int wh,
                                  const HyprlandMonitor& m, int threshold) {
    if (wx + ww < m.x || wx > m.x + m.width) return false;
    if (wy + wh < m.y || wy > m.y + m.height) return false;

    const int topEdge    = m.y + m.reservedTop;
    const int rightEdge  = m.x + m.width  - m.reservedRight;
    const int bottomEdge = m.y + m.height - m.reservedBottom;
    const int leftEdge   = m.x + m.reservedLeft;

    if (wx <= leftEdge  + threshold) return true;
    if (wx + ww >= rightEdge  - threshold) return true;
    if (wy <= topEdge   + threshold) return true;
    if (wy + wh >= bottomEdge - threshold) return true;

    return false;
}

void EdgeDetector::Tick(CursorBackend* backend) {
    if (!m_enabled || !backend || !backend->SupportsWindowInfo()) {
        if (!m_enabled) {
            m_edgeWindows.clear();
            m_monitors.clear();
        }
        return;
    }

    // Refresh monitors
    std::vector<BackendMonitor> rawMonitors = backend->GetWindowMonitors();
    m_monitors.clear();
    for (const auto& rm : rawMonitors) {
        HyprlandMonitor m;
        m.id = rm.id;
        m.name = rm.name;
        m.x = rm.x;
        m.y = rm.y;
        m.width = rm.width;
        m.height = rm.height;
        m.scale = rm.scale;
        m.reservedTop    = rm.reserved[0];
        m.reservedBottom = rm.reserved[1];
        m.reservedLeft   = rm.reserved[2];
        m.reservedRight  = rm.reserved[3];
        m.activeWorkspaceId = rm.activeWorkspaceId;
        m_monitors.push_back(m);
    }

    // Refresh windows and detect edges
    std::vector<BackendWindow> rawWindows = backend->GetWindowList();
    m_edgeWindows.clear();
    for (const auto& rw : rawWindows) {
        const HyprlandMonitor* monitor = nullptr;
        for (const auto& mon : m_monitors) {
            if (mon.id == rw.monitorId) {
                monitor = &mon;
                break;
            }
        }

        if (!monitor || rw.workspaceId != monitor->activeWorkspaceId) continue;
        if (IsWindowAtEdge(rw.x, rw.y, rw.width, rw.height, *monitor)) {
            m_edgeWindows.push_back({rw.id, rw.x, rw.y, rw.width, rw.height,
                                     rw.title, rw.cls, rw.monitorId, rw.workspaceId,
                                     rw.floating});
        }
    }
}
