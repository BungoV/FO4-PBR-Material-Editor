# Surface States: Runtime-Owned Architecture

Status: concept, agreed 2026-07-22. Supersedes the per-material Surface State
slot design (`SurfaceStates/State0..7`) shipped in v0.2.0.

## Principle

**The material describes what a surface *is*. The runtime describes what
happens *to* surfaces.**

Rain, dust, frost, heat, and blood are environmental phenomena. Their behavior
— how fast they accumulate, what they do to roughness, how they ripple and
drip — must be defined once, globally, so a rainstorm looks coherent across
every material in the world. The v0.2.0 design let every `.pbrm` carry its own
30-parameter definition of "wetness", which means per-material drift, a huge
authoring burden, and no single tuning point.

After this shuffle the material contributes only **measurable surface
physics**, expressed through maps it mostly already has:

| Property | Drives | Source |
| --- | --- | --- |
| Roughness / Metallic | Grip vs. shed, sealing, film behavior | RMAOS.R / RMAOS.G |
| Ambient occlusion | Where grime and moisture collect | RMAOS.B |
| Height | Pooling (puddles) | Normal+Height.B |
| **Porosity** | Soak-in darkening vs. beading, retention, dry rate | **RMAOS.A** (new duty) |
| **Signed curvature** | Convex edge shedding, concave collection, cavity specular occlusion, SSS scatter width, transmission thickness fallback | **Normal+Height.A** (new duty) |
| Emissive | Heat suppression (rain steams off the forge) | Emissive slot |
| Hero placement | Authored splatter / snow / opt-out | StateMasks slot (unchanged) |

Everything behavioral leaves the material and becomes a runtime-owned
**Surface State Definitions** asset.

## Ownership tiers

### 1. Runtime definitions (FO4CS-owned)

