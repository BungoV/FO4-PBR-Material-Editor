# FO4 PBR Material Editor

> **⚠️ Work in progress — this tool is not yet meant to be used; it is feature
> incomplete.** The `.pbrm` material format it authors has no released runtime
> yet (the Fallout 4 Community Shaders PBR runtime is still in development),
> so materials created today cannot be loaded by any game. Published for
> visibility and early feedback only.

An independent C++20/Qt 6 material authoring application for the Fallout 4
Community Shaders PBR material system. The UI and data model were written from
scratch for this project and do not depend on Bethesda's Material Editor.

## Current scope

- Creates, opens, edits, and atomically saves `.pbrm` version 5 materials
  (version 4 files load through an in-memory migration).
- Eleven focused shader families plus the `Advanced` validation/authoring superset.
- Primary plus optional Secondary and Tertiary UV texture sets, including
  explicit overlay-to-UV0 layout mirroring and independently authored
  transforms/animation. The Tertiary layer can source either mesh UV channel
  and composites above the Secondary layer.
- A dedicated Landscape workflow: ordinary UV pages are hidden and terrain
  base-color, normal/height, RMAOS, height, and macro arrays live together.
- World-space resources such as the fallback environment cubemap remain outside
  both UV sets.
- Packed-channel enable/fallback controls, per-texture and page-wide
  texture/constant source controls, automatic fallback-field dependencies,
  and individual plus page-wide folding.
- Compact checked input toggles and a visibly muted inactive Secondary UV layer.
- Base color, tint masks, normal/height, RMAOS, emissive, subsurface,
  clearcoat, surface effects, gradients, dynamic normals, displacement,
  decals, landscape, hair, vegetation, eye, glass, water, and effect inputs.
- Standard-surface anisotropy (tangent map with rotation and strength), bent
  normals driving ambient diffuse direction and cavity-aware specular
  occlusion, retroreflection constants, a transmission-lobe tint, and a
  clearcoat authorable per UV layer with OpenPBR-style coat tint/absorption.
- Height Blend layer coverage compares Normal+Height blue channels so
  overlays settle into recesses; the preview BRDF applies Kulla-Conty
  multi-scatter energy compensation and optional screen-space specular
  anti-aliasing.
- Runtime-owned Surface States (PBRM v5): state behavior — behavior tags and
  interaction physics (liquids wash particulates, thermal dries liquids and
  melts frost into liquid), albedo/roughness/F0 responses, sparkle, blackbody
  glow, directionality, streaking, animated rain ripples and drips, gain/dry
  rates, retention, and map-guidance weights — lives in a
  `FO4.PBRM.SurfaceStateDefinitions` JSON defined once globally, never per
  material. The material contributes physical inputs only: porosity in RMAOS
  alpha (or a constant, or the `saturate(roughness × (1 − metallic))`
  heuristic), signed curvature in Normal alpha (edge shedding, cavity
  collection, cavity specular occlusion on direct light, subsurface scatter
  broadening), plus AO/height/roughness/metallic/emissive read directly, and
  the small Surface Response bindings (adhesion constant, shelter vertex
  channel, StateMasks name bindings for hero placement). Wetness / Dust /
  Frost / Heat / Blood ship as built-in defaults; Settings can export the
  starter definitions JSON or preview a modded set; the preview's States tab
  simulates amounts with a looping weather timeline (rain storm, dust storm,
  blizzard, heat wave). Legacy v4 materials migrate on load, with batch
  operations to extract authored states to a definitions JSON and strip the
  retired data; landscape materials additionally offer stochastic hex-tile
  anti-tiling.
- Multi-material asset preview: imported multi-submesh models can assign a
  saved .pbrm to each submesh (Scene tab → Materials…). Assigned submeshes
  render their material through the standard PBR path while the edited
  document keeps full fidelity on the rest; assignments persist next to the
  mesh in a .pbrmset sidecar.
- A Half-Life: Alyx-style bottle liquid for the glass families: a fill plane
  clips the container mesh into a liquid volume with depth absorption, a lit
  flat cap, a meniscus band, and a damped spring that sloshes the surface when
  the preview object is moved or rotated.
- UV1 overlay composition, vertex-painted layer selection, pattern reveal,
  gradient alpha, and independent UV transform/animation authoring fields.
