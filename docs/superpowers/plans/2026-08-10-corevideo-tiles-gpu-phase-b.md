# CoreVideo Tiles — Background, Borders, Crop (Phase B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the CoreVideo Tiles wall a background (colour or any OBS source), tile borders with a colour and a square/rounded shape, and a per-slot left/right crop.

**Architecture:** Phase A moved the wall onto the GPU, so all three features are shader and draw-order work rather than pixel blitting. The background is drawn first (a solid colour, optionally with another OBS source rendered over it); tiles are then drawn with a rounded-rect mask and border stroke computed in the pixel shader; the per-slot crop narrows the source rectangle before the existing `solve_cover_crop` runs.

**Tech Stack:** C++17, CMake, OBS Studio plugin API (`gs_effect_*`, `gs_draw_sprite_subregion`, `obs_source_video_render`), OBS effect (HLSL-like) shipped in `data/effects/`. Tests are plain `main()` executables returning 0/1 — **no test framework in this repo**; do not introduce gtest or Catch.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-08-10-corevideo-tiles-v2-design.md`. Read the Phase B section before starting.
- **Branch:** `feat/tiles-on-main` in `C:\Users\walla\CoreVideo\cv-tiles2-wt`. Work only there.
- **Do not regress Phase A.** The neutral canvas fill, the deliberate absence of an `if (feeds.empty()) return;`, the BT.709 **full-range** conversion with chroma offset `128.0/255.0`, and the existing grid geometry are all parity-critical and already verified. Leave them alone unless a task says otherwise.
- **Crop is per tile slot, not per participant.** Slot 3's crop applies to whoever is in slot 3.
- **Border width and corner radius are in canvas pixels and do not scale with the canvas.** This deliberately differs from the gutter, which scales as `canvas_height / kSpacingDivisor`.
- **Max 9 tile slots** (`kMaxTileSlots`), **3 exclude slots** (`kMaxExcludes`) — both already exist in `src/zoom-supersource.cpp`.
- **All user-visible strings go through `obs_module_text`** with keys in `data/locale/en-US.ini`.
- **Log prefix:** `[obs-zoom-plugin]`.
- **Shaders compile at runtime, not build time.** A clean C++ build proves nothing about the effect. Every task that touches `data/effects/corevideo-tiles.effect` must be run in a real OBS and confirm `[obs-zoom-plugin] Tiles effect loaded` before being called done.
- **Reduced test build:**
  ```
  cmake -S . -B build-tests -DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_ENGINE=OFF -DCOREVIDEO_BUILD_SIDECAR=OFF -DBUILD_TESTING=ON
  cmake --build build-tests --config Debug
  ctest --test-dir build-tests -C Debug --output-on-failure
  ```
- **Full build:** `cmake --build build-rel --config Release --parallel` (already configured — do NOT re-run configure), then `ctest --test-dir build-rel -C Release`. 26 tests exist today; all must pass.
- **Install** (needs OBS closed, owner accepts UAC; syncs `data/` as well as binaries):
  `Start-Process powershell -Verb RunAs -ArgumentList '-ExecutionPolicy','Bypass','-File','C:\Users\walla\AppData\Local\Temp\claude\C--Users-walla\ee90dd79-2091-49db-b5b0-b49223466efa\scratchpad\install-tiles-build.ps1'`
- **Check OBS and meeting state yourself before touching the machine.** It has changed repeatedly. If a meeting is live and you would tear it down, stop and report instead.
- **Test meeting for live verification:** `https://myivi.zoom.us/j/8916561023?pwd=VFpxNWNBc1JNV09GMkhzYUNmbjllUT09` (owner-provided; a mimoLive test room that resets periodically).
- Stage files by explicit path — never `git add -A`. End every commit message with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `data/effects/corevideo-tiles.effect` (modify) | Add a solid-fill technique; add rounded-rect mask + border to the tile technique | 1, 3 |
| `src/zoom-tiles-effect.h` / `.cpp` (modify) | Resolve the new techniques and uniform params | 1, 3 |
| `src/zoom-tile-border.h` (new) | Pure: tile rect → content rect after border inset, with clamping | 3 |
| `tests/tile-border-test.cpp` (new) | Border inset tests | 3 |
| `src/zoom-tile-crop.h` (new) | Pure: slot crop composed with cover-crop | 4 |
| `tests/tile-crop-test.cpp` (new) | Crop composition tests | 4 |
| `src/zoom-tiles-background.h` / `.cpp` (new) | Background source: weak ref, recursion guard, showing refcount, render | 2 |
| `src/zoom-supersource.cpp` (modify) | Properties, settings parsing, draw order | 1, 2, 3, 4 |
| `data/locale/en-US.ini` (modify) | Strings for every new control | 1, 2, 3, 4 |
| `CMakeLists.txt` (modify) | Register two new tests; add `zoom-tiles-background.cpp` to the plugin target | 2, 3, 4 |

---

### Task 1: Background colour

