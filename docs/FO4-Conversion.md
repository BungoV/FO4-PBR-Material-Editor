# Fallout 4 / Fallout 76 → PBR asset conversion

Status: spec agreed 2026-07-22, and largely **shipped** — the reader, math core,
derivations, orchestrator, CLI verbs, BA2-direct reads, correction masking,
geometry AO, the Convert workspace and the node graph are all live. What remains
is noted per section. The converter is an editor-integrated module: the editor
already owns BA2 reading, BC1–7 decode, game-ready DDS encode with mip
discipline and Toksvig, the channel compositor, batch-operation framing,
validation, and the live preview that makes manual review practical.

Where this document states a number — a threshold, a floor, a ceiling — it is
**measured against the real corpus at `E:\Projects\F76\Data`** and the survey
that produced it is named, so it can be re-run rather than trusted. Physical
reflectance tables do not describe this data and are not used as a source.

## Source profiles

Conversion is organized as per-title **source profiles** that own parsing,
texture-suffix conventions, channel semantics, and inference math; everything
downstream of them — the derivations, geometry AO, masks, workspace, and
`.pbrm` output — is shared:

- **Fallout 4** (BGSM/BGEM version 2): a **spec-gloss workflow that never
  states metallicity**. `_s.G` inverts to roughness (with the `.bgsm` scalar
  substituted where that channel is flat, which is 13.4% of the corpus and
  would otherwise ship a mirror finish), and `_s.R` does double duty — clamped
  into the physical F0 window anchored on the corpus median, and windowed
  through Levels into a **metal stencil**. Measured across all 6616 vanilla
  materials nothing in a `.bgsm` *classifies* metal better than a coin flip
  (see *RMAOS ← `_s`* below for the four tests), but `_s.R` still has the right
  spatial shape, so it is a starting point the user tunes and then masks rather
  than an answer. Two steps: adjust the Levels window, then paint out what
  should not be metal.
- **Fallout 76** (BGSM/BGEM version 20+): a **true specular workflow** —
  the diffuse is strictly the Lambertian term, so **metals are authored
  with black diffuse** and their color (gold's yellow, steel's grey) lives
  in the RGB `_r` reflectance map. Conversion is the textbook
  specular→metalness reconstruction:
  `metallic` inferred from reflectance intensity joined with diffuse
  near-blackness (a very reliable pair — the diffuse under metal measures
  3.2× darker across 200 materials, holding in 185 of them);
  `baseColor = mix(diffuse, reflectance.rgb, metallic)` — the diffuse fix,
  mirror image of FO4's recovery (FO4 keeps hue and loses brightness to
  the cubemap; FO76 moves hue *and* brightness into `_r`);
  dielectric F0 from reflectance clamped to the physical window;
  smoothness inverts to roughness as a **plain `1 − _l.R`, with no clamp
  and no normalisation** — see *FO76 metallic + roughness* below for the
  900-material survey behind that, and why the treatment `_r` needs is
  exactly the treatment `_l.R` must not get. Dielectric
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
  vanilla texture sets they reference (`_d`, `_n`, `_s`, `_g`). BGSM v2 has no
  env-mask texture slot — the field that looked like one is BGEM's — so there
  is no env mask in the FO4 path.
