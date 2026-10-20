# Source provenance and third-party notices

## WindowSmash source

This research snapshot derives from [SNO2207/WindowSmash](https://github.com/SNO2207/WindowSmash), upstream commit `5e048ab38e681e24338e145af9d277470f5cedf3`. The local upstream checkout was shallow; this identifies the imported baseline, not a complete authorship history.

The upstream NOTICE states: **"No project-wide license has been specified for the WindowSmash source outside the separately identified third-party material below."** That statement is preserved here. This repository does not grant a new project-wide license over inherited code. Confirm the required permissions before publicly redistributing it.

The imported baseline includes XC2-specific local changes and additional sources: language/region initialization, account and OE services, HID state and shared memory, generic IPC dispatch, audio renderer timing and DSP-ADPCM/PCM16 output, save commit compatibility, QUADS handling, 3D LUT assembly and surface alias handling. This cleanup relocates the build, replaces captured audio test vectors with synthetic vectors, removes two inherited Smash-only network debug hooks and their guest instruction signatures, and preserves useful regression checks. Original source comments and notices remain applicable.

## libvpx

`runtime/gpu_vp9_tables.h` contains default VP9 probability tables derived from libvpx. The upstream WebM Project notices from tag `v1.14.1` are retained in full:

- [third_party/libvpx/LICENSE](third_party/libvpx/LICENSE)
- [third_party/libvpx/PATENTS](third_party/libvpx/PATENTS)

## libopus

The libopus 1.3.1 notice is retained in [third_party/opus/COPYING](third_party/opus/COPYING), for builds or runtime packages that statically link that codec. No codec binary is committed here.

## SDL3 and other external dependencies

Controller input can use [SDL3](https://www.libsdl.org/), copyright Sam Lantinga, under its [zlib license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt). SDL3 source and binaries are not included. If you distribute `SDL3.dll`, include the original development package's `LICENSE.txt` as well.

Other external tools and libraries retain their own licenses: LLVM/Clang, Visual Studio and Windows SDK, PyCryptodome (`pycryptodomex`), optional Capstone/Unicorn, libopus and libvpx. Obtaining, linking or distributing them requires retaining the notices applicable to the actual packages used.

ADPCM arithmetic has an existing Dolphin rounding-reference comment. It is retained as provenance, not a claim that a full source/permission review has been completed. Audio self-test coefficients, packed frames and expected PCM values in this snapshot are synthetic and include a reproducible exact-arithmetic recipe.

The inherited `runtime/linux/` compatibility sources are retained for research. The Linux build/distribution toolchain is not part of this Windows snapshot and has not been validated for the XC2 hooks. No Linux dependency package or binary distribution is provided.

## Game content and names

The intended Git source tree excludes Nintendo game packages, modules, translated game functions, RomFS, assets, fonts, keys, firmware, memory/surface/audio captures and personal saves. They are local user-supplied inputs or generated artifacts, not repository deliverables.

Xenoblade Chronicles 2, Super Smash Bros. Ultimate and associated names are the property of their respective owners. This is independent research and is not affiliated with Nintendo.