**Files:**
- Modify: `data/effects/corevideo-tiles.effect`, `src/zoom-tiles-effect.h`, `src/zoom-tiles-effect.cpp`, `src/zoom-supersource.cpp`, `data/locale/en-US.ini`

**Interfaces:**
- Consumes: the existing `TilesEffect` struct and `s_tiles_effect`.
- Produces:
  ```cpp
  // added to struct TilesEffect
  gs_technique_t *tech_solid  = nullptr;
  gs_eparam_t    *param_color = nullptr;
  ```
  and the settings key `bg_color` (int, `0xAARRGGBB` as OBS colour properties store it).

**Why a new technique rather than reusing the I420 path.** The neutral canvas today is drawn through `I420` with 1×1 `0x80` textures, which was the right call for bit-exact parity. An arbitrary background colour has no parity constraint, and pushing an RGB colour through a YUV conversion just to fill a rectangle is indirection with no benefit.

- [ ] **Step 1: Add the solid technique to the effect**

Append to `data/effects/corevideo-tiles.effect`, after the existing `PSI420`/`technique I420` block:

```hlsl
uniform float4 fill_color;

float4 PSSolid(VertInOut vert_in) : TARGET
{
    return fill_color;
}

technique Solid
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PSSolid(vert_in);
    }
}
```

- [ ] **Step 2: Resolve the new technique and parameter**

In `src/zoom-tiles-effect.h`, add to `struct TilesEffect`:

```cpp
    gs_technique_t *tech_solid  = nullptr;
    gs_eparam_t    *param_color = nullptr;
```

and extend `valid()` to also require `tech_solid != nullptr`.

In `src/zoom-tiles-effect.cpp`, inside the `if (out.effect)` block where the other handles are resolved, add:

```cpp
        out.tech_solid  = gs_effect_get_technique(out.effect, "Solid");
        out.param_color = gs_effect_get_param_by_name(out.effect, "fill_color");
```

and extend the existing "compiled but missing" check to also fail when `out.tech_solid` or `out.param_color` is null, keeping the same loud `LOG_ERROR`.

- [ ] **Step 3: Add the property, locale string, and default**

`data/locale/en-US.ini`:
```ini
CoreVideoTiles.BackgroundColor="Background colour"
```

In `tiles_source_get_properties`, add before the canvas size properties:
```cpp
    obs_properties_add_color(props, "bg_color",
        obs_module_text("CoreVideoTiles.BackgroundColor"));
```

In `tiles_source_get_defaults`:
```cpp
    // 0xFF808080 — the neutral grey the CPU compositor used, so an existing
    // scene looks unchanged until the operator picks a colour.
    obs_data_set_default_int(settings, "bg_color", 0xFF808080);
```

- [ ] **Step 4: Store the colour and draw it**

In `tiles_source_update`, read it into a new atomic on `tiles_source`:
```cpp
    ctx->bg_color.store(static_cast<uint32_t>(obs_data_get_int(settings, "bg_color")),
                        std::memory_order_release);
```
declared as `std::atomic<uint32_t> bg_color{0xFF808080};` beside `canvas_width` — the graphics thread reads it every frame and must never block on `ctx->mutex`.

In `tiles_source_render`, replace the full-canvas neutral sprite with a solid draw.

**Channel order is a real trap here — do not assume it.** `gs_effect_set_color`
is declared as taking `uint32_t argb` (`graphics.h:442`), while OBS colour
*properties* commonly store the picker's value in the opposite byte order.
Getting it wrong renders red as blue, which is obvious once seen and invisible
in code review. Do not reason about it: set the background to pure red
(`#FF0000`) in the dialog and look at the result. If it renders blue, swap the
red and blue bytes when passing the value through, and write the observed byte
order into a comment so the next reader does not have to rediscover it.

```cpp
    // Background first, so tiles and their borders draw over it.
    gs_effect_set_color(s_tiles_effect.param_color,
                        ctx->bg_color.load(std::memory_order_acquire));
    gs_technique_t *solid = s_tiles_effect.tech_solid;
    gs_technique_begin(solid);
    if (gs_technique_begin_pass(solid, 0)) {
        gs_draw_sprite(nullptr, 0, canvas_w, canvas_h);
        gs_technique_end_pass(solid);
    }
    gs_technique_end(solid);
```

Note `gs_draw_sprite(nullptr, ...)` with explicit width and height — the standard libobs idiom for a solid rectangle with no texture.

The per-tile neutral placeholder (`tiles_draw_neutral`) stays exactly as it is — that is for tiles with no frame, and it is parity-critical.

- [ ] **Step 5: Build, install, verify in OBS**

Build, install, restart OBS. Confirm `[obs-zoom-plugin] Tiles effect loaded` (the shader changed, so this must be re-confirmed). Add or open a Tiles source and check: the default looks unchanged from before, and changing Background colour changes the gutters and margins immediately. Screenshot via obs-websocket and sample a gutter pixel to confirm it matches the chosen colour.

