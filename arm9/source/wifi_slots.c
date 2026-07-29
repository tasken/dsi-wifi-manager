// SPDX-License-Identifier: CC0-1.0
//
// See wifi_slots.h. Portable C -- no libnds, no I/O.

#include <string.h>

#include "wifi_slots.h"

// Record field offsets, GBATEK "DS Firmware WiFi Internet Access Points".
#define OFF_SSID        0x40
#define OFF_IP          0xC0
#define OFF_GATEWAY     0xC4
#define OFF_DNS1        0xC8
#define OFF_DNS2        0xCC
#define OFF_SUBNET      0xD0
#define OFF_MTU         0xEA
#define OFF_CONFIG      0xEF
#define OFF_WEP_MODE    0xE6
#define OFF_STATUS      0xE7
#define OFF_CRC         0xFE
#define OFF_SECURITY    0x181
#define OFF_CRC2        0x1FE

#define CRC_RANGE_END   0xFE    // CRC covers [0x00,0xFE)
#define CRC2_RANGE_BEG  0x100   // second CRC covers [0x100,0x1FE)
#define CRC2_RANGE_END  0x1FE

#define STATUS_FREE     0xFF

// Header offsets, GBATEK "DS Firmware User Settings".
#define HDR_CONSOLE_TYPE    0x1D
#define HDR_USER_SETTINGS   0x20

uint16_t wifi_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t c = 0x0000;

    for (uint32_t i = 0; i < len; i++)
    {
        c ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            c = (c >> 1) ^ ((c & 1) ? 0xA001 : 0);
    }

    return c;
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

wifi_layout_err_t wifi_layout_derive(const uint8_t *header, uint32_t flash_len,
                                     wifi_layout_t *out)
{
    uint32_t base = (uint32_t)read_u16(&header[HDR_USER_SETTINGS]) * 8;

    if (base == 0)
        return WIFI_LAYOUT_ERR_BASE_ZERO;

    // Slot 4 sits at base-0xA00; anything lower would wrap.
    if (base < 0xA00)
        return WIFI_LAYOUT_ERR_BASE_LOW;

    uint32_t limit = (flash_len != 0) ? flash_len : WIFI_FLASH_MAX_LEN;
    if (base + 0x200 > limit)
        return WIFI_LAYOUT_ERR_BASE_HIGH;

    memset(out, 0, sizeof(*out));
    out->base = base;
    out->implied_size = base + 0x200;
    out->console_type = header[HDR_CONSOLE_TYPE];
    out->is_dsi = (out->console_type == WIFI_CONSOLE_TYPE_DSI);

    static const struct { uint8_t number; uint32_t back; } ntr[] = {
        { 1, 0x400 }, { 2, 0x300 }, { 3, 0x200 },
    };
    static const struct { uint8_t number; uint32_t back; } twl[] = {
        { 4, 0xA00 }, { 5, 0x800 }, { 6, 0x600 },
    };

    uint8_t n = 0;
    for (unsigned i = 0; i < sizeof(ntr) / sizeof(ntr[0]); i++)
    {
        out->slots[n].number = ntr[i].number;
        out->slots[n].family = WIFI_FAMILY_NTR;
        out->slots[n].offset = base - ntr[i].back;
        out->slots[n].length = WIFI_NTR_LEN;
        n++;
    }

    if (out->is_dsi)
    {
        for (unsigned i = 0; i < sizeof(twl) / sizeof(twl[0]); i++)
        {
            out->slots[n].number = twl[i].number;
            out->slots[n].family = WIFI_FAMILY_TWL;
            out->slots[n].offset = base - twl[i].back;
            out->slots[n].length = WIFI_TWL_LEN;
            n++;
        }
    }

    out->count = n;

    out->region_start = out->slots[0].offset;
    for (uint8_t i = 1; i < n; i++)
    {
        if (out->slots[i].offset < out->region_start)
            out->region_start = out->slots[i].offset;
    }

    return WIFI_LAYOUT_OK;
}

