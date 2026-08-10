# CoreVideo Tiles — GPU Compositor (Phase A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw the CoreVideo Tiles wall on the GPU instead of blitting it on the CPU, with the finished wall visually identical to today.

**Architecture:** The source changes from `OBS_SOURCE_ASYNC_VIDEO` to `OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW` and implements `video_render`. The engine reader thread's half is untouched — it still copies I420 into each feed's staging buffer and `tile_take_snapshot` still swaps buffers in O(1). What changes is the back half: instead of a worker thread blitting one canvas, `video_render` uploads each changed feed's three planes to GPU textures and draws one sprite per tile through a plugin-supplied I420 effect.

**Tech Stack:** C++17, CMake, OBS Studio plugin API (libobs graphics: `gs_texture_*`, `gs_effect_*`, `gs_draw_sprite_subregion`), OBS effect (HLSL-like) shipped as a data file. Tests are plain `main()` executables returning 0/1 — **no test framework in this repo**; do not introduce gtest or Catch.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-08-10-corevideo-tiles-v2-design.md`. Read the Phase A section before starting. Phase B (background, borders, crop) is **out of scope** — do not build any of it.
- **Phase A adds no operator-visible features.** If the wall looks different at the end, that is a defect, not progress.
- **Branch:** `feat/tiles-on-main` in the worktree `C:\Users\walla\CoreVideo\cv-tiles2-wt`. Work only there.
- **Colour parity is BT.709, FULL range.** `src/zoom-supersource.cpp` currently calls `video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_FULL, ...)` with `full_range = true`. The shader must reproduce exactly that. Getting it wrong shows as washed-out or crushed faces — easy to miss on a webcam, obvious on a broadcast.
- **Geometry is unchanged.** `solve_tile_grid`, `solve_cover_crop`, the even-snapping and short-row centering stay exactly as they are and keep their existing tests. This plan changes how tiles are drawn, not where they land.
- **Log prefix:** `[obs-zoom-plugin]` — match the existing `blog()` convention.
- **`data/` is already installed** by `CMakeLists.txt:507-509` (`install(DIRECTORY data/ DESTINATION "data/obs-plugins/obs-zoom-plugin")`), so adding `data/effects/` needs **no CMake change**.
- **Graphics-context rule:** creating or destroying any `gs_*` object outside `video_render` must be wrapped in `obs_enter_graphics()` / `obs_leave_graphics()`. Inside `video_render` the context is already entered — do not nest it there.
- **Full plugin build loop** (run from the repo root with the PowerShell tool; `build-rel` is already configured, do NOT re-run the `cmake -S . -B build-rel ...` configure step):
  ```
  cmake --build build-rel --config Release --parallel
  ctest --test-dir build-rel -C Release --output-on-failure
  ```
  There are currently 25 tests; all must pass.
- **Installing into OBS needs elevation and OBS fully closed.** Use `C:\Users\walla\AppData\Local\Temp\claude\C--Users-walla\ee90dd79-2091-49db-b5b0-b49223466efa\scratchpad\install-tiles-build.ps1` via `Start-Process powershell -Verb RunAs`; the owner accepts the UAC prompt. It backs up and SHA256-verifies.
- Stage files by explicit path — never `git add -A`. End every commit message with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `docs/design-reference/tiles-gpu-parity/` | Before/after screenshots proving parity | 1, 6 |
| `data/effects/corevideo-tiles.effect` | I420→RGB conversion, BT.709 full range | 2 |
| `src/zoom-tiles-effect.h` / `.cpp` | Effect load/destroy + technique/param handles | 2 |
| `src/zoom-tile-texture.h` | Pure: does a feed need re-upload this frame? | 3 |
| `tests/tile-texture-test.cpp` | Tests for the upload-decision logic | 3 |
| `src/zoom-supersource.cpp` (modify) | `video_render` path; worker removal | 3, 4, 5 |
| `CMakeLists.txt` (modify) | Register `CoreVideoTileTextureTest`; add effect source | 2, 3 |

---

### Task 1: Capture the parity baseline

**Files:**
- Create: `docs/design-reference/tiles-gpu-parity/before-neutral.png`
- Create: `docs/design-reference/tiles-gpu-parity/README.md`

**Interfaces:**
- Consumes: nothing.
- Produces: the reference images Task 6 compares against.

**This task must run BEFORE any code changes.** Once the compositor is converted there is no way to reproduce the old renderer, and "looks about right from memory" is not a parity check. If you are starting this plan and the baseline images do not exist, stop and capture them first.

The currently-installed build is the CPU compositor. OBS exposes screenshots over obs-websocket v5 on `127.0.0.1:4455` with `auth_required: false` on this machine.

- [ ] **Step 1: Capture the no-meeting baseline**

This captures tile geometry and the neutral fill without needing a live meeting. Write this to a scratch file and run it with `node`:

```js
const ws = new WebSocket('ws://127.0.0.1:4455');
const fs = require('fs');
let nextId = 1; const pending = new Map();
function request(type, data = {}) {
  const id = String(nextId++);
  ws.send(JSON.stringify({ op: 6, d: { requestType: type, requestId: id, requestData: data } }));
  return new Promise((res, rej) => { pending.set(id, {res, rej});
    setTimeout(() => pending.has(id) && rej(new Error(type + ' timed out')), 8000); });
}
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.op === 0) return ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1 } }));
  if (m.op === 2) return run().catch((err) => { console.error(err.message); process.exitCode = 1; ws.close(); });
  if (m.op === 7) { const p = pending.get(m.d.requestId); if (!p) return;
    pending.delete(m.d.requestId);
    m.d.requestStatus.result ? p.res(m.d.responseData ?? {}) : p.rej(new Error(m.d.requestStatus.comment)); }
});
async function run() {
  const { inputs } = await request('GetInputList', { inputKind: 'corevideo_tiles_source' });
  for (const i of inputs) {
    const { imageData } = await request('GetSourceScreenshot',
      { sourceName: i.inputName, imageFormat: 'png', imageWidth: 1920 });
    const file = 'docs/design-reference/tiles-gpu-parity/before-' +
      i.inputName.replace(/[^a-z0-9]+/gi, '-').toLowerCase() + '.png';
    fs.writeFileSync(file, Buffer.from(imageData.split(',')[1], 'base64'));
    console.log('wrote', file);
  }
  ws.close();
}
```

Create the directory first. If no Tiles source exists, add one via `CreateInput` with `inputKind: 'corevideo_tiles_source'` before capturing.

- [ ] **Step 2: Capture the live-feed baseline**

**Requires the owner and a Zoom meeting with 3+ participants sending video.** If you are an agent without rig access, record in the README that this image is missing and hand back — Task 6 cannot fully verify colour parity without it.

With a wall showing real participants, run the same script and save the results as `before-live-<n>.png`. Note the participant count and canvas size.

- [ ] **Step 3: Write the README**

Create `docs/design-reference/tiles-gpu-parity/README.md` recording: the date, the plugin build (git SHA), which images are the CPU baseline, the participant count and canvas size for each, and the explicit statement that these are the "before" side of the Phase A parity check. State plainly if the live-feed baseline is missing.

- [ ] **Step 4: Commit**

```bash
git add docs/design-reference/tiles-gpu-parity/
git commit -m "docs(tiles): capture CPU compositor parity baseline"
```

---

### Task 2: Ship and load the I420 effect

**Files:**
- Create: `data/effects/corevideo-tiles.effect`
- Create: `src/zoom-tiles-effect.h`, `src/zoom-tiles-effect.cpp`
- Modify: `CMakeLists.txt` (add `src/zoom-tiles-effect.cpp` to the plugin target source list, alongside `src/zoom-supersource.cpp`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```cpp
  struct TilesEffect {
      gs_effect_t   *effect      = nullptr;
      gs_technique_t *tech_i420  = nullptr;
      gs_eparam_t   *param_y     = nullptr;
      gs_eparam_t   *param_u     = nullptr;
      gs_eparam_t   *param_v     = nullptr;
      bool valid() const { return effect && tech_i420; }
  };
  // Both enter/leave the graphics context themselves. Safe to call twice.
  bool tiles_effect_load(TilesEffect &out);
  void tiles_effect_destroy(TilesEffect &fx);
  ```

**Nothing draws with this yet.** The deliverable is that the effect compiles at load time and says so in the log — a shader that fails to compile must be loud, because the symptom otherwise is an invisible source and no explanation.

- [ ] **Step 1: Write the effect**

Create `data/effects/corevideo-tiles.effect`:

```hlsl
// I420 -> RGB for the CoreVideo Tiles wall.
//
// BT.709, FULL range. This must match set_yuv_frame_color_info() in
// src/zoom-source.cpp, which passes VIDEO_CS_709 + VIDEO_RANGE_FULL with
// full_range = true. Full range means the luma is NOT rescaled from 16-235;
// applying a limited-range rescale here would wash every face out.

