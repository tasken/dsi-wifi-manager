// The backup file format. Portable for the same reason wifi_slots.c is:
// tools/host_slotlist.c builds a file with this code and tools/decode_backup.py reads
// it back, so the format is exercised without a console.
//
//     "DSIWIFI1"  magic, 8 bytes, no terminator
//     u8          record count
//     per record:
//       u8        source slot (1-6)
//       u8        family (0 = NTR 0x100, 1 = TWL 0x200)
//       u16       length, little-endian
//       bytes     the raw flash record, unmodified
//
// The payload is raw flash bytes with nothing re-encoded, so a backup/restore round
// trip is byte-exact by construction. Each record carries its slot and family, so
// restore never has to infer them from length.
//
// A file written by this code holds Wi-Fi passphrases in plaintext, because that is how
// the hardware stores them. Anything that reads the SD card can read them.

#ifndef BACKUP_FILE_H
#define BACKUP_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_slots.h"

#define BACKUP_MAGIC            "DSIWIFI1"
#define BACKUP_MAGIC_LEN        8
#define BACKUP_HEADER_LEN       (BACKUP_MAGIC_LEN + 1)
#define BACKUP_REC_HEADER_LEN   4

// Every slot at once: three TWL records and three NTR records.
#define BACKUP_MAX_LEN  (BACKUP_HEADER_LEN + \
                         WIFI_MAX_SLOTS * BACKUP_REC_HEADER_LEN + \
                         3 * WIFI_TWL_LEN + 3 * WIFI_NTR_LEN)

// Directory is DSIWIFI/<MAC>/, 12 uppercase hex digits, matching fwTool's FW<MAC>/.
//
// The folder groups by console and shows provenance; it is NOT what prevents collisions --
// pick_name() checks access(F_OK) and never returns an existing name. Do not hash the MAC to
// "anonymise" it: 24 of its 48 bits are a known Nintendo OUI, so a digest is a 2^24 search.
#define BACKUP_DIR_ROOT     "DSIWIFI"
#define BACKUP_EXT          ".dswifi"
#define BACKUP_DIR_LEN      (sizeof(BACKUP_DIR_ROOT) + 12 + 1)
#define BACKUP_NAME_LEN     32
#define BACKUP_PATH_LEN     (BACKUP_DIR_LEN + BACKUP_NAME_LEN)
#define BACKUP_MAX_INDEX    999

// How many backup files the restore picker will hold at once.
#define BACKUP_PICK_MAX     24

typedef struct {
    uint8_t         slot;       // 1-6
    uint8_t         family;     // 0 = NTR, 1 = TWL
    uint16_t        length;
    const uint8_t   *data;      // raw record; borrowed, never owned
} backup_record_t;

// One backup file as the restore picker sees it: where it is, and what is inside it
// already decoded, so the pickers never have to reopen the card to draw a screen.
typedef struct {
    char        dir[BACKUP_DIR_LEN];
    char        name[BACKUP_NAME_LEN];
    uint8_t     count;                      // records, 0 if unreadable
    wifi_slot_t rec[WIFI_MAX_SLOTS];        // offset is meaningless here; number is the
                                            // slot the record was taken from
    // The picker describes these bytes before restore reopens the file. Keeping the exact
    // records lets the write gate reject a file changed after that preview, rather than
    // writing another valid record from the same path.
    uint8_t     data[WIFI_MAX_SLOTS][WIFI_TWL_LEN];
    bool        ok;
    const char  *problem;                   // why not, for the picker to show
} backup_entry_t;

typedef enum {
    BACKUP_OK = 0,
    BACKUP_ERR_SHORT,           // file cannot even hold a header
    BACKUP_ERR_MAGIC,
    BACKUP_ERR_COUNT,           // zero records, or more slots than exist
    BACKUP_ERR_TRUNCATED,       // a record header or payload runs off the end
    BACKUP_ERR_SLOT,            // slot outside 1-6, or not ascending
    BACKUP_ERR_FAMILY,          // family does not match the slot or the length
    BACKUP_ERR_TRAILING,        // bytes after the last record: not a file we wrote
    BACKUP_ERR_SPACE,           // build: the output buffer is too small
} backup_err_t;

const char *backup_strerror(backup_err_t err);

// Serialise `count` records into `buf`. Records must be in ascending slot order with no
// duplicates, and each family/length pair must agree with its slot number: that is an
// invariant of the hardware (slots 1-3 are always 0x100 NTR, 4-6 always 0x200 TWL), so a
// file that breaks it was not produced by reading a console.
// Returns the number of bytes written, or 0 on error with *err set.
uint32_t backup_build(uint8_t *buf, uint32_t buf_len, const backup_record_t *recs,
                      uint8_t count, backup_err_t *err);

// Parse a file. Records point into `buf`, which must outlive them. `*count_out` is set
// even on some failures, so callers can report how far parsing got.
backup_err_t backup_parse(const uint8_t *buf, uint32_t len, backup_record_t *out,
                          uint8_t out_max, uint8_t *count_out);

// The family a slot number must have. Slots 1-3 are NTR, 4-6 TWL -- fixed by the flash
// layout, not by anything in the file.
wifi_family_t backup_family_for_slot(uint8_t slot);
uint16_t backup_length_for_family(uint8_t family);

// "DSIWIFI/0009BF010203" from the six MAC bytes at flash 0x36.
void backup_dir_for_mac(const uint8_t mac[6], char *out, size_t out_len);

void backup_path_join(const char *dir, const char *name, char *out, size_t out_len);

// Backups are named from the console clock, because "which one do I want" is a question
// about when it was taken, and that is the only place the answer can be recorded.
typedef struct {
    uint16_t    year;
    uint8_t     month;      // 1-12
    uint8_t     day;        // 1-31
    uint8_t     hour;
    uint8_t     minute;
    uint8_t     second;
} backup_stamp_t;

// The DSi shipped in 2008. A clock outside this window has never been set, or is wrong,
// and a filename built from it would state something false about when the file was made
// -- which is worse than a filename that says nothing. Callers fall back to the index.
#define BACKUP_YEAR_MIN 2005
#define BACKUP_YEAR_MAX 2099

bool backup_stamp_plausible(const backup_stamp_t *t);

// `undo_slot` is 0 for an ordinary backup and 1-6 for the copy taken of a destination
// slot just before a restore overwrites it:
//
//     wifi-20260728-015530.dswifi     undo4-20260728-015530.dswifi
//     wifi000.dswifi                  undo4-001.dswifi
//
// Two backups inside one second would collide, and so would a clock that went backwards,
// so the caller checks the name is free and falls back to the indexed form if it is not.
void backup_name_stamped(char *out, size_t out_len, uint8_t undo_slot,
                         const backup_stamp_t *t);
void backup_name_indexed(char *out, size_t out_len, uint8_t undo_slot, uint16_t index);

#endif // BACKUP_FILE_H
