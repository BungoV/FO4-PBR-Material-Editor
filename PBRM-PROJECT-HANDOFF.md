# Fallout 4 Community Shaders PBR / PBRM handoff

Date: 2026-07-15  
Status: editor-side work is complete and published; direct PBRM v4 loading and the shared Community Shaders fallback cubemap are implemented, built, and deployed to the active MO2 test setup. In-game validation remains.

## Goal and scope guardrails

The current goal is to complete and validate the Fallout 4 PBR implementation. Dynamic cubemaps are the next phase, not the current one. Do not drift into unrelated renderer work before the PBR path is physically coherent in sunlight, shadow, and interiors.

The new material format is `.pbrm`. It is a prototype and may deliberately invalidate earlier `.pbrm` files. The user explicitly accepts breaking format changes during this phase.

The new editor is intentionally `.pbrm`-only. Do not restore BGSM/BGEM parsing or editing unless the user reverses that decision. BGSM/BGEM compatibility belongs in the future runtime resolver/Creation Kit compatibility path, not in the new editor UI.

Only one test NIF in the PBR mod is deliberate: `meshes\actors\powerarmor\x01\x01_torso.nif`. Do not interpret that as a missing bulk-conversion task.

## 2026-07-15 current verified state (supersedes stale status below)

- Canonical editor source is `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt`.
  The portable executable is `publish\PBR Material Editor.exe`; SHA-256
  `59C2F4027913AF605D201666F804852E5CE890094C1D5C3D736D67F0F97BA64D`.
- The editor suite passes 149 checks with zero failures. Texture selection no
  longer opens an automatic companion prompt; each texture slot has an explicit
  `Find Set` action, and the texture browser uses collapsible real folders.
- Community Shaders directly loads versioned PBRM v4 resources through
  `BSResourceNiBinaryStream`, caches parsed runtime materials, creates only the
  internal Fallout renderer bridge, and binds immutable per-material constants.
  PBRM does not inherit from or fall back to BGSM/BGEM.
- Forward compatibility is capability- and ABI-gated. Unknown optional data is
  ignored/preserved; unknown or newer required capabilities fail closed;
  incompatible semantic changes require a new PBRM version and migrator.
- `[TruePBR] sDefaultCubemap` now owns the interim runtime environment fallback.
  Default: `Shared\Cubemaps\mipblur_DefaultOutside1.dds`, relative to
  `Data\Textures`; an empty value disables it. Fallout's assigned probe always
  wins. Only no-probe PBR pixels sample the shared `TextureCube` at ambient t20.
- Native Community Shaders build passed. Focused TruePBR/config and lighting
  tests pass. The full host run reached 833 cases: 830 passed; the three failures
  are unrelated post tests whose checked-in `res/LUTs/neutral_32.dds` fixture is
  absent. Both single- and dual-accum PBR ambient permutations compile as
  `ps_5_0` with FXC.
- Active MO2 deployment is `E:\Projects\Fallout 4 Mods\mods\B21_F4FX`.
  Deployed DLL SHA-256:
  `BA56CD5787302CCE6CA8AE69882C9018A63F7BE35F4A3F9FD175B26339A9960C`.
  Deployed ambient shader SHA-256:
  `4F73020711258BE500F9D9830A18F3C536BA0F398EADF9B31967C4B4C914A26D`.
  The customized MO2 INI was preserved and only the fallback key/comments were
  added. One-time `.bak-pre-default-cubemap` backups exist beside all three
  deployed files.
- Next required step is an in-game smoke test: confirm the log reports
  `[TruePBR] default cubemap ready`, then compare a valid-probe and no-probe PBR
  mesh in sun, cast shadow, and an interior. Dynamic cubemaps remain later work.

## Important paths

### Community Shaders runtime

- Repository: `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders`
- Current branch according to `.git/HEAD`: `main`
- Recorded ref at handoff time: `ddd162fc2af68e0a0d8dbf32458b84a5409f6c21`

Critical Git warning: this repository's `.git/index` is missing while `.git/index.lock` exists. Git consequently reports the tracked tree as deleted and the working files as untracked. Do **not** commit, reset, clean, checkout, or otherwise normalize this tree until the index is safely restored/rebuilt while preserving the current files. The PBR implementation is in the working tree and is not represented by the recorded `main` commit.

### New C++ material editor

- Canonical working copy for this task: `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt`
- Deployed source: `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt`
- Deployed portable application: `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt\publish\PBR Material Editor.exe`
- User-facing portable package: `C:\Users\bungo\Documents\Codex\2026-07-14\checkbox-resource-fix-is-deployed-with\outputs\PBRMaterialEditorQt`

### Assets and tools

- PBR test mod: `E:\Projects\Fallout 4 Mods\mods\PBR`
- Wild Wasteland NifSkope fork: `E:\Projects\ClaudeNifskope`
- Legacy C# editor (superseded; reference only): `E:\Projects\Fo4CommunityShaders\PBRMaterialEditor`

## Decisions already made

### NIF material resolution concept

The intended eventual resolver supports both workflows:

1. A NIF may directly reference `foo.pbrm`.
2. A PBRM-enabled NIF references `foo.pbrm` directly. Community Shaders owns
   that load and does not look for, inherit from, or require `foo.bgsm`/`foo.bgem`.
3. BGSM/BGEM references remain an independent vanilla/legacy path and are never
   interpreted as implicit PBRM references.
4. Creation Kit support will require tooling that authors or patches direct
   `.pbrm` root names; same-name interception is not part of the format contract.
5. A CK/F4SE plugin or equivalent integration can be investigated later; it is not part of the editor rewrite.

This resolver and parser are now implemented; see the superseding verified-state section above and `fallout4-community-shaders\docs\PBRM-RUNTIME.md`.

### One format, multiple shaders

There is one `.pbrm` extension rather than separate surface/effect extensions. Behavior is selected by the shader field:

- Standard
- Advanced Subsurface
- Vegetation
- Hair
- Eye
- Landscape
- Glass
- Water
- Unlit Effect
- Simple Lit Effect
- Physically Lit Effect
- Advanced

`Advanced` is the authoring/validation superset formerly called `All Features`; legacy files using the old name remain readable.

### Texture/channel conventions represented by the editor

- Base Color / Opacity: sRGB RGB color, A opacity/coverage.
- Tint Mask: linear RGBA, four independently tintable regions in the Warframe-style recoloring concept.
- Normal + Height: linear R/G tangent normal, optional B height, A layer-blend modulation. Normal-only maps should be BC5; normal+height maps should use a four-channel format such as BC7.
- RMAOS: linear R roughness, G metallic, B ambient occlusion, A dielectric F0/specular. Alpha is optional; fallback F0 is `0.04`.
- Emissive: sRGB RGB radiance, A intensity mask.
- Subsurface Scattering / Transmission: RGB scatter/transmission tint, A thickness. The former FO4 inner-layer concept is folded into this family rather than exposed as a separate ordinary map.
- Surface Effects: R sheen/fuzz, G glints, B normalized thin-film thickness, A iridescence mask.
- Clearcoat is a Secondary UV feature: RG clearcoat normal, B clearcoat roughness, A coverage. Clearcoat height was explicitly removed.
- Secondary Gradient: RGB gradient color and A gradient coverage; alpha must participate in final layer coverage.
- Secondary Layer Coverage: separate R coverage when normal alpha or base alpha is not suitable.
- Screen Distortion is the authoring name for the old effect/refraction-style screen offset.
- Landscape uses dedicated texture arrays and hides ordinary Primary/Secondary UV pages.

Secondary UV can be independently transformed/animated or configured to mirror the Primary UV layout. Broad layer coverage may come from a selectable vertex-color channel, then be combined with texture-authored high-frequency breakup. The intended use is an overlaid second material/decal layer within the same draw rather than an extra decal material draw.

## New material editor: completed work

The application was rewritten independently from scratch in C++20/Qt 6. It does not reuse the old C# editor implementation.

### Real, functional pieces

- `.pbrm` version-4 binary envelope load/save with atomic writes.
- Real document editing for shader, UV texture inputs, packed-channel toggles/fallbacks, settings, and specialized resources.
- Undo/redo and modified-file prompting.
- Primary UV and optional Secondary UV authoring.
- Dedicated Landscape mode with UV pages hidden.
- Per-texture enable, collapse, browse, clear, copy, paste, and reset.
- Whole-UV-layer copy, paste, and reset, including transforms, animation, and composition.
- Global cross-page search with direct navigation.
- Default, override, runtime, and invalid state badges.
- Texture diagnostics: resolution, compression format, color space, available channels, mip count, file size, and resolved path.
- DDS/BC1-BC5 plus ordinary-image thumbnails and RGBA channel isolation. BC7 metadata is diagnosed even where thumbnail decoding depends on Qt image support.
- Project-native color picker with previous/current swatches, HSV field, RGB/HSV/alpha fields, hex entry, presets/custom palette, and screen eyedropper.
- Blender-style numeric fields: hover arrows and horizontal dragging; Shift is fine and Ctrl is coarse.
- Material templates for common metal, painted metal, plastic, glass, skin, vegetation, hair, water, effect, and landscape starts.
- Full-screen startup and a compact Wild Wasteland NifSkope-inspired dark UI.
- White checkmarks on checked controls. The latest fix uses `resources/check.xpm`, not the SVG, because the MSYS2 Qt installation lacks the SVG image plugin. Do not revert the QSS or texture-input icon to `check.svg` without deploying Qt SVG support. The post-fix package compiled and deployed, but there is no automated GUI-pixel test; visually confirm the tick in the next launch.
- Live OpenGL 3.3 preview on a camera-facing plane, cube, sphere, or imported
  static `.fbx` mesh. Static import uses vendored MIT-licensed `ufbx`,
  triangulates, centers, normalizes, diagnoses the import, supports UV0/UV1 and
  submesh selection, and generates missing normals/tangents. Skinning and
  animation are deliberately ignored.
- Wild Wasteland NifSkope viewport controls: MMB orbit, Shift+MMB pan, RMB
  dolly, wheel zoom, Shift+LMB key-light rotation, F frame, Numpad 1/3/7 axis
  views, and Numpad 5 perspective/orthographic toggle.