- Per-layer oscillators for non-linear "breathing" animation: each UV layer
  gets a waveform (sine, triangle, pulse, eased breathing), frequency, and
  phase driving independent amplitudes for UV scale/offset/rotation, emissive
  pulse, coverage/opacity fade, and normal-strength swell — composing with
  the linear scroll/rotation animation and previewing on the scrubbable clock.
- World and Object Planar coordinate sources preview live, including true
  three-tap normal-weighted triplanar for the primary layer's core maps
  (secondary/tertiary triplanar and auxiliary samples use the dominant-axis
  projection).
- Channel-audit extras: bent normals carry a baked occlusion-cone channel for
  measured specular occlusion, six-way smoke gains an alpha fire mask with a
  blackbody self-illumination ramp, flipbooks accept an optional motion-vector
  map that advects frame cross-fades, a tiling detail normal covers close-range
  micro-surface, landscape layers pack surface-state porosity into the normal
  array's alpha, and single-channel inputs get BC4 bake suggestions plus
  validation when fed multi-channel textures. Packed-texture export generates
  mean-preserving per-channel mips; normal-map exports vector-average and
  renormalize each mip, and RMAOS exports can fold a paired normal map's
  variance into the roughness chain (Toksvig) so minified speculars stop
  aliasing without changing mip 0.
- A particle compositing stack for the effect families: per-layer flipbooks
  (grid, age/time/constant drivers, loop modes, frame cross-fade), polar
  coordinates, extended layer blends (Screen, Subtract, Min, Max, Overlay),
  modifier layers whose RG field distorts or flow-advects the primary layer
  and whose alpha erodes it over particle age with a glowing edge, palette
  remap through the Particle Gradient or a procedural blackbody ramp,
  per-particle start-frame/phase/hue variation, and edge-on plus camera-near
  fades. The viewport's Particle Simulation controls scrub or auto-loop age
  and seed.
- Blender-style numeric fields: hover to reveal arrows, then drag left/right.
  Hold Shift for fine movement or Ctrl for coarse movement.
- A project-native color picker matching the Wild Wasteland NifSkope visual
  language: previous/current swatches, HSV field, RGB/HSV/alpha controls, hex,
  preset and custom palettes, plus a multi-monitor screen eyedropper.
- DDS/BC1-BC7 and ordinary-image thumbnails with R/G/B/A isolation, resolution,
  compression, color-space, channel, mip-count, and file-size diagnostics,
  including inline mip/color-space/compression warnings and portable-path state.
- Texture drag-and-drop and optional discovery of matching base-color, normal,
  RMAOS, and emissive maps in the same directory.
- Ordered texture sources for loose mod folders, a Fallout 4 Data/Textures
  directory, and Fallout 4 DX10/GNRL BA2 archives. Loose files override archive
  entries, and selected files are authored relative to their `Textures` root.
- A searchable NifSkope-style texture browser merges configured loose and BA2
  assets, identifies their source, previews supported formats, and assigns the
  portable virtual path directly to a texture input.
- A live OpenGL 3.3 metal/rough material preview on a camera-facing plane,
  cube, sphere, or imported static FBX/Fallout 4 NIF mesh. It includes UV0/UV1 and submesh
  selection, perspective/orthographic and axis views, a generated studio
  cubemap, DDS cubemaps and floating-point 2:1 OpenEXR preview environments,
  an independently rotatable key light, lighting presets (including Rim
  Backlight and Overhead Softbox studio rigs), exposure, selectable tonemap
  operators (ACES, AgX, Reinhard, Linear), backgrounds, a ground plane,
  PCSS-style contact-hardening key-light shadows, depth-based SSAO, bloom,
  a camera turntable, overlay-free PNG viewport screenshots, comparison,
  material debug views, and a Substance-style PBR Validation render mode
  that false-colors non-physical albedo/metallic values.
  A progressive path-traced reference mode (BVH-accelerated, 1 spp/frame up
  to 512 spp) renders true soft area shadows, bounce light, and traced
  reflections for ground-truth material checks; it simplifies to the primary
  layer with smooth vertex normals and pauses raster-only effects while
  active.
  Specular image-based lighting uses a GGX-prefiltered cubemap across its full
  roughness mip range and a generated split-sum BRDF lookup table; the sharp
  source cubemap remains separate for the background and mirror reflections.
  Navigation matches Wild Wasteland NifSkope: MMB orbit, Shift+MMB pan, RMB
  dolly, wheel zoom, Shift+LMB key-light rotation, F frame, Numpad 1/3/7 axis
  views, and Numpad 5 projection. Preview setup is an editor preference and is
  never serialized into `.pbrm`.
