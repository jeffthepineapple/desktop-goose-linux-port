# Workspace Memory

## What This Project Is

This workspace is a C++17 desktop-pet application built with GTK4 and CMake. It renders one or more animated geese in transparent overlay windows, drives their behavior with a small simulation/state machine, and optionally integrates:

- cursor control across Hyprland, wlroots Wayland, or X11
- speech capture/transcription via `whisper.cpp`
- Gemini API calls for short text responses
- simple external tool launching such as calculator/browser actions

The main executable target is `CppGoose`.

## Actual Workspace Shape

This snapshot is not a Git checkout. It looks like a delivered workspace with both source files and build artifacts already present in the top level.

Notable source-vs-workspace mismatches:

- `CMakeLists.txt` expects `third_party/whisper.cpp`, but there is no top-level `third_party/` directory in the current tree.
- `CMakeLists.txt` also defines `tests/gemini_client_test.cpp`, but that source file is not present even though compiled test artifacts exist under `build/CMakeFiles/gemini_client_test.dir/tests/`.
- Root-level generated files such as `CMakeCache.txt`, `Makefile`, `CppGoose`, generated Wayland protocol files, and screenshots are checked into the workspace snapshot or left behind from a previous build.

Treat this as a mixed source/build directory, not a clean repo root.

## Runtime Model

Startup path:

1. `main.cpp` creates a `GtkApplication`.
2. On activation it calls:
   - `Config_InitRegistry()`
   - `setup_overlay_window(app)`
   - `activate_control_panel(app)`
   - `g_backendManager.Init()`
   - `ShowInitialNamePrompt(app)`
3. `setup_overlay_window()` discovers monitors, initializes assets, creates one transparent layer-shell overlay window per monitor, and installs a 16 ms tick callback for each drawing area.
4. `on_tick()` advances global time, updates every goose, expires dropped items/footprints, queues redraw, and refreshes input regions.
5. `draw_overlay()` renders footprints, dropped items, geese, whisper UI, and optional debug overlays.

The simulation is largely driven by global state in `world.cpp` and per-goose logic in `goose.cpp`.

## Main State Containers

`src/world.cpp` owns most global runtime collections and shared state:

- `g_geese`
- `g_monitors`
- `g_droppedItems`
- `g_footprints`
- `g_nextId`
- `g_screenWidth` / `g_screenHeight`
- `g_selectedGooseId`
- `g_uiLog`
- `g_cursorGrabberId`

This is effectively the app-wide world model.

## Module Ownership

### Core simulation

- `src/goose.cpp`
  Owns the `Goose` class behavior, movement, animation rigging, drawing, item carrying, cursor chase/snatch logic, and honk timing.

- `src/world.cpp`
  Owns global containers and lookup helpers.

- `include/goose_math.h`
  Shared vector/math helpers used across movement and rendering.

### UI and app shell

- `src/ui.cpp`
  Largest orchestration file. Builds the control panel, overlay drawing, tick loop, monitor overlays, per-goose controls, debug UI, and whisper status display.

- `main.cpp`
  Thin GTK entry point.

### Assets and content

- `src/assets.cpp`
  Resolves asset root, loads sound effects, scans meme/text content folders, and constructs `ItemData` objects.

- `Assets/`
  Runtime content folder. Relevant subtrees include:
  - `Assets/Images/Memes`
  - `Assets/Text/NotepadMessages`
  - `Assets/Sound/NotEmbedded`

### Config

- `src/config.cpp`
  Implements a simple registry-based config system persisted to `config.ini`.

- `config.ini`
  Runtime config file written in the workspace root.

### Cursor backends

- `src/cursor_backend.cpp`
  Backend manager and backend selection.

- `src/hyprland.cpp`
  Hyprland IPC backend. Reads cursor position and moves cursor through compositor IPC.

- `src/wlroots_backend.cpp`
  Wayland virtual-pointer backend using generated wlroots protocol bindings.

- `src/x11_backend.cpp`
  X11/XTest backend for pointer querying and motion.

Backend init order is Hyprland, wlroots, then X11.

### Speech / AI / tools

- `src/whisper_manager.cpp`
  SDL audio capture, whisper model loading, speech segmentation/transcription, intent extraction, and action dispatch.

- `src/gemini_client.cpp`
  Raw Gemini HTTP client using libcurl and `nlohmann::json` from the whisper dependency tree.

- `src/tool_manager.cpp`
  Lightweight intent-to-command bridge for calculator/browser launching.

## State Machine Notes

`GooseState` values:

- `WANDER`
- `FETCHING`
- `RETURNING`
- `CHASE_CURSOR`
- `SNATCH_CURSOR`

Behavior highlights:

- geese can wander, fetch memes/notes, and return/drop them
- cursor chase uses whichever backend exposes cursor capabilities
- one goose at a time can own the cursor via `g_cursorGrabberId`
- footprints and dropped items age out over time

## Build and Dependency Notes

Build system:

- CMake 3.17+
- C++17
- executable target: `CppGoose`
- test target: `gemini_client_test`

Declared native dependencies:

- GTK4
- gtk4-layer-shell
- SDL2
- SDL2_mixer
- gdk-pixbuf-2.0
- wayland-client
- libcurl
- X11
- Xtst

Vendored/generated dependency assumptions:

- `third_party/whisper.cpp` is expected by source CMake, but missing from the current top-level tree
- generated Wayland files are produced from `protocols/wlr-virtual-pointer-unstable-v1.xml`

## Practical Caveats For Later Work

- `ui.cpp` is the main integration hub and is large; most feature work will touch it.
- The workspace mixes generated files with source, so avoid assuming every top-level file should be edited.
- Build reproducibility is questionable until the missing `third_party/whisper.cpp` and missing `tests/gemini_client_test.cpp` source are reconciled.
- `src/gemini_client.cpp` includes `../third_party/whisper.cpp/examples/json.hpp`, so the missing source dependency affects more than speech support.