- Independent key light and generated studio cubemap controls, including
  rotation, intensity, temperature/custom color, exposure, Studio/Direct
  Sun/Outdoor Shadow/Interior/Neutral Validation presets, five backgrounds,
  ground/contact shadow, and current-vs-default split comparison.
- Preview debug modes for base color, normals, roughness, metallic, AO, F0,
  emissive, UV checker, wireframe, and tangent basis. Preview setup persists in
  editor preferences and is never written into `.pbrm`.

### Preview limitations

The OpenGL preview evaluates core Primary and Secondary UV textures, positive
channel overrides, UV transforms/animation, shader-space layer composition,
base/gradient alpha sources, alpha test and blend composition, material IBL
scales/rotation, emissive, and Offset/POM/Self-Shadowed POM.

Specialized clearcoat, subsurface/transmission, surface effects, iridescence,
true displacement, dynamic normals, decals, hair, physical glass refraction,
water, vegetation motion, eye shading, landscape arrays, and screen distortion
remain authoring-only. The preview title explicitly identifies that limitation
for specialized shaders rather than implying those algorithms are simulated.

Selected-NIF-mesh preview is still future NifSkope integration. The standalone
editor now accepts user-selected static FBX meshes, but does not import NIF or
render skinned/animated geometry.

### 2026-07-15 material-editor UI and preview pass

- Sidebar texture pages are now `Primary UV Textures` and `Secondary UV Textures`; transforms, address modes, animation, composition, and controller bindings moved to `UV Mapping`.
- Overview, Validation, and Authored Overrides are one `Overview` workspace entry, with Validation shown first.
- The preview viewport is above its settings. Built-in meshes hide irrelevant submesh controls; multi-submesh controls appear only for custom FBX meshes with more than one submesh.
- Preview layer selection offers Primary (UV0), Secondary (UV1), and combined UV0+UV1. Meshes without distinct UV1 explicitly mirror UV0 for preview. The combined mode previews authored secondary coverage by alpha-compositing the enabled secondary maps; the full runtime blend algorithms remain future renderer work.
- Preview numeric controls now use the same horizontal-scrub editor as material numeric values. Shift is fine control and Ctrl is coarse.
- The sphere/environment seam was fixed by enabling seamless cubemap sampling and avoiding the face-local coarsest mip for diffuse IBL.
- The key-color field is now a separate swatch plus a selectable/editable hex field. Copy/paste and typed hex values propagate to the preview.
- The color dialog now follows the Wild Wasteland NifSkope layout and workflow: circular hue/saturation wheel, separate value control, RGB/HSV sliders, scrub numeric fields, hex/copy, eyedropper, presets, persistent custom colors, and OK/Cancel. The orphan alpha spin box is explicitly hidden for RGB-only colors.
- Material `.pbrm` files no longer author or serialize `Environment/fallbackCubemap`; legacy occurrences are stripped when read or written. Runtime cubemaps are owned by the Community Shaders PBR component.
- Editor Settings owns the preview cubemap library. The default is `Shared/Cubemaps/mipblur_DefaultOutside1.dds`; named custom cubemaps can be added/removed and selected in Preview. These values live only in `QSettings`.
- Shader ordering is now Standard, Advanced Subsurface, Vegetation, Hair, Eye, Landscape, Glass, Water, Unlit Effect, Simple Lit Effect, Physically Lit Effect, Advanced.
- Release build succeeded, 80 automated checks pass, source OpenGL UI smoke passed, and packaged portable UI smoke passed.

Dynamic Normals remains authoring-only. It describes an additional normal contribution driven by facial expression, animation/controller data, vertex data, or a constant; the standalone preview does not evaluate that runtime-driven contribution yet.

### 2026-07-15 diffuse preview cubemap correction

- The preview no longer samples a relatively sharp cubemap mip directly with the surface normal for diffuse IBL. That path visibly projected a low-resolution, static copy of the environment onto non-metallic parts of the sphere and made it look like a second upside-down cubemap.
- Diffuse environment lighting now uses a 32-sample cosine-weighted hemisphere convolution. The view-dependent specular reflection remains separate and unchanged.
- The release build succeeded, all 96 automated checks passed, and the OpenGL UI smoke test rendered successfully.
- The portable package and `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt\publish` were updated. The deployed executable SHA-256 is `8DB51076B1EDB9C3FE3AC81EC84927570C3E299B6BD29256FE4AAEC522A0B45F`.

## PBRM v4 source of truth and documentation warning

The format envelope in `src/materialdocument.h/.cpp` is authoritative:

- Bytes 0-3: ASCII `PBRM`
- `uint32` little-endian version: `4`
- `uint32` little-endian compact-JSON payload length
- UTF-8 compact JSON payload, maximum 64 MiB

The current JSON root written by the code is:

```json
{
  "schema": "FO4.PBRM.Material",
  "schemaVersion": 4,
  "shader": "Standard",
  "secondaryUvEnabled": false,
  "primaryUv": {},
  "secondaryUv": {},
  "resources": {},
  "settings": {}
}
```

Each texture input stores `enabled`, `expanded`, `path`, and a socket-specific `values` object.

`docs/PBRM-v4.md` now matches the current schema/root names, positive override
semantics, editor-only cubemap ownership, and typed unknown-key behavior.

### 2026-07-15 bug-hunt correction pass

- Fixed the template undo-baseline bug that could mark an unsaved template
  clean after editing and undoing.
- Fixed the Glass template to author `Transparency/Composition.mode` as
  `Physical Transmission` instead of writing an orphaned key.
- Disabled Secondary UV defaults no longer inflate validation counts or emit
  false missing-texture warnings.
- Validation now checks texture resolution/decoding, channels, mip chains,
  color space, normal/RMAOS formats, portable paths, height-source validity,
  parallax step/fade ordering, Advanced fade ordering, alpha-source scope, and
  Landscape composition.
- Transparency now exposes shader-valid alpha sources and greys out controls
  that do not apply to the selected composition/sort mode.
- Added missing Constant-driver values, Custom subsurface profile controls,
  clearcoat strength/IOR, vegetation override wind timing/direction, planar
  projection settings, and constant Secondary coverage.
- Landscape now exposes Height & Geometry with Landscape Normal Blue and
  Separate Height sources.
- The viewport now evaluates transparency modes, alpha threshold, Base/Gradient
  alpha selection, UV transforms and animation, material IBL settings, global
  normal settings, and shader-space Primary + Secondary texture layering.
  Encoded normal and RMAOS textures are no longer alpha-composited as images.
- Custom FBX selection is restored between editor sessions when the file still
  exists.
- Portable packaging removes smoke-test captures before deployment.
- Release build succeeded, all 113 automated checks passed, source OpenGL UI
  smoke passed, and packaged portable UI smoke passed.
- User-facing and `E:` deployed executable SHA-256:
  `25247479A607714CE33BF8BC02BCCA13AB37F71D89FD880384DC1998D7E2F086`.

## Existing Community Shaders PBR runtime state

Community Shaders already contains a partially working legacy/BGSM-backed RMAOS PBR path, separate from the new `.pbrm` editor. It remains a prototype and is default-off:

- `src/Materials/TruePBRShim.h/.cpp`
- `src/Materials/TruePBRShimRuntime.cpp`
- `src/Effects/TruePBRPrepassHashes.h`
- `src/Effects/LightingIntegration.cpp/.h`
- `res/Lighting/deferred_prepass.hlsl`
- `res/Lighting/ambient_ibl_pass.hlsl`
- `res/Lighting/pbr76_brdf.hlsli`
- `res/Lighting/bsdf_light_sun.hlsl`
- `res/Lighting/bsdf_light_spot.hlsl`
- `res/Lighting/bsdf_light_deferred.hlsl`
- `res/Lighting/pbr_prepass_set.txt`
- `res/ShaderReplacement.json`
- `docs/RE/lighting-recon-deferred-prepass.md`
- `docs/RE/lighting-recon-ambient-ibl-pass.md`

The existing Route-A shim identifies `_RMAOS.dds` in the BGSM SmoothSpec slot, uses engine-bound `t2`, tags matching materials, and selects audited deferred-prepass replacements. The packed channels are decoded into the reconstructed lighting path.

There is an architectural mismatch to reconcile: parts of `TruePBRShimRuntime` still describe filename/tag-store classification and a per-draw `b13.x` stamp, while the current selective HLSL recognizes the shipping BGSM path through the CB2 sentinel. Treat some of the tag-store/draw-classification machinery as legacy until the `.pbrm` resolver establishes one authoritative contract.

Current sentinel convention in the BGSM-era prototype is a float bit pattern `0x5042522X`:

- Bit 0: authored dielectric specular/F0 from RMAOS alpha.
- Bit 1: parallax/height present in normal blue; detection exists, POM sampling does not.
- Bits 2-3: reserved.
- `0x50425220`: default 4% dielectric F0, no parallax.
- `0x50425221`: F0 from RMAOS alpha.
- `0x50425222`: parallax flag with default F0.
- `0x50425223`: both.

The normal-channel reconstruction bug was fixed: FO4 normal maps use texture `.xy`, consistent with the overwhelmingly BC5 vanilla set. Do not regress this to `.zw`.

The prepass replacement set was audited by shader shape. Unsupported Blnd/decal, Gr/St, Face, and other incompatible families were evicted rather than forced through the wrong MRT/constant-buffer contract. Alpha-test, AAM, vertex-color-aware alpha test, and glow axes were reconstructed for the kept family. Preserve that conservative approach.

Other current runtime limits:

- Material code 84 is provisional.
- PBR directional variants currently supersede simultaneous screen-space shadow/cloud/skylighting variants; combined permutations remain future work.
- The multibounce AO helper exists, but diffuse indirect does not currently call it.
- Legacy probe mips are box filtered; the current code approximates roughness and explicitly expects properly GGX-prefiltered dynamic cubemaps in the later phase.
- `docs/plans/truepbr76-remaining-work.md` is stale and still labels already implemented shim/ambient work as missing.

## Observed rendering state and validation target

The test spheres broadly demonstrate the expected metallic/roughness grid, and AO can work with the separate SSAO module disabled. The last reported visual problem was that materials became excessively glossy in outdoor shadow and interiors compared with full sunlight/authoring-reference renders.

