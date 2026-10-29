#include "niri_cursor.h"

// Generated from the installed wayland-protocols staging XML (see CMakeLists.txt).
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#include <wayland-client-protocol.h>
#include <wayland-client.h>

#include <poll.h>

#include <algorithm>
#include <cstring>

struct NiriCursorTracker::Impl {
    struct Output {
        // Bound wl_output used only to learn its name (wl_output v4).
        struct wl_output* wlOutput = nullptr;
        std::string wlName;
        Impl* owner = nullptr;
        uint32_t globalName = 0;
    };

    // One pointer cursor session bound to one niri output.
    struct Slot {
        Impl* owner = nullptr;
        NiriCursorOutput info;
        struct ext_image_capture_source_v1* source = nullptr;
        struct ext_image_copy_capture_cursor_session_v1* cursor = nullptr;
        struct ext_image_copy_capture_session_v1* session = nullptr;
        bool stopped = false;
        bool entered = false;                 // cursor is over this output
        bool hasPos = false;                  // position received since enter
        int32_t px = 0, py = 0;               // hotspot-relative, buffer pixels
        uint64_t seq = 0;                     // ordering of the last position
    };

    struct wl_display* display = nullptr;
    struct wl_registry* registry = nullptr;
    struct wl_seat* seat = nullptr;
    uint32_t seatCaps = 0;
    struct wl_pointer* pointer = nullptr;
    struct ext_output_image_capture_source_manager_v1* sourceMgr = nullptr;
    struct ext_image_copy_capture_manager_v1* copyMgr = nullptr;
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Slot>> slots;
    uint64_t seq = 0;
    bool dead = false;
    bool initialized = false;
    bool outputsChanged = false;
};

