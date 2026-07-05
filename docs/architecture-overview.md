# Visage Architecture Overview

## Overview

Visage is a C++17 library that lets a developer open a native, GPU-rendered window and draw a
custom UI/2D-graphics scene into it every frame. It combines an immediate-mode-style drawing API
(`Canvas`) with a retained-mode widget/frame tree (`Frame`), running on top of
[bgfx](https://github.com/bkaradzic/bgfx) for cross-platform GPU rendering. It targets desktop
native applications, audio-plugin GUIs (a CLAP plugin example ships in-tree), and
WebAssembly/Emscripten builds.

This document explains how Visage is put together internally: its module structure and dependency
graph, its threading model, how a frame gets from "something changed" to "pixels on screen," the
relationships between its core classes, how the build system produces the `visage` target, and how
it abstracts over per-platform windowing and rendering backends. It is written for developers who
want to understand, extend, or contribute to Visage itself — not primarily for application authors
looking for a how-to guide (see the companion User Guide and API Reference for that).

Everything in this document is sourced from the Visage source tree at `Z:/Github/visage` (headers,
`.cpp` files, CMake scripts, and CI workflow files), validated file:line against the code and not
against README claims. File references use paths relative to the repository root, e.g.
`visage_ui/frame.h`.

---

## Core Concepts

Before diving into module-by-module detail, four ideas recur throughout Visage's design:

- **A `Frame` tree is retained; a `Canvas` is redrawn.** `Frame` objects form a persistent
  parent/child hierarchy that survives across frames (like a DOM or a widget tree). `Canvas` is the
  per-frame drawing surface each `Frame` paints into via `draw(Canvas&)` — it does not persist state
  between frames beyond what a `Frame` explicitly stores.
- **Redraw is opt-in per `Frame`, but the render loop is continuous.** The OS drives a draw callback
  every vsync/frame regardless of whether anything changed; Visage keeps this cheap by only
  re-painting the subset of `Frame`s that called `redraw()` since the last frame ("stale children").
  Layout recomputation is a separate, rarer event tied to bounds changes, not to every frame.
  See [Rendering Pipeline](#rendering-pipeline-redraw-and-staleness-mechanics) below.
- **Windowing and graphics are independent siblings; the app layer couples them.**
  `visage_windowing` (native OS windows) and `visage_graphics` (bgfx-based rendering) do not depend
  on each other. `visage_app` is the layer that wires a `Window` to a `Canvas` via
  `ApplicationEditor`/`ApplicationWindow`. This means `Canvas` and `Window` can each be used
  standalone, as `examples/BringYourOwnWindow` demonstrates.
- **The UI thread is single-threaded; bgfx manages its own render thread.** All `Frame`/`Canvas`/
  `Layout` mutation and dispatch happens on the thread that calls `Window::runEventLoop()`. bgfx runs
  its own internal render thread regardless of application code. See
  [Threading Model](#threading-model) below.

---

## Module Breakdown

Visage's source is organized into seven top-level directories under the repository root, six of
which compile as CMake `OBJECT` libraries, plus a seventh that is a CMake-only build helper. The
top-level `CMakeLists.txt` merges all of their outputs into one public target, `visage::visage`.

| Directory | CMake object library | Role |
|---|---|---|
| `visage_utils` | `VisageUtils` | Platform-agnostic primitives: export macros/assertions, geometry/dimension types, string/time/thread utilities, filesystem helpers, child-process spawning, shared input enums. |
| `visage_graphics` | `VisageGraphics` | The rendering core: `Canvas`, `Renderer`/`Layer`/`Region` (bgfx submission), `Font`/`Path`/`Image`/`Gradient`/`Palette`/theme, `PostEffect`s, screenshotting, platform-specific emoji rendering. |
| `visage_ui` | `VisageUi` | The retained widget-tree layer: `Frame`, UI-level event types, the flex-style `Layout` engine, undo/redo history, `PopupMenu`, `ScrollBar`, `SvgFrame`. |
| `visage_widgets` | `VisageWidgets` | Concrete widgets built on `Frame`: `Button` family, `TextEditor`, `ColorPicker`, `GraphLine`, `HeatMap`, `BarList`, `PaletteColorEditor`/`PaletteValueEditor`, `ShaderEditor`, `ShaderQuad`. Optional via `VISAGE_ENABLE_WIDGETS`. |
| `visage_windowing` | `VisageWindowing` | The `Window` abstract base plus one concrete backend per platform (Win32, Cocoa, X11, Emscripten), plus free functions for clipboard, cursor, message boxes, and DPI scale. |
| `visage_app` | `VisageApp` | The glue layer: `ApplicationEditor`, `ApplicationWindow`, `WindowEventHandler` (bridges `Window::EventHandler` into `Frame` dispatch), `ClientWindowDecoration`. |
| `visage_file_embed` | *(not an object library — CMake helper only)* | Hex-encodes arbitrary files (fonts, icons, shaders, images) into generated `.cpp` files exposing `EmbeddedFile` structs, each compiled into its own small object library (e.g. `VisageEmbeddedShaders`, `VisageEmbeddedFonts`, `VisageEmbeddedIcons`). |

**On "modules merged into one target":** the top-level `visage` target actually merges **nine**
object-library outputs — the six code modules above plus three embedded-asset libraries generated
by `visage_file_embed` — even though they are sourced from seven top-level directories. This
distinction matters because `visage_file_embed` itself never produces compiled object code; it only
generates CMake targets that do (`CMakeLists.txt:170-179`).

### Dependency Graph

The module dependency graph, confirmed from each module's `CMakeLists.txt`
`target_link_libraries`/`add_library` declarations, layers as follows (top depends on bottom):

```
visage_app        (couples visage_ui + visage_windowing + visage_graphics)
   |         \
visage_widgets    (depends on visage_ui, visage_graphics)
   |         \
visage_ui         (depends on visage_graphics)
   |
visage_graphics   (depends on visage_utils only, + external bgfx/FreeType)
   |
visage_windowing  (depends on visage_utils only — a SIBLING of visage_graphics)
   |
visage_utils      (foundational — no Visage-internal dependencies)
```

Key facts behind this diagram:

- `VisageUtils` (`visage_utils/CMakeLists.txt:20`) has no `target_link_libraries` calls to any other
  Visage module — it is the foundational layer.
- `VisageGraphics` (`visage_graphics/CMakeLists.txt:180`) links external bgfx/bx/bimg/FreeType, but
  has no dependency on `VisageUi`, `VisageWindowing`, or `VisageApp`.
- `VisageWindowing` (`visage_windowing/CMakeLists.txt:33`) links only platform libraries
  (`X11_LIBS`/`WIN_LIBS`). It has **no dependency on `VisageUi` or `VisageGraphics`** — this is the
  architecturally significant point: windowing and graphics are parallel, independent modules that
  both depend only on `visage_utils`. `visage_windowing` uses `visage_utils` types (`events.h`,
  `space.h`, `dimension.h`) at the header-include level only, since all module headers are visible
  through the single top-level `visage` target.
- `VisageUi` (`visage_ui/CMakeLists.txt:17`) links `VisageGraphicsEmbeds` (for built-in SVG icons
  used by `PopupMenu`/`ScrollBar`) and depends at the header level on `visage_graphics/canvas.h`,
  `palette.h`, and `theme.h` (`visage_ui/frame.h:27-29`).
- `VisageWidgets` (`visage_widgets/CMakeLists.txt:15`) also links `VisageGraphicsEmbeds` and depends
  at the header level on `visage_ui/frame.h`, since every widget subclasses `Frame`.
- `VisageApp` (`visage_app/CMakeLists.txt:11`) has no explicit `target_link_libraries` of its own,
  but depends at the header level on `visage_ui/frame.h` (via `application_editor.h:24`) and
  `visage_windowing/windowing.h` (via `application_window.h:26`). This is the one layer that
  **does** couple UI to windowing — via `WindowEventHandler`, which bridges `Window::EventHandler`
  callbacks into `Frame` dispatch.

The practical consequence: because `visage_windowing` and `visage_graphics` never depend on each
other, a `Window` and a `Canvas` can be created and driven completely independently of the
`visage_app` convenience layer. `examples/BringYourOwnWindow` demonstrates exactly this — it calls
`createWindow(800, 800)` directly, constructs a `Renderer` and a bare `Canvas`, calls
`canvas.pairToWindow(...)`, and drives the native event loop itself, bypassing `ApplicationWindow`/
`ApplicationEditor` entirely.

---

## Core Class Relationships

Four classes anchor the object model that application code interacts with directly:
`Frame`, `Canvas`, `Window`, and the `visage_app` pair `ApplicationEditor`/`ApplicationWindow`.

### `Frame` (`visage_ui/frame.h`)

`Frame` is the base class for every UI element in Visage — every widget in `visage_widgets`, and
the `visage_app` classes themselves, derive from it. It is the single largest and
most-depended-upon public class in the library, combining several concerns in one type:

- **Hierarchy.** A `Frame` tracks all of its children (owned or not) in
  `std::vector<Frame*> children_` (`frame.h:478`) for iteration and event dispatch. It supports a
  **dual ownership model** selected by which `addChild` overload is called:
  - `addChild(Frame* child, bool make_visible = true)` — non-owning; the caller retains lifetime.
  - `addChild(Frame& child, bool make_visible = true)` — same, by reference.
  - `addChild(std::unique_ptr<Frame> child, bool make_visible = true)` — **transfers ownership**;
    the parent stores it in `std::map<Frame*, std::unique_ptr<Frame>> owned_children_`
    (`frame.h:479`) and destroys it when removed or when the parent itself is destroyed.

  The non-owning form is the common pattern in the example applications — a parent frame typically
  holds children as plain data members (e.g. an array of `Frame` objects), calling
  `addChild(&child)`. This means a non-owned child's storage must outlive its removal from the
  parent; `Frame`'s destructor defensively calls `parent_->eraseChild(this)`/`removeAllChildren()`
  (`frame.h:54-60`) to keep the tree consistent even so. The `unique_ptr` overload exists for cases
  where the parent should own and destroy the child itself (e.g. dynamically created popups or
  dialogs).

- **Events.** Every input virtual method (`mouseDown`, `keyPress`, `resized`, `dpiChanged`, etc.)
  has a matching `CallbackList` accessor (`onMouseDown()`, `onKeyPress()`, `onResize()`,
  `onDpiChange()`, ...), and **both fire** — overriding the virtual and registering a lambda are not
  mutually exclusive; the callback list's default entry simply invokes the virtual
  (`frame.h:461`). This lets subclasses override behavior while still allowing application code to
  attach additional lambdas without subclassing.

- **Layout.** `Frame::layout()` lazily allocates a `Layout` object (`std::make_unique<Layout>()`) on
  first access (`frame.h:264-268`). Calling `setBounds`/`setNativeBounds` automatically triggers
  `computeLayout()` as a side effect (`frame.cpp:164-176`) — application code normally never calls
  `computeLayout()` directly.

- **Palette/theme.** `setPalette(Palette*)` propagates recursively to all children
  (`frame.h:118-122`); `paletteColor(theme::ColorId)`/`paletteValue(theme::ValueId)` are what a
  widget's `draw()` method calls to resolve themed colors and values.

- **Post-effects.** `setPostEffect(PostEffect*)`/`setBackdropEffect(PostEffect*)` attach GPU
  post-processing (blur, bloom, custom shaders) to a frame's own rendered output or to what's behind
  it. `Frame` does not take ownership of a `PostEffect*` passed this way — the one exception is the
  convenience `setBlurRadius(float)`, which internally owns a `std::unique_ptr<BlurPostEffect>`
  (`frame.h:157-173, 490`).

- **Undo/redo.** `addUndoableAction(std::unique_ptr<UndoableAction>)`, `triggerUndo()`/
  `triggerRedo()`, `canUndo()`/`canRedo()` (`frame.h:405-409`) delegate to a `UndoHistory` found via
  the frame tree.

- **Redraw/staleness.** `redraw()` only actually requests a redraw if the frame
  `isVisible() && isDrawing() && !redrawing_`, forwarding to `event_handler_->request_redraw(this)`
  if an event handler is attached (`frame.h:136-139, 339-345`). See
  [Rendering Pipeline](#rendering-pipeline-redraw-and-staleness-mechanics) for how this feeds the
  frame loop.

Because `Frame` combines layout, events, palette, post-effects, and undo/redo in one class, it is
also flagged (in the underlying analysis) as a maintainability consideration: as the class grows,
splitting these concerns into more composable pieces could ease both testing and documentation —
though this is a design trade-off rather than a defect.

### `Canvas` (`visage_graphics/canvas.h`, ~826 lines)

`Canvas` is the per-frame immediate-mode drawing surface. A `Frame`'s `draw(Canvas&)` override (or
its `onDraw()` lambda) calls `Canvas` methods to accumulate shape draw calls for the current frame.

- **No explicit begin/end frame pair is exposed to application code.** The closest analogs are
  internal `beginRegion(Region*)`/`endRegion()` (`canvas.h:522-534`), called by the framework around
  each `Frame`'s region draw, and the top-level `Canvas::submit(int submit_pass = 0)`
  (`canvas.h:67`), which the framework calls once per real render pass
  (`ApplicationEditor::drawWindow()`) to flush all accumulated shapes to bgfx.
- **State persistence.** Brush/color, position offset, clamp/clip bounds, and blend mode all live in
  a `Canvas::State` struct (`canvas.h:50-60`) that persists across draw calls within a region unless
  explicitly changed. `saveState()`/`restoreState()` (`canvas.h:502-510`) provide a push/pop stack
  for scoped overrides.
- **Units.** Every drawing primitive's numeric parameters are resolved through a private `pixels<T>()`
  helper (`canvas.h:576-583`): a plain `float`/`int` is always treated as a logical pixel value
  (multiplied by `state_.scale`, the current DPI/UI scale), while a `Dimension` argument resolves via
  its own unit kind (native pixels, logical pixels, or a percentage of the parent's dimensions — see
  [DPI and Dimension Units](#dpi-and-dimension-units) below).
- **Primitive inventory** spans filled/stroked shapes (`fill`, `rectangle`, `circle`, `ring`,
  `squircle`, arcs, triangles, `segment`, quadratic Bézier strokes), text (`text(...)`), vector
  images (`svg(...)`), raster images (`image(...)`), data-driven visuals (`graphLine`, `graphFill`,
  `heatMap`), and custom-shader quads (`shader(Shader*, ...)`).
- **Important correction:** `Canvas` has **no** generic `stroke(x, y, w, h, ...)` method. The only
  `stroke()` overload takes a vector `Path` (`canvas.h:488-500`). Built-in primitives are "stroked"
  via dedicated border/outline methods instead: `rectangleBorder`/`roundedRectangleBorder`,
  `ring` (a circle with `thickness`), `squircleBorder`, `arc`/`flatArc`/`roundedArc` (all take a
  `thickness` parameter), `segment` (a stroked line with `thickness`), and `triangleBorder`/
  `roundedTriangleBorder`.

### `Window` (`visage_windowing/windowing.h`)

`Window` is the abstract base for native OS windows, with one concrete subclass per platform
(Win32, Cocoa/macOS, X11/Linux, Emscripten). Its nested `Window::EventHandler` is a pure-abstract
interface with 20 pure virtuals covering hit-testing, mouse, keyboard, focus, resize, lifecycle, and
drag-drop events; this interface is implemented by `visage_app::WindowEventHandler`, which is the
bridge that turns raw OS events into `Frame` tree dispatch.

Free factory functions create windows without requiring the `visage_app` layer:

```cpp
std::unique_ptr<Window> createWindow(const Dimension& x, const Dimension& y,
                                      const Dimension& width, const Dimension& height,
                                      Window::Decoration decoration_style = Window::Decoration::Native);

std::unique_ptr<Window> createPluginWindow(const Dimension& width, const Dimension& height,
                                            void* parent_handle);
```

`parent_handle` is an untyped `void*` in the cross-platform header; each platform backend casts it
internally (Win32: `HWND`; macOS: `NSView*`; X11/Linux: an Xlib `Window` handle, i.e. an
`unsigned long`; Emscripten: unused/ignored). This is the mechanism that lets Visage embed itself
inside a host application's window — see the CLAP-plugin walkthrough under
[Platform Abstraction](#platform-abstraction) below.

`Window::Decoration` has three values: `Native` (OS-drawn title bar), `Client` (the app draws its
own title bar via `ClientWindowDecoration`), and `Popup` (no title bar, used for menus/pickers).

`Window::setDrawCallback(std::function<void(double)>)` (`windowing.h:117-121`) registers the
framework's main per-frame draw hook — see [Rendering Pipeline](#rendering-pipeline-redraw-and-staleness-mechanics).

### `ApplicationEditor` and `ApplicationWindow` (`visage_app/application_editor.h`, `application_window.h`)

`ApplicationEditor` (`application_editor.h:47-129`) is itself a `Frame` subclass — it **is** the root
of the UI tree, not a separate container wrapping one. It owns:

- a `Canvas` (`canvas_`),
- a `TopLevelFrame` wrapper (`top_level_`) that hosts client-drawn title-bar decoration when used,

and can operate in two modes:

- **Windowed**, via `addToWindow(Window*)` — attaches to a `Window` instance (native or embedded).
- **Windowless**, via `setWindowless(int width, int height)` — renders the `Canvas` off-screen. This
  is used for plugin editors that supply their own host window, or for headless snapshotting via
  `takeScreenshot()`.

Its `drawWindow()` method renders one frame: it draws stale children, then calls
`canvas_->submit()`.

`ApplicationWindow` (`application_window.h:30-77`) **inherits from `ApplicationEditor`** and adds
native-OS-window lifecycle management on top: window title, always-on-top, decoration style, window
dimensions, and the `show()` family:

```cpp
void show();                                                     // use last-set dimensions/position
void show(void* parent_window);                                  // embed into a host window (plugin mode)
void show(const Dimension& width, const Dimension& height, void* parent_window);
void show(const Dimension& width, const Dimension& height);
void show(const Dimension& x, const Dimension& y, const Dimension& width, const Dimension& height);
void showMaximized();
void hide();
void close();
bool isShowing() const;
void runEventLoop();                                             // blocks pumping the OS event loop
```

`ApplicationWindow` internally owns its native `Window` as `std::unique_ptr<Window> window_`
(`application_window.h:75`), created inside `show()` and destroyed on `close()`/destruction — so
application code never touches a raw `Window*` unless it deliberately drops down to the
lower-level `createWindow()` API (as `BringYourOwnWindow` does).

`onDraw()` (inherited from `Frame`) is the **only** hook for per-frame drawing at this level: either
override `virtual void draw(Canvas&)` in a subclass, or assign a lambda to `onDraw()` — both are
equivalent, since the default `on_draw_` callback target simply calls `draw(canvas)`
(`frame.h:447`).

**Class relationship summary:**

```
Frame  (base: hierarchy, events, layout, palette, post-effects, undo/redo, redraw)
  ^
  |  (is-a)
ApplicationEditor  (adds: owns Canvas, windowed/windowless mode, drawWindow())
  ^
  |  (is-a)
ApplicationWindow  (adds: native OS window lifecycle, show()/hide()/close())
```

Meanwhile `Window` and `Canvas` are peers used internally by `ApplicationEditor`/`ApplicationWindow`,
but neither is a base or subclass of `Frame` — they are composed into it, not inherited.

---

## Rendering Pipeline (Redraw and Staleness Mechanics)

Visage uses a **continuous-redraw** model driven by the OS/GPU's own vsync or timer signal, made
cheap in the common case by a "stale children" partial-repaint mechanism.

### What triggers a new frame

Every platform backend calls `Window::drawCallback(double time)` (`windowing.h:117-124`) from its
own native timing source — this callback fires every vsync/frame regardless of whether the
application changed anything:

- **Windows** — a dedicated v-blank thread: `drawCallback(v_blank_thread_->vBlankTime())`
  (`visage_windowing/win32/windowing_win32.cpp:1025`).
- **macOS** — a `CVDisplayLink`-driven callback (`visage_windowing/macos/windowing_macos.mm:335`).
- **Linux/X11** — an internal timing loop (`visage_windowing/linux/windowing_x11.cpp:1107, 1443, 1507`).
- **Emscripten** — `emscripten_set_main_loop(runLoop, 0, 1)`, where `fps=0` means the browser's own
  `requestAnimationFrame` cadence drives the loop (`visage_windowing/emscripten/windowing_emscripten.cpp:179, 626`).

### Why the continuous callback is cheap

`ApplicationEditor::drawWindow()` (`application_editor.cpp:151-163`) is the function invoked (via
the draw callback chain) on every one of these ticks. It does two things:

1. Calls `drawStaleChildren()`, which only re-renders `Frame`s that called `redraw()` since the last
   frame. These are tracked in `stale_children_`/`drawing_children_` vectors
   (`application_editor.h:125-126`), populated by the `request_redraw` event-handler lambda
   installed in `ApplicationEditor`'s constructor (`application_editor.cpp:63-66`).
2. Unconditionally calls `canvas_->submit()` to flush whatever was accumulated (which may be
   nothing new) to bgfx.

### Layout recomputation is a separate, rarer event

Layout is **not** recomputed every frame. It recomputes only when `Frame::setBounds`/
`setNativeBounds` is called, which directly invokes `computeLayout()` as a side effect
(`frame.cpp:164-176`). Painting ("stale children" redraw) is the per-frame-conditional operation;
layout recomputation is tied to actual bounds/resize changes, which happen far less often than
paints. In short: the "stale children" skip is a **repaint-only** optimization, not a layout-skip
optimization.

### Data flow, end to end

Putting the whole chain together, from an OS input event to pixels on screen:

1. OS input event arrives at the platform `Window` subclass.
2. The `Window` subclass's `handle*` methods (e.g. `handleMouseDown`) are called.
3. These dispatch into `Window::EventHandler`, implemented by `visage_app::WindowEventHandler`.
4. `WindowEventHandler` dispatches the event into the `Frame` tree (`processMouseDown`, etc.), which
   invokes both the relevant virtual method and its matching `onXxx()` callback list.
5. Application code mutates state and calls `redraw()` on the affected `Frame`(s).
6. On the next vsync-driven draw callback, `ApplicationEditor::drawWindow()` calls
   `drawStaleChildren()`, invoking `draw(Canvas&)` top-down only over frames marked stale.
7. `Canvas` accumulates shape draw calls into a `Region`'s shape batcher.
8. `Canvas::submit()` flushes the batched shapes through `Renderer`/`Layer` to bgfx for actual GPU
   submission.

### Two recurring communication patterns

Two patterns recur throughout this pipeline and are worth calling out explicitly:

1. **Virtual-method-plus-`CallbackList` pairing** on `Frame` — e.g. `mouseDown()` (virtual) and
   `onMouseDown()` (callback list) both fire for the same event, letting subclasses either override
   behavior directly or attach lambdas without subclassing.
2. **A small `EventHandler` struct of `std::function`s** (`Window::EventHandler`) that decouples
   `visage_ui`/`visage_app` from the concrete windowing backend, avoiding a hard dependency from the
   UI layer down into platform-specific code.

---

## Threading Model

`Frame`, `Canvas`, and `Layout` construction, mutation, and drawing are **single-threaded**. All
event-dispatch code (`processMouseDown`, `draw`, etc.) runs on the thread that calls
`Window::runEventLoop()` — the "UI thread." There is no internal locking in `Frame` or `Canvas`;
application code must not touch these objects from any other thread.

Visage does manage one background thread itself: **bgfx's own render thread**, started
unconditionally by `Renderer::initialize()` (`visage_graphics/renderer.cpp`), which polls via
`bgfx::renderFrame()` in a loop inside `Renderer::run()`/`Renderer::render()`.

The CMake option `VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD` (default `OFF`, `CMakeLists.txt:23`)
sets bgfx's `BGFX_CONFIG_MULTITHREADED=1` and defines `VISAGE_BACKGROUND_GRAPHICS_THREAD=1`
(`visage_graphics/CMakeLists.txt:45-49`). This is a **bgfx-internal** threading knob — it lets
bgfx's own submit/render stages run on separate threads per bgfx's multithreaded rendering model —
rather than something that changes `Frame`/`Canvas` thread safety. Application code should still
only touch `Frame`/`Canvas`/`Window` from the UI/main thread regardless of this flag's setting.

The one place application-facing code runs work off the UI thread is the `ShaderEditor` widget's
`ShaderCompiler` (a `Thread` subclass, `visage_widgets/shader_editor.h:36`), which shells out to the
bundled `shaderc` binary asynchronously and synchronizes results back via a `std::mutex
code_mutex_` and an `std::atomic<bool> new_code_` flag (`shader_editor.h:143-149`).

---

## DPI and Dimension Units

Because layout, drawing, and windowing all need to reconcile logical UI coordinates with physical
device pixels, Visage centers unit handling on a single polymorphic type, `Dimension`
(`visage_utils/dimension.h:29-187`), used throughout `Layout` and `Canvas`. Each `Dimension` wraps a
closure resolved at layout/draw time against `(dpi_scale, parent_width, parent_height)`:

```cpp
Dimension::nativePixels(float px)      // raw device pixels, ignores dpi_scale
Dimension::logicalPixels(float px)     // px * dpi_scale (the default for a bare float literal)
Dimension::widthPercent(float pct)     // pct% of parent width
Dimension::heightPercent(float pct)    // pct% of parent height
Dimension::viewMinPercent(float pct)   // pct% of min(parent_width, parent_height)
Dimension::viewMaxPercent(float pct)   // pct% of max(parent_width, parent_height)
```

Literal suffixes in `namespace visage::dimension` provide shorthand: `10_npx` (native pixels),
`10_px` (logical pixels), `50_vw`/`50_vh` (viewport width/height percent), `50_vmin`/`50_vmax` (CSS
`vmin`/`vmax` equivalents) — `dimension.h:190-236`.

**DPI scale propagation:** `Window::dpiScale()` is the authoritative per-window value, queried from
the OS. `ApplicationEditor::addToWindow()` copies it into the `TopLevelFrame` via `setDpiScale`
(`application_editor.cpp:119`), and `Frame::setDpiScale` recursively propagates it to every child
(`frame.h:310-321`), firing `onDpiChange()` only when the value actually changes. `Canvas` keeps its
own copy (`Canvas::setDpiScale`, `canvas.h:95`), used by the `pixels<T>()` conversion described
under [Canvas](#core-class-relationships) above.

`Frame::scale()` is a **separate, independent** multiplier (`frame.h:323-337`) layered on top of DPI
scale — useful for an application-controlled "UI zoom" level distinct from OS-reported DPI.

---

## Build System Architecture

### Toolchain requirements

- **CMake**: minimum version 3.17 (`CMakeLists.txt:1`).
- **C++ standard**: C++17, set unconditionally if not already defined by a parent project
  (`CMakeLists.txt:9-11`).

### Target structure

The public consumption target is `visage::visage`, an `ALIAS` for the `visage` target
(`CMakeLists.txt:142`). Internally, the top-level `CMakeLists.txt` merges the six code modules'
`OBJECT` library outputs (`VisageUtils`, `VisageGraphics`, `VisageUi`, `VisageWidgets`,
`VisageWindowing`, `VisageApp`) plus three generated embedded-asset object libraries
(`VisageEmbeddedShaders`, `VisageEmbeddedFonts`, `VisageEmbeddedIcons`) into this one target
(`CMakeLists.txt:170-179`). A downstream consumer links against `visage::visage` and never sees the
individual module libraries directly.

Minimal downstream integration via `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(visage
  GIT_REPOSITORY https://github.com/VitalAudio/visage.git
  GIT_TAG main   # or a pinned commit/tag
)
FetchContent_MakeAvailable(visage)

add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE visage::visage)
```

Or, against a system-installed copy:

```cmake
find_package(visage REQUIRED CONFIG)
target_link_libraries(MyApp PRIVATE visage::visage)
```

(`visageConfig.cmake.in` is processed by `configure_package_config_file` into `visageConfig.cmake`,
installed to `<prefix>/lib/cmake/visage`, `CMakeLists.txt:247-257` — this is what
`find_package(visage)` locates.)

### Build options

| Option | Default | Effect |
|---|---|---|
| `VISAGE_AMALGAMATED_BUILD` | `ON` | Concatenates each module's `.cpp` files into a handful of generated translation units for faster clean builds, at the cost of coarser incremental-compile granularity (`CMakeLists.txt:21`). |
| `VISAGE_ENABLE_WIDGETS` | `ON` | Builds `visage_widgets` and links it into the `visage` target (`CMakeLists.txt:22, 159-161`). |
| `VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD` | `OFF` | Sets bgfx's `BGFX_CONFIG_MULTITHREADED=1` (see [Threading Model](#threading-model)) (`CMakeLists.txt:23`; `visage_graphics/CMakeLists.txt:45-49`). |
| `VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING` | `OFF` | Shows a graphics debug log in the console in debug builds (`CMakeLists.txt:24`). |
| `VISAGE_ADDRESS_SANITIZER` | `OFF` | Compiles/links with AddressSanitizer (`CMakeLists.txt:25, 28-36`). |
| `BUILD_SHARED_LIBS` | `OFF` | Builds `visage` as a shared library instead of static; when `ON`, `visage_copy_runtime_dlls()` copies `visage.dll`/`bgfx.dll`/`freetype.dll` next to example executables on Windows (`CMakeLists.txt:26, 98-126`). |
| `VISAGE_BUILD_EXAMPLES` | `ON` if top-level project, else `OFF` | Builds everything under `examples/` (`CMakeLists.txt:41, 44`). |
| `VISAGE_BUILD_TESTS` | `ON` if top-level, else `OFF` | Builds the Catch2-based test suite (`CMakeLists.txt:42, 45`). |
| `VISAGE_SYSTEM_BGFX` | `OFF` | Uses a pre-installed bgfx/bx/bimg via `find_package` instead of fetching/building from source (`visage_graphics/CMakeLists.txt:32`). |
| `VISAGE_SYSTEM_FREETYPE` | `OFF` | Same, for FreeType (`visage_graphics/CMakeLists.txt:33`). |

When the `VISAGE_SYSTEM_*` options are left at their default `OFF`, dependencies are fetched at
pinned versions: bgfx `v1.129.8958-499` (`visage_graphics/CMakeLists.txt:105`), FreeType
`VER-2-14-1` (`visage_graphics/CMakeLists.txt:162`). A consumer supplying system-installed versions
instead should be aware of potential version drift between the pinned `FetchContent` version and
whatever system version is substituted.

### Asset embedding at build time

`visage_file_embed`'s `add_embedded_resources(project, include_filename, namespace, files)` CMake
function (`visage_file_embed/CMakeLists.txt:4`) generates, per input file, a `.cpp` translation unit
defining an `EmbeddedFile` instance in the given C++ namespace, plus a header exposing them. This is
how fonts, SVG icons, and compiled shaders end up linked directly into the binary with no runtime
file loading required. `EmbeddedFile`'s shape (`visage_file_embed/embedded_file.h:25-29`):

```cpp
struct EmbeddedFile {
  const char* name = nullptr;
  const unsigned char* data = nullptr;
  int size = 0;
};
```

### Shader cross-compilation

Visage's shaders are authored once, in bgfx's `.sc` dialect, and cross-compiled at build time for
each target platform's native shading language:

- Windows → HLSL (Direct3D)
- macOS → Metal
- Linux → GLSL/SPIR-V (Vulkan)
- Emscripten → ESSL (WebGL)

This transpilation step is driven by `visage_graphics/embedded.cmake`. A `.sc` shader declares
varying inputs via `$input`, includes shared helper code, and writes `gl_FragColor`, e.g. the
built-in circle fragment shader (`visage_graphics/shaders/fs_circle.sc`):

```glsl
$input v_coordinates, v_dimensions, v_shader_values, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos

#include <shader_include.sh>

SAMPLER2D(s_gradient, 0);

void main() {
  gl_FragColor = gradient(s_gradient, v_gradient_texture_pos, v_gradient_pos, v_gradient_pos2, v_position);
  gl_FragColor.a = gl_FragColor.a * circle(v_coordinates, v_dimensions.x, v_shader_values.x, v_shader_values.y);
}
```

For rapid iteration without a full rebuild, the `ShaderEditor` widget (`visage_widgets`) recompiles
shader text live via the bundled `shaderc` binary — but it is explicitly marked in-source as
"development purposes only, not for production use" (`shader_editor.h:32-33`).

### CI-verified platform matrix

From `.github/workflows/*.yaml`:

| Platform | Runner | Generator | Notes |
|---|---|---|---|
| Windows (MSVC) | `windows-latest` | Visual Studio 17 2022, x64 | Debug config |
| Windows (MinGW) | `windows-latest` | MinGW Makefiles | Release config |
| macOS | `macos-latest` | Xcode | Universal `arm64;x86_64`, deployment target 10.15, `VISAGE_ADDRESS_SANITIZER=ON` |
| Linux | `ubuntu-latest` | Unix Makefiles, clang/clang++ | Debug, `VISAGE_ADDRESS_SANITIZER=ON`, `VISAGE_ENABLE_GRAPHICS_DEBUG_LOGGING=ON`, IPO/LTO on; Vulkan validation layers installed, confirming Vulkan is the CI-tested Linux backend |
| Emscripten | `ubuntu-latest` | `emsdk` latest, `emcmake`/`emmake` | Only platform with an automated deploy pipeline (uploads example builds to S3/R2 behind Cloudflare on every push to `main`, via `emscripten-deploy.yaml`) |

All four native platforms run the same basic sequence: configure with CMake, `cmake --build`, then
`ctest --output-on-failure`. A separate `format.yml` workflow enforces `clang-format` compliance as
a distinct check, not part of the `ctest` run.

---

## Platform Abstraction

Visage abstracts over two independent platform-specific concerns: **windowing** (native OS window
creation and event pumping) and **rendering backend** (GPU API selection via bgfx). As established
in the [Dependency Graph](#dependency-graph) section, these are handled by separate, mutually
independent modules (`visage_windowing` and `visage_graphics`).

### Windowing backends

`visage_windowing` provides the abstract `Window` base plus one concrete subclass per platform:

- `win32/windowing_win32.cpp` (Windows)
- `macos/windowing_macos.mm` (macOS/Cocoa)
- `linux/windowing_x11.cpp` (Linux/X11)
- `emscripten/windowing_emscripten.cpp` (WebAssembly/Emscripten)

Each backend supplies its own vsync/timing source for the draw callback (see
[Rendering Pipeline](#rendering-pipeline-redraw-and-staleness-mechanics)) and its own native handle
type behind the `void* parent_handle` parameter of `createPluginWindow`.

### Rendering backends (bgfx)

`visage_graphics/renderer.cpp` selects a bgfx renderer type per platform at initialization:

| Platform | Default backend | Notes |
|---|---|---|
| Windows | Direct3D11 | Upgrades to **Direct3D12** at runtime when compiled with the `USE_DIRECTX12` define **and** `bgfx::getSupportedRenderers` reports Direct3D12 as available on the machine (`renderer.cpp:126-131`). |
| macOS | Metal | `renderer.cpp:134`. |
| Linux | Vulkan | `renderer.cpp:138`; CI installs Vulkan validation layers alongside this backend. |
| Emscripten (Web) | OpenGLES | `renderer.cpp:140`. bgfx maps its `OpenGLES` backend to WebGL when compiled for Emscripten — this is presented to end users as "runs via WebGL in the browser," but the bgfx-level renderer type is `OpenGLES`. |

The Direct3D11-to-Direct3D12 upgrade logic:

```cpp
bgfx_init.type = bgfx::RendererType::Direct3D11;
#if USE_DIRECTX12
  for (int i = 0; i < num_supported; ++i) {
    if (supported_renderers[i] == bgfx::RendererType::Direct3D12)
      bgfx_init.type = bgfx::RendererType::Direct3D12;
  }
#endif
```

### Embedding Visage in a host window (plugin scenario)

The `examples/ClapPlugin` example demonstrates the canonical pattern for embedding Visage inside a
host application's window (e.g. a DAW hosting an audio plugin), exercising the
`createPluginWindow`/`ApplicationWindow::show(void*)` seam directly:

1. **`guiCreate(api, is_floating)`** (`clap_plugin.cpp:67-92`) — rejects floating windows, then
   constructs the editor without showing it yet:
   ```cpp
   app_ = std::make_unique<visage::ApplicationWindow>();
   app_->setWindowDimensions(80_vmin, 60_vmin);
   app_->onDraw() = [this](visage::Canvas& canvas) { /* ... */ };
   ```
2. **`guiSetParent(const clap_window* window)`** (`clap_plugin.cpp:103-113`) — this is where the
   editor actually attaches to the host's window: `app_->show(window->ptr);`. CLAP's own opaque
   native-handle union (`window->ptr`) resolves to `HWND` on Windows, `NSView*` on macOS, or an X11
   `Window` id on Linux, and is passed straight through as the `void* parent_window` argument. On
   Linux, the plugin additionally registers its POSIX fd with the host for event-loop integration
   (`_host.posixFdSupportRegister(app_->window()->posixFd(), ...)`) — confirming that Visage's own
   X11 event loop is **not** run standalone inside a plugin; the host polls Visage's fd instead.
3. **`guiSetSize`/`guiGetSize`/`guiAdjustSize`/`guiGetResizeHints`** proxy host resize negotiation to
   `ApplicationWindow::adjustWindowDimensions(...)`/`setNativeWindowDimensions(...)` (or
   `setWindowDimensions` on macOS, since macOS reports logical points rather than native pixels).
4. **`guiDestroy()`** (`clap_plugin.cpp:93-101`) — unregisters the POSIX fd on Linux, then calls
   `app_->close(); app_ = nullptr;`. Teardown is synchronous, driven entirely by the host calling
   `guiDestroy` — there is no async/deferred close handshake.

### Emscripten/WebAssembly-specific behavior

Building for the web changes several architectural behaviors, not just the rendering backend:

- **Main loop.** `emscripten_set_main_loop(runLoop, 0, 1)` substitutes for the native blocking
  `Window::runEventLoop()` used on other platforms — `fps=0` means the loop runs at the browser's
  own `requestAnimationFrame` cadence (`windowing_emscripten.cpp:626`). `runEventLoop()` still
  exists as an entry point but hands control to the browser's loop instead of blocking
  synchronously.
- **Live shader compilation has no native shellout.** `ShaderCompiler::compile()` branches on
  `#if VISAGE_EMSCRIPTEN` to call a `compileWebGlShader(...)` path instead of shelling out to the
  bundled `shaderc` binary via `spawnChildProcess` (`shader_editor.h:92-102`) — there is no native
  child-process spawn capability inside a WASM build.
- **No shader-folder watching.** `ShaderCompiler::watchShaders(...)`, which watches shader source
  files on disk for live-reload, is compiled out entirely under `#if !VISAGE_EMSCRIPTEN`
  (`shader_editor.h:111-118`) — there is no filesystem watching in the browser build.
- **Build/link flags.** The Emscripten example target adds `-sALLOW_MEMORY_GROWTH`,
  `-sGL_ENABLE_GET_PROC_ADDRESS`, `--bind`, and explicit `EXPORTED_FUNCTIONS`/
  `EXPORTED_RUNTIME_METHODS` (`_main`, `_pasteCallback`, `ccall`/`cwrap`/`UTF8ToString`) —
  clipboard paste is bridged in via an exported C function called from JavaScript, rather than a
  native OS clipboard API.

---

## Memory and Lifetime Ownership Model

Understanding who owns what is important when integrating Visage, since much of the API uses raw
pointers deliberately (for non-owning references) alongside `unique_ptr` (for owning transfers):

- **Top-level `ApplicationWindow`/`Window`** are **not** self-managed — they are owned by
  application `main()` or, in a plugin, by the plugin wrapper object (e.g. `ClapPlugin::app_` is a
  `std::unique_ptr<visage::ApplicationWindow>`, `examples/ClapPlugin/clap_plugin.h:88`).
  `ApplicationWindow` internally owns its native `Window` as `std::unique_ptr<Window> window_`
  (`application_window.h:75`), created inside `show()` and destroyed on `close()`/destruction.
- **Child `Frame`s** follow the dual-ownership model described under
  [Frame](#core-class-relationships) above. The non-owning `addChild(Frame*)` pattern is the
  dominant one in the example applications (e.g. a parent frame holding an array of `Frame` members
  and calling `addChild(&frame)` for each); the `unique_ptr` overload is reserved for
  dynamically-created children the parent should destroy itself.
- **`PostEffect`, `Palette`, `Image`**, etc. are attached to `Frame`/`Canvas` as raw,
  caller-owned pointers (`setPostEffect(PostEffect*)`, `setPalette(Palette*)`) — they are not copied
  or reference-counted by the attaching object. The sole exception is `BlurPostEffect` via the
  `setBlurRadius(float)` convenience path, which `Frame` does own internally.
- **`Font`** is the one type with automatic lifetime sharing: instances are reference-counted and
  cached through an internal `FontCache` singleton keyed by content hash
  (`visage_graphics/font.h:147-207`), so constructing "the same" font and size repeatedly is cheap,
  and `Font` objects are safe and cheap to pass by value.

---

## Security-Relevant Design Note

Visage has no networking, authentication, or persistence layer — it is a client-side rendering/UI
toolkit, not a service, so those concerns are out of scope by design. The one capability worth
flagging architecturally is `visage::spawnChildProcess` (`visage_utils/child_process.h:29-34`),
which launches an arbitrary external executable via `posix_spawn`/`CreateProcess`:

```cpp
static constexpr int kDefaultChildProcessTimeoutMs = 10000;   // 10 seconds
static constexpr size_t kMaxOutputSize = 1024 * 1024;         // 1 MiB captured-output cap

bool spawnChildProcess(const std::string& command, const std::string& arguments,
                       std::string& output, int timeout_ms = kDefaultChildProcessTimeoutMs);
```

Its argument-splitting is **naive whitespace splitting with no quoting/escaping support** — the
POSIX backend uses `std::getline(stream, segment, ' ')` over the `arguments` string
(`visage_utils/posix/child_process_posix.cpp:48-63`) before handing the resulting argv to
`posix_spawn`. There is no shell involved (so no shell-injection risk in the classic sense), but a
space inside attacker-influenced input will silently split into an unintended extra argument rather
than being escaped or rejected. Its only in-tree caller is `visage_widgets/shader_editor.cpp:176`,
which invokes the bundled `shaderc` compiler with fixed, developer-controlled flags — a safe usage
pattern. Any downstream integrator piping user-controlled or untrusted strings into
`spawnChildProcess` should be aware of this splitting behavior.

---

## Summary

Visage's architecture rests on a small number of consistent design decisions: a strict layering
where `visage_utils` is foundational, `visage_windowing` and `visage_graphics` are independent
siblings built only on `visage_utils`, and `visage_app` is the sole layer that couples windowing to
the UI/graphics stack; a single-threaded UI model with an independently-managed bgfx render thread;
a continuous, vsync-driven frame loop made efficient through per-`Frame` "stale children" repaint
tracking (with layout recomputation kept separate and rarer); a unified `Dimension` type reconciling
logical, native, and percentage-based units across `Layout` and `Canvas`; and a build system that
compiles seven source directories into nine object-library outputs merged into one
`visage::visage` CMake target, with build-time cross-compilation turning single-source `.sc`
shaders into each platform's native shading language. Together, these decisions let the same
`Frame`/`Canvas` application code run — largely unmodified — as a native desktop app, an embedded
audio-plugin editor, or a WebAssembly/WebGL browser application.
