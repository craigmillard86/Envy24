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
