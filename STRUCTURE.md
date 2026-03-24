# Workspace Structure

## Top Level

### Source and config

- `main.cpp`
  GTK application entry point.

- `src/`
  Primary application implementation files.

- `include/`
  Public/shared headers for app modules.

- `protocols/`
  Wayland protocol XML source used for code generation.

- `Assets/`
  Runtime content: memes, text notes, sounds, mods, and related assets.

- `config.ini`
  Persisted runtime settings written by the config system.

### Build system and generated files in root

- `CMakeLists.txt`
  Main build definition.

- `Makefile`
- `CMakeCache.txt`
- `CTestTestfile.cmake`
- `cmake_install.cmake`
- `compile_commands.json`

These are generated build artifacts and not source-of-truth project docs.

### Generated/built binaries and protocol outputs in root

- `CppGoose`
  Built executable already present in the workspace.

- `gemini_client_test`
  Built test executable already present in the workspace.

- `wlr-virtual-pointer-unstable-v1-client-protocol.h`
- `wlr-virtual-pointer-unstable-v1-protocol.c`

These are generated outputs, not hand-maintained source files.

### Miscellaneous

- `mpv-shot0001.jpg`
- `mpv-shot0002.jpg`

Likely local screenshots/debug leftovers, not code assets used by the build.

## `src/`

- `assets.cpp`
  Asset discovery, sound loading, meme/text item creation.

- `config.cpp`
  Registry-backed config initialization plus `config.ini` load/save.

- `cursor_backend.cpp`
  Backend manager and backend registration/selection.

- `gemini_client.cpp`
  Gemini HTTP request/response handling.

- `goose.cpp`
  Core goose simulation, state machine, physics, and drawing.

- `hyprland.cpp`
  Hyprland IPC cursor backend.

- `tool_manager.cpp`
  Speech/UI-triggered external tool launching.

- `ui.cpp`
  Control panel UI, overlay creation, draw loop, tick handling, debug overlays, whisper widgets.

- `whisper_manager.cpp`
  Audio capture, whisper inference, transcript handling, and intent routing.

- `wlroots_backend.cpp`
  Wayland virtual pointer backend.

- `world.cpp`
  Global runtime collections and shared world state.

- `x11_backend.cpp`
  X11/XTest cursor backend.

## `include/`

- `assets.h`
  Asset manager declarations and asset-root globals.

- `config.h`
  Config structs, registry declarations, and globals.

- `cursor_backend.h`
  Abstract backend interface and backend manager.

- `gemini_client.h`
  Gemini request and response-parse API.

- `goose.h`
  `Goose` class, state enum, rig/foot data.

- `goose_math.h`
  Shared vector/math helpers.

- `hyprland.h`
  Hyprland backend interface.

- `items.h`
  `ItemData` and `DroppedItem`.

- `tool_manager.h`
  Speech intent and tool execution manager.

- `ui.h`
  Overlay/control-panel functions and tick/draw hooks.

- `whisper_manager.h`
  Whisper visualizer state and manager API.

- `wlroots_backend.h`
  wlroots backend interface.

- `world.h`
  Global world state declarations.

- `x11_backend.h`
  X11 backend interface.

## `Assets/`

Observed subtree layout:

- `Assets/Images/`
- `Assets/Images/Memes/`
- `Assets/Images/OtherGfx/`
- `Assets/Text/`
- `Assets/Text/NotepadMessages/`
- `Assets/Sound/`
- `Assets/Sound/Music/`
- `Assets/Sound/NotEmbedded/`
- `Assets/Mods/`
- `Assets/Mods/Autumn/`

Notes:

- `assets.cpp` specifically scans `Images/Memes`, `Text/NotepadMessages`, and `Sound/NotEmbedded`.
- this snapshot does include sample meme images, note text files, and sound files under those scanned folders.

## `protocols/`

- `wlr-virtual-pointer-unstable-v1.xml`
  Source XML for generated Wayland client header/C code used by `wlroots_backend.cpp`.

## Generated / build-artifact directories

- `CMakeFiles/`
  Root-level generated CMake state and object files.

- `build/`
  Separate build tree with compiled targets and generated files.

- `build/CMakeFiles/`
  Generated CMake internals for the out-of-tree build.

- `build/third_party/whisper.cpp/`
  Build output for the whisper dependency.

Important distinction:

- `build/third_party/whisper.cpp/` is build output, not a substitute for the missing top-level `third_party/whisper.cpp/` source tree referenced by `CMakeLists.txt`.

## Structural Mismatches Worth Remembering

- `CMakeLists.txt` references `third_party/whisper.cpp`, but that directory is absent from the top-level source tree.
- `CMakeLists.txt` defines `tests/gemini_client_test.cpp`, but there is no visible `tests/` directory in the current workspace.
- Compiled outputs for both the whisper dependency and `gemini_client_test` exist, so this workspace likely came from a previously fuller source tree or a partially copied build environment.
