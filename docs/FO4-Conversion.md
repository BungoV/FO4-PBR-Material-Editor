# Fallout 4 / Fallout 76 → PBR asset conversion

Status: spec, agreed 2026-07-22. The converter is an editor-integrated module:
the editor already owns BA2 reading, BC1–7 decode, game-ready DDS encode with
mip discipline and Toksvig, the channel compositor, batch-operation framing,
validation, and the live preview that makes manual review practical.

## Source profiles

Conversion is organized as per-title **source profiles** that own parsing,
texture-suffix conventions, channel semantics, and inference math; everything
downstream of them — the derivations, geometry AO, masks, workspace, and
`.pbrm` output — is shared:

- **Fallout 4** (BGSM/BGEM version 2): the full pipeline below, including
  metal albedo recovery and envmap-based metallic inference — FO4's dark
  metal + cubemap authoring demands it.
- **Fallout 76** (BGSM/BGEM version 20+): a **true specular workflow** —
  the diffuse is strictly the Lambertian term, so **metals are authored
  with black diffuse** and their color (gold's yellow, steel's grey) lives
  in the RGB `_r` reflectance map. Conversion is the textbook
  specular→metalness reconstruction:
  `metallic` inferred from reflectance intensity joined with diffuse
  near-blackness (a very reliable pair);
  `baseColor = mix(diffuse, reflectance.rgb, metallic)` — the diffuse fix,
  mirror image of FO4's recovery (FO4 keeps hue and loses brightness to
  the cubemap; FO76 moves hue *and* brightness into `_r`);
  dielectric F0 from reflectance clamped to the physical window;
  smoothness inverts to roughness through a profile curve. Dielectric
  albedo passes through untouched. Packed-map channel semantics are
  confirmed against the Material Editor reference source
  (`E:\Projects\ClaudeMaterialEditor`, ousnius fork parses both versions)
  and real extracted assets before being pinned in tests — not assumed
  from community folklore.

The BGSM version field routes each scanned material to its profile
automatically; a mixed FO4/FO76 source tree needs no user setting. The BA2
reader gains FO76 archive-version tolerance (v1 plus the later v7/v8
header variants).

## Contract

- **Input**: Fallout 4 `.bgsm` / `.bgem` materials (loose or BA2) and the
  vanilla texture sets they reference (`_d`, `_n`, `_s`, `_g`, env masks).