The current source now contains a newer ambient-branch attempt that postdates the earlier screenshots: RMAOS AO is used for roughness/view-aware specular occlusion independently of SSAO, probe LOD is derived from roughness squared, sharp SSR is limited to smooth materials, and legacy probe exposure is normalized using a coarse probe mip plus live directional ambient. Treat the bug as **requiring fresh in-game validation**, not as proven fixed or proven still present.

Likely areas to validate rather than masking with an arbitrary multiplier:

- Indirect specular energy and specular occlusion in `ambient_ibl_pass.hlsl`.
- Whether RMAOS AO is attenuating both diffuse IBL and specular IBL in a physically reasonable, view-dependent way.
- Environment/probe validity and fallback behavior in unlit/shadowed regions.
- Roughness decode and prefiltered-cubemap mip selection under low direct-light contribution.
- Double counting between ambient cube, SSR, sky IBL, and reconstructed specular.
- The direct/indirect split: shadowing direct light must not make an unchanged indirect highlight look disproportionately dominant.

Do not make PBR AO depend on the optional SSAO module. Texture AO is material data and must remain available independently.

## Test assets and environment-map/NIF finding

The deliberate live test mesh is:

`E:\Projects\Fallout 4 Mods\mods\PBR\meshes\actors\powerarmor\x01\x01_torso.nif`

Its `.nif.bak-envflag` file is the before-patch backup. The patch changed only the seven affected Shader Flags 1 fields by adding `0x80`; all seven BSLightingShaderProperty blocks use shader type 1 (`EnvironmentMap`). Do not bulk-edit other NIFs during this test.

The loose materials are BGSM v2 with specular and environment mapping enabled, environment-mask scale 1, sentinel `0x50425220`, and `_RMAOS` in SmoothSpec. Existing X01/PBRSpheres base color uses BC7 sRGB, normals use BC5U, and RMAOS uses BC7 linear.

Critical interpretation detail: the current X01 and sphere RMAOS alpha is effectively opaque (roughly 251-255), not authored 4% F0. Sentinel bit 0 is clear, so the shader must resolve dielectric F0 to `0.04`. Sampling that alpha accidentally would produce nearly 1.0 F0 and severe white/glossy output.

The sphere chart has a complete 12-mip chain; several X01/PBRSpheres textures currently have only one mip. Add proper mip chains before treating distance stability as a shader result.

The engine only assigns an environment probe when the mesh/material path is eligible. For the earlier BGSM-backed test path, both of these mattered:

- `BSLightingShaderProperty` shader type `1` (`EnvironmentMap`).
- Shader Flags 1 bit 7 (`0x80`, Environment_Mapping).

The mesh-edit/export pipeline had silently stripped bit 7 from affected properties. First-person meshes that bypassed the pipeline retained it. Recheck these flags after any NIF export.

Current debug-view mapping is:

- 0: Off
- 1: Base color
- 2: Roughness
- 3: Metallic
- 4: Ambient occlusion
- 5: Resolved specular/F0
- 6: RMA false color
- 7: Probe slice

Mode 7 used to be called `F0 blackout`; it was repurposed. Blue in mode 7 means no probe assignment, not a material-color result. The code-84 base-combine path also now suppresses the vanilla flat RT23/F0 add while retaining GGX direct specular.

## NifSkope and Creation Kit state

The intended NifSkope target is `E:\Projects\ClaudeNifskope`, the Wild Wasteland fork. The material editor was styled for eventual embedding into that UI. At handoff time, a quick source search did not find committed `.pbrm` handling in that fork, so treat direct `.pbrm` assignment/editing there as pending unless a newer unindexed branch exists.

Creation Kit support is conceptual only. Supporting it requires a tool or patch
workflow that writes direct `.pbrm` root material references into the asset;
Community Shaders does not infer PBRM from legacy BGSM/BGEM names.

## Recommended next sequence

1. **Freeze a minimal Standard Surface runtime contract.** Use the actual editor JSON keys, decide path normalization, default values, packed-channel feature flags, error behavior, and migration/version policy. Keep specialized shaders out of the first runtime slice.
2. **Correct `docs/PBRM-v4.md` and add golden fixtures.** Save a minimal Standard Surface, a Secondary UV material, and a Landscape material; assert exact parsing and round trips.
3. **Add a pure, host-testable PBRM parser to Community Shaders.** No hooks first: envelope validation, JSON parsing, defaults, normalized paths, feature-mask derivation, and cache-key generation.
4. **Add resolver/cache/hot reload.** Support direct `.pbrm` references with
   clean fail-closed behavior. Keep the existing BGSM RMAOS path as a separate
   legacy feature until PBRM parity is proven.
5. **Bind minimal Standard Surface.** Primary base, normal/height, RMAOS, emissive and environment data. Reuse the audited prepass/lighting path where safe rather than creating unaudited broad replacements.
6. **Validate the shadow/interior gloss fix.** Keep SSAO off. Check modes 1-6, then confirm mode 7 shows valid probe slices rather than blue/no-probe. Compare the same material in sun, cast shadow, and an interior. If gloss is still unstable, capture RenderDoc and inspect RT23 F0/roughness, `t2.z` AO, probe slice, ambient replacement execution, and the code-84 base-combine gate.
7. **Add Secondary UV.** Mirror-UV0 mode first, then true UV1 vertex data, independent transforms/controllers, blend modes, vertex-color coverage, pattern modulation, and sample-skipping optimization.
8. **Expand shader families one at a time.** Clearcoat, subsurface/transmission, vegetation/hair, glass/water/eye, effects, decals, landscape, parallax/displacement.
9. **Integrate NifSkope.** Direct `.pbrm` reference assignment and optionally embed the Qt editor widgets.
10. **Only after PBR validation, start dynamic cubemaps.** This ordering is an explicit user requirement.

## Editor build, test, package, and deploy

Run from `C:\Users\bungo\Documents\Codex\2026-07-13\do\work\PBRMaterialEditorQt`:

```powershell
$env:Path = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:Path
qmake6 PBRMaterialEditor.pro
mingw32-make -j4

Set-Location tests
qmake6 MaterialDocumentTests.pro
mingw32-make -j4
..\bin\MaterialDocumentTests.exe
```

Current result: 78 checks, zero failures when the optional real-FBX fixture is
enabled (75 checks without it). The suite now also covers procedural preview
geometry, normalization, tangent/normal generation, and static FBX import.

The editor now has a `--ui-smoke <png>` hook. It was run in a real visible
OpenGL 3.3 context after this preview rewrite; both preview shaders compiled,
the UI rendered, and the screenshot was written successfully. This is a smoke
test rather than a pixel-diff suite. Packaging interaction is still verified by
hashing the final executable after publish/deployment.

Package from the project root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package.ps1 `
  -Destination 'C:\Users\bungo\Documents\Codex\2026-07-13\do\outputs\PBRMaterialEditorQt'
```

Then copy the source folders/root project files to `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt` and the portable package contents to its `publish` directory. The last deployed executable hash matched the workspace package after the XPM checkbox fix.

The deployed tree currently contains a stale nested `publish\publish` bundle from an earlier packaging pass. Do not use that nested executable; use the top-level `publish\PBR Material Editor.exe`. Clean packaging should remove/recreate the destination after verifying the resolved path is exactly the intended publish directory.

## Community Shaders build and test notes

From `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders`, once the Git/index situation is understood and the normal build environment is available:

```powershell
xmake f -m releasedbg -y
xmake build -y B21_F4FX
xmake build -y f4fx_tests
xmake run f4fx_tests
```

Existing test artifacts reported:

- `*TruePBR*`: 19/19 cases, 123 assertions passed.
- `*PBR76*`: 13/13 cases, 86 assertions passed.
- Full suite: 818/821 cases passed. The three failures are unrelated LUT tests caused by absent `res/LUTs/neutral_32.dds`, likely an incomplete LFS asset checkout.

Normal deployment commands used by this project are `xmake install -y` followed by `modkit mod deploy B21_F4FX --no-esp`, but do not deploy a new runtime merely to continue editor work.

## Communication notes

The user prefers direct, simple answers where possible. Do not spend several minutes overexplaining a narrow question. When implementing, keep the user updated and stay on the PBR task rather than branching into unrelated reverse engineering.

## 2026-07-15 GGX preview-environment filtering

The material editor no longer treats ordinary box-filtered cubemap mipmaps as
rough reflections or limits roughness to `roughness * 3.0`. The preview now
keeps the full-resolution source cubemap for the background and sharp mirror
reflections, builds a separate GGX importance-sampled specular cubemap, and maps
roughness across that cubemap's complete mip range. It also generates and uses
a split-sum BRDF integration LUT, so dielectric/metallic Fresnel response and
roughness blur are represented coherently.

This is editor-preview code only in `src/materialpreviewwidget.cpp`; Community
Shaders runtime code was not changed. Release compilation passed, all 113
automated checks passed, and source, packaged, and deployed OpenGL UI smokes
passed with the mesh, background, prefilter, and BRDF-integration shaders linked
and rendered. Workspace, output, and `E:` executable SHA-256:
`700C81EA63896F269A7F0B8782D69566E43DB98489511DAA0DF26EC67C47A0BD`.

## 2026-07-15 material-editor declutter pass

The editor received a cohesive UI-density and navigation pass. `Settings` is now a top-level menu beside File/Edit and owns editor preferences, texture-root selection, Basic/Advanced interface modes, and the `Add Feature…` shortcuts. Preview visibility moved to `View > Material Preview` with F9; texture-card Expand/Collapse All actions are also in View. The version badge moved to the status bar.

The sidebar is now grouped as Workspace, Textures, Material, Geometry & Layering, and Advanced. Section headings collapse and persist through QSettings. Basic mode hides uncommon texture fields plus Dynamic Normals/Advanced pages; Advanced mode exposes them. Overview carries a warning/error dot when validation finds a missing texture root or unresolved enabled texture.

UV layer Copy/Paste/Reset actions are now in the layer header's right-click menu. Bulk texture actions moved into a compact overflow menu. Texture cards have tighter spacing, icon/dot state markers, compact R/G/B/A source controls, no redundant empty-texture diagnostic line, and fallbacks are shown only when their corresponding texture source is disabled. Page descriptions become tooltips in Basic mode. The preview remains a dock-like right pane and keeps its existing NifSkope controls and editor-only configurable cubemap selection.