- The core preview evaluates UV transforms and animation, Primary/Secondary
  shader-space layering, packed-channel overrides, alpha-test and blend
  composition, material IBL scales/rotation, and Base/Gradient alpha sources.
  It no longer alpha-composites encoded normal or RMAOS images.
- Static FBX loading uses the vendored MIT-licensed `ufbx` importer, and the
  native Fallout 4 NIF path reads BSTriShape-family geometry and vertex colors.
  Meshes are
  triangulated, centered, normalized, and supplied with generated normals or
  tangents when missing. NIF skinning and mesh animation are deliberately ignored;
  skinned NIFs are shown in bind pose.
- The material preview covers the core Primary/Secondary base color, normal,
  RMAOS, emissive, gradients, coverage, and height inputs. It previews single-sample offset,
  parallax occlusion mapping, and key-light self-shadowed POM from Primary or
  Secondary Normal Blue / Separate Height sources, with adaptive quality
  presets, expert ray-step overrides, grazing fade, and optional UV clipping.
  Tessellated true displacement, dynamic normals, tint masks, surface fuzz (sheen),
  glints, iridescence, clearcoat, subsurface/transmission, hair lobes, glass,
  water/foam, eye/cornea, vegetation wind, projected-decal channel masks,
  distortion, and distinct unlit/simple-lit/full-PBR effect paths are evaluated
  in the standalone viewport. Landscape mode can select individual DDS array
  slices and applies the authored macro color/roughness variation. Scene-only
  integrations such as runtime LAND weights, scene color, shadows, fog, and
  motion-vector history use isolated-preview approximations where applicable.
- Per-channel source swaps on packed inputs: any channel of RMAOS, normal, or
  other packed textures can be redirected to a channel of another file (with
  inversion for gloss-to-roughness). Thumbnails and the preview show the
  composited result, and Export Packed Texture bakes it to a game-ready DDS —
  BC1/BC4/BC5/BC7 with generated mip chains via the vendored rgbcx/bc7enc
  encoders, plus uncompressed DDS and PNG.
- Global cross-page search with direct navigation; complete texture-input and UV
  layer copy/paste/reset; default/override/runtime/invalid state badges.
- Starting templates for metal, painted metal, plastic, glass, skin, vegetation,
  hair, water, physically lit effects, and landscape materials.
- Validation with quick-fixes: findings render as severity-ordered rows, and
  findings with a safe automatic remedy carry a Fix button (each fix is a
  single undo step and re-validates immediately); texture-file findings stay
  manual by design. Plus undo/redo, recent materials and last-session reopen,
  an authored-overrides comparison page, Ctrl+Backspace field reset,
  expert-mode overrides, and root-relative portable paths.

The editor defines and previews authoring data. Community Shaders runtime
binding, animated/skinned NIF evaluation, and Creation Kit integration remain
separate follow-up work.

## Build

The project uses qmake and the same Qt 6/MinGW family as the custom NifSkope
fork.

```powershell
$env:Path = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:Path
qmake6 PBRMaterialEditor.pro
mingw32-make -j4
```

The executable is written to `bin/PBR Material Editor.exe`. The console
companion builds from `cli/PbrmCli.pro` the same way and lands at
`bin/pbrm.exe`.

Create a portable folder (including the MSYS2 runtime dependencies omitted by
`windeployqt`) with:

```powershell
.\scripts\package.ps1
```

## Command line (`pbrm.exe`)

Every headless-capable operation has a CLI verb (and new features ship with
theirs). The portable package includes `pbrm.exe` beside the editor:

