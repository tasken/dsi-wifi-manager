// See restore.h.

#include <string.h>

#include "restore.h"

// writeFirmware() returns -1 unless both the address and the length are multiples of
// this. Records are 0x100 and 0x200 at 0x100-aligned offsets, so a per-record write
// lands on that granularity by construction -- but check rather than assume, because
// the cost of being wrong is somebody's user settings.
#define WRITE_GRANULARITY 0x100u

const char *restore_strerror(restore_err_t err)
{
    switch (err)
    {
        case RESTORE_OK:            return "ok";
        case RESTORE_ERR_NO_SLOT:   return "no such slot on this console";
        case RESTORE_ERR_FAMILY:    return "wrong family for that slot";
        case RESTORE_ERR_LENGTH:    return "record length is wrong for its family";
        case RESTORE_ERR_CRC:       return "record checksum is bad";
        case RESTORE_ERR_CRC2:      return "second checksum is bad";
        case RESTORE_ERR_FREE:      return "that record is an empty slot";
        case RESTORE_ERR_ALIGN:     return "destination is not 0x100-aligned";
        case RESTORE_ERR_BOUNDS:    return "write would fall outside the flash";
    }
    return "unknown";
}

const wifi_slot_pos_t *restore_find_slot(const wifi_layout_t *layout, uint8_t number)
{
    for (uint8_t i = 0; i < layout->count; i++)
    {
        if (layout->slots[i].number == number)
            return &layout->slots[i];
    }
    return NULL;
}

restore_err_t restore_check(const backup_record_t *rec, const wifi_slot_pos_t *dest)
{
    if (dest == NULL)
        return RESTORE_ERR_NO_SLOT;

    if (rec->length != backup_length_for_family(rec->family))
        return RESTORE_ERR_LENGTH;

    if ((uint8_t)dest->family != rec->family || dest->length != rec->length)
        return RESTORE_ERR_FAMILY;

    if ((dest->offset % WRITE_GRANULARITY) != 0 ||
        (dest->length % WRITE_GRANULARITY) != 0)
        return RESTORE_ERR_ALIGN;

    // The file was checked when it was parsed, but that was before the user chose a
    // destination and nothing guarantees the two happened in the same run. Recompute.
    uint16_t stored = (uint16_t)(rec->data[0xFE] | (rec->data[0xFF] << 8));
    if (wifi_crc16(rec->data, 0xFE) != stored)
        return RESTORE_ERR_CRC;

    if (rec->family == WIFI_FAMILY_TWL)
    {
        uint16_t stored2 = (uint16_t)(rec->data[0x1FE] | (rec->data[0x1FF] << 8));
        if (wifi_crc16(&rec->data[0x100], 0x1FE - 0x100) != stored2)
            return RESTORE_ERR_CRC2;
    }

    // Checked last, because a record that is both free and corrupt is better described
    // as corrupt. This is the rule that used to live only in main.c's UI -- a rule a
    // screen enforces is one the next caller does not have to obey.
    if (rec->data[0xE7] == 0xFF)
        return RESTORE_ERR_FREE;

    return RESTORE_OK;
}

bool restore_is_noop(const backup_record_t *rec, const uint8_t *dest_bytes)
{
    return memcmp(rec->data, dest_bytes, rec->length) == 0;
}

restore_err_t restore_apply(uint8_t *flash, uint32_t flash_len,
                            const wifi_layout_t *layout, const backup_record_t *rec,
                            uint8_t dest_number)
{
    const wifi_slot_pos_t *dest = restore_find_slot(layout, dest_number);

    restore_err_t err = restore_check(rec, dest);
    if (err != RESTORE_OK)
        return err;

    if (dest->offset + dest->length > flash_len)
        return RESTORE_ERR_BOUNDS;

    // Only the destination slot's own bytes. Nothing either side of it is touched.
    memcpy(flash + dest->offset, rec->data, rec->length);
    return RESTORE_OK;
}