- **Output**: a **same-name sibling `.pbrm`** at the same relative
  `Materials\` path inside a chosen output mod folder, plus converted PBR
  textures at mirrored `Textures\` paths. The `.bgsm`/`.bgem` files are
  **never modified** — they remain the vanilla fallback, and the FO4CS
  runtime probes the sibling `.pbrm` by path through the game VFS.
- BGSM/BGEM support is a **read-only importer**. The editor remains
  `.pbrm`-only as an editor; it never writes or edits Bethesda materials.

## Texture pipeline

### Base Color ← `_d`

Pass-through for dielectrics (sRGB, alpha preserved for opacity), with
**metal albedo recovery**: vanilla metals are authored dark because the
engine composites the cubemap on top (gated by environment mapping, the env
mask, and spec strength). PBR metal base color *is* the reflectance color,
so recovered texels take **hue from the diffuse, brightness from spec**:

```
m           = metallic confidence (below)
cubemapTint = mean color of the material's referenced _e cubemap
reliability = saturate(diffuseLuma / threshold)
hueSource   = mix(cubemapTint, diffuseHue, reliability)
metalAlbedo = normalize(hueSource) * lerp(F0min, F0max, specR)
baseColor   = mix(diffuse, metalAlbedo, m * recoveryStrength)
```

The cubemap term exists because some vanilla metals are authored **fully
black** — the chosen cubemap *is* their entire appearance (gold trim =
black diffuse + warm cubemap), so the cubemap choice is the color
authoring. The mean is computed from the actual referenced cubemap
(luminance-weighted, mid mip, cached per cubemap, loaded through the
existing cubemap-face loader) with a **saturation clamp** so scene-neutral
cubemaps (`DefaultOutside1`'s sky blue) cannot tint metals — deliberately
tinted cubemaps pass through. The review panel shows the derived metal
color as an overridable swatch per material.

`recoveryStrength`, `F0min`, `F0max`, the reliability threshold, and the
tint saturation clamp are conversion options (defaults tuned on the X01
set). Recovery runs in linear space; output BC7 sRGB (BC1 when alpha is
unused and composition is opaque).

### Normal + derived physics ← `_n`

**Derivation-side denoising (auto-detected, case-by-case).** The shipped
normal map is always the **original** — vanilla normals stay normals. But
the derivations cannot ingest grain: the Laplacian (curvature) amplifies
exactly the frequencies compression garbage and dither live in, and the
horizon scan picks them up as false micro-cavities. So a **cleaned internal
copy** feeds the Poisson/AO/curvature pipeline only: a per-texture noise
metric (1–2 px XY variance vs. structural scales, plus a 4×4-block-aligned
fingerprint for BC artifacts) gates an optional median pre-pass,
bilateral/guided filtering on the XY vector field (range sigma = edge
threshold, so real creases survive), and a BC deblocking pass that smooths
block boundaries only where the jump reads as artifact. Toksvig continues
to measure the **original** normal — that is what ships and what minifies.
Dry-run reports noise scores; batch cleans only above threshold;
per-material strength/threshold override in review, 0 = derive from the
raw field.

One reconstruction feeds three channels:

1. **Height field** — Poisson-integrate the normal's XY gradient field (FFT
   with mirrored borders; exact for tiling textures), high-pass filtered to
   remove low-frequency drift.
2. **AO** ← horizon-scan the height field (HBAO-style, 8–16 directions,
   radius/intensity options) → RMAOS.B. This is meso-scale detail AO by
   construction — seams, panel lines, crevices the low-poly mesh cannot
   express. When a reference NIF is associated (see the geometry AO bake
   below) the two scales multiply: `AO = geometryAO^a × detailAO^b`, with
   independent intensity exponents; without a NIF, detail AO ships alone.
3. **Signed curvature** ← Laplacian of the height field, 0.5-neutral
   encoding → Normal.A (`curvatureInAlpha=true`), lighting up edge shedding,
   cavity collection, and cavity specular occlusion on every converted asset.
4. **Height** ← the filtered field itself → Normal.B
   (`heightInBlue=true`, optional — off by default for flat trim sheets).

Output BC7 (RG normal + B height + A curvature). Porosity needs no map:
the runtime heuristic `saturate(roughness × (1 − metallic))` covers
converted assets automatically.

### Geometry AO bake ← reference NIF (optional, recommended)

Form-scale occlusion the normal map cannot know — between armor plates,
under collars, inside cuffs — bakes from the associated reference NIF on
the GPU, no ray tracer required:

1. Rasterize the material's UV layout once, interpolating per-texel world
   position and normal (the same `position = UV` pass the mask painter
   uses).
2. Accumulate visibility over ~128–256 hemisphere directions: per
   direction, render the **entire NIF's** depth from an orthographic camera
   (the existing shadow-map machinery) and shadow-test every texel's world
   position, weighted by `max(0, N·d)`.
3. Dilate across UV seams, then compose with detail AO as above.

Details: only the converted material's submeshes *receive*, but the whole
NIF *occludes* (the torso shadows the arm material). Skinned meshes bake in
bind pose. The material → NIF association is chosen in the review flow (the
same reference-NIF step as masking) and recorded in the manifest so batch
re-runs rebake without re-asking; a future convenience can auto-match by
scanning NIF shader-property material paths. Geometry AO defaults
conservative — runtime SSAO/shadows overlap part of its range.

**Duplicate UV islands (canonical-instance baking).** Vanilla meshes
deliberately stack duplicate islands inside 0–1 for texture reuse (both
boots, repeated pouches/trim) — never offset outside the unit square the
way bake-ready meshes are — so each texel receives several world-space
points with different occlusion. The baker resolves this the way artists do
manually, automatically:

1. Segment UV islands by UV-space triangle connectivity.
2. Detect duplicate groups by rasterizing island footprints into an
   ID/count buffer; islands whose texel sets intersect beyond a ratio
   threshold form a group (slivers of incidental overlap stay independent).
3. Bake **one canonical instance per group**: only its texels receive;
   every instance still occludes. Duplicates inherit the canonical texels
   by construction.
4. Canonical selection policy per material: **Brightest** (least occluded —
   default, since a too-flat result reads neutral while one tucked-away
   instance's shadows stamped on every copy read wrong), *Average*,
   *Darkest*, or **Pick manually** — overlap groups highlight in the
   viewport and the clicked instance wins, riding the element-picking
   machinery the fill tools already use.

**One texture shared by different models/layouts.** Geometry AO in a shared
texture is only exactly right for one mesh, so the bake always targets the
chosen canonical reference NIF; when the NIF scan finds multiple models
referencing the material with materially different UV layouts, the dry-run
flags it so the user picks the most representative model or reduces/skips
geometry AO for that material. Detail AO is layout-independent and always
safe.

Shelf note: the same bake with rays flipped inward is a **thickness bake**
for SSS alpha; the infrastructure is shared when that becomes worth
exposing.

### RMAOS ← `_s` (+ env mask + BGSM parameters)

FO4 `_s`: R = specular strength, G = glossiness.

- **Roughness** = `1 − gloss`, remapped through an adjustable curve
  (vanilla gloss is exponent-flavored; default curve tuned visually), then
  clamped to the preview floor (0.035).
- **Metallic confidence** `m` from the standard spec-gloss → metal-rough
  inference: env mapping enabled × env-mask scale × spec R × diffuse
  darkness, smoothstepped with adjustable thresholds. Written to G.
- **AO** from the derivation pass → B.
- **A carries Dielectric F0** by default: dielectric texels get
  `F0 = clamp(specR × specMult × 0.08, 0.02, 0.08)` (the vanilla spec axis
  compressed into the physical dielectric window); metal texels' F0 comes
  from base color, so A can stay at 0.04 there.
- **Spec clamping**: batch mode applies percentile normalization per texture
  (blown-out vanilla spec maps otherwise convert to mirror chrome) and the
  dielectric window clamp above. The single-material import dialog exposes
  the clamps as live sliders because some materials need manual judgment.

Output BC7 linear, full mip chain, **Toksvig folded from the converted
normal automatically** — the converter always has the paired normal in hand.

### Emissive ← `_g` + BGSM emittance color/multiplier

The FO4 `_g` glow map is a **grayscale mask only** — the emissive color
lives in the `.bgsm` (emittance color + multiplier). Conversion bakes
`_E.dds` with RGB = mask × emittance color (sRGB) and A = the raw mask;
the slot's intensity carries the BGSM multiplier, so authored glow colors
survive instead of converting to white. `bEmitEnabled` without a glow map
emits uniformly: no texture, constant color + intensity on the slot.
Skipped when the material does not emit.

BGEM emissives are different: there is **no glow map** — the diffuse is
the emission source and the `.bgem`'s scale is the intensity. Conversion
reuses the base texture's path in the Emissive slot (no duplicate file),
color = BGEM tint, intensity = the scale; for materials landing in the
Unlit Effect family the scale maps to `Shading/Unlit` intensity instead,
since unlit base color already is the radiance.

## Material parameter mapping

Parameter conversion is a **complete field-disposition audit**: every
BGSM/BGEM field (both versions) is assigned exactly one fate, the
assignment is pinned in tests, and the per-material manifest records any
field that could not be honored — nothing drops silently:

1. **Mapped** — a direct `.pbrm` counterpart exists. Notable beyond the
   table below: `bModelSpaceNormals` → `Shading/Core` normal space;
   `bRefraction` + `fRefractionPower` → Screen Distortion;
   `bZBufferWrite`/`bZBufferTest` → depth settings; `bDecal`/`bDecalNoFade`
   → Decals; `bHair` + hair tint → Hair family; skin/facegen flags →
   Advanced Subsurface.
2. **Inference input** — consumed by conversion math, never serialized:
   environment mapping + mask scale, specular color/mult/smoothness,
   rim/backlight powers (also informing subsurface approximations).
3. **Superseded by runtime** — the `fWetnessControl_*` block is vanilla's
   wetness system; PBRM v5 runtime surface states replace it wholesale, so
   it is deliberately not converted.
4. **Flagged for review** — grayscale-to-palette, external emittance, and
   anything unrepresentable.

Per-material custom cubemaps (`_e`) are an **inference input**: their mean
color feeds metal albedo recovery (black-diffuse metals are colored
entirely by their chosen cubemap). As textures they still convert to
nothing — the PBR runtime owns environment lighting.

**Root material chains**: `sRootMaterialPath` template inheritance is
resolved before any conversion math (child overrides over the resolved
root, with missing-root and cycle handling reported as errors) — child
materials are frequently sparse and convert half-empty otherwise.

### BGSM → `.pbrm` (Standard family)

| BGSM | `.pbrm` |
| --- | --- |
| diffuse/normal/spec/glow paths | converted texture paths (portable, texture-root relative) |
| alpha blend / test / threshold | `Transparency/Composition` mode + threshold |
| two-sided | `Shading/Core.twoSided` |
| emissive color / mult | Emissive slot color / intensity |
| environment mapping + mask scale | metallic inference input (not serialized — PBR has no envmap toggle) |
| specular color / mult / smoothness | RMAOS conversion inputs + dielectric F0 |
| subsurface / skin tint flags | `Advanced Subsurface` family + profile defaults |
| grayscale-to-palette, tile U/V, UV offset/scale | UV transform settings; palette cases flagged for manual review |

### BGEM → `.pbrm` (effect families)

Blend/additive flags select `Unlit Effect` vs `Simple Lit Effect`; base
texture, tint, opacity, soft-fade and falloff parameters map to the effect
inputs. Emission follows the BGEM rule above (diffuse × scale, no glow
map). Anything unrepresentable is flagged in the report rather than
silently dropped.

## UI: the Convert workspace

Conversion is a separate **workspace**, not a dialog: a top-bar switcher
(**Author** | **Convert**, NifSkope-flat) swaps the central widget stack in
the same window; menus enable contextually. `File → Import Fallout 4
Material…` remains as an entry point that switches to Convert with that
material selected.

Layout (same visual language as Author):

- **Left** — sources (loose folders / BA2s) and the scanned
  `.bgsm`/`.bgem` list as a work queue with per-material status
  (*pending / flagged / masked / converted*), filterable to walk just the
  flagged set.
- **Center** — conversion options (recovery strength, gloss curve, clamps,
  AO radius/intensity, height on/off; output mod folder), the selected
  material's dry-run detail (textures found/missing, inferred metal
  coverage, clamp outliers), and its post-conversion validation results.
- **Right** — the shared preview viewport showing the *converted* result
  live; hosts the reference NIF and the mask paint/fill tools.
- **Footer** — Dry Run, Convert Selected, Convert All, and **Open in
  Author**: the bridge into the normal authoring workspace for hand-polish
  beyond what conversion options express.

Intended flow: scan once → batch-convert the clean majority → step through
the flagged queue (tune sliders, paint corrections, convert, next).
Materials whose spec histogram trips the outlier heuristic arrive
pre-flagged.
- **3D correction masking**: the metallic inference has false positives
  (bright spec on non-metals — the vault suit's boots). In the single-import
  flow, load the NIF that wears the material (the existing custom-mesh
  preview path) and paint corrections directly on the mesh:
  - Projection painting renders the mesh with `position = UV` while the
    fragment shader accumulates brush influence by world-space distance
    from the brush ray into a UV-space mask FBO — seams and split charts
    handled for free.
  - The mask is a correction layer, 0.5 neutral: below forces dielectric,
    above forces metal, strength = distance from neutral —
    `m' = mix(inferred, painted, |mask − 0.5| × 2)`. Albedo recovery
    follows the corrected metallic, so a dielectric correction also stops
    the brightening.
  - Substance-Painter-style fill modes beside the brush, sharing the same
    mask and fill values (*Force dielectric* / *Force metal* / *Clear to
    neutral*):
    - **Triangle fill** — click or drag over individual triangles. Picking
      uses a GPU ID pass (triangle IDs rendered offscreen, pixel read under
      the cursor); filled triangles rasterize into the mask through the
      same `position = UV` pass with a subset index buffer.
    - **Element fill** — click any connected shell (buckle, strap, sole)
      to fill the whole piece. Connectivity is built by position-welding
      vertices first (NIF geometry splits at UV/normal seams; union-find
      over welded triangles restores physical-piece semantics).
    - **Submesh fill** — the coarsest level: *Force dielectric* /
      *Force metal* on the selected submesh.
    Hover highlights the triangle/element about to fill; brush
    radius/softness/erase plus an inference-vs-correction overlay tint
    cover the freehand cases.
  - Masks persist as sidecar images recorded in the conversion manifest;
    batch re-runs reuse them, and the dry-run report lists which materials
    carry corrections. R = metallic correction; G/B/A reserved (painted
    spec clamping is the intended next tenant).