uniform float4x4 ViewProj;
uniform texture2d image;   // Y plane, GS_R8
uniform texture2d tex_u;   // U plane, GS_R8, half size
uniform texture2d tex_v;   // V plane, GS_R8, half size

sampler_state def_sampler {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertInOut {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VertInOut VSDefault(VertInOut vert_in)
{
    VertInOut vert_out;
    vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv  = vert_in.uv;
    return vert_out;
}

float4 PSI420(VertInOut vert_in) : TARGET
{
    float y = image.Sample(def_sampler, vert_in.uv).r;
    float u = tex_u.Sample(def_sampler, vert_in.uv).r - 0.5;
    float v = tex_v.Sample(def_sampler, vert_in.uv).r - 0.5;

    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;

    return float4(saturate(float3(r, g, b)), 1.0);
}

technique I420
{
    pass
    {
        vertex_shader = VSDefault(vert_in);
        pixel_shader  = PSI420(vert_in);
    }
}
```

- [ ] **Step 2: Write the header**

```cpp
// src/zoom-tiles-effect.h
#pragma once

#include <graphics/graphics.h>

// The plugin-supplied effect used to draw tiles. Loaded once at module load
// and shared by every Tiles source: effects are immutable once compiled, and
// one wall per OBS instance is already the recommended configuration.
struct TilesEffect {
    gs_effect_t    *effect     = nullptr;
    gs_technique_t *tech_i420  = nullptr;
    gs_eparam_t    *param_y    = nullptr;
    gs_eparam_t    *param_u    = nullptr;
    gs_eparam_t    *param_v    = nullptr;

    bool valid() const { return effect != nullptr && tech_i420 != nullptr; }
};

// Compiles data/effects/corevideo-tiles.effect and resolves its technique and
// parameters. Enters and leaves the graphics context itself. Returns false and
// logs loudly on failure — a silently missing effect renders an invisible
// source with no explanation, which is the worst possible symptom.
bool tiles_effect_load(TilesEffect &out);

// Releases the effect. Safe to call on an unloaded/failed TilesEffect.
void tiles_effect_destroy(TilesEffect &fx);
```

- [ ] **Step 3: Write the implementation**

```cpp
// src/zoom-tiles-effect.cpp
#include "zoom-tiles-effect.h"

#include <obs-module.h>

bool tiles_effect_load(TilesEffect &out)
{
    tiles_effect_destroy(out);

    char *path = obs_module_file("effects/corevideo-tiles.effect");
    if (!path) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles effect not found: effects/corevideo-tiles.effect "
             "is missing from the plugin's data directory");
        return false;
    }

    char *error = nullptr;
    obs_enter_graphics();
    out.effect = gs_effect_create_from_file(path, &error);
    if (out.effect) {
        out.tech_i420 = gs_effect_get_technique(out.effect, "I420");
        out.param_y   = gs_effect_get_param_by_name(out.effect, "image");
        out.param_u   = gs_effect_get_param_by_name(out.effect, "tex_u");
        out.param_v   = gs_effect_get_param_by_name(out.effect, "tex_v");
    }
    obs_leave_graphics();

    bfree(path);

    if (!out.effect) {
        blog(LOG_ERROR, "[obs-zoom-plugin] Tiles effect failed to compile: %s",
             error ? error : "(no compiler message)");
        bfree(error);
        return false;
    }
    bfree(error);

    if (!out.valid() || !out.param_y || !out.param_u || !out.param_v) {
        blog(LOG_ERROR,
             "[obs-zoom-plugin] Tiles effect compiled but is missing its I420 "
             "technique or a plane parameter");
        tiles_effect_destroy(out);
        return false;
    }

    blog(LOG_INFO, "[obs-zoom-plugin] Tiles effect loaded");
    return true;
}