const char *wifi_layout_strerror(wifi_layout_err_t err)
{
    switch (err)
    {
        case WIFI_LAYOUT_OK:            return "ok";
        case WIFI_LAYOUT_ERR_BASE_ZERO: return "pointer at 0x20 is zero";
        case WIFI_LAYOUT_ERR_BASE_LOW:  return "base too low for a wifi region";
        case WIFI_LAYOUT_ERR_BASE_HIGH: return "base runs past the end of the chip";
    }
    return "unknown";
}

void wifi_slot_parse(const wifi_slot_pos_t *pos, const uint8_t *record, wifi_slot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->number = pos->number;
    out->family = pos->family;
    out->offset = pos->offset;
    out->length = pos->length;

    out->crc_ok = (wifi_crc16(record, CRC_RANGE_END) == read_u16(&record[OFF_CRC]));

    if (pos->family == WIFI_FAMILY_TWL)
    {
        out->has_crc2 = true;
        out->crc2_ok = (wifi_crc16(&record[CRC2_RANGE_BEG], CRC2_RANGE_END - CRC2_RANGE_BEG)
                        == read_u16(&record[OFF_CRC2]));
        out->has_security = true;
        out->security = record[OFF_SECURITY];
    }

    out->status = record[OFF_STATUS];
    out->is_free = (out->status == STATUS_FREE);
    out->wep_mode = record[OFF_WEP_MODE];

    // Copied verbatim, interpreted nowhere here. Several of these read as errors on a
    // perfectly good connection -- see the comment on the struct -- and a decoder that
    // decides what they mean is a decoder that can be wrong about it in one place.
    memcpy(out->ip, &record[OFF_IP], 4);
    memcpy(out->gateway, &record[OFF_GATEWAY], 4);
    memcpy(out->dns1, &record[OFF_DNS1], 4);
    memcpy(out->dns2, &record[OFF_DNS2], 4);
    out->subnet_prefix = record[OFF_SUBNET];
    out->mtu = read_u16(&record[OFF_MTU]);
    out->config_bits = record[OFF_CONFIG];

    // The SSID field is NUL-padded; anything after the first NUL is not ours to read.
    const uint8_t *ssid = &record[OFF_SSID];
    uint8_t len = 0;
    while (len < WIFI_SSID_MAX && ssid[len] != 0)
        len++;

    out->ssid_len = len;
    out->ssid_printable = true;
    for (uint8_t i = 0; i < len; i++)
    {
        uint8_t c = ssid[i];
        if (c < 0x20 || c > 0x7E)
        {
            out->ssid[i] = '?';
            out->ssid_printable = false;
        }
        else
        {
            out->ssid[i] = (char)c;
        }
    }
    out->ssid[len] = '\0';
}

uint8_t wifi_slot_security_byte(const wifi_slot_t *s)
{
    return s->has_security ? s->security : s->wep_mode;
}

const char *wifi_security_label(const wifi_slot_t *s)
{
    if (s->has_security)
    {
        // 0x07 is confirmed against hardware: a slot 4 configured on a WPA2-PSK (AES)
        // network stores it. The rest are still read off the order of options in the
        // System Settings menu and keep their '?' until one of those networks is
        // configured and dumped. The raw byte is shown beside the label either way.
        switch (s->security)
        {
            case 0x00: return "open";
            case 0x01: return "WEP";
            case 0x04: return "WPA-TKIP?";
            case 0x05: return "WPA2-TKIP?";
            case 0x06: return "WPA-AES?";
            case 0x07: return "WPA2-AES";
            default:   return "undocumented";
        }
    }

    switch (s->wep_mode)
    {
        case 0x00: return "open";
        case 0x01: return "WEP 64";
        case 0x02: return "WEP 128";
        case 0x03: return "WEP 152";
        default:   return "undocumented";
    }
}

bool wifi_addr_is_zero(const uint8_t addr[4])
{
    return (addr[0] | addr[1] | addr[2] | addr[3]) == 0;
}
