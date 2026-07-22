# PBRM version 5

PBRM v5 is a compact binary envelope around a UTF-8 JSON authoring document.
`src/materialdocument.h` and `src/materialdocument.cpp` are authoritative for
the editor. All integer fields are unsigned little-endian values.

v5's semantic change: surface state behavior moved out of the material into
runtime-owned definitions (`FO4.PBRM.SurfaceStateDefinitions`); the material
carries physical maps only — porosity in RMAOS alpha, signed curvature in
Normal alpha — plus the small `SurfaceResponse` bindings. See
`docs/SurfaceStates-Architecture.md` for the design. The editor reads v4 with
an in-memory migration and always writes v5.

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `char[4]` | ASCII `PBRM` |
| 4 | `uint32` | Envelope version (`5`; `4` accepted on read) |
| 8 | `uint32` | JSON payload size in bytes |
| 12 | `byte[]` | Compact UTF-8 JSON payload |

The payload limit is 64 MiB. The declared size must consume the rest of the
file exactly; truncation and trailing bytes are errors. Files are written with
`QSaveFile`, so replacing an existing material is atomic.

## JSON root

The editor writes the core root fields and empty authoring collections.
`requirements` is optional and is shown here to illustrate its placement:

```json
{
  "schema": "FO4.PBRM.Material",
  "schemaVersion": 5,
  "shader": "Standard",
  "secondaryUvEnabled": false,
  "primaryUv": {},
  "secondaryUv": {},
  "tertiaryUvEnabled": false,
  "tertiaryUv": {},
  "resources": {},
  "settings": {},
  "requirements": {}
}
```

`schema`, `schemaVersion`, and `shader` are required. The schema string is
case-sensitive. The editor can edit names returned by `shaderModeNames()`;
matching is case-insensitive on read and canonicalized on write. Earlier v4
names ending in ` Surface` remain accepted as read-only aliases and are
rewritten to their concise names on save. A runtime must treat any other
non-empty shader name as a valid but unsupported future family, never as
`Standard`. The current editor refuses to edit such a document rather than
silently converting it. If present,
`secondaryUvEnabled` and `tertiaryUvEnabled` must be Booleans and each
collection must be an object. A missing collection is treated as an empty
object.

`tertiaryUvEnabled` and `tertiaryUv` are additive members: the editor writes
them only when the tertiary layer is enabled or authored, so documents that
never use the layer keep a byte-identical payload. The tertiary layer offers
the same overlay socket family as the secondary layer with a `tertiary` key
prefix, and its coordinate source may select either mesh channel
(`Mesh UV1` by default or `Mesh UV0`); its `UV2`, `UV2/Animation`, and
`UV2/Composition` setting groups are independent and it composites above the
secondary layer.

Effect-family materials may author the additive `Particles/Layer0..2` and
`Particles/Global` setting groups: per-layer flipbook atlases (columns, rows,
driver, loop mode, frame blending), polar coordinates, layer roles (`Color`
composites with the layer blend mode — including the extended `Screen`,
`Subtract`, `Min (Darken)`, `Max (Lighten)`, and `Overlay` modes — while
`Modifier` layers distort/advect and erode the primary layer), palette
remapping, per-particle variation, and view fades. Particle age and seed are
runtime inputs and are never serialized.

Texture-slot `values` may carry additive `swapR`/`swapG`/`swapB`/`swapA`
objects (`{path, channel, invert}`) redirecting single channels of a packed
input to another texture. These are authoring-time metadata: the editor
composites them for preview and thumbnails, and validation warns until they
are baked to a real packed texture, because a runtime always samples the one
authored path.

Surface state behavior is runtime-owned as of v5: the response profiles
(blend modes, roughness/F0 responses, gain/dry rates, map-guidance weights,
ripples) live in a `FO4.PBRM.SurfaceStateDefinitions` version-1 JSON asset
shipped with the runtime, and state amounts are runtime data bound by state
name. Neither is ever serialized into a material. The material contributes
physical inputs that the definitions read:

- **Porosity** — RMAOS alpha when the slot's `alphaCarries` is `Porosity`,
  else the `overridePorosity`/`porosity` constant when checked, else derived
  as `saturate(roughness × (1 − metallic))`. Landscape keeps per-layer
  porosity in the `normalHeightArray` alpha.
- **Signed curvature** — Normal alpha when `curvatureInAlpha` is true
  (`0.5` neutral, below = convex edges that shed, above = concave cavities
  that collect). There is no curvature constant; without the map the concave
  half derives from AO (`0.5 + (1 − ao) × 0.5`), which feeds states only —
  lighting-side consumers (cavity specular occlusion via the slot's
  `cavitySpecOcclusion` weight, subsurface scatter broadening, the convex
  thickness fallback) require the authored map.
- **`SurfaceResponse` settings group** — `adhesion` (0..1 constant; 0 sheds
  every state), `shelterChannel` (`None`/`Red`/`Green`/`Blue`/`Alpha`
  vertex-color channel painted 1 where geometry shelters the surface; which
  states respect it is part of the definitions), and `maskR`/`maskG`/
  `maskB`/`maskA` (runtime state names placed by the `primaryStateMasks`
  RGBA socket instead of map-guided accumulation).
