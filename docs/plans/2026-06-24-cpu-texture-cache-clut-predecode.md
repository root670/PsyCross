# CPU Texture Cache (CLUT Pre-Decode) Implementation Plan

> **REQUIRED SUB-SKILL:** Use the executing-plans skill to implement this plan task-by-task.

**Goal:** Eliminate the per-fragment CLUT decode that makes PsyCross ~10× more GPU-expensive than PCSX ReARMed, by pre-decoding PSX texture pages + CLUTs into cached RGBA textures on the CPU and rendering them with a trivial single-sample fragment shader.

**Architecture:** Today every textured primitive binds one giant texture = the full 1024×512 VRAM, and the fragment shader decodes CLUT via 2–4 dependent texture samples per pixel (×4 for bilinear) at screen resolution. We add a CPU-side texture cache keyed on `(tpage-coords, clut-coords, format)` → a layer of a `GL_TEXTURE_2D_ARRAY`. At vertex-build time (`MakeTexcoord*`, which already receives `poly->tpage`/`clut`), we look up the cache → stash the layer index in the vertex's unused `a_extra.zw` bytes. The fragment shader does one `texture(sampler2DArray, vec3(uv, layer))`. **The split/batching structure is unchanged** — the split still keys on `format+blend` only, so no split-explosion. Bilinear becomes free GPU hardware filtering on the pre-decoded RGBA layer. VRAM dirty-tracking (a new per-block bitmap) invalidates cache layers lazily.

**Tech Stack:** C/C++, OpenGL ES 3.0 (`sampler2DArray`, GLSL ES 3.00), deko3d/SDL2 windowing on Switch, existing `GR_*` backend seam in `PsyX_render.cpp` (0 GL calls in `PsyX_GPU.cpp`).

**Target platforms:** All PsyCross targets (Switch is the primary perf motivation, but Mac/PC benefit identically). Lives in `~/PsyCross` on the `mac-port` branch (which contains the switch-port work).

---

## Background & Evidence (read before starting)

### Why this exists
PCSX ReARMed (RetroArch) runs Silent Hill at ~7% GPU / <30% CPU on Switch. The native PsyCross port runs at ~66% GPU. The native port should beat an emulator, not lose to it.

### Root cause — confirmed by source
PsyCross uses **VRAM-as-texture** (`src/render/PsyX_render.cpp`). The fragment shader (`GPU_SAMPLE_TEXTURE_4BIT_FUNC` ~line 729, `8BIT` ~746, `16BIT` ~757) decodes CLUT per-pixel via dependent VRAM texture samples at screen resolution.

Reference implementations confirmed via local source:
- **`~/pcsx_rearmed/plugins/gpu_neon`**: pure CPU rasterizer, GPU does zero fragment work.
- **`~/pcsx_rearmed/plugins/gpu-gles`**: CLUT pre-decoded on CPU into cached RGBA GL textures keyed on `(GlobalTexturePage, ClutID)` (`CheckTextureInSubSCache`, `gpuTexture.c:3557`); fragment shader = one sample. **This is the architecture we're adopting.**

### What's already been tried & ruled out (PsyCross `mac-port` history)
| Commit | Attempt | Result |
|---|---|---|
| `f7f137f`→`8db8622` | batch consecutive same-state splits into one `glDrawArrays` | no benefit — 3D scenes have diverse split states |
| `cbc07a4`→`c657ff5` | combined 4/8/16-bit shader w/ `u_texMode` (kill `glUseProgram`) | reverted |
| `b4ee8ce`, `3e0c0d8` | cache `glViewport` + `GR_Ortho2D` | **kept** — real wins |
| `9c002cf`, `d8b11a6` | perf instrumentation + VRAM upload counters | measured, then removed for release (`5319919`) |

**Conclusion:** GL-call/state-change overhead is not the bottleneck after the kept caching. The cost is fragment throughput — CLUT-in-shader at screen resolution.

### REDRIVER2 comparison (same PsyCross base)
`~/REDRIVER2-switch/src_rebuild/PsyCross` uses the **identical** CLUT-in-shader architecture (byte-identical `samplePSX` functions). REDRIVER2 ships fine on Switch — but caps at **1280×720** (`config.ini`: `windowWidth=1280, windowHeight=720`), while our SH renders **1920×1080 docked** (`pc_port/switch/main_switch.c:267-268`) = **2.25× more pixels**.