void tiles_effect_destroy(TilesEffect &fx)
{
    if (fx.effect) {
        obs_enter_graphics();
        gs_effect_destroy(fx.effect);
        obs_leave_graphics();
    }
    fx = TilesEffect{};
}
```

- [ ] **Step 4: Load at module load, destroy at unload**

In `src/zoom-supersource.cpp`, add a file-scope `static TilesEffect s_tiles_effect;` and two functions the module can call, declared in `src/zoom-supersource.h`:

```cpp
void zoom_supersource_load_gfx();
void zoom_supersource_unload_gfx();
```

`zoom_supersource_load_gfx()` calls `tiles_effect_load(s_tiles_effect)`; `zoom_supersource_unload_gfx()` calls `tiles_effect_destroy(s_tiles_effect)`. Call the first from `obs_module_load` in `src/plugin-main.cpp` immediately after `zoom_supersource_register();`, and the second from `obs_module_unload`. Read `src/plugin-main.cpp` to find the existing unload function and follow its structure.

- [ ] **Step 5: Add the source file to the plugin target**

In `CMakeLists.txt`, add `src/zoom-tiles-effect.cpp` to the `add_library(obs-zoom-plugin MODULE ...)` source list, next to `src/zoom-supersource.cpp`.

- [ ] **Step 6: Build and confirm the effect compiles in a real OBS**

```
cmake --build build-rel --config Release --parallel
```
Then install (see Global Constraints) and start OBS. Expected in the OBS log:
```
[obs-zoom-plugin] Tiles effect loaded
```
If instead you see a compile error, the message contains the shader compiler's output — fix the effect, do not proceed. Do not mark this step done on a successful C++ build alone; the shader is only compiled at runtime.

- [ ] **Step 7: Commit**

```bash
git add data/effects/corevideo-tiles.effect src/zoom-tiles-effect.h src/zoom-tiles-effect.cpp src/zoom-supersource.cpp src/zoom-supersource.h src/plugin-main.cpp CMakeLists.txt
git commit -m "feat(tiles): ship and load the I420 render effect"
```

---

### Task 3: Convert the source to custom draw, neutral tiles only

**Files:**
- Create: `src/zoom-tile-texture.h`
- Test: `tests/tile-texture-test.cpp`
- Modify: `src/zoom-supersource.cpp`
- Modify: `CMakeLists.txt` (inside `if(BUILD_TESTING)`, after the `CoreVideoTileFill` registration)

**Interfaces:**
- Consumes: `TilesEffect` and `s_tiles_effect` from Task 2; `solve_tile_grid` and the existing snap/geometry helpers in `src/zoom-supersource.cpp`.
- Produces:
  ```cpp
  // src/zoom-tile-texture.h
  bool tile_texture_needs_realloc(uint32_t have_w, uint32_t have_h,
                                  uint32_t want_w, uint32_t want_h);
  bool tile_texture_needs_upload(uint64_t uploaded_generation,
                                 uint64_t frame_generation);
  ```
  and, in `zoom-supersource.cpp`, a `video_render` callback wired into `obs_source_info`.

**Scope of this task:** the source stops pushing async frames and starts drawing. It draws **only the neutral placeholder tile** for every slot — no participant video yet. That is a deliberate intermediate: it isolates *geometry* parity, which is the most regression-prone part, from *colour* parity, which Task 4 adds. A reviewer should see a wall of correctly-placed, correctly-sized grey tiles.

**How neutral is drawn, and why it matters for parity.** The CPU path fills neutral tiles with I420 `(0x80, 0x80, 0x80)` and lets OBS convert with BT.709 full range. Converting that by hand into an RGB constant introduces rounding that will not match. Instead, create one shared 1×1 texture per plane holding the byte `0x80` and draw neutral tiles through the *same* `I420` technique. The conversion is then bit-identical by construction rather than by arithmetic you have to trust.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/tile-texture-test.cpp
// The upload-decision rules for tile textures. Getting these wrong is either a
// per-frame GPU upload of unchanged pixels (a performance bug that is invisible
// until a 9-tile 4K wall) or a frozen tile (a correctness bug that is obvious
// but hard to attribute).

#include "zoom-tile-texture.h"

#include <iostream>

static bool check(const char *name, bool got, bool want)
{
    if (got == want) return true;
    std::cerr << name << ": expected " << want << ", got " << got << "\n";
    return false;
}

int main()
{
    // Realloc: only when the dimensions actually differ.
    if (!check("same size needs no realloc",
               tile_texture_needs_realloc(640, 360, 640, 360), false)) return 1;
    if (!check("width change needs realloc",
               tile_texture_needs_realloc(640, 360, 1280, 360), true)) return 1;
    if (!check("height change needs realloc",
               tile_texture_needs_realloc(640, 360, 640, 720), true)) return 1;
    // A texture that does not exist yet reports 0x0 and must allocate.
    if (!check("unallocated needs realloc",
               tile_texture_needs_realloc(0, 0, 640, 360), true)) return 1;
    // A zero-sized request must not allocate.
    if (!check("zero request needs no realloc",
               tile_texture_needs_realloc(640, 360, 0, 0), false)) return 1;

    // Upload: only when the feed produced a newer frame than we uploaded.
    if (!check("new frame uploads", tile_texture_needs_upload(3, 4), true)) return 1;
    if (!check("same frame skips", tile_texture_needs_upload(4, 4), false)) return 1;
    // Generation 0 means "no frame has ever been stored" — nothing to upload.
    if (!check("no frame yet skips", tile_texture_needs_upload(0, 0), false)) return 1;
    // A feed rebuilt from scratch restarts its generation; a lower incoming
    // generation still means "different from what we uploaded", so upload it
    // rather than showing the previous assignee's last frame forever.
    if (!check("generation went backwards uploads",
               tile_texture_needs_upload(9, 2), true)) return 1;

    std::cout << "tile-texture: all tests passed\n";
    return 0;
}
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Add inside the existing `if(BUILD_TESTING)` block in `CMakeLists.txt`, after the `add_test(NAME CoreVideoTileFill ...)` lines:

```cmake
    add_executable(CoreVideoTileTextureTest
        tests/tile-texture-test.cpp
    )
    target_include_directories(CoreVideoTileTextureTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME CoreVideoTileTexture
             COMMAND CoreVideoTileTextureTest)