- **Output**: a **same-name sibling `.pbrm`** at the same relative
  `Materials\` path inside a chosen output mod folder, plus converted PBR
  textures at mirrored `Textures\` paths. The `.bgsm`/`.bgem` files are
  **never modified** — they remain the vanilla fallback, and the FO4CS
  runtime probes the sibling `.pbrm` by path through the game VFS.
- BGSM/BGEM support is a **read-only importer**. The editor remains
  `.pbrm`-only as an editor; it never writes or edits Bethesda materials.

## Texture pipeline

### Base Color ← `_d`

Pass-through for dielectrics (sRGB, alpha preserved for opacity), with **metal
albedo recovery** wherever the `_s.R` stencil says metal. Vanilla metals are
authored dark because the engine composites the cubemap on top, and PBR metal
base color *is* the reflectance color, so recovered texels take **hue from the
diffuse, brightness from spec**:

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

### RMAOS ← `_s` (measured, not assumed)

Everything here is calibrated against the vanilla corpus at
`E:\Tools\Fallout 4\DataUnpacked\Data` — 6616 BGSM, 283 BGEM, 31950 DDS. The
survey is reproducible from the GUI binary:

```bash
"PBR Material Editor.exe" --survey-fo4 <materials> <report.tsv> --texture-root <data> --limit 99999
```

Add `--no-textures` for a scalar-only pass, which answers anything about what
the `.bgsm` declares in seconds rather than ten minutes.

`_s` is **BC5U across 99.8%** of the corpus: two channels, linear, with no blue
and no alpha to read. R is specular strength, G is glossiness.

#### Which channel is which

Not taken from the field name — `sSmoothSpecTexture` lists them in the opposite
order, and getting this backwards swaps roughness and F0 in every conversion.
The `.bgsm` `smoothness` scalar is a fallback (below), so it goes non-default
only when *its own* channel carries nothing. Isolating each channel:

| maps where… | `smoothness` non-default |
| --- | --- |
| only `_s.G` is flat, R still varies | **80.3%** (n=827) |
| only `_s.R` is flat, G still varies | **5.9%** (n=51) — *below* the 20.9% baseline |

The scalar substitutes for G. **G is gloss, R is specular.**

#### The scalars: one falls back, one multiplies

- **`fSmoothness` is a fallback, not a multiplier** — non-default in 80.9% of
  flat-G maps against 20.8% of textured ones. `_s.G` is the gloss where it
  varies; the scalar is the gloss where it does not. Multiplying instead would
  have made 1055 materials with real gloss maps uniformly rougher.
- That matters more than it sounds. Where G is flat its stored level is
  **median 255 — pure white** — while the scalar sits at median 0.50. Vanilla
  writes the gloss channel white and puts the real value in the scalar. Read
  the texel literally and gloss = 1.0 → roughness 0 → **a mirror finish on 827
  materials, 13.4% of the corpus.**
- **`fSpecularMult` genuinely multiplies.** It runs the other way — non-default
  in 29.1% of textured-R maps against 19.3% of flat ones — so it scales `_s.R`
  rather than standing in for it.

#### Roughness

`roughness = 1 − gloss` through the same adjustable curve FO76 has, defaulting
to a plain inversion, with the `.bgsm` scalar substituted wherever G is flat. No
per-material normalisation, for the reason under *Smoothness `_l.R`* below:
roughness is a continuous physical quantity whose absolute value carries meaning
*between* materials.

#### Dielectric F0

`_s.R` is a Blinn-Phong specular intensity with no physical anchor, so the
mapping is anchored on the corpus instead: the **measured median (54/255) maps
to the physical dielectric default 0.04**, and the 0.02–0.08 window clamps the
tails. The top decile (94/255) reaches ≈0.07.

The old `clamp(specR × specMult × 0.08, …)` put that median texel at 0.017 —
*under* the floor — so the whole F0 axis pinned flat at 0.02 and carried no
information at all.

**Percentile spec normalisation is off by default** as of 2026-07-26. It existed
to stop blown-out spec maps converting to mirror chrome, and chrome no longer
comes from a bright texel (below). What remained was a per-material stretch of a
continuous quantity — the thing the roughness rule warns against.

#### Metallic: Fallout 4 does not encode it

This is the finding, and it is a negative one. Four independent tests:

| signal | recall | false-pos | precision | separation |
| --- | --- | --- | --- | --- |
| `_s.R` threshold (best) | 45% | 28% | 53% | +22.5 |
| `_s.G` threshold (best) | 73% | 57% | 47% | +16.2 |
| both jointly | 50% | 31% | 53% | **+18.7** |
| `_s.R × _s.G` product (best) | 62% | 38% | 53% | +24.4 |
| `bEnvironmentMapping` alone | 61% | 19% | 69% | +41.3 |

Scored over ~2750 materials labelled metal or dielectric by vanilla's own asset
naming. The labels are sound: they *bracket* the corpus base rate rather than
sitting on it, which is what says they carry signal rather than noise.

In order:

1. **The diffuse gate that carries FO76 runs backwards here.** Across 5350
   materials the diffuse under top-decile spec is *brighter* — median 95 against
   76, ratio 0.84, agreeing in 26.3% where FO76's holds in 92.5%. FO4 metals
   keep their hue in the diffuse; they are not authored black. Keeping that gate
   removed metal from exactly the texels that carry it.
2. **`_s.R` and `_s.G` jointly are worse than `_s.R` alone.** FO4 is full of
   glossy dielectrics — polished wood, painted brick, wet asphalt, plastic — so
   "reflective and smooth" describes an enormous amount of non-metal. Precision
   sticks at ~53% either way: a coin flip.
3. **Inside the envmap gate the two populations are identical** — metal `_s.R`
   p50 = 64, dielectric p50 = 63 — and separation goes *negative* above 150,
   because very bright spec is more often a dielectric highlight than metal.
4. **The cubemap choice adds a little, and only by name.** `metalchrome01cube_e`
   is 93% metal, but the generic `mipblur_defaultoutside1` carries 1309
   materials at 68% — and the other 32% are brick walls, hair and bloodbug fur.

#### Metallic: a stencil, then a mask

The conclusion above kills `_s.R` as a *classifier*. It does not kill it as a
**stencil**, and that distinction is the whole FO4 metal workflow.

The artist put the panel seams, rivets and wear into `_s.R`. Its *shape* is
right. What it gets wrong is whole **regions** — the brick body, the leather
grip, the wooden stock. Regional errors are two or three fills to paint out,
where building metal up from zero means redrawing every rivet by hand. So FO4
converts metal from `_s.R` and the user subtracts what should not be metal —
the same direction of work as masking a layer in Substance Painter.

That reframing changes what the window is tuned for, and it is easy to get
wrong. Optimising for *decisiveness* alone — as little as possible in the
ambiguous band — trivially favours a high threshold, because most of the corpus
is mostly non-metal. That objective is wrong here. **Because the workflow
subtracts, under-detecting is the fatal failure**: a metal object stencilled at
0% gives the user nothing to paint out. Over-detecting is the cheap error.

Measured over the name-labelled sets, "dead" = a metal material left with under
2% metal, i.e. no usable starting point:

| window | metal dead | median metal area | ambiguous |
| --- | --- | --- | --- |
| 0.35 / 0.45 | **21.0%** | 17.3% | 6.9% |
| 0.25 / 0.35 | 12.5% | 35.4% | 12.5% |
| **0.20 / 0.30** | **7.8%** | **46.0%** | **14.2%** |
| 0.15 / 0.25 | 5.5% | 58.4% | 15.7% |
| 0.20 / 0.45 | 8.0% | 34.7% | 31.8% |

**0.20 black / 0.30 white** is the default — placed low so real metal survives,
kept narrow so what it emits commits rather than smearing. 0.35/0.45 scored best
on ambiguity alone and killed `MetalVerTrimFuturistic01` and `SerratedBlade`
outright.

On real vanilla with this window: SerratedBlade 80%, TireIron 88%,
MetalVerTrimFuturistic01 45%, BrickRed01 50%. The brick is wrong — visibly and
obviously wrong, which is the point. It is one fill to remove.

**`fSpecularMult` is deliberately excluded from the stencil.** It scales the
specular *response*, not the material class, so it belongs on the F0 axis where
it genuinely dims a reflection. `MetalVerTrimFuturistic01` is metal trim whose
`_s.R` peaks at 84/255 against a mult of 0.400; folding the mult in put it at
0.13 and stencilled the material at 0% metal.

The workflow is two steps, in this order:

1. **Adjust the Levels**, per material, against the histogram the node draws
   under its own handles. Drag the black point down for a mostly-metal object,
   up for a mostly-painted one. Setting the black point to 1 converts fully
   dielectric — that is the "no metal here at all" answer, and it holds even for
   a saturated `_s.R` against a `fSpecularMult` above 1.
2. **Paint the correction mask** for what the window cannot separate.
   `applyMetallicCorrection` is 0.5-neutral and bidirectional: below 0.5 forces
   dielectric, above forces metal, strength scaling with distance from neutral.
   Subtracting is the common direction; adding is there for the metal the
   stencil missed entirely.

A material that produces metal and has never been masked is **flagged** — it is
genuinely unfinished work, which is what the flagged walk exists for. Once a
mask exists the material stops being flagged, whether the mask removed metal or
added it.

`cubemapDeclaresMetal()` survives here, but it changes **no pixel**. It orders
the review queue: a material naming `metalchrome01cube_e` is 93% likely to be
genuinely metal, so most of its stencil is probably right and it needs a light
pass; one on the generic sky cube needs a hard look. The 3.1× enrichment is
useful for deciding *what to review first* and useless for deciding what is
metal.

#### Model-space normals

`bModelSpaceNormals` skips the derivations entirely. Curvature, detail AO and
height all read the normal as a *tangent-space gradient field* — the Poisson
solve integrates XY as slopes — and a model-space normal stores object axes
instead, so the chain would produce confident nonsense. Only 3 vanilla materials
set it (all `CryoHumanMale`), but mods use it more; the normal ships unchanged
and a note records the skip.

#### Template chains

51.9% of vanilla materials inherit through a root chain, and
`Template/DefaultTemplate_Wet.bgsm` **names itself as its own root**. A revisited
template is therefore a completed chain, not a cycle: every pass only fills
fields the child left *empty*, so a second lap can contribute nothing by
construction, whether the loop is a deliberate self-reference or a genuine
A→B→A mistake. Treating it as an error flagged roughly 3400 materials over a
template that had already resolved correctly. An unresolvable root is still an
error.

#### Presets, not conversion nodes

**`convert.fo4` and `convert.fo76` are retired — neither node type exists any
more.** Both stock graphs are generic nodes throughout: Separate Maps → Upscale
→ Levels for the metal stencil, Invert for roughness, sRGB→Linear / Mix /
Linear→sRGB for base colour, one Levels for F0, Solid Colour × Math for
emissive.

A registered node that no shipped graph uses is a *second definition of the
same conversion*, sitting one refactor away from disagreeing with the one that
actually runs. The recipes live in the graph **presets** instead
(`defaultFo4Graph`, `defaultFo76Graph`, and anything saved beside them), where
every threshold is on the canvas and tunable per material.

`Converter::convertFo4` / `convertFo76` stay as the batch path — that is the
same maths, not a duplicate of it. The two are pinned to each other in tests:
the preset's stencil Levels window must equal `Options::metalSpecStart/Full`,
and the F0 ramp's out-points must equal the dielectric window. Checked
end-to-end on real vanilla too — TireIron converts 88% metal through the preset
against 88.2% through the batch path.

A graph saved before the retirement still loads; the retired node reports as an
unknown type rather than taking the graph down with it.

One exception, and it is a real one rather than laziness. **Metal Albedo**
(`color.metalAlbedo`) is its own node because rebuilding a metal's colour means
normalising to unit luminance and then scaling to a target brightness, and the
intermediate routinely exceeds 1 — a near-black diffuse divided by its own luma
is enormous. Every image port here is 8-bit, so a Divide followed by a Multiply
clips the hue away in between, and clamping per channel instead of rescaling by
the maximum *flattens* hue, which is a bug the tests already pin.

Two details that would otherwise bite:

- **Auto levels is OFF on both FO4 Levels nodes.** It defaults on, and it is
  right for FO76 — `_r` has a real absolute anchor at the 0.22 dielectric floor,
  so re-deriving per material stays tied to something and a flat map stays flat.
  `_s.R` has no anchor, so auto levels would stretch a brick wall's own spec
  variation across the full range and stencil it as metal every time.
- **`fSpecularMult` reaches F0 through a Solid Colour node** wired from the
  material's scalar port. That scalar bridge is what lets a graph use the
  numbers a `.bgsm` states instead of hard-coding them.

Output BC7 linear, full mip chain, **Toksvig folded from the converted
normal automatically** — the converter always has the paired normal in hand.

### FO76 metallic + roughness ← `_r` and `_l` (measured, not assumed)

Everything in this section is calibrated against the real corpus at
`E:\Projects\F76\Data`, not against physical reflectance tables — which do not
describe this data (steel reads 0.09 linear in `_r` where real steel F0 is
0.56). Both surveys are reproducible from the GUI binary:

```bash
"PBR Material Editor.exe" --survey-r <materials> <report.tsv> --texture-root <data> --limit 900
```

`--survey-gloss` takes the same arguments and measures `_l.R`.

#### Reflectance `_r` → metallic: two conventions, one channel

`_r` ships under **two incompatible authoring conventions**, and which one a
material uses is readable from its compression. Measured over 900 materials:

| Convention | Count | Dielectric floor (median p1) | Metal ceiling (median p99, textured maps) |
| --- | --- | --- | --- |
| BC4, linear | 573 (64%) | 9/255 = 0.035 linear = **0.207** encoded | 53/255 = 0.208 linear = **0.493** encoded |
| BC1, sRGB | 326 (36%) | **56/255 = 0.22**, in 273 of 326 (**84%**) | **179/255 = 0.70** |

0.22 sRGB is not arbitrary: it is dielectric F0 0.04 linear, encoded. BC4 is a
*data* format and stores the same physical floor without the transfer, which is
why the two sets of numbers look nothing alike while meaning the same thing.
`normaliseReflectanceToSrgbScale()` in the source node applies the transfer and
stretch to BC4 maps up front, so one downstream window serves both.

The ceilings are quoted over **textured** maps for a reason: **481 of the 573
BC4 maps (84%) are flat** — a constant dielectric value with no metal in them at
all. BC4 is not merely a second encoding, it is largely the format chosen when a
material has no metal to describe. Only 92 materials in 900 use BC4 with real
metal variation, so a ceiling averaged over all BC4 maps reads 9/255 and means
nothing. BC1 is the opposite: only 2 of 326 are flat.

The Levels black point sits at **0.25**, not on the 0.22 floor. BC1 is 4bpp and
quantises hard: a median **10.2%** of every map's texels smear into 57–64. A
black point *on* the floor lets that smear through as low-level metal — the
"everything is slightly grey" failure. The 0.03 of headroom is what clamps it.
The white point is **0.70** because the median map's p99 lands there, so its
brightest metal reaches *full* metal; at 0.75 it stalls near 90%, inside the
band a PBR validator flags as ambiguous.

Auto levels (on by default) re-derives this window per material and snaps to a
**0.05 ladder** — deliberately coarse, so two near-identical maps cannot resolve
to 0.70 and 0.71 and convert differently.

#### The diffuse gate

Metal is confirmed by a second, independent signal: metals are authored with
near-black diffuse. Across 200 materials containing both populations, diffuse
under metal is **3.2× darker** (median 17.8 vs 64.3 of 255), holding in 185 of
the 200. The window is inverted and sits in the gap — 0.20 black, 0.11 white —
deliberately generous toward metal, because this gate can only ever *remove*
metal: a false negative destroys a real metal surface outright, while a false
positive still has to survive the reflectance signal it is multiplied with.

#### Smoothness `_l.R` → roughness: **plain inversion, no normalisation**

Surveyed 2026-07-25 over 900 FO76 materials. `_l.R` does **not** use its full
range — only **0.8%** reach both black and white; median p1 = 32/255, median
p99 = 197/255. That looks like the `_r` problem, and it is not.

The distribution settles it. `_r`'s floor is a **spike**: 83% of BC1 maps at
exactly 56. `_l.R`'s ceiling is a **broad bell**, mode 180–199, tailing smoothly
down to 60. A spike is an encoding convention; a bell is a thousand authoring
decisions. Nothing here is being lost in encoding.

**Rule: `roughness = 1 − _l.R`, with no clamp and no per-material stretch.** Two
reasons, and the second is decisive:

1. A compressed range is a legitimate answer. A map peaking at 197 says "the
   shiniest part of this material is roughness 0.23" — glossy, entirely
   plausible for painted metal or leather. Where `_r`'s compression broke
   something concrete (a metal *mask* that never saturated, so nothing was ever
   fully metal), roughness is continuous and 0.23 is a value, not a failure.
2. **Per-material normalisation would destroy cross-material contrast.** Auto
   levels stretches each material independently. Applied to roughness, a matte
   cloth and a polished helmet would both be stretched to full range and end up
   with equally sharp highlights. Roughness's absolute value carries meaning
   *between* materials in a way a metal mask does not.

That asymmetry is the general rule worth carrying forward: **normalise
classifications, never continuous physical quantities.** Reflectance feeds a
mask that must saturate; roughness is a measurement.

The stock graph is therefore an Invert and nothing else. A gloss-curve node used
to sit after it at `pow(x, 1.0)` — the identity — as somewhere to dial a curve
in, but a node that computes nothing is clutter, and the curve already has two
homes that do something: `pbrm convert --gloss-curve` and
`convertercore::glossToRoughness`. "No curve" is best expressed by there being
no node.

**Known open point.** The `.bgsm` `smoothness` scalar is *discarded* whenever an
`_l` texture exists — `convertercore.cpp` sets it then overwrites it with the
texel, and the graph does the same. 94.3% of materials have it at exactly 1.000
so it is nearly always moot, but **51 of 900 do not**, including 8 sitting at
0.100. Bethesda scalars are usually fallbacks rather than multipliers, so the
current behaviour is probably right, but this has not been confirmed against the
FO76 shader or an in-game A/B.

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

### Subsurface

FO76 authors a translucency block (v≥8) and FO4 a simpler subsurface-lighting
pair. Both select the Advanced Subsurface family, and both carry values that
must reach `Shading/Subsurface` — selecting the family alone converts skin as a
subsurface material with *default* scattering, which is what the pipeline used
to do.

| Source field | `.pbrm` | Note |
| --- | --- | --- |
| `bTranslucency` + `bTranslucencyThickObject` | `mode` | thick → `Combined`, thin → `Thin Transmission` |
| `fTranslucencyTransmissiveScale` | `transmission` | clamped 0–4 |
| `translucencySubsurfaceColor` | `profile` = `Custom` + `customScatterColor` | the authored colour *is* a wavelength tint; a default white one carries no information, so the stock profile stays |
| `bSubsurfaceLighting` (FO4) | `mode` = `Diffusion Profile` | |
| `fSubsurfaceLightingRolloff` (FO4) | `weight` | normalised against its own 0.3 default, so an untouched material lands on neutral weight |
| `bTranslucencyMixAlbedo` | — | flagged: runtime blends scatter colour with albedo |
| `fTranslucencyTurbulence` | — | flagged: no editor equivalent |

Vanilla `BaseHumanFemaleSkin.bgsm` converts to `Combined` / `Custom` /
`#ff0000` / `0.7` — red scattering furthest through flesh, as expected.