Verified from the copied working tree at:

`C:\Users\bungo\Documents\Codex\2026-07-14\checkbox-resource-fix-is-deployed-with\work\PBRMaterialEditorQt`

- Release compilation: passed.
- `MaterialDocumentTests.exe`: 80 checks, zero failures.
- Source-build `--ui-smoke`: passed, exit code 0.
- Portable-package `--ui-smoke`: passed, exit code 0 without the build PATH.
- Portable output: `C:\Users\bungo\Documents\Codex\2026-07-14\checkbox-resource-fix-is-deployed-with\outputs\PBRMaterialEditorQt`.

The default preview cubemap is edited at `Settings > Preview Cubemaps…`; it defaults to `Shared/Cubemaps/mipblur_DefaultOutside1.dds`. That preference and custom preview cubemaps are editor-only and must never be serialized into `.pbrm`.

## 2026-07-15 portable texture sources and browser

Texture authoring no longer preserves full absolute paths after browsing, dropping, or finishing an absolute-path edit. Files underneath a configured root or any `Textures` directory are converted to portable virtual paths such as `PBRSpheres\PBRSphere_BC.dds`.

`Settings > Texture Sources…` now configures:

- A Fallout 4 `Data` or `Data\Textures` directory.
- Ordered loose mod or `Textures` folders.
- Ordered Fallout 4 BA2 archives, with multi-file selection and Move Up/Down priority controls.

Loose sources resolve before archives; collisions within each list use the configured order. `Settings > Texture Browser…` and every texture card's Browse button open the searchable merged browser. It lists the virtual path, source, and loose/BA2 kind, provides an image/diagnostic preview, and assigns the portable path. `Browse File…` remains available from the browser for an unindexed loose file.

The new BA2 reader supports Fallout 4 version-1 `DX10` texture archives and `GNRL` archives, including zlib-compressed chunks and DDS header reconstruction. BC7 decoding was added using the small permissively licensed detex subset already used by the local NifSkope fork. The real `E:\Projects\Fallout 4 Mods\mods\PBR\Textures\PBRSpheres\PBRSphere_BC.dds` was identified and decoded successfully.

Verification after this pass:

- Release build: passed.
- Test suite: 92 checks, zero failures with the real BC7 fixture enabled.
- Tests cover relative authoring, loose-source resolution, compressed BA2 reconstruction, archive resolution, browser enumeration, and real BC7 decoding.
- Source-build UI smoke: passed.
- Portable-package UI smoke: passed without the build PATH.

## 2026-07-15 parallax mapping pass

`Height & Geometry` now exposes three explicit parallax algorithms: Offset,
Parallax Occlusion Mapping, and Self-Shadowed POM. Existing files that only
contain the earlier `Geometry/Parallax/enabled` and `depth` fields naturally use
Offset mode, so the schema extension does not strand prototype materials.

The authoring controls include Low/Medium/High/Ultra adaptive-quality presets,
optional custom minimum/maximum view steps, binary refinement, grazing-angle
fade, self-shadow light steps/strength/bias/softness, UV silhouette clipping,
distance fades, and the runtime-facing parallax-depth-output flag. Height can be
sourced independently from Primary or Secondary Normal Blue or Separate Height,
with shared invert, center, and bias controls.

The OpenGL preview now uploads the selected height source separately and applies
Offset/POM displacement to all core material samples. Self-Shadowed POM traces
the editor's rotatable key light in tangent space; environment illumination stays
unshadowed. The depth-output flag is serialized for Community Shaders runtime
integration but does not alter preview depth, because the standalone preview has
no screen-space contact/decal consumers.

Verification and deployment:

- Release build: passed.
- Test suite: 96 checks, zero failures with the real BC7 fixture enabled.
- Source-build OpenGL UI smoke: passed; both preview shaders compiled.
- Portable-package UI smoke without the development Qt path: passed.
- Source, canonical working copy, output package, and
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt` deployment updated.
- Source/package/deployed executable SHA-256:
  `0A337E333B9EE019777FB13A780C4D474F147F5D4CA30E1DC829050B14DAFE54`.

## 2026-07-15 plane preview z-fighting fix

The built-in two-sided plane previously contained two quads with opposite
windings at identical coordinates. Because preview face culling is normally
disabled, both copies competed in the depth buffer. The plane now contains one
quad: two-sided mode draws that quad from either side and uses the shader's
existing `gl_FrontFacing` normal correction; one-sided mode enables back-face
culling only for the plane draw.

- Release build: passed.
- Test suite: 96 checks, zero failures; the plane regression asserts one
  non-overlapping four-vertex/six-index quad.
- Source and portable OpenGL UI smokes: passed.
- Canonical copy, output package, and E: deployment updated.
- Source/package/deployed executable SHA-256:
  `B579AA3AFD0B63C4CB80AA82A26E7CD8D0CFF546B309C9EDAF4050208BB9618C`.

## 2026-07-15 preview cubemap axis fix

Fallout cubemaps use the game's Z-up world convention while the standalone
material preview uses Y-up geometry. The preview previously sampled Y-up world
directions directly, turning `mipblur_DefaultOutside1.dds` onto its side. Both
background and material IBL sampling now convert preview directions from Y-up
to Fallout Z-up after applying the user yaw. The generated fallback studio
cubemap was updated to the same convention.

- The corrected capture shows sky above, ground below, and a level horizon.
- Cubemap rotation remains a yaw around the preview's vertical axis.
- Release build and 96-test suite: passed.
- Source and portable OpenGL UI smokes: passed.
- Canonical copy, output package, and E: deployment updated.
- Source/package/deployed executable SHA-256:
  `99F1C69B92CD852FCFA0FA523EE5CDD62BD1626CBB4CB55B6AFB6C1AB9E1E88E`.

### Cubemap vertical-sign correction

Follow-up visual testing showed the initial Y-up/Z-up conversion used the
opposite X-axis rotation for Fallout's DDS cube convention, leaving the cube
vertically inverted. The conversion sign and the generated fallback studio
environment were corrected together. Background and material IBL continue to
share the exact same direction transform.

- Release build and 96-test suite: passed.
- Source and portable OpenGL UI smokes: passed.
- Canonical copy, output package, and E: deployment updated.
- Source/package/deployed executable SHA-256:
  `85A7A66F006A06834188BF421D6D4A6D2F323515FE1BA5C36E4B48FF0109923E`.

## 2026-07-15 positive texture-channel overrides

Primary and Secondary UV texture pages remain separate. Each page now starts
with one compact toolbar containing its page name, UV badge, Enabled Only
filter, and an overflow menu. Copy/Paste/Reset Layer are also available from
that toolbar's right-click menu; the three former stacked page headers were
removed.

Packed texture-channel controls now use positive constant-override semantics:

- Checked means the authored constant overrides the corresponding texture
  channel; unchecked means the texture channel is used.
- Empty textures force every applicable constant override on.
- Assigning a texture inspects its real channel coverage. Present channels
  switch automatic overrides off, while absent channels remain forced on.
- A deliberate override on a present channel survives texture replacement.
  Automatically forced overrides clear when a replacement texture supplies
  the channel.
- Clearing a texture forces all of its channel constants back on.
- Disabled constant values remain visible but are greyed out.
- `Use Available Texture Channels` preserves required missing-channel
  overrides; `Enable All Constant Overrides` checks every channel.

Base Color / Opacity now exposes independent RGB color and alpha opacity
overrides. The preview resolves texture-versus-constant RGB and alpha before
applying tint and color scale. RMAOS and the other packed inputs use the same
positive convention. Legacy `use...` source booleans are migrated on load so
existing materials preserve their visual intent.

Normal + Height now has an always-visible `B contains height` switch. It is
disabled and cleared when the assigned image does not contain a usable blue
channel (for example BC5). Height source defaults to `None`, and parallax or
displacement consumers are disabled until their selected Normal Blue or
Separate Height input is valid.

Verification and deployment:

- Release build: passed.
- Expanded test suite: 101 checks, zero failures.
- Source OpenGL UI smoke: passed.
- Portable-package UI smoke without the development PATH: passed.
- Portable package: `outputs/PBRMaterialEditorQt`.
- Source/package/deployed executable SHA-256:
  `3B39FEDAAE87E57491DA6EA5BADB67C72B2C877AD4146F18924B27F240467A3B`.

## 2026-07-15 complete texture override audit

The small colored state dot beside every texture input overflow button was
removed. Texture-card disclosure state now has an explicit regression test:
expanded cards retain the existing down-pointing triangle and folded cards use
the matching right-pointing triangle.

The positive constant-override model was extended beyond Base Color and RMAOS
to every applicable channel group in the editor's texture schemas. This now
includes normal vectors, packed height and layer masks, emissive RGB and
intensity masks, tint masks, gradients, distortion direction/strength/masks,
separate height, dynamic normals, subsurface tint/thickness, hair flow and
scatter, glass absorption and imperfections, water flow and foam, skin detail,
eye and cornea masks, clearcoat, landscape arrays, and macro variation.

Grouped channels such as RGB color and RG vectors use one coherent override;
independent packed scalar channels retain separate controls. Every paired
constant stays visible when texture-driven and is greyed out rather than
removed. Feature-disabled channels are also shown disabled but do not create
false missing-channel warnings. In particular, Normal Blue height and Normal
Alpha layer modulation become applicable only when their feature switches are
enabled.

The material preview now resolves constant-versus-texture normal, height,
emissive RGB, and emissive intensity-mask values in addition to the existing
Base and RMAOS overrides. Older `use...` booleans and the old separate-height
`fallback` field migrate to the positive override schema while preserving
their authored intent.

Verification and deployment:

- Release build: passed.
- Expanded test suite: 104 checks, zero failures.
- Source OpenGL UI smoke and visual inspection: passed.
- Portable-package UI smoke without the development PATH: passed.
- Source/package/deployed executable SHA-256:
  `5C9C0026F2039ED208869274105024F0413FE5CDCFBC6B397C40ED5F4E1E2584`.

## 2026-07-15 complete standalone editor preview

All material behavior that can be evaluated inside the standalone editor is
now connected to the OpenGL preview. Community Shaders runtime code was not
changed.

- The viewport resolves every authored texture-channel override, including
  Secondary UV gradient/coverage behavior and decal channel masks.
- Specialized preview paths now cover dynamic normals, tint masks, surface
  sheen/glints/iridescence, subsurface/transmission, hair lobes, glass,
  water/foam, eye/cornea, clearcoat, distortion, vegetation wind, true
  displacement, and distinct Unlit/Simple Lit/Physically Lit effect behavior.
- True displacement automatically swaps the flat preview plane for a single
  non-overlapping 64x64 tessellated plane, so silhouette displacement is
  visible without reintroducing the former z-fighting bug.
- Landscape preview supports selecting DDS texture-array slices 0-15 and
  applies authored macro color and roughness variation. Runtime LAND weights
  remain game-owned and therefore are not fabricated by the editor.
- Custom preview meshes accept static FBX and Fallout 4 NIF files. The NIF
  importer reads BSTriShape, BSSubIndexTriShape, and BSMeshLODTriShape geometry,
  submeshes, UV0/UV1, normals, tangents, vertex colors, and local transforms.
  Skinned NIFs are intentionally shown in bind pose; skinning and mesh
  animation remain out of scope as requested.
- Material UV animation, particle-gradient drivers, dynamic-normal drivers,
  vegetation motion, and water flow update live. Material depth-write,
  depth-test, and two-sided settings are honored by the preview draw.
- Scene-owned inputs (runtime LAND weights, scene color, shadows, fog, and
  motion-vector history) use isolated-preview approximations or are left to the
  eventual runtime integration; they are not serialized as fake editor data.

Verification and deployment:

- Release build: passed.
- Expanded test suite: 122 checks, zero failures, including the deliberate
  X-01 torso Fallout 4 NIF and a two-slice DX10 DDS array fixture.
- Source and final portable OpenGL runtime smokes: passed.
- Portable smoke passed with the development toolchain removed from PATH.
- Portable package: `outputs/PBRMaterialEditorQt`.
- Source/package/deployed executable SHA-256:
  `95995785FF2B302E4163FC08887B965EC10239CE023A351DDF0D87D87CCE1CB4`.

## 2026-07-15 material-preview black-screen hotfix

The standalone material preview is rendering again. The displacement vertex
shader used `packed` as a local variable name; `packed` is a reserved GLSL
layout keyword, so NVIDIA's driver rejected the complete mesh shader and left
the viewport black. The variable was renamed and all preview shader failures
now share an optional `PBRM_SHADER_LOG` diagnostic path.

Enabling proper material-sidedness also exposed inward triangle winding on the
built-in sphere. Its indices now face outward, so back-face culling no longer
cuts away the visible hemisphere.

The `--ui-smoke` runtime check now exits with an error unless the mesh,
background, environment-prefilter, and BRDF-integration programs all link.
This converts the formerly silent black-screen failure into a failing smoke
test.

Verification and deployment:

- Release build: passed.
- Expanded test suite: 123 checks, zero failures.
- Renderer-ready source and portable OpenGL smokes: passed with no shader log.
- Portable smoke passed with the development toolchain removed from PATH.
- Portable package: `outputs/PBRMaterialEditorQt`.
- Source/package executable SHA-256:
  `454A9893CF9E1EC58C0A8809D82F5D792FD924BC1399DE07C662AF7F0FC7F2DC`.

## 2026-07-15 HDR EXR environments and viewport shadows

Preview cubemap preferences now accept `.exr` files in addition to DDS and
ordinary images. A single EXR is interpreted as a 2:1 latitude-longitude HDR
environment, decoded to floating-point RGBA, converted into six cubemap faces,
and uploaded through an RGBA16F OpenGL cubemap. Values above 1.0 survive the
decode/conversion/upload path and participate in the existing GGX prefilter and
exposure controls. TinyEXR is vendored under its BSD-3-Clause license; portable
packages now include the TinyEXR, ufbx, and detex license files.

The preview key light now produces a real 2048x2048 depth shadow map with 3x3
PCF filtering. It shadows both the material object and preview ground. The
Lighting tab exposes persistent preview-only `Shadows` and `Shadow intensity`
controls; disabling shadows also disables the intensity field. The former
painted radial contact-shadow approximation was removed.

Verification and deployment:

- Release build: passed.
- Expanded test suite: 128 checks, zero failures.
- Generated 2:1 EXR retained HDR values above 1.0 and rendered end-to-end.
- Source and portable sphere/ground shadow visual smokes: passed.
- Portable EXR + shadow smoke passed with the development toolchain removed
  from PATH and produced no shader error log.
- Portable package: `outputs/PBRMaterialEditorQt`.
- Source/package executable SHA-256:
  `B91EDDA23CEF53AB95FA73305CBB5BD716FF98640A8FAD8852AAC01FE7AD9D33`.

## 2026-07-15 VATS-inspired application icon

The editor now has a native Windows executable icon and a matching Qt window
icon. The final mark is limited to a luminous green material-preview sphere
inside four luminous targeting brackets. The surrounding canvas is fully
transparent; there is no monitor, screen, tile, bezel, casing, or text. A soft
phosphor glow remains around both the sphere and brackets so the VATS-inspired
silhouette reads at small Windows icon sizes.

Source artwork and application assets live under
`resources/icons`. `RC_ICONS` embeds the multi-resolution ICO in the Windows
executable, while `resources.qrc` supplies the 256 px PNG to Qt at runtime.

Verification and deployment:

- Release build: passed.
- Full test suite: 128 checks, zero failures.
- The icon extracted successfully from the compiled executable and retained
  the transparent, glowing sphere-and-brackets silhouette.
- Source, portable-package, and exact `E:` project `bin` OpenGL smokes passed.
- Portable smoke passed with the development toolchain removed from PATH.
- The canonical project is now
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt`; future source edits,
  builds, tests, and packaging should be performed there.