- [ ] **Step 6: Commit**

```bash
git add data/effects/corevideo-tiles.effect src/zoom-tiles-effect.h src/zoom-tiles-effect.cpp src/zoom-supersource.cpp data/locale/en-US.ini
git commit -m "feat(tiles): background colour"
```

---

### Task 2: Background source

**Files:**
- Create: `src/zoom-tiles-background.h`, `src/zoom-tiles-background.cpp`
- Modify: `src/zoom-supersource.cpp`, `data/locale/en-US.ini`, `CMakeLists.txt` (add `src/zoom-tiles-background.cpp` to the plugin target source list)

**Interfaces:**
- Consumes: `tiles_source` context from Task 1.
- Produces:
  ```cpp
  // Holds a weak reference to another OBS source rendered behind the tiles.
  class TilesBackground {
  public:
      // Returns false and changes nothing if `name` names a source that would
      // create a render cycle (the tiles source itself, or a scene containing
      // it). Empty name clears the selection.
      bool set_source(obs_source_t *parent, const char *name);
      // Renders the background scaled to fill canvas_w x canvas_h. No-op when
      // nothing is selected or the source has been deleted.
      void render(uint32_t canvas_w, uint32_t canvas_h);
      void clear(obs_source_t *parent);
  private:
      obs_weak_source_t *m_weak = nullptr;
      std::string        m_name;
  };
  ```

**The two hazards this task exists to handle.** Both are in the spec and both are unforgiving:

1. **Recursion.** Selecting the Tiles source itself — or a scene that contains it — is an infinite render cycle, i.e. a stack overflow that takes OBS down mid-show. `obs_source_add_active_child(parent, child)` returns `false` when the link would create a cycle. Use its return value as the gate: register the child *before* accepting the selection, and refuse the selection if it returns false.
2. **Inactive media.** A Media or Browser source that is not in any active scene does not play. Hold it with `obs_source_inc_showing` while it is selected and `obs_source_dec_showing` when the selection changes or the tiles source is destroyed, or the operator sees a frozen first frame and reports "background video is broken".

- [ ] **Step 1: Write the header**

```cpp
// src/zoom-tiles-background.h
#pragma once

#include <obs.h>

#include <string>

// A background layer for the tiles wall: an optional reference to another OBS
// source, rendered behind the tiles and scaled to the canvas.
//
// Holds a WEAK reference so selecting a source never keeps it alive after the
// operator deletes it; a deleted background silently falls back to the
// background colour rather than erroring.
class TilesBackground {
public:
    // Selects `name` as the background, replacing any previous selection.
    // An empty or null name clears it.
    //
    // Returns false and leaves the previous selection intact when the choice
    // would create a render cycle — selecting the tiles source itself, or a
    // scene that contains it. That case is not merely wrong, it is an infinite
    // recursion that would crash OBS, so it is refused rather than attempted.
    bool set_source(obs_source_t *parent, const char *name);

    // Draws the background filling canvas_w x canvas_h. No-op when nothing is
    // selected or the selected source has since been deleted.
    void render(uint32_t canvas_w, uint32_t canvas_h);

    // Releases the reference and the showing/active-child holds. Safe to call
    // when nothing is selected.
    void clear(obs_source_t *parent);

    const std::string &name() const { return m_name; }

private:
    obs_weak_source_t *m_weak = nullptr;
    std::string        m_name;
};
```

- [ ] **Step 2: Write the implementation**

```cpp
// src/zoom-tiles-background.cpp
#include "zoom-tiles-background.h"

#include <obs-module.h>

void TilesBackground::clear(obs_source_t *parent)
{
    if (m_weak) {
        obs_source_t *prev = obs_weak_source_get_source(m_weak);
        if (prev) {
            // Order matters: stop holding it visible, then break the parent
            // link, then release our strong reference.
            obs_source_dec_showing(prev);
            if (parent) obs_source_remove_active_child(parent, prev);
            obs_source_release(prev);
        }
        obs_weak_source_release(m_weak);
        m_weak = nullptr;
    }
    m_name.clear();
}

bool TilesBackground::set_source(obs_source_t *parent, const char *name)
{
    if (!name || !*name) {
        clear(parent);
        return true;
    }
    if (m_name == name) return true;  // unchanged

    obs_source_t *next = obs_get_source_by_name(name);
    if (!next) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Tiles background source not found: %s", name);
        return false;
    }

    // Register the parent/child link FIRST: this is the cycle check, and a
    // cycle here would be an infinite render recursion, not a cosmetic bug.
    if (parent && !obs_source_add_active_child(parent, next)) {
        blog(LOG_WARNING,
             "[obs-zoom-plugin] Tiles background refused (would render itself): %s",
             name);
        obs_source_release(next);
        return false;
    }

    clear(parent);

    // A Media or Browser source that is in no active scene does not play.
    // Hold it showing for as long as we reference it.
    obs_source_inc_showing(next);
    m_weak = obs_source_get_weak_source(next);
    m_name = name;
    obs_source_release(next);
    blog(LOG_INFO, "[obs-zoom-plugin] Tiles background source: %s", name);
    return true;
}

void TilesBackground::render(uint32_t canvas_w, uint32_t canvas_h)
{
    if (!m_weak || canvas_w == 0 || canvas_h == 0) return;
    obs_source_t *src = obs_weak_source_get_source(m_weak);
    if (!src) return;  // deleted since selection; fall back to the colour

    const uint32_t sw = obs_source_get_width(src);
    const uint32_t sh = obs_source_get_height(src);
    if (sw == 0 || sh == 0) { obs_source_release(src); return; }

    // Stretch to fill the canvas. Fit modes are explicitly out of scope.
    gs_matrix_push();
    gs_matrix_scale3f(static_cast<float>(canvas_w) / static_cast<float>(sw),
                      static_cast<float>(canvas_h) / static_cast<float>(sh), 1.0f);
    obs_source_video_render(src);
    gs_matrix_pop();

    obs_source_release(src);
}
```

