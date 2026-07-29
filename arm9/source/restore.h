// SPDX-License-Identifier: CC0-1.0
//
// Everything that must be true before the app writes a byte to flash, and the write
// itself expressed as an operation on a buffer so it can be tested without a console.
//
// A restore within a family is a pure byte copy. Nothing in a record depends on which
// slot it occupies -- checked against the real dump: slot 1's record has CRC 0x28DD
// stored, recomputing gives 0x28DD, and the same bytes verify at slot 2 and slot 3
// untouched. So restore_check() recomputing the CRC is a self-consistency test of the
// file, not a transformation of it. If that ever stops holding, this is the file that
// has to grow a fixup step.

#ifndef RESTORE_H
#define RESTORE_H

#include <stdbool.h>
#include <stdint.h>

#include "backup_file.h"
#include "wifi_slots.h"

typedef enum {
    RESTORE_OK = 0,
    RESTORE_ERR_NO_SLOT,    // destination is not in this console's layout
    RESTORE_ERR_FAMILY,     // 0x200 record into a 0x100 slot, or the reverse
    RESTORE_ERR_LENGTH,     // record length disagrees with its own family
    RESTORE_ERR_CRC,        // the record in the file is not self-consistent
    RESTORE_ERR_CRC2,       // its second checksum is not
    RESTORE_ERR_FREE,       // the record is an unconfigured slot: no network to restore
    RESTORE_ERR_ALIGN,      // writeFirmware needs address and length 0x100-aligned
    RESTORE_ERR_BOUNDS,     // the write would fall outside the image
} restore_err_t;

const char *restore_strerror(restore_err_t err);

// The slot with this number, or NULL. Slots 4-6 do not exist on a non-DSi layout.
const wifi_slot_pos_t *restore_find_slot(const wifi_layout_t *layout, uint8_t number);

// Every precondition, in one place, with nothing written. A caller that has not called
// this has no business calling writeFirmware.
//
// A WPA passphrase at 0x120 and its precomputed PSK at 0x100 physically do not fit in a
// 0x100 record, which is why crossing families is refused rather than truncated.
//
// It also refuses a record whose status byte says the slot it came from was free. Such a
// record is well-formed and checksums correctly, so nothing else here would stop it, and
// writing it would quietly mark the destination unconfigured -- an erase this app does
// not offer and no screen describes. The app cannot produce one, but a file made by hand
// or by tools/host_slotlist.c --backup naming a free slot can, and restore reads whatever
// it is given.
restore_err_t restore_check(const backup_record_t *rec, const wifi_slot_pos_t *dest);

// True if the destination already holds exactly these bytes. Then the write programs
// nothing: libnds compares each page before erasing it. Worth telling the user, and
// worth knowing for the first hardware test, which is a slot restored onto itself.
bool restore_is_noop(const backup_record_t *rec, const uint8_t *dest_bytes);

// The write, against an image in memory. The console does the same thing through
// writeFirmware; this is what tools/host_slotlist.c --restore uses so the result can be
// decoded and compared on a PC.
restore_err_t restore_apply(uint8_t *flash, uint32_t flash_len,
                            const wifi_layout_t *layout, const backup_record_t *rec,
                            uint8_t dest_number);

#endif // RESTORE_H
