#!/usr/bin/env python3
"""Build a synthetic 128 KB DSi wifi flash containing known AP records.

This is the positive control for the NAND scanner: if a scanner cannot find records that
are definitely there, "0 found in the NAND" means nothing. It doubles as the test fixture for
the decoder, since no real dump exists yet.

Layout follows fwTool (firmware/nds/fwTool/arm9/source/main.cpp:255-267):
    userSettingsOffset = u16 at 0x20, times 8
    fwSize             = userSettingsOffset + 0x200
    AP1..AP3 at base-0x400 / -0x300 / -0x200   (0x100 each)
    AP4..AP6 at base-0xA00 / -0x800 / -0x600   (0x200 each, DSi only, gated on header[0x1D]==0x57)
"""
import sys

SIZE = 128 * 1024
BASE = SIZE - 0x200            # what fwTool's formula must reproduce: fwSize = base + 0x200
NTR_LEN = 0x100                # slots 1-3
TWL_LEN = 0x200                # slots 4-6

TABLE = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ (0xA001 if c & 1 else 0)
    TABLE.append(c)


def crc16(data):
    """Polynomial A001h, initial value 0000h -- confirmed against a real console's slot 1."""
    c = 0x0000
    for b in data:
        c = (c >> 8) ^ TABLE[(c ^ b) & 0xFF]
    return c


def ntr_record(ssid, wep_mode=0x00, status=0x00):
    r = bytearray(0x100)
    r[0x40:0x40 + len(ssid)] = ssid.encode("latin-1")
    r[0xE6] = wep_mode
    r[0xE7] = status
    r[0xEF] = 0x00
    r[0xFE:0x100] = crc16(bytes(r[:0xFE])).to_bytes(2, "little")
    return bytes(r)


def twl_record(ssid, passphrase, security=0x07, status=0x10):
    """A configured slot 4-6 record, matching the one real WPA record on hand.

    status is 0x10, not the 0x00 an NTR record uses: a real configured slot 4 on the
    reference console reads 0x10 there. MTU is 1400, which is what System Settings
    writes. Both were guessed before a real WPA record existed; guessing produced a
    fixture that no console would ever have written.
    """
    r = bytearray(0x200)
    raw = ssid.encode("latin-1")   # one byte per character, so 0xE8 below is the true length
    r[0x40:0x40 + len(raw)] = raw
    r[0xE7] = status
    r[0xE8] = len(raw)                        # SSID length, populated in both families
    r[0xEA:0xEC] = (1400).to_bytes(2, "little")   # MTU, as System Settings writes it
    r[0xEF] = 0x01                            # config bits, as seen on hardware
    r[0xFE:0x100] = crc16(bytes(r[:0xFE])).to_bytes(2, "little")
    r[0x100:0x120] = bytes(range(0x20))       # stand-in for the precomputed PSK
    r[0x120:0x120 + len(passphrase)] = passphrase.encode()
    r[0x181] = security                       # 0x07 == WPA2-PSK (AES)
    r[0x1FE:0x200] = crc16(bytes(r[0x100:0x1FE])).to_bytes(2, "little")
    return bytes(r)


def empty_record(length):
    """An unused slot as real hardware writes it, NOT an all-zero block.

    This matters more than it looks. A slot is marked unused by status byte 0xE7 == 0xFF; an
    all-zero record has 0xE7 == 0x00, which every consumer reads as "configured". A fixture built
    from zero blocks reports all six slots populated, so a free-slot list view tested against it
    passes while being wrong -- which is exactly the misreading the hardware finding warns about.

    The reference console's unused slots also carry MTU 1400 on slots 4-6 and 0 on slots 2-3, and
    valid checksums throughout. Reproduced here so the fixture exercises the same edge.
    """
    r = bytearray(length)
    r[0xE7] = 0xFF
    if length == TWL_LEN:
        r[0xEA:0xEC] = (1400).to_bytes(2, "little")
    r[0xFE:0x100] = crc16(bytes(r[:0xFE])).to_bytes(2, "little")
    if length == TWL_LEN:
        r[0x1FE:0x200] = crc16(bytes(r[0x100:0x1FE])).to_bytes(2, "little")
    return bytes(r)


fw = bytearray(b"\xFF" * SIZE)
fw[0x20:0x22] = (BASE // 8).to_bytes(2, "little")
fw[0x1D] = 0x57                               # DSi console type -> AP4-6 present
fw[0x36:0x3C] = bytes.fromhex("0009BF010203")  # a Nintendo-OUI MAC, for the folder name

# Slot 1 carries the longest SSID the field can hold: 32 bytes, the full 0x40..0x5F.
# Every screen that names a network has to fit one beside its own labels, and both real
# dumps here have 3- and 6-byte SSIDs, so nothing else in the test inputs pushes a row to
# its full width. A too-narrow row is not cosmetic -- the console cuts the line and the
# end of the name silently disappears. The widths phase of tools/crosscheck.py found
# exactly that in describe(), on seven screens at once, only once this record existed.
LONGEST_SSID = "WidestPossibleSSID-32-bytes-Ab12"

slots = {
    1: (BASE - 0x400, ntr_record(LONGEST_SSID, wep_mode=0x00)),
    2: (BASE - 0x300, empty_record(NTR_LEN)),
    3: (BASE - 0x200, empty_record(NTR_LEN)),
    # A byte outside printable ASCII in the SSID, so ssid_printable is false. That branch
    # substitutes '?', takes the wrapped layout in the detail pane, and makes crosscheck
    # compare the name by length rather than by text. Until this existed, the overflow in
    # that branch was invisible: every SSID in every test input was plain ASCII.
    4: (BASE - 0xA00, twl_record("4ds-5g\xe9wifi", "correcthorsebattery")),
    # Security 0x05, one of the values still inferred from the System Settings menu order
    # rather than observed. That is the point: an unconfirmed value is the only thing that
    # makes security_words() return "Password protected, type unconfirmed", which at 36
    # characters is the longest security phrase the list row can carry. Every other record in
    # every input decodes as open, WEP, or the confirmed 0x07, so without this the widest
    # possible row is never drawn and its width is never checked -- the same hole the 32-byte
    # SSID above was added to close.
    #
    # It is a test input, not evidence. docs/HARDWARE.md's "The one real WPA record" still records
    # 0x07 as the only confirmed value, and only a TKIP network configured on hardware can
    # settle 0x04-0x06.
    5: (BASE - 0x800, twl_record("tkip-guess-net", "hunter2hunter2", security=0x05)),
    6: (BASE - 0x600, empty_record(TWL_LEN)),
}
for n, (off, rec) in sorted(slots.items()):
    fw[off:off + len(rec)] = rec
    state = "free" if rec[0xE7] == 0xFF else "in use"
    print(f"slot {n}: 0x{off:05X} .. 0x{off + len(rec) - 1:05X}  "
          f"({len(rec)} bytes, {state})")

out = sys.argv[1] if len(sys.argv) > 1 else "fixture_dsfirmware.bin"
open(out, "wb").write(bytes(fw))
print(f"\nwrote {out} ({len(fw)} bytes), base = 0x{BASE:05X}")