- Launch the complete portable build from
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt\publish\PBR Material Editor.exe`.
- The accidental nested `publish\PBRMaterialEditorQt` deployment was removed;
  the executable, Qt runtime, plugins, third-party DLLs, README, and licenses
  now live directly inside `publish`.
- Source/package/deployed executable SHA-256:
  `D03EC790F374B4EEEBB7DDFDD7F009C985DD7552711D04A6D4025B750626C184`.

## 2026-07-15 taskbar icon scale hotfix

The application icon was tightly cropped around the glowing sphere and VATS
brackets. Low-alpha chroma-key noise outside the intended glow was removed,
and the visible mark now occupies about 90% of the square canvas instead of
roughly 72%. The glow remains intact, but Windows no longer shrinks the useful
artwork inside a large transparent border.

Verification and deployment:

- Multi-resolution ICO rebuilt at 16, 20, 24, 32, 40, 48, 64, 128, and 256 px.
- The 32 px icon extracted successfully from the compiled executable.
- Full test suite: 128 checks, zero failures.
- Exact `E:` publish executable opened `PBRSpheres.pbrm` successfully in the
  renderer-ready smoke test.
- Published executable SHA-256:
  `23F141245F2180565E39A481A770BC1B31766B22E864E8A732C94880A15353DA`.

## 2026-07-15 EXR orientation and rough-prefilter hotfix

Latitude-longitude EXR environments are now converted through the inverse of
the preview shader's Fallout/DDS axis remap. This keeps imported EXRs world-Y-up
and aligns their background, reflection, and cubemap-rotation directions with
the preview scene without changing the established DDS cubemap convention.

The GGX environment prefilter now copies the sharp mip with one exact sample,
uses 256 samples for the narrow low-roughness level, and uses 1024 Hammersley
samples for the rough levels. This removes the strong static variance and white
HDR firefly pattern that became visible when rough materials integrated bright
EXR texels, while avoiding the cost of oversampling the full-resolution sharp
mip.

Verification and deployment:

- Added an oriented latitude-longitude EXR regression fixture that verifies
  the world-up and world-down poles land on the expected preview cubemap axes.
- Full test suite: 130 checks, zero failures.
- Exact `E:` publish executable passed renderer-ready smoke tests with both a
  generated HDR EXR and a 29.6 MB production EXR; no shader error log was
  produced.
- Published executable SHA-256:
  `CC5697D99D89136BC922E361B76D0FD90E789128B1BD42C95887ABE7E8A1367D`.

## 2026-07-15 DDS/EXR separation and black-region hotfix

Fallout 4 DDS cubemaps and latitude-longitude EXR environments now use
explicitly different prefilter rules. DDS roughness convolution always samples
the full-resolution source face (LOD 0); it never reuses the game cubemap's
authored lower mip chain. This preserves the earlier fix for Fallout 4's
incorrect rough-surface mip behavior. EXR environments retain floating-point
HDR source-mip filtering and their dedicated inverse axis conversion.

The prefilter sample count uniform is now a signed integer, matching Qt's
OpenGL uniform upload and preventing undefined loop counts that produced large
black regions on EXR-lit spheres. Sharp levels use one exact sample, DDS rough
levels use 256 base-LOD samples, and rough EXR levels use up to 1024 samples.

Preview-environment preferences now live beside the executable in
`publish\preview-environments.ini`. The editor migrates legacy registry values
once, and packaging preserves this file, so rebuilding the portable package no
longer removes the user's custom EXR/DDS catalog or active selection.

Verification and deployment:

- Full test suite: 130 checks, zero failures.
- Oriented EXR pole regression: passed.
- Isolated portable EXR viewport smoke: passed with no black regions.
- Runtime DLLs and `platforms\qwindows.dll`: present directly in `publish`.
- No accidental nested `publish` directory.
- Canonical executable:
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt\publish\PBR Material Editor.exe`.
- Published executable SHA-256:
  `944160DBFB6B72B642CF6B4C027CCFE5A8B7CA36BDF3CA3100631F8CAE1F83F5`.

## 2026-07-15 Fallout DDS authored-mip regression guard

The DDS preview path now makes the distinction explicit: it decodes only the
six full-resolution cubemap faces and discards every authored Fallout mip.
OpenGL generates a clean replacement mip chain from those base faces, and the
GGX roughness convolution may sample that generated chain. It never samples
the lower mip levels stored in the Fallout DDS file.

A new regression fixture contains six valid base faces followed by deliberately
poisoned lower mips. The test verifies that only the base faces reach the
preview loader. The real `mipblur_DefaultOutside1.dds` was also decoded and its
six face adjacencies/orientations were checked.