```text
pbrm validate [--texture-root <dir>] [--json] <files or folders...>
pbrm migrate [--dry-run] <paths>            # re-save on the current schema
pbrm extract-states <paths>                 # legacy states -> definitions JSON sidecars
pbrm strip-legacy [--dry-run] <paths>       # remove legacy surface data
pbrm set-setting --section S --key K --value V [--dry-run] <paths>
pbrm repair-paths [--texture-root <dir>] [--dry-run] <paths>
pbrm export-definitions <file.json>         # built-in surface state definitions
pbrm export-json <paths>                    # readable .pbrm.json sidecars
pbrm pack --material <m.pbrm> --slot <key> --out <f.dds>
          [--set UV0] [--format bc1|bc4|bc5|bc7|bc7-srgb|rgba8]
          [--no-mips] [--toksvig <normal>] [--texture-root <dir>]
pbrm inspect [--texture-root <dir>] <textures> # diagnostics + channel statistics
pbrm convert --out <mod folder> [--texture-root <dir>] [--dry-run] [--json]
             [--force] [--height-in-blue] [--no-detail-ao] [--metal-diagnostic]
             [--recovery <0-1>] [--gloss-curve <v>] [--derivation-size <px>]
             <.bgsm/.bgem files or folders>   # FO4/FO76 -> PBR conversion
pbrm convert --out <mod folder> --clean       # roll back exactly the recorded output
pbrm material-info <.bgsm/.bgem files>        # parsed material field dump
pbrm bake-ao --mesh <nif/fbx> --out <image>   # geometry AO from a reference mesh
             [--size <px>] [--directions <n>] [--strength <exp>]
```

`convert` writes `Materials/` + `Textures/` mirrors, same-name sibling
`.pbrm` files (sources untouched), and `pbrm-conversion-manifest.json`;
identical source pairings dedup to one converted texture set, and
divergent pairings get a hash suffix instead of a silent overwrite.
Re-runs are incremental: materials whose source, options, and sibling are
unchanged since the recorded run are skipped (`--force` re-runs them);
`--clean` removes exactly the files the manifest records — foreign files
in the output folder are never touched.

Reporting verbs accept `--json`; exit codes are 0 (clean), 1 (findings or
failures), 2 (usage). `--texture-root` overrides the editor's configured
sources so CI machines behave predictably. The GUI executable keeps
`--validate` and `--ui-smoke` as compatibility aliases.

## Tests

```powershell
Set-Location tests
qmake6 MaterialDocumentTests.pro
mingw32-make -j4
..\bin\MaterialDocumentTests.exe
```

The tests validate all shader identifiers, complete `.pbrm` save/load
round-trips, UV and specialized resource state, settings, invalid-container
rejection, DDS/BC7 channel decoding, compressed BA2 texture reconstruction and
browser enumeration, DDS texture-array slice selection, procedural preview
geometry and displacement tessellation, tangent/normal generation,
normalization, optional real-FBX import via `PBRM_TEST_FBX`, and optional real
Fallout 4 NIF import via `PBRM_TEST_NIF`. A real BC7
texture can be supplied through `PBRM_TEST_BC7`.

Third-party source and licenses are in `third_party/ufbx/`,
`third_party/detex/`, `third_party/tinyexr/`, and `third_party/bc7enc/`
(rgbcx/bc7enc BC1–BC7 encoders used by packed-texture export).

## PBRM format

See [docs/PBRM-v6.md](docs/PBRM-v6.md). Version 6 replaces the authored dielectric F0 with OpenPBR specular weight and IOR ([docs/PBRM-v6-Specular.md](docs/PBRM-v6-Specular.md)). Version 5 moves surface state
behavior out of materials into runtime-owned definitions
([docs/SurfaceStates-Architecture.md](docs/SurfaceStates-Architecture.md)):
materials carry physical maps — porosity in RMAOS alpha, signed curvature in
Normal alpha — and v4 files load through an in-memory migration. The
prototype-format license continues: breaking changes remain possible before
runtime integration.

## Changelog

### 2026-09-17 — Retroreflection input, sheen is fuzz

- **Retroreflection input** - retroreflection is its own texture slot
  (`primaryRetroreflection`, R = retro mask with the usual override + constant)
  carrying `retroStrength` (default 1) and `retroWidth`. The slot checkbox is
  the switch. Documents that kept the retro pair on Surface Effects, or in the
  old Shading group, move it on load; a lit strength enables the slot.
