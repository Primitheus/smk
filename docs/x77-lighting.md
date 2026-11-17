# CIY X77 lighting

See [onboard configuration](x77-configuration.md) for firmware 2.0 colour modes, macros and keyboard shortcuts.

This fork adds host-generated lighting to the X77 while retaining normal keyboard input, onboard lighting and the ISP entry feature. The lighting transport is enabled only for the X77. Host effects can be changed without reflashing.

## Build

Use the upstream build dependencies and instructions with these options:

```sh
meson setup build --buildtype release -Disp=enabled -Dboot=disabled -Dusb_product='SMK X77 Fast v1'
ninja -C build ciy-x77_iso_smk.hex
```

This builds an application for the existing stock bootloader. It does not install SMK's replacement bootloader. Keep a verified backup of the individual keyboard's stock firmware and use an application-only flashing procedure that preserves its bootloader. A build succeeding does not guarantee recovery from a bad flash.

## HID transport

The vendor collection uses usage page `0xFF32`, usage `1`. Output report 10 is 64 bytes including its report ID:

| Bytes | Meaning |
| --- | --- |
| 0 | Report ID: 10 |
| 1 | Magic: 70 (`F`) |
| 2 | Protocol version: 1 |
| 3 | Frame sequence, wrapping at 255 |
| 4 | Chunk index, 0–5 |
| 5 | Number of RGB cells in the chunk |
| 6 | Flags: 0 for data, 1 to release host lighting |
| 7–63 | RGB triples, padded to report length |

A frame contains 108 electrical cells, in row-major order for a 6×18 matrix. Send six chunks containing 19, 19, 19, 19, 19 and 13 cells. Only 88 cells correspond to physical UK ISO keys. A release report uses chunk 0 and count 0.

The endpoint remains NAKed until the main loop copies a packet; the interrupt only marks it pending. Frames are checked and assembled before display. Legacy feature reports 8 and 9 remain available. Inactivity returns control to onboard lighting.

SMK WebUI generates the effects on the host at a target of 30 frames per second. This is a host submission rate, not a measurement of the physical LED scan rate.

## ISO onboard geometry

The ISO backslash key beside left Shift is wired to row 4, column 14. Onboard horizontal and radial animations remap that position to physical column 1 and shift Z through slash one position right. The default layout keeps its original geometry. This affects animation coordinates only; matrix scanning, LED addresses and host streaming remain unchanged.
