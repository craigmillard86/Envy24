# Porting notes: M-Audio Delta 66 on macOS Sonoma (Intel)

## 1. Card identification (2026-10-25)

Host: Dell Precision T3610, macOS 14.8.9 (23J631), x86_64.

From `ioreg -l -w0` (node `pci1412,1712@4`, pcidebug `8:4:0`):

| Field               | Value  |
|---------------------|--------|
| Vendor ID           | 0x1412 (VIA/ICEnsemble) |
| Device ID           | 0x1712 (ICE1712 / Envy24) |
| Subsystem vendor ID | 0x1412 |
| Subsystem ID        | 0xd632 |
| Revision ID         | 0x02 (chip revision) |
| Class code          | 0x040100 (multimedia audio) |
| IRQ                 | io-apic-0, pin 7 |

The card enumerates, so no OpenCore/PCI bridge work is needed. Device ID is 0x1712, not 0x1724,
so this is the ICE1712 path.

Note: `system_profiler SPPCIDataType` lists only the Quadro K600. It reports devices that have a
driver attached, so the absence of the Envy24 there is expected.

### Matching against ALSA

Reference: Linux v6.12 `sound/pci/ice1712/` (copies in `reference/alsa/`).

The `ICE1712_SUBDEVICE_*` constants in `delta.h` are not PCI subsystem IDs. They are the first
four EEPROM bytes, or, if the EEPROM is blank, the byte-swapped PCI subsystem vendor/ID
(`ice1712.c:2248-2254`):

    swab16(0x1412) << 16 | swab16(0xd632) = 0x121432d6 = ICE1712_SUBDEVICE_DELTA66 (delta.h:29)

So this is a Delta 66 family card. **Classic or rev E cannot be determined from PCI config
space.** ALSA tells them apart only at runtime (`delta.c:624-626`): if the EEPROM GPIO direction
byte is `0xfb` the card is treated as `ICE1712_SUBDEVICE_DELTA66E`. Both variants share the same
PCI IDs, so an Info.plist personality cannot separate them either.

Status: board revision is open. It has to be read from the EEPROM by driver code.

## 2. Building the original driver (Phase 2)

Upstream: https://github.com/vrudikov/Envy24 at `4d66ef3`, imported unchanged into `driver/`.

### Toolchain

There's no Xcode on this machine, only the Command Line Tools (Apple clang 16, SDKs 13.3 to 15.2).
Those include everything needed to build a kext (`Kernel.framework` headers including
`IOKit/audio`, `libkmod.a`, `libkmodc++.a`, `libclang_rt.cc_kext.a`). So instead of
`Envy24_64bit_Driver.xcodeproj`, the top-level `Makefile` builds the same thing:

- Sources: the kext target's six files (`AudioDevice`, `AudioEngine`, `I2C`, `misc`, `ak_codec`,
  `Phase88`), plus `SampleAudioClip.cpp`, which the project built as the static
  `libAudioFloatLib` and linked into the kext.
- `build-support/kmod_info.c` replaces the `_info.c` Xcode generates (the `kmod_info` struct).
- SDK 14.5, `-mmacosx-version-min=14.0`, `-arch x86_64` only, `-mkernel -fapple-kext`, no
  exceptions or RTTI, unsigned (no codesign step at all).
- `make` builds `build/Envy24PCIAudioDriver.kext`.

### Changes needed to compile

Upstream source wasn't changed at all. The only fixes are build settings:

1. `-Idriver`: `misc.h` uses `#include <DriverData.h>`, which Xcode's header map used to resolve.
2. `-Wno-deprecated-declarations -Wno-#warnings`: IOAudioFamily and `IOPCIDevice` are deprecated
   but still present on Intel Sonoma. We're deliberately staying on them, and the warnings drown
   out everything else.

The remaining warnings are left as-is (Phase 2 doesn't change logic): missing `override`,
narrowing `int` to `unsigned char` in `misc.cpp`/`ak_codec.cpp`, shifts of negative values in
`misc.cpp`, and a broken header guard in `misc.h` (`AHI_Drivers_misc_h` vs. the `#define` after it).

`ld` warns that `libkmod.a` was built for 14.5 while we target 14.0. That's harmless; the host
runs 14.8.9.

### Validation (no load)

`kextutil -n -t` no longer works on Sonoma (`-n is not a supported kmutil mode`). What was run instead:

| Check | Result |
|---|---|
| `file` | `Mach-O 64-bit kext bundle x86_64`; `_kmod_info`, `__start`, `__stop` present |
| `plutil -lint Info.plist` | OK |
| `kmutil print-diagnostics -p` | **Dependencies: OK**. Also reports ownership 501:20 instead of 0:0 and a bad code signature, both expected for an unsigned build in a user folder and irrelevant to OpenCore injection |
| `kextlibs` | Reports 487 symbols missing, but that's a false result: on Sonoma library kexts have no binary on disk (they're only in the kernel collection), so `kextlibs` can't see them |
| `kmutil create` (scratch aux collection) | Not possible: needs a KDK matching build 23J631. Not installed |
| Imports vs. XNU `xnu-10063.141.1/config/*.exports` | All non-IOAudio, non-IOPCIFamily imports are exported (`kernel_task` in Mach, `IOMallocContiguous`/`IODelay`/`IOSleep` in IOKit, stack protector in Libkern). `_RESERVED*` vtable slots are handled by the kext linker |
| IOAudioFamily | Loaded on host at 540.3, compatible version 1.0, so the `OSBundleLibraries` requirement of 1.1 is satisfied. Its symbols couldn't be checked without a KDK |

`install.sh` and `go` in `driver/` copy the kext into `/System/Library/Extensions`.
**Never run them.** They're kept only because they're part of the upstream import.

## Open items

### MUST FIX before first load: DMA buffer can land above the chip's 28-bit limit

ALSA limits ICE1712 DMA to 28 bits (`ice1712.c:2468`, `DMA_BIT_MASK(28)`). Upstream
`AudioEngine.cpp:142-182`:

- first tries `IOBufferMemoryDescriptor::inTaskWithPhysicalMask(..., 0x0FFFFFFC)`, which is correct;
- **if that fails, it falls back to `IOMallocContiguous()` with no address limit**;
- if the address is above 256 MB it only logs (`Address of output is above 256 MB!`) and then
  writes the address to `MT_DMA_PB_ADDRESS`/`MT_DMA_REC_ADDRESS` anyway via `ioWrite32`.

This host has 16 GB of RAM and no VT-d remapping (`AppleVTD` has 0 instances, so the card sees
raw physical addresses). If the masked allocation fails, the card will DMA to a truncated address:
silent memory corruption or a panic. The fix is to drop the fallback and fail `initHardware`
instead. That's a logic change, so it's deferred to the start of Phase 3.

### Other

- Board revision (classic vs. rev E) is unknown; it needs an EEPROM read at runtime (section 1).
- `getPhysicalAddress()` is only correct while there's no IOMMU. If VT-d is ever enabled, DMA
  needs to go through `IODMACommand`.