Source Material exposes **Subsurface Colour** and **Transmission** ports, which
report exactly what the conversion will write.

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
- **Center** — the **node graph** (below), which replaced the flat options
  form: every conversion setting now lives on the node it belongs to.
- **Right** — the shared preview viewport showing the *converted* result
  live; hosts the reference NIF and the mask paint/fill tools. It also
  renders the **native** source material (see *Native views*).
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

### The conversion node graph

The pipeline above is exposed as a node graph (`nodegraph.h` core,
`converternodes.h` library) rather than a fixed sequence of options. The
stock FO4/FO76 graphs reproduce the orchestrator's writes byte for byte;
users rewire copies of them. `pbrm convert --graph <file.pbrgraph>` runs
the same graph headlessly.

Nodes follow Blender's body layout, top to bottom: **header → preview →
outputs → settings → status → inputs**. Consequences worth knowing:

- **Settings are edited in the node**, not only in the side inspector.
  Drag a number to scrub it, click it to type, click a checkbox to toggle,
  click an enum for its menu. A drag re-evaluates once on release, not per
  mouse move.
- **Box geometry lives in `NodeGraph::Metrics` / `nodeSize()`**, and
  `layoutGraph()` reflows a graph into dependency columns from those
  sizes. Anything that positions nodes must go through them — hard-coded
  spacing is how stock graphs previously drew on top of each other. *Arrange
  Nodes* in the canvas context menu re-runs the layout by hand.
