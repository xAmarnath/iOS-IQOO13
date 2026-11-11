# Recorded results

This document describes the retained results from development. The public archive was prepared from saved material after the project was halted; no additional phone VM test was run for publication.

## Boot and userspace

| Stage | Recorded result |
|---|---|
| Gunyah VM entry | Guest execution on the iQOO 13 |
| Firmware | EDK II boot manager and Darwin-loader handoff |
| XNU | Kernel initialization, virtual platform, BSD and root storage |
| APFS | System/root access and persistent writable Data |
| Process startup | `launchd` followed by the graphical service chain |
| Interface | Apple logo, wallpaper lock screen, SpringBoard and Settings |
| Input | Touch unlock and navigation through the viewer bridge |

The saved About page identifies the guest as iPadOS 27.0.1. Its model name belongs to the virtual board description; it is not a claim that the Snapdragon phone became Apple hardware.

## Graphics

| Check | Result |
|---|---|
| Shared-buffer import | Host GPU and CPU access to the expected backing |
| Guest command execution | GPU-written values, canaries and native completion |
| Turnip probe | Fill, 1,024 compute outputs, triangle rendering and cleanup |
| Standalone host IPC/backend | 1,732 checks, 30 simultaneous contexts, three execution/fence cycles |
| Context lifecycle | Capacity bounds, rejection, destruction and resource/ID reuse |
| Kept pipeline regression | Descriptor policy preserved; GPU dispatch/readback passes |
| Related translation regressions | 27-case suite passed |

These rows retain their original scope. A standalone Android backend check exercises the host graphics service. A guest probe additionally exercises XNU and the transport. Neither turns a screenshot into a sustained frame-time measurement.

## Storage and memory

- A 6 GiB APFS Data reconstruction preserved 29,110 entries and the guest's required volume identity.
- The 2 GiB private-RAM / 1 GiB graphics profile supported the captured graphical runs.
- Scattered shared backing completed the recorded 266,240-page Gunyah qualification at that profile.
- Private and shared allocations have separate guards for alignment, overlap, residency, metadata and lifetime.
- The final 1.5 GiB graphics checkpoint passed standalone GPU checks and then encountered an address-space mapping error in the full VM attempt. The exact resource observations are recorded in the memory document.

## Presentation and interaction timing

The publisher has a 120 Hz ceiling and passed a modeled 119–120 publications/second regression. Short live frame-delivery windows varied widely, with recorded samples from single-digit rates to approximately 56 fps. Other interactions stalled for several seconds. Those samples do not establish sustained application FPS or a uniform interaction latency.

Graphical startup, wallpaper, icons and touch navigation are the demonstrated interface results. Full-panel ownership transfer is a separate architecture step: Android still owns the panel and hosts the viewer. Wi-Fi, Bluetooth and other phone peripherals were not assigned to this iPadOS guest as part of the retained result set.

## Screenshot provenance

| Public file | Saved capture |
|---|---|
| `apple-logo.png` | Guest frame, 2026-11-10 22:35:33 UTC |
| `lock-screen.png` | Guest frame, 2026-11-10 22:36:06 UTC |
| `home-screen.png` | Guest frame, 2026-11-10 19:45:59 UTC |
| `about.png` | Saved phone capture from the 2026-11-10 17:45 session |

The first three images preserve the captured guest frame. The About image removes Android's black letterbox area and masks the visible Bluetooth address; the guest UI text is preserved. Screenshots are retained from different successful stages, not represented as one continuous recording.

## Archive checkpoint

The reorganized archive passed all 16 host-only regression suites with address/undefined-behavior sanitizers. Publication checks cover patch syntax, series entries, Python syntax, local documentation links, image decoding and excluded/private artifacts.

Development is paused. The held VM was released through a normal phone reboot, the stored Data image was privately backed up and checked, and project runtime files and the display app were removed from the phone. The phone returned to stock boot. The source, patches and captures are retained here without deployment secrets, disk images or raw logs.
