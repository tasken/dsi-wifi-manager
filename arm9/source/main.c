// SPDX-License-Identifier: CC0-1.0
//
// DSi WiFi Slot Manager: list the six WiFi slots, back one up to the SD card, and restore
// a record from a backup into a slot of the same family.
//
// Both screens are drawn as 16-bit bitmaps with fb_render.c putting text into them, which
// is what gets past the 32 columns a tile console is fixed at. libnds lives in this file
// and nowhere else in the app.
//
// This is the first build that can write to flash. Every path that does goes through
// restore_check() first, writes only the destination slot's own 0x100 or 0x200 bytes,
// and re-reads the slot afterwards to confirm what landed.

#include <dirent.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <fat.h>
#include <nds.h>

#include "backup_file.h"
#include "fb_render.h"
#include "restore.h"
#include "slot_list_view.h"
#include "wifi_slots.h"

// Debug affordances, set by the build from DSIWIFI_BUILD_KIND. Off unless the build says
// otherwise, so a stray compilation never quietly gains them.
#ifndef DSIWIFI_DEBUG
#define DSIWIFI_DEBUG 0
#endif

// readFirmware() hands the request to the ARM7 over the FIFO, and the ARM7 can only
// reach main RAM. A stack buffer would live in DTCM and make libnds bounce it through
// a malloc'd copy, so keep the buffers static: .bss is main RAM.
static uint8_t header[WIFI_HEADER_LEN];
static uint8_t records[WIFI_MAX_SLOTS][WIFI_TWL_LEN];
static wifi_slot_t slots[WIFI_MAX_SLOTS];

static uint8_t file_buffer[BACKUP_MAX_LEN];
static uint8_t verify_buffer[BACKUP_MAX_LEN];
static uint8_t scan_buffer[BACKUP_MAX_LEN];
static uint8_t write_buffer[WIFI_TWL_LEN];

static char backup_dir[BACKUP_DIR_LEN];
static char backup_name[BACKUP_NAME_LEN];
static char backup_path[BACKUP_PATH_LEN];

static backup_entry_t entries[BACKUP_PICK_MAX];
static uint8_t entry_count;

static wifi_layout_t layout;
static bool have_sd;

// --- the two screens -----------------------------------------------------------------
//
// Both engines draw a 16-bit bitmap background and fb_render.c writes text into it. The
// tile console this replaced was fixed at 32 columns whatever font it was given; 5-pixel
// glyphs give 51, which is what lets a whole 32-byte SSID sit in a list row.
//
// dsiOnly() returns before touching any video register when isDSiMode() is true, and
// never returns otherwise, so on every console this app runs on both engines are ours.
// It uses banks A through D itself on the path that never comes back here.

static uint16_t *pane_fb[2];    // indexed by view_pane_t

// The framebuffer has no cursor of its own, so the sink keeps one. Reset by the clear,
// which is why clearing and resetting are the same call: apart, they drift, and a screen
// drawn on top of the last one is the result.
static int pane_row[2];

static void pane_clear(view_pane_t pane)
{
    fb_clear(pane_fb[pane], FB_BG);
    pane_row[pane] = 0;
}

static void clear_bottom(void) { pane_clear(VIEW_BOTTOM); }
static void clear_top(void)    { pane_clear(VIEW_TOP); }

// The palette. The view layer says what a line means and this decides what that looks
// like, because the host harness answers the same question with a letter in a column.
static void attr_colours(view_attr_t attr, uint16_t *fg, uint16_t *bg)
{
    *fg = FB_TEXT;
    *bg = FB_BG;

    switch (attr)
    {
        case VIEW_DIM:    *fg = FB_SECONDARY; break;
        case VIEW_GOOD:   *fg = FB_GOOD;      break;
        case VIEW_BAD:    *fg = FB_DANGER;    break;
        // Drawn as a bar rather than a colour: the row under the cursor should be findable
        // without reading it, and fb_row_text paints the background the full width.
        case VIEW_CURSOR: *fg = FB_BG;        *bg = FB_ACCENT; break;
        default: break;
    }
}

