# Visage User Guide

> A practical, example-driven guide to integrating and using Visage — a C++17, GPU-accelerated
> UI/2D-graphics framework — in your own application or audio plugin.

This guide is written for **developers** who want to add Visage to a C++ project: application
authors building a native desktop UI, and audio-plugin authors embedding a GUI inside a DAW host.
Every claim below is sourced from the Visage source tree at `Z:/Github/visage` (CMake scripts,
headers, and example code), cross-checked against the project's validated code analysis.

---

## Table of Contents

1. [What You'll Learn](#what-youll-learn)
2. [Core Concepts](#core-concepts)
3. [Getting Started: Adding Visage to Your CMake Project](#getting-started-adding-visage-to-your-cmake-project)
4. [Building Visage From Source](#building-visage-from-source)
5. [Build Options Reference](#build-options-reference)
6. [Running the Test Suite](#running-the-test-suite)
7. [Your First Application: Walking Through `basic.cpp`](#your-first-application-walking-through-basiccpp)
8. [Tour of the Example Gallery](#tour-of-the-example-gallery)
9. [Embedding Visage in a Plugin Host (CLAP Walkthrough)](#embedding-visage-in-a-plugin-host-clap-walkthrough)
10. [Theming and Palette Customization](#theming-and-palette-customization)
11. [Embedding Custom Assets (Fonts, Icons, Images)](#embedding-custom-assets-fonts-icons-images)
12. [Writing Custom Shaders](#writing-custom-shaders)
13. [Security Note: `spawnChildProcess`](#security-note-spawnchildprocess)
14. [Troubleshooting](#troubleshooting)
15. [Where to Go Next](#where-to-go-next)

---

## What You'll Learn

By the end of this guide you will be able to:

- Pull Visage into a CMake project and link against the `visage::visage` target.
- Build Visage from source on Windows, macOS, and Linux, and understand what each build option does.
- Run the project's test suite with `ctest`.
- Write and run a minimal Visage application.
- Navigate the in-repo example gallery to find a reference for the feature you need.
- Embed a Visage-drawn UI inside a plugin host window (CLAP pattern, portable to VST/AU-style hosts).
- Customize the visual theme via Visage's palette system.
- Embed your own fonts, icons, and images at build time.
- Author a custom bgfx shader (`.sc`) for a post-effect.
- Use `spawnChildProcess` safely, understanding its argument-parsing limitations.

## Core Concepts

Before diving into code, it helps to know the four ideas that recur throughout Visage:

- **`Canvas`** — the per-frame, immediate-mode drawing API. Inside a `draw(Canvas&)` call (or an
  `onDraw()` lambda) you call methods like `canvas.fill(...)`, `canvas.circle(...)`, and
  `canvas.text(...)` to describe what should appear on screen this frame.
- **`Frame`** — the retained-mode building block of the UI tree. Every widget (buttons, text
  editors, your own custom views) is a `Frame` subclass. Frames own layout, input-event dispatch,
  palette/theme lookup, and post-effects.
- **`Window`** — the OS-native window abstraction (Win32/Cocoa/X11/Emscripten under one interface).
  Most applications don't touch `Window` directly; `ApplicationWindow` manages it for you.
- **`ApplicationWindow`** (built on `ApplicationEditor`, itself a `Frame`) — the top-level object
  most apps construct. It owns a `Canvas`, manages an OS `Window`, and is the root of your `Frame`
  tree.

Visage is **single-threaded on the UI side**: all `Frame`/`Canvas`/`Layout` work runs on the thread
that calls `runEventLoop()`. The library also starts its own internal bgfx render thread, but that
is transparent to application code — you never need to synchronize your own drawing calls against
it.

---

## Getting Started: Adding Visage to Your CMake Project

### Prerequisites

- **CMake 3.17 or newer** (`cmake_minimum_required(VERSION 3.17)`, `CMakeLists.txt:1`).
- **A C++17 compiler.** Visage sets `CMAKE_CXX_STANDARD 17` itself if your project hasn't already
  defined it (`CMakeLists.txt:9-11`) — you do not need C++20.
- Per-platform system libraries, matching what Visage's own CI installs:
  - **Windows**: Visual Studio 2022 (generator `"Visual Studio 17 2022"`) or MinGW.
  - **macOS**: Xcode toolchain; deployment target defaults to `10.15` unless your project sets
    `CMAKE_OSX_DEPLOYMENT_TARGET` first.
  - **Linux**: X11 and Vulkan development packages. Visage's own CI installs:
    `mesa-common-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev
    libglib2.0-0 libfontconfig1-dev mesa-vulkan-drivers libvulkan1 vulkan-tools
    vulkan-validationlayers` — install the equivalents for your distribution before building.
  - **Emscripten**: the `emsdk` toolchain, invoked via `emcmake`/`emmake`.

### Add Visage via `FetchContent`

The simplest way to consume Visage is to fetch it directly in your project's `CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(visage
  GIT_REPOSITORY https://github.com/VitalAudio/visage.git
  GIT_TAG main   # pin to a specific tag/commit for reproducible builds
)
FetchContent_MakeAvailable(visage)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE visage::visage)
```

`visage::visage` is an `ALIAS` for the internal `visage` target (`CMakeLists.txt:142`), so this is
the only target name you need to know.

### Or, link against an installed copy

If Visage has already been built and installed on the machine (`cmake --install`), use
`find_package` instead:

```cmake
find_package(visage REQUIRED CONFIG)
target_link_libraries(MyApp PRIVATE visage::visage)
```

This works because the top-level build installs a generated `visageConfig.cmake` (from
`visageConfig.cmake.in` via `configure_package_config_file`, `CMakeLists.txt:247-257`) to
`<prefix>/lib/cmake/visage`, which is what `find_package(visage)` locates.

> **Note**: When Visage is fetched with its default options, it pulls in bgfx (pinned to tag
> `v1.129.8958-499`) and FreeType (pinned to `VER-2-14-1`) itself via `FetchContent` — you don't
> need to install these separately unless you want to use system-installed copies (see
> `VISAGE_SYSTEM_BGFX` / `VISAGE_SYSTEM_FREETYPE` below).

---

## Building Visage From Source

If you want to build the Visage repository itself (to run its examples, its test suite, or to
develop against a local checkout), the general pattern — identical across the four native
platforms in Visage's own CI — is:

```bash
mkdir cmake_build && cd cmake_build
cmake <platform-specific generator/options> ..
cmake --build . [--config Release|Debug] --parallel
```

### Platform-specific configure lines (from Visage's CI workflows)

- **Linux**:
  ```bash
  cmake -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DVISAGE_ADDRESS_SANITIZER=ON \
        -DVISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING=ON -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G "Unix Makefiles" ..
  ```
- **macOS**:
  ```bash
  cmake "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" "-DVISAGE_ADDRESS_SANITIZER=ON" \
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=10.15" -G "Xcode" ..
  ```
- **Windows (MSVC)**:
  ```bash
  cmake -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Debug -A x64 ..
  ```
- **Windows (MinGW)**:
  ```bash
  cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
  ```

### Using `build.bat` as a reference

The repository ships a `build.bat` at its root that shows a real-world configuration using
**system-installed** bgfx and FreeType (rather than the default `FetchContent` build) plus a
shared-library output:

```bat
cmake -B build ^
  -G Ninja ^
  -D CMAKE_INSTALL_PREFIX=%INSTALL_DIR% ^
  -D CMAKE_PREFIX_PATH=%INSTALL_DIR% ^
  -D BUILD_SHARED_LIBS=ON ^
  -D VISAGE_BUILD_EXAMPLES=ON ^
  -D VISAGE_BUILD_TESTS=OFF ^
  -D VISAGE_SYSTEM_BGFX=ON ^
  -D VISAGE_SYSTEM_FREETYPE=ON ^
  -D bgfx_DIR=<path-to-installed-bgfx>\lib\cmake\bgfx ^
  -D Freetype_DIR=<path-to-installed-freetype>\lib\cmake\freetype

cmake --build build --config %BUILD_TYPE% --target install
```

> **Note**: `build.bat`'s `INSTALL_DIR`, `bgfx_DIR`, and `Freetype_DIR` values are hardcoded to
> paths on the maintainer's own development machine. Treat this script as a **template**, not a
> drop-in command — replace those paths with locations on your own system before running it, or
> simply use the plain `cmake -B build ..` invocation above if you're happy with the default
> `FetchContent`-built bgfx/FreeType.

### On Windows shared builds: DLL copying

When `BUILD_SHARED_LIBS=ON`, the build automatically copies `visage.dll`, `bgfx.dll`, and
`freetype.dll` next to each example executable via `visage_copy_runtime_dlls()`
(`CMakeLists.txt:98-126`), so example binaries run without manually managing `PATH`.

---

## Build Options Reference

All options are declared with CMake's `option()` command in the top-level `CMakeLists.txt`. Set
them with `-D<OPTION>=ON|OFF` on the `cmake` configure line.

| Option | Default | Effect |
|---|---|---|
| `VISAGE_AMALGAMATED_BUILD` | `ON` | Concatenates each module's `.cpp` files into a handful of generated translation units for faster clean builds, at the cost of coarser incremental-compile granularity. Turn this `OFF` if you need to step through individual source files in a debugger without amalgamated-file noise. |
| `VISAGE_ENABLE_WIDGETS` | `ON` | Builds the `visage_widgets` module (`Button`, `TextEditor`, `ColorPicker`, etc.) and links it into the `visage` target. Turn `OFF` if you only need low-level `Frame`/`Canvas` drawing and want a smaller binary. |
| `VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD` | `OFF` | Sets bgfx's `BGFX_CONFIG_MULTITHREADED=1`, letting bgfx's own submit/render stages run on a separate thread. This is a bgfx-internal threading knob only — it does **not** make `Frame`/`Canvas` thread-safe; always touch them from the UI thread regardless of this setting. |
| `VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING` | `OFF` | Prints a graphics debug log to the console in debug builds. |
| `VISAGE_ADDRESS_SANITIZER` | `OFF` | Compiles/links with AddressSanitizer (`/fsanitize=address` on MSVC, `-fsanitize=address` elsewhere). |
| `BUILD_SHARED_LIBS` | `OFF` | Builds `visage` as a shared library (`.dll`/`.so`/`.dylib`) instead of static. |
| `VISAGE_BUILD_EXAMPLES` | `ON` when Visage is the top-level CMake project, `OFF` when it's a subdirectory/dependency | Builds everything under `examples/`. |
| `VISAGE_BUILD_TESTS` | `ON` when top-level, `OFF` otherwise | Builds the Catch2-based test suite. |
| `VISAGE_SYSTEM_BGFX` | `OFF` | Uses a pre-installed bgfx/bx/bimg via `find_package` instead of fetching and building it from source. |
| `VISAGE_SYSTEM_FREETYPE` | `OFF` | Same, for FreeType. |

> **Practical implication**: because `VISAGE_BUILD_EXAMPLES` and `VISAGE_BUILD_TESTS` default to
> `OFF` when Visage is consumed as a dependency (e.g. via `FetchContent` from *your* project), you
> won't accidentally build Visage's examples or test suite as part of your own application build —
> you only get them when building the Visage repository directly as the top-level project.

When `VISAGE_SYSTEM_BGFX`/`VISAGE_SYSTEM_FREETYPE` are left `OFF` (the default), Visage fetches
pinned versions automatically: bgfx `v1.129.8958-499` and FreeType `VER-2-14-1`. If you supply
system-installed versions instead, be aware of possible version drift between what Visage was
tested against and what you provide.

---

## Running the Test Suite

Visage uses [Catch2](https://github.com/catchorg/Catch2) (fetched automatically when
`VISAGE_BUILD_TESTS=ON`) and CTest as the test runner. After configuring and building with
`VISAGE_BUILD_TESTS=ON` (the default when building the repository as the top-level project), run:

```bash
ctest --output-on-failure
```

from inside your build directory. This is exactly the invocation Visage's own CI uses on every
platform after the build step completes.

A few platform notes carried over from CI, useful if you hit unexpected failures:

- **Linux CI** runs tests under AddressSanitizer with `ASAN_OPTIONS=detect_leaks=0` exported first
  (leak detection is intentionally suppressed in CI; consider enabling it locally if you want
  stricter checks).
- **macOS CI** builds a universal `arm64;x86_64` binary with `VISAGE_ADDRESS_SANITIZER=ON`.
- A separate `format.yml` CI workflow enforces `clang-format` compliance. This is **not** part of
  the `ctest` run — it's a formatting-diff check — but is worth running locally (`clang-format
  --dry-run` or your editor's integration) before submitting a pull request.

---

## Your First Application: Walking Through `basic.cpp`

The smallest complete Visage program lives at `examples/Basic/basic.cpp`. Here it is in full:

```cpp
#include <visage/app.h>

int runExample() {
  visage::ApplicationWindow app;

  app.onDraw() = [&app](visage::Canvas& canvas) {
    canvas.setColor(0xff000066);
    canvas.fill(0, 0, app.width(), app.height());

    float circle_radius = app.height() * 0.1f;
    float x = app.width() * 0.5f - circle_radius;
    float y = app.height() * 0.5f - circle_radius;
    canvas.setColor(0xff00ffff);
    canvas.circle(x, y, 2.0f * circle_radius);
  };

  app.setTitle("Visage Basic Example");
  app.show(800, 600);
  app.runEventLoop();
  return 0;
}
```

Step by step:

1. **`visage::ApplicationWindow app;`** — constructs the top-level app object. It is not yet
   visible and owns no OS window until `show()` is called.
2. **`app.onDraw() = [&app](visage::Canvas& canvas) { ... };`** — registers a per-frame draw
   callback. `onDraw()` is a `CallbackList` inherited from `Frame`; assigning a lambda here is
   equivalent to overriding `virtual void draw(Canvas&)` in a subclass — both mechanisms invoke the
   same drawing path, so pick whichever fits your code structure (lambda for simple apps,
   subclassing for more structured widget code, as `examples/MultiWindow` does).
3. Inside the callback: `canvas.setColor(0xff000066)` sets the active fill/stroke color (as an
   ARGB hex literal), and `canvas.fill(0, 0, app.width(), app.height())` fills that whole
   rectangle — a common "clear the background" idiom. Numeric arguments like these are always
   interpreted as **logical pixels** (scaled by the current DPI factor internally).
4. `canvas.circle(x, y, 2.0f * circle_radius)` draws a filled circle whose bounding box starts at
   `(x, y)` with the given diameter (`2 * radius`).
5. **`app.setTitle(...)`** sets the OS window title.
6. **`app.show(800, 600)`** creates the native OS window at 800x600 logical pixels and makes it
   visible. This is one of several `show()` overloads (see below); this one uses fixed numeric
   dimensions.
7. **`app.runEventLoop()`** blocks, pumping the native OS event loop, dispatching input events into
   the `Frame` tree and calling your `onDraw()` callback once per redrawn frame, until the window is
   closed.

### `ApplicationWindow::show()` overloads

`ApplicationWindow` (which extends `ApplicationEditor`, itself a `Frame`) exposes several `show()`
variants depending on how you want to size/parent the window:

```cpp
void show();                                                      // reuse last-set dimensions/position
void show(void* parent_window);                                   // embed into a host window (plugin mode)
void show(const Dimension& width, const Dimension& height, void* parent_window);
void show(const Dimension& width, const Dimension& height);
void show(const Dimension& x, const Dimension& y, const Dimension& width, const Dimension& height);
void showMaximized();
```

The `void* parent_window` overloads are what plugin hosts use to embed Visage inside another
application's window — see [Embedding Visage in a Plugin Host](#embedding-visage-in-a-plugin-host-clap-walkthrough)
below.

### Building and running it

If you've configured Visage as the top-level project with `VISAGE_BUILD_EXAMPLES=ON` (the
default), the `Basic` example builds as its own target. After `cmake --build .`, run the produced
executable (e.g. `ExampleBasic` or similar, depending on generator/config) from your build
directory — it opens an 800x600 window with a dark-blue background and a cyan circle centered in
it.

---

## Tour of the Example Gallery

The `examples/` directory is the best reference material in the repository — each example
isolates one feature area. All examples share a small cross-platform `main`/`WinMain` shim in
`examples/main.cpp`.

| Example | What it demonstrates |
|---|---|
| **Basic** | Minimal `ApplicationWindow` + `onDraw()` + `show()`/`runEventLoop()` — start here. |
| **BlendModes** | Draws overlapping RGB circles with `canvas.circle(...)` under different `BlendMode`s to visually compare compositing modes. |
| **Bloom** | Draws a ring of circles with a runtime-selectable `BloomPostEffect`/custom shader post-effect via a `PostEffectSelector` frame. |
| **BringYourOwnWindow** | Bypasses `ApplicationWindow`/`ApplicationEditor` entirely: creates a raw `visage::Window` via `createWindow(800, 800)`, initializes a `Renderer` and bare `Canvas` manually, and calls `canvas.pairToWindow(...)` before running the native event loop directly. The reference for using Visage's lower-level rendering API without the app-framework layer. |
| **ClapPlugin** | A full CLAP audio-plugin GUI: creates an `ApplicationWindow` in `guiCreate()`, embeds it into the host's window handle in `guiSetParent()`, tears it down in `guiDestroy()`. The canonical embedding-in-a-host reference — see the dedicated walkthrough below. |
| **Gradients** | Custom `Gradient` construction, including sampling perceptual color spaces (OkLab) and comparing against the built-in `Gradient::kViridis` preset. |
| **Layout** | Builds a wrapping flex grid of `Frame`s using `setFlexLayout(true)` and `layout().setPadding/setFlexGap/setFlexWrap/setFlexReverseDirection/setFlexWrapReverse`. The primary `Layout`/flexbox reference. |
| **LiveShaderEditing** | Embeds the `visage_widgets::ShaderEditor` widget to hot-edit and recompile a fragment shader against a live-rendered scene. |
| **MouseEvents** | A minimal `Frame` overriding `mouseDown`/`mouseDrag`/etc. to track and visualize cursor position and button state, plus `visage_ui/popup_menu.h` usage. |
| **MultiWindow** | Opens multiple independent `ApplicationWindow` instances simultaneously, each with its own draw callback — the reference for multi-top-level-window apps. |
| **Paths** | Builds and fills/strokes arbitrary vector `Path`s (e.g. a star polygon via `moveTo`/`lineTo`/`close`) — the `Path`/`Canvas::fill(Path, ...)`/`Canvas::stroke(Path, ...)` reference. |
| **PostEffects** | A `PostEffectSelector` frame lets you switch between grayscale, sepia, glitch, blur, and other shader-based post effects applied to a drawn ring of circles — the `PostEffect`/`ShaderPostEffect` reference. |
| **Showcase** | A multi-page demo assembling most `visage_widgets` and graphics features together with a themed palette (including custom `VISAGE_THEME_COLOR` colors) — the closest thing to a full "kitchen sink" reference app. |

**Where to look for what:**

- Need flex-style layout? Read `Layout`.
- Need to embed Visage in a host (plugin, sub-view)? Read `ClapPlugin` and/or `BringYourOwnWindow`.
- Need custom visual effects? Read `Bloom` and `PostEffects`.
- Need vector-path drawing? Read `Paths`.
- Want to see a fully themed, multi-widget app? Read `Showcase`.

---

## Embedding Visage in a Plugin Host (CLAP Walkthrough)

If you're building an audio plugin (or any host application that needs to embed Visage inside a
window it already owns, rather than letting Visage own a top-level OS window), `examples/ClapPlugin`
is the authoritative reference. The lifecycle below is confirmed directly from
`examples/ClapPlugin/clap_plugin.cpp` and `clap_plugin.h`.

### 1. `guiCreate(api, is_floating)` — construct, but don't show yet

```cpp
if (is_floating)
  return false;  // this example only supports embedded (non-floating) GUIs

app_ = std::make_unique<visage::ApplicationWindow>();
app_->setWindowDimensions(80_vmin, 60_vmin);
app_->onDraw() = [this](visage::Canvas& canvas) { /* ... draw UI ... */ };
```

Note that the plugin does **not** call `show()` here. It only sizes the editor and wires up draw
logic. `80_vmin`/`60_vmin` are `Dimension` literal suffixes meaning "80%/60% of
`min(parent_width, parent_height)`" — see [Theming and Palette Customization](#theming-and-palette-customization)
and the `Dimension` unit system for more literal suffixes (`_px`, `_npx`, `_vw`, `_vh`, `_vmax`).

### 2. `guiSetParent(const clap_window* window)` — attach to the host's window

```cpp
app_->show(window->ptr);
```

This is the step that actually embeds the editor. `window->ptr` is CLAP's own opaque native-handle
union — it resolves to an `HWND` on Windows, an `NSView*` on macOS, or an X11 `Window` id on
Linux — and is passed straight through as the `void* parent_window` argument of
`ApplicationWindow::show(void*)`.

On Linux, this step also registers the plugin's POSIX file descriptor with the host for event-loop
integration:

```cpp
_host.posixFdSupportRegister(app_->window()->posixFd(), ...);
```

This confirms an important integration detail: **inside a plugin, Visage's own X11 event loop does
not run standalone** — the host polls Visage's file descriptor itself. Your plugin does not call
`runEventLoop()` in this scenario; the host drives the event pump.

### 3. Resize negotiation

`guiSetSize`/`guiGetSize`/`guiAdjustSize`/`guiGetResizeHints` proxy the host's resize requests to
`ApplicationWindow::adjustWindowDimensions(...)` / `setNativeWindowDimensions(...)` (or
`setWindowDimensions(...)` on macOS, since macOS reports logical points rather than native pixels —
see the `#if __APPLE__` branches in `clap_plugin.h`).

### 4. `guiDestroy()` — synchronous teardown

```cpp
// (Linux only) unregister the POSIX fd first
app_->close();
app_ = nullptr;
```

Teardown is synchronous and entirely host-driven — there is no async/deferred close handshake to
implement.

### Applying this pattern outside CLAP

Even if you're targeting VST3, AU, or another plugin format, the same three moves generalize:

1. Construct your `ApplicationWindow` (or `ApplicationEditor` if you want a windowless/off-screen
   canvas) without calling `show()`.
2. When the host hands you its native parent-window handle, call `show(parent_handle)` (or
   `show(width, height, parent_handle)` if you need to size at the same time).
3. On teardown, call `close()` and release your `ApplicationWindow` — do this synchronously in
   response to the host's destroy callback, matching the CLAP example.

---

## Theming and Palette Customization

Visage's visual styling is driven by a **palette** of named colors and numeric values, resolved
at draw time through each `Frame`. Color/value IDs are not a hand-maintained enum — they are
auto-registered at static-init time via macros:

```cpp
#define VISAGE_THEME_COLOR(color, default_color) \
  const ::visage::theme::ColorId color = ::visage::theme::ColorId::nextId(#color, __FILE__, default_color)
```

### Step-by-step: defining and applying a custom theme

1. **Declare a color with a default**, at file/module scope:
   ```cpp
   VISAGE_THEME_COLOR(MyBg, 0xff223344);
   ```
   This registers a new `theme::ColorId` named `MyBg` with a default ARGB value, the first time
   this translation unit is loaded.

2. **Create and populate a `Palette`**:
   ```cpp
   visage::Palette palette;
   palette.initWithDefaults();              // seed every registered ColorId/ValueId with its default
   palette.setColor(MyBg, visage::Color(0xff112233));  // override specific entries
   ```
   For a subtree-scoped override that doesn't affect the rest of the app, use
   `palette.setColor(overrideId, MyBg, brush)` together with a `theme::OverrideId` (see
   `Frame::setPaletteOverride`).

3. **Attach the palette to your root `Frame`**:
   ```cpp
   rootFrame.setPalette(&palette);
   ```
   `setPalette` propagates recursively to every child in the tree.

4. **Read the resolved color inside `draw(Canvas&)`**:
   ```cpp
   void MyWidget::draw(visage::Canvas& canvas) {
     canvas.setColor(paletteColor(MyBg));
     // ... or directly: canvas.setColor(MyBg) — Canvas::setColor(theme::ColorId) resolves the
     // same way Frame::paletteColor does.
   }
   ```

5. **(Optional) Let users edit the palette interactively.** `visage_widgets` provides two ready-made
   editor widgets you can embed anywhere in your UI:
   - `PaletteColorEditor(Palette*)` — a scrollable, grouped list of color swatches with embedded
     `ColorPicker`s for editing gradient stops.
   - `PaletteValueEditor(Palette*)` — a scrollable list of numeric theme values with inline text
     editors.

   > Note: there is no single class literally named `PaletteEditor` — it's split into these two
   > classes. The `Showcase` example's built-in palette-editing page uses exactly this pair.

6. **Persist a palette** with `Palette::encode()` / `Palette::decode(string)`, which round-trip a
   palette to and from a string — useful for saving a user's theme choice to disk or plugin state.

If no override is set for a given `ColorId`/`ValueId` on the resolving `OverrideId` layer, lookup
falls back to that ID's registered default color/value automatically — you never need to populate
every single entry by hand once you've called `initWithDefaults()`.

---

## Embedding Custom Assets (Fonts, Icons, Images)

Visage's own fonts, icons, and shaders are compiled directly into the library binary at build
time — no runtime file loading is required for built-in assets, and the same mechanism is
available to your own application via the `visage_file_embed` CMake module.

### The pattern, step by step

1. **Collect the files you want to embed** (fonts, SVGs, images, anything) and call
   `add_embedded_resources` in your `CMakeLists.txt`:
   ```cmake
   file(GLOB_RECURSE FONT_TTF_FILES fonts/*.ttf)
   add_embedded_resources(EmbeddedFontResources "example_fonts.h" "resources::fonts" "${FONT_TTF_FILES}")
   ```
   This is exactly the pattern `examples/CMakeLists.txt` uses for the example gallery's own fonts
   and icons. The arguments are: a target/library name you choose (`EmbeddedFontResources`), the
   generated header filename, the C++ namespace the generated symbols will live in
   (`resources::fonts`), and the list of source files.

2. **Link the generated object library into your app target**:
   ```cmake
   target_link_libraries(MyApp PRIVATE visage EmbeddedFontResources)
   ```

3. **Include the generated header and reference the embedded asset in code.** Each input file
   becomes an `EmbeddedFile` instance:
   ```cpp
   struct EmbeddedFile {
     const char* name;
     const unsigned char* data;
     int size;
   };
   ```
   Application code references a generated instance directly — no manual byte-array plumbing:
   ```cpp
   canvas.svg(resources::icons::my_icon, x, y, w, h);
   visage::Font my_font(size, resources::fonts::my_font);
   ```
   Both `Canvas::svg(const EmbeddedFile&, ...)` and the `Font(float, const EmbeddedFile&, float)`
   constructor accept an `EmbeddedFile` directly.

> **When you don't need embedding**: `Font` also has a constructor that loads a `.ttf` file from
> disk at runtime (`Font(float size, const std::string& file_path, float dpi_scale = 0.0f)`), so
> build-time embedding is a convenience for shipping self-contained binaries, not a hard
> requirement. `Image`, by contrast, has no `Image::fromFile(path)` — images must be supplied as
> already-decoded byte blobs, either as an `EmbeddedFile` or a raw in-memory pointer your
> application already holds.

---

## Writing Custom Shaders

Visage's GPU effects (built-in shapes, `PostEffect`s, and custom `ShaderPostEffect`s) are all
authored in bgfx's own `.sc` shader-source dialect and cross-compiled at build time to each
platform's native shader language: HLSL (Windows/D3D), Metal (macOS), GLSL/SPIR-V (Linux/Vulkan),
and ESSL (Emscripten/WebGL).

### A complete, minimal example

Here is one of Visage's own built-in fragment shaders in full, `visage_graphics/shaders/fs_circle.sc`:

```glsl
$input v_coordinates, v_dimensions, v_shader_values, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos

#include <shader_include.sh>

SAMPLER2D(s_gradient, 0);

void main() {
  gl_FragColor = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  gl_FragColor.a = gl_FragColor.a * circle(v_coordinates, v_dimensions.x, v_shader_values.x, v_shader_values.y);
}
```

Key structural elements:
- `$input ...` declares the varying inputs this fragment shader receives from its paired vertex
  shader.
- `#include <shader_include.sh>` pulls in Visage's shared shader helper functions (like `gradient`
  and `circle` used above).
- The shader writes `gl_FragColor` exactly like classic GLSL — bgfx's shader compiler (`shaderc`)
  translates this to the target platform's native shader language at build time.

### Writing your own `PostEffect` shader

1. **Write a matching vertex+fragment `.sc` pair** for your effect.
2. **Embed and compile them via the same CMake shader-embedding path the examples use for their own
   shaders** — the `visage_embed_shaders(...)` CMake function (used for `EXAMPLE_SHADERS` in
   `examples/CMakeLists.txt`) hex-encodes and cross-compiles your `.sc` files the same way
   Visage's built-in shaders are handled.
3. **Construct a `ShaderPostEffect`** from the two resulting embedded files:
   ```cpp
   visage::ShaderPostEffect my_effect(vertex_shader_embedded_file, fragment_shader_embedded_file);
   my_effect.setUniformValue("my_param", 1.0f, 0.5f, 0.0f, 1.0f);  // feed a vec4 uniform by name
   ```
4. **Attach it to a `Frame`**:
   ```cpp
   frame.setPostEffect(&my_effect);       // applied to the frame's own rendered output
   // or:
   frame.setBackdropEffect(&my_effect);   // applied to what's behind the frame (e.g. frosted-glass blur)
   ```
   `Frame` does not take ownership of a `PostEffect*` attached this way — your application code
   must keep `my_effect` alive for as long as it's attached (the one exception is the convenience
   `Frame::setBlurRadius(float)` path, which internally owns its own `BlurPostEffect`).

### Rapid iteration without a full rebuild

The `visage_widgets::ShaderEditor` widget (also demonstrated by the `LiveShaderEditing` example)
lets you hot-edit and recompile fragment-shader text against a live-rendered scene, without a full
CMake rebuild. It works by shelling out to the bundled `shaderc` compiler binary via
`spawnChildProcess` (see the security note below for what that means).

> **This widget is explicitly marked in source as "For shader development purposes only. Not for
> production use."** Use it while iterating on shader code; don't ship it as part of a production
> UI. On Emscripten builds, `ShaderEditor` uses a WebGL-side compile path instead of shelling out
> to `shaderc`, since native child-process spawning isn't available in the browser.

---

## Security Note: `spawnChildProcess`

`visage_utils/child_process.h` exposes a function for launching an external process and capturing
its output:

```cpp
static constexpr int kDefaultChildProcessTimeoutMs = 10000;   // 10 seconds
static constexpr size_t kMaxOutputSize = 1024 * 1024;         // 1 MiB captured-output cap

bool spawnChildProcess(const std::string& command, const std::string& arguments,
                        std::string& output, int timeout_ms = kDefaultChildProcessTimeoutMs);
```

It returns `true`/`false` for success/failure, writes captured stdout into the `output`
out-parameter (capped at 1 MiB), and fails/returns after `timeout_ms` (10 seconds by default) if
the child process hangs.

### Why this needs a callout

`arguments` is split into individual argv entries by **naive whitespace splitting** on the POSIX
backend (`std::getline` over the string, splitting on `' '`) before being handed to `posix_spawn`.
**There is no quoting or escaping support.** There is also no shell involved, so classic
shell-injection (`; rm -rf /`, backticks, `$()`) is not a risk here — but a stray space inside the
argument string will silently produce an extra, unintended argument rather than being escaped,
rejected, or erroring out.

The only in-tree caller is `visage_widgets/shader_editor.cpp`, which invokes the bundled `shaderc`
binary with fixed, developer-controlled flags — a safe usage pattern by construction.

### Guidance for your own code

- **Do**: pass fixed, developer-controlled argument strings assembled from tokens that contain no
  embedded spaces — exactly how the in-tree caller uses it.
- **Don't**: build the `arguments` string by concatenating user-supplied text (a file path a user
  typed that happens to contain a space, or any string sourced from an untrusted network/file
  input). A space in attacker- or user-influenced input will silently split into an extra
  argument, potentially changing which flag or positional argument the spawned process receives.
- **Do** rely on the built-in safety bounds: check the returned `bool` before trusting `output`,
  and remember the call will return after `timeout_ms` even if the child process never exits on
  its own.
- If your use case genuinely requires passing untrusted or space-containing arguments, do not use
  `spawnChildProcess` as-is — you would need to add proper argument-array (not string-split)
  support before it's safe for that purpose.

---

## Troubleshooting

- **"I built with `VISAGE_SYSTEM_BGFX=ON`/`VISAGE_SYSTEM_FREETYPE=ON` and get link errors."**
  Check for a version mismatch between the system-installed bgfx/FreeType and the versions Visage
  is developed/tested against (bgfx `v1.129.8958-499`, FreeType `VER-2-14-1` when fetched
  automatically). Version drift between a substituted system library and these pinned versions is
  a known source of subtle incompatibility.
- **"My example target didn't build."** Confirm `VISAGE_BUILD_EXAMPLES` is `ON`. It defaults to
  `ON` only when Visage is configured as the *top-level* CMake project — if you've added Visage as
  a subdirectory/`FetchContent` dependency of your own project, you must set it explicitly:
  `-DVISAGE_BUILD_EXAMPLES=ON`.
  Note: this option builds Visage's own gallery of examples (`Basic`, `Layout`, `ClapPlugin`,
  etc.) — it has no effect on whether *your own* application target builds; that's controlled
  entirely by your own `add_executable`/`target_link_libraries` calls.
- **"Tests don't run."** Same pattern as above for `VISAGE_BUILD_TESTS`; it also defaults `OFF`
  when Visage is a subdirectory dependency.
- **"Stepping through Visage source in a debugger is confusing / breakpoints land in the wrong
  file."** This is the expected effect of `VISAGE_AMALGAMATED_BUILD=ON` (the default) — multiple
  `.cpp` files are concatenated into a handful of generated translation units for faster clean
  builds. Set `-DVISAGE_AMALGAMATED_BUILD=OFF` for a normal one-`.cpp`-per-translation-unit debug
  build.
- **"DLLs missing at runtime on Windows with a shared build."** Confirm `BUILD_SHARED_LIBS=ON` was
  set at configure time — this is what triggers `visage_copy_runtime_dlls()` to copy
  `visage.dll`/`bgfx.dll`/`freetype.dll` next to your executable automatically. If you're linking
  your own app against an installed shared Visage outside its own build tree, you'll need to copy
  or add these DLLs to `PATH` yourself.
- **"My plugin's window doesn't respond to input on Linux."** Confirm you registered the plugin's
  POSIX file descriptor with the host, as `ClapPlugin::guiSetParent` does
  (`_host.posixFdSupportRegister(app_->window()->posixFd(), ...)`). Inside a plugin, Visage does
  not run its own event loop on Linux — the host must poll Visage's fd.

---

## Where to Go Next

- Read `visage_ui/frame.h` directly for the full `Frame` API — it's the base class every widget
  and the whole app layer derives from, and is the single highest-leverage header to understand.
- Read `visage_graphics/canvas.h` for the complete drawing-primitive inventory (shapes, text, SVG,
  images, shaders, graphs, heat maps).
- Read `visage_windowing/windowing.h` if you need lower-level windowing control beyond what
  `ApplicationWindow` exposes (used by `examples/BringYourOwnWindow`).
- Explore `visage_widgets/*.h` for ready-made UI controls: `Button`, `TextEditor`, `ColorPicker`,
  `GraphLine`, `HeatMap`, `BarList`, `PaletteColorEditor`/`PaletteValueEditor`, `ShaderEditor`,
  `ShaderQuad`.
- Study `examples/Showcase` as a single "kitchen sink" reference that ties together widgets,
  theming, and graphics features in one app.