namespace {

using Impl = NiriCursorTracker::Impl;
using Slot = NiriCursorTracker::Impl::Slot;
using Output = NiriCursorTracker::Impl::Output;

// --- wl_output (name only) ------------------------------------------------

void OutputGeometry(void*, struct wl_output*, int32_t, int32_t, int32_t, int32_t,
                    int32_t, const char*, const char*, int32_t) {}
void OutputMode(void*, struct wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void OutputDone(void* data, struct wl_output*) {
    auto* owner = static_cast<Output*>(data)->owner;
    if (owner->initialized) owner->outputsChanged = true;
}
void OutputScale(void*, struct wl_output*, int32_t) {}
void OutputName(void* data, struct wl_output*, const char* name) {
    static_cast<Output*>(data)->wlName = name ? name : "";
}
void OutputDescription(void*, struct wl_output*, const char*) {}

const struct wl_output_listener kOutputListener = {
    OutputGeometry,
    OutputMode,
    OutputDone,
    OutputScale,
    OutputName,
    OutputDescription,
};

// --- wl_seat (capabilities only) ------------------------------------------

void SeatCapabilities(void* data, struct wl_seat*, uint32_t caps) {
    static_cast<Impl*>(data)->seatCaps = caps;
}
void SeatName(void*, struct wl_seat*, const char*) {}

const struct wl_seat_listener kSeatListener = {
    SeatCapabilities,
    SeatName,
};

// --- ext-image-copy-capture cursor session --------------------------------

void CursorEnter(void* data, struct ext_image_copy_capture_cursor_session_v1*) {
    auto* s = static_cast<Slot*>(data);
    s->entered = true;
    s->hasPos = false;
}

void CursorLeave(void* data, struct ext_image_copy_capture_cursor_session_v1*) {
    auto* s = static_cast<Slot*>(data);
    s->entered = false;
    s->hasPos = false;
}

void CursorPosition(void* data, struct ext_image_copy_capture_cursor_session_v1*,
                    int32_t x, int32_t y) {
    auto* s = static_cast<Slot*>(data);
    s->px = x;
    s->py = y;
    s->hasPos = true;
    s->seq = ++s->owner->seq;
}

void CursorHotspot(void*, struct ext_image_copy_capture_cursor_session_v1*, int32_t, int32_t) {}

const struct ext_image_copy_capture_cursor_session_v1_listener kCursorListener = {
    CursorEnter,
    CursorLeave,
    CursorPosition,
    CursorHotspot,
};

// --- ext-image-copy-capture capture session -------------------------------

void SessionBufferSize(void*, struct ext_image_copy_capture_session_v1*, uint32_t, uint32_t) {}

void SessionShmFormat(void*, struct ext_image_copy_capture_session_v1*, uint32_t) {}
void SessionDmabufDevice(void*, struct ext_image_copy_capture_session_v1*, struct wl_array*) {}
void SessionDmabufFormat(void*, struct ext_image_copy_capture_session_v1*, uint32_t, struct wl_array*) {}

void SessionDone(void*, struct ext_image_copy_capture_session_v1*) {}

void SessionStopped(void* data, struct ext_image_copy_capture_session_v1*) {
    auto* s = static_cast<Slot*>(data);
    s->stopped = true;
    s->entered = false;
    s->hasPos = false;
}

const struct ext_image_copy_capture_session_v1_listener kSessionListener = {
    SessionBufferSize,
    SessionShmFormat,
    SessionDmabufDevice,
    SessionDmabufFormat,
    SessionDone,
    SessionStopped,
};

// --- wl_registry ----------------------------------------------------------

void RegistryGlobal(void* data, struct wl_registry* registry, uint32_t name,
                    const char* iface, uint32_t version) {
    auto* impl = static_cast<Impl*>(data);
    if (std::strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
        impl->sourceMgr = static_cast<struct ext_output_image_capture_source_manager_v1*>(
            wl_registry_bind(registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
    } else if (std::strcmp(iface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        impl->copyMgr = static_cast<struct ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0 && !impl->seat) {
        impl->seat = static_cast<struct wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
        wl_seat_add_listener(impl->seat, &kSeatListener, impl);
    } else if (std::strcmp(iface, wl_output_interface.name) == 0 && version >= 4) {
        // wl_output.name is only sent from version 4 on; older outputs cannot
        // be matched to niri outputs and are ignored.
        auto out = std::make_unique<Output>();
        out->owner = impl;
        out->globalName = name;
        if (impl->initialized) impl->outputsChanged = true;
        out->wlOutput = static_cast<struct wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, 4));
        wl_output_add_listener(out->wlOutput, &kOutputListener, out.get());
        impl->outputs.push_back(std::move(out));
    }
}

void RegistryGlobalRemove(void* data, struct wl_registry*, uint32_t name) {
    auto* impl = static_cast<Impl*>(data);
    for (const auto& output : impl->outputs)
        if (output->globalName == name) impl->outputsChanged = true;
}

const struct wl_registry_listener kRegistryListener = {
    RegistryGlobal,
    RegistryGlobalRemove,
};

// Releases a proxy with its protocol release request when the bound version
// provides one, otherwise destroys the local proxy.
void ReleaseProxy(struct wl_proxy* proxy, uint32_t releaseSince,
                  void (*release)(struct wl_proxy*)) {
    if (!proxy) return;
    if (wl_proxy_get_version(proxy) >= releaseSince) {
        release(proxy);
    } else {
        wl_proxy_destroy(proxy);
    }
}

void WlOutputRelease(struct wl_proxy* p) { wl_output_release(reinterpret_cast<struct wl_output*>(p)); }
void WlSeatRelease(struct wl_proxy* p) { wl_seat_release(reinterpret_cast<struct wl_seat*>(p)); }
void WlPointerRelease(struct wl_proxy* p) { wl_pointer_release(reinterpret_cast<struct wl_pointer*>(p)); }

} // namespace

NiriCursorTracker::NiriCursorTracker() : m_impl(std::make_unique<Impl>()) {}

NiriCursorTracker::~NiriCursorTracker() {
    Impl* im = m_impl.get();
    if (!im) return;

    for (auto& slot : im->slots) {
        if (slot->cursor) ext_image_copy_capture_cursor_session_v1_destroy(slot->cursor);
        if (slot->session) ext_image_copy_capture_session_v1_destroy(slot->session);
        if (slot->source) ext_image_capture_source_v1_destroy(slot->source);
    }
    if (im->pointer) ReleaseProxy(reinterpret_cast<struct wl_proxy*>(im->pointer), 3, WlPointerRelease);
    if (im->copyMgr) ext_image_copy_capture_manager_v1_destroy(im->copyMgr);
    if (im->sourceMgr) ext_output_image_capture_source_manager_v1_destroy(im->sourceMgr);
    for (auto& out : im->outputs) {
        if (out->wlOutput) ReleaseProxy(reinterpret_cast<struct wl_proxy*>(out->wlOutput), 3, WlOutputRelease);
    }
    if (im->seat) ReleaseProxy(reinterpret_cast<struct wl_proxy*>(im->seat), 5, WlSeatRelease);
    if (im->registry) wl_registry_destroy(im->registry);
    if (im->display) {
        wl_display_flush(im->display);
        wl_display_disconnect(im->display);
    }
}

bool NiriCursorTracker::Init(const std::vector<NiriCursorOutput>& outputs) {
    Impl* im = m_impl.get();
    m_reason.clear();

    im->display = wl_display_connect(nullptr);
    if (!im->display) {
        m_reason = "cannot connect to the Wayland display";
        return false;
    }

    im->registry = wl_display_get_registry(im->display);
    wl_registry_add_listener(im->registry, &kRegistryListener, im);
    // Round 1: advertised globals; binds happen while handling these events.
    wl_display_roundtrip(im->display);
    // Round 2: wl_output names and seat capabilities arrive after binding.
    wl_display_roundtrip(im->display);

    if (!im->copyMgr) {
        m_reason = "ext_image_copy_capture_manager_v1 is not advertised";
        return false;
    }
    if (!im->sourceMgr) {
        m_reason = "ext_output_image_capture_source_manager_v1 is not advertised";
        return false;
    }
    if (!im->seat) {
        m_reason = "no wl_seat advertised";
        return false;
    }
    if (!(im->seatCaps & WL_SEAT_CAPABILITY_POINTER)) {
        m_reason = "seat has no pointer capability";
        return false;
    }
    im->pointer = wl_seat_get_pointer(im->seat);

    for (const NiriCursorOutput& info : outputs) {
        Output* match = nullptr;
        for (auto& out : im->outputs) {
            if (out->wlName == info.name) {
                match = out.get();
                break;
            }
        }
        if (!match) continue;

        auto slot = std::make_unique<Slot>();
        slot->owner = im;
        slot->info = info;
        slot->source = ext_output_image_capture_source_manager_v1_create_source(
            im->sourceMgr, match->wlOutput);
        slot->cursor = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
            im->copyMgr, slot->source, im->pointer);
        ext_image_copy_capture_cursor_session_v1_add_listener(slot->cursor, &kCursorListener, slot.get());
        slot->session = ext_image_copy_capture_cursor_session_v1_get_capture_session(slot->cursor);
        ext_image_copy_capture_session_v1_add_listener(slot->session, &kSessionListener, slot.get());
        im->slots.push_back(std::move(slot));
    }

    // Round 3: buffer_size and done for each session.
    wl_display_roundtrip(im->display);

    bool anyUsable = std::any_of(im->slots.begin(), im->slots.end(), [](const auto& s) {
        return !s->stopped && s->info.w > 0 && s->info.h > 0 && s->info.scale > 0;
    });
    if (!anyUsable) {
        m_reason = im->slots.empty()
            ? "no niri output matched a wl_output name"
            : "no output session delivered cursor constraints";
        return false;
    }
    im->initialized = true;
    return true;
}

bool NiriCursorTracker::Active() const {
    const Impl* im = m_impl.get();
    if (!im || !im->display || im->dead) return false;
    return std::any_of(im->slots.begin(), im->slots.end(), [](const auto& s) {
        return !s->stopped;
    });
}

bool NiriCursorTracker::NeedsRefresh() const {
    return m_impl && m_impl->outputsChanged;
}

void NiriCursorTracker::Pump() {
    Impl* im = m_impl.get();
    if (!im || !im->display || im->dead) return;

    while (wl_display_prepare_read(im->display) != 0) {
        if (wl_display_dispatch_pending(im->display) < 0) {
            im->dead = true;
            return;
        }
    }
    wl_display_flush(im->display);

    pollfd pfd{};
    pfd.fd = wl_display_get_fd(im->display);
    pfd.events = POLLIN;
    int n = poll(&pfd, 1, 0);
    if (n > 0 && (pfd.revents & POLLIN)) {
        wl_display_read_events(im->display);
    } else {
        wl_display_cancel_read(im->display);
    }

    if (wl_display_dispatch_pending(im->display) < 0 || wl_display_get_error(im->display) != 0) {
        im->dead = true;
        return;
    }
    if (n > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
        im->dead = true;
    }
}

Vector2 NiriCursorTracker::Position() const {
    const Impl* im = m_impl.get();
    if (!im || im->dead) return {-1.0f, -1.0f};

    const Slot* best = nullptr;
    for (const auto& s : im->slots) {
        if (!s->entered || !s->hasPos || s->stopped) continue;
        if (!best || s->seq > best->seq) best = s.get();
    }
    if (!best) return {-1.0f, -1.0f};

    // Position is in transformed *output* buffer pixels, not cursor-image
    // pixels. The transform is already applied; only output scale is removed.
    return {
        static_cast<float>(best->info.x + best->px / best->info.scale),
        static_cast<float>(best->info.y + best->py / best->info.scale),
    };
}
