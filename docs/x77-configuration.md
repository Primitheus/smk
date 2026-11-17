# X77 onboard configuration

Firmware 2.0 adds five colour modes, key assignments and four macros. These run on the keyboard; the WebUI is needed only to configure shortcuts and macros.

## Keyboard controls

| Shortcut | Action |
| --- | --- |
| Fn+1–5 | Select CM1–CM5 |
| Hold Fn+1–5 for about one second | Edit that colour mode |
| Tap keys while editing | Cycle off and 15 colours |
| Press the same Fn+number again | Save and leave the editor |
| Fn+F9 | Cycle onboard animations |
| Fn+− / Fn+= | Decrease / increase speed |
| Fn+PgUp / Fn+PgDn | Increase / decrease brightness |
| Fn+Up / Fn+Down | Change the animation colour |
| Fn+Delete | Reset lighting settings; stored profiles and macros remain |
| Esc during macro playback | Cancel the macro |

While editing a colour mode, ordinary keys change their lights without typing. Its number key pulses, turns amber while saving, then green after verification or red on failure. Fn remains reserved. Keyboard lighting shortcuts take control from PC effects; selecting a PC effect in SMK WebUI hands control back.

Onboard animations include radial, horizontal and vertical waves, white, breathing, colour cycle, reactive typing, ripple and scanner, plus off. Reactive effects use matrix events, including the ISO backslash key, without host key monitoring.

## Shortcuts and macros

SMK WebUI can assign a key, a modifier chord, one of four macros, or disable a physical key. Fn cannot be remapped. Each macro supports up to eight chords, with a delay from 0 to 2550 ms in 10 ms steps. Chords are held for approximately 40 ms; there is a minimum 10 ms gap between steps. Playback is finite and does not repeat automatically while held. Physical key and modifier states are merged with synthetic reports rather than replaced.

## Storage and updates

The original 590-byte configuration contains five 108-key palettes (4 bits per key), 108 two-byte assignments, four macro lengths and four eight-step macro sequences. Two 1 KiB sectors at `0xe000` and `0xe400` hold alternating records. Code is link-limited to `0xe000`. The stock bootloader at `0xf000` and the original external EEPROM settings record are not used for these records.

Records have a version, length, sequence and CRC-16/CCITT checksum. A save writes the inactive slot, checks its contents, then writes a commit marker. Unchanged records are not rewritten. Configuration uploads are validated before saving; abandoned uploads revert after approximately ten seconds. Power-loss tests exercise each incremental write before the commit marker.

An application-only firmware flash normally erases these sectors too. Back up the current application and preserve valid compatible configuration sectors when upgrading. SMK WebUI also saves a JSON backup before writing a configuration. Keep the stock firmware backup separately.

## HID control protocol

The USB product string stays `SMK X77 Fast v1` for transport compatibility. Query feature report 12 to identify firmware capabilities. Both new feature reports are eight bytes including the report ID.

- **12:** `[12, 'S', 'M', 'K', major, minor, flags, layout]`. Version is 2.3; layout 1 is ISO and 0 is default. Flag bits 0–4 indicate colour modes, macros, assignments, onboard effect selection and PC handoff.
- **11 request:** `[11, tag, offset_lo, offset_hi, data0, data1, data2, 0]`. The low four tag bits select a command; the high four are a request sequence.
- **11 reply:** the same tag and offset, three result bytes, then status (0 success, 1 busy, 2 invalid). The reply tag is published last.

Commands: 1 reads three configuration bytes; 2 uploads the next three bytes; 3 begins an upload; 4 validates and saves it; 5 aborts; 6 reads status; 7 selects an onboard effect; 8 selects CM1–CM5; 9 hands lighting to PC effects. Upload offsets must be sequential. A host polls status and verifies full readback after saving.

Status contains saving, the current effect (bit 7 indicates CM editing), and flags for save failure and keyboard lighting ownership. Configuration and flash operations run in the main loop; the USB interrupt only moves bounded packets and serves prepared responses.

## Firmware 2.1

Fn+0 requests Direct control from the running WebUI. Status byte 2 bit 2 remains set until command 9 acknowledges the request; selecting onboard lighting cancels it. Releasing Fn before 0 still releases the shortcut cleanly. With no host application, onboard lighting continues.

Command 10 returns the compiled Fn keycode at the matrix-cell offset (low byte, high byte, reserved). Cell 28 reports 0xfffe for the fixed Fn+0 Direct shortcut. These mappings are read-only. Command 12 restores onboard lighting defaults; data0=1 also clears Windows-key lock, for a full reset.

Fresh configuration defaults are white, red WASD, cyan arrows, rainbow rows and purple. Valid saved records are never replaced with defaults on upgrade. The configuration format and save-slot layout are unchanged.

## Firmware 2.2

Configuration version 2 is 638 bytes: the previous 590 bytes followed by sixteen RGB triples. Slot 0 must be black (Off). The remaining fifteen colours are shared across CM1–CM5. Valid legacy 590-byte records are migrated in RAM with the original palette, and written in the new format on the next save. CRC validation uses the stored record length; upgrades must preserve either valid record length.

Status flags include Windows-key lock in bit 3. The physical Windows key shows red while locked, including in Direct mode or with normal lighting off. When the matrix transitions to all keys released, physical reports and remap overlays are cleared to prevent stale modifiers. This does not cancel a running finite macro.

## Firmware 2.3

Fresh CM defaults are Glacier (cool white/cyan), Arena (gaming keys), Midnight (blue/violet), Sunset (warm gradient) and Aurora (green/cyan/violet rows). Existing records and custom palettes are preserved. Apply the new defaults explicitly in WebUI when wanted. The configuration format is unchanged from 2.2.