- **Every node reports its resolution**, and texture sources report the
  compression too (`2048×2048 · BC7_UNORM_SRGB`), read from the DDS header
  so it stays truthful while the preview evaluates downsampled.
- **Under-resolution is flagged.** `EvalContext::materialMaxResolution` is
  the largest authored texture in the material; any node whose *native*
  resolution falls below it draws its status line red plus an amber warning.
  Native resolution propagates from sources downstream and is independent of
  the preview downsample, so previewing at 1024 does not flag the world.
- **Upscale** (`image.upscale`) raises an under-sized map. Its target is
  expressed against the *material's own* highest-resolution map — *Match
  highest*, *2×*, *4×* — so one rule set once holds for a whole batch whatever
  size each material is; *Fixed size* and *Off* override it, and the project
  setting on the canvas toolbar drives every node left on *Project setting*.
  Two engines behind one call (`upscaler.h`): **Real-ESRGAN** (ncnn/Vulkan) when
  a binary is configured under `Upscaler/binaryPath`, and **Lanczos-3**
  otherwise. It never fails — a missing binary or unusable model degrades to
  Lanczos and says why in its status. Which model runs comes from the map's
  **role** (below); *Auto* reads the role off the upstream port's label, which
  is how a graph states what a map is without a second declaration, and the
  node's Model setting overrides the standard for one map without changing it.

