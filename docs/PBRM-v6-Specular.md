# PBRM v6: specular weight, specular IOR, normal alpha selector

Ruling: bungo with DarkStar [CSE], 2026-09-13 (Discord), ratified with Claude
2026-09-16 19:42. This page is the contract for the v6 schema bump. It is the
input to the editor lane (this repo) and to the FO4CS runtime lane, which
builds last by standing order. Until the lane lands, `docs/PBRM-v5.md` still
describes what ships.

## 1. What changes, in one table

| Socket / field | v5 | v6 |
| --- | --- | --- |
| `primaryRmaos` A | dielectric F0, consumer clamps to 0.16 | OpenPBR `specular_weight`, linear 0..1, texel 255 = 1 |
| `primaryRmaos` constant `f0` (default 0.04) | scalar F0 | renamed `specularWeight`, default 1 |
| `primaryRmaos.alphaCarries` | `"Dielectric F0"` / `"Porosity"` | `"Specular Weight"` / `"Porosity"` |
| `primarySpecularColor` RGB | sRGB tint on the dielectric Fresnel (doc row wrongly said linear) | unchanged, doc row corrected to sRGB |
| `primarySpecularColor` A | tint coverage weight | `specular_ior` map, linear 0..`specularIorMax`, texel 255 = `specularIorMax`; read only when `iorInAlpha=true` |
| `primarySpecularColor.iorInAlpha` | — | bool, default false |
| material `specularIor` | — | float, default 1.5, range [0, inf). Used when no IOR map is read |
| material `specularIorMax` | — | float, default 4.25, range (0, inf). What texel 255 of the IOR map means |
| `primaryNormal.curvatureInAlpha` | bool | retired |
| `primaryNormal.alphaCarries` | — | `"None"` / `"Curvature"` / `"Cone Map"`, default `"None"` |
| `schemaVersion` | 5 | 6 |

The 0.16 conductor guard on the authored scalar is REMOVED, in the preview and
in the runtime. The level now lives in the IOR and the weight can only lower
it; the guard has no operand left.

## 2. Arithmetic

    F0_ior     = ((ior - 1) / (ior + 1))^2
    tint       = decodeSrgb(specularColor.rgb)            (1,1,1 when the slot is absent)
    F0_diel    = specularWeight * tint * F0_ior           (per channel)
    F0         = lerp(F0_diel, baseColor, metallic)       (metals untouched, as before)

`ior` is the map texel * `specularIorMax` when `iorInAlpha` is true and the slot
resolves, otherwise the material scalar `specularIor`. The map is never
multiplied by the scalar (Option A: texture replaces scalar).

`specularWeight` is the RMAOS alpha texel when `alphaCarries="Specular Weight"`
and `overrideSpecularWeight=false`, otherwise the constant. A missing map or a
missing alpha channel is weight 1: the surface gets the full F0 the IOR gives it.

Surface states keep blending the RESOLVED F0 (after weight, tint, IOR and the
metal mix) toward their targets exactly as in v5. Nothing in the state system
reads weight or IOR.

Secondary and tertiary layers: their RMAOS alpha is a weight too, with the same
selector and override fields. They have no specular color slot and no IOR map;
they use the material's `specularIor`.

## 3. IOR landmarks at the default ceiling 4.25

| IOR | Texel | F0 (weight 1, white tint) | Note |
| --- | --- | --- | --- |
| 0 | 0 | 1.00 | tint becomes F0 outright |
| 1 | 60 | 0.00 | no dielectric specular |
| 1.33 | 80 | 0.02 | water |
| 1.5 | 90 | 0.04 | default; EXACT on 8 bits (1.5/4.25*255 = 90) |
| 2.0 | 120 | 0.11 | |
| 2.42 | 145 | 0.17 | diamond |
| 4.25 | 255 | 0.38 | germanium, the ceiling |

Texels 0..60 run backwards (F0 falls from 1 to 0, then climbs). Known and
accepted; only the IOR-0 case uses it. Raising `specularIorMax` rescales the
whole map (at 8.5 the default 1.5 sits on texel 45) and coarsens every step
below, which is why the ceiling is a per-material override and 4.25 ships.

