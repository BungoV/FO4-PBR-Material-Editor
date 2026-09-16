# PBRM version 6

Version 6 replaces the authored dielectric F0 with OpenPBR `specular_weight`
and `specular_ior` (contract, arithmetic, landmarks and migration in
[PBRM-v6-Specular.md](PBRM-v6-Specular.md)) and turns the normal-alpha
curvature flag into a selector. Everything else is version 5 unchanged.

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
  "schemaVersion": 6,
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
- **Signed curvature** — Normal alpha when the slot's `alphaCarries` is
  `Curvature` (`Cone Map` reserves the alpha for a relaxed-cone parallax map
  and yields the derived curvature; `None` is the default)
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
`windowEmissive` intensity;
`SurfaceEffects` R is the sheen/fuzz weight, B is the **thin-film thickness
in MICROMETRES (µm)** and A is the thin-film weight. The unit on B is
normative and matches OpenPBR Surface v1.1.1's `thin_film_thickness`, whose
range is `[0, ∞)` µm with a default of `0.5`; it is stated here because an
unstated unit makes every import mapping a guess.

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

Feature parameters live on the feature's texture input. A slot's `values`
hold its channel overrides and the parameters of the lobe or effect that
input drives: `primarySpecularColor` (`overrideColor`, `color`, `ior`, `iorMax`, `overrideIor`),
`primarySubsurface` (`mode`, `profile`, `customScatterColor`,
`customRadiusR/G/B`, `weight`, `transmission`, `transmissionTint`,
`thickness`), `primarySurfaceEffects` (`sheenStrength`, `sheenRoughness`,
`glintIntensity`, `glintScale`, `filmIor`, `iridescenceStrength`,
`retroStrength`, `retroWidth`), `primaryHairDirection` (`direction`, the
`primary*` and `secondary*` lobe constants), `primaryHairScatter`
(`scatterStrength`, `backlightStrength`), `primaryTransmissionAbsorption`
(glass `ior`, `absorptionDistance`, `refractionRoughness`, `thinWalled`),
`primaryCornea` (`ior`, `wetness`) and `primaryIrisMasks` (`irisDepth`,
`limbalStrength`, `pupilScale`). Subsurface and surface effects are on while
their input is enabled, with or without a map (there is no `Disabled`
subsurface mode); hair, glass and eye read their input's constants
regardless, as does the specular IOR. The `settings` object keeps only
groups that have no texture input (`Shading/Core`, `Shading/Water`,
`Shading/BottleLiquid`, `Shading/Unlit`, transparency, geometry, vegetation,
environment and advanced groups). The retired groups `Specular`,
`Shading/Subsurface`, `Shading/SurfaceEffects`, `Shading/Hair`,
`Shading/Glass` and `Shading/Eye` migrate into the primary slot on load
(subsurface `thicknessScale` multiplies into the slot `thickness`, mode
`Disabled` disables the input, hair `scatter`/`backlight` become
`scatterStrength`/`backlightStrength`, `corneaIor` becomes `ior`).

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

## OpenPBR Surface v1.1.1 mapping