#### Standard models per map role

`Settings ▸ Upscaling ▸ Standard Models…` pins one model per role. Roles are
deliberately finer than the handling classes behind them: **handling** decides
whether blue is zeroed and rebuilt, **role** decides which network runs, and two
roles sharing a handling class can still want different models.

| Role | Handling | Why it is its own row |
| --- | --- | --- |
| Base Colour / Diffuse, Glow | picture | Photographic content; a photo-trained model is appropriate. |
| **Reflectance (`_r`)** | picture | **The metal window is derived from this histogram.** A model that invents detail here changes what converts as *metal*, not merely how it looks — so it never inherits base colour's choice. |
| Normal | RG0 in, Z rebuilt | Blue is dropped going in (the normal models are trained on RG0) and rebuilt from RG coming out. With no normal-trained model installed it declines outright and resamples: a photo model bends the vectors in ways `reconstructBlue` cannot undo. |

**Normals do not come back as unit vectors, and must be made into them.**
`4x-Normal-RG0-BC7` states it plainly — *"do not rely on this network producing
unit vectors"* — and it is not alone. `reconstructBlue()` therefore does two
things, not one: where XY is over-long it **rescales XY onto the unit circle**
before solving for Z, rather than clamping Z to zero and shipping a normal
longer than 1. Clamping alone looks like it works (no NaN, plausible image) and
is a real shading error at every affected texel.

