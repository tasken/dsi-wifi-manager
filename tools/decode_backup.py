#!/usr/bin/env python3
"""Decode a .dswifi backup file produced by the app, and check every record in it.

    python3 tools/decode_backup.py DSIWIFI/0009BF010203/wifi000.dswifi

The format is in DESIGN.md and arm9/source/backup_file.h:

    "DSIWIFI1"  magic
    u8          record count
    per record: u8 slot, u8 family (0=NTR 0x100, 1=TWL 0x200), u16 length LE, bytes

Records are raw flash bytes, so each one is handed to decode_wifi.Slot -- the decoder
that was validated against real hardware -- rather than re-parsed here. A backup whose
checksums fail is a backup that will not restore.

This prints the SSID, as decode_wifi.py does, but never the passphrase, PSK, WEP keys or
WFC user ID. Those are summarised as a length and a SHA-256 prefix, which is enough to
tell two files apart or confirm a value survived a round trip without disclosing it.
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from decode_wifi import NTR_LEN, SECURITY_TYPES, TWL_LEN, WEP_MODES, Slot

MAGIC = b"DSIWIFI1"
HEADER_LEN = len(MAGIC) + 1
REC_HEADER_LEN = 4
FAMILY_NAMES = {0: "NTR", 1: "TWL"}


class BadBackup(ValueError):
    pass


def parse(data):
    """Return [(slot, family, length, record_bytes)], or raise BadBackup.

    Mirrors backup_parse() in arm9/source/backup_file.c, including its integrity rules:
    slots ascend without repeating, and slot number, family and length must agree --
    slots 1-3 are always 0x100 NTR and 4-6 always 0x200 TWL on the hardware, so a file
    that says otherwise was not made by reading a console.
    """
    if len(data) < HEADER_LEN:
        raise BadBackup(f"file is {len(data)} bytes, too short to be a backup")
    if data[:len(MAGIC)] != MAGIC:
        raise BadBackup(f"magic is {data[:len(MAGIC)]!r}, expected {MAGIC!r}")

    count = data[len(MAGIC)]
    if not 1 <= count <= 6:
        raise BadBackup(f"record count is {count}, expected 1-6")

    out = []
    pos = HEADER_LEN
    previous = 0

    for i in range(count):
        if pos + REC_HEADER_LEN > len(data):
            raise BadBackup(f"file ends inside the header of record {i + 1}")

        slot = data[pos]
        family = data[pos + 1]
        length = int.from_bytes(data[pos + 2:pos + 4], "little")
        pos += REC_HEADER_LEN

        if slot <= previous:
            raise BadBackup(f"record {i + 1}: slot {slot} does not follow slot {previous}")
        previous = slot
        if not 1 <= slot <= 6:
            raise BadBackup(f"record {i + 1}: slot {slot} is not 1-6")

        want_family = 1 if slot >= 4 else 0
        if family != want_family:
            raise BadBackup(f"record {i + 1}: slot {slot} says family {family}, "
                            f"but slot {slot} is always {FAMILY_NAMES[want_family]}")

        want_length = TWL_LEN if family == 1 else NTR_LEN
        if length != want_length:
            raise BadBackup(f"record {i + 1}: family {FAMILY_NAMES[family]} says "
                            f"length {length}, expected {want_length}")

        if pos + length > len(data):
            raise BadBackup(f"file ends inside the payload of record {i + 1}")

        out.append((slot, family, length, data[pos:pos + length]))
        pos += length

    if pos != len(data):
        raise BadBackup(f"{len(data) - pos} trailing bytes after the last record")

    return out


def digest(b):
    return hashlib.sha256(b).hexdigest()[:16]


def secret_summary(name, b):
    """Report a secret's presence and identity without printing it."""
    if not any(b):
        return f"  {name:<14} absent"
    used = b.split(b"\x00")[0] if b"\x00" in b else b
    return f"  {name:<14} present, {len(used)} bytes, sha256 {digest(bytes(used))}"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("backup", help="a .dswifi file written by the app")
    args = ap.parse_args()

    data = open(args.backup, "rb").read()

    try:
        records = parse(data)
    except BadBackup as e:
        sys.exit(f"{args.backup}: not a valid backup: {e}")

    print(f"{args.backup}: {len(data)} bytes, {len(records)} record(s)")
    print(f"  file sha256    {digest(data)}\n")

    bad = 0
    for i, (slot, family, length, rec) in enumerate(records, 1):
        s = Slot(slot, 0, rec)
        head = f"record {i}: slot {slot}  {FAMILY_NAMES[family]}  {length} bytes"
        print(head)
        print("-" * len(head))

        crc = "ok" if s.crc_ok else "BAD"
        if s.family == "TWL":
            crc += f" / {'ok' if s.crc2_ok else 'BAD'}"
        print(f"  checksum       {crc}")
        if not s.crc_ok or (s.family == "TWL" and not s.crc2_ok):
            print("  ^ this record will not restore correctly")
            bad += 1

        # A backup of a free slot is possible to construct by hand but the app refuses
        # to make one, so say so rather than decode meaningless fields.
        if s.blank:
            print(f"  status byte    0x{rec[0xE7]:02X}  (free slot -- no network here)")
            print()
            continue

        print(f"  SSID           {s.ssid!r}")
        wep = rec[0xE6]
        print(f"  WEP mode       0x{wep:02X}  {WEP_MODES.get(wep, '(undocumented)')}")

        if s.family == "TWL":
            sec = rec[0x181]
            print(f"  security       0x{sec:02X}  {SECURITY_TYPES.get(sec, '(undocumented)')}")
            print(secret_summary("passphrase", rec[0x120:0x160]))
            print(secret_summary("PSK", rec[0x100:0x120]))
            print(f"  proxy          {'enabled' if rec[0x182] else 'disabled'}")
        else:
            keys = rec[0x80:0xC0]
            print(secret_summary("WEP keys", keys))
            print(secret_summary("WFC user ID", rec[0xF0:0xF6]))

        print(f"  MTU            {int.from_bytes(rec[0xEA:0xEC], 'little')}")
        print(f"  status byte    0x{rec[0xE7]:02X}")
        print(f"  record sha256  {digest(rec)}")
        print()

    if bad:
        print(f"FAIL: {bad} record(s) have a bad checksum")
        return 1
    print("OK: every record's checksum verifies")
    return 0


if __name__ == "__main__":
    sys.exit(main())
