#!/usr/bin/env python3
"""Decode the six WiFi access-point slots out of a DSi wifi-flash dump.

Usage:
    python3 tools/decode_wifi.py dsfirmware.bin          # full 128 KB dump (dsibiosdumper "X")
    python3 tools/decode_wifi.py WifiSettings.bin --raw  # fwTool's 0xA00 region, no header

Nothing here is hardcoded to an address. The slot offsets are derived from the flash header the
same way fwTool does it (firmware/nds/fwTool/arm9/source/main.cpp:255-267), so a console whose
flash is laid out differently decodes correctly instead of silently reading the wrong bytes:

    base = (u16 at 0x20) * 8
    slots 1-3 (NTR, 0x100 bytes) at base-0x400, -0x300, -0x200
    slots 4-6 (TWL, 0x200 bytes) at base-0xA00, -0x800, -0x600, only if flash[0x1D] == 0x57

Field offsets come from GBATEK "DS Firmware WiFi Internet Access Points". Values whose meaning
is documented are labelled; values that are not are printed as raw hex rather than guessed at.
The one label that is a guess rather than a reading is SECURITY_TYPES -- see the comment on it.
"""
import argparse
import sys

NTR_LEN = 0x100
TWL_LEN = 0x200
DSI_CONSOLE_TYPE = 0x57

TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (0xA001 if _c & 1 else 0)
    TABLE.append(_c)


def crc16(data):
    """CRC16 over an AP record: polynomial A001h, initial value 0000h.

    Note the init is 0000h, NOT the FFFFh the firmware *header* CRCs use. That was solved by
    brute-forcing every (polynomial, range, init) combination against a real slot 1 whose stored
    value was known; only A001h / [0x00,0xFE) / 0000h reproduces it. Assuming FFFFh made every
    valid record on a real console decode as corrupt.
    """
    c = 0x0000
    for b in data:
        c = (c >> 8) ^ TABLE[(c ^ b) & 0xFF]
    return c


# Documented in GBATEK. Anything not listed is reported as raw hex.
WEP_MODES = {
    0x00: "none",
    0x01: "WEP 64-bit",
    0x02: "WEP 128-bit",
    0x03: "WEP 152-bit",
}

# The DSi security byte at 0x181. The System Settings menu offers None / WEP /
# WPA-PSK(TKIP) / WPA2-PSK(TKIP) / WPA-PSK(AES) / WPA2-PSK(AES), and this table is the
# obvious reading of that order.
#
# 0x07 is CONFIRMED against hardware: a slot 4 record configured on a WPA2-PSK (AES)
# network stores 0x07. The other three WPA values are still inferred from the menu order
# and keep their '?' until a network of that type is configured and dumped. Confirming
# one value does not confirm a table -- it only makes the ordering it came from likelier.
#
# The raw byte is always printed alongside, so a wrong label here can never hide the truth.
SECURITY_TYPES = {
    0x00: "none",
    0x01: "WEP",
    0x04: "WPA-PSK (TKIP)?",
    0x05: "WPA2-PSK (TKIP)?",
    0x06: "WPA-PSK (AES)?",
    0x07: "WPA2-PSK (AES)",
}


def cstr(b):
    """Decode a NUL-terminated field, showing anything unprintable as an escape."""
    end = b.find(b"\x00")
    if end >= 0:
        b = b[:end]
    return b.decode("utf-8", "backslashreplace")


def ip(b):
    return ".".join(str(x) for x in b)


