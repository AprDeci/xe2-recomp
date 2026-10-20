# Research status — 2026-10-20

This is a Windows research snapshot using the XC2 base 1.0.0 ExeFS and matching base RomFS. It is not a claim of a complete, accurate or consistently playable port.

## Results recorded before this cleanup

- The recompiler generated three modules, approximately 90,632 functions, and linked a roughly 65 MiB executable. These counts describe local game-derived artifacts; none belongs in the source repository.
- Language initialization, basic account/OE/HID services, shared-memory handling, audio-renderer events and save commit compatibility allowed initialization to progress into menus and an opening scene.
- Title/UI and 3D scene content were displayed. GPU-rendered 2D LUT slices were assembled into a 3D texture; model render targets were shown to be sampled through translated GPU virtual addresses.
- The earlier statement that all five model render targets were never consumed was incorrect: it compared guest addresses with GPU virtual addresses. Do not treat the old "missing deferred pass" theory as a current finding.
- Game-source DSP-ADPCM music and PCM16 sound effects were recorded in earlier local runs. Sustained device buffer recycling and 90-second stability runs were observed. Those private audio captures and reports are not shipped here.

These are historical observations, not a full regression run of every scene after repository cleanup.

## Known limitations

- The title/menu background has shown a hard-edged black upper-left quadrant. The preserved check fails on affected frames; this defect is not claimed fixed.
- Accurate rendering of every model, effect and scene has not been verified against matching original-hardware frames.
- Frame rate depends heavily on scene, CPU-side rendering, uploads and synchronization. Old simple scenes without trace reached approximately 25–28 FPS; complex scenes recorded approximately 14–17 FPS with large uploads. These are not performance targets or same-scene results for this snapshot.
- The 3D LUT assembly can synchronize multiple GPU readbacks when its source slices change. No function-level profile proves its current share of the slowdown; do not disable the repair or cache forever without preserving invalidation semantics.
- Audio uses direct unity mixing. Voice gain, attenuation and reverb are incomplete; stereo ADPCM and original-hardware sample equivalence are unverified.
- Region/start/loop audio checks use synthetic silence. They cover counts and boundaries but do not establish correct prediction history for a nonzero start inside a frame. The preroll/frame-cache interaction deserves a separate regression and fix.
- New-save creation and writing have not received full gameplay regression coverage. Existing option-save hashes were preserved during the filesystem migration; build-only validation does not touch saves.
- Keyboard bindings are fixed. The configurable device mappings currently apply to controllers.
- Only the Windows build is supported by this snapshot. Inherited Linux sources remain, but the XC2 audio wrappers and external toolchain are not validated there.

## Retained regression tools

Run from the project root. All diagnostic outputs are local and excluded from Git.

| Check | Input / trigger | What it establishes |
| --- | --- | --- |
| `tests/check_lut3d.py` | Set `SSBU_T24_SELFCHECK` to an existing output directory; check its `lut3d_asm_*.txt` | Corresponding R/G/B axis ranges must exceed a threshold. Cross-channel variation is reported, not asserted; this is not a full color-accuracy proof. |
| `tests/check_model_consumed.py` | Set `SSBU_T26_DIAG=1`; set `SSBU_T26_SURF_DIR` to an existing output directory and `SSBU_T26_SURF_AT` to the scene time; retain stderr | All five fixed base-version model-MRT guest addresses appear via sampled aliases; all required BMPs decode and at least two carry substantial signal. Addresses are scene/version specific. |
| `tests/check_title_quarter.py` | A BMP of the affected title/menu background | Detects a near-black upper-left quadrant. Use the correct scene; a legitimately dark frame is not a universal rendering failure. |
| `tests/audio_vectors.c` | Compiled by `tests/test_project.py` when VS environment is available | Synthetic C decode vectors, sample capacity and existing region/loop checks, without a game or audio device. |
| `tools/difftest.py`, `tools/functest.py` | Optional development requirements and explicitly supplied test input | Recompiler validation tools inherited from upstream; not an XC2 gameplay smoke test. |

For present-source diagnosis of the title defect, `SSBU_T29_SRC_DIR` and `SSBU_T29_SRC_AT` retain the targeted source/final capture facility. F3/F4 and F6 remain available for research. These diagnostic features are normally off. Captures and profiling affect performance; compare clean runs first.

## Cleanup verification — 2026-10-20

- `br.ps1` passed PowerShell syntax parsing. After removing the two inherited Smash-only network debug hooks and their guest instruction signatures, `br.ps1 -BuildOnly` regenerated C for all three modules, reused unchanged generated objects, rebuilt the affected runtime and linked 158 objects successfully. This was not a clean recompilation of every object. Import checking reported zero unresolved strong symbols and 21 unresolved weak symbols; generation reported 1,482 untranslated instructions, so linking does not establish complete instruction coverage.
- Launching `br.ps1 -BuildOnly -Jobs 4` from the parent directory also passed and restored the caller's working directory. Python `compileall` passed for all retained tools and tests.
- All nine `unittest` checks passed, including compiling and executing the synthetic C audio vectors in a VS x64 environment; no tests were skipped. Extraction checks use synthetic inputs, and graphics checks verify that representative bad inputs fail.
- Git ignore checks covered both ExeFS directories, RomFS, module dumps, saves, generated code, the virtual environment, keys, captures and local settings. The source inventory contains 98 files, approximately 1.3 MB; no remote is configured and no commit or publication has been made.
- The option-save SHA-256 remains `B89572AF7EA05866874284BB54761188B93F95BD6CFB46A4C1F1C7B779A17C79`, matching the migration record. This verification did not start the game or access an audio device.

## Cleanup boundary

Runtime sources, required tools and third-party notices moved into this checkout. Module data, shader cache, local settings and saves also moved, but are ignored by Git. Startup uses the project root and a relative RomFS configuration. The launcher no longer has a debug timeout or default trace; repeated LUT-success logging now requires trace.

Captured audio test constants were replaced by synthetic Q11 coefficients and frames with independently reconstructible expected values. Production audio behavior was not changed by that replacement.

Old logs, captures, probes, generated sample C, old virtual environments and historical reports were moved to a private sibling archive outside the Git root. They remain available locally for later research and are not included in this repository.