The sharp division visible on `PBRSpheres.pbrm` was isolated separately. A
prefilter-only render and a uniform-material render are seamless; the division
appears only when `PBRSphere_RMAOS.dds` is enabled. It is an authored hard
roughness/metallic test band, not a second, mirrored, or upside-down cubemap.
Enable the R roughness constant override to inspect the sphere at one uniform
roughness value.

Verification and deployment:

- Full current suite: 129 checks, zero failures.
- Poisoned authored-mip regression: passed.
- Real Fallout DDS six-face decode and orientation inspection: passed.
- Uniform-material DDS viewport isolation: seamless.
- Canonical executable:
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt\publish\PBR Material Editor.exe`.
- Published executable SHA-256:
  `A5A9B0C43F097025EFFB7107E58A6E059CFFA7AB2CFD4CD18AF452C97ACA9650`.

## 2026-07-15 non-metal diffuse IBL smear hotfix

The remaining DDS grain and EXR white streaks were isolated to diffuse
environment lighting. The per-fragment cosine convolution used 32 directions
but sampled source mip 1, allowing individual high-energy HDR texels to become
static streaks on non-metallic surfaces. Metals hid the problem because their
diffuse lobe is suppressed.

Diffuse irradiance now samples the clean, generated environment chain at a
resolution-derived broad-solid-angle level (the source's approximately 4x4
mip). This stabilizes the 32-sample diffuse integral without changing the GGX
specular prefilter, metallic reflections, EXR orientation, or the rule that
Fallout-authored DDS mips are discarded.

Verification and deployment:

- Full suite: 129 checks, zero failures.
- Real `Cave Entry Forest` EXR portable smoke: passed; scratch-like white smear
  removed and the remaining highlight is smooth.
- Canonical `bin` and `publish` executables were rebuilt from the same source.
- Canonical executable:
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt\publish\PBR Material Editor.exe`.
- Published executable SHA-256:
  `1F0BE68FBEEA37759CAEC183156DD77F8E103F883AFB81F633271FBD505D9E3A`.

## 2026-07-15 preview-settings redesign and EXR-derived shadows

The material-preview controls were reorganized into compact `Scene`,
`Lighting`, and `Display` tabs. The checkbox-like `Preview Setup` disclosure
and disabled `Full Material` strip were removed. Geometry, material-layer,
camera, lighting, and display names now describe their actual scope, including
`Primary Material (UV0)`, `Imported Mesh`, `Axis view`, `Render mode`, and
`Environment` rather than the DDS-only term `cubemap`. Built-in mesh statistics
and the full NifSkope control list moved out of the always-visible layout into
tooltips where appropriate.

When a floating-point EXR environment contains a sufficiently dominant HDR
source, the preview now derives a matching key-light direction, color, and
contrast-scaled intensity and uses that light in the existing shadow-map pass.
The scan locally averages source pixels and uses cubemap solid-angle weighting,
which prevents one isolated HDR firefly from steering the light. EXR/world-axis
conversion follows the established EXR orientation path, and Environment
Rotation rotates the derived key light and shadow direction with the image.
Environment Intensity scales both the image lighting and derived key light.

`Derive key light and shadows from EXR` is enabled by default for a usable EXR
and is unavailable for DDS environments. Manual key-light edits disable the
derived state. Switching from EXR back to DDS restores the prior manual light;
the EXR-derived values do not leak into the separate DDS path or overwrite the
saved manual-light preference. The real `Cave Entry Forest` EXR derives a warm
light at azimuth `-111.9°`, elevation `17.0°`, and calibrated intensity `8.0`
at Environment Intensity `1.0`.

Verification and deployment:

- Release build: passed.
- Expanded suite: 137 checks, zero failures.
- Real `Cave Entry Forest` EXR published OpenGL smoke: passed with no shader log.
- Preview UI was visually checked at the narrow right-pane size for all new
  Scene and Lighting controls; no clipping was observed.
- `bin` and `publish` executables are byte-identical.
- Published executable SHA-256:
  `1ACFD3B32EB7B479842605AFECFB5926EEA8028B28ACD529AB8FFE46E1D0B261`.

## 2026-07-15 EXR sole-source lighting correction and resolved-channel views

The EXR-derived mode no longer creates an analytic key light in addition to
the HDR environment. `Derive shadow direction from EXR` now uses the dominant
HDR radiance only to orient the shadow-map pass; the EXR remains the sole
source of material illumination and the effective analytic key intensity is
forced to exactly zero. The derived controls show intensity `0.0`, EXR color
and direction, and disable manual key edits while active. Manual lighting and
DDS environments retain the original analytic-key path.

Derived shadow-map visibility is applied only where a cast shadow is needed,
such as the ground plane. It is not multiplied back into the object's IBL,
because the EXR already supplies its directional surface shading. This avoids
both double lighting and a serrated shadow-map terminator on the material.

The Display tab's Render Mode inspector now exposes the resolved values that
actually reach shading after texture selection, constant overrides, UV choice,
and secondary-layer composition:

- Shaded, Base Color, and Opacity
- Normal Map (Tangent Space), Normal (World Space), and Height
- Roughness, Metallic, Roughness + Metallic (R/G), Ambient Occlusion,
  Specular F0, and RMAOS (Packed RGB)
- Emissive Color, Emissive Mask, Emissive (Final), and Layer Coverage
- UV Checker, Wireframe, and Tangent Basis

Existing numeric Render Mode values remain compatible with saved settings.
The new debug views also work for unlit shader families.

Verification and deployment:

- Release build: passed.
- Expanded suite: 141 checks, zero failures.
- Real `Cave Entry Forest` EXR published OpenGL smoke: passed with no shader
  log; the analytic key is zero and the shaded sphere has no shadow-map seam.
- Published runtime smokes for tangent-space normal, roughness + metallic, and
  packed RMAOS views: passed with no shader logs.
- The prior saved Render Mode was restored after each runtime smoke.
- `bin` and `publish` executables are byte-identical.
- `platforms/qwindows.dll` and `preview-environments.ini` are present; no
  nested `publish` directory exists.
- Canonical executable:
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt\publish\PBR Material Editor.exe`.
- Published executable SHA-256:
  `F3478E4BF0DA397A89F9DC8B0509E866E577D643E17238C6FEC80ACA469A1F2C`.

## 2026-07-15 Fallout 4 Community Shaders PBRM v4 runtime integration

Canonical runtime branch:
`E:\Projects\Fo4CommunityShaders\fallout4-community-shaders` at original HEAD
`ddd162fc2af68e0a0d8dbf32458b84a5409f6c21` (`main`). The pre-existing Git
index/worktree state is unusual (tracked deletions plus same-path untracked
files); it was preserved and no reset, checkout, commit, or staging operation
was performed.

The runtime now intercepts a NIF's direct `Materials\path\name.pbrm` root name at
`BSShaderMaterial::ReceiveValuesFromRootMaterial` vfunc `0x07`, before Fallout's
BGSM/BGEM database is consulted. Both the base and concrete Standard material
vtables are patched so actual `BSLightingShaderMaterial` instances take the
path. It reads through `BSResourceNiBinaryStream`
for loose/BA2 parity, validates the complete PBRM v4 envelope and JSON root, and
compiles supported data to an immutable cached record. Successful, missing,
invalid, and unsupported results use case/slash-normalized single-flight keys.
BGSM/BGEM names are explicitly rejected by the PBRM resolver.

Initial safe runtime slice:

- Standard shader, Primary UV only;
- sampled or constant base RGB/opacity, linear constant color, tint, and scale;
- sampled or constant encoded normal plus strength;
- independently sampled or constant packed RMAOS roughness, metallic, AO, and
  dielectric F0;
- a minimal internal Standard `BSShaderData` and engine texture set reusing base
  `t0`, normal `t1`, and SmoothSpec `t2`, so the integration adds no texture
  samples. This is an internal Fallout renderer bridge, not Bethesda material
  format compatibility.

Secondary UV and specialized shader families parse but fail closed to the NIF's
embedded material state. PBRM emissive is parsed and diagnosed but is not bound
because the current auxiliary G-buffer carrier is already used by PBR
F0/roughness; it needs an explicit expanded G-buffer/composite contract.

Runtime hot-path properties:

- no JSON, resource I/O, path work, or texture discovery per draw;
- no per-draw `UpdateSubresource` from the material hook;
- ordinary materials perform one extension check, do not enter the PBRM cache,
  and continue through the original engine call;
- only sentinel-marked PBRM draws look up and bind immutable 96-byte constants;
- malformed or missing direct PBRM materials log once and negative-cache instead
  of being passed to Bethesda's material parser.

Files and documentation:

- `src/Materials/PBRM.h/.cpp`
- `src/Materials/TruePBRShim.h/.cpp` and `TruePBRShimRuntime.cpp`
- `res/Lighting/deferred_prepass.hlsl` and installed `F4FX` copy
- `tests/pbrm_tests.cpp` and expanded TruePBR tests
- `docs/PBRM-RUNTIME.md`
- `res/B21_F4FX.ini` (`bPBRMMaterials = 1` by default; legacy `_RMAOS` remains
  separately disabled by default)

Verification:

- focused PBRM: 10 cases / 70 assertions, zero failures;
- focused TruePBR: 20 cases / 128 assertions, zero failures;
- full host suite: 825/828 cases and 17,550/17,553 assertions; only the same
  three unrelated LUT tests fail because `res/LUTs/neutral_32.dds` is absent;
- all 60 non-FO76 unique prepass define sets from the manifest compile with
  `fxc ps_5_0`; source and installed prepass HLSL are byte-identical;
- canonical `releasedbg` `B21_F4FX.dll` build and project-local install passed;
- DLL SHA-256 after the direct-reference loader adjustment:
  `7D1934834CCB7693C110A4621D6FDDF2B910911DD7DE9D0BB606303D09BCEDD3`.

No game-directory deployment or in-game smoke was performed. A pre-existing,
separate manifest issue was exposed by the exhaustive shader sweep: FO76 PBR
plus Glow declares both the FO76 reflectance texture and the glow texture at
`t3/s3`, which `fxc` rejects as overlapping registers. It does not affect the
RMAOS/PBRM path but should be resolved before enabling FO76 PBR on Glow
permutations.

## 2026-07-15 PBRM v4 forward-compatibility policy implemented

The additive-extension policy is now enforced by both the editor and Community
Shaders runtime:

- unknown non-empty shader names parse as valid cached PBRM materials but remain
  runtime-unsupported; they are never coerced to Standard;
- the editor still refuses to edit an unknown shader family, but for known
  families it preserves unknown root members, requirements, collection members,
  slots, and nested JSON values across load/save;
- optional `requirements` maps semantic capability names to positive integer
  versions. The initial runtime provides `standard.primaryUv: 1`. Missing or
  newer required capabilities retain a valid document but fail closed before
  engine binding;
- additive optional fields/slots remain PBRM v4. Only incompatible structural
  or semantic changes may use v5, with an explicit migrator;
- the renderer bridge is versioned separately from the file: draw-constants ABI
  `1` and texture-binding ABI `1` occupy the reserved PBRM header words inside
  the unchanged 96-byte `b13` block. Texture ABI `1` is Base Color `t0`, Normal
  `t1`, and packed RMAOS `t2`. HLSL takes the vanilla path on ABI mismatch.

Validation after the policy change:

- editor regression suite: 145 checks, zero failures;
- focused PBRM runtime: 13 cases / 90 assertions, zero failures;
- all 60 unique non-FO76 prepass define sets compile with `fxc ps_5_0`;
- live `B21_F4FX` releasedbg build and project-local install succeeded;
- full runtime host suite: 828/831 cases and 17,570/17,573 assertions; only the
  same three unrelated neutral-LUT tests fail because
  `res/LUTs/neutral_32.dds` is absent;
- built/project-installed DLLs are byte-identical, SHA-256
  `62C0CA3AF0EE7C4F6E42B49B28538EB8F3481A98B901F307A5D38972444F6D5E`;
- editor `bin`/`publish` executables are byte-identical, SHA-256
  `45D5C6ABD7690555CAD315DE92DF83A9348E7360ECA7CB3B39B50453A9EF56B1`;
- runtime source/installed prepass HLSL copies are byte-identical, SHA-256
  `3A5236E5A653DA91F22B80DD0F1C58174D7946270F7231EB4F5F1901B938BD9C`;
- no accidental nested editor `publish` directory exists.

No Fallout 4 game-directory deployment or in-game smoke was performed.

## 2026-07-15 FO4CS live PBRM integration and final cubemap/culling correction

This is the newest continuation point. Treat the paths and state in this
section as authoritative over older Community Shaders notes above.

### Canonical and active trees

- Material editor source/build tree:
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt`
- Material editor executable:
  `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt\publish\PBR Material Editor.exe`