class Slot:
    def __init__(self, number, offset, data):
        self.number = number
        self.offset = offset
        self.data = data
        self.family = "TWL" if len(data) == TWL_LEN else "NTR"
        self.crc_ok = crc16(data[:0xFE]) == int.from_bytes(data[0xFE:0x100], "little")
        self.crc2_ok = None
        if self.family == "TWL":
            self.crc2_ok = crc16(data[0x100:0x1FE]) == int.from_bytes(data[0x1FE:0x200], "little")

    @property
    def blank(self):
        """An unconfigured slot is marked by status byte 0xFF, not by being zeroed.

        Confirmed on a real console: the one configured slot reads 0x00 there and the five unused
        ones read 0xFF, while all six still carry valid checksums and non-zero fields (MTU 1400,
        config bits). Testing for all-zero or all-FF bytes would have called every slot populated.
        """
        return self.data[0xE7] == 0xFF

    @property
    def ssid(self):
        return cstr(self.data[0x40:0x60])

    def report(self):
        d = self.data
        head = f"Slot {self.number}  [{self.family}]  flash 0x{self.offset:05X}  {len(d)} bytes"
        print(head)
        print("-" * len(head))

        if self.blank:
            print("  (empty)\n")
            return

        crc = "ok" if self.crc_ok else "BAD"
        if self.family == "TWL":
            crc += f" / {'ok' if self.crc2_ok else 'BAD'}"
        print(f"  checksum       {crc}")
        if not self.crc_ok:
            print("  ^ a bad checksum means this is not a valid record; fields below are suspect")

        print(f"  SSID           {self.ssid!r}")

        # 0xE8 is the SSID length in BOTH families, not just TWL as this once assumed.
        # Confirmed on hardware: a slot 1 NTR record read 0 with a 3-character SSID, and
        # re-saving that connection in DSi System Settings set it to 3 and nothing else.
        # Records written by something older leave it 0, so print it either way and say
        # when it disagrees -- a field that only shows up when it happens to be right is
        # worse than useless.
        note = "" if d[0xE8] == len(self.ssid) else f"  (SSID is {len(self.ssid)} long)"
        print(f"  SSID length    {d[0xE8]}{note}")

        wep = d[0xE6]
        print(f"  WEP mode       0x{wep:02X}  {WEP_MODES.get(wep, '(undocumented)')}")

        if self.family == "TWL":
            sec = d[0x181]
            print(f"  security       0x{sec:02X}  {SECURITY_TYPES.get(sec, '(undocumented)')}")
            print(f"  WPA passphrase {cstr(d[0x120:0x160])!r}")
            print(f"  precomputed PSK {d[0x100:0x120].hex()}")
        else:
            for i in range(4):
                key = d[0x80 + i * 0x10:0x80 + (i + 1) * 0x10]
                if any(key):
                    print(f"  WEP key {i + 1}      {key.hex()}")

        print(f"  IP             {ip(d[0xC0:0xC4])}")
        print(f"  gateway        {ip(d[0xC4:0xC8])}")
        print(f"  DNS 1          {ip(d[0xC8:0xCC])}")
        print(f"  DNS 2          {ip(d[0xCC:0xD0])}")
        print(f"  subnet mask    /{d[0xD0]}")
        print(f"  MTU            {int.from_bytes(d[0xEA:0xEC], 'little')}")
        print(f"  status byte    0x{d[0xE7]:02X}")
        print(f"  config bits    0x{d[0xEF]:02X}")

        if self.family == "TWL":
            print(f"  proxy          {'enabled' if d[0x182] else 'disabled'}"
                  f"  auth={'yes' if d[0x183] else 'no'}")
            if d[0x182]:
                print(f"  proxy host     {cstr(d[0x184:0x1B4])!r}")
                print(f"  proxy port     {int.from_bytes(d[0x1E8:0x1EA], 'little')}")
                print(f"  proxy user     {cstr(d[0x00:0x20])!r}")
                print(f"  proxy pass     {cstr(d[0x20:0x40])!r}")
        else:
            print(f"  WFC user ID    {d[0xF0:0xF6].hex()}")
        print()


def slot_offsets(flash):
    """Derive the six slot offsets from the flash header, the way fwTool does."""
    base = (flash[0x20] | (flash[0x21] << 8)) * 8
    if base == 0 or base + 0x200 > len(flash):
        sys.exit(f"header at 0x20 gives base 0x{base:X}, which is not sane for a "
                 f"{len(flash)}-byte file -- is this really a wifi flash dump?")

    layout = [(1, base - 0x400, NTR_LEN), (2, base - 0x300, NTR_LEN), (3, base - 0x200, NTR_LEN)]
    is_dsi = flash[0x1D] == DSI_CONSOLE_TYPE
    if is_dsi:
        layout += [(4, base - 0xA00, TWL_LEN), (5, base - 0x800, TWL_LEN), (6, base - 0x600, TWL_LEN)]
    return base, is_dsi, sorted(layout)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="wifi flash dump (128 KB), or the 0xA00 region with --raw")
    ap.add_argument("--raw", action="store_true",
                    help="input is fwTool's WifiSettings.bin (slot 4 first, no header)")
    args = ap.parse_args()

    flash = open(args.dump, "rb").read()

    if args.raw:
        if len(flash) != 0xA00:
            sys.exit(f"--raw expects a 0xA00-byte region, got {len(flash)}")
        # fwTool dumps base-0xA00 .. base, so the region starts at slot 4.
        layout = [(4, 0x000, TWL_LEN), (5, 0x200, TWL_LEN), (6, 0x400, TWL_LEN),
                  (1, 0x600, NTR_LEN), (2, 0x700, NTR_LEN), (3, 0x800, NTR_LEN)]
        print(f"{args.dump}: {len(flash)} bytes (fwTool wifi region, header not present)\n")
        for number, off, length in sorted(layout):
            Slot(number, off, flash[off:off + length]).report()
        return

    base, is_dsi, layout = slot_offsets(flash)
    mac = ":".join(f"{b:02X}" for b in flash[0x36:0x3C])
    print(f"{args.dump}: {len(flash)} bytes ({len(flash) // 1024} KB)")
    print(f"  MAC              {mac}")
    print(f"  console type     0x{flash[0x1D]:02X}  {'DSi' if is_dsi else 'DS (no slots 4-6)'}")
    print(f"  user settings    0x{base:05X}   (u16 at 0x20 = 0x{base // 8:04X}, times 8)")
    print(f"  implied fw size  {base + 0x200} bytes")
    if base + 0x200 != len(flash):
        print(f"  ! header implies {base + 0x200} bytes but the file is {len(flash)}")
    low = min(off for _, off, _ in layout)
    print(f"  wifi region      0x{low:05X} .. 0x{base - 1:05X}  ({base - low} bytes)\n")

    for number, off, length in layout:
        Slot(number, off, flash[off:off + length]).report()


if __name__ == "__main__":
    main()