- **AO, height, roughness, metallic, emissive** — read directly by the
  definitions' map-guidance weights at zero extra cost.

v4 documents migrate on load: the retired `SurfaceStateResponse` socket's
porosity constant folds onto RMAOS, its adhesion constant into
`SurfaceResponse`, per-state `maskChannel`/`shelterChannel` selections lift
into the `SurfaceResponse` bindings, and the retired `Primary Normal Alpha`
layer-pattern source becomes `Constant`. Legacy `SurfaceStates/*` groups and
response sockets that still carry an assigned map are preserved inert;
validation flags them and Batch Operations can extract them to a definitions
JSON and strip them.

Additional additive channel semantics:
`BentNormal` B carries the baked occlusion-cone openness (1 = open);
`SixWayB` A is an emissive fire mask driven by the slot's `fireIntensity`;
`SixWayA` supports `overrideOpacity`/`opacity` (default: sample texture A);
the landscape `normalHeightArray` A is per-layer surface-state porosity
(`overridePorosity`/`porosity` constants); `MotionVectors` (RG, effect
families) shares the primary flipbook grid and advects frame cross-fades;
`DetailNormal` is a tiling micro-detail normal (`strength`, `uvScale`) whose
optional B carries micro roughness variation (`overrideRoughVariation` /
`roughVariation`, centered at 0.5 — BC5 textures simply skip it) and whose
optional A masks where detail applies; the `InteriorMap` cubemap's alpha is
a lit-window mask whose areas self-illuminate at the slot's
`windowEmissive` intensity.

`UV0/Oscillation`, `UV1/Oscillation`, and `UV2/Oscillation` are additive
per-layer oscillator groups for non-linear ("breathing") animation: a
`waveform` (`Sine`, `Triangle`, `Pulse` with `duty`, or `Breathing` — an
eased sine), `frequency` in Hz, and `phase`, plus independent amplitudes
that the one oscillator drives — `scaleAmplitude` (UV swell),
`offsetAmplitudeU`/`V` (sway), `rotationAmplitude` (wobble),
`emissiveAmplitude` (glow pulse), `coverageAmplitude` (layer coverage, or
opacity on the primary layer), and `normalAmplitude` (surface-detail
swell). All values are constants — the runtime reproduces the motion from
its own clock; no keyframe data is serialized.

`.pbrmset` files are editor-side sidecars stored next to a preview mesh, not
part of the PBRM format: `{"schema": "FO4.PBRM.MaterialSet", "version": 1,
"assignments": {"<submesh name>": "<.pbrm path>"}}` with paths relative to the
mesh directory when possible. Runtimes that want per-shape material binding
read material paths from the NIF itself; the sidecar only drives the editor's
multi-material preview.

`Shading/BottleLiquid` is an additive setting group for the glass families
describing a contained liquid volume (fill level, liquid and meniscus colors,
absorption, surface brightness, and wobble spring constants). The liquid is
rendered by clipping the container mesh against a fill plane whose tilt is a
runtime spring response to object motion; the wobble state itself is runtime
data and is never serialized.

`primaryUv`, `secondaryUv`, and `tertiaryUv` map stable socket keys to
texture-slot objects:

```json
{
  "enabled": true,
  "expanded": true,
  "path": "textures\\pbr\\example_rmaos.dds",
  "values": {
    "overrideRoughness": false,
    "roughness": 0.5
  }
}
```

`expanded` is editor state and has no runtime effect. `values` contains the
socket-specific options. `settings` is grouped by authoring page and feature.
`resources` contains shader-owned non-UV sets such as the `Landscape` arrays.
Preview cubemaps are editor preferences and are never serialized into PBRM.

Unknown root keys, collection members, texture slots, and nested values are
additive extension data. Consumers ignore data they do not understand. The
editor preserves unknown root members and unknown members inside the four
collection objects across load/save for every shader family it can edit.

## Runtime requirements

`requirements` is an optional object mapping semantic capability names to
positive integer versions:

```json
{
  "requirements": {
    "standard.primaryUv": 1,
    "standard.clearcoat": 1
  }
}
```

Optional data must not appear in `requirements`. An input or behavior that is
essential to interpreting the material must declare a requirement. A consumer
that does not provide the named capability at the required version keeps the
PBRM document valid but must fail closed instead of partially rendering it.
The initial runtime provides `standard.primaryUv` version `1`.

## Minimal Standard runtime slice

The first Community Shaders runtime slice recognizes only `Standard`
and these Primary UV socket keys:

