# Source and build guide

## What is in this archive

This is a curated source archive: the monitor, virtual-device integration, Android GPU service, viewer, firmware DMA work and kernel/graphics patch series. It contains the key implementation units and host regression coverage, with readable component paths.

External platform trees and toolchains supply XNU/IOKit, EDK II, the Darwin loader, Mesa, metal2vulkan, virglrenderer/drm2kgsl and the NativeAdreno interfaces. Owner-supplied OS assets remain separate from source and are not distributed here.

Source filenames use concise component names. Existing ABI symbols, persisted data identifiers and JNI package names remain compatible with the matching platform trees.

## Quick source checks

Under Linux or WSL, with Python 3, a C compiler and a C++17 compiler:

```sh
python3 tools/check-host.py
```

This runs the host-only monitor suites with AddressSanitizer and UndefinedBehaviorSanitizer. It also generates the guest touch-worker fixture from the included source, substitutes a host memory barrier and runs the modeled IOKit/MMIO test. It does not connect to a phone or start a VM.

`CC` and `CXX` can select compatible compilers. Test binaries and generated fixtures live in a temporary directory and are removed after the command.

## Host monitor

The Android-side monitor is AArch64 Linux code using the included Gunyah UAPI. An example static cross-build is:

```sh
mkdir -p build
aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -Werror -pthread \
  -DVM_VCPUS=2 -DTARGET_MIXED_FOLIOS -DVM_RAM_MIB=2048 \
  src/host/monitor.c -o build/monitor
```

Use the device's matching UAPI and firmware contract. The archived header contains the later 1.5 GiB graphics-pool checkpoint; the compile command alone does not recreate a matched demonstrated 1 GiB firmware/package configuration. `config/` and the memory document separate those profiles.

## Android GPU service

The included builder targets Windows with the Android NDK:

| Setting | Value |
|---|---|
| `ANDROID_HOME` | Installed Android SDK root |
| NDK | `28.2.13676358` |
| Target | `aarch64-linux-android30` |
| `VIRGLRENDERER_SOURCE` | Compatible source tree with `prebuilt-intermediates/config.h` |
| `ADRENO_PM4_SOURCE` | Directory containing `AdrenoPM4.h` and `AdrenoPM4.c` |

```powershell
python src/graphics/host/build.py --service
```

The builder compiles the native drm2kgsl path, the queue and IPC backend, and writes its output under `src/graphics/host/build/`. `--transport` selects the transport probe; omitting both switches selects the native-context smoke executable. These commands build artifacts only.

Expected external headers include virglrenderer context/resource interfaces, DRM/MSM protocol headers, drm2kgsl definitions and AdrenoPM4. The build receipt records source hashes and compiler commands locally; receipts and binaries are ignored by Git.

## Android viewer

The Windows viewer builder requires Python, a JDK, Android build-tools 37.0.0, platform `android-37.0` and NDK 28.2.13676358:

```powershell
python src/viewer/build.py
```

It builds the Java application, GLES/JNI shared-frame reader and scanout/touch bridges. A local development signing key is generated in the ignored build directory. The fixed development password protects a disposable local key and is not a deployment credential.

The viewer needs session-specific endpoints and tokens from the matching controller. Those records are not included in the repository.

## Guest kernel integration

`src/guest/` compiles as part of the matched XNU/IOKit platform build. Add `include/graphics/` to the include search path for `VirtgpuABI.h`. NativeAdreno memory/user-client headers and verified-root policy headers come from the matching platform tree.

The files implement storage registration, persistent Data, clocks, diagnostics, graphics, display and touch. Kernel collections, prelinked payloads and extracted OS components remain private build inputs/outputs.

`patches/xnu/series` specifies the board patch order after the matching arm64e and userspace-platform base series. It is not a patch set for an arbitrary clean XNU release. Apply changes against the intended source layout and review any mismatch rather than using fuzzy application.

## Firmware integration

`src/firmware/virtio-mmio/` contains EDK II virtual-device library changes and the restricted shared-DMA allocator. It requires EDK II protocols/libraries, an appropriate Gunyah platform description and a shared reserved-memory node in the FDT. `tests/firmware/` preserves the allocator's modeled regression fixture.

The boot media needs an agreed firmware/device-tree/kernel contract for RAM, shared regions, disks, interrupts and entry addresses. No firmware or disk image is supplied here.

## Graphics patch series

`config/source-pins.json` records the retained graphics source revision identifiers. Each graphics patch directory contains an ordered `series` file. Mesa patches cover Darwin transport, Turnip behavior and compiler integration; the metal2vulkan series contains the retained local translation/runtime changes.

A syntax check can use `git apply --numstat` per patch. Actual applicability must be checked against its matching source revision and earlier prerequisite patches. Neither a syntax check nor a source pin alone is a complete device build.

## Packaging boundaries

Firmware and monitor address maps, private RAM size, GPU pool geometry, driver ABI and boot arguments must be packaged as a unit. Preserve hashes of those inputs when assembling a session. Keep Boot/OS/SystemOS read-only and make the stored Data image authoritative after it has been used.

Do not publish OS images, signing material, session tokens, raw device logs or identifiers with a source contribution. Component licenses and copyright notices remain with the source and patches.
