# Fallout 4 Community Shaders PBR / PBRM handoff

Date: 2026-07-22
Status: the editor is feature-complete at PBRM v5. All editor-side work through
the surface-state architecture slice has shipped, is tested (222 checks, zero
failures), and is deployed to `publish`. The remaining campaign is runtime-side
(#38) in the separate Community Shaders repository, which already carries a
validated PBRM v4 slice that must be upgraded to v5.

## What this project is

`PBR Material Editor` — a standalone C++20/Qt 6 authoring tool for the `.pbrm`
material format that the Fallout 4 Community Shaders fork (FO4CS) consumes.
Written from scratch; it does not reuse the legacy C# editor. The README covers
the feature surface; `docs/PBRM-v5.md` is the format spec;
`docs/SurfaceStates-Architecture.md` is the authoritative design for the v5
surface-state split and the future runtime catalog.

## Current architecture (v5, 2026-07-22)

**The material describes what a surface is; the runtime describes what happens
to it.** Surface state behavior (wetness, dust, frost, heat, blood — and later
wear, rust, soot) lives in a runtime-owned `FO4.PBRM.SurfaceStateDefinitions`
JSON, never in materials. State amounts are runtime data bound by state name.
Materials carry physical inputs only:

- Porosity in RMAOS alpha (`alphaCarries` selector; fallback
  `saturate(roughness × (1 − metallic))`; constant via `overridePorosity`,
  which is exempt from the empty-texture auto-force so the heuristic fires).
- Signed curvature in Normal alpha (`curvatureInAlpha`; 0.5 neutral, below =
  convex edges; no constant — map-or-AO-derived). Consumers: state placement,
  cavity specular occlusion (`cavitySpecOcclusion`, direct specular only,
  authored-map only), SSS scatter broadening, convex thickness fallback.
- `SurfaceResponse` settings: `adhesion` constant, `shelterChannel`
  vertex-color binding, `maskR..A` state-name bindings into the
  `primaryStateMasks` socket.
- AO / height / roughness / metallic / emissive are read directly by the
  definitions' map-guidance weights.

The old per-material `SurfaceStates/State0..7` groups and the 4-channel
`SurfaceStateResponse` socket are retired. v4 files load through an in-memory
migration (constants fold into their new homes; legacy groups stay inert until
Batch Operations extracts them to a definitions JSON and strips them). The
layer-mask-in-Normal-alpha concept is retired too: layer breakup is Height
Blend coverage or the dedicated Layer Coverage socket.

Editor touchpoints: the Surface Response page (replaced the Surface States
page), Settings > Surface State Definitions (export starter JSON / load a
modded set / built-in defaults), the preview States tab (simulates the active
definitions with weather envelopes and tag physics — verified live 2026-07-22
with Wetness 1.0 darkening, glossing, and rippling the X01 sphere), and the
channel packer (mean-preserving packed-data mips, vector-averaged and
renormalized normal mips, no premultiplied-alpha bleed; optional Toksvig
normal-variance baking into roughness mips on RMAOS exports).

## Paths

- Editor source (canonical): `E:\Projects\Fo4CommunityShaders\PBRMaterialEditorQt`
- Portable build the user launches: `...\PBRMaterialEditorQt\publish\PBR Material Editor.exe`
  (always re-run `scripts\package.ps1` after builds)
- GitHub: `https://github.com/BungoV/FO4-PBR-Material-Editor` (GPLv3, WIP)
- FO4CS runtime repo (campaign #38): `E:\Projects\Fo4CommunityShaders\fallout4-community-shaders`
- Active MO2 FO4CS mod: `E:\Projects\Fallout 4 Mods\mods\FO4CS`
- PBR test mod: `E:\Projects\Fallout 4 Mods\mods\PBR`
  (deliberate single test NIF: `meshes\actors\powerarmor\x01\x01_torso.nif`)
- Wild Wasteland NifSkope fork (future integration): `E:\Projects\ClaudeNifskope`
- Legacy C# editor (reference only): `E:\Projects\Fo4CommunityShaders\PBRMaterialEditor`

## Build, test, package

```powershell
$env:Path = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:Path
qmake6 PBRMaterialEditor.pro
mingw32-make -j4

Set-Location tests
qmake6 MaterialDocumentTests.pro
mingw32-make -j4
..\bin\MaterialDocumentTests.exe

# package (from the project root); the user launches the publish copy
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\package.ps1
```

Verification workflow that works: `--ui-smoke <png>` exits non-zero unless all
preview programs link (shader edits only fail at runtime — always run it; set
`PBRM_SHADER_LOG` for diagnostics); deeper GUI checks drive the live window via
PrintWindow (`PW_RENDERFULLCONTENT=2`) plus PostMessage `WM_LBUTTONDOWN/UP`
client coordinates and `WM_CHAR` typing (resolve the hwnd from the process
`MainWindowHandle`; the window can restore onto the second monitor, which
breaks screen-coordinate assumptions but not client-coordinate clicks).
Release flow: `scripts\package.ps1` → zip `publish\*` → `gh release create`
(portable gh at `E:\Tools\gh-cli\unpacked\bin\gh.exe`, authed as BungoV).

## FO4CS runtime state (as of the 2026-07-16 sessions)

The runtime repo already contains a **user-validated** PBRM v4 slice:

- Direct `.pbrm` root-name interception at
  `BSShaderMaterial::ReceiveValuesFromRootMaterial` plus a same-name `.bgsm`
  sidecar replacement mode; `BSResourceNiBinaryStream` loads with loose/BA2
  parity; fail-closed caching; capability `requirements` gating; renderer
  bridge ABI 1 (Base `t0`, Normal `t1`, RMAOS `t2`, 96-byte `b13`).
- The Standard/Primary-UV binding slice with per-channel feature bits and
  debug channel views; hot reload from the in-game menu; the six X01/sphere
  test sidecars applied successfully in game.
- The environment is a known-good offline GGX-prefiltered `TextureCubeArray`
  (`sDefaultCubemap`, 8 roughness cubes × 6 faces × 3 mips, generator at
  `scripts/build_pbr_prefiltered_cubemap.py`). The user confirmed the rough
  reflections in game. Never reintroduce runtime angular convolution, the
  six-axis fallback, or roughness across a conventional low-res mip tail.
- Host suite last stood at 831/834 (the three failures are unrelated LUT
  tests missing `res/LUTs/neutral_32.dds`). Build via portable xmake
  (`f -m releasedbg`, `build FO4CS`, `build f4fx_tests`).

Caution: that repository previously had a damaged git index (`.git/index`
missing beside a stale `index.lock`, with the PBR work living only in the
working tree). Verify its state before any git operation; never normalize it
blindly.

## Guardrails that still apply

- The editor is `.pbrm`-only **as an editor**. The conversion campaign adds
  a **read-only** BGSM/BGEM importer (2026-07-22 decision); the editor
  still never writes or edits Bethesda materials.
- Only the one X-01 torso test NIF in the PBR mod is deliberate; do not
  bulk-edit NIFs there. (Bulk *conversion* is now the explicit goal of the
  converter campaign — but its output lives in its own output mod folder
  and never touches source assets.)
- Preview cubemaps and preview settings are editor preferences (QSettings /
  `publish\preview-environments.ini`) and are never serialized into `.pbrm`.
- Any combo box inside a scrollable page needs `WheelFocusGuard::install`
  (the preview widget sweeps all its combos in the constructor).

## Backlog (agreed 2026-07-22, in order)

1. **Validation quick-fixes** — Overview → Validation becomes structured
   issue rows with per-row Fix buttons where a safe automatic remedy
   exists (undo-able via `applyDocumentChange`, immediate re-validate);
   texture-file findings stay manual by design. The widget is reused by
   the Convert workspace.
2. **`pbrm.exe` CLI companion** — full CLI parity is a standing rule:
   every headless-capable operation gets a verb, and new features ship
   with theirs. Console-subsystem binary from the same core sources; verbs
   for existing operations first (`validate`, `migrate`, `extract-states`,
   `strip-legacy`, `repair-paths`, `set-setting`, `pack`,
   `export-definitions`, `export-json`); `--json` reporting; explicit
   source/output flags with QSettings only as fallback.
3. **FO4/FO76 → PBR converter** — `docs/FO4-Conversion.md` is the
   authoritative spec: per-title source profiles (FO4 metal-darkness
   recovery vs. FO76 true-specular reconstruction), emissive color rules,
   the four-fate parameter audit with root-material chains, derivation
   pipeline (derivation-side denoising, Poisson height → AO/curvature),
   geometry AO with canonical-instance island baking, correction masking
   with Substance-style fills, the Convert workspace queue,
   dedup/manifest/rollback, and the `convert`/`bake-ao` verbs. Finish
   line: dry-run + convert across the entire vanilla Fallout 4 texture
   BA2 set.
4. **Runtime campaign** (#38, below).

## Next: runtime campaign (#38, FO4CS repo)

The editor's preview fragment shader is the reference implementation. In
order:

1. Upgrade the runtime PBRM parser to v5: accept envelope/schema 4-5, the new
   alpha semantics (`alphaCarries`, `curvatureInAlpha`), feature bits 9-10,
   and the `SurfaceResponse` bindings per `docs/PBRM-v5.md`.
2. Freeze/ship the definitions asset (export the starter JSON from the
   editor's built-in defaults; decide file location, load order, hot reload).
3. Weather → state-amount binding by name; the tag interaction physics stays
   fixed-function (`Pbr::surfaceStateInteractions` is the reference).
4. Port the state block + lighting-side curvature consumers from the editor
   shader (fold-don't-permute: uniform branches or scale/bias folding, no new
   shader variants — see the doc's Performance model).
5. Later: the future runtime catalog in `docs/SurfaceStates-Architecture.md`
   (wear/condition states, decal physics inheritance, corona, moss…), which
   needs the noted schema generalizations.

## Communication notes

The user prefers direct, simple answers. Keep implementation updates brief and
stay on the current task. The user owns in-game validation; the agent owns
editor-side GUI verification via the harnesses above.