- **Sheen is fuzz** - the Surface Effects sheen keys and labels are now fuzz
  (OpenPBR's term): `overrideFuzz`, `fuzzFallback`, `fuzzStrength`,
  `fuzzRoughness`. Sheen-keyed documents migrate on load.

### 2026-09-16 — PBRM v6, feature parameters on their inputs, specular color override (v0.5.0)

- **PBRM v6** - the authored dielectric F0 is gone: RMAOS alpha carries the
  OpenPBR specular weight (texel 255 = 1 = default) and the level is a specular
  IOR, a constant (1.5 = F0 0.04) or the Specular Color map's alpha over a
  ceiling (4.25, so IOR 1.5 is texel 90). The 0.16 conductor guard went with the
  F0 it guarded. The Normal slot's alpha is a selector: None / Curvature / Cone
  Map (reserved). Old documents load exactly; authored F0 or tint-coverage
  alphas are marked and repacked by the validation quick-fix or `pbrm migrate`
  (`.v5.bak` beside every rewritten file). Spec: `docs/PBRM-v6.md`; runtime
  contract: `docs/PBRM-v6-Specular.md`.
- **Feature parameters live on their texture input** - specular IOR on Specular
  Color, the subsurface profile/weight/transmission/thickness on Subsurface,
  sheen/glints/film/retro on Surface Effects, hair lobes on Hair Direction and
  Hair Scatter, glass on Transmission Absorption, the eye on Cornea and Iris
  Masks. The Shading page keeps Core, Water, Bottle liquid and Unlit. The slot
  checkbox is the feature switch; the "Disabled" subsurface mode is gone.
  Shading-page groups migrate on load and leave the file.
- **Specular Color RGB override** - the slot carries the editor-wide RGB
  override (toggle + constant colour, white = no change) beside the IOR
  override, the IOR constant and the map ceiling. Authored maps keep being read.

### 2026-09-03 — A specular color map

- **Specular Color slot** - a new texture input, `primarySpecularColor`: RGB is
  a specular tint and alpha is how much of it applies. RMAOS alpha carries the
  dielectric F0 as a single grey number, so until now a material had no way to
  say that its own specular reflection is colored — a green bottle, a tarnish
  film, colored lacquer. Following OpenPBR's `specular_color`, the map **tints**:
  it multiplies the reflectance level RMAOS still owns, so it can darken or
  shift the hue and can never raise reflectivity. White RGB and zero weight are
  both the exact identity, and a material without the map renders exactly as it
  did before. Metals are untouched — their F0 is the base color, and a metal's
  82-degree edge tint is an angular effect this shading model does not carry.
  The slot had no constants at first; since 2026-09-16 it carries the RGB
  override (`overrideColor`, `color`) and the specular IOR (see PBRM v6). The "Specular F0"
  render mode now shows the resolved color, and the preview clamps the
  dielectric level to 0.16 the way the runtime always has.
- **Converter** - Fallout 76 materials get the hue of their `_r` reflectance
  map, which the F0 luminance used to throw away (the level still goes to RMAOS
  alpha, so only the color is new), and only where the map is genuinely colored.
  Fallout 4 materials get their own `cSpecularColor` constant, which the game's
  deferred renderer discards and which 15% of vanilla materials actually author
  — a Fallout 4 `_s` map is BC5, specular strength and gloss, with no color in
  it to take. Both write `<name>_SC.dds` beside the other converted textures.
- **Format** - `docs/PBRM-v6.md` gained the socket as an additive row. No schema
  version change: a document written before this parses unchanged, and a
  consumer that ignores the socket renders what it always rendered.

### 2026-09-03 — The Material Converter is experimental and opt-in

- **Converter gated behind an option** - Settings > Preview Cubemaps… (the
  Editor Settings dialog) gained an **Experimental: Material Converter**
  checkbox, off by default and stored as `Interface/experimentalConverter`.
  While it is off the converter is not merely disabled: the "Material
  Converter" row is never added to the workspace dropdown, and the switcher
  itself is hidden, because a one-destination dropdown is dead UI. Turning the
  option off while the converter is open returns the window to the Material
  Editor first, so the workspace stack and the window title never point at a
  mode the switcher can no longer name. The `--converter` / `--converter-scan`
  verification flags still open it, enabling the gate for that session only and
  never writing the stored setting.

## License

Copyright (C) 2026 Bungo (BungoV)

GPLv3 (see `LICENSE`), matching the Community Shaders ecosystem. The vendored
`third_party/bc7enc` texture encoder is MIT licensed and keeps its own license
file. Qt 6 is used under LGPLv3.
