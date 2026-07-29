// SPDX-License-Identifier: CC0-1.0
//
// See backup_file.h.

#include <stdio.h>
#include <string.h>

#include "backup_file.h"

const char *backup_strerror(backup_err_t err)
{
    switch (err)
    {
        case BACKUP_OK:             return "ok";
        case BACKUP_ERR_SHORT:      return "file too short to be a backup";
        case BACKUP_ERR_MAGIC:      return "not a DSIWIFI1 file";
        case BACKUP_ERR_COUNT:      return "bad record count";
        case BACKUP_ERR_TRUNCATED:  return "file ends inside a record";
        case BACKUP_ERR_SLOT:       return "bad or out-of-order slot number";
        case BACKUP_ERR_FAMILY:     return "family does not match slot or length";
        case BACKUP_ERR_TRAILING:   return "junk after the last record";
        case BACKUP_ERR_SPACE:      return "buffer too small";
    }
    return "unknown";
}

wifi_family_t backup_family_for_slot(uint8_t slot)
{
    return (slot >= 4) ? WIFI_FAMILY_TWL : WIFI_FAMILY_NTR;
}

uint16_t backup_length_for_family(uint8_t family)
{
    return (family == WIFI_FAMILY_TWL) ? WIFI_TWL_LEN : WIFI_NTR_LEN;
}

static bool record_is_consistent(const backup_record_t *r)
{
    if (r->slot < 1 || r->slot > WIFI_MAX_SLOTS)
        return false;
    if (r->family != (uint8_t)backup_family_for_slot(r->slot))
        return false;
    if (r->length != backup_length_for_family(r->family))
        return false;
    return true;
}

uint32_t backup_build(uint8_t *buf, uint32_t buf_len, const backup_record_t *recs,
                      uint8_t count, backup_err_t *err)
{
    backup_err_t ignored;
    if (err == NULL)
        err = &ignored;

    if (count == 0 || count > WIFI_MAX_SLOTS)
    {
        *err = BACKUP_ERR_COUNT;
        return 0;
    }

    uint32_t need = BACKUP_HEADER_LEN;
    uint8_t previous = 0;

    for (uint8_t i = 0; i < count; i++)
    {
        if (recs[i].slot <= previous)   // ascending and no duplicates
        {
            *err = BACKUP_ERR_SLOT;
            return 0;
        }
        previous = recs[i].slot;

        if (!record_is_consistent(&recs[i]))
        {
            *err = (recs[i].slot < 1 || recs[i].slot > WIFI_MAX_SLOTS)
                   ? BACKUP_ERR_SLOT : BACKUP_ERR_FAMILY;
            return 0;
        }

        need += BACKUP_REC_HEADER_LEN + recs[i].length;
    }

    if (need > buf_len)
    {
        *err = BACKUP_ERR_SPACE;
        return 0;
    }

    memcpy(buf, BACKUP_MAGIC, BACKUP_MAGIC_LEN);
    buf[BACKUP_MAGIC_LEN] = count;

    uint32_t pos = BACKUP_HEADER_LEN;
    for (uint8_t i = 0; i < count; i++)
    {
        buf[pos++] = recs[i].slot;
        buf[pos++] = recs[i].family;
        buf[pos++] = (uint8_t)(recs[i].length & 0xFF);
        buf[pos++] = (uint8_t)(recs[i].length >> 8);
        memcpy(&buf[pos], recs[i].data, recs[i].length);
        pos += recs[i].length;
    }

    *err = BACKUP_OK;
    return pos;
}

backup_err_t backup_parse(const uint8_t *buf, uint32_t len, backup_record_t *out,
                          uint8_t out_max, uint8_t *count_out)
{
    if (count_out)
        *count_out = 0;

    if (len < BACKUP_HEADER_LEN)
        return BACKUP_ERR_SHORT;

    if (memcmp(buf, BACKUP_MAGIC, BACKUP_MAGIC_LEN) != 0)
        return BACKUP_ERR_MAGIC;

    uint8_t count = buf[BACKUP_MAGIC_LEN];
    if (count == 0 || count > WIFI_MAX_SLOTS || count > out_max)
        return BACKUP_ERR_COUNT;

    uint32_t pos = BACKUP_HEADER_LEN;
    uint8_t previous = 0;

    for (uint8_t i = 0; i < count; i++)
    {
        if (pos + BACKUP_REC_HEADER_LEN > len)
            return BACKUP_ERR_TRUNCATED;

        backup_record_t r;
        r.slot = buf[pos];
        r.family = buf[pos + 1];
        r.length = (uint16_t)(buf[pos + 2] | (buf[pos + 3] << 8));
        pos += BACKUP_REC_HEADER_LEN;

        if (r.slot <= previous)
            return BACKUP_ERR_SLOT;
        previous = r.slot;

        if (r.slot < 1 || r.slot > WIFI_MAX_SLOTS)
            return BACKUP_ERR_SLOT;
        if (r.family != (uint8_t)backup_family_for_slot(r.slot))
            return BACKUP_ERR_FAMILY;
        if (r.length != backup_length_for_family(r.family))
            return BACKUP_ERR_FAMILY;

        if (pos + r.length > len)
            return BACKUP_ERR_TRUNCATED;

        r.data = &buf[pos];
        pos += r.length;

        out[i] = r;
        if (count_out)
            *count_out = (uint8_t)(i + 1);
    }

    // Nothing this app writes leaves a byte over. A file that has some is not the file
    // it claims to be, and restore reads from whatever this accepts.
    if (pos != len)
        return BACKUP_ERR_TRAILING;

    return BACKUP_OK;
}

void backup_dir_for_mac(const uint8_t mac[6], char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%02X%02X%02X%02X%02X%02X", BACKUP_DIR_ROOT,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void backup_path_join(const char *dir, const char *name, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", dir, name);
}

bool backup_stamp_plausible(const backup_stamp_t *t)
{
    if (t->year < BACKUP_YEAR_MIN || t->year > BACKUP_YEAR_MAX)
        return false;
    if (t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31)
        return false;
    if (t->hour > 23 || t->minute > 59 || t->second > 60)   // 60 for a leap second
        return false;
    return true;
}

// "wifi-" or "undo4-", the one thing both name forms share.
static void name_prefix(char *out, size_t out_len, uint8_t undo_slot)
{
    if (undo_slot == 0)
        snprintf(out, out_len, "wifi");
    else
        snprintf(out, out_len, "undo%u", undo_slot);
}

void backup_name_stamped(char *out, size_t out_len, uint8_t undo_slot,
                         const backup_stamp_t *t)
{
    char prefix[8];
    name_prefix(prefix, sizeof(prefix), undo_slot);

    snprintf(out, out_len, "%s-%04u%02u%02u-%02u%02u%02u%s", prefix,
             t->year, t->month, t->day, t->hour, t->minute, t->second, BACKUP_EXT);
}

void backup_name_indexed(char *out, size_t out_len, uint8_t undo_slot, uint16_t index)
{
    char prefix[8];
    name_prefix(prefix, sizeof(prefix), undo_slot);

    index %= (BACKUP_MAX_INDEX + 1);

    // An ordinary backup keeps the flat wifiNNN form it has always had; only the undo
    // copies need the separator, because their prefix already ends in a digit.
    if (undo_slot == 0)
        snprintf(out, out_len, "%s%03u%s", prefix, index, BACKUP_EXT);
    else
        snprintf(out, out_len, "%s-%03u%s", prefix, index, BACKUP_EXT);
}