| Socket | Texture data | Missing-value defaults |
| --- | --- | --- |
| `primaryBaseColor` | sRGB RGB color, A opacity/coverage | `overrideColor=true`, color `#FFFFFF`; `overrideOpacity=true`, opacity `1.0`; tint `#FFFFFF`, scale `1.0`, blend `Base` |
| `primaryNormal` | linear RG tangent normal, optional B height, A signed curvature | `overrideNormal=true`, neutral normal `#8080FF`; strength `1.0`, `heightInBlue=false`, `curvatureInAlpha=false`, `cavitySpecOcclusion=1.0` |
| `primaryRmaos` | linear R roughness, G metallic, B AO, A dielectric F0 or porosity | `override*` channel fields default true; roughness `0.5`, metallic `0.0`, AO `1.0`, F0 `0.04`; `alphaCarries="Dielectric F0"`, `overridePorosity=false`, porosity `0.5` |
| `primaryEmissive` | sRGB RGB radiance, A intensity mask | color `#FFFFFF`, intensity `0.0` |

A texture is sampled only when its slot has `enabled=true`, a non-empty valid
runtime path, and the corresponding positive `override*` flag is false.
Otherwise the constant applies. Missing or unavailable texture channels force
their positive overrides on, with one documented exemption: `overridePorosity`
defaults false and is never auto-forced, because an absent porosity source has
a meaningful derived value (the roughness/metallic heuristic). RMAOS alpha is
sampled as F0 only while `alphaCarries` is `Dielectric F0` and
`overrideF0=false`; while alpha carries porosity, F0 always uses its constant
(default `0.04`).

The runtime feature mask is derived rather than serialized. The initial bit
assignments are:

| Bit | Name | Condition |
| ---: | --- | --- |
| 0 | `BaseColorTexture` | enabled, valid `primaryBaseColor` path |
| 1 | `NormalTexture` | enabled, valid `primaryNormal` path |
| 2 | `NormalHeightBlue` | bit 1 and `heightInBlue=true` |
| 3 | `RmaosTexture` | enabled, valid `primaryRmaos` path |
| 4 | `RmaosRoughness` | bit 3 and `overrideRoughness=false` |
| 5 | `RmaosMetallic` | bit 3 and `overrideMetallic=false` |
| 6 | `RmaosAo` | bit 3 and `overrideAo=false` |
| 7 | `RmaosF0` | bit 3, `overrideF0=false`, and `alphaCarries="Dielectric F0"` |
| 8 | `EmissiveTexture` | enabled, valid `primaryEmissive` path |
| 9 | `NormalCurvature` | bit 1 and `curvatureInAlpha=true` |
| 10 | `RmaosPorosity` | bit 3 and `alphaCarries="Porosity"` |

These are parser/cache feature bits, not the older BGSM CB2 sentinel. Secondary
UV, specialized shaders, and other sockets remain parseable authoring data but
are outside the first runtime binding slice.

## Path contract

The editor retains `path` exactly as authored. This preserves round trips and
allows absolute paths for local preview, but absolute paths are not portable
runtime material references.

For runtime lookup and cache keys, a consumer must:

1. Treat an empty or whitespace-only path as no texture.
2. Convert `/` to `\\`, remove leading `.\\`, collapse repeated separators,
   and compare/cache case-insensitively.
3. Treat a leading `textures\\` as optional so both Data-relative and
   texture-root-relative authoring resolve to the same engine texture key.
4. Reject drive-qualified, UNC, root-qualified, and parent-traversal paths.
5. Preserve the authored string in diagnostics; normalization is a derived
   lookup value and must not rewrite the document.

Path rejection disables that texture slot and reports a material diagnostic; it
does not make the whole PBRM envelope unparsable.

## Errors, fallback, and migration

Invalid magic, an envelope/schema version other than `4` or `5`, an invalid
payload size, malformed/non-object JSON, an unknown schema, an empty shader
name, wrong root field types, and malformed requirement versions are hard
parse errors. An unknown non-empty shader name is valid but
runtime-unsupported.

v4 reads through the in-memory migration described above and saves as v5.
There is no implicit migration from PBRM versions 1-3 and no best-effort read
of future versions. Additive optional fields, slots, and requirements remain
v5. Existing names, channel meanings, and defaults are stable. Only an
incompatible semantic or structural change may use a new envelope/schema
version (v6), and that change requires an explicit migrator.

PBRM is standalone. A NIF directly references its `.pbrm`; no same-name BGSM or
BGEM is consulted. An invalid, unsupported, or requirements-incompatible PBRM
uses the runtime's fail-closed invalid-material behavior.

The Community Shaders renderer bridge has its own ABI versions, independent of
the PBRM v4 envelope. Draw-constant ABI `1` is a 96-byte `b13` block; texture
binding ABI `1` maps Base Color to `t0`, Normal to `t1`, and packed RMAOS to
`t2`. An ABI mismatch must take the vanilla path rather than reinterpret rows or
texture slots.

## Golden fixtures

The exact typed JSON contracts used by tests live in:

- `tests/fixtures/minimal-standard-surface.json`
- `tests/fixtures/secondary-uv.json`
- `tests/fixtures/landscape.json`

The test suite wraps each JSON fixture in the v5 binary envelope, parses it,
compares the complete typed JSON object, saves it again, and compares the exact
compact payload. The JSON files are text-form golden sources so format changes
remain reviewable.