// The view layer composes lines and hands them here with the pane and the attribute. This
// is the only thing in the app that knows a pane is a framebuffer.
static void fb_sink(view_pane_t pane, view_attr_t attr, const char *line)
{
    uint16_t fg, bg;
    attr_colours(attr, &fg, &bg);

    // Rows past the bottom are dropped by fb_row_text rather than wrapping round to the
    // top, so an over-long screen loses its tail instead of corrupting the one above it.
    // tools/crosscheck.py counts rows per screen so that shows up on a PC, not here.
    fb_row_text(pane_fb[pane], pane_row[pane]++, fg, bg, line);
}

// The handful of one-line progress and error messages this file emits directly. Routed
// through the same sink so they obey the same width discipline as every other line
// instead of being the only text in the app that escapes it.
static void msg(view_attr_t attr, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void msg(view_attr_t attr, const char *fmt, ...)
{
    char line[VIEW_COLS + 1];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    fb_sink(VIEW_BOTTOM, attr, line);
}

// A blank row. Its own function rather than msg(attr, "") because that is a formatted
// call with nothing to format, which -Wformat-zero-length rightly complains about.
static void blank(void)
{
    fb_sink(VIEW_BOTTOM, VIEW_PLAIN, "");
}


static void hang(void)
{
    while (1)
        swiWaitForVBlank();
}

static void die(const char *what)
{
    blank();
    msg(VIEW_BAD, "%s", what);
    blank();
    msg(VIEW_PLAIN, "The flash was not written.");
    msg(VIEW_PLAIN, "Hold POWER to turn off.");
    hang();
}

// Waits for one of the keys in `mask` and returns it.
static uint32_t wait_for_key(uint32_t mask)
{
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        uint32_t down = keysDown();
        if (down & mask)
            return down & mask;
    }
}

// Clears the bottom screen only: whatever context the top pane holds is what the user
// needs in order to make sense of the message.
//
// `attr` colours the title. Use VIEW_BAD for a refusal or a failure and VIEW_GOOD for an
// outcome that is simply finished: painting "there is nothing to do" in red reads as an
// error, and this app says enough alarming things where it means them.
static void message_a(view_attr_t attr, const char *title,
                      const char *l1, const char *l2, const char *l3)
{
    clear_bottom();
    blank();
    msg(attr, "%s", title);
    blank();
    if (l1)
        msg(VIEW_PLAIN, "%s", l1);
    if (l2)
        msg(VIEW_PLAIN, "%s", l2);
    if (l3)
        msg(VIEW_PLAIN, "%s", l3);
    blank();
    msg(VIEW_DIM, "B: back");
    wait_for_key(KEY_B);
}

// The common case: something was refused or went wrong, so the title is loud.
static void message(const char *title, const char *l1, const char *l2, const char *l3)
{
    message_a(VIEW_BAD, title, l1, l2, l3);
}

// --- naming ------------------------------------------------------------------------

static bool read_stamp(backup_stamp_t *out)
{
    time_t now = time(NULL);
    if (now == (time_t)-1)
        return false;

    struct tm *t = localtime(&now);
    if (t == NULL)
        return false;

    out->year = (uint16_t)(t->tm_year + 1900);
    out->month = (uint8_t)(t->tm_mon + 1);
    out->day = (uint8_t)t->tm_mday;
    out->hour = (uint8_t)t->tm_hour;
    out->minute = (uint8_t)t->tm_min;
    out->second = (uint8_t)t->tm_sec;

    return backup_stamp_plausible(out);
}

// Fills backup_name and backup_path with a name that does not exist yet. Prefers the
// clock; falls back to the index if the RTC is unset or two files land in one second.
static bool pick_name(const char *dir, uint8_t undo_slot)
{
    backup_stamp_t t;

    if (read_stamp(&t))
    {
        backup_name_stamped(backup_name, sizeof(backup_name), undo_slot, &t);
        backup_path_join(dir, backup_name, backup_path, sizeof(backup_path));
        if (access(backup_path, F_OK) != 0)
            return true;
    }

    for (uint16_t i = 0; i <= BACKUP_MAX_INDEX; i++)
    {
        backup_name_indexed(backup_name, sizeof(backup_name), undo_slot, i);
        backup_path_join(dir, backup_name, backup_path, sizeof(backup_path));
        if (access(backup_path, F_OK) != 0)
            return true;
    }

    return false;
}

// --- writing a backup ----------------------------------------------------------------