Getting that model in is a **format** problem, not an architecture one. It is
ESRGAN/RRDBNet, which is exactly what `realesrgan-ncnn-vulkan` runs — but
OpenModelDB publishes `.pth`, and ncnn loads `.param`/`.bin`, so it needs
converting (`.pth` → ONNX → ncnn) or a runner that reads PyTorch weights
directly. Keep the file named with `normal`/`rg0` and its `4x`: the suggestion
matcher and `modelNativeScale()` both read the name. One mismatch worth knowing:
it is trained on **BC7** normals while FO76 ships **BC5**, so its artifact model
is off-distribution — BC5 carries no blue at all, which suits RG0 input, but the
compression it learned to undo is not the compression in front of it.
| Specular/Gloss, Smoothness, AO, Subsurface, Other | data | Not pictures. They default to a *general-purpose* model, never a photographic or anime one. |

Unpinned roles fall back to `suggestedModelForSlot()`, which matches by name
against whatever is installed. That matching is pure string work and it has
already been wrong once in a way that looked right — a "conservative" hint meant
to steer data maps away from photographic models matched `Photo-Conservative-x4`
and sent every data role to a photo model. `chooseModel()` is therefore exposed
separately from the installed set so the ordering is tested against a fixture on
machines with no upscaler at all.