- [ ] **Step 3: Add the property and wire it**

`data/locale/en-US.ini`:
```ini
CoreVideoTiles.BackgroundSource="Background source"
CoreVideoTiles.BackgroundNone="- none -"
```

In `tiles_source_get_properties`, after the background colour, add a list populated by enumerating sources:

```cpp
    obs_property_t *bg = obs_properties_add_list(props, "bg_source",
        obs_module_text("CoreVideoTiles.BackgroundSource"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(bg, obs_module_text("CoreVideoTiles.BackgroundNone"), "");
    obs_enum_sources([](void *param, obs_source_t *src) -> bool {
        auto *list = static_cast<obs_property_t *>(param);
        const uint32_t flags = obs_source_get_output_flags(src);
        // Video-producing sources only: an audio-only source as a background
        // is a control that can do nothing.
        if (flags & OBS_SOURCE_VIDEO) {
            const char *n = obs_source_get_name(src);
            if (n) obs_property_list_add_string(list, n, n);
        }
        return true;
    }, bg);
```

In `tiles_source_get_defaults`: `obs_data_set_default_string(settings, "bg_source", "");`

Add `TilesBackground background;` to `struct tiles_source`, guarded by `ctx->mutex`, and in `tiles_source_update` call
`ctx->background.set_source(ctx->source, obs_data_get_string(settings, "bg_source"));`
**outside** `ctx->mutex` — `set_source` calls into libobs and must not run under a lock the graphics thread takes.

In `tiles_source_destroy`, call `ctx->background.clear(ctx->source)` before the feeds are retired.

In `tiles_source_render`, call `ctx->background.render(canvas_w, canvas_h);` immediately after the background colour draw and before the tile loop.

- [ ] **Step 4: Build, install, verify all three hazards in a real OBS**

Verify, and report each observation:
1. Selecting an Image source shows it behind the tiles.
2. Selecting a Media source **plays** rather than showing a frozen first frame — this is the `inc_showing` check.
3. Selecting the Tiles source itself is **refused**, with the warning in the log, and OBS does not crash. Then select a *scene containing* the tiles source and confirm the same refusal. This is the recursion guard, and it is the one that takes OBS down if it is wrong.
4. Deleting the selected background source falls back to the background colour without error.

- [ ] **Step 5: Commit**

```bash
git add src/zoom-tiles-background.h src/zoom-tiles-background.cpp src/zoom-supersource.cpp data/locale/en-US.ini CMakeLists.txt
git commit -m "feat(tiles): render any OBS source as the wall background"
```

---

### Task 3: Borders

**Files:**
- Create: `src/zoom-tile-border.h`, `tests/tile-border-test.cpp`
- Modify: `data/effects/corevideo-tiles.effect`, `src/zoom-tiles-effect.h`, `src/zoom-tiles-effect.cpp`, `src/zoom-supersource.cpp`, `data/locale/en-US.ini`, `CMakeLists.txt`

**Interfaces:**
- Consumes: the tile rects from `snap_tile_grid_even`.
- Produces:
  ```cpp
  struct BorderParams { double width = 0.0; double radius = 0.0; };
  // Clamps width and radius so a tile can never invert or self-overlap.
  BorderParams clamp_border(double width, double radius,
                            double tile_w, double tile_h);
  ```
  plus effect uniforms `border_color`, `border_width`, `corner_radius`, `tile_size`.

**Why the border is drawn in the shader rather than as extra sprites.** Rounded corners must mask the *video*, not just outline it, so the background shows through the corner. That needs per-pixel coverage, which is what a distance field gives — and anti-aliased, which four extra rectangles never would be.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tile-border-test.cpp
// Border clamping. A border wider than half the tile, or a radius larger than
// half the shorter side, would invert the tile or produce a degenerate shape.
// These are operator-reachable via the properties dialog, so they are clamped
// rather than trusted.

#include "zoom-tile-border.h"

#include <cmath>
#include <iostream>

