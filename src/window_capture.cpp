#include "window_capture.h"

#include "hyprland-toplevel-export-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <poll.h>
#include <string>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <fstream>
#include <signal.h>
#include <iterator>

namespace {


void SetError(std::string* errorOut, const std::string& error) {
    if (errorOut) {
        *errorOut = error;
    }
}

void registry_global_remove(void*, wl_registry*, uint32_t) {}

int CreateMemfd(const char* name) {
#ifdef SYS_memfd_create
    return static_cast<int>(syscall(SYS_memfd_create, name, MFD_CLOEXEC));
#else
    (void)name;
    return -1;
#endif
}

struct ToplevelCaptureState {
    wl_shm* shm = nullptr;
    hyprland_toplevel_export_manager_v1* manager = nullptr;
    hyprland_toplevel_export_frame_v1* frame = nullptr;
    wl_shm_pool* pool = nullptr;
    wl_buffer* buffer = nullptr;
    void* mapped = MAP_FAILED;
    size_t mappedSize = 0;
    int fd = -1;
    uint32_t format = 0, width = 0, height = 0, stride = 0, flags = 0;
    bool copied = false, ready = false, failed = false;
    std::string error;
};

void ToplevelCleanup(ToplevelCaptureState* state) {
    if (state->frame) hyprland_toplevel_export_frame_v1_destroy(state->frame);
    if (state->buffer) wl_buffer_destroy(state->buffer);
    if (state->pool) wl_shm_pool_destroy(state->pool);
    if (state->mapped != MAP_FAILED) munmap(state->mapped, state->mappedSize);
    if (state->fd >= 0) close(state->fd);
    if (state->manager) hyprland_toplevel_export_manager_v1_destroy(state->manager);
    if (state->shm) wl_shm_destroy(state->shm);
}

void AllocateToplevelBuffer(ToplevelCaptureState* state) {
    if (state->width == 0 || state->height == 0 ||
        state->width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        state->height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        state->stride < state->width * 4u ||
        state->height > std::numeric_limits<size_t>::max() / state->stride) {
        state->failed = true;
        state->error = "Hyprland export returned invalid buffer dimensions";
        return;
    }
    if (state->format != WL_SHM_FORMAT_ARGB8888 &&
        state->format != WL_SHM_FORMAT_XRGB8888) {
        state->failed = true;
        state->error = "Hyprland export did not offer ARGB8888 or XRGB8888";
        return;
    }
    state->mappedSize = static_cast<size_t>(state->stride) * state->height;
    state->fd = CreateMemfd("cppgoose-toplevel");
    if (state->fd < 0 || ftruncate(state->fd, static_cast<off_t>(state->mappedSize)) != 0) {
        state->failed = true;
        state->error = "failed to allocate Hyprland export buffer";
        return;
    }
    state->mapped = mmap(nullptr, state->mappedSize, PROT_READ | PROT_WRITE,
                         MAP_SHARED, state->fd, 0);
    if (state->mapped == MAP_FAILED) {
        state->failed = true;
        state->error = "failed to map Hyprland export buffer";
        return;
    }
    state->pool = wl_shm_create_pool(state->shm, state->fd,
                                      static_cast<int>(state->mappedSize));
    if (!state->pool) {
        state->failed = true;
        state->error = "failed to create Hyprland export SHM pool";
        return;
    }
    state->buffer = wl_shm_pool_create_buffer(
        state->pool, 0, static_cast<int32_t>(state->width),
        static_cast<int32_t>(state->height), static_cast<int32_t>(state->stride),
        state->format);
    if (!state->buffer) {
        state->failed = true;
        state->error = "failed to create Hyprland export SHM buffer";
    }
}

void toplevel_buffer(void* data, hyprland_toplevel_export_frame_v1*, uint32_t format,
                     uint32_t width, uint32_t height, uint32_t stride) {
    auto* state = static_cast<ToplevelCaptureState*>(data);
    if (state->buffer || state->failed) return;
    state->format = format;
    state->width = width;
    state->height = height;
    state->stride = stride;
    AllocateToplevelBuffer(state);
}
void toplevel_damage(void*, hyprland_toplevel_export_frame_v1*, uint32_t, uint32_t,
                     uint32_t, uint32_t) {}
void toplevel_flags(void* data, hyprland_toplevel_export_frame_v1*, uint32_t flags) {
    static_cast<ToplevelCaptureState*>(data)->flags = flags;
}
void toplevel_ready(void* data, hyprland_toplevel_export_frame_v1*, uint32_t, uint32_t,
                    uint32_t) {
    static_cast<ToplevelCaptureState*>(data)->ready = true;
}
void toplevel_failed(void* data, hyprland_toplevel_export_frame_v1*) {
    auto* state = static_cast<ToplevelCaptureState*>(data);
    state->failed = true;
    state->error = "Hyprland failed to export the toplevel";
}
void toplevel_linux_dmabuf(void*, hyprland_toplevel_export_frame_v1*, uint32_t, uint32_t,
                           uint32_t) {}
void toplevel_buffer_done(void* data, hyprland_toplevel_export_frame_v1* frame) {
    auto* state = static_cast<ToplevelCaptureState*>(data);
    if (!state->buffer) {
        state->failed = true;
        state->error = "Hyprland export did not offer a SHM buffer";
    } else if (!state->copied) {
        hyprland_toplevel_export_frame_v1_copy(frame, state->buffer, 1);
        state->copied = true;
    }
}

const hyprland_toplevel_export_frame_v1_listener kToplevelListener = {
    toplevel_buffer, toplevel_damage, toplevel_flags, toplevel_ready,
    toplevel_failed, toplevel_linux_dmabuf, toplevel_buffer_done};

void toplevel_registry_global(void* data, wl_registry* registry, uint32_t name,
                              const char* interface, uint32_t version) {
    auto* state = static_cast<ToplevelCaptureState*>(data);
    if (std::strcmp(interface, wl_shm_interface.name) == 0 && !state->shm) {
        state->shm = static_cast<wl_shm*>(wl_registry_bind(
            registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface,
                           hyprland_toplevel_export_manager_v1_interface.name) == 0 &&
               !state->manager) {
        state->manager = static_cast<hyprland_toplevel_export_manager_v1*>(
            wl_registry_bind(registry, name,
                             &hyprland_toplevel_export_manager_v1_interface,
                             std::min(version, 2u)));
    }
}
const wl_registry_listener kToplevelRegistryListener = {
    toplevel_registry_global, registry_global_remove};

}  // namespace

GdkPixbuf* CaptureHyprlandToplevel(uint32_t handle, std::string* errorOut) {
    if (errorOut) errorOut->clear();
    if (handle == 0) {
        SetError(errorOut, "invalid Hyprland toplevel handle");
        return nullptr;
    }
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        SetError(errorOut, "failed to connect to the Wayland display");
        return nullptr;
    }
    ToplevelCaptureState state;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kToplevelRegistryListener, &state);
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0 ||
        !state.shm || !state.manager) {
        ToplevelCleanup(&state);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        SetError(errorOut, "Hyprland toplevel export protocol is unavailable");
        return nullptr;
    }
    state.frame = hyprland_toplevel_export_manager_v1_capture_toplevel(
        state.manager, 0, handle);
    if (!state.frame) {
        ToplevelCleanup(&state);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        SetError(errorOut, "failed to create Hyprland toplevel export frame");
        return nullptr;
    }
    hyprland_toplevel_export_frame_v1_add_listener(
        state.frame, &kToplevelListener, &state);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!state.ready && !state.failed) {
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            state.failed = true;
            state.error = "Wayland display disconnected during toplevel capture";
            break;
        }
        pollfd socketFd{wl_display_get_fd(display), POLLIN, 0};
        const int waitResult = poll(&socketFd, 1, 5);
        if (waitResult < 0 && errno != EINTR) {
            state.failed = true;
            state.error = "poll failed during Hyprland toplevel capture";
            break;
        }
        if (waitResult > 0 && (socketFd.revents & POLLIN) &&
            wl_display_dispatch(display) < 0) {
            state.failed = true;
            state.error = "Wayland display disconnected during toplevel capture";
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            state.failed = true;
            state.error = "timed out waiting for Hyprland toplevel capture";
            break;
        }
    }

    GdkPixbuf* pixbuf = nullptr;
    if (state.ready && !state.failed && state.mapped != MAP_FAILED) {
        pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8,
                                static_cast<int>(state.width),
                                static_cast<int>(state.height));
        if (pixbuf) {
            const int dstStride = gdk_pixbuf_get_rowstride(pixbuf);
            auto* dst = gdk_pixbuf_get_pixels(pixbuf);
            const auto* src = static_cast<const uint8_t*>(state.mapped);
            for (uint32_t y = 0; y < state.height; ++y) {
                const uint32_t sourceY =
                    (state.flags & HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_FLAGS_Y_INVERT)
                        ? state.height - 1 - y : y;
                const auto* srcRow = src + static_cast<size_t>(sourceY) * state.stride;
                auto* dstRow = dst + static_cast<size_t>(y) * dstStride;
                for (uint32_t x = 0; x < state.width; ++x) {
                    const auto* p = srcRow + x * 4;
                    dstRow[x * 4] = p[2];
                    dstRow[x * 4 + 1] = p[1];
                    dstRow[x * 4 + 2] = p[0];
                    dstRow[x * 4 + 3] =
                        state.format == WL_SHM_FORMAT_ARGB8888 ? p[3] : 255;
                }
            }
        } else {
            state.failed = true;
            state.error = "failed to allocate toplevel capture pixbuf";
        }
    }
    const std::string failure =
        state.error.empty() ? "Hyprland toplevel capture failed" : state.error;
    ToplevelCleanup(&state);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    if (!pixbuf) SetError(errorOut, failure);
    return pixbuf;
}