- Community Shaders source/build tree:
  `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders`
- Active MO2 Community Shaders mod:
  `E:\Projects\Fallout 4 Mods\mods\FO4CS`
- Active DLL:
  `E:\Projects\Fallout 4 Mods\mods\FO4CS\F4SE\Plugins\FO4CS.dll`
- Active PBR test-material mod:
  `E:\Projects\Fallout 4 Mods\mods\PBR`

`E:` remains canonical. The `C:\Users\bungo\Documents\Codex` task directory
contains staging tools and diagnostic outputs only.

### Material replacement contract now working

- NIFs continue to reference ordinary `.bgsm` / `.bgem` root materials.
- FO4CS automatically probes a same-name sibling `.pbrm` and layers its data
  over the engine-initialized Bethesda material.
- Direct `.pbrm` references from NIFs remain reserved for future support.
- `BSDFPrePassShader::SetupMaterial` was the essential missing per-draw hook.
  Automatic replacement and per-draw PBRM constants/SRV binding now work.
- Loose PBRM/DDS hot reload is available from the in-game menu.
- Base Color, Roughness, Metallic, AO, Specular, and packed RMA debug views
  work independently.
- The six current test sidecars apply successfully: X01 helmet, torso, decals,
  arms, legs, and `PBRSpheres`.

The `.pbrm` format is a new portable material format, not a BGSM/BGEM variant.
Do not edit NIFs to force current automatic replacement.

### PBRM two-sided policy correction

The renderer previously ignored `settings["Shading/Core"].twoSided`, so it
inherited the fallback BGSM's culling flag. The current X01 and sphere files
have empty `settings` objects; in the editor this means the Standard default,
which is one-sided.

The runtime now:

- parses `Shading/Core/twoSided` into `RuntimeMaterial::twoSided`;
- defaults missing values to `false` for Standard materials;
- explicitly sets or clears `BSShaderProperty::kTwoSided` after a successful
  PBRM replacement rather than inheriting the BGSM value;
- has a regression test for both default-one-sided and explicit-two-sided.

Relevant files:

- `src/Materials/PBRM.h`
- `src/Materials/PBRM.cpp`
- `src/Materials/TruePBRShimRuntime.cpp`
- `tests/pbrm_tests.cpp`

### Cubemap artifact correction

The original source cube is:

`E:\Projects\Fallout 4 Mods\mods\Vanilla Cubemap Fix\Textures\Shared\Cubemaps\mipblur_DefaultOutside1.dds`

It is a valid legacy BGRA8 cubemap with six 128x128 faces and eight authored
mips. No higher-resolution equivalent exists in the cubemap-fix mod. The
previous runtime roughness filters used sparse angular taps and produced
recognizable repeated scene copies; using lower mips also reduced definition.
Grazing reflections made the source face boundaries and low-resolution texels
especially visible.

The deployed replacement is an offline GGX-prefiltered `TextureCubeArray`:

- eight roughness cubes;
- all six faces of every roughness cube remain 128x128;
- one mip only per cube, so roughness never selects a lower-resolution mip;
- seam-aware sampling and 32-512 offline importance samples per texel;
- BC7 sRGB, 786,580 bytes;
- two cubemap reads per PBR pixel to interpolate adjacent roughness slices,
  replacing the former maximum of thirteen runtime reads;
- configured FO4CS environment is authoritative until dynamic PBR probes are
  implemented; an engine probe is only the compatibility fallback when the
  configured array is unavailable.

Canonical asset:

`E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\Textures\FO4CS\Cubemaps\FO4CS_PBR_DefaultOutside1_prefiltered.dds`

Active asset:

`E:\Projects\Fallout 4 Mods\mods\FO4CS\Textures\FO4CS\Cubemaps\FO4CS_PBR_DefaultOutside1_prefiltered.dds`

Active INI setting:

```ini
[TruePBR]
sDefaultCubemap = FO4CS\Cubemaps\FO4CS_PBR_DefaultOutside1_prefiltered.dds
```

The t20 shader declaration is now `TextureCubeArray<float4>`. The runtime
intentionally accepts only an eight-cube `D3D11_SRV_DIMENSION_TEXTURECUBEARRAY`
for this setting. Pointing `sDefaultCubemap` back to an ordinary legacy
`TextureCube` will fail closed and log an error.

Relevant files:

- `res/Lighting/ambient_ibl_pass.hlsl`
- `F4FX/Lighting/ambient_ibl_pass.hlsl`
- `src/Materials/TruePBRShim.h`
- `src/Materials/TruePBRShim.cpp`
- `src/Materials/TruePBRShimRuntime.cpp`
- `res/FO4CS.ini`
- `tests/truepbr_shim_tests.cpp`

The generator remains diagnostic/scratch at:

`C:\Users\bungo\Documents\Codex\2026-07-15\continue-the-fo4-pbr-material-editor\work\cubemap-prefilter\build_prefiltered_cube_array.py`

If the cubemap must be regenerated or generalized, move a reviewed version of
that generator into the canonical repository first; do not treat C: as source.

### Build, tests, and deployment verification

The normal `xmake` executable was no longer installed/on PATH. An official
portable xmake 3.0.8 was downloaded to:

`C:\Users\bungo\Documents\Codex\2026-07-15\continue-the-fo4-pbr-material-editor\work\xmake-portable\xmake\xmake.exe`

Build commands used from the Community Shaders root:

```powershell
& <xmake.exe> f -m releasedbg -y
& <xmake.exe> build -y f4fx_tests
& <xmake.exe> build -y FO4CS
```

Results:

- `f4fx_tests` rebuilt successfully;
- `FO4CS.dll` rebuilt, linked, and project-installed successfully;
- focused PBRM tests: 14/14 cases, 94/94 assertions;
- focused TruePBR tests: 22/22 cases, 142/142 assertions;
- full host suite: 831/834 cases and 17,588/17,591 assertions pass;
- the only three full-suite failures are unrelated pre-existing neutral-LUT
  tests because `res/LUTs/neutral_32.dds` is absent;
- ambient HLSL compiles as baseline, dual-accum, PBR, and all-feature
  permutations with `fxc ps_5_0`;
- the game was not running during final deployment.

Final deployed SHA-256 values:

- `FO4CS.dll`:
  `07F930AFD45D60DEFC623B4FB4E43E3012D041A5E7728764595032045BDA5A06`
- `ambient_ibl_pass.hlsl`:
  `520233D0854787AA0F397F7677D43A8A7CFD27F7206315AECDAEB4BEC4489B65`
- `FO4CS_PBR_DefaultOutside1_prefiltered.dds`:
  `F86F363EF7155C34C99D3C92211C139A8EE2BDB28934A7E14625F98C1048A312`

For all three, canonical/build and active MO2 copies were byte-identical at
final verification.

### Material-asset audit notes

All referenced X01 and sphere DDS files exist and decode correctly. Runtime
logs showed no DDS load errors. The X01 roughness/metallic/AO debug values vary
between limbs because the five packed RMAOS textures contain materially
different authored pixel distributions and UV layouts; FO4CS is binding the
correct sidecar and DDS per material. Normalizing their visual response is an
asset-authoring task, not a PBRM parser fix.

All eighteen current material DDS files have only one mip. This is a genuine
asset quality/performance issue for ordinary material textures and should be
addressed later by generating correct clean mip chains. It is independent of
the new full-resolution cubemap roughness array.

### Exact continuation point

The final DLL, HLSL, cubemap array, and INI are deployed, but the user has not
yet launched Fallout 4 to visually verify this final correction.

On the next launch, first inspect the newest F4SE log for:

