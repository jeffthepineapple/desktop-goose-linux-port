#include "window_capture.h"

#include "wlr-screencopy-unstable-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <list>
#include <limits>
#include <poll.h>
#include <string>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {

struct OutputInfo {
    wl_output* output = nullptr;
    std::string name;
    int x = 0;
    int y = 0;
};

struct CaptureState {
    wl_shm* shm = nullptr;
    zwlr_screencopy_manager_v1* manager = nullptr;
    uint32_t managerVersion = 0;
    std::list<OutputInfo> outputs;

    zwlr_screencopy_frame_v1* frame = nullptr;
    wl_shm_pool* pool = nullptr;
    wl_buffer* buffer = nullptr;
    void* mapped = MAP_FAILED;
    size_t mappedSize = 0;
    int fd = -1;

    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t flags = 0;
    bool copied = false;
    bool ready = false;
    bool failed = false;
    std::string error;
};

void SetError(std::string* errorOut, const std::string& error) {
    if (errorOut) {
        *errorOut = error;
    }
}

void output_geometry(void* data, wl_output*, int32_t x, int32_t y, int32_t,
                     int32_t, int32_t, const char*, const char*, int32_t) {
    auto* output = static_cast<OutputInfo*>(data);
    output->x = x;
    output->y = y;
}

void output_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void output_done(void*, wl_output*) {}
void output_scale(void*, wl_output*, int32_t) {}
void output_name(void* data, wl_output*, const char* name) {
    static_cast<OutputInfo*>(data)->name = name ? name : "";
}
void output_description(void*, wl_output*, const char*) {}