- Converted materials run normal validation; the report includes its
  errors/warnings per file.

## Output naming, dedup, and the manifest

Converted textures use the established suffix convention beside the vanilla
names, in the output mod folder:
`Textures\...\<base>_BC.dds`, `<base>_N.dds`, `<base>_RMAOS.dds`,
`<base>_E.dds`.

**Shared-texture dedup (collision-safe).** Conversion output depends on the
*pairing*, not just the source texture — metal recovery reads the spec map,
emissive bakes read material color — so two materials sharing a `_d` can
legitimately produce different converted results. Each unique
**input-set + options hash** converts exactly once (shared pairings reuse
the output; batches never redo work), and when the same source diverges
across pairings the later outputs disambiguate with a short hash suffix.
Silent overwrites are structurally impossible.

**Manifest.** A sidecar in the output root records, per artifact:
provenance (sources, material, profile), the options hash, masks and NIF
associations, and unhonored parameter fields. It powers:

- **Incremental re-runs** — up-to-date outputs skip; converting a full
  game's assets resumes instead of restarting.
- **Rollback** — `--clean` (and a workspace button) removes exactly what
  the converter produced, nothing else.
- Reproducibility — re-running with the recorded options bit-matches.

Mask paint strokes and fills participate in the editor's normal undo stack.

