// Decoder for the six DSi Wi-Fi access-point records.
//
// Deliberately free of libnds: the ARM9 build compiles this file, and so does
// tools/host_slotlist.c under plain gcc, so the on-console decode can be diffed
// against tools/decode_wifi.py without hardware. Nothing here touches hardware --
// the caller supplies the bytes and says where they came from.
//
// Layout is derived from the flash header, never hardcoded (see wifi_layout_derive).
// Field offsets are GBATEK "DS Firmware WiFi Internet Access Points".

#ifndef WIFI_SLOTS_H
#define WIFI_SLOTS_H

#include <stdbool.h>
#include <stdint.h>

#define WIFI_NTR_LEN            0x100u  // slots 1-3
#define WIFI_TWL_LEN            0x200u  // slots 4-6
#define WIFI_MAX_SLOTS          6
#define WIFI_SSID_MAX           32      // field at 0x40, NUL-padded to 0x60

// Everything wifi_layout_derive() reads lives below 0x40: the console type at
// 0x1D and the user-settings pointer at 0x20.
#define WIFI_HEADER_LEN         0x40u

// The DSi's DS-mode flash is 128 KB. Accept up to 256 KB before calling a header
// insane, so an unexpected but self-consistent chip decodes rather than being
// rejected out of hand.
#define WIFI_FLASH_MAX_LEN      0x40000u

// flash[0x1D]. GBATEK: FFh=DS, 20h=DS Lite, 57h=DSi, 43h/63h=iQue. Only 57h has slots
// 4-6, so this is tested for equality and every other value takes the three-slot path
// -- never test against a specific non-DSi value. testdata/ds-original-256k.bin really
// does read FFh.
#define WIFI_CONSOLE_TYPE_DSI   0x57

typedef enum {
    WIFI_FAMILY_NTR = 0,    // 0x100 bytes, open/WEP only, also visible to the DS
    WIFI_FAMILY_TWL = 1     // 0x200 bytes, WPA/WPA2 + proxy, DSi only
} wifi_family_t;

// Where a slot lives. Produced by wifi_layout_derive(), consumed by the reader.
typedef struct {
    uint8_t         number;     // 1-6, as the user counts them
    wifi_family_t   family;
    uint32_t        offset;     // byte offset into the flash
    uint16_t        length;     // WIFI_NTR_LEN or WIFI_TWL_LEN
} wifi_slot_pos_t;

typedef struct {
    uint32_t        base;           // user-settings block: (u16 at 0x20) * 8
    uint32_t        implied_size;   // base + 0x200, i.e. what the header says the chip is
    uint32_t        region_start;   // lowest slot offset
    uint8_t         console_type;   // raw flash[0x1D], reported whatever it is
    bool            is_dsi;         // console_type == WIFI_CONSOLE_TYPE_DSI
    uint8_t         count;          // 3 on a DS, 6 on a DSi
    wifi_slot_pos_t slots[WIFI_MAX_SLOTS];
} wifi_layout_t;

typedef enum {
    WIFI_LAYOUT_OK = 0,
    WIFI_LAYOUT_ERR_BASE_ZERO,      // pointer at 0x20 is 0 -- blank or unreadable header
    WIFI_LAYOUT_ERR_BASE_LOW,       // base < 0xA00, so slot 4 would sit at a negative offset
    WIFI_LAYOUT_ERR_BASE_HIGH,      // base + 0x200 runs past the end of the chip
} wifi_layout_err_t;

