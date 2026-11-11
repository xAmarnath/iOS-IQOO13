# Adreno 830 graphics integration

## The end-to-end route

```text
Guest application / graphics service
    Metal API
    metal2vulkan translation
    Mesa Turnip, Darwin virtgpu transport
    Guest graphics ABI and XNU virtio GPU driver
        shared commands, resources and fences
Host Gunyah monitor
    bounded IPC to native-context GPU worker
    drm2kgsl → stock KGSL → Adreno 830
        completion and readback return to the guest
```

The guest uses a virtual GPU interface backed by the phone's real Adreno hardware. Android's KGSL driver manages physical GPU access, firmware and host memory imports. This is a mediated graphics architecture.

## Guest driver and ABI

`VirtioGpu3D.cpp` exposes resource creation, mapping, submission and fence operations through the guest accelerator user client. `VirtgpuABI.h` defines the shared interface: fixed-width records, methods, contexts, resource handles and error behavior.

Each client owns a context. Resource mappings and commands are checked against that context and its buffer bounds. User-client closure, GEM-style close operations and queue completion must agree on when backing memory can be recycled.

`Gpu.cpp` connects the virtual transport to the guest accelerator service. Physical SMMU/ring/timer classes used by a direct hardware driver are not attached in this virtual platform; the native-context path supplies their relevant work.

## Host GPU worker

The monitor sends bounded requests and shared-memory descriptors to a separate renderer process. The worker validates message lengths, descriptor types, offsets, ranges, duplicate resources, context identifiers and lifecycle transitions before using the native backend.

The renderer does not receive VM-owner descriptors. This prevents a renderer restart or close operation from silently releasing the VM's memory-ownership state.

The response arena supports **30 simultaneous contexts**, respecting its size and the native address-space slice limit. Tests cover maximum response allocations, nonoverlap, over-capacity rejection, duplicate IDs, busy destruction, cleanup and ID/arena reuse.

The host driver accepted a 192 GiB virtual buffer-object address space in the recorded import matrix. A 256 GiB virtual reservation was accepted initially but subsequent imports failed, so the compatible geometry was selected. These are virtual address-space sizes, not RAM allocations.

## Shared buffers and IOSurface

Guest-private LEND pages cannot be imported into Android. The copied-host path therefore tracks CPU-visible guest storage separately from shared GPU backing:

1. Resolve the source plane, stride, offset, format and alias relationship.
2. Allocate or reuse shared backing within the pool budget.
3. Upload the required CPU content into that backing.
4. Submit translated commands through Turnip and the native context.
5. Wait for completion before consuming or merging output.
6. Retain the relevant source/backing relationship until queued work has finished.

Changes address read-only imports, copied-page lifetime, texture aliases, rotated planes, scratch buffers and resource recycling. These are correctness requirements as well as performance work: a stale alias or premature recycle can produce missing content even when command submission succeeds.

## Turnip and translation changes

| Area | Included work |
|---|---|
| Darwin transport | Virtgpu methods, guest mappings, native-context resource operations |
| Address space | High shared windows, VA geometry, release and exhaustion handling |
| Scheduling | Queue priority and deadline handling |
| Allocation | Lazy buffer objects, cache budgets, oversized-buffer rules, pressure fallback |
| Command streams | Empty/failed stream cleanup and bounded allocation errors |
| Image operations | Host import, rotated copies, scissor handling, resolve preferences |
| Metal resources | Copied-host storage, aliases, read-only inputs and backing coverage |
| Recycling | Scratch caps, pressure trimming, descriptor policy and kept-object lifetimes |
| Diagnosis | Placement, completion, import and allocation counters with bounded output |

The kept-descriptor regression captured a concrete failure: a recycled pipeline changed its dynamic bindings while retaining an incompatible pipeline layout. Preserving descriptor policy with the kept object allowed the same test to dispatch GPU work and verify the expected values. The related 27-case suite passed.

## Display delivery

GPU execution and screen presentation are separate boundaries. The guest produces a 1536 × 2048 image. Completed scanout is exported through a shared 12 MiB frame and published into the Android viewer's three-slot ring. JNI/GLES uploads the chosen frame for display.

The viewer selects recent completed output rather than replaying stale frames. A host regression exercised 119–120 publications per second under modeled timing. That checks the publication policy, not the guest compositor's sustained rendering rate.

The saved wallpaper lock screen, home screen and Settings captures establish real graphical output. Hardware GPU tests establish command execution, compute, raster and readback. Their results do not measure how much of every UI frame used the hardware path. Actual interaction timing varied by scene and run; a sustained 120 fps user interface is not a result claimed by this archive.

## Hardware results

- Guest-initiated command writes and GPU-written readback with intact canaries.
- Native fence completion, including recorded approximately 1 ms command rounds.
- Turnip fill, 1,024 compute values, triangle rendering and cleanup.
- CPU aliases of shared imported buffer memory.
- Standalone Android GPU/IPC qualification: 1,732 checks, 30 contexts, three execution/fence cycles and resource reuse.
- Regression coverage for copied backing, barriers, descriptors, resource pressure and queue error paths.

Host-side qualification and guest-side probes are labeled separately because they exercise different sections of the route.
