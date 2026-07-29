# DSi WiFi Slot Manager

An NDS/DSi homebrew app to back up and restore the console's six WiFi connection slots,
individually, to and from the SD card.

**Status: listing, backup and restore all work on real hardware.** A backup taken on the
reference DSi is byte-identical to the flash region it came from, and restoring it into another
slot puts those bytes back verbatim — the console's own System Settings then shows the restored
slot as a complete connection, SSID and custom DNS included. What exists today is a decoder, a fixture generator, proof
of where the data does and does not live, and a working app.

## The one fact this project rests on

**WiFi settings are not in the NAND.** They live on a separate 128 KB SPI flash chip — the same
one that stores the nickname, birthday and touchscreen calibration. A NAND dump never touches it.

Verified, not assumed. `tools/scan_nand_full.py` decrypts an entire `nand.bin` and scans every
byte — raw and decrypted, MBR through slack space, at every 16-byte alignment — for anything
shaped like an access-point record:

| input | result |
|---|---|
| a real 257 MB DSi NAND, whole image | **0 records** |
| that console's real wifi flash, same scanner | **1 record** — slot 1, the configured network |

The positive control is what makes the zero meaningful.

## Confirmed layout

Read off a real console's `dsfirmware.bin`, not derived:

```
flash size        131072 bytes (128 KB)
console type      flash[0x1D] = 0x57  (DSi -> slots 4-6 exist)
base              0x1FE00     (u16 at 0x20 = 0x3FC0, times 8)
wifi region       0x1F400 .. 0x1FDFF  (0xA00 bytes)
```

Six slots, two families, not interchangeable:

| slots | offsets | size | capability |
|---|---|---|---|
| 1-3 | `0x1FA00` `0x1FB00` `0x1FC00` | `0x100` | Open/WEP only. The DS reads these too. |
| 4-6 | `0x1F400` `0x1F600` `0x1F800` | `0x200` | WPA/WPA2 + proxy. DSi only. |

The code derives these from the header rather than hardcoding them, the same way fwTool does
(`firmware/nds/fwTool/arm9/source/main.cpp:255-267`):

```c
base = (u16 at 0x20) * 8;
slots 1-3 at base-0x400, -0x300, -0x200;
if (flash[0x1D] == 0x57)                  // DSi
    slots 4-6 at base-0xA00, -0x800, -0x600;
```

### Two things the documentation got wrong for us

**The record CRC16 uses initial value `0x0000`, not `0xFFFF`.** Polynomial `A001h` over
`[0x00,0xFE)`, stored little-endian at `0xFE`; slots 4-6 carry a second one over `[0x100,0x1FE)`
at `0x1FE`. `0xFFFF` is what the firmware *header* CRCs use, and assuming it here made every
valid record on a real console decode as corrupt. The right init was found by brute-forcing every
(polynomial, range, init) combination against a slot whose stored value was known. All nine
checksums on the real dump verify with it.

**An unused slot is marked by status byte `0xE7 == 0xFF`, not by being blank.** On the real
console all six slots pass their checksums and none is all-zero or all-`FF`, so either of those
tests reports every slot as populated. The five unused ones read `0xE7 = 0xFF`; the configured
one reads `0x00`.

What an unused slot holds differs by family, so don't generalise from one: slots 4-6 carry
`MTU 1400` and 5 non-zero bytes each, while slots 2-3 carry `MTU 0`, config bits `0x01` and
`0x00`, and 17 and 3 non-zero bytes. Slot 3 is the sparsest — its status byte plus two CRC bytes
and nothing else — and still not blank by a byte test.

## Tools

| file | what it does |
|---|---|
| `tools/decode_wifi.py` | Decodes all six slots from a flash dump. Prints raw bytes next to every interpreted field, so a wrong label can't hide the truth. `--raw` reads fwTool's headerless `0xA00` region. |
| `tools/make_fixture.py` | Builds a synthetic 128 KB flash with known records — test input, and the scanner's positive control before a real dump existed. |
| `tools/scan_nand_full.py` | Decrypts a whole `nand.bin` and proves the settings aren't in it. Needs `pycryptodome`: use `../.venv/bin/python`. |
| `tools/crosscheck.py` | The test suite. Builds the app's own decoder, screens and backup writer for the host, then diffs the decode against `decode_wifi.py`, round-trips a backup byte for byte, checks that malformed backups are refused, restores every record into every slot to confirm it lands verbatim or is refused, and draws every screen on both panes to confirm no line is wider than the console. Compares without echoing, so a real dump's SSID stays out of the terminal. |
| `tools/decode_backup.py` | Decodes a `.dswifi` backup and verifies every record's checksum. Reports passphrase, PSK, WEP keys and WFC ID as a length and hash, never as a value. |
| `tools/host_slotlist.c` | The host build itself. `--list`/`--screens` print what the console draws, `--backup` writes a `.dswifi`, `--restore` applies one to a copy of a dump, `--fields` is what `crosscheck.py` reads. They print SSIDs and write plaintext passphrases, so use them on fixtures. |

```sh
python3 tools/decode_wifi.py dsidump/dsfirmware.bin
python3 tools/crosscheck.py dsidump/dsfirmware.bin
```

## Getting a dump

