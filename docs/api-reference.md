# Visage API Reference

Visage is a C++17 library for building GPU-rendered, native UI/2D-graphics applications. This
reference documents the public C++ header API — there is no network or RPC surface; every type
below is consumed by `#include`-ing a `visage_*` header and linking against the `visage::visage`
CMake target.

All types live in namespace `visage` unless otherwise noted. Signatures and file:line references
are taken directly from the headers under `Z:/Github/visage` (verified against
`code-analysis.md`'s validated addendum and cross-checked against source in this pass).

> **Audience**: application/plugin developers writing C++ against Visage. This document assumes
> familiarity with modern C++ (templates, `std::function`, smart pointers) and does not re-explain
> the build system beyond the minimum needed to link against the library (see
> [CMake Integration](#cmake-integration)).

---

## Table of Contents

1. [Core Concepts](#core-concepts)
2. [Frame](#frame) — the base UI element
3. [Canvas](#canvas) — the per-frame drawing API
4. [Layout](#layout) — flex-style layout engine
5. [Events](#events) — mouse/keyboard event types and enums
6. [Window and Window::EventHandler](#window-and-windoweventhandler) — low-level windowing
7. [ApplicationEditor, ApplicationWindow, TopLevelFrame](#applicationeditor-applicationwindow-toplevelframe)
8. [Widgets](#widgets) — `Button` family, `TextEditor`, `ColorPicker`, `GraphLine`, `HeatMap`,
   `BarList`, `PaletteColorEditor`/`PaletteValueEditor`, `ShaderEditor`, `ShaderQuad`
9. [Graphics Primitives](#graphics-primitives) — `Font`, `Path`, `Image`, `Gradient`/`Brush`, `Color`
10. [Palette and Theme System](#palette-and-theme-system)
11. [PostEffect Family](#posteffect-family)
12. [Utility Functions](#utility-functions) — `spawnChildProcess`, `visage_utils/file_system.h`
13. [CMake Integration](#cmake-integration)
14. [Enum Reference (Appendix)](#enum-reference-appendix)

---

## Core Concepts

Read this section before the class reference — several patterns recur across nearly every class.

### Module layering

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

`visage_windowing` (`Window`) and `visage_graphics` (`Canvas`) do not depend on each other; only
`visage_app` wires them together. This is why the `BringYourOwnWindow` example can create a raw
`Window` and a bare `Canvas` and pair them manually without using `ApplicationWindow` at all.

### `Dimension`: the unit-polymorphic size type

`Dimension` (`visage_utils/dimension.h`) is used throughout `Layout` and `Canvas` for any size or
position that should scale correctly across DPI settings and parent-relative percentages. A
`Dimension` wraps a closure resolved later against `(dpi_scale, parent_width, parent_height)`:

```cpp
Dimension::nativePixels(float px);    // raw device pixels, ignores dpi_scale
Dimension::logicalPixels(float px);   // px * dpi_scale — the default for a bare float literal
Dimension::widthPercent(float pct);   // pct% of parent width
Dimension::heightPercent(float pct);  // pct% of parent height
Dimension::viewMinPercent(float pct); // pct% of min(parent_width, parent_height)
Dimension::viewMaxPercent(float pct); // pct% of max(parent_width, parent_height)
```

A bare `float` implicitly converts to `Dimension::logicalPixels(value)` (see the
`Dimension(float amount)` constructor). Literal suffixes are available via
`using namespace visage::dimension;`:

| Suffix | Equivalent |
|---|---|
| `10_npx` | `Dimension::nativePixels(10)` |
| `10_px` | `Dimension::logicalPixels(10)` |
| `50_vw` / `50_vh` | `Dimension::widthPercent(50)` / `heightPercent(50)` |
| `50_vmin` / `50_vmax` | `Dimension::viewMinPercent(50)` / `viewMaxPercent(50)` |

`Dimension` supports `+`, `-`, unary `-`, `*`/`/` by scalar, and static `min`/`max` combinators, all
of which compose the underlying closures rather than resolving eagerly.

**`Canvas` drawing calls do not require `Dimension`** — every `Canvas` primitive is a template that
accepts either a plain numeric type (`float`/`int`, always interpreted as *current-scale* logical
pixels) or a `Dimension` (resolved against the current region's width/height). Internally this is
the private `Canvas::pixels<T>()` helper:

```cpp
template<typename T>
constexpr float pixels(T&& value) {
  if constexpr (std::is_same_v<std::decay_t<T>, Dimension>)
    return value.compute(state_.scale, state_.current_region->width(), state_.current_region->height());
  else
    return state_.scale * value;
}
```
(`visage_graphics/canvas.h:576-583`)

### Frame ownership: non-owning vs. owning children

`Frame::addChild` has three overloads (`frame.h:205-207`):

```cpp
void addChild(Frame* child, bool make_visible = true);              // non-owning — caller keeps ownership
void addChild(Frame& child, bool make_visible = true);              // same, by reference
void addChild(std::unique_ptr<Frame> child, bool make_visible = true); // transfers ownership to the parent
```

The common pattern in the examples is **non-owning**: a parent `Frame` subclass holds children as
data members (`Frame frames_[N];` or `ChildWidget child_;`) and calls `addChild(&child_)`. Members
are destroyed after the parent's own destructor body runs, and `Frame`'s destructor defensively
detaches from its parent and destroys any owned children first, so this pattern is safe as long as
a non-owned child's storage outlives its removal from the tree. Use the `unique_ptr` overload when
the parent should dynamically create and own a child (e.g. popups/dialogs created at runtime).

### Event dispatch pattern: virtual + `CallbackList`

Every input/lifecycle event on `Frame` follows the same shape: a virtual method with a matching
`onXxx()` accessor that returns a `CallbackList<...>&`. **Overriding the virtual and registering a
lambda via `onXxx()` both fire** — the default callback registered in the `CallbackList`'s
initializer simply calls the virtual:

```cpp
CallbackList<void(const MouseEvent&)> on_mouse_down_ { [this](auto& e) { mouseDown(e); } };
```
(`frame.h:461`)

`CallbackList<T>` (`visage_utils/events.h`) holds a list of `std::function<T>` callbacks; `add()`/
`operator+=` appends an additional callback, `set()`/`operator=` replaces the whole list with one
callback (this is what `onDraw() = [...]{ ... };` does), and `callback(args...)` invokes every
registered callback in order, returning the last one's return value.

### Threading model

`Frame`/`Canvas`/`Layout`/`Window` are **single-threaded** — all mutation and event dispatch must
happen on the thread that calls `Window::runEventLoop()` (the UI/main thread); there is no internal
locking. bgfx runs its own render thread internally regardless of application code.
`VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD` only toggles bgfx's own multithreaded submit/render
mode — it does not make `Frame`/`Canvas` safe to touch from a second thread. The one built-in
exception is `ShaderEditor`'s internal `ShaderCompiler` (a `Thread` subclass), which shells out to
`shaderc` off the UI thread and synchronizes results back via a mutex/atomic flag.

### Render/redraw pipeline

Every platform backend drives frames from its own vsync/timer source (`Window::drawCallback`) —
Visage is a continuous-redraw model at the OS level, but `ApplicationEditor::drawWindow()` is cheap
per frame when idle: it calls `drawStaleChildren()`, which only re-renders `Frame`s that called
`redraw()` since the last frame, then unconditionally calls `Canvas::submit()` to flush to bgfx.
Layout is **not** recomputed every frame — `Frame::setBounds`/`setNativeBounds` triggers
`computeLayout()` as a side effect, so layout only recomputes on an actual bounds/resize change.
Painting ("stale children") is the per-frame-conditional part.

---

## Frame

`visage_ui/frame.h` — the base class for every UI element (every widget in `visage_widgets` and the
whole `visage_app` layer derive from it).

### Construction / destruction

```cpp
Frame();
explicit Frame(std::string name);
virtual ~Frame();   // detaches from parent, destroys all children (owned and tracked)
```

### Hierarchy

```cpp
void addChild(Frame* child, bool make_visible = true);
void addChild(Frame& child, bool make_visible = true);
void addChild(std::unique_ptr<Frame> child, bool make_visible = true); // parent takes ownership
void removeChild(Frame* child);
void removeChild(Frame& child);
void removeAllChildren();
int indexOfChild(const Frame* child) const;
void setParent(Frame* parent);          // normally called internally by addChild
Frame* parent() const;
const std::vector<Frame*>& children() const;
template<typename T> T* findParent() const;   // walks up, dynamic_cast at each level
Frame* frameAtPoint(Point point);
Frame* topParentFrame();
bool containsPoint(Point point) const;
```

### Virtual event methods and their `onXxx()` callback-list accessors

| Virtual method | Signature | Accessor |
|---|---|---|
| `draw` | `virtual void draw(Canvas&)` | `onDraw()` |
| `resized` | `virtual void resized()` | `onResize()` |
| `dpiChanged` | `virtual void dpiChanged()` | `onDpiChange()` |
| `scaleChanged` | `virtual void scaleChanged()` | — (no `onXxx`) |
| `visibilityChanged` | `virtual void visibilityChanged()` | `onVisibilityChange()` |
| `hierarchyChanged` | `virtual void hierarchyChanged()` | `onHierarchyChange()` |
| `focusChanged` | `virtual void focusChanged(bool is_focused, bool was_clicked)` | `onFocusChange()` |
| `mouseEnter`/`mouseExit`/`mouseDown`/`mouseUp`/`mouseMove`/`mouseDrag` | `virtual void x(const MouseEvent&)` | `onMouseEnter()`, `onMouseExit()`, `onMouseDown()`, `onMouseUp()`, `onMouseMove()`, `onMouseDrag()` |
| `mouseWheel` | `virtual bool mouseWheel(const MouseEvent&)` | `onMouseWheel()` |
| `keyPress`/`keyRelease` | `virtual bool x(const KeyEvent&)` | `onKeyPress()`, `onKeyRelease()` |
| `textInput` | `virtual void textInput(const std::string&)` | `onTextInput()` |
| — | — | `onChildBoundsChanged()`, `onChildAdded()`, `onChildRemoved()` (no matching virtual) |

Other virtuals with **no** matching `onXxx()`: `init()`, `destroy()`,
`hitTest(const Point&) const -> HitTestResult`, `receivesTextInput() -> bool`,
`aspectRatio() const -> float`, and the drag-drop-source group: `receivesDragDropFiles()`,
`dragDropFileExtensionRegex()`, `receivesMultipleDragDropFiles()`,
`dragFilesEnter(const std::vector<std::string>&)`, `dragFilesExit()`,
`dropFiles(const std::vector<std::string>&)`, `isDragDropSource()`, `startDragDropSource()`,
`cleanupDragDropSource()`.

### Geometry / layout

```cpp
void setBounds(Bounds bounds);
void setBounds(float x, float y, float width, float height);      // triggers computeLayout()
void setNativeBounds(IBounds native_bounds);
void setNativeBounds(int x, int y, int width, int height);
void computeLayout();
void computeLayout(Frame* child);
const Bounds& bounds() const;
float x() const; float y() const; float width() const; float height() const;
float right() const; float bottom() const;
int nativeX() const; int nativeY() const; int nativeWidth() const; int nativeHeight() const;
Bounds localBounds() const;                 // { 0, 0, width(), height() }
Point positionInWindow() const;
Bounds relativeBounds(const Frame* other) const;

bool hasLayout() const;
Layout& layout();          // lazily allocates a Layout via std::make_unique<Layout>() on first call
void clearLayout();
void setFlexLayout(bool flex);   // shorthand for layout().setFlex(flex)
```

`setBounds`/`setNativeBounds` automatically call `computeLayout()` (`frame.cpp:164-176`) —
application code normally never calls `computeLayout()` directly.

### Palette / theming

```cpp
void setPalette(Palette* palette);                 // propagates recursively to all children
Palette* palette() const;
void setPaletteOverride(theme::OverrideId override_id, bool recursive = true);
theme::OverrideId paletteOverride() const;
float paletteValue(theme::ValueId value_id) const;
Brush paletteColor(theme::ColorId color_id) const;
```

### Post-effects

```cpp
void setPostEffect(PostEffect* post_effect);        // not owned — caller manages lifetime
PostEffect* postEffect() const;
void removePostEffect();
void setBackdropEffect(PostEffect* backdrop_effect); // applies to what's behind the frame (e.g. frosted glass)
PostEffect* backdropEffect() const;
void removeBackdropEffect();
void setBlurRadius(float blur_radius);              // lazily owns a BlurPostEffect internally (the one exception)
```

`setPostEffect`/`setBackdropEffect` store a raw, non-owning pointer — the caller is responsible for
the `PostEffect`'s lifetime. `setBlurRadius(float)` is the sole exception: it lazily allocates and
owns a `std::unique_ptr<BlurPostEffect>` internally.

### Visibility, interaction, redraw

```cpp
void setVisible(bool visible);   bool isVisible() const;
void setDrawing(bool drawing);   bool isDrawing() const;
void setOnTop(bool on_top);      bool isOnTop() const;
void setAcceptsKeystrokes(bool); bool acceptsKeystrokes() const;
void setReceiveChildMouseEvents(bool); bool receiveChildMouseEvents() const;
void setIgnoresMouseEvents(bool ignore, bool pass_to_children); bool ignoresMouseEvents() const;
bool hasKeyboardFocus() const;
bool tryFocusTextReceiver();
bool focusNextTextReceiver(const Frame* starting_child = nullptr) const;
bool focusPreviousTextReceiver(const Frame* starting_child = nullptr) const;

void redraw();       // no-ops unless isVisible() && isDrawing() && !already redrawing this frame
void redrawAll();    // recurses into all children
bool initialized() const;
void setCached(bool cached);
void setMasked(bool masked);
void setAlphaTransparency(float alpha);
void removeAlphaTransparency();   // == setAlphaTransparency(1.0f)
```

### DPI / scale

```cpp
void setDpiScale(float dpi_scale);   // propagates to children; fires onDpiChange() if changed
float dpiScale() const;
void setScale(float scale);          // independent "UI zoom" multiplier layered on top of DPI scale
float scale() const;
```

### Undo/redo, clipboard, cursor (delegated through `FrameEventHandler`)

```cpp
void addUndoableAction(std::unique_ptr<UndoableAction> action) const;
void triggerUndo() const;  void triggerRedo() const;
bool canUndo() const;      bool canRedo() const;

std::string readClipboardText();
void setClipboardText(const std::string& text);
void setCursorStyle(MouseCursor style);
void setCursorVisible(bool visible);
void setMouseRelativeMode(bool visible);
void requestKeyboardFocus();
```

These forward through a `FrameEventHandler*` (a struct of `std::function`s set once per tree via
`setEventHandler`) rather than talking to the platform directly — this is how `visage_ui` avoids a
hard dependency on `visage_windowing`.

---

## Canvas

`visage_graphics/canvas.h` (826 lines) — the per-frame immediate-mode drawing API. Drawing happens
inside `Frame::draw(Canvas&)` (or an `onDraw()` lambda), invoked once per stale frame via
`Frame::drawToRegion(Canvas&)`. There is no explicit `beginFrame`/`endFrame` exposed to application
code; the closest analogs are the framework-internal `beginRegion(Region*)`/`endRegion()` and the
top-level `Canvas::submit(int submit_pass = 0)`, which `ApplicationEditor::drawWindow()` calls once
per real render to flush all accumulated shapes to bgfx.

**Canvas is non-copyable** (`Canvas(const Canvas&) = delete`).

### State persistence

`Canvas::State` (brush/color, `x`/`y` offset, clamp/clip bounds, blend mode, current region) persists
across draw calls within a region until explicitly changed. `saveState()`/`restoreState()` push/pop
a state stack for scoped overrides:

```cpp
void saveState();      // pushes a copy of the current State
void restoreState();   // pops and restores it
```

### Color / brush

```cpp
void setBrush(const Brush& brush);
const Brush& brush();
void setColor(const Brush& brush);
void setColor(unsigned int color);          // 0xAARRGGBB, via Brush::solid
void setColor(const Color& color);
void setColor(theme::ColorId color_id);     // resolves through the attached Palette
void setBlendedColor(theme::ColorId from, theme::ColorId to, float t);
Brush color(theme::ColorId color_id);       // public resolver — same lookup setColor(ColorId) uses
Brush blendedColor(theme::ColorId from, theme::ColorId to, float t);
float value(theme::ValueId value_id);
void setBlendMode(BlendMode blend_mode);    // Opaque, Composite, Alpha, Add, Sub, Mult, MaskAdd, MaskRemove
```

### Coordinate space / DPI

```cpp
void setPosition(float x, float y);            // additive offset for subsequent draws
void setNativePixelScale();                    // state_.scale = 1.0f (raw device pixels)
void setLogicalPixelScale();                   // state_.scale = dpi_scale_ (default)
void setDpiScale(float scale);
float dpiScale() const;
void setClampBounds(float x, float y, float width, float height);
void trimClampBounds(float x, float y, float width, float height);
const ClampBounds& currentClampBounds() const;
bool totallyClamped() const;
```

### Drawing primitives

> **Every drawing method below is a template** on its numeric position/size parameters (`T1, T2,
> ...`), each independently accepting either a plain numeric type (interpreted as logical pixels at
> the current `Canvas` scale) or a `Dimension` (resolved against the current region's dimensions).
> Only the signature shapes are shown here (with representative parameter names), not the C++
> template boilerplate.

**Fills**

| Method | Purpose |
|---|---|
| `fill()` | Fill the entire current clamp/clip rectangle with the current brush |
| `fill(x, y, width, height)` | Fill an axis-aligned sub-rectangle |
| `fill(const Path& path, x, y, width, height)` | Fill an arbitrary vector path, scaled to `width`×`height` |
| `fill(const Path& path, x, y)` | Fill a path at its natural bounding-box size |
| `fill(const Path& path)` | Fill a path at `(0, 0)` |

**Stroking** — **only one `stroke()` overload exists**, and it operates on `Path`, not on built-in
shapes:

```cpp
void stroke(const Path& path, T x, T y, T width, T height, T stroke_width,
            Path::Join join = Path::Join::Round,
            Path::EndCap end_cap = Path::EndCap::Round,
            std::vector<float> dash_array = {},
            float dash_offset = 0.0f,
            float miter_limit = Path::kDefaultMiterLimit);
```

There is **no** `Canvas::stroke(x, y, w, h, ...)` for built-in shapes. To outline a built-in
primitive, use its dedicated border/outline method instead (see the table below).

**Circles, squircles, rings**

| Method | Purpose |
|---|---|
| `circle(x, y, width)` | Filled circle |
| `fadeCircle(x, y, width, pixel_width)` | Circle with an antialiased soft edge of `pixel_width` |
| `ring(x, y, width, thickness)` | Stroked circle outline (the "circle border" primitive) |
| `squircle(x, y, width, power = kDefaultSquirclePower /* 4.0 */)` | Superellipse ("squircle") |
| `squircleBorder(x, y, width, power, thickness)` | Squircle outline |
| `superEllipse(x, y, width, height, power)` | Non-uniform superellipse |

**Arcs** (angles in radians)

| Method | Purpose |
|---|---|
| `roundedArc(x, y, width, thickness, center_radians, radians)` | Arc/pie segment with rounded caps |
| `flatArc(x, y, width, thickness, center_radians, radians)` | Arc/pie segment with flat caps |
| `arc(x, y, width, thickness, center_radians, radians, rounded = false)` | Dispatches to `roundedArc`/`flatArc` |
| `roundedArcShadow(x, y, width, thickness, center_radians, radians, shadow_width)` | Soft-shadow variant of `roundedArc` |
| `flatArcShadow(x, y, width, thickness, center_radians, radians, shadow_width)` | Soft-shadow variant of `flatArc` |

**Lines and curves**

| Method | Purpose |
|---|---|
| `segment(ax, ay, bx, by, thickness, rounded)` | Stroked line between two points |
| `quadratic(ax, ay, bx, by, cx, cy, thickness)` | Quadratic Bézier stroke |

**Rectangles**

| Method | Purpose |
|---|---|
| `rectangle(x, y, width, height)` | Filled axis-aligned rectangle |
| `rectangleBorder(x, y, width, height, thickness)` | Rectangle outline |
| `roundedRectangle(x, y, width, height, rounding)` | Filled rounded rectangle |
| `roundedRectangleBorder(x, y, width, height, rounding, thickness)` | Rounded-rectangle outline |
| `roundedRectangleShadow(x, y, width, height, rounding, shadow_width)` | Soft shadow behind a rounded rect |
| `rectangleShadow(x, y, width, height, shadow_width)` | Soft shadow behind a sharp-cornered rect (`rounding = 0`) |
| `leftRoundedRectangle` / `rightRoundedRectangle` / `topRoundedRectangle` / `bottomRoundedRectangle`(x, y, width, height, rounding) | Rectangle rounded on one side only |
| `diamond(x, y, width, rounding)` | Diamond/rhombus shape |

**Triangles**

| Method | Purpose |
|---|---|
| `triangle(ax, ay, bx, by, cx, cy)` | Filled triangle from 3 points |
| `triangleBorder(ax, ay, bx, by, cx, cy, thickness)` | Triangle outline |
| `roundedTriangleBorder(ax, ay, bx, by, cx, cy, rounding, thickness)` | Rounded-corner triangle outline |
| `roundedTriangle(ax, ay, bx, by, cx, cy, rounding)` | Filled rounded-corner triangle |
| `triangleLeft` / `triangleRight` / `triangleUp` / `triangleDown`(x, y, width) | Arrow/caret glyphs (e.g. for dropdowns) |

**Text**

```cpp
void text(Text* text, T x, T y, T width, T height, Direction dir = Direction::Up);
void text(const String& string, const Font& font, Font::Justification justification,
          T x, T y, T width, T height, Direction dir = Direction::Up);
```
`Direction` (`visage_graphics/shapes.h`): `Left, Up, Right, Down` — controls the layout flow
direction (used for e.g. vertical/rotated labels).

**SVG**

```cpp
void svg(const Svg& svg, T x, T y);
void svg(const Svg& svg, T x, T y, T width, T height);
void svg(const unsigned char* svg_data, int svg_size, T x, T y, T width, T height);
void svg(const EmbeddedFile& file, T x, T y, T width, T height);
```

**Images**

```cpp
void image(const Image& image, T x, T y);
void image(const unsigned char* image_data, int image_size, T x, T y, T width, T height);
void image(const EmbeddedFile& image_file, T x, T y, T width, T height);
```

**Graphs and heat maps**

```cpp
void graphLine(const GraphData& data, T x, T y, T width, T height, T thickness);
void graphFill(const GraphData& data, T x, T y, T width, T height, float fill_center);
void heatMap(const HeatMapData& data, T x, T y, T width, T height);  // no-op if data is 0x0
```

**Custom shaders**

```cpp
void shader(Shader* shader, T x, T y, T width, T height);   // draws a custom ShaderWrapper quad
```

### Frame/region lifecycle (framework-internal, rarely called by application code)

```cpp
void clearDrawnShapes();
int submit(int submit_pass = 0);          // flush all accumulated shapes to bgfx
const Screenshot& takeScreenshot();
const Screenshot& screenshot() const;
void pairToWindow(void* window_handle, int width, int height);  // low-level: pair Canvas to a raw Window
void setWindowless(int width, int height);
void removeFromWindow();
int width() const; int height() const;
void setDimensions(int width, int height);
void updateTime(double time); double time() const; double deltaTime() const; int frameCount() const;
void ensureLayerExists(int layer); Layer* layer(int index);
```

### Atlases

```cpp
PathAtlas* pathAtlas();
ImageAtlas* imageAtlas();
ImageAtlas* dataAtlas();
GradientAtlas* gradientAtlas();
```

---

## Layout

`visage_ui/layout.h` — a single-axis flex layout engine, allocated lazily per `Frame` via
`Frame::layout()`. It maps closely onto CSS Flexbox concepts, using booleans/enums rather than a
`display: flex` string:

| Visage API | CSS Flexbox equivalent |
|---|---|
| `setFlex(bool)` | enables flex layout for this frame's children (`display: flex`) |
| `setFlexRows(bool rows)` | `true` = row axis (`flex-direction: row`), `false` = column axis |
| `setFlexReverseDirection(bool)` | `flex-direction: row-reverse` / `column-reverse` |
| `setFlexWrap(bool)` | `flex-wrap: wrap` (internally sets a tri-state `flex_wrap_` to `1`) |
| `setFlexWrapReverse(bool)` | `flex-wrap: wrap-reverse` (sets `flex_wrap_` to `-1`) |
| `setFlexGrow(float)` | `flex-grow` |
| `setFlexShrink(float)` | `flex-shrink` |
| `setFlexGap(Dimension gap)` | `gap` |
| `setFlexItemAlignment(ItemAlignment)` | `align-items` on the parent |
| `setFlexSelfAlignment(ItemAlignment)` | `align-self` on the child |
| `setFlexWrapAlignment(WrapAlignment)` | `align-content` (multi-line wrap alignment) |
| `setWidth(Dimension)` / `setHeight(Dimension)` | fixed-size basis |
| `setMargin(Dimension)` / `setMarginLeft/Right/Top/Bottom(Dimension)` | CSS `margin` (per side) |
| `setPadding(Dimension)` / `setPaddingLeft/Right/Top/Bottom(Dimension)` | CSS `padding`, applied to the **parent's** flex container |
| `setDimensions(Dimension width, Dimension height)` | sets both `width()`/`height()` at once |
| `boundingBox() const -> IBounds` | the computed bounding box of the last layout pass |

```cpp
enum class ItemAlignment { NotSet, Stretch, Start, Center, End };
enum class WrapAlignment { Start, Center, End, Stretch, SpaceBetween, SpaceAround, SpaceEvenly };
```

`ItemAlignment` maps to `stretch`/`flex-start`/`center`/`flex-end`. `WrapAlignment` doubles for both
single-line main-axis distribution and multi-line cross-axis alignment (`align-content`), including
`space-between`/`space-around`/`space-evenly`; there is no separate `justify-content` equivalent.
All size/spacing values are `Dimension` (see [Core Concepts](#dimension-the-unit-polymorphic-size-type)),
so layouts can mix logical pixels, native pixels, and percentages freely.

---

## Events

`visage_ui/events.h` and `visage_utils/events.h`.

### `MouseEvent` (struct)

```cpp
struct MouseEvent {
  const Frame* event_frame = nullptr;
  Point position;
  Point relative_position;
  Point window_position;
  MouseButton button_id = kMouseButtonNone;   // which button triggered this event
  int button_state = kMouseButtonNone;        // bitmask of all currently-held buttons
  int modifiers = 0;
  bool is_down = false;
  float wheel_delta_x = 0.0f, wheel_delta_y = 0.0f;
  float precise_wheel_delta_x = 0.0f, precise_wheel_delta_y = 0.0f;
  bool wheel_reversed = false;
  bool wheel_momentum = false;
  int repeat_click_count = 0;                 // for double/triple-click detection

  Point relativePosition() const; Point windowPosition() const;
  bool isAltDown() const; bool isShiftDown() const; bool isCtrlDown() const;  // RegCtrl || MacCtrl
  bool isCmdDown() const; bool isMetaDown() const; bool isOptionDown() const;
  bool isMainModifier() const;                // RegCtrl || Cmd
  bool isDown() const; bool isTouch() const;  bool hasWheelMomentum() const;
  int repeatClickCount() const;
  bool isLeftButtonCurrentlyDown() const; bool isMiddleButtonCurrentlyDown() const; bool isRightButtonCurrentlyDown() const;
  bool isLeftButton() const; bool isMiddleButton() const; bool isRightButton() const;
  bool shouldTriggerPopup() const;            // right-click, or ctrl-click on macOS
  MouseEvent relativeTo(const Frame* new_frame) const;
};
```

### `KeyEvent` (class)

```cpp
class KeyEvent {
public:
  KeyEvent(KeyCode key, int mods, bool is_down, bool repeat = false);
  KeyCode key_code = KeyCode::Unknown;
  int modifiers = 0;
  bool key_down = false;
  bool is_repeat = false;

  KeyCode keyCode() const;
  bool isAltDown/isShiftDown/isCtrlDown/isCmdDown/isMetaDown/isOptionDown() const;
  bool isMainModifier() const; bool isRepeat() const;
  KeyEvent withMainModifier() const; KeyEvent withMeta() const;
  KeyEvent withShift() const;       KeyEvent withAlt() const;   // return a copy with the bit added
};
```

### Key enums

```cpp
enum MouseButton {
  kMouseButtonNone = 0, kMouseButtonLeft = 1, kMouseButtonMiddle = 2,
  kMouseButtonRight = 4, kMouseButtonTouch = 8
};

enum Modifiers {
  kModifierNone = 0, kModifierShift = 1, kModifierRegCtrl = 2, kModifierMacCtrl = 4,
  kModifierAlt = 8 /* == kModifierOption */, kModifierCmd = 16, kModifierMeta = 32
};

enum class HitTestResult { Client, TitleBar, CloseButton, MinimizeButton, MaximizeButton };

enum class MouseCursor {
  Invisible, Arrow, IBeam, Crosshair, Pointing, Dragging,
  HorizontalResize, VerticalResize, TopLeftResize, TopRightResize,
  BottomLeftResize, BottomRightResize, MultiDirectionalResize
};
```

`KeyCode` (`visage_utils/events.h:76-323`) is a large enum. Printable keys alias their ASCII value
directly (`A = 'a'`, `Number1 = '1'`, `Return = '\n'`, `Space = ' '`, etc.); roughly 180 unprintable
keys (function keys, navigation, keypad, media keys) are each OR'd with
`kUnprintableKeycodeMask = 1 << 30`, so `isPrintableKeyCode(KeyCode)` can distinguish the two groups
cheaply: `key_code != KeyCode::Unknown && (key_code & kUnprintableKeycodeMask) == 0`.

### `CallbackList<T>`

```cpp
template<typename T> class CallbackList {
public:
  CallbackList() = default;
  explicit CallbackList(std::function<T> callback);   // sets a "default" callback re-applied by reset()
  void add(std::function<T> callback);                 // appends
  CallbackList& operator+=(std::function<T> callback);  // same, operator form
  void set(std::function<T> callback);                  // replaces the whole list with one callback
  CallbackList& operator=(const std::function<T>& callback);
  void remove(const std::function<T>& callback);
  void reset();          // clears, then re-adds the constructor-provided default (if any)
  void clear();
  bool isEmpty() const;
  template<typename... Args> auto callback(Args&&... args) const;  // invokes all; returns the last result
};
```

---

## Window and Window::EventHandler

`visage_windowing/windowing.h` — the lower-level windowing abstraction, one native backend per
platform (Win32/Cocoa/X11/Emscripten). Use this directly only for embedding scenarios that bypass
`ApplicationWindow` (see `examples/BringYourOwnWindow`); most application code uses
`ApplicationWindow` instead.

### `Window::Decoration`

```cpp
enum class Decoration { Native, Client, Popup };
```
`Native` = OS-drawn title bar. `Client` = the app draws its own title bar (see
`ClientWindowDecoration`). `Popup` = no title bar (menus/pickers).

### `Window::EventHandler` (pure-abstract inner class)

Implemented by `visage_app::WindowEventHandler`, which bridges these calls into the `Frame` tree.
Application code implementing a custom embedding layer would implement this interface directly.

```cpp
class EventHandler {
public:
  virtual HitTestResult handleHitTest(int x, int y) = 0;
  virtual HitTestResult currentHitTest() const = 0;
  virtual void handleMouseMove(int x, int y, int button_state, int modifiers) = 0;
  virtual void handleMouseDown(MouseButton button_id, int x, int y, int button_state, int modifiers, int repeat_clicks) = 0;
  virtual void handleMouseUp(MouseButton button_id, int x, int y, int button_state, int modifiers, int repeat_clicks) = 0;
  virtual void handleMouseEnter(int x, int y) = 0;
  virtual void handleMouseLeave(int last_x, int last_y, int button_state, int modifiers) = 0;
  virtual void handleMouseWheel(float delta_x, float delta_y, float precise_x, float precise_y,
                                int mouse_x, int mouse_y, int button_state, int modifiers, bool momentum) = 0;
  virtual bool handleKeyDown(KeyCode key_code, int modifiers, bool repeat) = 0;
  virtual bool handleKeyUp(KeyCode key_code, int modifiers) = 0;
  virtual bool handleTextInput(const std::string& text) = 0;
  virtual bool hasActiveTextEntry() = 0;
  virtual void handleFocusLost() = 0;
  virtual void handleFocusGained() = 0;
  virtual void handleAdjustResize(int* width, int* height, bool horizontal_resize, bool vertical_resize) { }
  virtual void handleResized(int width, int height) = 0;
  virtual void handleWindowShown() = 0;
  virtual void handleWindowHidden() = 0;
  virtual bool handleCloseRequested() = 0;
  virtual bool handleFileDrag(int x, int y, const std::vector<std::string>& files) = 0;
  virtual void handleFileDragLeave() = 0;
  virtual bool handleFileDrop(int x, int y, const std::vector<std::string>& files) = 0;
  virtual bool isDragDropSource() = 0;
  virtual std::string startDragDropSource() = 0;
  virtual void cleanupDragDropSource() = 0;
};
```

### `Window` (concrete base; one native subclass per platform)

```cpp
Window();
Window(int width, int height);
Window(const Window&) = delete;

virtual void runEventLoop() = 0;              // blocks, pumping the native event loop
virtual void* nativeHandle() const = 0;       // HWND / NSView* / X11 Window id, per platform
virtual void windowContentsResized(int width, int height) = 0;
virtual void show() = 0; virtual void showMaximized() = 0; virtual void hide() = 0; virtual void close() = 0;
virtual bool isShowing() const = 0;
virtual void setWindowTitle(const std::string& title) = 0;
virtual void setFixedAspectRatio(bool fixed) { }
virtual IPoint maxWindowDimensions() const = 0;
virtual void setAlwaysOnTop(bool on_top) { }

void setDrawCallback(std::function<void(double)> callback);  // the framework's per-frame draw hook
void setWindowSize(int width, int height);
void setNativeWindowSize(int width, int height);
void setInternalWindowSize(int width, int height);
void setDpiScale(float scale);   float dpiScale() const;
IPoint convertToNative(const Point& logical_point) const;
Point convertToLogical(const IPoint& point) const;
void setMouseRelativeMode(bool relative);
int clientWidth() const; int clientHeight() const;
void setEventHandler(EventHandler* event_handler);
void clearEventHandler();
bool hasActiveTextEntry() const;
static void setDoubleClickSpeed(int ms); static int doubleClickSpeed();
```

### Free functions

```cpp
void setCursorStyle(MouseCursor style);
void setCursorVisible(bool visible);
Point cursorPosition();
void setCursorPosition(Point window_position);
void setCursorScreenPosition(Point screen_position);
bool isMobileDevice();
void showMessageBox(std::string title, std::string message);
std::string readClipboardText();
void setClipboardText(const std::string& text);
int doubleClickSpeed(); void setDoubleClickSpeed(int ms);
float defaultDpiScale();
IBounds computeWindowBounds(const Dimension& x, const Dimension& y, const Dimension& width, const Dimension& height);
IBounds computeWindowBounds(const Dimension& width, const Dimension& height);   // x,y default to {}
void closeApplication();
```

### Factory functions

```cpp
std::unique_ptr<Window> createWindow(const Dimension& x, const Dimension& y,
                                      const Dimension& width, const Dimension& height,
                                      Window::Decoration decoration_style = Window::Decoration::Native);
std::unique_ptr<Window> createWindow(const Dimension& width, const Dimension& height,
                                      Window::Decoration decoration_style = Window::Decoration::Native);  // x,y default to {}
std::unique_ptr<Window> createPluginWindow(const Dimension& width, const Dimension& height,
                                            void* parent_handle);
```

`parent_handle` is an untyped `void*`; the platform backend casts it internally (Win32: `HWND`;
macOS: `NSView*`; X11/Linux: `::Window`, an Xlib handle — an `unsigned long`; Emscripten: unused).

---

## ApplicationEditor, ApplicationWindow, TopLevelFrame

`visage_app/application_editor.h`, `application_window.h`.

### `TopLevelFrame` (`Frame` subclass)

```cpp
explicit TopLevelFrame(ApplicationEditor* editor);
void resized() override;
void addClientDecoration();
bool hasClientDecoration() const;
```

### `ApplicationEditor` (`Frame` subclass — `Frame` is the root of the UI tree, not a separate container)

Owns a `Canvas` and a `TopLevelFrame`; can operate windowed (`addToWindow`) or windowless
(`setWindowless`, canvas rendered off-screen — used for headless snapshotting or hosts that
supply their own window).

```cpp
static constexpr int kDefaultClientTitleBarHeight = 30;

ApplicationEditor();

auto& onShow();                    // CallbackList<void()>
auto& onHide();                    // CallbackList<void()>
auto& onCloseRequested();          // CallbackList<bool()> — return false to veto the close
auto& onWindowContentsResized();   // CallbackList<void()>

void notifyContentsResized();
const Screenshot& takeScreenshot();
void setCanvasDetails();

void addToWindow(Window* window);
void setWindowless(int width, int height);
void removeFromWindow();
void drawWindow();                 // renders one frame: drawStaleChildren() then canvas_->submit()

bool isFixedAspectRatio() const;
void setFixedAspectRatio(bool fixed);
float aspectRatio() const override;   // width/height, or 1.0f if either is 0

Window* window() const;
void drawStaleChildren();
void setMinimumDimensions(float width, float height);
void adjustWindowDimensions(int* width, int* height, bool horizontal_resize, bool vertical_resize) const;
void adjustWindowDimensions(uint32_t* width, uint32_t* height, bool horizontal_resize, bool vertical_resize) const;
void addClientDecoration();
HitTestResult hitTest(const Point& position) const override;  // TitleBar within the client-decoration strip, else Client
```

### `ApplicationWindow` (inherits `ApplicationEditor`; adds native-OS window lifecycle)

```cpp
ApplicationWindow();

const std::string& title() const;
void setTitle(std::string title);
bool isAlwaysOnTop() const;
void setWindowOnTop(bool on_top);
void setWindowDecoration(Window::Decoration decoration);
void setNativeWindowDimensions(int width, int height);
void setWindowDimensions(const Dimension& width, const Dimension& height);
void setWindowDimensions(const Dimension& x, const Dimension& y, const Dimension& width, const Dimension& height);

void show();                                                       // use last-set dimensions/position
void show(void* parent_window);                                    // embed into a host window (plugin mode)
void show(const Dimension& width, const Dimension& height, void* parent_window);
void show(const Dimension& width, const Dimension& height);
void show(const Dimension& x, const Dimension& y, const Dimension& width, const Dimension& height);
void showMaximized();
void hide();
void close();
bool isShowing() const;
void runEventLoop();                                               // blocks pumping the OS event loop
```

`onDraw()` (inherited from `Frame`) is the only way to hook per-frame drawing — either override
`virtual void draw(Canvas&)` in a subclass, or assign a lambda to `onDraw()`; both are equivalent
because the `CallbackList`'s default target simply calls `draw(canvas)`.

**Minimal usage** (from `examples/Basic/basic.cpp`):

```cpp
visage::ApplicationWindow window;
window.onDraw() = [](visage::Canvas& canvas) {
  canvas.setColor(0xff000000);
  canvas.fill();
  // ... more drawing ...
};
window.show(800, 600);
window.runEventLoop();
```

**Embedding in a host window** (CLAP/VST-style, from `examples/ClapPlugin`):

```cpp
app_ = std::make_unique<visage::ApplicationWindow>();
app_->setWindowDimensions(80_vmin, 60_vmin);
app_->onDraw() = [this](visage::Canvas& canvas) { /* ... */ };
// later, once the host provides a native parent handle:
app_->show(window->ptr);          // window->ptr is the host's native handle (HWND / NSView* / X11 Window id)
// on teardown:
app_->close();
```

---

## Widgets

All widgets live under `visage_widgets/` and are built via `Frame` (only compiled when
`VISAGE_ENABLE_WIDGETS=ON`, the default).

### Button family (`button.h`)

**`Button`** (base class):

```cpp
Button();
explicit Button(const std::string& name);
auto& onToggle();                        // CallbackList<void(Button*, bool)>
virtual bool toggle();
virtual void setToggled(bool toggled);
virtual void setToggledAndNotify(bool toggled);
void draw(Canvas& canvas) final;         // dispatches to the hook below
virtual void draw(Canvas& canvas, float hover_amount);   // override this in subclasses
void setToggleOnMouseDown(bool mouse_down);
float hoverAmount() const;               // animated 0-1 hover value
void setActive(bool active); bool isActive() const;
void setUndoSetupFunction(std::function<void()> undo_setup_function);
bool wasAltClicked() const;
```

**`UiButton`** (text button, themed background):

```cpp
explicit UiButton(const std::string& text);
UiButton();                                       // == UiButton("")
explicit UiButton(const std::string& text, const Font& font);
void setFont(const Font& font);
void setActionButton(bool action = true);
void setText(const std::string& text);
void drawBorderWhenInactive(bool border);
```
Theme colors: `UiButtonBackground`, `UiButtonBackgroundHover`, `UiButtonText`, `UiButtonTextHover`,
`UiActionButtonBackground`, `UiActionButtonBackgroundHover`, `UiActionButtonText`,
`UiActionButtonTextHover`.

**`IconButton`** (SVG-icon button):

```cpp
static constexpr float kDefaultShadowRadius = 3.0f;
explicit IconButton(bool shadow = false);
explicit IconButton(const Svg& icon, bool shadow = false);
explicit IconButton(const EmbeddedFile& icon_file, bool shadow = false);
IconButton(const unsigned char* svg, int svg_size, bool shadow = false);
void setIcon(const EmbeddedFile& icon_file);
void setIcon(const unsigned char* svg, int svg_size);
void setIcon(const Svg& icon);
void setShadowRadius(const Dimension& radius);
void setMargin(const Dimension& margin);
```

**`ToggleButton`** (adds persistent toggled state; pairs with `ButtonChangeAction` for undo):

```cpp
ToggleButton();
explicit ToggleButton(const std::string& name);
bool toggle() override;
void setToggled(bool toggled) override;
virtual void toggleValueChanged();
void setToggledAndNotify(bool toggled) override;
bool toggled() const;
void setUndoable(bool undoable);
```
Theme colors: `ToggleButtonDisabled`, `ToggleButtonOff`, `ToggleButtonOffHover`, `ToggleButtonOn`,
`ToggleButtonOnHover`.

```cpp
class ButtonChangeAction : public UndoableAction {
public:
  ButtonChangeAction(ToggleButton* button, bool toggled_on);
  void undo() override;   // setToggledAndNotify(!toggled_on)
  void redo() override;   // setToggledAndNotify(toggled_on)
};
```

**`ToggleIconButton`** / **`ToggleTextButton`**: combine `ToggleButton`'s state with `IconButton`'s
icon rendering or `UiButton`'s text rendering, respectively — same constructor shapes as their
non-toggle counterparts, plus `ToggleButton`'s toggled-state API.

### `TextEditor` (`text_editor.h`, extends `ScrollableFrame`)

```cpp
static constexpr int kDefaultPasswordCharacter = '*';
static constexpr int kMaxUndoHistory = 1000;

explicit TextEditor(const std::string& name = "");

auto& onTextChange();   // CallbackList<void()>
auto& onEnterKey();     // CallbackList<void()>
auto& onEscapeKey();    // CallbackList<void()>

void setText(const String& text);
const String& text() const;
int textLength() const;
void setPassword(int character = kDefaultPasswordCharacter);  // visually masks text; underlying text is unmasked in memory
void setMultiLine(bool multi_line);
void setMaxCharacters(int max);
void setDefaultText(const String& default_text);              // placeholder text
void setFilteredCharacters(const std::string& characters);
void setNumberEntry();
void setTextFieldEntry();
void setJustification(Font::Justification justification);
void setFont(const Font& font);
const Font& font() const;
Font::Justification justification() const;
void setSelectOnFocus(bool select_on_focus);
void setActive(bool active);
void setBackgroundColorId(theme::ColorId color_id);
void setBackgroundRounding(float rounding);
void setMargin(float x, float y);

bool copyToClipboard(); bool cutToClipboard(); bool pasteFromClipboard();
bool undo(); bool redo();
bool selectAll();
void clear(); void deselect(); void cancel(); void deleteSelected();
String selection() const;
int selectionStart() const; int selectionEnd() const;
```
Theme colors: `TextEditorBackground`, `TextEditorBorder`, `TextEditorText`, `TextEditorDefaultText`,
`TextEditorCaret`, `TextEditorSelection`. Theme values: `TextEditorRounding`, `TextEditorMarginX`,
`TextEditorMarginY`.

`TextEditor` maintains its **own** undo stack (`kMaxUndoHistory = 1000` entries), independent from
the `Frame`-tree-wide `UndoHistory`/`UndoableAction` mechanism.

> **Security note**: `setPassword(character)` only masks the *displayed* glyph — the real text
> content is still held in plain memory in the `Text` object. It is a visual affordance, not an
> authentication or secure-storage mechanism.

### `ColorPicker` (`color_picker.h`)

A composite `Frame` combining `HueEditor` + `ValueSaturationEditor` + three `TextEditor`s
(hex / alpha / HDR):

```cpp
static constexpr float kHueWidth = 24.0f, kPadding = 8.0f, kEditHeight = 40.0f;

ColorPicker();
auto& onColorChange();      // CallbackList<void(const Color&)>
void setColor(const Color& color);
```

Supporting sub-widgets (usable independently): **`HueEditor`** (`setHue`/`hue()`, `onEdit()` ->
`CallbackList<void(float)>`) and **`ValueSaturationEditor`** (`setValue`/`setSaturation`/`setHueColor`,
`onEdit()` -> `CallbackList<void(float, float)>`).

### `GraphLine` (`graph_line.h`)

```cpp
explicit GraphLine(int num_points, bool loop = false);
float at(int index) const;
void set(int index, float val);
bool isFilled() const; void setFilled(bool fill);
void setFillCenter(FillCenter fill_center);   // enum: kCenter, kBottom, kTop, kCustom
void setFillCenter(float center);             // implies kCustom
float fillLocation() const;
int numPoints() const;
bool active() const; void setActive(bool active);
void setFillAlphaMult(float mult);
```
Theme colors: `LineColor`, `LineFillColor`, `LineFillColor2`, `LineDisabledColor`,
`LineDisabledFillColor`, `CenterPoint`, `GridColor`, `HoverColor`, `DragColor`. Theme value:
`LineWidth`.

### `HeatMap` (`heat_map.h`)

```cpp
HeatMap();
HeatMap(int width, int height);
void setDimensions(int width, int height);
void setOctaves(float octaves);
void setGradient(Gradient gradient);          // defaults to Gradient::kMagma
float at(int x, int y) const;
void set(int x, int y, float val);
int dataWidth() const; int dataHeight() const;
```

### `BarList` (`bar_list.h`)

```cpp
explicit BarList(int num_bars);
void setY(int index, float y);
void positionBar(int index, float x, float y, float width, float height);
int numBars() const;
```
Theme color: `BarColor`.

### `PaletteColorEditor` / `PaletteValueEditor` (`palette_editor.h`)

> There is **no** class literally named `PaletteEditor` — the header defines two separate,
> independently usable widgets.

```cpp
class PaletteColorEditor : public ScrollableFrame {
public:
  explicit PaletteColorEditor(Palette* palette);
  void setEditedPalette(Palette* palette);
  void setCurrentOverrideId(theme::OverrideId override_id);
  theme::OverrideId currentOverrideId() const;
  bool isExpanded(const std::string& group) const;
  void toggleExpandGroup(const std::string& group);
};
```
A scrollable, grouped list of theme color swatches with two embedded `ColorPicker`s for editing a
gradient's from/to stops.

```cpp
class PaletteValueEditor : public ScrollableFrame {
public:
  static constexpr int kMaxValues = 500;
  explicit PaletteValueEditor(Palette* palette);
  void setEditedPalette(Palette* palette);
  void setCurrentOverrideId(theme::OverrideId override_id);
  theme::OverrideId currentOverrideId() const;
  bool isExpanded(const std::string& group) const;
  void toggleExpandGroup(const std::string& group);
};
```
A scrollable list of numeric theme values with inline `TextEditor`s (backed by a fixed-size array
of up to `kMaxValues` editors).

### `ShaderEditor` / `ShaderCompiler` (`shader_editor.h`)

> Source comment: *"For shader development purposes only. Not for production use."*

```cpp
class ShaderEditor : public Frame {
public:
  ShaderEditor();
  void setShader(const EmbeddedFile& shader, const EmbeddedFile& original_shader);
};
```

Internally drives a `ShaderCompiler` (`public Thread`) that recompiles GLSL-like shader text live:

```cpp
class ShaderCompiler : public Thread {
public:
  enum class Platform { Linux, Mac, Windows, Emscripten };
  enum class ShaderType { Vertex, Fragment };
  enum class Backend { Glsl, Vulkan, Metal, Dx11, WebGl };

  void compile(const std::string& shader_name, std::string code, std::function<void(std::string)> callback);
  void compile(const EmbeddedFile& shader, std::string code, std::function<void(std::string)> callback);
  void watchShaders(const std::vector<std::string>& shaders);   // compiled out under Emscripten
  void watchShaderFolder(const std::string& folder_path);
};
```

On native builds, `ShaderCompiler::compile` shells out to the bundled `shaderc` binary via
`spawnChildProcess` (its only in-tree call site — `shader_editor.cpp:176`) on a background
`Thread`, synchronized back via a mutex and an `std::atomic<bool>` flag. Under Emscripten,
`compile()` instead calls `compileWebGlShader(...)` — there is no child-process capability in a
WASM build — and `watchShaders`/`watchShaderFolder` are compiled out entirely (`#if !VISAGE_EMSCRIPTEN`).

### `ShaderQuad` (`shader_quad.h`)

```cpp
ShaderQuad(const EmbeddedFile& vertex_shader, const EmbeddedFile& fragment_shader, BlendMode state);
void draw(Canvas& canvas) override;
```
The low-level full-frame custom-shader quad building block that `PostEffect`/`ShaderPostEffect` and
`Canvas::shader()` both use internally.

---

## Graphics Primitives

### `Font` (`visage_graphics/font.h`)

```cpp
enum Justification {
  kCenter = 0, kLeft = 0x1, kRight = 0x2, kTop = 0x10, kBottom = 0x20,
  kTopLeft = kTop | kLeft, kBottomLeft = kBottom | kLeft,
  kTopRight = kTop | kRight, kBottomRight = kBottom | kRight,
};

Font() = default;
Font(float size, const unsigned char* font_data, int data_size, float dpi_scale = 0.0f);
Font(float size, const EmbeddedFile& file, float dpi_scale = 0.0f);
Font(float size, const std::string& file_path, float dpi_scale = 0.0f);   // loads a .ttf from disk at runtime

Font withDpiScale(float dpi_scale) const;   // returns a modified copy
Font withSize(float size) const;
float stringWidth(const char32_t* string, int length, int character_override = 0) const;
float lineHeight() const; float capitalHeight() const; float lowerDipHeight() const;
std::vector<int> lineBreaks(const char32_t* string, int length, float width) const;
int size() const;
```

`Font` is **not** locked to embedded byte arrays — it can load a `.ttf` file at runtime via the
`file_path` constructor. Instances are reference-counted/cached through an internal `FontCache`
singleton keyed by content hash, so constructing the "same" font+size repeatedly is cheap and safe
to pass by value/copy.

### `Path` (`visage_graphics/path.h`)

Built imperatively via SVG-path-like commands, or parsed from an SVG path-data string:

```cpp
void moveTo(Point point, bool relative = false);       void moveTo(float x, float y, bool relative = false);
void lineTo(Point point, bool relative = false);        void lineTo(float x, float y, bool relative = false);
void horizontalTo(float x, bool relative = false);      void verticalTo(float y, bool relative = false);
void quadraticTo(Point control, Point end, bool relative = false);
void smoothQuadraticTo(Point end, bool relative = false);
void bezierTo(Point control1, Point control2, Point end, bool relative = false);
void smoothBezierTo(Point end_control, Point end, bool relative = false);
void arcTo(float rx, float ry, float x_axis_rotation, bool large_arc, bool sweep_flag, Point point, bool relative = false);
void close();

void addRectangle(float x, float y, float width, float height);
void addRoundedRectangle(float x, float y, float width, float height, float rx, float ry);
void addEllipse(float cx, float cy, float rx, float ry);
void addCircle(float cx, float cy, float r);

void loadSvgPath(const std::string& path);
static CommandList parseSvgPath(const std::string& path);
void loadCommands(const CommandList& commands);

Path offset(float amount, Join join = Join::Square, EndCap end_cap = EndCap::Butt, float miter_limit = kDefaultMiterLimit);
Path stroke(float stroke_width, Join join = Join::Round, EndCap end_cap = EndCap::Round,
            std::vector<float> dash_array = {}, float dash_offset = 0.0f, float miter_limit = kDefaultMiterLimit);

int numPoints() const;
Bounds boundingBox() const;
float length() const;
void setFillRule(FillRule fill_rule);   FillRule fillRule() const;
Path scaled(float mult) const;   Path translated(const Point& offset) const;
Path rotated(float angle) const; Path transformed(const Transform& transform) const;
Path reversed() const;
```

```cpp
enum class FillRule { NonZero, Positive, EvenOdd };
enum class Join { Round, Miter, Bevel, Square };
enum class EndCap { Round, Square, Butt };
```

`path.offset(...)` and `path.stroke(...)` produce derived `Path`s (outset/stroked outline), which
is what `Canvas::stroke(const Path&, ...)` uses internally.

### `Image` (`visage_graphics/image.h`)

A plain data-holding struct, **not** a codec:

```cpp
struct Image {
  Image() = default;
  Image(const unsigned char* data, int data_size, int width = 0, int height = 0);
  const unsigned char* data = nullptr;
  int data_size = 0;
  int width = 0;
  int height = 0;
};
```

`data` is expected to already be encoded image bytes (e.g. a PNG byte blob); the `ImageAtlas`/bgfx
layer decodes it internally. There is no `Image::fromFile(path)` — images are supplied as embedded
byte arrays (`EmbeddedFile`) or raw pointers the application already has in memory.

### `Gradient` / `GradientPosition` / `Brush` (`visage_graphics/gradient.h`)

```cpp
class Gradient {
public:
  static Gradient kViridis;
  static Gradient kMagma;
  static Gradient fromSampleFunction(int resolution, const std::function<Color(float)>& sample_function);
  static Gradient interpolate(const Gradient& from, const Gradient& to, float t);

  Gradient() = default;
  template<typename... Args> explicit Gradient(const Args&... colors);  // evenly spaces N colors across [0,1]

  void addColorStop(const Color& color, float position);   // custom stop; clamps position to [0,1]
  void setRepeat(bool repeat); void setReflect(bool reflect);
  const std::vector<Color>& colors() const;
  Color sample(float t) const;
  int numColors() const;
  Gradient interpolateWith(const Gradient& other, float t) const;
  Gradient withMultipliedAlpha(float mult) const;
  std::string encode() const; void decode(const std::string& data);
};
```

```cpp
struct GradientPosition {
  enum class InterpolationShape { Solid, Horizontal, Vertical, PointsLinear, Radial };
  static GradientPosition linear(const Point& from, const Point& to);
  static GradientPosition radial(const Point& center, float radius_x, float radius_y,
                                  Point focal_center, float focal_radius = 0.0f);
  static GradientPosition radial(const Point& center, float radius_x, float radius_y);
  static GradientPosition radial(const Point& center, float radius);
};
```

```cpp
class Brush {
public:
  static Brush none();
  static Brush solid(const Color& color);
  static Brush horizontal(const Gradient& gradient);
  static Brush horizontal(const Color& left, const Color& right);
  static Brush vertical(Gradient gradient);
  static Brush vertical(const Color& top, const Color& bottom);
  static Brush linear(Gradient gradient, const Point& from_position, const Point& to_position);
  static Brush linear(const Color& from_color, const Color& to_color, const Point& from_position, const Point& to_position);
  static Brush radial(Gradient gradient, const Point& center, float radius_x, float radius_y,
                      Point focal_center, float focal_radius = 0.0f);
  static Brush radial(const Color& from_color, const Color& to_color, const Point& center, float radius);
  static Brush interpolate(const Brush& from, const Brush& to, float t);

  const Gradient& gradient() const; const GradientPosition& position() const;
  Brush withMultipliedAlpha(float mult) const;
  Brush interpolateWith(const Brush& other, float t) const;
  bool isNone() const;
};
```

`Brush` pairs a `Gradient` (color stops) with a `GradientPosition` (solid / horizontal / vertical /
linear-two-point / radial) and is what `Canvas::setBrush`/`setColor` ultimately consume; the
`Brush::solid/horizontal/vertical/linear/radial` static factories are the entry points application
code typically calls.

### `Color` (`visage_graphics/color.h`)

```cpp
Color() = default;
Color(float alpha, float red, float green, float blue, float hdr = 1.0f);   // components in [0,1]
Color(unsigned int argb, float hdr = 1.0f);                                 // 0xAARRGGBB packed
static Color fromAHSV(float alpha, float hue /* [0,360) */, float saturation, float value);
static Color fromHexString(const std::string& color_string);   // "#RGB", "#RGBA", "#RRGGBB", "#RRGGBBAA" (leading # optional)

float alpha() const; float red() const; float green() const; float blue() const; float hdr() const;
float hue() const; float saturation() const; float value() const;
Color withAlpha(float alpha) const;
Color interpolateWith(const Color& other, float t) const;
unsigned int toARGB() const; unsigned int toABGR() const; unsigned int toRGB() const;
std::string toARGBHexString() const; std::string toRGBHexString() const;
```

---

## Palette and Theme System

`visage_graphics/theme.h`, `palette.h`. Color/value IDs are **not** a hand-maintained enum — they
are auto-registered at static-init time through macros that call a global counter singleton:

```cpp
#define VISAGE_THEME_COLOR(color, default_color) \
  const ::visage::theme::ColorId color = ::visage::theme::ColorId::nextId(#color, __FILE__, default_color)
#define VISAGE_THEME_DEFINE_COLOR(color) static const ::visage::theme::ColorId color   // header declaration
#define VISAGE_THEME_IMPLEMENT_COLOR(container, color, default_color) /* .cpp definition */

#define VISAGE_THEME_VALUE(value, default_value) \
  const ::visage::theme::ValueId value = ::visage::theme::ValueId::nextId(#value, __FILE__, default_value)
#define VISAGE_THEME_DEFINE_VALUE(value) static const ::visage::theme::ValueId value
#define VISAGE_THEME_IMPLEMENT_VALUE(container, value, default_value) /* .cpp definition */

#define VISAGE_THEME_PALETTE_OVERRIDE(override_name) \
  const ::visage::theme::OverrideId override_name = ::visage::theme::OverrideId::nextId(#override_name)
```

Each `ColorId`/`ValueId` carries a name, a "group" (derived from the declaring file's name), and a
default color/value. `theme::OverrideId` (default `Global`, id `0`) lets a subtree apply a themed
override without affecting the rest of the app.

```cpp
class ColorId {
public:
  static ColorId nextId(std::string name, const std::string& file_path, unsigned int default_color);
  static unsigned int defaultColor(ColorId color_id);
  static const std::string& groupName(ColorId color_id);
  static const std::string& name(ColorId color_id);
  bool isValid() const;
};

class ValueId { /* same shape as ColorId, float default_value */ };

class OverrideId {
public:
  static constexpr unsigned int kDefaultId = 0;
  static OverrideId nextId(std::string name);
  bool isDefault() const;
};
```

### `Palette`

```cpp
class Palette {
public:
  void initWithDefaults();                                    // seeds every registered ColorId/ValueId's default
  int numColors() const;
  Brush colorIndex(int index) const;
  int addColor(const Color& color = 0xffff00ff);
  int addBrush(const Brush& color);

  bool color(theme::OverrideId override_id, theme::ColorId color_id, Brush& color);   // false = "not set" (fall back to default)
  void setColor(theme::OverrideId override_id, theme::ColorId color_id, const Color& color);
  void setColor(theme::OverrideId override_id, theme::ColorId color_id, const Brush& color);
  void setColor(theme::ColorId color_id, const Color& color);   // uses the default (Global) override
  void setColor(theme::ColorId color_id, const Brush& color);

  bool value(theme::OverrideId override_id, theme::ValueId value_id, float& result);
  void setValue(theme::OverrideId override_id, theme::ValueId value_id, float value);
  void setValue(theme::ValueId value_id, float value);

  std::map<std::string, std::vector<theme::ColorId>> colorIdList(theme::OverrideId override_id);
  std::map<std::string, std::vector<theme::ValueId>> valueIdList(theme::OverrideId override_id);

  std::string encode() const;
  void decode(const std::string& data);
};
```

### Authoring a custom theme

1. Declare colors/values at file scope: `VISAGE_THEME_COLOR(MyColor, 0xff223344);` (auto-registers
   on module load).
2. Create and populate a `Palette`: `Palette palette; palette.initWithDefaults();` then
   `palette.setColor(MyColor, Color(0xff112233));` to override specific entries (or
   `palette.setColor(overrideId, MyColor, brush)` for a subtree-scoped override).
3. Apply it to the root: `rootFrame.setPalette(&palette);` — propagates to every child recursively.
4. Inside `Frame::draw(Canvas&)`, read the resolved value: `canvas.setColor(paletteColor(MyColor))`
   or the `Canvas`-side equivalent `canvas.setColor(theme::ColorId)` (resolves through the same
   lookup as `Frame::paletteColor`).
5. For an interactive editor, embed `PaletteColorEditor`/`PaletteValueEditor` pointed at the live
   `Palette*`.

---

## PostEffect Family

`visage_graphics/post_effects.h`. Attached to a `Frame` via `Frame::setPostEffect(PostEffect*)`
(applied to the frame's own rendered output) or `Frame::setBackdropEffect(PostEffect*)` (applied to
what's *behind* the frame — e.g. frosted glass). **Not owned** by the `Frame` (raw pointer, caller
manages lifetime) — the sole exception is `Frame::setBlurRadius(float)`, which internally owns its
`BlurPostEffect`.

```cpp
class PostEffect {
public:
  explicit PostEffect(bool hdr = false);
  virtual int preprocess(Region* region, int submit_pass) { return submit_pass; }
  virtual void submit(const BatchVector<SampleRegion>& batches, Layer& destination, int submit_pass) { }
  bool hdr() const;
};
```

```cpp
class BlurPostEffect : public DownsamplePostEffect {
public:
  BlurPostEffect();
  float blurRadius() const;
  void setBlurRadius(float size);   // clamped to >= 0
};
```
This is the effect `Frame::setBlurRadius(float)` creates and manages automatically.

```cpp
class BloomPostEffect : public DownsamplePostEffect {
public:
  BloomPostEffect();
  void setBloomSize(float size);        // stored internally as log2(max(1, size))
  void setBloomIntensity(float intensity);
};
```

```cpp
class ShaderPostEffect : public PostEffect {
public:
  ShaderPostEffect(const EmbeddedFile& vertex_shader, const EmbeddedFile& fragment_shader);
  BlendMode state() const; void setState(BlendMode state);
  const EmbeddedFile& vertexShader() const; const EmbeddedFile& fragmentShader() const;
  void setUniformValue(const std::string& name, float value);                              // broadcasts to all 4 vec4 components
  void setUniformValue(const std::string& name, float v1, float v2, float v3, float v4);    // full vec4
  void removeUniform(const std::string& name);
};
```

Minimum steps to write a custom `PostEffect` shader: (1) write a matching vertex+fragment `.sc`
pair (bgfx's shader dialect); (2) embed and compile them via the CMake shader-embedding path
(`visage_embed_shaders(...)`); (3) construct `ShaderPostEffect(vertex_embedded, fragment_embedded)`
and attach it via `Frame::setPostEffect(&my_effect)`.

### `BlendMode` (`visage_graphics/graphics_utils.h`)

```cpp
enum class BlendMode { Opaque, Composite, Alpha, Add, Sub, Mult, MaskAdd, MaskRemove };
```

---

## Utility Functions

### `spawnChildProcess` (`visage_utils/child_process.h`)

```cpp
static constexpr int kDefaultChildProcessTimeoutMs = 10000;   // 10 seconds
static constexpr size_t kMaxOutputSize = 1024 * 1024;         // 1 MiB captured-output cap

bool spawnChildProcess(const std::string& command, const std::string& arguments,
                        std::string& output, int timeout_ms = kDefaultChildProcessTimeoutMs);
```

Launches `command` as a child process (`posix_spawn` on POSIX, `CreateProcess` on Windows — **no
shell** is involved). Returns `true` on success; captured stdout is written into the `output`
out-parameter, capped at `kMaxOutputSize` (1 MiB), and the call fails/returns after `timeout_ms`
(default 10 s) if the child hangs.

> **Security note**: `arguments` is split into an argv array by **naive whitespace splitting with
> no quoting or escaping support** (confirmed in the POSIX backend, which reads the string token by
> token on `' '`). Concretely:
> - **Do**: pass fixed, developer-controlled argument strings where no individual token contains a
>   space — exactly how the library's only in-tree caller uses it
>   (`visage_widgets/shader_editor.cpp:176`, invoking the bundled `shaderc` binary with fixed flags).
> - **Don't**: build `arguments` by concatenating user-supplied or otherwise untrusted text (e.g. a
>   file path containing a space). There is no shell, so there is no shell-injection risk in the
>   classic sense — but a stray space in attacker-influenced input will silently split into an
>   extra, unintended argument rather than being escaped or rejected, which can change which
>   flag/positional argument the spawned process receives.
> - Always check the `bool` return value before trusting `output`, and rely on `timeout_ms` as a
>   safety bound against a hung child process.

### `visage_utils/file_system.h`

Plain filesystem helpers built on `std::filesystem` (`typedef std::filesystem::path File`). Not a
database — local-file CRUD only:

```cpp
bool replaceFileWithData(const File& file, const unsigned char* data, size_t size);
bool replaceFileWithText(const File& file, const std::string& text);
bool hasWriteAccess(const File& file);
bool fileExists(const File& file);
bool isDirectory(const File& file);
bool appendTextToFile(const File& file, const std::string& text);
std::unique_ptr<unsigned char[]> loadFileData(const File& file, size_t& size);
std::string loadFileAsString(const File& file);

File hostExecutable();
File appDataDirectory();
File userDocumentsDirectory();
File createTemporaryFile(const std::string& extension);
void createDirectory(const File& file);

std::string fileName(const File& file);
std::string fileStem(const File& file);
std::string hostName();
std::vector<File> searchForFiles(const File& directory, const std::string& regex);
std::vector<File> searchForDirectories(const File& directory, const std::string& regex);
```

### `EmbeddedFile` (`visage_file_embed/embedded_file.h`)

```cpp
struct EmbeddedFile {
  const char* name = nullptr;
  const unsigned char* data = nullptr;
  int size = 0;
};
```

Generated per-file by the CMake `add_embedded_resources(project, include_filename, namespace,
files)` function (`visage_file_embed`), which hex-encodes each input file into a `.cpp` translation
unit defining an `EmbeddedFile` instance in the given namespace. Application code references the
generated instance directly:

```cmake
file(GLOB_RECURSE FONT_TTF_FILES fonts/*.ttf)
add_embedded_resources(EmbeddedFontResources "example_fonts.h" "resources::fonts" "${FONT_TTF_FILES}")
target_link_libraries(MyApp PRIVATE visage EmbeddedFontResources)
```
```cpp
canvas.svg(resources::icons::my_icon, x, y, w, h);
Font font(size, resources::fonts::my_font);
```

---

## CMake Integration

Confirmed target name: `visage::visage` (an `ALIAS` for `visage`). Minimal `FetchContent`-based
integration:

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

Or, against a system-installed copy (`cmake --install`):

```cmake
find_package(visage REQUIRED CONFIG)
target_link_libraries(MyApp PRIVATE visage::visage)
```

Key build options (see the analysis report for the full table with defaults):
`VISAGE_ENABLE_WIDGETS` (default `ON`, builds `visage_widgets`), `VISAGE_AMALGAMATED_BUILD` (default
`ON`), `VISAGE_ENABLE_BACKGROUND_GRAPHICS_THREAD` (default `OFF`), `BUILD_SHARED_LIBS` (default
`OFF`), `VISAGE_SYSTEM_BGFX` / `VISAGE_SYSTEM_FREETYPE` (default `OFF`, use `FetchContent`-built
copies). Requires CMake ≥ 3.17 and C++17.

---

## Enum Reference (Appendix)

| Enum | Values | Declared in |
|---|---|---|
| `Window::Decoration` | `Native, Client, Popup` | `windowing.h` |
| `HitTestResult` | `Client, TitleBar, CloseButton, MinimizeButton, MaximizeButton` | `visage_utils/events.h` |
| `MouseButton` (bitmask) | `kMouseButtonNone=0, kMouseButtonLeft=1, kMouseButtonMiddle=2, kMouseButtonRight=4, kMouseButtonTouch=8` | `visage_utils/events.h` |
| `Modifiers` (bitmask) | `kModifierNone=0, kModifierShift=1, kModifierRegCtrl=2, kModifierMacCtrl=4, kModifierAlt/kModifierOption=8, kModifierCmd=16, kModifierMeta=32` | `visage_utils/events.h` |
| `MouseCursor` | `Invisible, Arrow, IBeam, Crosshair, Pointing, Dragging, HorizontalResize, VerticalResize, TopLeftResize, TopRightResize, BottomLeftResize, BottomRightResize, MultiDirectionalResize` | `visage_utils/events.h` |
| `KeyCode` | ~180+ values; printable keys alias ASCII (`A='a'`, …); unprintable keys OR `kUnprintableKeycodeMask` (`1<<30`) | `visage_utils/events.h` |
| `Layout::ItemAlignment` | `NotSet, Stretch, Start, Center, End` | `visage_ui/layout.h` |
| `Layout::WrapAlignment` | `Start, Center, End, Stretch, SpaceBetween, SpaceAround, SpaceEvenly` | `visage_ui/layout.h` |
| `Direction` | `Left, Up, Right, Down` | `visage_graphics/shapes.h` |
| `BlendMode` | `Opaque, Composite, Alpha, Add, Sub, Mult, MaskAdd, MaskRemove` | `visage_graphics/graphics_utils.h` |
| `Font::Justification` (bitmask) | `kCenter=0, kLeft=0x1, kRight=0x2, kTop=0x10, kBottom=0x20`, plus OR'd `kTopLeft/kBottomLeft/kTopRight/kBottomRight` | `visage_graphics/font.h` |
| `Path::FillRule` | `NonZero, Positive, EvenOdd` | `visage_graphics/path.h` |
| `Path::Join` | `Round, Miter, Bevel, Square` | `visage_graphics/path.h` |
| `Path::EndCap` | `Round, Square, Butt` | `visage_graphics/path.h` |
| `GradientPosition::InterpolationShape` | `Solid, Horizontal, Vertical, PointsLinear, Radial` | `visage_graphics/gradient.h` |
| `GraphLine::FillCenter` | `kCenter, kBottom, kTop, kCustom` | `visage_widgets/graph_line.h` |
| `ShaderCompiler::Platform` | `Linux, Mac, Windows, Emscripten` | `visage_widgets/shader_editor.h` |
| `ShaderCompiler::ShaderType` | `Vertex, Fragment` | `visage_widgets/shader_editor.h` |
| `ShaderCompiler::Backend` | `Glsl, Vulkan, Metal, Dx11, WebGl` | `visage_widgets/shader_editor.h` |

---

*This reference reflects the source at `Z:/Github/visage` as read for this pass; method
enumeration for `Canvas` was cross-checked directly against `visage_graphics/canvas.h` per the
critical-reader's validation notes.*