## 4. Migration v5 -> v6

Constants, in memory on load, exact:

    w   = min(f0 / 0.04, 1)
    ior = 1.5                      if f0 <= 0.04
    ior = (1 + sqrt(f0)) / (1 - sqrt(f0))   if f0 > 0.04   (then w = 1)
    curvatureInAlpha=true  -> normal alphaCarries="Curvature"
    curvatureInAlpha=false -> normal alphaCarries="None"

Textures cannot migrate in memory. They raise validation findings with quick
fixes (editor) and are handled by `pbrm migrate` (CLI):

- v5 RMAOS alpha carrying F0: repack alpha as weight with the rule above per
  texel; texels above 0.04 need an IOR texel, written into the specular color
  alpha (a white sRGB color map is created if the slot is empty) with
  `iorInAlpha=true`.
- v5 specular color map whose alpha was a tint coverage: bake
  `lerp(1, rgb, a)` into RGB, set alpha to texel 90 (IOR 1.5), `iorInAlpha=true`.
  Equivalent by construction, since v5 resolved the tint as exactly that lerp.

A v5 document with constants only renders identically after migration; that is
a test pin. A v6 file is refused by a v5 reader (schemaVersion gate, as before).

## 5. Runtime half (FO4CS lane, later)

- `res/Lighting/deferred_prepass.hlsl` ~854-867: `f4fxF0Scalar` becomes the
  weight; drop `min(., 0.16)`; multiply by `F0_ior` from the new constants and
  by the tint when the color map is bound.
- Constant buffer: two new floats, `specularIor` and `specularIorMax`. The v5
  note "the color slot needs no new constant-buffer space" is void.
- Feature bits: 7 `RmaosF0` renamed `RmaosSpecularWeight` (condition: bit 3,
  `overrideSpecularWeight=false`, `alphaCarries="Specular Weight"`); 9
  `NormalCurvature` condition becomes `alphaCarries="Curvature"`; NEW 26
  `SpecularIorTexture` (bit 25 and `iorInAlpha=true`); NEW 27 `NormalConeMap`
  (bit 1 and `alphaCarries="Cone Map"`), reserved, no consumer yet.
- PREREQUISITE: the glossy-diffuse energy split (OpenPBR alignment item A2,
  `ambient_ibl_pass.hlsl` ~1086/3587). With the guard gone, a dielectric can
  legally sit at F0 0.38 with metallic 0, and without the split its diffuse is
  double-counted. The split lands before or with this change, not after.

## 6. Normal alpha selector

`"Cone Map"` reserves the alpha for a relaxed-cone-step parallax map baked from
the height that sits in the same texture (`heightInBlue=true` required; the
validator flags Cone without it). Neither the preview nor the runtime marches
cones yet; both treat Cone as "alpha carries no curvature" and derive
curvature from the normal as they already do for a material without one.
Cone-step marching is a later lane on both sides.

## 7. Editor lane work list

1. Format doc: fold this page into `docs/PBRM-v5.md` and rename it
   `docs/PBRM-v6.md`; fix the color row to sRGB.
2. Document model: version 6, field renames, three new material fields, the
   normal selector, constant migration on load.
3. Preview shader: section 2 verbatim; guard removed; layers; IOR map read.
4. UI: RMAOS alpha row relabelled Specular Weight (0..1); IOR-in-alpha
   checkbox on the color slot; Specular IOR and IOR Map Max spins on the
   material; normal alpha selector combo. Labels only, no descriptions.
5. Validation quick-fixes and `pbrm migrate` per section 4.
6. Converter: FO4 `_s.R` written raw as weight, the level anchor moved into
   `specularIor` (decide the anchor in-lane, state the method); FO76 keeps its
   route with weight + IOR replacing the F0 write.
7. Tests: round trips, exact constant migration, the texel-90 and IOR-0
   landmarks, layer blend, identity pin for a constants-only v5 document.
8. Build root, then `cli/`, then `tests/` (three qmake targets; timestamps
   must agree), run the suite, `scripts\package.ps1`, hash-verify, commit and
   push main.
