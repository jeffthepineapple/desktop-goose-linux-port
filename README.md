# Desktop Goose Linux Port (CppGoose)

Version: 0.31

A cross-compositor Linux desktop pet application that renders animated geese roaming freely across your screen in a click-through overlay. This repository builds and runs as a standalone C++17 app using GTK4, with cursor control backends for Hyprland, wlroots-based compositors, and X11/XWayland.

This codebase is the maintained Linux/Wayland/X11 port of the classic desktop goose behavior, including collectible meme items, note messages, footprints, and cursor chase/snatch mechanics.

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Requirements](#requirements)
- [Building from Source](#building-from-source)
- [Running](#running)
- [Configuration](#configuration)
- [Project Structure](#project-structure)
- [Cursor Backend Selection](#cursor-backend-selection)
- [State Machine](#state-machine)
- [Assets](#assets)
- [Known Limitations](#known-limitations)
- [Contributing](#contributing)
- [License](#license)

---

## Overview

CppGoose is a Linux port of the classic desktop goose concept. Each goose lives in a transparent, click-through overlay window that sits above your normal desktop content. The geese roam between monitors, pick up and drop meme images and notepad-style messages, leave footprints that fade over time, and can chase or snatch your cursor when the mood strikes.

The application supports multiple simultaneous geese, each with its own name and independently running behavior state machine. A control panel provides per-goose tuning and global settings at runtime.

---

## Features

**Core behavior**
- One or more geese rendered in transparent overlay windows covering all connected monitors
- Smooth animation with directional sprite rigging
- Wandering movement with collision-aware path selection
- Footprint trails that age out over time

**Interactivity**
- Geese can chase the cursor across the screen
- Cursor snatching: a goose can grab and drag the cursor briefly before releasing it
- Dropped item system: geese pick up, carry, and drop meme images and text notes
- Honking on a configurable timer

**Multi-monitor support**
- Monitors are discovered at startup
- Overlay windows are created per monitor
- Geese can roam across monitor boundaries

**Control panel**
- GTK4 settings window for live configuration
- Per-goose controls: name, speed, behavior toggles
- Debug overlay toggle for visualizing state, hitboxes, and item zones

**Configuration persistence**
- Settings are read from and written to `config.ini` in the working directory
- All tunable values survive restarts

---

## Requirements

### Build tools

- CMake 3.17 or newer
- A C++17-capable compiler (GCC 9+ or Clang 10+ recommended)
- `pkg-config`

### Runtime libraries

| Library | Purpose |
|---|---|
| GTK4 | UI toolkit and rendering |
| gtk4-layer-shell | Wayland layer-shell overlay windows |
| SDL2 | Audio playback |
| SDL2_mixer | Sound effect mixing |
| gdk-pixbuf-2.0 | Image loading for meme assets |
| wayland-client | Wayland protocol base |
| libcurl | (retained in build; unused after AI backend removal) |
| X11 | X11 cursor position queries |
| Xtst | X11 cursor movement injection |

### Optional compositor support

- **Hyprland**: full cursor control via IPC socket
- **wlroots-based compositors**: cursor movement via the `wlr-virtual-pointer-unstable-v1` Wayland protocol
- **X11**: cursor queries and movement via XTest

One of the above is required for cursor chase and snatch behavior. The application runs without cursor control but those features will be disabled.

---

## Building from Source

### 1. Install dependencies

On Arch Linux:

```
sudo pacman -S cmake gtk4 gtk4-layer-shell sdl2 sdl2_mixer gdk-pixbuf2 wayland libcurl xorg-server-devel libxtst
```

On Fedora:

```
sudo dnf install cmake gtk4-devel gtk4-layer-shell-devel SDL2-devel SDL2_mixer-devel gdk-pixbuf2-devel wayland-devel libcurl-devel libX11-devel libXtst-devel
```

On Ubuntu 24.04 or later:

```
sudo apt install cmake libgtk-4-dev libgtk4-layer-shell-dev libsdl2-dev libsdl2-mixer-dev libgdk-pixbuf-2.0-dev libwayland-dev libcurl4-openssl-dev libx11-dev libxtst-dev
```

> **Note:** `gtk4-layer-shell` may not be available in older Ubuntu/Debian repositories. Build it from source from [github.com/wmww/gtk4-layer-shell](https://github.com/wmww/gtk4-layer-shell) if your package manager does not provide it.

### 2. Configure and build

```bash
git clone https://github.com/yourname/CppGoose.git
cd CppGoose
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The compiled binary will be at `build/CppGoose`.

### 3. In-tree builds

The repository ships with some pre-generated Wayland protocol binding files. CMake will regenerate these via `wayland-scanner` if the tool is available. If `wayland-scanner` is not installed, the pre-generated files under `protocols/` will be used as-is.

---

## Running

Run the binary from the repository root so that the `Assets/` directory is resolved correctly:

```bash
./build/CppGoose
```

On first launch a name prompt will appear for the initial goose. Additional geese can be added from the control panel at any time.

### Wayland notes

The application uses `gtk4-layer-shell` to create overlay windows. Your compositor must support the `wlr-layer-shell-unstable-v1` protocol. Most wlroots-based compositors (Sway, Hyprland, river, niri) and KDE Plasma 6 support this. GNOME does not expose this protocol by default.

### X11 notes

The application can run under X11 via XWayland or a native X11 session. Overlay transparency and always-on-top behaviour depend on a compositing window manager being active (e.g., Picom, Compton, or a compositor built into your desktop environment).

---

## Configuration

Settings are persisted to `config.ini` in the working directory. The file is created automatically on first run. You can edit it by hand or use the control panel.

Selected keys:

| Key | Default | Description |
|---|---|---|
| `goose_speed` | `2.0` | Base movement speed in pixels per tick |
| `enable_cursor_chase` | `true` | Allow geese to chase the cursor |
| `enable_cursor_snatch` | `true` | Allow geese to briefly grab the cursor |
| `honk_interval_ms` | `8000` | Milliseconds between honks |
| `footprint_lifetime_ms` | `4000` | How long footprints remain visible |
| `item_lifetime_ms` | `15000` | How long dropped items remain on screen |
| `debug_overlay` | `false` | Draw debug hitboxes and state labels |

All values can also be changed live from the control panel without restarting.

---

## Project Structure

```
CppGoose/
  src/
    main.cpp                 GTK application entry point
    goose.cpp                Goose class: movement, animation, drawing, behavior
    world.cpp                Global state containers (geese, monitors, items, footprints)
    ui.cpp                   Control panel, overlay drawing, tick loop
    assets.cpp               Asset resolution, image/sound loading, item data construction
    config.cpp               INI-based config registry
    cursor_backend.cpp       Backend manager and selection logic
    hyprland.cpp             Hyprland IPC cursor backend
    wlroots_backend.cpp      wlroots virtual-pointer cursor backend
    x11_backend.cpp          X11/XTest cursor backend
    tool_manager.cpp         Intent-to-command bridge (calculator, browser launch)
  include/
    goose_math.h             Shared vector and math helpers
    ...
  Assets/
    Images/Memes/            PNG images geese can pick up and drop
    Text/NotepadMessages/    Plain text files geese can carry as notes
    Sound/NotEmbedded/       Sound effect files loaded at runtime
  protocols/
    wlr-virtual-pointer-unstable-v1.xml
                             Wayland protocol definition
  CMakeLists.txt
  config.ini                 Runtime configuration (auto-generated)
```

---

## Cursor Backend Selection

At startup, `cursor_backend.cpp` attempts to initialise backends in this order:

1. **Hyprland** — detected via the `HYPRLAND_INSTANCE_SIGNATURE` environment variable. Communicates through the Hyprland IPC socket to read and set cursor position.
2. **wlroots** — uses the `zwlr_virtual_pointer_manager_v1` Wayland global to inject pointer motion events. Available on most wlroots compositors that expose the protocol.
3. **X11** — uses `XQueryPointer` to read position and `XTestFakeMotionEvent` to move the cursor. Works on native X11 sessions and XWayland.

If none of these backends initialise successfully, the application continues without cursor interaction features. All other goose behaviors remain active.

Only one goose can hold cursor control at a time, tracked globally via `g_cursorGrabberId`.

---

## State Machine

Each goose runs an independent state machine with the following states:

| State | Description |
|---|---|
| `WANDER` | Default state. The goose walks in a chosen direction, periodically picking new targets. |
| `FETCHING` | The goose has selected a nearby item and is walking toward it to pick it up. |
| `RETURNING` | The goose is carrying an item and walking toward a drop location. |
| `CHASE_CURSOR` | The goose is actively pursuing the cursor position reported by the active backend. |
| `SNATCH_CURSOR` | The goose has reached the cursor and is temporarily controlling pointer movement. |

State transitions are evaluated each tick in `goose.cpp`. External events such as item spawning or cursor proximity trigger transitions out of `WANDER`. The goose always returns to `WANDER` after completing a fetch/return cycle or releasing cursor control.

---

## Assets

### Meme images

Place PNG files in `Assets/Images/Memes/`. The asset loader scans this directory at startup. Geese will randomly select from available images when entering the `FETCHING` state.

### Notepad messages

Place plain text files (`.txt`) in `Assets/Text/NotepadMessages/`. Each file represents one message a goose can carry and display as a floating note above its head.

### Sound effects

Sound files are loaded from `Assets/Sound/NotEmbedded/` via SDL2_mixer. Supported formats are WAV, OGG, and MP3 (depending on SDL2_mixer build options on your system). Honk sounds and other behavioral audio cues are resolved by filename convention defined in `assets.cpp`.

---

## Known Limitations

- **GNOME / Mutter**: The `wlr-layer-shell` protocol is not supported on stock GNOME. The application will not display overlay windows on GNOME without a third-party shell extension that exposes the protocol.
- **Multi-GPU / mixed DPI**: Monitor layout discovery relies on GTK4 monitor enumeration. Fractional scaling and mixed-DPI setups may produce minor positional drift in goose movement near monitor edges.
- **Wayland cursor snatching**: The wlroots virtual-pointer backend can inject relative motion but cannot read absolute cursor position without a secondary input mechanism. Absolute position queries fall back to the compositor IPC (Hyprland) or X11 on mixed sessions.
- **No Wayland screencopy**: Geese do not interact with window content. They walk over the top of windows without any awareness of what is underneath.

---

## Contributing

Contributions are welcome. A few things to keep in mind:

- The main orchestration file is `src/ui.cpp`. It is large and most feature additions will touch it. Try to keep new logic in dedicated source files where possible and call into them from `ui.cpp`.
- `src/world.cpp` is the canonical home for any new global state. Avoid scattering new globals into other translation units.
- The build currently has a mix of source files and pre-built artifacts in the tree. Do not commit build outputs or generated Wayland binding files that are already tracked under `protocols/`.
- Run a `clang-format` pass (`.clang-format` style file to be added) before submitting patches.

To open an issue or pull request, use the GitHub interface for this repository.

---

## License

This project is released under the MIT License. See [LICENSE](LICENSE) for the full text.

Third-party assets bundled under `Assets/` may carry their own licenses. Review individual files before redistribution.
