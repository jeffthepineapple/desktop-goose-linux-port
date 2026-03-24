#include "wlroots_backend.h"
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include <iostream>
#include <cstring>
#include "world.h" // g_screenWidth/Height

#include "world.h" // g_screenWidth/Height

struct WlrootsImpl {
    struct wl_display* display = nullptr;
    struct wl_registry* registry = nullptr;
    struct wl_seat* seat = nullptr;
    struct zwlr_virtual_pointer_manager_v1* manager = nullptr;
    struct zwlr_virtual_pointer_v1* pointer = nullptr;
};

WlrootsBackend::WlrootsBackend() = default;

static void registry_handle_global(void* data, struct wl_registry* registry,
                                   uint32_t name, const char* interface, uint32_t version) {
    auto impl = static_cast<WlrootsImpl*>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        impl->seat = static_cast<struct wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        impl->manager = static_cast<struct zwlr_virtual_pointer_manager_v1*>(
            wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2));
    }
}

static void registry_handle_global_remove(void* data, struct wl_registry* registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove,
};

WlrootsBackend::~WlrootsBackend() {
    if (m_impl) {
        if (m_impl->pointer) zwlr_virtual_pointer_v1_destroy(m_impl->pointer);
        if (m_impl->manager) zwlr_virtual_pointer_manager_v1_destroy(m_impl->manager);
        if (m_impl->seat) wl_seat_destroy(m_impl->seat);
        if (m_impl->registry) wl_registry_destroy(m_impl->registry);
        if (m_impl->display) {
            wl_display_flush(m_impl->display);
            wl_display_disconnect(m_impl->display);
        }
    }
}

bool WlrootsBackend::Init() {
    // Only if WAYLAND_DISPLAY is set
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (!waylandDisplay) return false;

    auto impl = std::make_unique<WlrootsImpl>();
    impl->display = wl_display_connect(nullptr);
    if (!impl->display) return false;

    impl->registry = wl_display_get_registry(impl->display);
    wl_registry_add_listener(impl->registry, &registry_listener, impl.get());

    // Roundtrip to find globals
    wl_display_roundtrip(impl->display);

    if (!impl->manager) {
        std::cerr << "WlrootsBackend: zwlr_virtual_pointer_manager_v1 not supported by compositor." << std::endl;
        wl_display_disconnect(impl->display);
        return false;
    }

    // Create the pointer
    impl->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(impl->manager, impl->seat);
    if (!impl->pointer) {
        std::cerr << "WlrootsBackend: failed to create virtual pointer." << std::endl;
        wl_display_disconnect(impl->display);
        return false;
    }

    wl_display_flush(impl->display);

    m_impl = std::move(impl);
    return true;
}

void WlrootsBackend::MoveCursorAbs(int x, int y) {
    if (!m_impl || !m_impl->pointer) return;

    // zwlr_virtual_pointer_v1_motion_absolute(pointer, time, x, y, x_extent, y_extent)
    // We use g_screenWidth/Height as extents.
    zwlr_virtual_pointer_v1_motion_absolute(m_impl->pointer, 0, x, y, g_screenWidth, g_screenHeight);
    zwlr_virtual_pointer_v1_frame(m_impl->pointer);
    wl_display_flush(m_impl->display);
}

void WlrootsBackend::MoveCursorRel(int dx, int dy) {
    if (!m_impl || !m_impl->pointer) return;

    // Fixed point arithmetic helper
    auto to_fixed = [](double v) -> wl_fixed_t {
        return (wl_fixed_t)(v * 256.0);
    };

    zwlr_virtual_pointer_v1_motion(m_impl->pointer, 0, to_fixed(dx), to_fixed(dy));
    zwlr_virtual_pointer_v1_frame(m_impl->pointer);
    wl_display_flush(m_impl->display);
}
