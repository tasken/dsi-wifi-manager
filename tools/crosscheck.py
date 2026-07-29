#!/usr/bin/env python3
"""The test suite. Runs the app's own C against the Python that hardware validated.

There is no emulator, so correctness comes from the C the ARM9 runs and the Python that
decoded the real dump agreeing on the same bytes. This builds the app's portable sources
for the host and runs these phases over every dump given:

  1. decode -- diff every field the app decodes against decode_wifi.py
  2. backup -- build a .dswifi with the app's own writer, parse it with
     decode_backup.py, and compare each record byte for byte with the flash it came from
  3. reject -- corrupt that file eight ways and require both parsers to refuse it
  4. restore -- put every record into every slot and require the right ones to be refused
  5. widths -- draw every screen on both panes; every line must fit VIEW_COLS and every
     screen must fit the display height
  6. renderer -- fb_render.c's own properties, including that every pixel it writes is opaque

    python3 tools/crosscheck.py dsidump/dsfirmware.bin
    python3 tools/crosscheck.py dsidump/dsfirmware.bin testdata/*.bin build/fixture.bin

Values are compared, never echoed: a real dump's SSID stays out of the transcript, and a
mismatch is reported by field name and length rather than by content. Passphrase, PSK,
WEP keys, MAC and WFC user ID are compared as raw bytes and never printed in any form.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import decode_backup
from decode_wifi import NTR_LEN, TWL_LEN, SECURITY_TYPES, WEP_MODES, Slot, slot_offsets

SRC = [
    os.path.join(HERE, "host_slotlist.c"),
    os.path.join(ROOT, "arm9", "source", "wifi_slots.c"),
    os.path.join(ROOT, "arm9", "source", "slot_list_view.c"),
    os.path.join(ROOT, "arm9", "source", "backup_file.c"),
    os.path.join(ROOT, "arm9", "source", "restore.c"),
    os.path.join(ROOT, "arm9", "source", "fb_render.c"),
]
BIN = os.path.join(ROOT, "build", "host_slotlist")
SCRATCH = os.path.join(ROOT, "build", "crosscheck")


def build():
    os.makedirs(os.path.dirname(BIN), exist_ok=True)
    # -O2 to match Makefile.arm9. Not for speed: several of gcc's most useful warnings,
    # -Wformat-truncation among them, need optimisation to do their analysis and say
    # nothing at -O0. Without it this build passed clean while the ARM9 build emitted
    # eleven truncation warnings, which is the gate failing to gate.
    # Deliberately longer than any real version, because the banner right-aligns this and a
    # string that overflows would fail the widths phase on a build that is otherwise fine.
    # A real one is now short -- "Dev 18975b7-dirty", or "Release v1.2.3" -- since the branch
    # was dropped from it. Testing well past that keeps the cut path exercised anyway.
    version = "Dev an-unreasonably-long-version-string-abc1234-dirty"

    cmd = ["gcc", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-O2",
           f'-DDSIWIFI_VERSION_STR="{version}"',
           "-I", os.path.join(ROOT, "arm9", "source"), "-o", BIN] + SRC
    subprocess.run(cmd, check=True)
    return BIN


def run_c(path):
    out = subprocess.run([BIN, "--fields", path], check=True, capture_output=True,
                         text=True).stdout
    layout, slots = {}, {}
    for line in out.splitlines():
        parts = line.split()
        if parts[0] == "layout":
            layout = dict(p.split("=", 1) for p in parts[1:])
        elif parts[0] == "slot":
            # Values can contain spaces, so this cannot split on whitespace. "WEP 64-bit"
            # is the label for wep mode 0x01, and until an input existed with a WEP record
            # -- every in-use NTR record before was 0x00 -- a plain dict(p.split("=")) over
            # the tokens looked fine and threw the moment one appeared.
            #
            # Each value runs up to the next " key=" or the end of the line. ssid is taken
            # from the raw line because it is last and may contain anything at all.
            head, _, ssid = line.partition(" ssid=")
            fields = dict(re.findall(r"(\w+)=(.*?)(?= \w+=|$)", head.split(None, 2)[2]))
            fields["ssid"] = ssid
            slots.setdefault(int(parts[1]), {}).update(fields)
        elif parts[0] == "net":
            slots.setdefault(int(parts[1]), {}).update(
                dict(p.split("=", 1) for p in parts[2:]))
    return layout, slots


class Check:
    def __init__(self):
        self.failed = 0
        self.checked = 0

    def eq(self, where, field, c_val, py_val, show=True):
        self.checked += 1
        if c_val == py_val:
            return
        self.failed += 1
        if show:
            print(f"  MISMATCH {where} {field}: C={c_val!r} python={py_val!r}")
        else:
            print(f"  MISMATCH {where} {field}: values differ "
                  f"(C {len(str(c_val))} chars, python {len(str(py_val))} chars)")


def check_dump(path):
    c_layout, c_slots = run_c(path)

    flash = open(path, "rb").read()
    base, is_dsi, layout = slot_offsets(flash)

    chk = Check()
    print(f"{os.path.basename(path)}: {len(flash)} bytes\n")

    print("layout")
    chk.eq("layout", "base", int(c_layout["base"], 16), base)
    chk.eq("layout", "dsi", c_layout["dsi"] == "1", is_dsi)
    chk.eq("layout", "count", int(c_layout["count"]), len(layout))
    chk.eq("layout", "type", int(c_layout["type"], 16), flash[0x1D])
    chk.eq("layout", "region", int(c_layout["region"], 16),
           min(off for _, off, _ in layout))
    chk.eq("layout", "implied_size", int(c_layout["implied"]), base + 0x200)
    print(f"  base 0x{base:05X}  count {len(layout)}  dsi {is_dsi}")

    for number, off, length in layout:
        py = Slot(number, off, flash[off:off + length])
        c = c_slots.get(number)
        where = f"slot {number}"
        print(f"\n{where}")
        if c is None:
            print(f"  MISMATCH {where}: missing from the C output")
            chk.failed += 1
            continue

        chk.eq(where, "offset", int(c["offset"], 16), off)
        chk.eq(where, "len", int(c["len"]), length)
        chk.eq(where, "family", c["family"], py.family)
        chk.eq(where, "crc_ok", c["crc"] == "1", py.crc_ok)

        if length == TWL_LEN:
            chk.eq(where, "crc2_ok", c["crc2"] == "1", py.crc2_ok)
        else:
            chk.eq(where, "crc2", c["crc2"], "-")

        chk.eq(where, "free", c["free"] == "1", py.blank)
        chk.eq(where, "status", int(c["status"], 16), py.data[0xE7])

        # decode_wifi.py reads WEP mode for both families and the security byte only
        # for TWL; the app shows whichever one applies, so compare the byte it chose.
        py_sec_byte = py.data[0x181] if length == TWL_LEN else py.data[0xE6]
        chk.eq(where, "sec_byte", int(c["sec_byte"], 16), py_sec_byte)

        # The two label sets are worded differently on purpose -- 31 columns will not
        # hold "WPA2-PSK (AES)?" -- so compare what they mean: does each side consider
        # this byte documented, and does each keep the '?' on the unverified DSi ones.
        table = SECURITY_TYPES if length == TWL_LEN else WEP_MODES
        py_label = table.get(py_sec_byte)
        c_label = c["label"]
        chk.eq(where, "label_known", c_label != "undocumented", py_label is not None)
        if py_label is not None:
            chk.eq(where, "label_unverified_mark", "?" in c_label, "?" in py_label)
        print(f"  sec byte 0x{py_sec_byte:02X}  C {c_label!r}  python {py_label!r}")

        # Compare bytes to bytes. decode_wifi.py decodes with backslashreplace, so a
        # field of 32 undecodable bytes comes back as 128 characters; len(py.ssid) is
        # not a byte count and comparing it to one reports a mismatch that is not there.
        field = py.data[0x40:0x60]
        nul = field.find(b"\x00")
        py_ssid_len = len(field) if nul < 0 else nul
        chk.eq(where, "ssid_len", int(c["ssid_len"]), py_ssid_len)

        if c["ssid_printable"] == "1":
            chk.eq(where, "ssid", c["ssid"], py.ssid, show=False)
        else:
            # The app substitutes '?' for non-ASCII so a hostile SSID cannot scribble
            # on the console; decode_wifi.py escapes instead, so the text will differ
            # by design. Length is still comparable.
            print(f"  note: SSID has non-ASCII bytes, comparing length only")

        # The network block. Compared for free slots too: the app copies those bytes
        # whatever the slot's state, and a decoder that agrees only on populated records
        # is a decoder that has not been checked on the other branch.
        #
        # Addresses are compared but not printed on a mismatch. They are not on the
        # must-mask list, but a home LAN address is nobody's business and the SSID is
        # handled the same way a few lines up.
        def quad(off):
            return ".".join(str(x) for x in py.data[off:off + 4])

        chk.eq(where, "ip", c["ip"], quad(0xC0), show=False)
        chk.eq(where, "gateway", c["gw"], quad(0xC4), show=False)
        chk.eq(where, "dns1", c["dns1"], quad(0xC8), show=False)
        chk.eq(where, "dns2", c["dns2"], quad(0xCC), show=False)
        chk.eq(where, "subnet", int(c["subnet"]), py.data[0xD0])
        chk.eq(where, "mtu", int(c["mtu"]),
               int.from_bytes(py.data[0xEA:0xEC], "little"))
        chk.eq(where, "config_bits", int(c["cfg"], 16), py.data[0xEF])

        state = "free" if py.blank else "in use"
        crc = "ok" if py.crc_ok else "BAD"
        if length == TWL_LEN:
            crc += "/ok" if py.crc2_ok else "/BAD"
        print(f"  {py.family} 0x{off:05X} {state} crc {crc} ssid_len {py_ssid_len}")

    print(f"\n{chk.checked - chk.failed}/{chk.checked} field comparisons agree")
    if chk.failed:
        print("FAIL: the app decodes this dump differently from decode_wifi.py")
        return 1
    print("PASS: field for field")
    return 0


def check_backup(path):
    """Back up every in-use slot, then prove the file holds the flash bytes verbatim."""
    flash = open(path, "rb").read()
    base, _, layout = slot_offsets(flash)

    in_use = [(n, off, ln) for n, off, ln in layout
              if not Slot(n, off, flash[off:off + ln]).blank]

    print("\nbackup")
    if not in_use:
        print("  every slot is free -- nothing to back up, which the app says outright")
        return 0

    os.makedirs(SCRATCH, exist_ok=True)
    out = os.path.join(SCRATCH, os.path.basename(path) + ".dswifi")
    subprocess.run([BIN, "--backup", path, out], check=True, capture_output=True)

    data = open(out, "rb").read()
    try:
        records = decode_backup.parse(data)
    except decode_backup.BadBackup as e:
        print(f"  FAIL the app wrote a file decode_backup.py rejects: {e}")
        return 1

    if len(records) != len(in_use):
        print(f"  FAIL wrote {len(records)} records for {len(in_use)} in-use slots")
        return 1

    failed = 0
    for (slot, family, length, rec), (n, off, ln) in zip(records, in_use):
        # The payload must be the flash bytes with nothing re-encoded -- that is what
        # makes a restore byte-exact by construction.
        want = flash[off:off + ln]
        if (slot, length) != (n, ln) or rec != want:
            print(f"  FAIL slot {n}: record does not match flash 0x{off:05X}")
            failed += 1
            continue

        s = Slot(slot, off, rec)
        if not s.crc_ok or (s.family == "TWL" and not s.crc2_ok):
            print(f"  FAIL slot {n}: checksum does not verify in the backup")
            failed += 1
            continue

        print(f"  slot {n} {s.family} {ln} bytes byte-exact, checksum verifies")

    # The app's own parser has to accept what the app's own writer produced.
    if subprocess.run([BIN, "--verify", out], capture_output=True).returncode != 0:
        print("  FAIL backup_parse rejects a file backup_build wrote")
        failed += 1

    if failed:
        print("FAIL: the backup does not round trip")
        return 1
    print(f"PASS: {len(records)} record(s) round trip byte for byte")
    return 0


def mutations(data):
    """Files that must be refused. Each is one plausible way a backup goes wrong."""
    n_off = len(decode_backup.MAGIC)
    first = decode_backup.HEADER_LEN

    yield "bad magic", b"DSIWIFI2" + data[8:]
    yield "zero records", data[:n_off] + b"\x00" + data[first:]
    yield "count of 7", data[:n_off] + b"\x07" + data[first:]
    yield "truncated mid-payload", data[:-1]
    yield "truncated mid-header", data[:first + 2]
    yield "slot 0", data[:first] + b"\x00" + data[first + 1:]
    yield "slot 7", data[:first] + b"\x07" + data[first + 1:]
    # Slot 1 is always a 0x100 NTR record; claiming TWL makes the length a lie.
    yield "family lies about slot", data[:first + 1] + b"\x01" + data[first + 2:]
    yield "trailing junk", data + b"\x00"


def check_rejects(path):
    """Both parsers must refuse the same malformed files, not just the Python one."""
    out = os.path.join(SCRATCH, os.path.basename(path) + ".dswifi")
    if not os.path.exists(out):
        return 0

    print("\nreject")
    data = open(out, "rb").read()
    failed = 0

    for name, bad in mutations(data):
        bad_path = os.path.join(SCRATCH, "bad.dswifi")
        with open(bad_path, "wb") as f:
            f.write(bad)

        py_rejected = True
        try:
            decode_backup.parse(bad)
            py_rejected = False
        except decode_backup.BadBackup:
            pass

        c_rejected = subprocess.run([BIN, "--verify", bad_path],
                                    capture_output=True).returncode != 0

        if py_rejected and c_rejected:
            print(f"  both refuse: {name}")
        else:
            who = []
            if not c_rejected:
                who.append("backup_parse (C)")
            if not py_rejected:
                who.append("decode_backup.py")
            print(f"  FAIL {name} accepted by {' and '.join(who)}")
            failed += 1

    if failed:
        print("FAIL: a malformed backup was accepted")
        return 1
    print("PASS: every malformed file refused by both parsers")
    return 0


def run_restore(dump, backup, record, dest, out):
    """Returns (accepted, stdout). A refusal is a non-zero exit, not an exception."""
    r = subprocess.run([BIN, "--restore", dump, backup, str(record), str(dest), out],
                       capture_output=True, text=True)
    return r.returncode == 0, r.stdout


def check_restore(path):
    """Restore every record everywhere it is allowed, and nowhere it is not."""
    backup = os.path.join(SCRATCH, os.path.basename(path) + ".dswifi")
    if not os.path.exists(backup):
        return 0

    print("\nrestore")
    original = open(path, "rb").read()
    base, _, layout = slot_offsets(original)
    records = decode_backup.parse(open(backup, "rb").read())

    by_number = {n: (off, ln) for n, off, ln in layout}
    out = os.path.join(SCRATCH, "restored.bin")
    failed = 0

    for i, (slot, family, length, rec) in enumerate(records, 1):
        src_off, src_len = by_number[slot]

        for dest_n, (dest_off, dest_len) in sorted(by_number.items()):
            same_family = (dest_len == src_len)
            ok, said = run_restore(path, backup, i, dest_n, out)

            # A 0x200 record cannot fit a 0x100 slot, and a 0x100 slot has nowhere to
            # put a WPA passphrase. Crossing families must be refused, not truncated.
            if not same_family:
                if ok:
                    print(f"  FAIL slot {slot} -> slot {dest_n} crossed families")
                    failed += 1
                else:
                    print(f"  refused slot {slot} -> slot {dest_n} (other family)")
                continue

            if not ok:
                print(f"  FAIL slot {slot} -> slot {dest_n} refused: {said.strip()}")
                failed += 1
                continue

            after = open(out, "rb").read()
            changed = [k for k in range(len(original)) if original[k] != after[k]]

            if dest_n == slot:
                # The first hardware test is a slot restored onto itself. It must be a
                # no-op end to end, and the tool must say so before writing.
                if changed:
                    print(f"  FAIL slot {slot} onto itself changed {len(changed)} bytes")
                    failed += 1
                elif "no-op" not in said:
                    print(f"  FAIL slot {slot} onto itself was not reported as a no-op")
                    failed += 1
                else:
                    print(f"  slot {slot} onto itself: no-op, image unchanged")
                continue

            # Only the destination's own bytes may move.
            outside = [k for k in changed
                       if not (dest_off <= k < dest_off + dest_len)]
            if outside:
                print(f"  FAIL slot {slot} -> slot {dest_n} touched {len(outside)} "
                      f"bytes outside the slot")
                failed += 1
                continue

            if after[dest_off:dest_off + dest_len] != rec:
                print(f"  FAIL slot {slot} -> slot {dest_n} did not land verbatim")
                failed += 1
                continue

            landed = Slot(dest_n, dest_off, after[dest_off:dest_off + dest_len])
            if not landed.crc_ok or (landed.family == "TWL" and not landed.crc2_ok):
                print(f"  FAIL slot {slot} -> slot {dest_n} checksum bad after restore")
                failed += 1
                continue

            print(f"  slot {slot} -> slot {dest_n}: {len(changed)} bytes changed, all "
                  f"inside the slot, checksum ok")

    failed += check_free_record_refused(path, by_number, out)

    if failed:
        print("FAIL: restore did the wrong thing")
        return 1
    print("PASS: every restore landed verbatim or was refused")
    return 0


def check_free_record_refused(path, by_number, out):
    """A record taken from a free slot must never be written anywhere.

    Such a record is well-formed and its checksum verifies, so nothing about the file
    format stops it; restoring one would quietly mark the destination unconfigured. The
    app cannot produce such a file, but --backup naming a free slot can, which is exactly
    why the rule belongs in restore_check() and not in a screen.
    """
    flash = open(path, "rb").read()
    free = [n for n, (off, ln) in sorted(by_number.items())
            if Slot(n, off, flash[off:off + ln]).blank]
    if not free:
        return 0

    made = os.path.join(SCRATCH, "free.dswifi")
    subprocess.run([BIN, "--backup", path, made, str(free[0])], check=True,
                   capture_output=True)

    failed = 0
    for dest_n in sorted(by_number):
        ok, said = run_restore(path, made, 1, dest_n, out)
        if ok:
            print(f"  FAIL a free slot {free[0]} record was written into slot {dest_n}")
            failed += 1

    if not failed:
        print(f"  refused everywhere: a record taken from free slot {free[0]}")
    return failed


def check_render():
    """The framebuffer renderer's own properties, checked once rather than per dump.

    The one that matters most is that every pixel carries bit 15. On the DS that bit is the
    opaque flag in 16-bit bitmap mode, not part of the colour, so a pixel without it is
    invisible: a renderer that is otherwise perfect draws a blank screen. Nothing about the
    buffer contents would look wrong, which is why it has to be asserted rather than eyeballed.
    """
    print()
    r = subprocess.run([BIN, "--render"], capture_output=True, text=True)
    print(r.stdout.rstrip())
    return 1 if r.returncode else 0


def check_widths(path):
    """Every rendered line fits its screen, on both panes.

    The console is 32 columns and the renderer cuts at 31, so an over-long line does not
    wrap -- it loses its tail, and the end of an SSID or a file name disappears without
    anything saying so. --screens draws every screen the app has, tags each line with the
    pane it belongs to, and counts the ones that had to be cut. Zero is the only pass.

    Nothing is echoed: only the count is read, so a real dump's SSID stays out of the
    transcript even though the screens that print it were all rendered.
    """
    print("\nwidths")
    r = subprocess.run([BIN, "--screens", path], capture_output=True, text=True)

    over = None
    tall = 0
    cols = "?"
    screens = 0
    for line in r.stdout.splitlines():
        if line.startswith("--- "):
            screens += 1
        elif line.startswith("columns: "):
            cols = line.split(": ", 1)[1]
        elif line.startswith("overlong lines: "):
            over = int(line.split(": ", 1)[1])
        elif line.startswith("overtall screens: "):
            tall = int(line.split(": ", 1)[1])

    if over is None:
        print("  FAIL: --screens printed no count")
        print("FAIL: widths")
        return 1

    if over or tall:
        if over:
            print(f"  FAIL: {over} line(s) longer than {cols} columns "
                  f"across {screens} screens")
        if tall:
            # A screen taller than the display loses its last rows, and every line in it
            # is still a legal width, so nothing else catches this.
            print(f"  FAIL: {tall} screen(s) taller than the display")
        print("  rerun `./build/host_slotlist --screens` on a FIXTURE to see which")
        print("FAIL: widths")
        return 1

    print(f"  {screens} screens fit {cols} columns and the display height, on both panes")
    print("PASS: widths")
    return 0


def check_names():
    """The naming scheme, including the fallback for a console with an unset clock."""
    print("\nnames")
    cases = [
        ("stamped backup", ["--name", "0", "2026", "7", "28", "1", "55", "30"],
         "wifi-20260728-015530.dswifi"),
        ("stamped undo",   ["--name", "4", "2026", "7", "28", "1", "55", "30"],
         "undo4-20260728-015530.dswifi"),
        # A DSi cannot have been used in 1999; a name built from that clock would state
        # something false, so the caller falls back to the index.
        ("implausible year", ["--name", "0", "1999", "7", "28", "1", "55", "30"],
         "implausible"),
        ("indexed backup", ["--index", "0", "0"], "wifi000.dswifi"),
        ("indexed undo",   ["--index", "4", "1"], "undo4-001.dswifi"),
    ]

    failed = 0
    for label, args, want in cases:
        got = subprocess.run([BIN] + args, capture_output=True, text=True).stdout.strip()
        if got == want:
            print(f"  {label}: {got}")
        else:
            print(f"  FAIL {label}: got {got!r}, wanted {want!r}")
            failed += 1

    if failed:
        print("FAIL: naming")
        return 1
    print("PASS: names")
    return 0


def ensure_fixture(paths):
    """Regenerate build/fixture.bin if it is missing.

    The fixture is the fifth test input and the only one with a configured WPA record, a
    32-byte SSID and a non-printable SSID -- so losing it silently narrows the suite. It is
    also generated, gitignored, and lives in build/, which `./build.sh` wipes on every clean
    build. Running the suite after a build therefore failed with `cannot open
    build/fixture.bin`, which reads like a code regression and is not one.

    Regenerating is safe because the fixture is deterministic: make_fixture.py takes no
    randomness, so the file this writes is the file the last run used.
    """
    for p in paths:
        if os.path.basename(p) != "fixture.bin" or os.path.exists(p):
            continue
        print(f"{p} is missing (./build.sh wipes build/); regenerating")
        # The directory has to exist first. `make clean` removes build/ entirely, and this
        # runs before build() creates it again -- so without this line the one situation this
        # function was written for is the one where it crashes.
        os.makedirs(os.path.dirname(os.path.abspath(p)) or ".", exist_ok=True)
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "make_fixture.py"), p],
                       check=True, stdout=subprocess.DEVNULL)



# --- docs: no real console values --------------------------------------------------------
# Not a doc-quality check. The notes are tracked in git now, so a MAC or a network name pasted
# into one and pushed cannot be taken back. Needles come from the dump being tested, so this
# only runs when a real dump is among the inputs, and the values themselves are never printed.
def check_doc_secrets(paths):
    import glob

    dumps = [p for p in paths if "fixture" not in p and "testdata" not in p]
    if not dumps:
        print("docs\n  skip: no real dump among the inputs")
        return 0

    fw = open(dumps[0], "rb").read()
    mac = fw[0x36:0x3C]
    base = (fw[0x20] | (fw[0x21] << 8)) * 8
    rec = fw[base - 0x400:base - 0x300]
    ssid = rec[0:32].split(b"\x00")[0].decode("ascii", "replace")

    needles = {
        "MAC": mac.hex().upper(),
        "MAC lowercase": mac.hex().lower(),
        "MAC with colons": ":".join(f"{b:02X}" for b in mac),
        "WFC user ID": rec[0xF0:0xF6].hex().upper(),
    }
    if len(ssid) >= 4:
        needles["network name"] = ssid

    docs = ["README.md"] + sorted(glob.glob("docs/**/*.md",
                                                                     recursive=True))
    print("docs")
    bad = 0
    for d in docs:
        path = os.path.join(ROOT, d)
        if not os.path.exists(path):
            continue
        text = open(path, encoding="utf-8", errors="ignore").read()
        for name, value in needles.items():
            if len(value) >= 4 and value in text:
                print(f"  LEAK {d} contains this console's {name}")
                bad += 1

    if bad:
        print(f"FAIL: {bad} real console value(s) in tracked notes")
        return 1
    print(f"PASS: {len(docs)} notes carry no real console values")
    return 0


def main(paths):
    ensure_fixture(paths)
    build()
    failures = []
    rc = check_names()
    if rc:
        failures.append("naming")
    if check_render():
        failures.append("renderer")
    if check_doc_secrets(paths):
        failures.append("docs")
    for p in paths:
        rc = check_dump(p)
        rc |= check_backup(p)
        rc |= check_rejects(p)
        rc |= check_restore(p)
        rc |= check_widths(p)
        if rc:
            failures.append(p)

    print(f"\n{'=' * 60}")
    dumps_failed = [p for p in failures if p != "naming"]
    print(f"{len(paths) - len(dumps_failed)}/{len(paths)} dumps pass every phase")
    for p in failures:
        print(f"  FAIL {p}")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1:]))