A pinned model that is missing from the current install falls back to the
suggestion rather than invoking a network that is not there. Pinning *Lanczos*
is a real choice and is stored as one, distinct from never having chosen.
- **Levels** (`image.levels`) is the remap the FO76 inference is built on, and
  the only node that draws the input distribution under its own handles — which
  is the point, since these thresholds decide what counts as metal and picking
  them blind against invisible data is how everything ended up grey. It takes a
  **signed span**, so a descending signal needs no Invert. **Auto levels** is on
  by default: it classifies the histogram and snaps the result to a 0.05 ladder,
  deliberately coarse so two near-identical maps cannot resolve to 0.70 and 0.71
  and convert differently. It preserves an inverted direction rather than
  "correcting" it — the diffuse gate is deliberately inverted, and auto-levels
  reasoning ascending once silently flipped it and converted steel at 0% metal.
  See the calibration section above for where the numbers come from.
- **Separate Channels** (`image.separate`) splits a texture into R/G/B/A in
  one node; **Combine Channels** (`image.channelPack`) is its inverse.
- **The graph runs between two whole-material nodes.** *Source Material
  (Fallout 4)* / *Input Material (Fallout 76)* open a game material up;
  **Output Material (PBR)** closes a converted one. There is one node of each per graph.
  (*Source Material (Any Profile)* also exists, so graphs saved before the split
  still load.)
- **Source Material exposes the whole material**: one path output per texture
  slot *that its own title uses*, plus emittance/specular colour and the
  emittance/specular/smoothness scalars. Each refuses the other title's
  material rather than handing downstream nodes textures that mean something
  else. Drag a path onto a **Texture** node's Path input to load that slot, or a
  colour onto **Solid Colour** for the slots a material states as a value rather
  than a map (glow colour on an emissive with no glow map). Outputs that
  evaluate empty — unauthored slots — draw dimmed.
- **Output Material (PBR)** is the sink: everything ends there. Channels can arrive
  packed (`RMAOS` straight from a conversion node) or individually
  (`Roughness`/`Metallic`/`Ambient Occlusion`/`Specular F0`), and an individual
  input always overrides the packed channel it belongs to — which is how the
  derived detail AO lands in RMAOS.B and the derived curvature in Normal.A
  without disturbing anything else. Feeding the whole source normal in makes
  the old "normal B defaulted to white" class of bug structurally impossible,
  and collapsed ten Extract/Combine nodes out of both stock graphs.
- **Selecting either end shades a whole material**: a Source Material node
  gives the untouched original in its native view, Output Material (PBR) gives the
  converted result. Before and after, one click apart.