```

Run:
```
cmake -S . -B build-tests -DCOREVIDEO_BUILD_PLUGIN=OFF -DCOREVIDEO_BUILD_ENGINE=OFF -DCOREVIDEO_BUILD_SIDECAR=OFF -DBUILD_TESTING=ON
cmake --build build-tests --config Debug
```
Expected: FAIL — `zoom-tile-texture.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// src/zoom-tile-texture.h
#pragma once

#include <cstdint>

// Whether a plane texture must be recreated before the next upload. Textures
// are sized to the participant's stream, which changes when the engine raises
// or lowers that participant's resolution. Kept pure so the rules are testable
// without a graphics device.
inline bool tile_texture_needs_realloc(uint32_t have_w, uint32_t have_h,
                                       uint32_t want_w, uint32_t want_h)
{
    if (want_w == 0 || want_h == 0) return false;  // nothing to allocate for
    return have_w != want_w || have_h != want_h;
}

// Whether the feed has pixels we have not uploaded yet. Generation 0 means no
// frame has ever been stored for this feed. Any difference counts, not just an
// increase: a rebuilt slot restarts its generation, and treating a lower value
// as "already uploaded" would pin the previous assignee's last frame on screen.
inline bool tile_texture_needs_upload(uint64_t uploaded_generation,
                                      uint64_t frame_generation)
{
    if (frame_generation == 0) return false;
    return uploaded_generation != frame_generation;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```
cmake --build build-tests --config Debug
ctest --test-dir build-tests -C Debug --output-on-failure
```
Expected: PASS, with `CoreVideoTileTexture` among them.

- [ ] **Step 5: Switch the source to custom draw**

In `src/zoom-supersource.cpp`:

1. Change the registration in `zoom_supersource_register()` from
   `OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE` to
   `OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE`,
   and set `info.video_render = tiles_source_render;`.
2. Add a file-scope neutral texture set, created lazily inside `video_render`
   (the graphics context is already entered there) and destroyed in
   `zoom_supersource_unload_gfx()`:

```cpp
// One shared 1x1 sample per plane holding the same 0x80 the CPU compositor
// wrote. Drawing the placeholder through the I420 technique makes it
// bit-identical to the old neutral fill by construction, rather than by an
// RGB constant that has to be trusted to round the same way.
static gs_texture_t *s_neutral_y = nullptr;
static gs_texture_t *s_neutral_u = nullptr;
static gs_texture_t *s_neutral_v = nullptr;

static bool ensure_neutral_textures()
{
    if (s_neutral_y) return true;
    static const uint8_t kByte = 0x80;
    const uint8_t *data = &kByte;
    s_neutral_y = gs_texture_create(1, 1, GS_R8, 1, &data, 0);
    s_neutral_u = gs_texture_create(1, 1, GS_R8, 1, &data, 0);
    s_neutral_v = gs_texture_create(1, 1, GS_R8, 1, &data, 0);
    return s_neutral_y && s_neutral_u && s_neutral_v;
}
```

3. Write the render callback. It solves the same grid the compositor solved and
   draws one neutral sprite per tile:

```cpp
static void tiles_source_render(void *data, gs_effect_t *)
{
    auto *ctx = static_cast<tiles_source *>(data);
    if (!s_tiles_effect.valid() || !ensure_neutral_textures()) return;

    const uint32_t canvas_w = ctx->canvas_width.load(std::memory_order_acquire);
    const uint32_t canvas_h = ctx->canvas_height.load(std::memory_order_acquire);

    // Snapshot the feed list under the lock, then release it — the draw below
    // must never hold ctx->mutex, which update/roster changes also take.
    std::vector<TileFeedPtr> feeds;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        feeds = ctx->feeds;
    }
    if (feeds.empty()) return;

    // Byte-for-byte the CPU path's parameters, including the even-snapping
    // pass. Use the named constants, not literals: substituting 16.0/9.0 or
    // 135.0 here is exactly how geometry silently drifts.
    TileGridParams params;
    params.canvas_width  = static_cast<double>(canvas_w);
    params.canvas_height = static_cast<double>(canvas_h);
    params.tile_aspect   = kTileAspect;
    params.gutter        = static_cast<double>(canvas_h) / kSpacingDivisor;
    params.margin        = params.gutter;
    const std::vector<TileRect> rects =
        snap_tile_grid_even(solve_tile_grid(feeds.size(), params), params);

    gs_technique_t *tech = s_tiles_effect.tech_i420;
    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);
    gs_effect_set_texture(s_tiles_effect.param_y, s_neutral_y);
    gs_effect_set_texture(s_tiles_effect.param_u, s_neutral_u);
    gs_effect_set_texture(s_tiles_effect.param_v, s_neutral_v);

    for (const TileRect &r : rects) {
        gs_matrix_push();
        gs_matrix_translate3f(static_cast<float>(r.x), static_cast<float>(r.y), 0.0f);
        gs_draw_sprite(s_neutral_y, 0, static_cast<uint32_t>(r.width),
                       static_cast<uint32_t>(r.height));
        gs_matrix_pop();
    }

    gs_technique_end_pass(tech);
    gs_technique_end(tech);
}
```

The parameter construction and the `snap_tile_grid_even()` wrapper above are copied verbatim from the existing compositor (`src/zoom-supersource.cpp:647-657`). Do not "simplify" them: `kTileAspect` and `kSpacingDivisor` are file constants, and dropping the snapping pass moves every tile by up to a pixel, which is precisely the regression this task exists to catch.

4. Leave the worker thread running for now but stop it from calling
   `obs_source_output_video`; Task 5 removes it. The simplest correct
   intermediate is to return early from the compositor's output step.

- [ ] **Step 6: Build, install, and confirm geometry**

Build, install, restart OBS. Expected: the Tiles source shows correctly-sized, evenly-spaced grey tiles in the same positions as the baseline image from Task 1, reflowing with the participant count. No participant video yet — that is correct for this task.

Compare against `docs/design-reference/tiles-gpu-parity/before-neutral.png` and state in the commit whether the geometry matches.

- [ ] **Step 7: Commit**

```bash
git add src/zoom-tile-texture.h tests/tile-texture-test.cpp src/zoom-supersource.cpp CMakeLists.txt
git commit -m "feat(tiles): draw the wall through the graphics pipeline"
```

---

### Task 4: Upload and draw participant video

**Files:**
- Modify: `src/zoom-supersource.cpp`

**Interfaces:**
- Consumes: `tile_texture_needs_realloc` and `tile_texture_needs_upload` from Task 3; `TilesEffect` from Task 2; the existing `TileScratch` / `tile_take_snapshot` machinery and `solve_cover_crop`.
- Produces: nothing new — this completes Phase A.

**This is the task that restores full parity.** After it, the wall shows participant video and should be indistinguishable from the baseline.

**Cover-crop maps onto `gs_draw_sprite_subregion`.** The existing `solve_cover_crop(src_w, src_h, dst_aspect)` returns the sub-rectangle of the source to sample. `gs_draw_sprite_subregion(tex, flip, x, y, cx, cy)` samples exactly such a sub-rectangle. Do not reimplement the crop maths — call the tested function and pass its result through.

- [ ] **Step 1: Add per-feed texture state**

Add to `struct TileFeed` in `src/zoom-supersource.cpp`, in the block owned by the composite side (these are touched only on the graphics thread, so they need no mutex — state that explicitly in a comment):

```cpp
    // Graphics-thread-only state. Never touched by the engine reader thread,
    // so deliberately outside the mutex above.
    gs_texture_t *tex_y = nullptr;
    gs_texture_t *tex_u = nullptr;
    gs_texture_t *tex_v = nullptr;
    uint32_t      tex_w = 0;   // luma dimensions the textures were created for
    uint32_t      tex_h = 0;
    uint64_t      uploaded_generation = 0;
```

- [ ] **Step 2: Upload changed feeds in the render callback**

For each feed, take the existing snapshot (the O(1) buffer swap already in
`tile_take_snapshot`), then:

```cpp
// Reallocate only when this participant's stream size actually changed.
if (tile_texture_needs_realloc(feed->tex_w, feed->tex_h, w, h)) {
    gs_texture_destroy(feed->tex_y);
    gs_texture_destroy(feed->tex_u);
    gs_texture_destroy(feed->tex_v);
    feed->tex_y = gs_texture_create(w, h, GS_R8, 1, nullptr, GS_DYNAMIC);
    feed->tex_u = gs_texture_create(w / 2, h / 2, GS_R8, 1, nullptr, GS_DYNAMIC);
    feed->tex_v = gs_texture_create(w / 2, h / 2, GS_R8, 1, nullptr, GS_DYNAMIC);
    feed->tex_w = w;
    feed->tex_h = h;
    feed->uploaded_generation = 0;  // new textures hold nothing
}

// Skip the upload entirely when the feed has produced no new frame. An idle
// wall must not re-upload unchanged pixels every vsync.
if (tile_texture_needs_upload(feed->uploaded_generation, scratch.generation)) {
    const uint8_t *y = scratch.pixels.data();
    const uint8_t *u = y + static_cast<size_t>(w) * h;
    const uint8_t *v = u + (static_cast<size_t>(w) * h) / 4;
    gs_texture_set_image(feed->tex_y, y, w, false);
    gs_texture_set_image(feed->tex_u, u, w / 2, false);
    gs_texture_set_image(feed->tex_v, v, w / 2, false);
    feed->uploaded_generation = scratch.generation;
}
```

`TileScratch` is declared in `src/zoom-supersource.cpp` as:

```cpp
struct TileScratch {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;  // 0 means "nothing to show"
    uint64_t feed_id = 0;
    uint64_t epoch = 0;
};
```

so `w` and `h` above are `scratch.width` and `scratch.height`, and the plane
offsets are computed from those. `generation == 0` means the slot has nothing to
show, which `tile_texture_needs_upload` already returns false for.

- [ ] **Step 3: Draw each tile with its own textures and crop**

Replace the neutral-only draw loop from Task 3. For each tile index: if the feed has a current frame (the existing `TileSlotState::frame_is_current` check the compositor already performs — reuse it, do not invent a new rule), bind that feed's three textures and draw with the cover-crop sub-region; otherwise bind the shared neutral textures and draw as in Task 3.

```cpp
const CropRect crop = solve_cover_crop(static_cast<double>(feed->tex_w),
                                       static_cast<double>(feed->tex_h),
                                       kTileAspect);
gs_effect_set_texture(s_tiles_effect.param_y, feed->tex_y);
gs_effect_set_texture(s_tiles_effect.param_u, feed->tex_u);
gs_effect_set_texture(s_tiles_effect.param_v, feed->tex_v);

gs_matrix_push();
gs_matrix_translate3f(static_cast<float>(r.x), static_cast<float>(r.y), 0.0f);
gs_matrix_scale3f(static_cast<float>(r.width  / crop.width),
                  static_cast<float>(r.height / crop.height), 1.0f);
gs_draw_sprite_subregion(feed->tex_y, 0,
                         static_cast<uint32_t>(crop.x),
                         static_cast<uint32_t>(crop.y),
                         static_cast<uint32_t>(crop.width),
                         static_cast<uint32_t>(crop.height));
gs_matrix_pop();
```

The chroma planes are half-size but sampled with normalized UVs, so the same sub-region is correct for all three planes without separate maths.

- [ ] **Step 4: Destroy textures on teardown**

In `tile_feed_retire` — or wherever the feed is finally released — destroy the three textures inside `obs_enter_graphics()` / `obs_leave_graphics()`, since teardown runs off the graphics thread. Leaking a texture per repointed slot would accumulate GPU memory for the length of a show.

- [ ] **Step 5: Build, install, verify against the baseline**

Build, install, restart OBS, and bring up a wall with real participants. Compare against the Task 1 live baseline: tile positions, tile sizes, gutters, crop framing, and **skin tones**. A colour-space error typically looks like slightly washed-out or oversaturated faces rather than anything obviously broken, so compare the images side by side rather than judging from memory.

- [ ] **Step 6: Commit**

```bash
git add src/zoom-supersource.cpp
git commit -m "feat(tiles): upload participant frames as textures and draw them"
```

---

### Task 5: Remove the CPU compositor

**Files:**
- Modify: `src/zoom-supersource.cpp`

**Interfaces:**
- Consumes: the completed render path from Task 4.
- Produces: nothing — this is deletion.

Now that `video_render` draws everything, the worker thread, the canvas buffer, and the CPU blit helpers are dead. Leaving them costs a thread and a full-canvas allocation per source, and invites someone to "fix" code that no longer runs.

- [ ] **Step 1: Delete the dead code**

Remove: the worker thread and its start/stop functions, the canvas buffer and its `buf_width` / `buf_height` / `buf_tiles` bookkeeping, `blit_tile`, `fill_plane_rect`, `fill_tile_neutral`, `scale_plane` (or whatever the plane scaler is named), the `obs_source_output_video` call and the `obs_source_frame` construction, and any include that becomes unused.

Keep: `TileScratch` and `tile_take_snapshot` (Task 4 depends on them), `TileSlotState`, the grid solver, and all feed lifecycle code.

Search the file for each removed symbol before deleting it to confirm nothing else calls it.

- [ ] **Step 2: Build and run the full suite**

```
cmake --build build-rel --config Release --parallel
ctest --test-dir build-rel -C Release --output-on-failure
```
Expected: builds clean with no unused-variable or unused-function warnings, 26/26 tests pass (25 existing plus `CoreVideoTileTexture`).

- [ ] **Step 3: Install and confirm nothing changed**

Install and restart OBS. The wall must look exactly as it did at the end of Task 4. If removing the worker changed the picture, something still depended on it — find out what before proceeding.

- [ ] **Step 4: Commit**

```bash
git add src/zoom-supersource.cpp
git commit -m "refactor(tiles): remove the CPU compositor now that rendering is on the GPU"
```

---

### Task 6: Parity verification

**Files:**
- Create: `docs/design-reference/tiles-gpu-parity/after-*.png`
- Modify: `docs/design-reference/tiles-gpu-parity/README.md`

**Interfaces:**
- Consumes: everything above, plus the baseline images from Task 1.
- Produces: the evidence that Phase A is done.

**This task requires the owner and a live Zoom meeting.** If you are an agent without rig access, stop here and hand back.

- [ ] **Step 1: Capture the after images**

Using the same script and the same conditions as Task 1 — same participant count, same canvas size, same fill mode — capture `after-neutral.png` and `after-live-<n>.png`.

- [ ] **Step 2: Compare and record**

Put the before and after images side by side and check each of:
1. Tile positions and sizes identical.
2. Gutters and margins identical.
3. Short-row centering identical (use a 5-participant wall).
4. Crop framing identical — each face sits the same way in its tile.
5. Skin tones and background colours match. This is the colour-space check; a mismatch here means the shader's BT.709 full-range conversion is wrong.
6. The neutral placeholder is the same grey.

Record the result of each check in the README, with the observed outcome — not the intended one. If any check fails, Phase A is not done: fix it before closing the plan.

- [ ] **Step 3: Measure the win**

With a wall of 6+ tiles live, record OBS's reported average render time (Stats dock) for the GPU build, and note it against whatever the CPU build cost. This is the number that answers whether the GPU path was worth doing, and it is the input to any future 4K decision.

- [ ] **Step 4: Commit**

```bash
git add docs/design-reference/tiles-gpu-parity/
git commit -m "docs(tiles): record GPU compositor parity verification"
```

---

## Definition of Done

- `ctest --test-dir build-rel -C Release` passes, including `CoreVideoTileTexture`.
- The Tiles source renders through `video_render`; no `obs_source_output_video` call and no compositor thread remain in `src/zoom-supersource.cpp`.
- Before/after images are committed and every parity check in Task 6 is recorded with its observed result.
- Render time at 6+ tiles is measured and written down.

## Explicitly Not Done After This Plan

Phase B in its entirety: background colour and background source, borders, corner shape and radius, and per-slot crop. Also unchanged: the concurrent-stream cap, the Phase 0 timestamp verdict, and the subscription-count indicator.