[`dsibiosdumper`](https://github.com/Arisotura/dsibiosdumper), press **X** ("dump DS-mode
firmware"). Writes `dsidump/dsfirmware.bin`, 128 KB — the whole chip including the header, so the
file is self-describing.

[`fwTool`](https://github.com/ahezard/nintendo-ds-tools) also works (`Backup Wifi Settings` →
`FW<MAC>/WifiSettings.bin`) but dumps only the `0xA00` region with no header, so the offsets have
to be taken on faith. Decode that form with `--raw`.

## The app

Standalone [BlocksDS](https://blocksds.skylyrac.net/) homebrew, structured like
`../SafeNANDManager`. Stock ARM7 (`installSystemFIFO()` is all that's needed); all logic on ARM9,
since `readFirmware`/`writeFirmware` are ARM9-callable wrappers over a FIFO handler libnds
installs itself. Build it with `./run.sh` (docker; no local toolchain needed).

Both screens are used. The bottom one is the step you are on; the top one is a pane that keeps
context across a whole operation, so a five-screen-deep restore never loses track of which file
and which slot it is about to write.

- **List** *(working on hardware)* — decode and show all six slots: number, family, SSID,
  security, in-use or free. The top pane gives the whole decode of the slot under the cursor,
  including the full 32-byte SSID the list row has to truncate.
- **Backup** *(working on hardware)* — back up the slot under the cursor to one file in
  `DSIWIFI/<MAC>/` on the SD, named from the console clock as `wifi-20260728-015530.dswifi`
  (falling back to `wifiNNN` if the RTC was never set). One slot per file. The app warns on screen that the file holds the passphrase in the clear,
  refuses to select a free slot, and re-reads the file it just wrote to confirm it parses before
  reporting success.
- **Restore** *(working on hardware)* — pick a file from any folder under `DSIWIFI/`,
  pick a record, pick a destination slot *in the same family*, confirm, write. If the destination
  is in use the app offers to save a copy of it first, so a restore can be undone. It re-reads the
  slot afterwards and reports what it actually holds.

A slot 4-6 record cannot restore into slots 1-3: a WPA passphrase and its precomputed PSK do not
fit in a `0x100` record. Within a family, any slot to any slot.

### Why writing is safe

- The region is meant to be rewritten — it is where System Settings saves when you configure
  WiFi normally. The protected part of the chip is elsewhere and needs the SL1 pad shorted.
- `writeFirmware` requires a `0x100`-aligned address *and* length. Records are `0x100` and
  `0x200` at `0x100`-aligned addresses, so a restore never has to read-modify-write a
  neighbouring page.
- libnds compares each 256-byte page before programming it and skips it if unchanged, then reads
  back to verify. Restoring a slot onto itself should physically program zero bytes — which makes
  it the ideal first hardware test of the write path.
- The app refuses to write at all unless the header decoded sanely.

### One thing to be honest about

**These records store WiFi passphrases in plaintext.** That is how the hardware stores them, and
a backup file will contain the password in the clear on the SD card. The app should say so rather
than pretend otherwise. `.gitignore` keeps dumps and backups out of git for the same reason.

## Open questions

- The DSi security-type byte at `0x181` was labelled from the order of options in the System
  Settings menu. `0x07` is **confirmed** against a real WPA2-PSK (AES) connection; `0x04`-`0x06`
  are still inferred and keep their `?`. `decode_wifi.py` prints the raw byte alongside, so a
  wrong label cannot hide the truth.
- `scan_nand_full.py` classifies a lone slot-1 record as `TWL` because the `0x200` window
  starting at it happens to satisfy the second CRC. Harmless — it is a detection tool, and the
  decoder uses the derived layout instead — but don't trust its family label.

## References

- [GBATEK: DS Firmware WiFi Internet Access Points](https://problemkaputt.de/gbatek-ds-firmware-wifi-internet-access-points.htm) — field-by-field record layout
- [GBATEK: DS Firmware User Settings](https://problemkaputt.de/gbatek-ds-firmware-user-settings.htm) — the `0x20` base pointer
- [ahezard/nintendo-ds-tools](https://github.com/ahezard/nintendo-ds-tools) — fwTool. Note its
  `Restore Wifi Settings` is `TODO`, never implemented.
- [Arisotura/dsibiosdumper](https://github.com/Arisotura/dsibiosdumper) — full 128 KB dump
- [blocksds/libnds](https://github.com/blocksds/libnds) — `source/arm9/storage/firmware.c`,
  `source/arm7/firmware.c`
- [Zakary2841/WifiManager](https://github.com/Zakary2841/WifiManager) — the 3DS equivalent, and
  the inspiration. Different platform, different storage; a design reference, not a port.

## Licence

MIT, Copyright (c) 2026 Augusto Daniele. See `LICENSE`. That file covers the code written for
this project and nothing else.

Files borrowed from other projects keep their own licences, reproduced in `licenses/`:

- The DSi-mode gate (`arm9/source/dsi_only.c` and its `gfx/` images) and the docker build
  skeleton come from [DS-Homebrew/SafeNANDManager](https://github.com/DS-Homebrew/SafeNANDManager),
  MIT, Copyright (c) 2019 zoogie. Its notice is in `licenses/SafeNANDManager-MIT.txt`, in full,
  as MIT requires.
- The three Makefiles derive from the [BlocksDS](https://github.com/blocksds/sdk) templates,
  CC0-1.0, credited to Antonio Niño Díaz, 2023. See `licenses/BlocksDS-CC0.txt`.

`licenses/README.md` lists which file falls under which.