// Derive the six slot offsets from the flash header the way fwTool does
// (firmware/nds/fwTool/arm9/source/main.cpp:255-267):
//
//     base = (u16 at 0x20) * 8
//     slots 1-3 at base-0x400, -0x300, -0x200
//     slots 4-6 at base-0xA00, -0x800, -0x600, only if flash[0x1D] == 0x57
//
// `header` must hold at least WIFI_HEADER_LEN bytes read from offset 0.
// `flash_len` is the size of the chip if the caller knows it (a file on a PC) or 0
// if it does not (the console, where nothing reports the size); 0 falls back to
// bounding against WIFI_FLASH_MAX_LEN.
//
// Slots come out sorted by number, so slots[i].number == i + 1.
wifi_layout_err_t wifi_layout_derive(const uint8_t *header, uint32_t flash_len,
                                     wifi_layout_t *out);

const char *wifi_layout_strerror(wifi_layout_err_t err);

typedef struct {
    uint8_t         number;
    wifi_family_t   family;
    uint32_t        offset;
    uint16_t        length;

    bool            crc_ok;         // CRC16 over [0x00,0xFE) against the u16 at 0xFE
    bool            has_crc2;       // TWL only
    bool            crc2_ok;        // CRC16 over [0x100,0x1FE) against the u16 at 0x1FE

    // An unconfigured slot is marked by its status byte, not by being blank: on real
    // hardware every slot carries a valid checksum and non-zero fields whether or not
    // it is in use. Testing for all-zero or all-FF bytes calls every slot populated.
    bool            is_free;        // status == 0xFF
    uint8_t         status;         // 0xE7

    uint8_t         wep_mode;       // 0xE6
    bool            has_security;   // TWL only
    uint8_t         security;       // 0x181

    // Display copy of the SSID at 0x40: NUL-terminated, bytes outside printable
    // ASCII replaced by '?' so a hostile or mojibake SSID cannot scribble on the
    // console. ssid_len is the true length in the record, before that substitution.
    char            ssid[WIFI_SSID_MAX + 1];
    uint8_t         ssid_len;
    bool            ssid_printable; // false if any byte was substituted

    // Network settings, present in both families at the same offsets. Stored raw and
    // interpreted at the point of display, because several of these look like errors and
    // are not: on the reference DSi a working connection reads subnet 0 and MTU 0, and
    // MTU 0 is the value System Settings later normalises to 1400.
    //
    // These are meaningless in a free slot -- the bytes are leftovers, exactly as its SSID
    // field is -- so nothing should show them when is_free.
    uint8_t         ip[4];          // 0xC0, all zero means the address comes from DHCP
    uint8_t         gateway[4];     // 0xC4
    uint8_t         dns1[4];        // 0xC8
    uint8_t         dns2[4];        // 0xCC
    uint8_t         subnet_prefix;  // 0xD0, a prefix length rather than a mask
    uint16_t        mtu;            // 0xEA, little-endian
    uint8_t         config_bits;    // 0xEF, meaning not established
} wifi_slot_t;

// True when all four bytes are zero. The address and the DNS servers are independent
// settings on real hardware -- the reference console takes its address from DHCP and has a
// manually set DNS 1 -- so this is asked separately about each field and never inferred
// from one to another.
bool wifi_addr_is_zero(const uint8_t addr[4]);

// Parse one record. `record` must hold pos->length bytes read from pos->offset.
void wifi_slot_parse(const wifi_slot_pos_t *pos, const uint8_t *record, wifi_slot_t *out);

// Security label for the list view. Reads the WEP mode on an NTR record and the DSi
// security byte on a TWL one; wifi_slot_security_byte() returns the byte it read, so
// callers can print the raw value beside the label. The WPA labels keep the '?' that
// tools/decode_wifi.py carries: their mapping is taken from the order of options in
// the System Settings menu and has never been checked against a real WPA record.
const char *wifi_security_label(const wifi_slot_t *s);
uint8_t wifi_slot_security_byte(const wifi_slot_t *s);

// Polynomial A001h, initial value 0000h. The init is 0000h and NOT the FFFFh the
// firmware *header* CRCs use; with FFFFh every valid record on a real console
// decodes as corrupt.
uint16_t wifi_crc16(const uint8_t *data, uint32_t len);

#endif // WIFI_SLOTS_H