| Renderer | Resolution | Pixels | Per-pixel cost |
|---|---|---|---|
| PCSX ReARMed (enhanced) | 640×480 | 307K | trivial (1 sample) |
| REDRIVER2 native | 1280×720 | 921K | heavy — **ships fine** |
| SH native (handheld) | 1280×720 | 921K | heavy |
| SH native (docked) | 1920×1080 | 2.07M | heavy — **2.25× REDRIVER2** |

Two orthogonal levers: resolution (REDRIVER2's choice) and per-pixel cost (this plan). **This plan keeps high-res and attacks the per-pixel multiplier.**

### deko3d considered & rejected
deko3d (`/opt/devkitpro/libnx/include/deko3d.h`) is fully available with compute/SSBO/BC1 support, but: (1) it optimizes the API/driver layer, which the evidence shows is **not** the bottleneck (high GPU% = GPU-bound, not driver-bound); (2) a backend port = reimplementing 291 GL call sites in the `GR_*` layer, Switch-only forever, high regression risk; (3) the same CLUT-in-shader architecture expressed in SPIR-V hits the same ~66% GPU. GPU-side CLUT predecode via compute would be an *enhancement* to this cache, not a replacement — deferred until the cache is proven. Decided: **stay on GLES 3.0**.

---

## Key code locations

| What | Where |
|---|---|
| Fragment shaders (CLUT decode to kill) | `src/render/PsyX_render.cpp:729-763` (`GPU_SAMPLE_TEXTURE_*BIT_FUNC`), `~797` (`GPU_FETCH_VRAM_FUNC`), `~804` (`GPU_BILINEAR_SAMPLE_FUNC`) |
| `floor(...*255)` cross-driver snap hack | `src/render/PsyX_render.cpp:786-802` (delete on cached path) |
| Shader compile / `#version` headers | `src/render/PsyX_render.cpp:993-1041` (`GR_Shader_Compile`) |
| Shader uniform/attrib setup | `src/render/PsyX_render.cpp` `GR_InitialisePSXShaders` (~1173), `GR_SetTexture` (1454) |
| Vertex struct (spare bytes `a_extra.zw`) | `include/PsyX/PsyX_render.h:120-137` (`GrVertex`, `_p0, _p1`) |
| Attrib enum | `include/PsyX/PsyX_render.h:139-147` (`ShaderAttrib`) |
| Per-poly tpage/clut → vertex | `src/gpu/PsyX_GPU.cpp:729-855` (`MakeTexcoordTriangle/Quad/Rect`) |
| Split struct + AddSplit (DO NOT add clut to split key) | `src/gpu/PsyX_GPU.cpp:496-506`, `1120-1170` |
| `DrawSplit` (binds texture, selects shader by format) | `src/gpu/PsyX_GPU.cpp:1175-1215` |
| `DrawAllSplits` (main loop) | `src/gpu/PsyX_GPU.cpp:1235-1260` |
| VRAM write sites (dirty-tracking hooks) | `GR_ClearVRAM:1614`, `GR_CopyRGBAFramebufferToVRAM:1716`, `GR_ReadFramebufferDataToVRAM:1763`, `GR_CopyVRAM:2243`, `GR_DirectUploadVRAMRegion:2328`, `GR_UpdateVRAM:2290` |
| VRAM buffer | `src/render/PsyX_render.cpp:581` (`unsigned short vram[VRAM_WIDTH*VRAM_HEIGHT]`) |
| Texpage/clut bit layout | `src/gpu/PsyX_GPU.cpp:15-18` (`GET_TPAGE_FORMAT`, etc.) |

---

## Design Decisions (resolved)

1. **Full replacement, no toggle.** The new texture-cache path replaces the VRAM-as-texture path outright — no runtime/compile toggle, no `#ifdef` branches in shipped code. Validation is golden-image based (capture reference screenshots from the current build *before* wiring, compare *after*) rather than live A/B. Git history is the safety net: every task commits, so `git revert` is granular. The old VRAM-texture shaders stay present-but-unused during Tasks 5–8 (useful as reference), then Task 9 deletes the dead code. No two-path maintenance burden ever ships.
2. **Split key stays format+blend-only.** The texture-array layer is a **per-vertex** attribute (`a_extra.z`), not split state → batching unchanged, no split-explosion (the prior failed attempt keyed the *split* on clut).
3. **Cache lives in PsyCross**, not a Switch-only shim — benefits Mac/PC too.
4. **Texture array, not atlas.** GLES 3 `GL_TEXTURE_2D_ARRAY` gives free per-vertex layer selection + hardware wrap modes; avoids atlas-bleed seams.
5. **Layer index in `a_extra.zw`** (the 2 explicitly-unused bytes; shader comment confirms `// unused.xy`).

---

## Task 0: Profile to confirm the bottleneck is fragment cost (EMPIRICAL GATE)

**Do not build the cache until this task proves the architecture choice empirically.** Two tests that need no timer queries:

### Task 0a: Resolution-scaling test (is it fragment-bound?)

**Files:**
- Modify: `pc_port/switch/main_switch.c:265-271` (temporarily force 720p even docked — reverted after measuring)
- Modify: `pc_port/src/main_pc.c` (temporary console `res 1280 720` — reverted after measuring)

**Step 1:** Temporarily force internal render resolution to 1280×720 even when docked (today docked = 1920×1080). This is throwaway instrumentation — reverted after measuring; it never ships.

**Step 2:** Build & deploy to Switch:
```
cd pc_port && ./build_switch.sh && nxlink -s SilentHill.nro
```

**Step 3:** Play the same docked scene at forced 720p vs default 1080p. Read GPU% from the existing PERF overlay (or RetroArch-style overlay).

**Step 4 — record results:**
- If 720p GPU% ≈ 1080p_GPU% / 2.25 → **fragment-bound, Option B confirmed.**
- If 720p GPU% barely drops → bottleneck is elsewhere (vertex/state/overdraw); STOP and re-evaluate before Task 1.

### Task 0b: Trivial-shader override test (is it the CLUT cost specifically?)

**Files:**
- Modify: `src/render/PsyX_render.cpp` — add a `g_dbg_texturelessMode`-style flag (`g_dbg_trivialShader`) that swaps the 4/8-bit CLUT shader for a single-VRAM-sample shader (skip the CLUT second fetch — sample VRAM directly as if 16-bit).

**Step 1:** Temporarily swap the 4/8-bit CLUT shader for a stripped `samplePSX` that does only the first `VRAM(uv)` fetch (no CLUT lookup). Throwaway instrumentation — reverted after measuring. It isolates CLUT cost from the base VRAM-fetch cost.

**Step 2:** Build, deploy, measure GPU% in the same gameplay scene with the trivial shader (textures will look wrong — that's fine, we're measuring cost not correctness).

**Step 3 — record results:**
- If trivial-shader GPU% drops dramatically (e.g., 66% → <30%) → **CLUT decode is the dominant cost, Option B confirmed.**
- The expected savings from the full cache ≈ this measured delta.

**Step 4 — capture the baseline (reused by Task 8):** Before discarding the instrumentation, record the docked-1080p and handheld-720p GPU% + frame time on the **unmodified** VRAM-texture path. This is the number Task 8 compares the cache against.

**Step 5:** Discard the throwaway instrumentation — it does not ship:
```bash
git checkout .   # discard the temp profiling edits; keep only the recorded numbers
```

**Gate:** Both 0a and 0b must confirm fragment/CLUT is the bottleneck before proceeding. If they don't, stop and report — the plan's premise is wrong.

---

## Task 1: VRAM dirty-tracking infrastructure

**Files:**
- Create: `src/render/PsyX_vram_dirty.h` (bitmap + helpers)
- Create: `src/render/PsyX_vram_dirty.c`
- Modify: `src/render/PsyX_render.cpp` — hook all VRAM write sites

**Design:** A dirty bitmap over VRAM in 16×16 texel blocks. VRAM is 1024×512 → 64×32 = 2048 blocks → 2048 bits = 256 bytes. CLUT regions (always 256×1 or 16×1 at specific coords) and texture-page regions mark the same shared bitmap; a cache layer is dirty if *either* its source page region *or* its CLUT region intersects a dirty block.

**Step 1: Write the dirty bitmap header**
```c
// src/render/PsyX_vram_dirty.h
#ifndef PSYX_VRAM_DIRTY_H
#define PSYX_VRAM_DIRTY_H
#include "PsyX/PsyX_render.h"

#define VRAM_BLOCK_SIZE 16
#define VRAM_BLOCKS_X   (VRAM_WIDTH  / VRAM_BLOCK_SIZE)  // 64
#define VRAM_BLOCKS_Y   (VRAM_HEIGHT / VRAM_BLOCK_SIZE)  // 32
#define VRAM_DIRTY_WORDS ((VRAM_BLOCKS_X * VRAM_BLOCKS_Y + 31) / 32)

extern unsigned int g_vramDirty[VRAM_DIRTY_WORDS];
extern int g_vramDirtyAllValid;  /* set by any clean→dirty transition */

void VRAM_DirtyInit(void);
void VRAM_DirtyRegion(int x, int y, int w, int h);   /* mark blocks covering (x,y,w,h) */
void VRAM_DirtyAll(void);
int  VRAM_IsRegionDirty(int x, int y, int w, int h); /* any dirty block in region? */
void VRAM_DirtyClearRegion(int x, int y, int w, int h);
#endif
```

**Step 2: Implement the bitmap** (`PsyX_vram_dirty.c` — block-clamping, bit set/test, the standard 1-bit-per-block logic).

**Step 3: Hook all VRAM write sites** in `PsyX_render.cpp`. Each function that writes `vram[]` calls `VRAM_DirtyRegion(x,y,w,h)`:
- `GR_ClearVRAM` (1614)
- `GR_CopyRGBAFramebufferToVRAM` (1716)
- `GR_ReadFramebufferDataToVRAM` (1763)
- `GR_CopyVRAM` (2243)
- `GR_DirectUploadVRAMRegion` (2328)

Also hook `GR_UpdateVRAM` (2290) → `VRAM_DirtyAll()` defensively on full re-upload (the double-buffer swap can replace any texel).

**Step 4:** Call `VRAM_DirtyInit()` in `GR_InitialiseRender` and `VRAM_DirtyAll()` at init.

**Step 5:** Build clean:
```
cd /Users/wesleycastro/PsyCross && grep -c "VRAM_DirtyRegion" src/render/PsyX_render.cpp  # expect ≥6
```
Verify the Switch SH build compiles:
```
cd /Users/wesleycastro/silent-hill-decomp/pc_port && ./build_switch.sh
```

**Step 6:** Commit:
```bash
git add -A && git commit -m "psycross: add VRAM dirty-tracking bitmap + hook all write sites (Task 1)"
```

---

## Task 2: Texture cache data structure + lookup

**Files:**
- Create: `src/render/PsyX_texcache.h`
- Create: `src/render/PsyX_texcache.cpp`
- Modify: `CMakeLists.txt` (add new sources)

**Design:** Cache keyed on `(pageX, pageY, clutX, clutY, format)` (all derivable from the PSX `tpage`/`clut` words — page coords = bits 0-4 of tpage × 64/256, clut coords = bits of clut word). Each entry: `{ GL_TEXTURE_2D_ARRAY layer (or its own GL texture), source-rect, last-used-frame, dirty }`. LRU eviction with a fixed cap (e.g., 512 layers) to bound memory.

**Important — the lookup happens at `MakeTexcoord*` time (per-poly), NOT at `AddSplit`/split time.** This is what preserves batching.

**Step 1: Define the entry + table**
```cpp
// src/render/PsyX_texcache.h
enum { TEXCACHE_MAX_LAYERS = 512 };
typedef struct {
    short pageX, pageY;     /* source texture-page top-left in VRAM (texels) */
    short clutX, clutY;     /* CLUT top-left in VRAM */
    short format;           /* TF_4_BIT / TF_8_BIT / TF_16_BIT */
    int   layer;            /* GL_TEXTURE_2D_ARRAY layer index, -1 if empty */
    int   lastUsedFrame;
    int   valid;            /* entry occupied */
} TexCacheEntry;

int  TexCache_GetLayer(short pageX, short pageY, short clutX, short clutY, short format);
/* Returns a ready-to-use RGBA layer, decoding lazily if dirty/absent.
 * Called from MakeTexcoord* in PsyX_GPU.cpp. */

void TexCache_Init(void);
void TexCache_Shutdown(void);
GLuint TexCache_GetArrayTexture(void);   /* the GL_TEXTURE_2D_ARRAY to bind */
void TexCache_FrameReset(int frameNum);
```

**Step 2: Implement lookup + LRU eviction + the decode call** (decode is Task 3; stub it to fill the layer with solid magenta for now).

**Step 3:** Add the new sources to `CMakeLists.txt` PsyCross source list.

**Step 4:** Build clean.

**Step 5:** Commit:
```bash
git add -A && git commit -m "psycross: add texture cache structure + layer lookup (Task 2)"
```

---

## Task 3: CPU CLUT pre-decode (4/8/16-bit → RGBA8)

**Files:**
- Modify: `src/render/PsyX_texcache.cpp` (fill in the decode stub)

**Design:** Decode a 256×256 region into a temp RGBA8 buffer using integer math (no floating point — this is *more* bit-exact than the current shader `floor(*255+0.5)` snap hack). PSX 16-bit pixel format = 15-bit BGR (bit 15 = semi-transparency flag, ignored for colour).

**Step 1: Implement `TexCache_DecodeLayer(entry, rgba8Out)`**
- **4-bit:** each VRAM halfword holds 4 palette indices (4 px per halfword). `index = nibble`; `color = vram[clutBase + index]`; convert 5551→8888.
- **8-bit:** each VRAM halfword holds 2 indices. `index = byte`; `color = vram[clutBase + index]`.
- **16-bit:** direct `vram[pageBase + ...]` → 8888.
- 5551→8888 conversion: `r=(c&0x1F)<<3; g=((c>>5)&0x1F)<<3; b=((c>>10)&0x1F)<<3;` (apply the standard `| (c>>2)` bit-replication for 5→8 accuracy, matching DuckStation).

**Step 2:** Upload decoded buffer to the texture-array layer:
```cpp
glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 256, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
```

**Step 3:** Respect VRAM dirty tracking — only decode if `VRAM_IsRegionDirty(pageX,pageY,256,256) || VRAM_IsRegionDirty(clutX,clutY,16/256,1)`. Mark the entry clean after decode.

**Step 4 — validation:** Add a debug mode that dumps one decoded layer to a PNG; eyeball it against the same region rendered by the current shader (use a PSX VRAM viewer / the existing `GR_SaveVRAM`). Colours must match exactly.

**Step 5:** Build clean.

**Step 6:** Commit:
```bash
git add -A && git commit -m "psycross: CPU CLUT pre-decode (4/8/16-bit) into RGBA8 array layers (Task 3)"
```

---

## Task 4: Per-vertex layer routing in MakeTexcoord*

**Files:**
- Modify: `src/gpu/PsyX_GPU.cpp:729-855` (`MakeTexcoordTriangle/Quad/Rect`)

**Design:** Each `MakeTexcoord*` already receives `page` and `clut`. Add the cache lookup here and stash the resulting layer index in the vertex's `a_extra.zw` (the `_p0,_p1` bytes, currently unused). All vertices of a poly share one layer (one tpage+clut), so per-poly it's constant.

**Step 1:** In each `MakeTexcoord*`, after the existing `vertex[i].page/clut` assignments, add:
```cpp
    short pageX = (page & 0x1F) * 64;   /* derive VRAM coords from tpage bits */
    short pageY = ((page >> 4) & 0x1) * 256;
    short clutX = (clut & 0x3F) << 4;
    short clutY = (clut >> 6) & 0x1FF;
    int layer = TexCache_GetLayer(pageX, pageY, clutX, clutY, GET_TPAGE_FORMAT(page));
    vertex[i].tcx = 0;       /* keep a_extra.xy (texcoord ofs) untouched */
    vertex[i].tcy = 0;
    vertex[i]._p0 = (char)(layer & 0xFF);    /* layer index low byte */
    vertex[i]._p1 = (char)((layer >> 8) & 0xFF);
```
(Exact page-coord derivation must be double-checked against PSX texpage bit layout — verify with a VRAM viewer before trusting; the `& 0x1F`/`*64`/etc. is the standard layout but confirm against `GET_TPAGE_FORMAT` at `PsyX_GPU.cpp:15`.)

**Step 2:** Ensure the `*Zero` variants (`MakeTexcoordTriangleZero` etc. for untextured prims) set `_p0=_p1=0` so untextured verts don't read garbage layers.

**Step 3:** Build clean. (No visual change yet — shader still ignores the layer.)

**Step 4:** Commit:
```bash
git add -A && git commit -m "psycross: route texture-cache layer via per-vertex a_extra.zw (Task 4)"
```

---

## Task 5: New trivial fragment shader (single texture-array sample)

**Files:**
- Modify: `src/render/PsyX_render.cpp` — add `GPU_FRAGMENT_ARRAY_SHADER`, compile it, wire into the shader table.

**Design:** A new shader that reads the layer from `a_extra.z` (re-pack the two bytes to int in the vertex shader, or pass as `mediump int`). Fragment sample = one `texture(s_array, vec3(uv, layer))`. No `VRAM()`, no `samplePSX()`, no `packRG`/`decodeRG`, no CLUT math. Bilinear = free `GL_LINEAR` on the sampler.

**Step 1: Write the new vertex-shader snippet** that extracts the layer:
```glsl
v_layer = float(a_extra.z) + float(a_extra.w) * 255.0;  /* or use ivec unpacking */
```
and the fragment snippet:
```glsl
uniform sampler2DArray s_texArray;
vec4 NearestTextureSample(vec2 P) {
    vec4 c = texture(s_texArray, vec3(P * c_VRAMTexel_page, v_layer));  /* UV in page space */
    if (c.a == 0.0) discard;   /* match existing fully-transparent discard */
    return c;
}
/* bilinear: just set sampler GL_LINEAR — hardware does it */
```
(Exact UV scaling: today UVs are in PSX texel space within the page; the array layer is a 256×256 page, so `uv_page = vec2(u, v) / 256.0`. Verify against the existing `samplePSX` UV math at line 729.)

**Step 2:** Compile it via `GR_Shader_Compile` (reuse the `ES3_SHADERS` `#version 300 es` path — `sampler2DArray` is core in 3.00).

**Step 3:** Add a `g_gte_shader_array` shader to the table alongside `g_gte_shader_4/8/16`.

**Step 4:** Build clean. (Shader exists but isn't selected yet.)

**Step 5:** Commit:
```bash
git add -A && git commit -m "psycross: add texture-array fragment shader (single-sample) (Task 5)"
```

---

## Task 6: DrawSplit integration (cache becomes the renderer)

**Files:**
- Modify: `src/gpu/PsyX_GPU.cpp` (`DrawSplit:1197`, select new shader + bind array)
- Modify: `src/render/PsyX_render.cpp` (`GR_SetTexture`, bind array texture)

**Step 0 — capture golden reference BEFORE wiring:** From the current unmodified build, screenshot a fixed set of reference scenes (title, café/alley map0_s01, one indoor scene, one cutscene, paper-map pickup). Save these as the Task-7 bit-exactness baseline. No live toggle exists to compare against later, so this capture *is* the comparison source.

**Step 1:** In `DrawSplit`, for textured splits that are not the 32-bit-RGBA override path: select `g_gte_shader_array` instead of the format-specific shader, and bind `TexCache_GetArrayTexture()` instead of `g_vramTexture`. Set `texFormat` to a new `TF_ARRAY` so the rest of the state setup is consistent. **This is unconditional — the cache is now the renderer.** The old 4/8/16 CLUT shaders + VRAM-texture path become dead code (present-but-unused as reference) until Task 9 deletes them.

**Step 2:** In `GR_SetTexture`, handle the array texture bind (it's a single bind per frame, since all layers live in one array).

**Step 3:** Build, deploy.

**Step 4 — first visual check:** The screen should render with textures. Expect wrong colours / offsets initially — compare against the Step-0 golden images (Task 7 is the systematic fix-up).

**Step 5:** Commit:
```bash
git add -A && git commit -m "psycross: wire texture cache into DrawSplit as the renderer path (Task 6)"
```

---

## Task 7: Bit-exactness validation & fix-up

**Files:** As needed across `PsyX_GPU.cpp`, `PsyX_texcache.cpp`, `PsyX_render.cpp`.

**Step 1:** Screenshot the same scenes captured as golden images in Task 6 Step 0. Compare pixel-by-pixel against those references (use ImageMagick `compare` or a diff tool). Document every discrepancy. If a reference needs re-capture, check out the pre-Task-6 build (`git stash` / `git show` / a worktree) — git history is the fallback, no live toggle needed.

**Step 2:** Common discrepancies & fixes:
- **UV off-by-one / flipped V:** PSX V increases downward; GL Y is bottom-up. The current shader handles this in the page-coord math (`v_page_clut`); mirror it for the array layer. Verify `clutY`/`pageY` derivation.
- **Colour banding:** the 5→8 bit expansion must match (bit-replication `| (c>>2)`, not `<<3`).
- **Fully-transparent discard:** keep the `discard` on alpha==0 to match the existing dithered-edges behaviour.
- **Semi-transparency flag (bit 15):** must be stripped when reading 5551 colour; it's blend state, not colour.
- **CLUT width:** 4-bit uses 16-entry CLUT, 8-bit uses 256-entry. Confirm `clutX` stride.

**Step 3:** Iterate until the two screenshots are visually indistinguishable (allow for 1-bit rounding in the 5→8 path, which is expected and present in both).

**Step 4:** Commit:
```bash
git add -A && git commit -m "psycross: texture cache bit-exactness fixes (Task 7)"
```

---

## Task 8: Performance measurement

**Step 1:** On the cache-enabled build (now the only path), measure docked-1080p and handheld-720p GPU% + frame time in the same scenes. Compare against the **unmodified-VRAM-texture baseline recorded in Task 0 Step 4**.

**Step 2 — record:**
- Expected: GPU% drops substantially (toward the trivial-shader delta measured in Task 0b).
- Bilinear mode (`g_cfg_bilinearFiltering`) should show an *additional* large win (4× → 1×).

**Step 3:** If GPU% doesn't drop as expected, the cache isn't being hit (layers re-decoding every frame = dirty tracking too aggressive, or page-coord derivation wrong so every lookup misses). Check `TexCache_GetLayer` hit rate (add a debug counter).

**Step 4:** Commit measurements note to this plan's file as an addendum, then:
```bash
git add -A && git commit -m "psycross: texture cache perf validation (Task 8)"
```

---

## Task 9: Delete dead code + full regression

**Files:**
- Modify: `src/render/PsyX_render.cpp` — delete the now-dead VRAM-texture shaders (`GPU_SAMPLE_TEXTURE_*BIT_FUNC`, `GPU_FETCH_VRAM_FUNC`, the old `GPU_FRAGMENT_SAMPLE_SHADER` path) and the `floor(*255+0.5)` cross-driver snap hack at `786-802`; drop the `g_gte_shader_4/8/16` compile/table entries if no longer referenced
- Modify: `src/gpu/PsyX_GPU.cpp` — delete the `g_vramTexture` bind path in `DrawSplit` if now unreachable

**Step 1:** The cache has been the only render path since Task 6. Delete the dead VRAM-texture fragment-shader code, the `floor(*255+0.5)` snap hack, and any now-unreferenced shader table entries. A clean compile confirms nothing still references the deleted path.

**Step 2:** Re-validate bit-exactness against the Task-6 golden images on this cleaned build.

**Step 3:** Full regression: boot, title, opening FMV, café/alley (map0_s01), an indoor scene, a cutscene, the paper-map pickup screen (the `GR_DirectUploadVRAMRegion` path — ensure its dirty hook fires correctly).

**Step 4:** Commit:
```bash
git add -A && git commit -m "psycross: delete dead VRAM-texture shaders + snap hack; cache is sole path (Task 9)"
```

---

## Follow-up (not in this plan)

- **GPU-side CLUT decode via deko3d compute** (Switch-only enhancement) if profiling shows CPU decode is ever a hotspot — unlikely since VRAM is already in CPU memory.
- **PGXP texture-wobble correction** through the array path (currently handled in-shader; verify it still applies to the array sampler).

---

## Risks

- **Dirty tracking too coarse → cache thrash.** 16×16 blocks may over-invalidate (a CLUT write invalidates a whole block that also covers a texture page). If hit rate is low in Task 8, shrink CLUT dirty granularity to per-texel.
- **Page-coord derivation wrong → garbage.** Task 4's bit math must be verified against a VRAM viewer before trusting; this is the most likely source of Task-7 bugs.
- **Texture-array memory.** 512 layers × 256×256×4 = 128 MB — too much. Cap lower (128 layers = 32 MB) or use 128×128 layers for 4-bit pages. Resolve in Task 2 sizing.
- **GLES 3.0 array support on Tegra X1** is core-spec, should be fine, but verify `GL_MAX_ARRAY_TEXTURE_LAYERS` at init in Task 5.