namespace {

// Removes the per-capture scratch directory and its files on every return path.
struct NiriScratch {
    std::string dir;
    std::vector<std::string> files;
    ~NiriScratch() {
        for (const std::string& file : files) unlink(file.c_str());
        if (!dir.empty()) rmdir(dir.c_str());
    }
};

// Runs `niri msg action screenshot-window` with an argv vector (no shell).
// niri's stderr goes to logPath so a failure can report niri's own message.
bool RunNiriScreenshot(uint64_t windowId, const std::string& pngPath,
                       const std::string& logPath, std::string* errorOut) {
    std::vector<std::string> args = {
        "niri", "msg", "action", "screenshot-window",
        "--id", std::to_string(windowId),
        "--write-to-disk", "true",
        "--show-pointer", "false",
        "--path", pngPath,
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (int rc = posix_spawn_file_actions_init(&actions); rc != 0) {
        SetError(errorOut, std::string("could not prepare niri spawn: ") + std::strerror(rc));
        return false;
    }
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, logPath.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = 0;
    const int spawnRc = posix_spawnp(&pid, "niri", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawnRc != 0) {
        SetError(errorOut, std::string("could not run niri: ") + std::strerror(spawnRc));
        return false;
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0 && errno != EINTR) {
            SetError(errorOut, "waiting for niri screenshot-window failed");
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            SetError(errorOut, "niri screenshot-window timed out");
            return false;
        }
        const timespec pause{0, 10000000};
        nanosleep(&pause, nullptr);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;

    std::string message = "niri screenshot-window failed";
    std::ifstream log(logPath);
    std::string detail((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == ' ')) detail.pop_back();
    if (detail.size() > 300) detail.resize(300);
    if (!detail.empty()) message += ": " + detail;
    SetError(errorOut, message);
    return false;
}

}  // namespace

GdkPixbuf* CaptureNiriWindow(uint64_t windowId, std::string* errorOut) {
    if (errorOut) errorOut->clear();
    if (windowId == 0) {
        SetError(errorOut, "niri window id is invalid");
        return nullptr;
    }

    std::string templ = std::string(g_get_tmp_dir()) + "/goose-niri-XXXXXX";
    std::vector<char> templBuf(templ.begin(), templ.end());
    templBuf.push_back('\0');
    if (!mkdtemp(templBuf.data())) {
        SetError(errorOut, "could not create niri screenshot directory");
        return nullptr;
    }

    NiriScratch scratch;
    scratch.dir = templBuf.data();
    const std::string pngPath = scratch.dir + "/window.png";
    const std::string logPath = scratch.dir + "/niri.log";
    scratch.files = {pngPath, logPath};

    if (!RunNiriScreenshot(windowId, pngPath, logPath, errorOut)) return nullptr;

    // niri acknowledges the action before the compositor finishes encoding the
    // PNG, so accept the file only once its size is stable and it decodes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    off_t lastSize = -1;
    std::string decodeError;
    for (;;) {
        struct stat st;
        const off_t size = stat(pngPath.c_str(), &st) == 0 ? st.st_size : -1;
        if (size > 0 && size == lastSize) {
            GError* error = nullptr;
            GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(pngPath.c_str(), &error);
            if (pixbuf) return pixbuf;
            decodeError = error ? error->message : "decode failed";
            if (error) g_error_free(error);
        }
        lastSize = size;
        if (std::chrono::steady_clock::now() >= deadline) break;
        const struct timespec pause = {0, 20 * 1000 * 1000};
        nanosleep(&pause, nullptr);
    }
    SetError(errorOut, "niri screenshot never became readable" +
                           (decodeError.empty() ? std::string() : ": " + decodeError));
    return nullptr;
}