## Implementation notes

- BGSM/BGEM binary layouts: reference the legacy C# editor
  (`E:\Projects\Fo4CommunityShaders\PBRMaterialEditor`) and the cached
  `wbDefinitionsFO4.pas`. Read-only parser in a new `src/bgsmreader.*`.
- Poisson solve: small vendored FFT (or iterative multigrid) — offline
  cost at 2048² is negligible.
- The conversion core must be pure/host-testable (images in → images +
  `.pbrm` JSON out) so the test suite can pin the math with fixtures:
  spec-gloss inference cases, metal recovery, gloss curve, AO/curvature
  derivation on synthetic normals, BGSM param mapping, BGEM family choice.
- Batch scale: conversion runs on a worker thread with progress; BA2
  extraction reuses the existing archive reader.

## CLI parity

Standing rule: **every headless-capable operation ships with a CLI verb**,
and new features land with their verb from day one. A companion console
binary `pbrm.exe` (same core sources as the GUI and tests — console
subsystem for clean stdout, exit codes, and pipeable output) exposes
subcommand grammar `pbrm <verb> [options]`:

`validate`, `migrate` (v4 → v5 re-save), `extract-states`, `strip-legacy`,
`repair-paths`, `set-setting`, `pack` (channel-packer bake incl. Toksvig),
`export-definitions`, `export-json`, `inspect` (texture diagnostics),
`convert` (full FO4/FO76 batch with `--dry-run` — **shipped**),
`material-info` (parsed `.bgsm`/`.bgem` field dump — **shipped**),
`bake-ao` (geometry-AO rebake against the manifest — with the workspace).