// Serialise, write, then read back and check byte for byte before calling it done. A
// backup that cannot be parsed is worse than no backup: it is discovered when it is
// needed. Returns bytes written, or 0 with *detail set.
static uint32_t write_records(const backup_record_t *recs, uint8_t n, const char **detail)
{
    backup_err_t err;
    uint32_t len = backup_build(file_buffer, sizeof(file_buffer), recs, n, &err);
    if (len == 0)
    {
        *detail = backup_strerror(err);
        return 0;
    }

    mkdir(BACKUP_DIR_ROOT, 0777);
    mkdir(backup_dir, 0777);

    FILE *f = fopen(backup_path, "wb");
    if (f == NULL)
    {
        *detail = "could not create the file";
        return 0;
    }

    size_t written = fwrite(file_buffer, 1, len, f);
    int closed = fclose(f);

    if (written != len || closed != 0)
    {
        *detail = "the SD card write was short";
        return 0;
    }

    f = fopen(backup_path, "rb");
    if (f == NULL)
    {
        *detail = "wrote the file but cannot reopen it";
        return 0;
    }

    size_t got = fread(verify_buffer, 1, sizeof(verify_buffer), f);
    fclose(f);

    if (got != len || memcmp(verify_buffer, file_buffer, len) != 0)
    {
        *detail = "the file on the card differs";
        return 0;
    }

    backup_record_t parsed[WIFI_MAX_SLOTS];
    uint8_t parsed_count = 0;
    backup_err_t perr = backup_parse(verify_buffer, (uint32_t)got, parsed,
                                     WIFI_MAX_SLOTS, &parsed_count);
    if (perr != BACKUP_OK || parsed_count != n)
    {
        *detail = backup_strerror(perr);
        return 0;
    }

    for (uint8_t i = 0; i < n; i++)
    {
        if (parsed[i].slot != recs[i].slot || parsed[i].length != recs[i].length ||
            memcmp(parsed[i].data, recs[i].data, recs[i].length) != 0)
        {
            *detail = "a record did not survive the round trip";
            return 0;
        }
    }

    return len;
}

// One slot per file: the unit the user thinks in is "my home network", and a file
// holding exactly one makes both the picker and the restore step obvious.
static uint32_t write_backup(uint8_t index, const char **detail)
{
    backup_record_t rec = {
        .slot = slots[index].number,
        .family = (uint8_t)slots[index].family,
        .length = slots[index].length,
        .data = records[index],
    };

    return write_records(&rec, 1, detail);
}

// --- finding backups on the card ------------------------------------------------------

static bool ends_with_ext(const char *name)
{
    size_t n = strlen(name);
    size_t e = strlen(BACKUP_EXT);
    return n > e && strcmp(name + n - e, BACKUP_EXT) == 0;
}

static void scan_one(const char *dir, const char *name)
{
    backup_entry_t *e = &entries[entry_count];

    memset(e, 0, sizeof(*e));
    snprintf(e->dir, sizeof(e->dir), "%s", dir);
    snprintf(e->name, sizeof(e->name), "%s", name);

    char path[BACKUP_PATH_LEN];
    backup_path_join(dir, name, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        e->problem = "cannot open";
        entry_count++;
        return;
    }

    size_t got = fread(scan_buffer, 1, sizeof(scan_buffer), f);
    fclose(f);

    backup_record_t recs[WIFI_MAX_SLOTS];
    uint8_t count = 0;
    backup_err_t err = backup_parse(scan_buffer, (uint32_t)got, recs, WIFI_MAX_SLOTS,
                                    &count);
    if (err != BACKUP_OK)
    {
        e->problem = backup_strerror(err);
        entry_count++;
        return;
    }

    for (uint8_t i = 0; i < count; i++)
    {
        // The record's flash offset is not knowable from a file and is never used for
        // one; what matters is the slot it came from and its family.
        wifi_slot_pos_t pos = {
            .number = recs[i].slot,
            .family = (wifi_family_t)recs[i].family,
            .offset = 0,
            .length = recs[i].length,
        };
        wifi_slot_parse(&pos, recs[i].data, &e->rec[i]);
    }

    e->count = count;
    e->ok = true;
    entry_count++;
}

