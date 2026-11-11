# Memory, storage and resource profiles

## Demonstrated graphical profile

| Resource | Size | Ownership / purpose |
|---|---:|---|
| Private RAM | 2048 MiB | Exclusively lent to the VM after loading |
| GPU pool | 1024 MiB | Shared host/guest graphics backing |
| GPU response prefix | 32 MiB | Inside the GPU pool; context responses |
| GPU commands | 16 MiB | Separate shared transport region |
| Block DMA | 1 MiB | Shared disk-transfer buffer |
| Scanout | 12 MiB | Shared completed guest image |
| Viewer ring | 36 MiB | Android presentation, three frames |
| vCPUs | 2 | Separate run pages and execution threads |
| Data disk | 6 GiB | Persistent writable APFS volume |

The host advertises 12 GB RAM and reported roughly 10.86 GiB of Linux `MemTotal`, alongside configured swap. These guest allocations use real resident system memory. Swap does not replace memory that must remain pinned and accessible under the Gunyah ownership contract. Host kernel, Android services, drivers and bookkeeping add to the table's allocations.

The 32 MiB response arena is a partition of the graphics pool, not an extra 32 MiB allocation. The viewer ring is outside the guest private RAM.

## Address map

Addresses below are guest physical addresses unless labeled otherwise.

| Region | Start | Size / role |
|---|---|---|
| UART | `0x000003f8` | Emulated serial registers |
| RTC | `0x00002000` | Virtual clock registers |
| Report/diagnostics | `0x10000000` | Bounded control/report page |
| Block devices | `0x10001000`–`0x10004000` | Four virtio MMIO interfaces |
| GPU device | `0x10005000` | Virtio GPU MMIO |
| Block DMA | `0x14000000` | 1 MiB |
| Scanout | `0x15000000` | 12 MiB |
| GIC redistributor | `0x3ffd0000` | Interrupt controller |
| GIC distributor | `0x3fff0000` | Interrupt controller |
| Private guest RAM | `0x80000000` | 2 GiB profile |
| GPU commands | `0x200000000` | 16 MiB |
| GPU pool | `0x201000000` | 1 GiB demonstrated / 1.5 GiB later source checkpoint |

The graphics windows moved into a high guest address range as capacity grew. The map separates private RAM, shared graphics and the interrupt-controller ranges. It must agree across firmware/device tree, monitor, guest driver and allocator contracts.

## LEND and SHARE

**LEND** transfers exclusive access to private guest pages. The host loads these pages before transfer and does not inspect or import them into its GPU afterward. Guest processes, kernel stacks and private application memory live here.

**SHARE** allows both sides to access the designated I/O pages. Device queues, block DMA, scanout, commands and GPU buffers live in shared ranges. Shared access does not imply unrestricted access: every command still needs bounds, ownership and lifetime checks.

This distinction matters for IOSurface. An address valid in the guest's private space cannot simply become a host KGSL buffer. The graphics path stages private CPU content into shared backing and merges completed results where required.

## Scattered backing and folios

Contiguous guest addresses do not require one physically contiguous host allocation. The implementation walks and validates the pinned backing, coalesces compatible physical runs and prefers large folios where available. Scattered pages remain usable under the supported mapping contract.

The monitor checks alignment, page residency, physical geometry, integer bounds, region overlap and metadata capacity. The private-memory metadata guard remains 131,072 folio records. A huge-folio preference improves representation and mapping costs without claiming that all RAM is one giant contiguous block.

The 1 GiB graphics configuration completed the recorded 266,240-page shared mapping qualification. Standalone GPU import/IPC checks also passed with scattered backing. GPU import and Gunyah address-space mapping are separate operations; success of one does not establish success of the other.

## Profile history and archived checkpoint

The project used 5 GiB, 3 GiB and 2 GiB private-RAM configurations while working through boot, pressure and host allocation constraints. Graphics backing was expanded separately. The two-CPU, 2 GiB private / 1 GiB graphics profile produced the retained graphical captures.

The final source checkpoint increases the graphics pool to **1536 MiB**. Its standalone GPU/IPC qualification passed 1,732 checks. During the later full mapping attempt, Gunyah rejected a mapping at guest address `0x216997000` with an address-space error translated to `ENOMEM` at 133.217 seconds. Available host RAM was about 3.1 GiB at that observation, so the error alone does not establish general host RAM exhaustion.

That run's shared backing contained 379,904 base pages, 34 huge folios and approximately 62,294 physical runs. The owner entered the retained recovery state. The archive preserves this source checkpoint separately from the demonstrated profile; see `config/`.

A graphics-pool size change must update the monitor, guest allocator, device tree and package contract together. Editing a single number is not a complete profile change.

## Persistent storage

Boot, OS and SystemOS are read-only disk inputs. Data is a separate writable APFS image. The 6 GiB Data expansion rebuilt the filesystem while preserving 29,110 entries and the seed/volume identity required by boot; extending the host file alone would not expand APFS.

The guest's Data disk is a persistent file, not a RAM disk. The phone's image becomes authoritative after a session writes to it. An older laptop snapshot must not overwrite that file during a later launch.

At archival shutdown, the stored Data image was copied to a private compressed backup and verified by the decompressed byte count and SHA-256. Guest image files were then removed from the phone. The public repository contains no disk data.