A single definitions asset ships with the runtime (working name
`Data\PBRM\SurfaceStates.json`; exact location is a #38 campaign decision).
Schema `FO4.PBRM.SurfaceStateDefinitions`, version 1. It carries what
`SurfaceStateSpec` carries today, minus the two material-side bindings:

- Identity: `name` (the amount-binding ID), `tags`
  (liquid/particulate/frozen/thermal/viscous).
- Physics: `resistance`, `gainRate`, `dryRate`, `retention`.
- Appearance: `color`, `blendMode` (darken/tint/coat), `desaturation`,
  `porosityInfluence`, `roughnessTarget/Weight/Mode`, `f0Target/Weight`,
  `filmF0`, `killMetallic`, `sparkle`, `glowIntensity`.
- Map-guided placement: `cavityBias`, `aoBias`, `heightBias`,
  `roughnessBias`, `metalSeal`, `emissiveDry`, `threshold`, `softness`,
  `directionality`, `streaking`, `edgeShedding`.
- Dynamics: `ripples` (ripples & drips), plus a new `respectsShelter` bool
  (whether vertex-color shelter data suppresses this state — rain yes,
  blood no).

The tag interaction matrix (thermal dries liquid and melts frozen into
liquid, liquid washes particulates and dilutes viscous, viscous resists
suppression) stays **fixed-function runtime code** keyed off the tags, exactly
as `Pbr::surfaceStateInteractions()` implements it today.

State *amounts* are bound by name at runtime: the weather system drives
"Wetness", gameplay can drive "Blood", etc. Materials never author amounts.

### 2. Material (`.pbrm`-owned)

Physical maps plus three small bindings:

- `SurfaceResponse/adhesion` — constant, default 1. Rare "treated surface
  sheds everything" opt-out. No map channel; spatial cases use StateMasks.
- `SurfaceResponse/shelterVertexChannel` — which vertex-color channel carries
  authored shelter (under-the-awning stays dry), −1 = none. The *channel* is a
  per-mesh authoring choice, so it stays material-side; whether a state cares
  is `respectsShelter` in the runtime definition.
- `SurfaceResponse/maskBindings` — map of state name → StateMasks channel
  (e.g. `{"Blood": "R"}`). Placement binding, not behavior: a state bound
  here uses the authored mask instead of response accumulation, as today.

### 3. Editor

The editor previews and simulates runtime definitions; it no longer authors
per-material behavior. Its preview shader remains the reference
implementation for the #38 runtime campaign. Optionally (later phase) it
gains a definitions-file editor so the existing per-state UI investment
survives as the authoring tool for the *global* asset.

## Channel moves

### Porosity → RMAOS alpha

F0 alpha and porosity are nearly mutually exclusive **by physics**:
dielectrics (cloth, leather, fabric — the dominant use case, character
clothing) never author F0 but are exactly where porosity varies; metals
author F0 but are sealed, so porosity is meaningless there (`metalSeal`
already handles wet-metal beading). RMAOS is already 4-channel BC7 — zero
format or memory cost.

Slot changes:

- New choice `alphaCarries`: **Dielectric F0** (default) / **Porosity**.
- Existing `overrideF0`/`f0` unchanged; apply when A carries F0, and `f0`
  serves as the constant when A carries porosity.
- New `overridePorosity`/`porosity` pair (default 0.5), following the
  editor's universal toggle convention. One documented exemption: porosity
  is excused from the empty-texture auto-force rule, because unlike other
  channels an absent texture still has a meaningful derived value.

Effective porosity resolution:

1. `overridePorosity` checked → the constant.
2. Else `alphaCarries == Porosity` and the texture channel is present →
   sample RMAOS.A.
3. Else → `saturate(roughness × (1 − metallic))` — the standard engine
   heuristic. Most materials need no authoring at all; you intervene only
   where the heuristic lies (polished marble, rough sealed plastic).

### Signed curvature → Normal+Height alpha

Curvature comes from the same high-poly bake as the normal map — the classic
pairing. Encoding: 0.5 neutral, < 0.5 convex (edges, shed), > 0.5 concave
(cavities, collect). Opting in costs BC5 → BC7, same 8 bpp, zero memory.

Slot changes:

- `layerMaskInAlpha`, `overrideLayerMask`, `layerMask` are **deleted**
  (see layer coverage below — alpha's old duty).
- New toggle `curvatureInAlpha` (default false; BC5 remains the normal-only
  default). No constant pair: a uniform curvature is not a material
  property — neutral is a no-op, and a non-neutral constant merely
  duplicates the per-state `cavityBias`/`edgeShedding` definition weights.
  Curvature is map-or-derived.
- New `cavitySpecOcclusion` number (0–1, default 1) — see below.

Effective curvature resolution:

1. `curvatureInAlpha` and channel present → sample Normal.A.
2. Else derive the concave half from AO: `0.5 + (1 − ao) × 0.5`. The convex
   half is not derivable from AO (AO reads 1 on flats and edges alike), so
   `edgeShedding` in a state definition only has effect on materials with
   authored curvature. Accepted.

#### Cavity specular occlusion (second consumer of the channel)

AO correctly darkens ambient light only, so direct sun specular streaks
straight across panel seams, screw recesses, and weld lines — meso-features
far below shadow-map resolution sparkle as if flat. The concave half of the
curvature map is exactly a list of those features:

```glsl
float concave = saturate((curvature - 0.5) * 2.0);   // 0 on flats and edges
directSpecular *= 1.0 - concave * cavitySpecOcclusion;
```

Applied to **direct (analytic-light) specular only**. It composes with, not
replaces, the existing occlusion terms: RMAOS AO / BentNormal cone govern
ambient & IBL specular; cavity governs direct.

Hard rule: **authored curvature only**. The AO-derived fallback is
contaminated with large-scale occlusion the shadow map already handles;
feeding it here would sneak AO onto direct light and double-darken. States
may use the fallback; this term must not. Cost is one uniform and one
multiply on a value the state system already sampled.

#### Subsurface scattering (third and fourth consumers)

Runs on the subsurface/vegetation families only.

**Scatter broadening (pre-integrated style, Penner GPU Pro 2).** How far the
scatter bleed crosses the terminator depends on surface curvature: ears,
nose rims, fingers, and creases broaden; flat areas stay crisp. Curvature is
the technique's second LUT axis. Use the **magnitude** — both signs scatter:

```glsl
float scatterCurv = abs(curvature - 0.5) * 2.0;
```

Scale split: the authored map is meso-scale (pores, wrinkles, seams), while
the broad geometric term (nose radius vs. skull radius) comes free from
screen-space derivatives — `length(fwidth(N)) / length(fwidth(worldPos))`.
The recipe is derivative curvature for broad forms **+** the authored map
for meso detail.

**Transmission thickness fallback (convex half).** Backlit glow through
ears, nostrils, fingertips, and leaf edges needs thickness. SSS.A carries
authored thickness and always wins; the convex half of curvature becomes
the fallback when A is not authored ("strongly convex ↔ thin" correlates
well on organics — and the families gate away the convex-boulder lie).
Same philosophy as the porosity heuristic: authored data wins, the derived
proxy covers the unauthored common case. Free Vegetation win for leaf-edge
translucency.

#### One fallback rule for every lighting-side consumer

The AO-derived curvature fallback feeds **states only**. Cavity specular
occlusion and both SSS uses require the authored map (screen-space
derivative curvature needs no authoring and is always available). The
thickness proxy could not use the fallback anyway — it needs the convex
half, which AO cannot produce.

### Deleted: Surface State Response slot

The 4-channel `SurfaceStateResponse` slot (R porosity / G adhesion /
B cavity / A variation) is removed on every UV page. Channel disposition:

| Old channel | Fate |
| --- | --- |
| R porosity | → RMAOS.A / constant / heuristic |
| G adhesion | → per-material constant only |
| B cavity | → Normal.A signed curvature; concave duty was already redundant with the direct AO read (`aoBias`) and pooling with the height read (`heightBias`) |
| A variation | → dropped; runtime world-space noise is the breakup, StateMasks is the authored intent |

### Unchanged

- **StateMasks** (`primaryStateMasks`, UV0 only) — optional hero placement.
- **Landscape** — the normal+height array's alpha keeps carrying per-layer
  porosity; no curvature channel for terrain (edge shed matters less there).
  Landscape constants (`overridePorosity`/`porosity`) unchanged.

## Layer coverage: height blend replaces the alpha mask

Normal.A's old duty — high-frequency breakup for UV1/UV2 layer blending —
had four pattern sources; source 0 (primary Normal.A) is removed. The
replacement hierarchy:

1. **Height blend (coverage mode 3) becomes the recommended default** for
   layered materials: `smoothstep(−softness, softness, over − under + bias)`
   comparing the layers' Normal.B heights. Dirt fills crevices, the top
   layer wins on peaks — physically motivated and better-looking than a
   painted lerp mask. Layered materials usually author height anyway (it
   also feeds parallax).
2. Art-directed breakup that height can't express (logos, deliberate
   patches) → the existing dedicated **Layer Coverage slot** (single R
   channel, BC4, sampled in the overlay's UV space). Needing curvature
   *and* a hand mask simultaneously costs this one cheap texture — the
   rare case pays, not every material.
3. Overlay base alpha and constant sources remain unchanged.

BC5 normal-only materials never had an alpha channel, so they lose nothing.

## Texture budget (typical outfit material)

| Texture | Channels | Format |
| --- | --- | --- |
| Base Color / Opacity | RGB + A | BC7 / BC1 when opaque |
| Normal + Height + Curvature | RG + B + A | BC7 (BC5 if neither B nor A used) |
| RMAOS + Porosity | R/G/B + A | BC7 |

**Zero additional textures** versus plain PBR. Optional extras only where
authored intent demands: StateMasks, Layer Coverage.

## Shader / uniform contract

The `uState*[8]` uniform family keeps its shape; only its *source* changes —
runtime definitions instead of per-material groups. The per-fragment response
fetch is rewritten:

- `porosity` — RMAOS.A / constant / `saturate(rough × (1 − metal))`.
- `curvature` — Normal.A / constant / AO-derived concave.
- `adhesion` — uniform scalar from the material constant.
- `variation` — world-space noise only (`vnoise3(worldPos × 9)` as today).

Plus the new lighting-side curvature consumers outside the state block, all
gated on authored curvature: the direct-specular cavity multiply, scatter
broadening (authored meso + screen-space derivative geometric term), and the
convex-half transmission thickness fallback when SSS.A is unauthored.

Ripples, drips, streaking, and the weather envelope stay algorithmically as
implemented (including the projection-quality fade and 3×3-cell ripple
neighborhood fixes) — they are runtime behavior, previewed by the editor.

## Editor changes

- **Surface States page** → replaced by a slim **Surface Response** page:
  adhesion constant, shelter vertex channel, StateMasks binding table, and a
  read-only summary of effective porosity/curvature sources (with links to
  the RMAOS / Normal slots). A note states that behavior is runtime-owned.
- **States preview tab** — unchanged UX (per-state amount sliders, weather
  simulation with gain/dry integration), but driven by the built-in default
  definitions. `surfaceStatePresets()` becomes those defaults — the single
  source for the simulator and for an exported starter JSON. Add a
  `Load definitions…` editor preference to preview a modded definitions file.
- **Definitions editor (later, optional)** — reuse the existing per-state
  profile widgets as an editor for the definitions JSON itself
  (File → Surface State Definitions…). Not part of the first slice.

## Migration

Prototype-format license applies (breaking changes accepted). Bump
`schemaVersion` to 5 with load-time migration:

- Legacy `SurfaceStates/State0..7` and named legacy groups
  (Wetness/Dust/Frost/Heat/Blood): stripped on load. A batch operation
  **Extract states → definitions JSON** converts a material's resolved
  states into a definitions file first, so authored tuning is not lost.
- `SurfaceStateResponse` slot: porosity constant folds into the RMAOS
  porosity constant; adhesion constant folds into
  `SurfaceResponse/adhesion`; an assigned map path becomes a validation
  error pointing at the channel packer (repack R → RMAOS.A, B → Normal.A).
- `layerMaskInAlpha == true`: validation error pointing at height blend or
  the channel packer (repack A → Layer Coverage BC4); the flag and its
  constants are dropped from the schema.
- Per-state `maskChannel`/`shelterChannel`: fold into
  `SurfaceResponse/maskBindings` / `shelterVertexChannel`.
- Batch folder ops (#41) gain all of the above as bulk migrations.

## Validation

- Error: legacy state groups or response-slot map present (with the
  migration pointers above).
- Warning: `alphaCarries == Porosity` while metallic is substantially
  authored (F0 authoring sacrificed); `curvatureInAlpha` on a texture with
  no usable alpha (e.g. BC5); `maskBindings` naming a state absent from the
  loaded definitions; `edgeShedding` present in definitions but no authored
  curvature (informational).
- Format advisories update: Normal → BC5 unless height or curvature used;
  RMAOS stays BC7; Layer Coverage → BC4.
- Advisory: curvature texture whose lower mips drift from the base-level
  mean (renormalized/contrast-boosted chain — repack to fix).

## Accepted losses (deliberate)

- Per-material behavior divergence — the point of the redesign.
- Spatial adhesion maps — StateMasks covers hero cases.
- Authored variation/dissolve maps — runtime noise + StateMasks.
- Edge shedding without authored curvature.
- Primary-Normal-alpha layer masks — height blend / Layer Coverage slot.

## Future runtime catalog (engine-level)

Everything here is implemented in the runtime (FO4CS / Creation Engine
side), **not** in the material editor — the material contributes only the
maps already specified plus the few toggles noted. The name-bound amount
model turns degradation systems into content: anything the runtime can
compute — weather, item condition, shots fired, wetness-hours, hit events —
can drive a named state, and curvature/porosity place it correctly with no
new material data.

**Schema generalizations these want**

- Signed curvature bias (collect on edges — wear — not just cavities).
- Signed directionality (down-facing accumulation for wading mud).
- `metallicTarget/Weight` (wear revealing bare metal must *raise* metallic;
  the profile can currently only kill it).
- Per-object amount bindings (condition, counters, exposure integrals).

**States-shader refinements (curvature-guided)**

- Drip & streak seeding — weight the drip/streak fields by local convexity
  so runs shed from ledge lips and fasteners (the rust-streak-under-bolt
  signature).
- Frost nucleation — bias frozen-tag dissolve by curvature magnitude; frost
  grows inward from edges and seams, as it does physically.
- Corona discharge — an "Energized" definition with glow weighted to the
  convex extreme (field concentration at sharp points): arcs and glow crawl
  on spikes and fins.

**Lighting-side (authored curvature only, per the fallback rule)**

- Micro-shadowing on direct diffuse — low-strength concave darkening of
  direct diffuse (Uncharted 4-style); rides `cavitySpecOcclusion` at a
  fixed, globally tunable ratio — no second material constant.
- Crease blood tint — subsurface family: concave × subtle red shift
  (knuckles, nose creases). Pairs with scatter broadening.
- Detail-normal gating — mask the DetailNormal contribution by convexity so
  micro-scratches live on edges: the static twin of the wear state. One
  editor-side toggle on the DetailNormal slot.

**Decal physics inheritance (porosity cross-use)**

Projected decals (blood, oil) read the receiving surface's porosity and
curvature exactly as states do: soak in and go matte on cloth, bead and
stay glossy on metal. One rule makes every splatter in the game obey the
same physics as the weather. No new material data.

**Content-only states (definitions data)**

Wear & grip polish (condition/use-driven, opposite roughness targets), rust
as a wetness-history integral, muzzle soot, weapon residue pushed onto
victims, rad-storm fallout, boot mud, moss/lichen (very slow,
north/down-facing, porosity-gated), sun bleaching (UV-hours, up-facing,
desaturates albedo).

## Performance model

Constants are the cheap side of the ledger: they ride the per-material
constant buffer that is uploaded and bound regardless, and a CB read is
cheaper than any texture sample. The expensive resources are fetches and
distinct bound textures — which this design strictly reduces: the response
map is gone, and porosity/curvature ride channels of fetches that already
happen (a BC7 block decompresses as RGBA whether three or four channels are
read).

Map-vs-constant selection must not create shader permutations. Resolve it
either as a uniform branch on a CB flag (coherent, effectively free — the
editor preview's `uHas*`/`uOverride*` pattern) or branchlessly by uploading
resolved per-channel `scale`/`bias` so one path serves both:
`value = sample × scale + bias`, constant case `scale = 0`. Permutations
stay reserved for structural switches (shader family, landscape), matching
the engine's existing technique boundaries.

## Mip discipline (channel packer)

- Curvature mips are plain mean-preserving averages — no contrast
  preservation, no renormalization. Distant results then converge to the
  correct area average: a seam-dense panel legitimately keeps a slight
  cavity dimming at range rather than snapping to neutral. Validation
  flags chains that drift from the base mean.
- Roughness mips get normal-variance baking (Toksvig): when packing RMAOS
  alongside a normal source, fold the normal map's per-mip variance into
  the roughness chain. This is the physically correct fix for distant
  specular aliasing, done offline — it replaces any runtime curvature-based
  approximation, and the runtime keeps only its screen-space specular AA.

## Open questions (deferred to #38)

- Definitions file location, load order, and mod-override policy
  (single file vs. additive fragments).
- Hot reload of definitions in-game.
- Amount sources beyond weather (interior dust baselines, per-cell zones,
  script/quest bindings) — all runtime concerns, invisible to materials.
- Definitions schema versioning/migration policy (mirror PBRM's
  capability-gated approach).

## Sequencing

1. **Editor slice (this repo):** schema v5 + migrations, channel moves on
   the two slots, response-fetch rewrite in the preview shader, Surface
   Response page, simulator repoint, validation, batch ops, golden fixtures.
2. **Definitions asset:** freeze the JSON schema, export the starter file
   from the built-in defaults, add `Load definitions…` preview.
3. **Runtime campaign (#38, FO4CS repo):** definitions loader, weather →
   amount binding, uniform upload, response fetch port from the editor's
   reference shader.