static bool near(double a, double b, double eps = 0.001)
{
    return std::fabs(a - b) < eps;
}

static bool check(const char *name, double got, double want)
{
    if (near(got, want)) return true;
    std::cerr << name << ": expected " << want << ", got " << got << "\n";
    return false;
}

int main()
{
    // Ordinary values pass through untouched.
    BorderParams p = clamp_border(6.0, 12.0, 620.0, 348.0);
    if (!check("width passthrough", p.width, 6.0)) return 1;
    if (!check("radius passthrough", p.radius, 12.0)) return 1;

    // Width is capped at half the SHORTER side, so the tile cannot invert.
    p = clamp_border(400.0, 0.0, 620.0, 348.0);
    if (!check("width clamped to half the short side", p.width, 174.0)) return 1;

    // Radius is capped at half the shorter side too: a larger value has no
    // additional meaning, it is just a capsule.
    p = clamp_border(0.0, 900.0, 620.0, 348.0);
    if (!check("radius clamped", p.radius, 174.0)) return 1;

    // Negative values are meaningless; treat as zero rather than inverting.
    p = clamp_border(-5.0, -5.0, 620.0, 348.0);
    if (!check("negative width floors at 0", p.width, 0.0)) return 1;
    if (!check("negative radius floors at 0", p.radius, 0.0)) return 1;

    // A degenerate tile must not produce a negative clamp.
    p = clamp_border(6.0, 6.0, 0.0, 0.0);
    if (!check("degenerate tile width", p.width, 0.0)) return 1;
    if (!check("degenerate tile radius", p.radius, 0.0)) return 1;

    std::cout << "tile-border: all tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Register the test and verify it fails**

Add inside `if(BUILD_TESTING)` in `CMakeLists.txt`, after the `CoreVideoTileTexture` registration:

```cmake
    add_executable(CoreVideoTileBorderTest
        tests/tile-border-test.cpp
    )
    target_include_directories(CoreVideoTileBorderTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileBorder
             COMMAND CoreVideoTileBorderTest)
```

Run the reduced test build. Expected: FAIL — `zoom-tile-border.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// src/zoom-tile-border.h
#pragma once

#include <algorithm>

struct BorderParams {
    double width  = 0.0;
    double radius = 0.0;
};

// Bounds the operator's border width and corner radius against the tile they
// are drawn on. Both are reachable from the properties dialog and from a
// hand-edited scene file, and both invert or degenerate the tile if trusted:
// a width past half the shorter side leaves no interior, and a radius past the
// same bound is just a capsule with extra steps.
inline BorderParams clamp_border(double width, double radius,
                                 double tile_w, double tile_h)
{
    BorderParams out;
    const double limit = std::min(tile_w, tile_h) / 2.0;
    if (limit <= 0.0) return out;  // degenerate tile: no border at all
    out.width  = std::min(std::max(width,  0.0), limit);
    out.radius = std::min(std::max(radius, 0.0), limit);
    return out;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Reduced test build; expected PASS with `CoreVideoTileBorder` among them.

- [ ] **Step 5: Add the border to the tile shader**

Replace the `PSI420` body and `technique I420` in `data/effects/corevideo-tiles.effect` so the tile technique takes a rounded-rect mask and a border stroke. Keep the existing colour maths exactly — the `128.0/255.0` offsets and the four BT.709 coefficients are parity-critical and verified:

```hlsl
uniform float4 border_color;
uniform float2 tile_size;      // tile width/height in canvas pixels
uniform float  border_width;   // canvas pixels; 0 = no border
uniform float  corner_radius;  // canvas pixels; 0 = square

// Signed distance to a rounded rectangle centred on the origin. Negative
// inside, positive outside, in the same units as `half_size`.
float rounded_rect_sd(float2 p, float2 half_size, float r)
{
    float2 q = abs(p) - (half_size - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 PSI420(VertInOut vert_in) : TARGET
{
    float y = image.Sample(def_sampler, vert_in.uv).r;
    float u = tex_u.Sample(def_sampler, vert_in.uv).r - 128.0 / 255.0;
    float v = tex_v.Sample(def_sampler, vert_in.uv).r - 128.0 / 255.0;

    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    float3 rgb = saturate(float3(r, g, b));

    // Distance field in canvas pixels, so width and radius are pixel units
    // regardless of the tile's size or the feed's resolution.
    float2 p = (vert_in.uv - 0.5) * tile_size;
    float d = rounded_rect_sd(p, tile_size * 0.5, corner_radius);

    // One pixel of anti-aliasing either side of each edge.
    float aa = max(fwidth(d), 0.0001);
    float outer = 1.0 - smoothstep(-aa, aa, d);                 // inside the tile
    float inner = 1.0 - smoothstep(-aa, aa, d + border_width);  // inside the video

    rgb = lerp(border_color.rgb, rgb, inner);
    return float4(rgb, outer);
}
```

Because tiles now have alpha at their corners, the draw must blend. In `tiles_source_render`, before the tile loop, set the blend state explicitly rather than inheriting whatever OBS last used:

```cpp
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
```
and `gs_blend_state_pop();` after the loop. All four names are confirmed present: `gs_blend_state_push`/`pop` at `graphics.h:610-611`, `gs_blend_function` at `:706`, and `GS_BLEND_SRCALPHA`/`GS_BLEND_INVSRCALPHA` at `:113-114`.

`fwidth` is a standard derivative intrinsic in both HLSL and GLSL. If the shader fails to compile on it, report that rather than working around it silently — the fallback would be to pass a per-tile pixel scale as a uniform.

- [ ] **Step 6: Resolve the new uniforms**

Add to `struct TilesEffect` in `src/zoom-tiles-effect.h`:
```cpp
    gs_eparam_t *param_border_color = nullptr;
    gs_eparam_t *param_border_width = nullptr;
    gs_eparam_t *param_corner_radius = nullptr;
    gs_eparam_t *param_tile_size = nullptr;
```
and resolve each with `gs_effect_get_param_by_name` alongside the existing ones, extending the "compiled but missing" failure check to cover them.

- [ ] **Step 7: Add the properties and set the uniforms per tile**

`data/locale/en-US.ini`:
```ini
CoreVideoTiles.BorderWidth="Border width"
CoreVideoTiles.BorderColor="Border colour"
CoreVideoTiles.BorderShape="Corner shape"
CoreVideoTiles.BorderSquare="Square"
CoreVideoTiles.BorderRounded="Rounded"
CoreVideoTiles.CornerRadius="Corner radius"
```

Properties: an int slider `border_width` (0–64, default 0), a colour `border_color` (default `0xFF000000`), a list `border_shape` (0 = Square, 1 = Rounded, default 0), and an int slider `corner_radius` (0–128, default 16) shown only when Rounded is selected — extend the existing `tiles_fill_mode_modified` pattern, or add a second modified-callback for the shape property.

In the render loop, for each tile, after computing its rect:
```cpp
        const BorderParams b = clamp_border(border_width_setting,
                                            shape_is_rounded ? radius_setting : 0.0,
                                            r.width, r.height);
        gs_effect_set_color(s_tiles_effect.param_border_color, border_color);
        gs_effect_set_float(s_tiles_effect.param_border_width, static_cast<float>(b.width));
        gs_effect_set_float(s_tiles_effect.param_corner_radius, static_cast<float>(b.radius));
        struct vec2 tile_size = { static_cast<float>(r.width), static_cast<float>(r.height) };
        gs_effect_set_vec2(s_tiles_effect.param_tile_size, &tile_size);
```
**Set these before `gs_technique_begin_pass` for that tile**, for the same reason the textures are bound first: libobs uploads pass parameters once, inside `begin_pass`.

The neutral placeholder path (`tiles_draw_neutral`) must set the same uniforms, or a tile with no frame will keep the previous tile's border geometry.

- [ ] **Step 8: Build, install, verify in OBS**

Confirm `Tiles effect loaded` (the shader changed). Then check: width 0 looks exactly like today; a non-zero width draws an even border on every tile; Square vs Rounded changes the corners; the corner radius slider appears only for Rounded; and at a rounded corner the **background shows through** rather than black or the video's corner. Screenshot and sample a corner pixel against the background colour.

- [ ] **Step 9: Commit**

```bash
git add src/zoom-tile-border.h tests/tile-border-test.cpp data/effects/corevideo-tiles.effect src/zoom-tiles-effect.h src/zoom-tiles-effect.cpp src/zoom-supersource.cpp data/locale/en-US.ini CMakeLists.txt
git commit -m "feat(tiles): tile borders with colour and square/rounded corners"
```

---

### Task 4: Per-slot left/right crop

**Files:**
- Create: `src/zoom-tile-crop.h`, `tests/tile-crop-test.cpp`
- Modify: `src/zoom-supersource.cpp`, `data/locale/en-US.ini`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `solve_cover_crop` and `CropRect` from `src/zoom-tile-grid.h`.
- Produces:
  ```cpp
  // Applies the slot's left/right crop (percent of source width), then the
  // existing cover-crop, and returns the source sub-rectangle to sample.
  CropRect solve_slot_crop(double src_width, double src_height,
                           double dst_aspect,
                           double crop_left_pct, double crop_right_pct);
  ```

**Composition order is the whole point.** The slot crop narrows the usable source first; the cover-crop then fills the tile from what remains. Reversing them would cover-crop the full frame and then chop the result, which cuts differently and would be visibly wrong for anything other than a centred subject.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tile-crop-test.cpp
// Slot crop composed with cover-crop. The composition ORDER is what this pins:
// crop first, then cover. Reversing it cuts a different part of the frame and
// is the kind of error that looks "nearly right" on a centred talking head and
// obviously wrong on anyone sitting off to one side.

#include "zoom-tile-crop.h"

#include <cmath>
#include <iostream>

static bool near(double a, double b, double eps = 0.001)
{
    return std::fabs(a - b) < eps;
}

int main()
{
    const double aspect = 16.0 / 9.0;

    // No crop: identical to plain cover-crop of a 16:9 source, i.e. the whole
    // frame.
    CropRect c = solve_slot_crop(1920.0, 1080.0, aspect, 0.0, 0.0);
    if (!near(c.x, 0.0) || !near(c.width, 1920.0) ||
        !near(c.y, 0.0) || !near(c.height, 1080.0)) {
        std::cerr << "zero crop should be the whole 16:9 frame\n";
        return 1;
    }

    // Crop 25% off the left: the usable region starts at x=480 and is 1440
    // wide. That is 1440x1080 = 4:3, which is TALLER than 16:9, so the cover
    // step keeps full width and trims height to 1440/(16/9) = 810.
    c = solve_slot_crop(1920.0, 1080.0, aspect, 25.0, 0.0);
    if (!near(c.x, 480.0)) {
        std::cerr << "left crop must move the origin to 480, got " << c.x << "\n";
        return 1;
    }
    if (!near(c.width, 1440.0)) {
        std::cerr << "left crop width wrong: " << c.width << "\n";
        return 1;
    }
    if (!near(c.height, 810.0)) {
        std::cerr << "cover step should trim height to 810, got " << c.height << "\n";
        return 1;
    }
    if (!near(c.y, (1080.0 - 810.0) / 2.0)) {
        std::cerr << "cover step should centre vertically, got y=" << c.y << "\n";
        return 1;
    }

    // Symmetric crop stays centred horizontally.
    c = solve_slot_crop(1920.0, 1080.0, aspect, 10.0, 10.0);
    if (!near(c.x + c.width / 2.0, 960.0)) {
        std::cerr << "symmetric crop should stay centred, got centre "
                  << (c.x + c.width / 2.0) << "\n";
        return 1;
    }

    // Right crop alone moves the far edge in, not the origin.
    c = solve_slot_crop(1920.0, 1080.0, aspect, 0.0, 25.0);
    if (!near(c.x, 0.0)) {
        std::cerr << "right-only crop must not move the origin\n";
        return 1;
    }
    if (!near(c.width, 1440.0)) {
        std::cerr << "right-only crop width wrong: " << c.width << "\n";
        return 1;
    }

    // Crops that would leave nothing are clamped to a usable strip rather than
    // producing a zero-width sample rect.
    c = solve_slot_crop(1920.0, 1080.0, aspect, 60.0, 60.0);
    if (c.width <= 0.0 || c.height <= 0.0) {
        std::cerr << "over-crop must clamp to a usable strip, got "
                  << c.width << "x" << c.height << "\n";
        return 1;
    }
    if (c.x < 0.0 || c.x + c.width > 1920.0) {
        std::cerr << "clamped crop escaped the source frame\n";
        return 1;
    }

    // Negative percentages are meaningless and must not widen the source.
    c = solve_slot_crop(1920.0, 1080.0, aspect, -10.0, 0.0);
    if (c.x < 0.0 || c.x + c.width > 1920.0) {
        std::cerr << "negative crop escaped the source frame\n";
        return 1;
    }

    std::cout << "tile-crop: all tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Register the test and verify it fails**

Add inside `if(BUILD_TESTING)` in `CMakeLists.txt`, after `CoreVideoTileBorder`:

```cmake
    add_executable(CoreVideoTileCropTest
        tests/tile-crop-test.cpp
        src/zoom-tile-grid.cpp
    )
    target_include_directories(CoreVideoTileCropTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileCrop
             COMMAND CoreVideoTileCropTest)
```

(`zoom-tile-grid.cpp` is needed because `solve_cover_crop` lives there.)

Run the reduced test build. Expected: FAIL — `zoom-tile-crop.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// src/zoom-tile-crop.h
#pragma once

#include "zoom-tile-grid.h"

#include <algorithm>

// The smallest fraction of the source width a slot crop may leave. Two crops
// summing past this would produce a zero- or negative-width sample rect, which
// is reachable from the properties dialog, so it is clamped rather than
// trusted.
constexpr double kMinCropRemainder = 0.1;

// Applies a slot's left/right crop and then the cover-crop, returning the
// sub-rectangle of the source frame to sample.
//
// Order matters and is the reason this is a tested unit: the slot crop narrows
// the usable source FIRST, and the cover-crop then fills the tile from what is
// left. Cover-cropping first and trimming afterwards keeps a different part of
// the frame — close enough to look plausible on a centred subject, and clearly
// wrong on anyone sitting off-centre.
inline CropRect solve_slot_crop(double src_width, double src_height,
                                double dst_aspect,
                                double crop_left_pct, double crop_right_pct)
{
    CropRect out;
    if (src_width <= 0.0 || src_height <= 0.0 || dst_aspect <= 0.0) return out;

    double left  = std::max(crop_left_pct,  0.0) / 100.0;
    double right = std::max(crop_right_pct, 0.0) / 100.0;

    // Preserve the operator's left/right ratio when scaling an over-crop back,
    // so the framing shifts the way they asked even though it is bounded.
    const double total = left + right;
    const double max_total = 1.0 - kMinCropRemainder;
    if (total > max_total && total > 0.0) {
        const double scale = max_total / total;
        left  *= scale;
        right *= scale;
    }

    const double usable_x = src_width * left;
    const double usable_w = src_width * (1.0 - left - right);

    // Cover-crop within the narrowed region, then translate back into
    // full-frame coordinates.
    const CropRect cover = solve_cover_crop(usable_w, src_height, dst_aspect);
    out.x      = usable_x + cover.x;
    out.y      = cover.y;
    out.width  = cover.width;
    out.height = cover.height;
    return out;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Reduced test build; expected PASS with `CoreVideoTileCrop` among them.

- [ ] **Step 5: Add the properties**

`data/locale/en-US.ini` — labels are built by concatenation, not substitution, so the keys are suffixes:
```ini
CoreVideoTiles.CropGroup="Per-tile crop"
CoreVideoTiles.CropLeftSuffix="crop left %"
CoreVideoTiles.CropRightSuffix="crop right %"
```

Eighteen sliders would swamp the dialog, so put them in a collapsible group. In `tiles_source_get_properties`:

```cpp
    obs_properties_t *crop_group = obs_properties_create();
    for (std::size_t i = 1; i <= kMaxTileSlots; ++i) {
        const std::string left  = "crop_left_"  + std::to_string(i);
        const std::string right = "crop_right_" + std::to_string(i);
        const std::string left_label  =
            std::string(obs_module_text("CoreVideoTiles.Tile")) + " " +
            std::to_string(i) + " " + obs_module_text("CoreVideoTiles.CropLeftSuffix");
        const std::string right_label =
            std::string(obs_module_text("CoreVideoTiles.Tile")) + " " +
            std::to_string(i) + " " + obs_module_text("CoreVideoTiles.CropRightSuffix");
        obs_properties_add_int_slider(crop_group, left.c_str(),
                                      left_label.c_str(), 0, 45, 1);
        obs_properties_add_int_slider(crop_group, right.c_str(),
                                      right_label.c_str(), 0, 45, 1);
    }
    obs_properties_add_group(props, "crop_group",
                             obs_module_text("CoreVideoTiles.CropGroup"),
                             OBS_GROUP_NORMAL, crop_group);
```

Each slider is capped at 45 so left and right together can never exceed 90%, which keeps the clamp in the header a defensive backstop rather than a routine occurrence. Defaults are 0 for all eighteen.

- [ ] **Step 6: Store and apply the crop**

Read all eighteen values in `tiles_source_update` into a fixed array on `tiles_source` (`std::array<std::pair<double,double>, kMaxTileSlots>`), guarded by `ctx->mutex` and snapshotted alongside the feed list in `tiles_source_render`.

In the render loop, replace the existing `solve_cover_crop(...)` call with:
```cpp
        const CropRect crop = solve_slot_crop(static_cast<double>(feed->tex_w),
                                              static_cast<double>(feed->tex_h),
                                              kTileAspect,
                                              slot_crop[i].first,
                                              slot_crop[i].second);
```
where `i` is the tile index. Everything downstream — the integer truncation and the scale division that Phase A settled — stays exactly as it is.

- [ ] **Step 7: Build, install, verify in OBS**

With a live wall, set Tile 1 crop left to 25% and confirm: only tile 1 changes, its framing moves right, the tile stays completely filled with no letterbox or black edge, and other tiles are untouched. Then set crop right on the same tile and confirm the framing narrows from both sides. Screenshot before and after.

- [ ] **Step 8: Commit**

```bash
git add src/zoom-tile-crop.h tests/tile-crop-test.cpp src/zoom-supersource.cpp data/locale/en-US.ini CMakeLists.txt
git commit -m "feat(tiles): per-slot left and right crop"
```

---

## Definition of Done

- `ctest --test-dir build-rel -C Release` passes, including `CoreVideoTileBorder` and `CoreVideoTileCrop` (28 tests total).
- Background colour changes the wall's empty space; a selected OBS source renders behind the tiles and **plays** if it is a media source.
- Selecting the tiles source itself, or a scene containing it, is refused with a log line and does not crash OBS.
- Borders draw at a chosen width and colour; Rounded corners are anti-aliased and the background shows through them.
- A per-slot crop changes only that slot's framing and never letterboxes.
- Every task that changed the effect confirmed `[obs-zoom-plugin] Tiles effect loaded` in a real OBS.

## Explicitly Not Done After This Plan

Background fit modes beyond stretch-to-canvas, per-tile border overrides, drop shadows, top/bottom crop, name-tile text, the subscription-count indicator, and Phase 2 lip sync. The concurrent-stream cap and the engine double-start bug are tracked separately.