// Every folder under DSIWIFI/, not just this console's: a record is portable between
// consoles, so a backup copied from another one has to be reachable.
static void scan_backups(void)
{
    entry_count = 0;

    DIR *root = opendir(BACKUP_DIR_ROOT);
    if (root == NULL)
        return;

    struct dirent *de;
    while ((de = readdir(root)) != NULL && entry_count < BACKUP_PICK_MAX)
    {
        if (de->d_name[0] == '.')
            continue;

        char dir[BACKUP_DIR_LEN];
        if (snprintf(dir, sizeof(dir), "%s/%s", BACKUP_DIR_ROOT, de->d_name) >=
            (int)sizeof(dir))
            continue;   // not a folder this app could have written

        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        DIR *sub = opendir(dir);
        if (sub == NULL)
            continue;

        struct dirent *fe;
        while ((fe = readdir(sub)) != NULL && entry_count < BACKUP_PICK_MAX)
        {
            if (fe->d_name[0] == '.' || !ends_with_ext(fe->d_name))
                continue;
            if (strlen(fe->d_name) >= BACKUP_NAME_LEN)
                continue;
            scan_one(dir, fe->d_name);
        }

        closedir(sub);
    }

    closedir(root);
}

// --- screens -------------------------------------------------------------------------

static void draw_list(uint8_t cursor)
{
    // The top pane follows the cursor. It is the app's detail view: the list row cuts the
    // SSID to about 20 columns and this is where the whole one is legible.
    clear_top();
    view_detail(&layout, &slots[cursor]);

    clear_bottom();
    view_list_title();
    for (uint8_t i = 0; i < layout.count; i++)
        view_slot(&slots[i], i == cursor);
    view_keys();
}

// Runs a cursor over `n` items, redrawing through `draw`. Returns the chosen index, or
// -1 if the user backed out.
typedef void (*draw_fn)(uint8_t cursor, void *ctx);

static int choose(uint8_t n, draw_fn draw, void *ctx)
{
    if (n == 0)
        return -1;

    uint8_t cursor = 0;
    draw(cursor, ctx);

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        uint32_t down = keysDown();

        if (down & KEY_B)
            return -1;
        if (down & KEY_A)
            return cursor;

        if (down & KEY_UP)
        {
            cursor = (cursor == 0) ? (uint8_t)(n - 1) : (uint8_t)(cursor - 1);
            draw(cursor, ctx);
        }
        else if (down & KEY_DOWN)
        {
            cursor = (uint8_t)((cursor + 1) % n);
            draw(cursor, ctx);
        }
    }
}

// The restore flow is five screens deep, and each one replaces the last. This is what
// the top pane holds on to across all of them, filled in as the user descends.
static view_restore_ctx_t restore_ctx;

static void draw_restore_top(void)
{
    clear_top();
    view_restore_context(&restore_ctx);
}

static void draw_file_pick(uint8_t cursor, void *ctx)
{
    (void)ctx;

    // The pane tracks the cursor here too: the file under it is the one being described,
    // and it stays described once it has been picked.
    restore_ctx.file = entries[cursor].name;
    restore_ctx.dir = entries[cursor].dir;
    restore_ctx.source = (entries[cursor].ok && entries[cursor].count == 1)
                       ? &entries[cursor].rec[0] : NULL;
    draw_restore_top();

    clear_bottom();

    // Keep the cursor inside the visible window.
    uint8_t top = 0;
    if (cursor >= VIEW_PICK_ROWS)
        top = (uint8_t)(cursor - VIEW_PICK_ROWS + 1);

    view_pick_file(entries, entry_count, cursor, top);
}

static void draw_record_pick(uint8_t cursor, void *ctx)
{
    const backup_entry_t *e = (const backup_entry_t *)ctx;

    restore_ctx.source = &e->rec[cursor];
    draw_restore_top();

    clear_bottom();
    view_pick_record(e, cursor);
}

// --- restore ---------------------------------------------------------------------------

// Save the destination slot's current bytes before anything overwrites them. Returns
// false and leaves *detail set if it could not, in which case the restore is abandoned:
// the user asked for a copy, so proceeding without one is not what they agreed to.
static bool write_undo(uint8_t dest_index, char *name_out, size_t name_len,
                       const char **detail)
{
    if (!pick_name(backup_dir, slots[dest_index].number))
    {
        *detail = "no free file name for the copy";
        return false;
    }

    backup_record_t rec = {
        .slot = slots[dest_index].number,
        .family = (uint8_t)slots[dest_index].family,
        .length = slots[dest_index].length,
        .data = records[dest_index],
    };

    if (write_records(&rec, 1, detail) == 0)
        return false;

    snprintf(name_out, name_len, "%s", backup_name);
    return true;
}