- **Resolution levelling**: a Texture node's Resolution setting defaults to
  *Material max*, so every map works at the material's largest size; *Native*
  and *Custom* override it. **This changes conversion output** for materials
  with mixed source resolutions — the batch orchestrator path does not level.

Canvas navigation, since a stock graph is past twenty nodes and none of these
are discoverable by looking:

- **`K`** arms the knife; drag across wires to sever them, `Esc` or `K` to exit.
  Wires go red before they are cut, so a stroke can be corrected mid-drag.
- **`Ctrl+F`** finds a node by name — every match rings, `Enter` walks them.
  **`F`** fits the graph, **`Shift+F`** frames just the selection.
- **The minimap** in the bottom-right corner is clickable and draggable to jump.
- Below the detail zoom threshold node bodies stop drawing their settings, and
  titles switch to a **fixed screen size** so the overview stays readable
  instead of becoming a wall of grey boxes. The zoom is reported bottom-left.
- Selecting a node golds **both ends** of every wire touching it.
- **Maths**: `math.op` (add/subtract/multiply/divide/power/min/max/abs/sqrt/
  floor/ceil/round/modulo/greater/less, against a second texture or a
  constant), `image.mix` (optionally mask-driven), `image.mapRange`.
- **Colour space**: `color.srgbToLinear` / `color.linearToSrgb` use the exact
  piecewise sRGB transfer from `convertercore`, not a 2.2 gamma approximation,
  so they agree with the conversion math.
- **Image File** loads your own DDS/PNG/TGA/JPG/BMP/TIFF.

### Native views

The preview can shade the **untouched source material in its own model**,
selectable as render modes *Native FO4 (Specular/Gloss)* and *Native FO76
(Reflectance)*. Put one on each half of the split view to judge a conversion
directly against what it came from. Channel meanings match what the
converter reads — FO4 `_s.R` specular strength / `_s.G` gloss; FO76 `_r.RGB`
reflectance, `_l.R` smoothness, `_l.G` baked AO — and both are lit by the
same key and environment as the PBR view, so only the material model differs.

`--graph-shot <png> [--fo76]` and `--native-shot <material> <png>
[--texture-root <dir>]` render the stock graph and the native views to a file
without driving the workspace; they exist so layout and shading changes can be
checked directly.

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
material: outputs incl. written texture list, flags, notes, errors, metal
coverage, source mtime, options signature). Options: `--texture-root`,
`--dry-run`, `--json`, `--force`, `--height-in-blue`, `--no-detail-ao`,
`--metal-diagnostic`, `--recovery`, `--gloss-curve`, `--derivation-size`.
**Incremental + rollback shipped**: unchanged (source mtime + options
signature + sibling exists) materials skip on re-run; `--force` overrides;
`pbrm convert --out X --clean` removes exactly the recorded outputs (path
containment enforced, foreign files untouched, empty dirs pruned).
Texture-free constant-color BGEMs convert to constants-only siblings
(overrideColor/opacity), not failures.

**Shipped 2026-07-23 (items 1–4 of the campaign backlog):**
- *Review loop*: Flagged ◀/▶ queue walk; per-material overrides (recovery,
  gloss curve, spec clamp percentile) with Reconvert This.
- *BA2-direct*: `convert` accepts `.ba2` targets (v1/v7/v8); textures
  auto-resolve from `*.ba2` beside the texture root; root chains probe
  archives; incremental skips key on the archive timestamp.
- *Correction masking* (`src/maskpaint.*`): Load Reference NIF → Pick fills
  (Triangle / position-welded Element / Submesh × Force Dielectric / Metal /
  Back to Inferred) → Save Mask sidecar under `<out>/Masks/`, attached to
  the material's conversions; CLI `--correction-mask`. CPU raycast picking
  and amber fill highlight in the shared preview. Freehand projection
  painting remains a later enhancement.
- *Geometry AO* (`src/geometryao.*`): CPU depth-map bake (UV G-buffer +
  sphere of ortho depth renders — the spec's algorithm without the GL
  context, so it runs headless), UV-island analysis with stacked-duplicate
  canonicalization, edge dilation; multiplies into RMAOS.B alongside detail
  AO for both titles. `pbrm bake-ao` standalone; the workspace's loaded
  reference mesh drives the bake on reconvert (x01 test mesh: 100 islands,
  64 dirs, 512² in ~1 s). Live-verified 2026-07-23 on FO76 (alien dielectric 0%,
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