const wl_output_listener kOutputListener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
    output_name,
    output_description,
};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version) {
    auto* state = static_cast<CaptureState*>(data);
    if (std::strcmp(interface, wl_shm_interface.name) == 0 && !state->shm) {
        state->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        state->outputs.emplace_back();
        OutputInfo& output = state->outputs.back();
        output.output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4u)));
        if (output.output) {
            wl_output_add_listener(output.output, &kOutputListener, &output);
        }
    } else if (std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0 && !state->manager) {
        state->managerVersion = std::min(version, 3u);
        state->manager = static_cast<zwlr_screencopy_manager_v1*>(wl_registry_bind(
            registry, name, &zwlr_screencopy_manager_v1_interface, state->managerVersion));
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {registry_global, registry_global_remove};

int CreateMemfd(const char* name) {
#ifdef SYS_memfd_create
    return static_cast<int>(syscall(SYS_memfd_create, name, MFD_CLOEXEC));
#else
    (void)name;
    return -1;
#endif
}

void AllocateBuffer(CaptureState* state) {
    if (state->width == 0 || state->height == 0 ||
        state->width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        state->height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        state->width > std::numeric_limits<uint32_t>::max() / 4u ||
        state->stride < state->width * 4u ||
        state->height > std::numeric_limits<size_t>::max() / state->stride) {
        state->failed = true;
        state->error = "screencopy returned invalid buffer dimensions";
        return;
    }
    if (state->format != WL_SHM_FORMAT_ARGB8888 && state->format != WL_SHM_FORMAT_XRGB8888) {
        state->failed = true;
        state->error = "screencopy did not offer a supported SHM format";
        return;
    }

    state->mappedSize = static_cast<size_t>(state->stride) * state->height;
    if (state->mappedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
        state->failed = true;
        state->error = "screencopy SHM allocation is too large";
        return;
    }
    state->fd = CreateMemfd("cppgoose-screencopy");
    if (state->fd < 0 || ftruncate(state->fd, static_cast<off_t>(state->mappedSize)) != 0) {
        state->failed = true;
        state->error = "failed to allocate screencopy SHM buffer";
        return;
    }
    state->mapped = mmap(nullptr, state->mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, state->fd, 0);
    if (state->mapped == MAP_FAILED) {
        state->failed = true;
        state->error = "failed to map screencopy SHM buffer";
        return;
    }
    state->pool = wl_shm_create_pool(state->shm, state->fd, static_cast<int>(state->mappedSize));
    if (!state->pool) {
        state->failed = true;
        state->error = "failed to create screencopy SHM pool";
        return;
    }
    state->buffer = wl_shm_pool_create_buffer(state->pool, 0, static_cast<int32_t>(state->width),
                                               static_cast<int32_t>(state->height),
                                               static_cast<int32_t>(state->stride), state->format);
    if (!state->buffer) {
        state->failed = true;
        state->error = "failed to create screencopy SHM buffer";
    }
}

void frame_buffer(void* data, zwlr_screencopy_frame_v1*, uint32_t format,
                  uint32_t width, uint32_t height, uint32_t stride) {
    auto* state = static_cast<CaptureState*>(data);
    if (state->buffer || state->failed) {
        return;
    }
    state->format = format;
    state->width = width;
    state->height = height;
    state->stride = stride;
    AllocateBuffer(state);
    if (!state->failed && state->managerVersion < 3) {
        zwlr_screencopy_frame_v1_copy(state->frame, state->buffer);
        state->copied = true;
    }
}

void frame_flags(void* data, zwlr_screencopy_frame_v1*, uint32_t flags) {
    static_cast<CaptureState*>(data)->flags = flags;
}
void frame_ready(void* data, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {
    static_cast<CaptureState*>(data)->ready = true;
}
void frame_failed(void* data, zwlr_screencopy_frame_v1*) {
    auto* state = static_cast<CaptureState*>(data);
    state->failed = true;
    state->error = "compositor failed to capture screencopy frame";
}
void frame_damage(void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void frame_linux_dmabuf(void*, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {}
void frame_buffer_done(void* data, zwlr_screencopy_frame_v1*) {
    auto* state = static_cast<CaptureState*>(data);
    if (!state->buffer) {
        state->failed = true;
        state->error = "screencopy did not offer a supported SHM buffer";
    } else if (!state->copied) {
        zwlr_screencopy_frame_v1_copy(state->frame, state->buffer);
        state->copied = true;
    }
}

const zwlr_screencopy_frame_v1_listener kFrameListener = {
    frame_buffer,
    frame_flags,
    frame_ready,
    frame_failed,
    frame_damage,
    frame_linux_dmabuf,
    frame_buffer_done,
};

void Cleanup(CaptureState* state) {
    if (state->frame) zwlr_screencopy_frame_v1_destroy(state->frame);
    if (state->buffer) wl_buffer_destroy(state->buffer);
    if (state->pool) wl_shm_pool_destroy(state->pool);
    if (state->mapped != MAP_FAILED) munmap(state->mapped, state->mappedSize);
    if (state->fd >= 0) close(state->fd);
    if (state->manager) zwlr_screencopy_manager_v1_destroy(state->manager);
    for (OutputInfo& output : state->outputs) {
        if (output.output) wl_output_destroy(output.output);
    }
    if (state->shm) wl_shm_destroy(state->shm);
}

}  // namespace

GdkPixbuf* CaptureWaylandRegion(const WindowCaptureRegion& region, std::string* errorOut) {
    if (errorOut) errorOut->clear();
    if (region.width <= 0 || region.height <= 0 || region.x < 0 || region.y < 0) {
        SetError(errorOut, "screencopy region is invalid or empty");
        return nullptr;
    }
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    SetError(errorOut, "screencopy requires a little-endian pixel layout");
    return nullptr;
#endif

    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        SetError(errorOut, "failed to connect to the Wayland display");
        return nullptr;
    }

    CaptureState state;
    wl_registry* registry = wl_display_get_registry(display);
    if (!registry) {
        wl_display_disconnect(display);
        SetError(errorOut, "failed to get the Wayland registry");
        return nullptr;
    }
    wl_registry_add_listener(registry, &kRegistryListener, &state);
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
        wl_registry_destroy(registry);
        Cleanup(&state);
        wl_display_disconnect(display);
        SetError(errorOut, "failed while discovering Wayland globals");
        return nullptr;
    }
    if (!state.shm) {
        wl_registry_destroy(registry);
        Cleanup(&state);
        wl_display_disconnect(display);
        SetError(errorOut, "Wayland compositor does not advertise wl_shm");
        return nullptr;
    }
    if (!state.manager || state.managerVersion == 0) {
        wl_registry_destroy(registry);
        Cleanup(&state);
        wl_display_disconnect(display);
        SetError(errorOut, "Wayland compositor does not advertise zwlr_screencopy_manager_v1");
        return nullptr;
    }

    OutputInfo* selected = nullptr;
    for (OutputInfo& output : state.outputs) {
        if (!region.outputName.empty() && output.output && output.name == region.outputName) {
            selected = &output;
            break;
        }
    }
    if (!selected) {
        for (OutputInfo& output : state.outputs) {
            if (output.output && output.x == region.outputX && output.y == region.outputY) {
                selected = &output;
                break;
            }
        }
    }
    if (!selected) {
        wl_registry_destroy(registry);
        Cleanup(&state);
        wl_display_disconnect(display);
        SetError(errorOut, "could not find the requested Wayland output");
        return nullptr;
    }

    state.frame = zwlr_screencopy_manager_v1_capture_output_region(
        state.manager, 0, selected->output, region.x, region.y, region.width, region.height);
    if (!state.frame) {
        wl_registry_destroy(registry);
        Cleanup(&state);
        wl_display_disconnect(display);
        SetError(errorOut, "failed to create screencopy frame");
        return nullptr;
    }
    zwlr_screencopy_frame_v1_add_listener(state.frame, &kFrameListener, &state);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!state.ready && !state.failed) {
        if (wl_display_dispatch_pending(display) < 0) {
            state.failed = true;
            state.error = "Wayland display disconnected during screencopy";
            break;
        }
        if (state.ready || state.failed) break;
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            state.failed = true;
            state.error = "failed to flush screencopy request to Wayland";
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            state.failed = true;
            state.error = "timed out waiting for screencopy frame";
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{wl_display_get_fd(display), POLLIN, 0};
        const int pollResult = poll(&pfd, 1, static_cast<int>(std::max<int64_t>(1, remaining)));
        if (pollResult == 0) {
            state.failed = true;
            state.error = "timed out waiting for screencopy frame";
        } else if (pollResult < 0 && errno != EINTR) {
            state.failed = true;
            state.error = "failed while polling for screencopy frame";
        } else if (pollResult > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            state.failed = true;
            state.error = "Wayland display closed during screencopy";
        } else if (pollResult > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(display) < 0) {
            state.failed = true;
            state.error = "Wayland display disconnected during screencopy";
        }
    }

    GdkPixbuf* pixbuf = nullptr;
    if (state.ready && !state.failed && state.mapped != MAP_FAILED) {
        pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, static_cast<int>(state.width),
                                static_cast<int>(state.height));
        if (!pixbuf) {
            state.failed = true;
            state.error = "failed to allocate screencopy pixbuf";
        } else {
            const int destinationStride = gdk_pixbuf_get_rowstride(pixbuf);
            auto* destination = gdk_pixbuf_get_pixels(pixbuf);
            const auto* source = static_cast<const uint8_t*>(state.mapped);
            for (uint32_t y = 0; y < state.height; ++y) {
                const uint32_t sourceY = (state.flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT)
                                             ? state.height - 1 - y
                                             : y;
                const auto* sourceRow = source + static_cast<size_t>(sourceY) * state.stride;
                auto* destinationRow = destination + static_cast<size_t>(y) * destinationStride;
                for (uint32_t x = 0; x < state.width; ++x) {
                    destinationRow[x * 4] = sourceRow[x * 4 + 2];
                    destinationRow[x * 4 + 1] = sourceRow[x * 4 + 1];
                    destinationRow[x * 4 + 2] = sourceRow[x * 4];
                    destinationRow[x * 4 + 3] = 255;
                }
            }
        }
    }

    const std::string failure = state.error.empty() ? "screencopy frame did not become ready" : state.error;
    wl_registry_destroy(registry);
    Cleanup(&state);
    wl_display_disconnect(display);
    if (!pixbuf) SetError(errorOut, failure);
    return pixbuf;
}