// The destination is decided before this runs: the user entered that slot and chose
// "restore into it". So there is no destination step, and the family it accepts is known
// while the file picker is still being drawn.
static void do_restore(uint8_t dest_index)
{
    memset(&restore_ctx, 0, sizeof(restore_ctx));
    restore_ctx.dest = &slots[dest_index];

    if (!have_sd)
    {
        message("No SD card.", "The card did not mount, so",
                "there is nothing to read.", NULL);
        return;
    }

    clear_top();
    view_restore_context(&restore_ctx);

    clear_bottom();
    blank();
    msg(VIEW_PLAIN, "Scanning DSIWIFI/ ...");
    scan_backups();

    if (entry_count == 0)
    {
        clear_bottom();
        view_no_backups();
        blank();
        msg(VIEW_DIM, "B: back");
        wait_for_key(KEY_B);
        return;
    }

    int fi = choose(entry_count, draw_file_pick, NULL);
    if (fi < 0)
        return;

    backup_entry_t *e = &entries[fi];
    if (!e->ok)
    {
        message("That file cannot be read.",
                e->problem ? e->problem : "bad file", NULL, NULL);
        return;
    }

    // The app writes one record per file, so normally there is nothing to pick. The
    // screen is still here for the multi-record files earlier versions wrote, and for
    // any made by hand -- backup_parse still reads them.
    int ri = 0;
    if (e->count > 1)
    {
        ri = choose(e->count, draw_record_pick, e);
        if (ri < 0)
            return;
    }

    const wifi_slot_t *source = &e->rec[ri];

    // Pin the pane to what was actually chosen. Up to here the pickers moved it with the
    // cursor; from here it must stop moving.
    restore_ctx.file = e->name;
    restore_ctx.dir = e->dir;
    restore_ctx.source = source;

    // A 0x100 record has no room for a WPA passphrase or its PSK. The destination is fixed
    // now, so a record of the wrong family is refused here rather than being offered a slot
    // list it cannot use. restore_check() still has the final say either way.
    if (source->family != slots[dest_index].family)
    {
        message("That backup does not fit here.",
                (source->family == WIFI_FAMILY_TWL)
                    ? "It holds a WPA network, which needs"
                    : "It holds a DS-era record, which needs",
                (source->family == WIFI_FAMILY_TWL)
                    ? "slots 4-6. This is slot 1-3."
                    : "slots 1-3. This is slot 4-6.", NULL);
        return;
    }

    draw_restore_top();

    // The file has to be reopened: the picker holds decoded fields, not raw bytes, and
    // the bytes are what gets written.
    char path[BACKUP_PATH_LEN];
    backup_path_join(e->dir, e->name, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        message("Cannot reopen that file.", path, NULL, NULL);
        return;
    }
    size_t got = fread(scan_buffer, 1, sizeof(scan_buffer), f);
    fclose(f);

    backup_record_t recs[WIFI_MAX_SLOTS];
    uint8_t count = 0;
    if (backup_parse(scan_buffer, (uint32_t)got, recs, WIFI_MAX_SLOTS, &count) !=
        BACKUP_OK || ri >= count)
    {
        message("That file changed.", "It no longer parses the way",
                "it did a moment ago.", NULL);
        return;
    }

    const backup_record_t *rec = &recs[ri];
    const wifi_slot_pos_t *dest_pos = restore_find_slot(&layout,
                                                        slots[dest_index].number);

    restore_err_t rerr = restore_check(rec, dest_pos);
    if (rerr != RESTORE_OK)
    {
        message("Refusing to write.", restore_strerror(rerr),
                "Nothing was written.", NULL);
        return;
    }

    bool noop = restore_is_noop(rec, records[dest_index]);

    restore_ctx.noop = noop;
    restore_ctx.noop_known = true;

    // Stop here when the slot already holds these exact bytes. The write would program
    // nothing -- libnds compares each page before erasing it -- so walking the user through
    // an undo prompt, a confirmation and a result screen only to report that nothing
    // happened is three screens spent saying "no".
    //
    // Note for anyone reading the project history: restoring a slot onto itself used to be
    // the recommended first hardware test of the write path, precisely because it programs
    // zero bytes. That test is no longer reachable from the UI, and it has done its job --
    // it passed on the reference DSi and in melonDS. Exercising writeFirmware now means
    // restoring into a slot that does not already match.
    if (noop)
    {
        draw_restore_top();
        clear_bottom();
        view_noop_notice(&slots[dest_index], DSIWIFI_DEBUG != 0);

        // A Dev build can still run the write. It programs nothing -- libnds skips any page
        // whose contents already match -- so this is the one way to exercise writeFirmware
        // end to end with nothing at risk. A Release build is not offered it.
        uint32_t answer = wait_for_key(DSIWIFI_DEBUG ? (KEY_X | KEY_B) : KEY_B);
        if (!(answer & KEY_X))
            return;
    }

    // Only offer the safety copy when there is something to lose: not for a free
    // destination, and not when the bytes already match -- a copy of a slot that is not
    // about to change is a file written for nothing.
    char undo_name[BACKUP_NAME_LEN];
    const char *undo = NULL;

    if (!slots[dest_index].is_free && !noop)
    {
        // Drawn before the prompt, so the question is asked with the slot it is about
        // still on screen above it.
        draw_restore_top();

        clear_bottom();
        view_undo_prompt(&slots[dest_index]);

        uint32_t answer = wait_for_key(KEY_A | KEY_X | KEY_B);
        if (answer & KEY_B)
            return;

        if (answer & KEY_A)
        {
            clear_bottom();
            blank();
            msg(VIEW_PLAIN, "Saving a copy of slot %u ...", slots[dest_index].number);

            const char *detail = NULL;
            if (!write_undo((uint8_t)dest_index, undo_name, sizeof(undo_name), &detail))
            {
                message("Could not save the copy.", detail,
                        "Nothing was written to flash.", NULL);
                return;
            }
            undo = undo_name;
        }
    }

    restore_ctx.undo = undo;
    restore_ctx.undo_settled = true;
    draw_restore_top();

    clear_bottom();
    view_restore_confirm(source, &slots[dest_index], undo);
    if (!(wait_for_key(KEY_A | KEY_B) & KEY_A))
        return;

    clear_bottom();
    blank();
    msg(VIEW_BAD, "Writing slot %u ...", slots[dest_index].number);

    // writeFirmware needs the source in main RAM and takes a non-const pointer.
    memcpy(write_buffer, rec->data, rec->length);

    const char *detail = NULL;
    bool ok = (writeFirmware(dest_pos->offset, write_buffer, rec->length) == 0);
    if (!ok)
        detail = "writeFirmware refused the write";

    // Re-read regardless of what the write claimed, and believe the flash rather than
    // the return value.
    const wifi_slot_t *now = NULL;

    if (readFirmware(dest_pos->offset, records[dest_index], dest_pos->length) != 0)
    {
        ok = false;
        detail = "cannot read the slot back";
    }
    else
    {
        wifi_slot_parse(dest_pos, records[dest_index], &slots[dest_index]);
        now = &slots[dest_index];

        if (ok && memcmp(records[dest_index], rec->data, rec->length) != 0)
        {
            ok = false;
            detail = "the slot does not hold what was written";
        }
    }

    // The destination was re-read, so the pane now describes what the slot actually
    // holds rather than what was asked for.
    clear_top();
    if (now != NULL)
        view_detail(&layout, now);
    else
        view_idle_context(&layout);

    clear_bottom();
    view_restore_result(ok, slots[dest_index].number, now, undo, detail);
    wait_for_key(KEY_B);
}

