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

- Creates, opens, edits, and atomically saves `.pbrm` version 4 materials.
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
- A generic Surface State system: up to eight authorable state slots, each
  with a runtime binding name, behavior tags (liquid, particulate, frozen,
  thermal, viscous) that drive the interaction physics (liquids wash
  particulates, thermal dries liquids and melts frost into liquid), and a
  generic response profile — albedo blend, roughness/F0 response, sparkle,
  blackbody glow, up-facing directionality, vertical streaking, animated rain
  ripples plus droplets dripping down vertical surfaces with wet trails,
  per-state gain/dry rate multipliers (honored by the weather simulation, so
  cloth soaks fast and stays damp long after the storm), porosity retention
  (porous areas collect first and dry last), map-guided accumulation from
  channels every material already carries (AO crevice collection, height-map
  puddling, roughness grip, metallic absorption sealing, emissive-hot
  drying), vertex-color sheltering, and an optional dedicated placement mask
  channel. One shared packed response map (porosity, adhesion, cavity,
  variation) serves every state, with a procedural AO+noise fallback when
  none is authored. Wetness/Dust/Frost/Heat/Blood ship as factory presets,
  legacy materials resolve transparently, and the preview's States tab
  simulates amounts per state with a looping weather timeline (rain storm,
  dust storm, blizzard, heat wave); landscape materials additionally offer
  stochastic hex-tile anti-tiling.
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
  array's alpha, the response map's cavity channel doubles as signed curvature
  for per-state edge shedding, and single-channel inputs get BC4 bake
  suggestions plus validation when fed multi-channel textures.
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
  an independently rotatable key light, lighting presets, exposure, backgrounds,
  a ground plane, adjustable real-time key-light shadows, comparison, and
  material debug views.
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
  Tessellated true displacement, dynamic normals, tint masks, surface sheen,
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
- Validation, undo/redo, recent materials and last-session reopen, an authored-
  overrides comparison page, Ctrl+Backspace field reset, expert-mode overrides,
  and root-relative portable paths.

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

The executable is written to `bin/PBR Material Editor.exe`.

Create a portable folder (including the MSYS2 runtime dependencies omitted by
`windeployqt`) with:

```powershell
.\scripts\package.ps1
```

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

See [docs/PBRM-v4.md](docs/PBRM-v4.md). Version 4 deliberately invalidates
the earlier prototype format so the schema can evolve cleanly before runtime
integration.

## License

Copyright (C) 2026 Bungo (BungoV)

GPLv3 (see `LICENSE`), matching the Community Shaders ecosystem. The vendored
`third_party/bc7enc` texture encoder is MIT licensed and keeps its own license
file. Qt 6 is used under LGPLv3.