```text
[TruePBR] default cubemap ready: FO4CS\Cubemaps\FO4CS_PBR_DefaultOutside1_prefiltered.dds
```

Then verify:

1. sphere reflections no longer contain repeated scene copies or horizontal
   face-edge bands at grazing angles;
2. rough spheres remain spatially defined while becoming smoothly blurred;
3. X01 and sphere geometry obey one-sided culling;
4. each PBR debug channel still resolves correctly;
5. all six automatic PBRM replacements still apply.

If startup fails or the asset is rejected, inspect the newest
`Documents\My Games\Fallout4\F4SE\FO4CS_*.log` before changing code. Do not
reintroduce sparse runtime convolution or ordinary mip-based roughness blur.
+
## 2026-07-16 seam-correct conventional GGX cubemap

This supersedes the eight-cube `TextureCubeArray` runtime design documented
above. The visible sphere seam was present in the stored face borders of
`FO4CS_PBR_DefaultOutside1_prefiltered.dds`, not introduced by SSR or sphere
geometry.

The runtime environment is now a conventional DX10 DDS `TextureCube`:

- `R8G8B8A8_UNORM_SRGB`;
- 128x128 mip 0 with eight total mips down to 1x1;
- mip 0 is sharp and mips 1-7 are offline GGX importance-sampled at increasing
  perceptual roughness;
- matching face borders are averaged in linear space after convolution and
  verified for exact equality after sRGB encoding;
- 524,428 bytes;
- one trilinear cubemap read per PBR pixel using `roughness * 7` as LOD.

The reviewed generator is now canonical at
`scripts/build_pbr_prefiltered_cubemap.py`. It reads the legacy BGRA8 Fallout
cube, ignores its authored lower mips, performs cross-face source sampling,
stitches every output mip, validates the encoded borders and DDS size, and
writes the conventional mip-chain resource.

Runtime changes:

- `ambient_ibl_pass.hlsl` declares t20 as `TextureCube<float4>` and samples the
  GGX mip chain directly;
- the loader accepts only a `D3D11_SRV_DIMENSION_TEXTURECUBE` with at least
  eight mips;
- the engine probe remains a compatibility fallback when the configured cube
  is absent or rejected;
- `docs/PBRM-RUNTIME.md` and `res/FO4CS.ini` describe the new contract.

Verification and deployment:

- generator border and DDS-size validation: passed for all eight mips;
- baseline and PBR+SSAO ambient HLSL permutations compile with FXC ps_5_0;
- focused TruePBR host tests: 22/22 cases and 142/142 assertions passed;
- releasedbg FO4CS build and project staging succeeded;
- canonical/build and active MO2 copies are byte-identical.

SHA-256:

- `FO4CS.dll`: `A7306454FEEC394B6763E39339722761CDA081869430957B2C082733A5E6A7DA`;
- `ambient_ibl_pass.hlsl`: `203EAC148B06080A8B9A8863CD4F9D20CB952C4E9611C57136FC9E9016DDDC93`;
- cubemap DDS: `7330153087FEE32DCAA5882184637D033979328FA388EFED53155B7BED3094B0`;
- generator: `CF0AD439888C6B793905855C71E514DC58DC773875AF4D1E9E74B3D9163AB6BD`.

Fresh in-game validation is still required. On launch, confirm the default
cubemap-ready log line, inspect smooth and rough spheres for face seams, and
recheck the PBR debug channels. Do not restore the superseded roughness-as-array
layout.

## 2026-07-16 final handoff — roughness cubemap problem fixed and validated

This section is the final authority for 2026-07-16. It supersedes the earlier
runtime-convolution, six-axis fallback, conventional roughness-mip, and
single-mip cube-array notes above.

### Final result

The user confirmed in game that the cubemap problem is fixed.

The final sphere grid now behaves as intended:

- smooth spheres retain one sharp environment reflection;
- increasing roughness produces one continuously broader reflection;
- rough spheres no longer show several overlapping copies of the environment;
- the dark/bright two-hemisphere split is gone;
- roughness consumed from RT23 remains correct.

The debug views isolated and verified the full path:

- mode 9: roughness actually consumed from RT23;
- mode 10: the single sharp source environment;
- mode 11: the offline-prefiltered environment selected by roughness;
- mode 12: final indirect specular before screen AO.

### Root cause

The RMAOS roughness decode and RT23 transport were not the problem.

The failure was attempting to construct a broad reflection lobe at runtime by
averaging a small number of sharp cubemap lookups. Every lookup remained a
recognizable copy of the scene:

- the original 12-direction filter produced several repeated environments;
- a six-axis rough fallback produced a piecewise dark/bright hemisphere split;
- increasing the filter to 32 directions only produced more, denser copies as
  roughness increased.

No practical low-count runtime angular convolution can turn this sharp,
high-contrast cubemap into a visually continuous rough reflection. The
environment must be integrated offline with many samples per output texel.

### Final implementation

The configured PBR environment is now a DX10 DDS `TextureCubeArray`:

- format: `R8G8B8A8_UNORM_SRGB`;
- eight cubemaps representing perceptual roughness `0/7` through `7/7`;
- six 128x128 faces per roughness cube;
- exactly three mips per cube: 128x128, 64x64, and 32x32;
- 48 total array faces;
- 4,128,916 bytes;
- offline GGX importance sampling with 1-1024 samples per output texel;
- cross-face source sampling;
- face borders stitched and validated after convolution and after each
  downsample.

Roughness is represented along the cube-array axis, not by selecting the noisy
low-resolution tail of a conventional cubemap mip chain. Mips 0-2 exist only
for normal texture minification and are the complete permitted mip range.

At runtime:

- `t20` is `TextureCubeArray<float4>`;
- roughness maps to the adjacent two array slices;
- the shader performs two coherent cubemap reads and linearly interpolates
  them;
- the sampler is clamped to maximum LOD 2;
- no runtime angular convolution remains;
- no six-axis fallback remains;
- the configured prefiltered array is authoritative while available;
- Fallout engine probes remain a bounded-mip compatibility fallback because
  they do not contain equivalent GGX roughness slices.

The loader fails closed unless the configured resource is:

- `D3D11_SRV_DIMENSION_TEXTURECUBEARRAY`;
- exactly eight cubes;
- exactly three mips;
- a square texture with `ArraySize == 48`.

### Canonical files

Community Shaders repository:

- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\scripts\build_pbr_prefiltered_cubemap.py`
- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\Textures\FO4CS\Cubemaps\FO4CS_PBR_DefaultOutside1_prefiltered.dds`
- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\res\Lighting\ambient_ibl_pass.hlsl`
- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\F4FX\Lighting\ambient_ibl_pass.hlsl`
- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\src\Materials\TruePBRShimRuntime.cpp`
- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\src\Materials\TruePBRShim.h`
- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\res\FO4CS.ini`
- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\docs\PBRM-RUNTIME.md`
- `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders\docs\RE\lighting-recon-ambient-ibl-pass.md`

Active MO2 deployment:

- `E:\Projects\Fallout 4 Mods\mods\FO4CS\F4SE\Plugins\FO4CS.dll`
- `E:\Projects\Fallout 4 Mods\mods\FO4CS\F4SE\Plugins\FO4CS.pdb`
- `E:\Projects\Fallout 4 Mods\mods\FO4CS\F4FX\Lighting\ambient_ibl_pass.hlsl`
- `E:\Projects\Fallout 4 Mods\mods\FO4CS\Textures\FO4CS\Cubemaps\FO4CS_PBR_DefaultOutside1_prefiltered.dds`

### Verification

Offline asset validation:

- generator completed all eight roughness cubes and all three mips;
- encoded face-border validation passed for every slice and mip;
- calculated DDS size matched the written size;
- DirectXTex identified the final file as:
  `128x128, 3 mips, 48 faces, R8G8B8A8_UNORM_SRGB CubeArray`.

Shader/build validation:

- baseline ambient shader: compiled cleanly;
- PBR ambient shader: compiled cleanly;
- dual-accum PBR ambient shader: compiled cleanly;
- all-feature ambient shader: compiled cleanly;
- releasedbg `FO4CS.dll`: built and installed successfully.

Focused host tests:

- PBRM: 14/14 cases, 94/94 assertions;
- TruePBR: 26/26 cases, 162/162 assertions;
- lighting integration: 48/48 cases, 1694/1694 assertions.

Latest runtime log:

`C:\Users\bungo\Documents\My Games\Fallout4\F4SE\FO4CS_20260716_230751.log`

Relevant evidence:

- replacement shaders compiled 30/30;
- variant selection applied with zero shader errors;
- ambient variant selected `phase-c-pbr`;
- the default cubemap array loaded successfully;
- X01 and PBR sphere draw bindings were active.

The log also contains an unrelated presenter fallback to native D3D11. It did
not prevent PBR initialization or validation and is outside this fix.

### Final deployed SHA-256

- `FO4CS.dll`:
  `DB344F4201B67380C92758CB1E574E9E973397393D288AFE54AA33EACC4D32F1`
- `FO4CS.pdb`:
  `06B3C6DF38D8DBE141DCE5C6590B4155D3D09020A5AC36D32DF1CFE25B5B0405`
- `ambient_ibl_pass.hlsl`:
  `FDB3A206BBD052B8FDB670683D6D28C3E519EE0056D27BBE2CD174FF347B3BC4`
- `FO4CS_PBR_DefaultOutside1_prefiltered.dds`:
  `45C101C9A299283BBCA66B6C186524CA5FF8E33EAAE213E782C82569F514B116`
- `build_pbr_prefiltered_cubemap.py`:
  `21E6240BE006B67E08B5AF71851D72D3C530414B4A6DDA2B3CAD7D498CC36ED9`

Canonical/build and active MO2 copies of the DLL, PDB, ambient shader, and DDS
were byte-identical at final verification.

### Continuation point

The cubemap/roughness-reflection defect is closed. Do not reintroduce:

- runtime angular cubemap convolution;
- the six-axis rough fallback;
- roughness mapped across a conventional low-resolution mip tail;
- engine probes as the preferred source while the configured prefiltered array
  is available.

Future work can treat the current two-read cube-array path as the known-good
baseline. Any later dynamic/local PBR probe system must produce an equivalent
prefiltered roughness representation before it replaces the shared configured
environment.