// --- main -------------------------------------------------------------------------------

static void do_backup(uint8_t index)
{
    // No free-slot guard here any more. view_slot_action_count() does not offer backup on a
    // free slot, so this is unreachable for one -- and restore_check()/backup_build still
    // refuse such a record if anything else ever calls in.
    if (!have_sd)
    {
        message("No SD card.", "The card did not mount, so",
                "there is nowhere to write.", NULL);
        return;
    }

    if (!pick_name(backup_dir, 0))
    {
        message("No free file name.", "Every name this app can use",
                "is taken. Move some off the", "card first.");
        return;
    }

    clear_top();
    view_backup_context(&slots[index], backup_dir, backup_name);

    clear_bottom();
    view_confirm(&slots[index], backup_dir, backup_name);
    if (!(wait_for_key(KEY_A | KEY_B) & KEY_A))
        return;

    const char *detail = NULL;
    uint32_t bytes = write_backup(index, &detail);

    clear_bottom();
    view_result(bytes != 0, backup_dir, backup_name, bytes, 1, detail);
    wait_for_key(KEY_B);
}

// --- the slot screen -------------------------------------------------------------------

static void draw_slot_screen(uint8_t cursor, void *ctx)
{
    const wifi_slot_t *s = (const wifi_slot_t *)ctx;

    clear_top();
    view_detail(&layout, s);

    clear_bottom();
    view_slot_screen(s, cursor);
}

