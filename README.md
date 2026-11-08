# RDR2 Vulkan FG

Fork of [RDR2 Vulkan Frame Generation 0.82](https://github.com/mor3a/rdr2-vulkan-frame-generation) (MIT — original `LICENSE.txt` and copyright retained; see [ACKNOWLEDGEMENTS.txt](ACKNOWLEDGEMENTS.txt) and [README-0.82-original.md](README-0.82-original.md) for the upstream project, its requirements and controls).

This branch (`hudfield`) is the field build used to bring **Intel XeSS 2x frame generation to RDR2 on native Vulkan with an Intel Arc B390**, running alongside [OptiScaler](https://github.com/optiscaler/OptiScaler). Validated on real hardware: ~36 rendered → ~72 displayed fps at 3440x1440.

## What this fork changes (tag `0.82+hudfield-v4.1`)

- **`XeSSUiMode` (INI, 1–5)** selects the XeSS-FG UI composition mode; **`XeSSUiComposition` (-1/0/1)** forces the composition state. Used to A/B the translucent wheel-menu flicker against Intel's documented backbuffer-extraction weakness.
- **`XeSSSynthUI` (INI, 0/1, default 0)** synthesizes the premultiplied UI color+alpha layer the SDK demands for UI modes 3/5: a compute shader solves `UI = B − (1−a)·C` from the presented frame (B) and the HUD-less capture (C) of the same epoch (compositional identity reproduces B for any alpha estimate; mode 5's backbuffer extraction refines edges temporally). Mode 2 is always clamped to 4, and modes 3/5 clamp with a log line while the feature is off — instead of letting the SDK refuse the swapchain and generate nothing. The synthesis pipeline (and its ~20 MB/slot staging ring) is allocated only when the key is enabled.
- **Menu/hotkey diagnostics**: `menu: open/close`, first observed game-queue clicks, and hotkeys rejected by wrong focus — added because a field session produced zero menu input events.
- **No network calls**: the upstream Nexus update checker's mod id is set to `0` in this fork, so nothing queries or links a third-party page.
- **Opt-in present-transfer HUD capture** (`CapturePresentTransfers=1`) for third-party Vulkan SR overlays (kept off by default; never needed in the validated session).
- User-visible strings are branded **RDR2 Vulkan FG**. The MIT license file is unchanged.

## Releases

The `hudfield` branch carries reproducible Windows x64 builds. The current package (`RDR2VulkanFG-hudfield-v4.1-B390.zip`) contains the `.asi`, the diagnostics DLL, checksums and a field-test guide (UI-mode A/B grid). It includes **no** NVIDIA/Intel/Rockstar binaries — the game runtime files are not redistributed here.

## Building

Upstream [docs/BUILD.md](docs/BUILD.md) applies. In practice we cross-compile on Linux with `zig c++ -target x86_64-windows-gnu` (zig 0.17.1; same pinned dependencies as upstream `Build.ps1`). A reproducible driver script lives in [rdr2-ngxbridge/tools/build-mor3a-zig-linux.sh](https://github.com/leonardocapellaro/rdr2-ngxbridge/blob/master/tools/build-mor3a-zig-linux.sh). The ASI loads and unloads cleanly under Wine; RDR2 validation runs on the target Arc hardware.

## Known limits

- Base is 0.82, not the 1.0 beta: the beta never reaches FG under OptiScaler (fails its HUD-less gate before the first interpolated frame).
- Wheel menus can flicker in the default UI mode 4 (known SDK weakness for semi-transparent UI). The mode grid in the release guide is the measurement protocol; the proper fix (synthesized UI texture for mode 5) ships opt-in as `XeSSSynthUI=1` — its edge quality is still field-validation pending.
- Multiplier switching in the menu was never confirmed on hardware: no input events reached the mod in the last session; this fork's diagnostics exist to localize that.