`pbrm convert --out <mod folder>` writes `Materials/` + `Textures/` mirrors,
the sibling `.pbrm` set, and `pbrm-conversion-manifest.json` (one entry per
material: outputs, flags, notes, errors, metal coverage). Options:
`--texture-root`, `--dry-run`, `--json`, `--height-in-blue`,
`--no-detail-ao`, `--metal-diagnostic`, `--recovery`, `--gloss-curve`,
`--derivation-size`. Live-verified 2026-07-23 on FO76 (alien dielectric 0%,
gold Protectron 90% metal reconstructed warm from black diffuse), FO4 BGSM
v1/v2 (X-02 metal trim + auto-denoise engagement + dangling-reference
warnings surfaced honestly), and FO4 BGEM (Unlit Effect emission via
base × tint × intensity) — plus the FO4 glowmap `_E` bake (mask × BGSM
emittance color, `primaryEmissive` intensity = emittance mult).

FO76 normal maps ship as **BC5_SNORM** (legacy `BC5S` fourCC or DXGI 84);
the DDS reader decodes signed BC4/BC5 natively (test-pinned endpoints) —
unsigned reads would fold negative components into garbage.

Every reporting verb accepts `--json` for machine-readable output; sources
and output roots are explicit flags with the editor's configured sources
only as fallback; geometry AO and projection passes run on a
`QOffscreenSurface` GL context with `--no-geometry-ao` as the degraded
mode. The GUI executable keeps `--validate`/`--ui-smoke` as compatibility
aliases.

## Non-goals

- Editing or writing `.bgsm`/`.bgem`.
- Meshes with **embedded shader properties and no material file**: there is
  no `.bgsm` path for the runtime to probe a sibling against, so the
  converter cannot reach them by design. Their path is the runtime's direct
  `.pbrm` NIF reference plus assignment tooling in the NifSkope fork.
- De-lighting baked shadows/highlights out of vanilla diffuses (beyond the
  metal recovery); that is an art pass, not a converter guarantee.
- NIF editing — the sibling-probe contract makes it unnecessary.
- Landscape (`.btd`/LTEX) conversion — separate future effort.