// One slot, its details and what can be done to it. Returns when the user backs out.
static void enter_slot(uint8_t index)
{
    // Loops rather than returning: after an action the user lands back on this slot's
    // screen, not on the list. A restore that just wrote is most likely to be followed by
    // looking at what landed, and the top pane has re-read the slot by then.
    //
    // The action cursor starts at the top each time. Remembering the last pick was tried
    // and removed: choose() has no way to take a starting position, and adding one to carry
    // a preference nobody asked for is not worth the parameter.
    while (1)
    {
        uint8_t n = view_slot_action_count(&slots[index]);
        int pick = choose(n, draw_slot_screen, &slots[index]);
        if (pick < 0)
            return;

        switch (view_slot_action_at(&slots[index], (uint8_t)pick))
        {
            case SLOT_ACTION_BACKUP:  do_backup(index); break;
            case SLOT_ACTION_RESTORE: do_restore(index); break;
            default: break;
        }
    }
}

int main(void)
{
    // Shows the "DSi only" screen and never returns if this is a DS, or a DSi booted
    // in DS mode -- where slots 4-6 do not exist and the flash reads differently.
    extern void dsiOnly(void);
    dsiOnly();

    // A 16-bit bitmap background on each engine, which fb_render.c draws text into.
    //
    // BgSize_B16_256x256 is 128 KB, exactly one VRAM bank, and only the top 192 rows are
    // ever displayed or written. Bank A is the main engine's and bank C the sub's; nothing
    // else in this app wants either.
    setBrightness(2, 0);

    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int top_bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bottom_bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);

    pane_fb[VIEW_TOP] = bgGetGfxPtr(top_bg);
    pane_fb[VIEW_BOTTOM] = bgGetGfxPtr(bottom_bg);

    // Both panes start black rather than showing whatever the loader left in VRAM. This
    // also sets the row counters, which is the same call on purpose.
    clear_top();
    clear_bottom();

    view_set_sink(fb_sink);

    if (readFirmware(0, header, sizeof(header)) != 0)
        die("Could not read the flash header.");

    // Nothing about the layout is hardcoded: a console whose flash differs decodes
    // correctly instead of us reading the wrong bytes and calling them settings.
    // Size is passed as 0 because nothing on the console reports the chip size.
    wifi_layout_err_t err = wifi_layout_derive(header, 0, &layout);
    if (err != WIFI_LAYOUT_OK)
    {
        blank();
        msg(VIEW_BAD, "Bad flash header: %s", wifi_layout_strerror(err));
        die("Refusing to guess at the layout.");
    }

    for (uint8_t i = 0; i < layout.count; i++)
    {
        const wifi_slot_pos_t *pos = &layout.slots[i];

        if (readFirmware(pos->offset, records[i], pos->length) != 0)
        {
            blank();
            msg(VIEW_BAD, "Could not read slot %u at 0x%05lX.",
                pos->number, (unsigned long)pos->offset);
            die("Aborting.");
        }

        wifi_slot_parse(pos, records[i], &slots[i]);
    }

    // The MAC at 0x36 only names the folder, so two consoles' backups cannot collide.
    backup_dir_for_mac(&header[0x36], backup_dir, sizeof(backup_dir));

    have_sd = fatInitDefault();

    uint8_t cursor = 0;
    draw_list(cursor);

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        uint32_t down = keysDown();

        if (down & KEY_START)
            break;

        if (down & KEY_UP)
        {
            cursor = (cursor == 0) ? (uint8_t)(layout.count - 1) : (uint8_t)(cursor - 1);
            draw_list(cursor);
        }
        else if (down & KEY_DOWN)
        {
            cursor = (uint8_t)((cursor + 1) % layout.count);
            draw_list(cursor);
        }
        else if (down & KEY_A)
        {
            enter_slot(cursor);
            draw_list(cursor);
        }
    }

    return 0;
}