PBRM is being aligned to the [OpenPBR Surface v1.1.1
specification](https://academysoftwarefoundation.github.io/OpenPBR/). This
table is the *reference* half of that alignment: it says, for every OpenPBR
parameter, what PBRM calls it, what unit it is in, and whether the two agree.
It is a mapping table, **not** a rename plan — the identifier alignment pass is
scheduled separately, and until it lands the PBRM names in the middle column
are the authoritative ones.

Three verdicts are used and they mean different things. **Present** = same
quantity, same units, name aside. **Different** = the same *idea* under a
different parametrization or in different units, so an importer must convert.
**Absent** = PBRM has no equivalent. A fourth annotation, **editor-only**,
marks parameters the editor authors and previews but that the FO4 runtime does
not yet consume.

| OpenPBR parameter | Unit / range | PBRM equivalent | Verdict |
| --- | --- | --- | --- |
| `base_weight` | float `[0,1]`, default 1 | `primaryBaseColor` `colorScale` | Present, different name |
| `base_color` | color3 `[0,1]³`, default 0.8 grey | `primaryBaseColor` RGB (sRGB) | Present; OpenPBR additionally constrains it by `E_diffuse = (1 − E_spec)·C` |
| `base_metalness` | float `[0,1]`, default 0 | `RMAOS` G | Present |
| `base_diffuse_roughness` | float `[0,1]`, default 0 | — | Absent (no Oren-Nayar σ on either side) |
| `specular_weight` | float `[0,∞)`, norm `[0,1]`, default 1 | `RMAOS` A while `alphaCarries="Specular Weight"`, else the `specularWeight` constant; `[0,1]`, default 1 | Present |
| `specular_color` | color3 `[0,1]³`, default white | `primarySpecularColor` RGB (sRGB; dielectric-only) | Different: OpenPBR uses it as the 82° **edge tint** on metals (the F82 model), which PBRM does not implement |
| `specular_roughness` | float `[0,1]`, default 0.3 | `RMAOS` R | Present, same convention (α = r²) |
| `specular_roughness_anisotropy` | float `[0,1]`, default 0 | `Anisotropy` slot | Editor-only, and a bent-reflection approximation rather than OpenPBR's αt/αb split |
| `specular_ior` | float `(0,∞)`, norm `[1,3]`, default 1.5 | `primarySpecularColor` constant `ior` (default 1.5, no ceiling), or its A over `[0, iorMax]` (ceiling default 4.25) while `overrideIor=false` | Present; `F0 = ((η−1)/(η+1))²`, IOR 0 makes the tint the F0 outright |
| `transmission_weight` | float `[0,1]`, default 0 | `CompositionMode::PhysicalTransmission` | Different: a composition mode, not a mix weight |
| `transmission_color` | color3 `[0,1]³`, default white | `TransmissionAbsorption` RGB | Editor-only |
| `transmission_depth` | length, default 0 | `TransmissionAbsorption` A | Different: a thickness, with no Beer-law `μt = −ln T / λ` |
| `transmission_scatter`, `transmission_scatter_anisotropy` | — | — | Absent |
| `transmission_dispersion_scale`, `transmission_dispersion_abbe_number` | — | — | Absent by design (a deferred G-buffer has no wavelength dimension) |
| `subsurface_weight` | float `[0,1]`, default 0 | `primarySubsurfaceScatter` W × thickness × scale | Present |
| `subsurface_color` | color3 `[0,1]³`, default 0.8 grey | `primarySubsurface` RGB | Different meaning: OpenPBR constrains it as a single-scatter albedo, ours tints a screen-space blur |
| `subsurface_radius` × `subsurface_radius_scale` | length per channel, default (1, 0.5, 0.25) | per-profile blur radius **in pixels** | Different: a global screen-space radius, not a per-material world-space mean free path |
| `subsurface_scatter_anisotropy` | float `[−1,1]` | — | Absent by design (a Gaussian-sum blur has no phase function) |
| — | — | `subsurfaceTransmission` / `subsurfaceTransmissionTint` | PBRM extension: OpenPBR has this only in thin-walled mode |
| `fuzz_weight` | float `[0,1]`, default 0 | `SurfaceEffects` R ("sheen") | Editor-only |
| `fuzz_color` | color3 `[0,1]³`, default white | — | Absent (the preview reuses the base colour) |
| `fuzz_roughness` | float `[0,1]`, default 0.5 | `uSheenRoughness` | Different: OpenPBR mandates the Zeltner-2022 SGGX microflake LTC model; ours is a Fresnel-rim add |
| `coat_weight` | float `[0,1]`, default 0 | `Clearcoat` A × coat strength | Editor-only |
| `coat_color` | color3 `[0,1]³` — the **square** of normal-incidence transmittance | coat tint × coat absorption | Different units: ours is a straight multiply, OpenPBR's is T² and view-dependent |
| `coat_roughness` | float `[0,1]`, default 0 | `Clearcoat` B | Present |
| `coat_roughness_anisotropy` | float `[0,1]`, default 0 | — | Absent |
| `coat_ior` | float `(0,∞)`, default 1.6 | coat IOR (`F0 = ((η−1)/(η+1))²`) | Present |
| `coat_darkening` | float `[0,1]`, default 1 | — | Absent |
| — | — | `Clearcoat` RG (coat normal) | Matches `geometry_coat_normal` |
| `thin_film_weight` | float `[0,1]`, default 0 | `SurfaceEffects` A × iridescence strength | Present in spirit |
| `thin_film_thickness` | **micrometres (µm)**, `[0,∞)`, default 0.5 | `SurfaceEffects` B | **Present, and the unit is µm** — see the channel-semantics note above |
| `thin_film_ior` | float `(0,∞)`, default 1.4 | film IOR | Present |
| — (placement) | between coat and base, folded into the Fresnel factor, energy-neutral (spec lines 867/873) | an additive post-shade term above the coat | Different, and known-wrong; scheduled, not fixed here |
| `emission_luminance` | cd/m² (nits), `[0,∞)`, norm `[0,1000]`, default 0 | `primaryEmissive` intensity × the class-based emissive radiance ladder | Different units: ours is a dimensionless authored intensity times a per-class multiplier calibrated against a measured frame, deliberately not nits |
| `emission_color` | color3 `[0,∞)³`, default white | `primaryEmissive` RGB (sRGB) | Present; OpenPBR permits values above 1 for HDR textures, ours caps at 1 and puts the headroom in the intensity scalar |
| `geometry_opacity` | float `[0,1]`, default 1 | `primaryBaseColor` A + global opacity + alpha threshold | Present, richer |
| `geometry_thin_walled` | bool | — | Absent by design |
| `geometry_normal` | vector | `primaryNormal` RG (+B height, +A curvature) | Present, richer |
| `geometry_tangent` | vector | `Anisotropy` RG | Editor-only |
| `geometry_coat_normal` | vector | `Clearcoat` RG | Editor-only |
| `geometry_coat_tangent` | vector | — | Absent |

PBRM slots with no OpenPBR counterpart are FO4-specific or effect-pipeline
data rather than gaps: `TintMask`, `StateMasks`, `BentNormal`, `DetailNormal`,
`SeparateHeight`, `DynamicNormal`, `SubsurfaceDetail`, `HairDirection`,
`HairScatter`, `InteriorMap`, `Imperfections`, `WaterFlow`, `Foam`,
`IrisMasks`, `Cornea`, `Gradient`, `LayerCoverage`, `ScreenDistortion`,
`ParticleGradient`, `SixWayA`/`SixWayB`, `MotionVectors`.

### Energy rules

Independently of the parameter table, OpenPBR imposes layering rules that a
conforming implementation must obey. Two are relevant to how PBRM materials
are authored:

- **Glossy-diffuse split** (spec line 671): `f_gd ≈ f_dielectric +
  (1 − E_dielectric(ω_o)) · f_diffuse`, so `base_color` is constrained by
  `E_diffuse = (1 − E_spec) · C`. A material authored to look right *without*
  this split will read slightly dark once it is applied. The FO4 runtime
  obeys it on the sky-ambient path and, since the `bIndirectEnergySplit` key,
  on the screen-space-GI path as well; the editor preview has always obeyed
  it.
- **Multiple-scattering compensation** (spec line 425) is required, and both
  the runtime and the preview implement it.

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
| `primaryNormal` | linear RG tangent normal, optional B height, A signed curvature or cone map per `alphaCarries` | `overrideNormal=true`, neutral normal `#8080FF`; strength `1.0`, `heightInBlue=false`, `alphaCarries="None"`, `cavitySpecOcclusion=1.0` |
| `primaryRmaos` | linear R roughness, G metallic, B AO, A specular weight or porosity | `override*` channel fields default true; roughness `0.5`, metallic `0.0`, AO `1.0`, specular weight `1.0`; `alphaCarries="Specular Weight"`, `overridePorosity=false`, porosity `0.5` |
| `primaryEmissive` | sRGB RGB radiance, A intensity mask | color `#FFFFFF`, intensity `0.0` |
| `primarySpecularColor` | sRGB RGB specular tint (read only while `overrideColor=false`), A specular IOR map (read only while `overrideIor=false`) | `overrideColor=true`, `color=#FFFFFF`, `overrideIor=true`, `ior=1.5`, `iorMax=4.25`; the IOR constants are read whether or not the slot is enabled; an enabled slot with RGB overridden or no map tints with `color`; a disabled or absent slot means a white tint and the constant IOR |

`primarySpecularColor` RGB is the OpenPBR `specular_color` tint on the
dielectric Fresnel, and its alpha, while `overrideIor=false`, is the
`specular_ior` map. The dielectric F0 resolves as

    F0_ior = ((ior − 1) / (ior + 1))²
    ior    = overrideIor ? ior : specularColor.a × iorMax     (ior, iorMax, overrideIor on the slot)
    F0     = specularWeight × specularColor.rgb × F0_ior

with `specularWeight` the RMAOS alpha (while `alphaCarries="Specular Weight"`
and `overrideSpecularWeight=false`) or the constant, and the tint white when
the slot is absent. A material without the map at the defaults (weight 1,
IOR 1.5) is F0 0.04, the v5 default point bit for bit. Metals are untouched:
their F0 is the base color, mixed in by metallic. The consumer no longer
clamps the dielectric F0 to 0.16: the weight can only lower the level and the
IOR owns it. Surface states blend the resolved dielectric F0 (after weight,
tint and IOR, before the metal mix, as in v5). An opaque colour map decodes
A as `1.0`, which would read as the IOR ceiling; that is why the alpha is
read only behind `overrideIor=false` (the override defaults to true, and the
editor forces it while the texture has no alpha channel).

A texture is sampled only when its slot has `enabled=true`, a non-empty valid
runtime path, and the corresponding positive `override*` flag is false.
Otherwise the constant applies. Missing or unavailable texture channels force
their positive overrides on, with one documented exemption: `overridePorosity`
defaults false and is never auto-forced, because an absent porosity source has
a meaningful derived value (the roughness/metallic heuristic). RMAOS alpha is
sampled as specular weight only while `alphaCarries` is `Specular Weight` and
`overrideSpecularWeight=false`; while alpha carries porosity, the weight
always uses its constant (default `1.0`).

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
| 7 | `RmaosSpecularWeight` | bit 3, `overrideSpecularWeight=false`, and `alphaCarries="Specular Weight"` |
| 8 | `EmissiveTexture` | enabled, valid `primaryEmissive` path |
| 9 | `NormalCurvature` | bit 1 and `alphaCarries="Curvature"` |
| 10 | `RmaosPorosity` | bit 3 and `alphaCarries="Porosity"` |
| 26 | `SpecularIorTexture` | enabled, valid `primarySpecularColor` path, and `overrideIor=false` |
| 27 | `NormalConeMap` | bit 1 and `alphaCarries="Cone Map"` (reserved; no consumer yet) |

Bits 11-24 were taken by later runtime slices for per-channel base/normal/
emissive sampling, composition, and the subsurface bindings; the runtime
header `src/Materials/PBRM.h` is authoritative for those. The next assignment
is:

| Bit | Name | Condition |
| ---: | --- | --- |
| 25 | `SpecularColorTexture` | enabled, valid `primarySpecularColor` path, and `overrideColor=false` |

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

Invalid magic, an envelope/schema version other than `4`, `5` or `6`, an invalid
payload size, malformed/non-object JSON, an unknown schema, an empty shader
name, wrong root field types, and malformed requirement versions are hard
parse errors. An unknown non-empty shader name is valid but
runtime-unsupported.

v4 and v5 read through in-memory migrations and save as v6. v5 constants
migrate exactly (`specularWeight = min(f0 / 0.04, 1)`; an F0 above 0.04 moves
its level into `primarySpecularColor.ior`), while texture alphas authored as F0 or tint
weight cannot: the loader marks them (`legacyF0Alpha`, `legacyTintAlpha`) for
the editor's validation quick-fix or `pbrm migrate`, which rewrite the
textures beside a `.v5.bak` (PBRM-v6-Specular.md section 4).
There is no implicit migration from PBRM versions 1-3 and no best-effort read
of future versions. Additive optional fields, slots, and requirements remain
v6. Existing names, channel meanings, and defaults are stable. Only an
incompatible semantic or structural change may use a new envelope/schema
version (v7), and that change requires an explicit migrator.

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
