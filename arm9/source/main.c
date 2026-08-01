// DSi Wi-Fi Manager: list the six connections, back one up to the SD card, and restore
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

static uint16_t *pane_fb[VIEW_PANES];   // indexed by view_pane_t

// The framebuffer has no cursor of its own, so the sink keeps one. Reset by the clear,
// which is why clearing and resetting are the same call: apart, they drift, and a screen
// drawn on top of the last one is the result.
// Sized for every pane value, not just the two with a running row. VIEW_FOOTER is handled
// before these are indexed, and sizing them to match means a future reorder cannot turn
// that into an out-of-bounds write into a framebuffer pointer.
static int pane_row[VIEW_PANES];

// One blocking 32-bit DMA fill, the colour duplicated into both halves, instead of the 49152
// halfword stores fb_clear() does. Both panes are cleared on every flow transition and a restore
// is five screens deep, so that is around 500k stores per restore on a 67MHz ARM9.
//
// Deliberately here and not in fb_render.c. That file is compiled for the host by
// tools/crosscheck.py and run under UBSan, and storing a uint32_t through a uint16_t * is exactly
// what UBSan is there to catch. fb_clear() stays halfword-only and stays the version the harness
// and the renderer self-checks exercise.
//
// Safe on the DMA channel, established by disassembling this binary rather than taken on trust
// from the sibling project that suggested it.
//
// dmaFillWords compiles to DMA3: after building, the only DMA register this inlines is
// 0x040000DC, DMA3's control word. The only other DMA3 users in the image are
// initSystem -> vramDefault, which runs once before main(), and
// consoleInitEx -> consoleLoadFont, reachable only from __sassert and exceptionStatePrint. So
// nothing this app runs can be mid-transfer on DMA3 when a screen is cleared.
//
// Nothing on the write path goes near it either: every fifo* function including both IRQ
// handlers, readFirmware, writeFirmware and the sdmmc SD path reference DMA zero times, and the
// ARM7 binary contains no DMA at all -- its channels are a separate controller regardless.
//
// dmaFillWords also blocks, and this side is single-threaded, so a fill cannot overlap anything
// else we start. If libnds ever changes which channel it picks, re-check by grepping the
// disassembly for 0x040000B0 through 0x040000DC; nothing here depends on the number.
static void pane_fill(uint16_t *fb, uint16_t colour)
{
    const uint32_t pixel = colour;
    dmaFillWords(pixel | (pixel << 16), fb, FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
}

static void pane_clear(view_pane_t pane)
{
    pane_fill(pane_fb[pane], FB_BG);
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
        case VIEW_ACCENT: *fg = FB_ACCENT;    break;
        // Every Dev-only affordance shares this colour, so "this is not in a Release build"
        // reads without having to parse the words.
        case VIEW_DEBUG:  *fg = FB_WARN;      break;
        // Drawn as a bar rather than a colour: the row under the cursor should be findable
        // without reading it, and fb_row_text paints the background the full width.
        case VIEW_CURSOR: *fg = FB_BG;        *bg = FB_ACCENT; break;
        default: break;
    }
}

// The view layer composes lines and hands them here with the pane and the attribute. This
// is the only thing in the app that knows a pane is a framebuffer.
static void fb_sink(view_pane_t pane, view_attr_t attr, const char *line,
                    const view_attr_t *spans)
{
    uint16_t fg, bg;
    attr_colours(attr, &fg, &bg);

    // Rows past the bottom are dropped by fb_row_text rather than wrapping round to the
    // top, so an over-long screen loses its tail instead of corrupting the one above it.
    // tools/crosscheck.py counts rows per screen so that shows up on a PC, not here.
    // VIEW_FOOTER is pinned to the last row, so it does not move with the content above it and
    // does not consume a row from the flow. Drawn on the bottom screen, where the user acts.
    int row;
    switch (pane)
    {
        case VIEW_FOOTER: row = FB_ROWS - 1; break;   // last row
        case VIEW_HINT:   row = FB_ROWS - 3; break;   // one blank row above the footer
        default:          row = pane_row[pane]++; break;
    }
    uint16_t *fb = pane_fb[(pane == VIEW_FOOTER || pane == VIEW_HINT) ? VIEW_BOTTOM : pane];

    // The whole row first, so it paints the background and erases any longer line underneath.
    fb_row_text(fb, row, fg, bg, line);

    if (spans == NULL)
        return;

    // Then repaint only the characters whose own attribute differs, one cell at a time. fb_text
    // does not touch the rest of the row, so the background above survives.
    for (int col = 0; line[col] != '\0' && col < FB_COLS; col++)
    {
        if (spans[col] == attr)
            continue;

        uint16_t sfg, sbg;
        char one[2] = { line[col], '\0' };
        attr_colours(spans[col], &sfg, &sbg);
        fb_text(fb, col, row, sfg, sbg, one);
    }
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

    fb_sink(VIEW_BOTTOM, attr, line, NULL);
}

// A blank row. Its own function rather than msg(attr, "") because that is a formatted
// call with nothing to format, which -Wformat-zero-length rightly complains about.
static void blank(void)
{
    fb_sink(VIEW_BOTTOM, VIEW_PLAIN, "", NULL);
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

// --- entropy ---------------------------------------------------------------------------
//
// The confirmation sequence below has to be unguessable, and this console has no random
// source worth the name. time(NULL) is the obvious choice and a bad one: the RTC may never
// have been set, and even when it has, a sequence derived from the clock second is the same
// for everyone who reaches this screen in that second.
//
// So the source is human timing. This advances once per frame that the app spends waiting
// for input, which by the time anyone reaches a write is a number nobody could predict:
// it is the total of every hesitation in front of every menu on the way here.
static uint32_t entropy;

static uint32_t entropy_next(void)
{
    // xorshift32, so consecutive draws from a slowly-growing counter do not come out
    // correlated. Not cryptography, and it does not need to be -- it needs to stop a user
    // learning one sequence by heart.
    uint32_t x = entropy ? entropy : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    entropy = x;
    return x;
}

// Waits for one of the keys in `mask` and returns it.
static uint32_t wait_for_key(uint32_t mask)
{
    while (1)
    {
        swiWaitForVBlank();
        entropy++;
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
    // Same <KEY> Verb form and same verbs as every legend in slot_list_view.c. Emitted through
    // msg() rather than the view layer's footer, because this helper draws a whole screen by
    // itself and does not go through a view function.
    msg(VIEW_DIM, (attr == VIEW_GOOD) ? "<A> Continue" : "<B> Back");
    wait_for_key(KEY_A | KEY_B);
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

// `backup_parse()` rejects trailing bytes, but only receives what its caller read. Reading one
// byte beyond a full buffer closes the maximum-size-file loophole: a valid BACKUP_MAX_LEN prefix
// with appended data must not look like the valid file it starts with.
static const char *read_backup_file(const char *path, uint8_t *buf, size_t buf_len,
                                    size_t *got)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return "cannot open";

    *got = fread(buf, 1, buf_len, f);
    int extra = (*got == buf_len) ? fgetc(f) : EOF;
    bool failed = ferror(f) != 0;
    int closed = fclose(f);

    if (failed || closed != 0)
        return "could not read";
    if (extra != EOF)
        return "file is too large";
    return NULL;
}

static void scan_one(const char *dir, const char *name)
{
    backup_entry_t *e = &entries[entry_count];

    memset(e, 0, sizeof(*e));
    snprintf(e->dir, sizeof(e->dir), "%s", dir);
    snprintf(e->name, sizeof(e->name), "%s", name);

    char path[BACKUP_PATH_LEN];
    backup_path_join(dir, name, path, sizeof(path));

    size_t got = 0;
    const char *read_error = read_backup_file(path, scan_buffer, sizeof(scan_buffer), &got);
    if (read_error != NULL)
    {
        e->problem = read_error;
        entry_count++;
        return;
    }

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
        memcpy(e->data[i], recs[i].data, recs[i].length);
    }

    e->count = count;
    e->ok = true;
    entry_count++;
}

static bool record_matches_preview(const backup_entry_t *entry, uint8_t index,
                                   const backup_record_t *record)
{
    return index < entry->count &&
           record->slot == entry->rec[index].number &&
           record->family == (uint8_t)entry->rec[index].family &&
           record->length == entry->rec[index].length &&
           memcmp(record->data, entry->data[index], record->length) == 0;
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
    // Redrawn on every return from enter_conn(), which is safe because do_restore() re-parses
    // its destination into slots[] before returning -- so a row and the pane show what the flash
    // holds, not what was read at startup.
    clear_top();
    view_summary(&layout, &slots[cursor], layout.count);

    clear_bottom();
    view_list_title();
    for (uint8_t i = 0; i < layout.count; i++)
        view_conn_row(&slots[i], i == cursor);
    view_keys(DSIWIFI_DEBUG != 0);
}

// Runs a cursor over `n` items, redrawing through `draw`. Returns the chosen index, or
// -1 if the user backed out.
typedef void (*draw_fn)(uint8_t cursor, void *ctx);

// Runs a cursor over `n` items, redrawing through `draw`. Returns the chosen index, or
// CHOOSE_BACK if the user backed out.
//
// `extra` is an optional second action key. When it is pressed this returns CHOOSE_EXTRA and
// leaves the cursor position in *at, so a caller can offer something besides "pick this one"
// -- deleting a backup from the file picker, for instance -- without a parallel input loop
// that would drift from this one.
#define CHOOSE_BACK   (-1)
#define CHOOSE_EXTRA  (-2)

static int choose_with_from(uint8_t n, draw_fn draw, void *ctx, uint8_t initial,
                            uint32_t extra, uint8_t *at)
{
    if (n == 0)
        return CHOOSE_BACK;

    uint8_t cursor = (initial < n) ? initial : 0;
    draw(cursor, ctx);

    while (1)
    {
        swiWaitForVBlank();
        entropy++;
        scanKeys();
        uint32_t down = keysDown();

        if (down & KEY_B)
            return CHOOSE_BACK;
        if (down & KEY_A)
            return cursor;
        if (extra && (down & extra))
        {
            if (at != NULL)
                *at = cursor;
            return CHOOSE_EXTRA;
        }

        // Stops at the ends rather than wrapping, as Cart-Flasher's lists do. Wrapping makes the
        // first and last entries adjacent, which on the restore pickers means one press past the
        // end lands on a different destination than the one you were heading for. Redrawing only
        // on an actual move also keeps a held key from flickering the screen at a boundary.
        if ((down & KEY_UP) && cursor > 0)
        {
            cursor--;
            draw(cursor, ctx);
        }
        else if ((down & KEY_DOWN) && cursor + 1 < n)
        {
            cursor++;
            draw(cursor, ctx);
        }
    }
}

static int choose_with(uint8_t n, draw_fn draw, void *ctx, uint32_t extra, uint8_t *at)
{
    return choose_with_from(n, draw, ctx, 0, extra, at);
}

static int choose(uint8_t n, draw_fn draw, void *ctx)
{
    return choose_with(n, draw, ctx, 0, NULL);
}

// The restore flow is several screens deep and each one replaces the last. This is what the
// top pane holds on to across all of them, filled in as the user descends.
static view_restore_ctx_t restore_ctx;

static void draw_restore_top(void)
{
    clear_top();
    view_restore_context(&restore_ctx);
}

// Index into entries[] of the n'th backup that can go in this connection, or -1. The cursor
// counts only the ones that fit, because the picker only draws those.
static int nth_fitting(const wifi_slot_t *dest, uint8_t n)
{
    uint8_t seen = 0;
    for (uint8_t i = 0; i < entry_count; i++)
    {
        if (!view_entry_fits(&entries[i], dest))
            continue;
        if (seen == n)
            return (int)i;
        seen++;
    }
    return -1;
}

static void draw_file_pick(uint8_t cursor, void *ctx)
{
    const wifi_slot_t *dest = (const wifi_slot_t *)ctx;
    int idx = nth_fitting(dest, cursor);
    if (idx < 0)
        return;

    // The pane tracks the cursor here too. A multi-record file has no confirmed source yet,
    // so show its first compatible record as a preview rather than claiming it was selected.
    restore_ctx.file = entries[idx].name;
    restore_ctx.dir = entries[idx].dir;
    restore_ctx.source = NULL;
    restore_ctx.preview = NULL;
    for (uint8_t r = 0; r < entries[idx].count; r++)
    {
        if (view_record_fits(&entries[idx].rec[r], dest))
        {
            restore_ctx.preview = &entries[idx].rec[r];
            break;
        }
    }
    draw_restore_top();

    clear_bottom();

    // Keep the cursor inside the visible window.
    uint8_t top = 0;
    if (cursor >= VIEW_PICK_ROWS)
        top = (uint8_t)(cursor - VIEW_PICK_ROWS + 1);

    view_pick_file(entries, entry_count, cursor, top, dest);
}

static void draw_undo_prompt(uint8_t cursor, void *ctx)
{
    draw_restore_top();
    clear_bottom();
    view_undo_prompt((const wifi_slot_t *)ctx, cursor);
}

// Same shape for the records inside one file: a multi-record file can hold both families.
struct record_ctx {
    const backup_entry_t *entry;
    const wifi_slot_t *dest;
};

static int nth_fitting_record(const struct record_ctx *rc, uint8_t n)
{
    uint8_t seen = 0;
    for (uint8_t i = 0; i < rc->entry->count; i++)
    {
        if (!view_record_fits(&rc->entry->rec[i], rc->dest))
            continue;
        if (seen == n)
            return (int)i;
        seen++;
    }
    return -1;
}

static void draw_record_pick(uint8_t cursor, void *ctx)
{
    const struct record_ctx *rc = (const struct record_ctx *)ctx;
    int idx = nth_fitting_record(rc, cursor);
    if (idx < 0)
        return;

    restore_ctx.source = NULL;
    restore_ctx.preview = &rc->entry->rec[idx];
    draw_restore_top();

    clear_bottom();
    view_pick_record(rc->entry, (uint8_t)idx, rc->dest);
}

// --- restore ---------------------------------------------------------------------------

// Save the destination slot's current bytes before anything overwrites them. Returns
// false and leaves *detail set if it could not, in which case the restore is abandoned:
// the user asked for a copy, so proceeding without one is not what they agreed to.
// Delete the highlighted backup, confirmed on its own screen with the safe option selected.
// Rescans afterwards, because entries[] is built from the card and is stale the moment a file
// goes. The connection the file came from is untouched; only the file is removed. Returns
// whether it was removed, so the picker can keep focus on its former position afterwards.
static void draw_delete_confirm(uint8_t cursor, void *ctx)
{
    clear_bottom();
    view_delete_confirm((const backup_entry_t *)ctx, cursor);
}

static bool delete_backup(uint8_t index)
{
    backup_entry_t *e = &entries[index];

    while (1)
    {
        int pick = choose(DELETE_CHOICE_COUNT, draw_delete_confirm, e);
        if (pick != DELETE_DO_IT)
            return false;

        char path[BACKUP_PATH_LEN];
        backup_path_join(e->dir, e->name, path, sizeof(path));

        // Keep the name: `e` points into entries[], which the rescan below rebuilds.
        char name[BACKUP_NAME_LEN];
        snprintf(name, sizeof(name), "%s", e->name);

        bool ok = (unlink(path) == 0);

        clear_bottom();
        view_delete_result(ok, name, ok ? NULL : "the SD card refused the delete");
        if (!ok)
        {
            // The file still exists, so the delete confirmation remains the immediate,
            // viable parent of this failure screen.
            wait_for_key(KEY_B);
            continue;
        }

        wait_for_key(KEY_A);
        scan_backups();
        return true;
    }
}

// The gate in front of the one action that changes somebody's console.
//
// A confirmation you can hold A through is not a confirmation. This asks for a sequence the
// user has to read off the screen: four directions, then A. It is the convention GodMode9
// established and Cart-Flasher follows before flashing a cart, and it is here for the same
// reason -- the cost of an accidental press is somebody's Wi-Fi settings.
//
// A wrong press abandons the whole thing rather than rewinding one step. Rewinding turns it
// into a puzzle to be solved by trial; abandoning keeps it a thing you do deliberately or not
// at all.
static bool confirm_write(const wifi_slot_t *source, const wifi_slot_t *dest,
                          const char *undo)
{
    uint8_t seq[VIEW_COMBO_LEN];

    // No direction repeats back-to-back: two identical symbols side by side read as one, and
    // the sequence is entered from what is on screen.
    uint8_t last = 0xFF;
    for (uint8_t i = 0; i < VIEW_COMBO_LEN; i++)
    {
        uint8_t d;
        do { d = (uint8_t)(entropy_next() & 3); } while (d == last);
        seq[i] = d;
        last = d;
    }

    static const uint32_t dir_key[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };

    // A plain while loop with the index moved by hand. A for-loop with at++ in the header needed
    // at-- and at = (uint8_t)-1 to express "stay here" and "start over", which means relying on
    // unsigned wraparound in the one function that gates writing to flash.
    uint8_t at = 0;
    while (1)
    {
        clear_bottom();
        view_restore_confirm(source, dest, undo, seq, at);

        // Everything that could be pressed, so a wrong press is seen and acted on rather than
        // ignored until the right one arrives.
        uint32_t want = (at < VIEW_COMBO_LEN) ? dir_key[seq[at] & 3] : KEY_A;
        uint32_t got = wait_for_key(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT |
                                   KEY_A | KEY_B | KEY_X | KEY_Y);

        if (got & KEY_B)
            return false;

        if (got & want)
        {
            if (at >= VIEW_COMBO_LEN)
                return true;            // the whole sequence, then A
            at++;
            continue;
        }

        // Double-tap forgiveness, as Cart-Flasher and GodMode9 do it: pressing the symbol you
        // just entered again is ignored rather than counted as a mistake. A d-pad press is easy
        // to double-register, and losing a four-symbol sequence to a bounced button buys no
        // safety. It only suppresses the failure -- it never advances the combo.
        //
        // Safe only because no direction repeats back-to-back where seq is built. If that ever
        // changes this would swallow a real press, and the user would be stuck pressing the key
        // the screen says is correct.
        if (at > 0 && (got & dir_key[seq[at - 1] & 3]))
            continue;

        // A wrong press restarts the sequence rather than abandoning the restore. Mistyping four
        // symbols is not a decision to cancel, and this used to treat it as one.
        clear_bottom();
        view_combo_wrong();
        if (wait_for_key(KEY_A | KEY_B) & KEY_B)
            continue;

        at = 0;
    }
}

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

typedef enum {
    RESTORE_STEP_FILE,
    RESTORE_STEP_RECORD,
    RESTORE_STEP_CHECK,
    RESTORE_STEP_NOOP,
    RESTORE_STEP_UNDO,
    RESTORE_STEP_CONFIRM,
} restore_step_t;

// The destination is decided before this runs: the user entered that slot and chose
// "restore into it". So there is no destination step, and the family it accepts is known
// while the file picker is still being drawn.
static bool do_restore(uint8_t dest_index)
{
    memset(&restore_ctx, 0, sizeof(restore_ctx));
    restore_ctx.dest = &slots[dest_index];

    if (!have_sd)
    {
        message("No SD card.", "Put a card in and try again.", NULL, NULL);
        return false;
    }

    clear_top();
    view_restore_context(&restore_ctx);

    clear_bottom();
    blank();
    msg(VIEW_PLAIN, "Looking for backups ...");
    scan_backups();

    if (entry_count == 0)
    {
        clear_bottom();
        view_no_backups();
        wait_for_key(KEY_A | KEY_B);
        return false;
    }

    // Only offer what can actually land here. The destination was chosen before this flow
    // started, so the family it accepts is known while the picker is still being drawn --
    // which is the point of entering a slot first. The old flow could only refuse after the
    // user had picked, which is a worse thing to do than not offering it.
    const wifi_slot_t *dest = &slots[dest_index];
    char undo_name[BACKUP_NAME_LEN];
    const char *undo = NULL;
    restore_step_t step = RESTORE_STEP_FILE;
    uint8_t file_cursor = 0;
    uint8_t record_cursor = 0;
    uint8_t undo_cursor = UNDO_SAVE_COPY;
    int fi = -1;
    int ri = -1;
    uint8_t fitting_recs = 0;
    bool noop = false;
    bool undo_saved = false;

    while (1)
    {
        backup_entry_t *e;
        const wifi_slot_t *source;
        restore_step_t parent = (fitting_recs > 1) ? RESTORE_STEP_RECORD
                                                    : RESTORE_STEP_FILE;

        switch (step)
        {
            case RESTORE_STEP_FILE:
            {
                restore_ctx.source = NULL;
                restore_ctx.preview = NULL;
                restore_ctx.undo = NULL;
                restore_ctx.undo_settled = false;
                restore_ctx.noop_known = false;
                undo = NULL;
                undo_saved = false;

                uint8_t fitting = view_entry_count_fitting(entries, entry_count, dest);
                if (fitting == 0)
                {
                    draw_restore_top();
                    clear_bottom();
                    if (entry_count == 0)
                        view_no_backups();
                    else
                        view_none_fit(dest);
                    wait_for_key(KEY_A | KEY_B);
                    return false;
                }

                uint8_t at = 0;
                int pick = choose_with_from(fitting, draw_file_pick, (void *)dest,
                                            file_cursor, KEY_X, &at);
                if (pick == CHOOSE_BACK)
                    return false;
                if (pick == CHOOSE_EXTRA)
                {
                    // Returning to this picker must retain the highlighted file even when
                    // deletion is cancelled or refused.
                    file_cursor = at;
                    int di = nth_fitting(dest, at);
                    if (di >= 0 && delete_backup((uint8_t)di))
                    {
                        // The successor occupies the deleted file's position; at the last
                        // item, retain the preceding one. The new list can also be empty.
                        uint8_t remaining = view_entry_count_fitting(entries, entry_count, dest);
                        file_cursor = (remaining == 0) ? 0
                                                       : ((at < remaining) ? at
                                                                           : remaining - 1);
                    }
                    continue;
                }

                file_cursor = (uint8_t)pick;
                fi = nth_fitting(dest, file_cursor);
                if (fi < 0)
                    continue;

                e = &entries[fi];
                fitting_recs = 0;
                for (uint8_t i = 0; i < e->count; i++)
                    if (view_record_fits(&e->rec[i], dest))
                        fitting_recs++;

                if (fitting_recs == 0)
                    continue;

                record_cursor = 0;
                struct record_ctx rc = { .entry = e, .dest = dest };
                ri = nth_fitting_record(&rc, 0);
                step = (fitting_recs > 1) ? RESTORE_STEP_RECORD : RESTORE_STEP_CHECK;
                continue;
            }

            case RESTORE_STEP_RECORD:
            {
                if (fi < 0)
                {
                    step = RESTORE_STEP_FILE;
                    continue;
                }

                restore_ctx.source = NULL;
                restore_ctx.undo = NULL;
                restore_ctx.undo_settled = false;
                restore_ctx.noop_known = false;
                e = &entries[fi];
                struct record_ctx rc = { .entry = e, .dest = dest };
                int pick = choose_with_from(fitting_recs, draw_record_pick, &rc,
                                            record_cursor, 0, NULL);
                if (pick == CHOOSE_BACK)
                {
                    step = RESTORE_STEP_FILE;
                    continue;
                }

                record_cursor = (uint8_t)pick;
                ri = nth_fitting_record(&rc, record_cursor);
                if (ri < 0)
                {
                    step = RESTORE_STEP_FILE;
                    continue;
                }

                step = RESTORE_STEP_CHECK;
                continue;
            }

            case RESTORE_STEP_CHECK:
            {
                if (fi < 0 || ri < 0)
                {
                    step = RESTORE_STEP_FILE;
                    continue;
                }

                e = &entries[fi];
                source = &e->rec[ri];
                restore_ctx.file = e->name;
                restore_ctx.dir = e->dir;
                restore_ctx.source = source;
                restore_ctx.preview = NULL;
                restore_ctx.undo = NULL;
                restore_ctx.undo_settled = false;
                draw_restore_top();

                char path[BACKUP_PATH_LEN];
                backup_path_join(e->dir, e->name, path, sizeof(path));
                size_t got = 0;
                const char *read_error = read_backup_file(path, scan_buffer,
                                                           sizeof(scan_buffer), &got);

                backup_record_t recs[WIFI_MAX_SLOTS];
                uint8_t count = 0;
                if (read_error != NULL ||
                    backup_parse(scan_buffer, (uint32_t)got, recs, WIFI_MAX_SLOTS, &count) !=
                    BACKUP_OK || ri >= count ||
                    !record_matches_preview(e, (uint8_t)ri, &recs[ri]))
                {
                    message("That file changed.",
                            (read_error != NULL) ? read_error
                                                 : "It no longer matches the backup shown.",
                            "Nothing was written.", NULL);
                    scan_backups();
                    step = RESTORE_STEP_FILE;
                    continue;
                }

                const backup_record_t *rec = &recs[ri];
                const wifi_slot_pos_t *dest_pos = restore_find_slot(&layout,
                                                                     slots[dest_index].number);
                restore_err_t rerr = restore_check(rec, dest_pos);
                if (rerr != RESTORE_OK)
                {
                    message("Refusing to write.", restore_strerror(rerr),
                            "Nothing was written.", NULL);
                    step = parent;
                    continue;
                }

                noop = restore_is_noop(rec, records[dest_index]);
                restore_ctx.noop = noop;
                restore_ctx.noop_known = true;
                step = noop ? RESTORE_STEP_NOOP
                            : (!slots[dest_index].is_free ? RESTORE_STEP_UNDO
                                                           : RESTORE_STEP_CONFIRM);
                continue;
            }

            case RESTORE_STEP_NOOP:
            {
                draw_restore_top();
                clear_bottom();
                view_noop_notice(&slots[dest_index], DSIWIFI_DEBUG != 0);
                uint32_t answer = wait_for_key(DSIWIFI_DEBUG ? (KEY_X | KEY_B) : KEY_B);
                if (answer & KEY_B)
                {
                    step = parent;
                    continue;
                }

                restore_ctx.undo_settled = true;
                step = RESTORE_STEP_CONFIRM;
                continue;
            }

            case RESTORE_STEP_UNDO:
            {
                draw_restore_top();
                int pick = choose_with_from(UNDO_CHOICE_COUNT, draw_undo_prompt,
                                            &slots[dest_index], undo_cursor, 0, NULL);
                if (pick == CHOOSE_BACK)
                {
                    step = parent;
                    continue;
                }

                undo_cursor = (uint8_t)pick;
                undo = NULL;
                undo_saved = false;
                if (pick == UNDO_SAVE_COPY)
                {
                    clear_bottom();
                    blank();
                    msg(VIEW_PLAIN, "Saving a copy of Connection %u ...",
                        slots[dest_index].number);

                    const char *detail = NULL;
                    if (!write_undo(dest_index, undo_name, sizeof(undo_name), &detail))
                    {
                        message("Could not save the copy.", detail,
                                "Nothing was written to flash.", NULL);
                        continue;
                    }
                    undo = undo_name;
                    undo_saved = true;
                }

                restore_ctx.undo = undo;
                restore_ctx.undo_settled = true;
                step = RESTORE_STEP_CONFIRM;
                continue;
            }

            case RESTORE_STEP_CONFIRM:
            {
                source = &entries[fi].rec[ri];
                restore_ctx.undo = undo;
                restore_ctx.undo_settled = true;
                draw_restore_top();

                if (!confirm_write(source, &slots[dest_index], undo))
                {
                    // Once the copy exists, its prompt is no longer a viable parent: choosing
                    // it again would make another undo file. Return to the record instead.
                    if (undo_saved || noop)
                    {
                        undo = NULL;
                        undo_saved = false;
                        step = parent;
                    }
                    else
                    {
                        step = RESTORE_STEP_UNDO;
                    }
                    continue;
                }

                char path[BACKUP_PATH_LEN];
                backup_path_join(entries[fi].dir, entries[fi].name, path, sizeof(path));
                size_t got = 0;
                const char *read_error = read_backup_file(path, scan_buffer,
                                                           sizeof(scan_buffer), &got);

                backup_record_t recs[WIFI_MAX_SLOTS];
                uint8_t count = 0;
                if (read_error != NULL ||
                    backup_parse(scan_buffer, (uint32_t)got, recs, WIFI_MAX_SLOTS, &count) !=
                    BACKUP_OK || ri >= count ||
                    !record_matches_preview(&entries[fi], (uint8_t)ri, &recs[ri]))
                {
                    message("That file changed.",
                            (read_error != NULL) ? read_error
                                                 : "It no longer matches the backup shown.",
                            "Nothing was written.", NULL);
                    scan_backups();
                    step = RESTORE_STEP_FILE;
                    continue;
                }

                const backup_record_t *rec = &recs[ri];
                const wifi_slot_pos_t *dest_pos = restore_find_slot(&layout,
                                                                     slots[dest_index].number);
                restore_err_t rerr = restore_check(rec, dest_pos);
                if (rerr != RESTORE_OK)
                {
                    message("Refusing to write.", restore_strerror(rerr),
                            "Nothing was written.", NULL);
                    step = parent;
                    continue;
                }

                clear_bottom();
                blank();
                msg(VIEW_BAD, "Saving to Connection %u ...", slots[dest_index].number);

                memcpy(write_buffer, rec->data, rec->length);
                const char *detail = NULL;
                bool ok = (writeFirmware(dest_pos->offset, write_buffer, rec->length) == 0);
                if (!ok)
                    detail = "The console refused the change.";

                const wifi_slot_t *now = NULL;
                if (readFirmware(dest_pos->offset, records[dest_index], dest_pos->length) != 0)
                {
                    ok = false;
                    detail = "cannot read the connection back";
                }
                else
                {
                    wifi_slot_parse(dest_pos, records[dest_index], &slots[dest_index]);
                    now = &slots[dest_index];
                    if (ok && memcmp(records[dest_index], rec->data, rec->length) != 0)
                    {
                        ok = false;
                        detail = "the connection does not hold what was written";
                    }
                }

                clear_top();
                if (now != NULL)
                    view_summary(&layout, now, layout.count);
                else
                    view_idle_context(&layout);

                clear_bottom();
                view_restore_result(ok, slots[dest_index].number, now, undo, detail);
                wait_for_key(KEY_A);
                return true;
            }
        }
    }
}

// --- main -------------------------------------------------------------------------------

static bool do_backup(uint8_t index)
{
    // No free-slot guard here any more. view_conn_action_count() does not offer backup on a
    // free slot, so this is unreachable for one -- and restore_check()/backup_build still
    // refuse such a record if anything else ever calls in.
    if (!have_sd)
    {
        message("No SD card.", "Put a card in and try again.", NULL, NULL);
        return false;
    }

    if (!pick_name(backup_dir, 0))
    {
        message("No free file name.", "Every name this app can use",
                "is taken. Move some off the", "card first.");
        return false;
    }

    clear_top();
    view_backup_context(&slots[index], backup_dir, backup_name);

    clear_bottom();
    view_confirm(&slots[index], backup_dir, backup_name);
    if (wait_for_key(KEY_A | KEY_B) & KEY_B)
        return false;

    const char *detail = NULL;
    uint32_t bytes = write_backup(index, &detail);

    clear_bottom();
    view_result(bytes != 0, backup_dir, backup_name, bytes, 1, detail);
    wait_for_key(KEY_A);

    return true;
}

// --- the slot screen -------------------------------------------------------------------

static void draw_conn_screen(uint8_t cursor, void *ctx)
{
    const wifi_slot_t *s = (const wifi_slot_t *)ctx;

    clear_top();
    view_summary(&layout, s, layout.count);

    clear_bottom();
    view_conn_screen(s, cursor, DSIWIFI_DEBUG != 0);
}

// One slot, its details and what can be done to it. Returns when the user backs out.
static void enter_conn(uint8_t index)
{
    // A finished backup or restore returns to the list; anything short of finishing stays here.
    // That is what do_backup()/do_restore() report: true once the user has seen a terminal result
    // screen, false for no card, no free name, or no compatible backup. Restore's nested picker
    // states handle B internally so it returns only one viable screen at a time.
    uint8_t action_cursor = 0;
    while (1)
    {
        uint8_t n = view_conn_action_count(&slots[index]);
        int pick = choose_with_from(n, draw_conn_screen, &slots[index], action_cursor, 0, NULL);
        if (pick < 0)
            return;

        action_cursor = (uint8_t)pick;
        bool finished = false;
        switch (view_conn_action_at(&slots[index], (uint8_t)pick))
        {
            case CONN_ACTION_BACKUP:  finished = do_backup(index); break;
            case CONN_ACTION_RESTORE: finished = do_restore(index); break;
            default: break;
        }

        if (finished)
            return;
    }
}

int main(void)
{
    // Shows the "DSi only" screen and never returns on a DS, or a DSi booted in DS mode from
    // a flashcart. Mostly belt and braces: the ROM header's unitcode is 0x02, so a DS will not
    // boot this far. Why the gate stays (SD via DLDI, untested write-protect behaviour) is in
    // docs/ARCHITECTURE.md, "Platform".
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
            msg(VIEW_BAD, "Could not read Connection %u at 0x%05lX.",
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

        // Same as every other list here: stops at the ends, and only redraws when it moved.
        if ((down & KEY_UP) && cursor > 0)
        {
            cursor--;
            draw_list(cursor);
        }
        else if ((down & KEY_DOWN) && cursor + 1 < layout.count)
        {
            cursor++;
            draw_list(cursor);
        }
        else if (down & KEY_A)
        {
            enter_conn(cursor);
            draw_list(cursor);
        }
        else if (DSIWIFI_DEBUG && (down & KEY_SELECT))
        {
            clear_bottom();
            view_about(&layout);
            wait_for_key(KEY_A | KEY_B);
            draw_list(cursor);
        }
    }

    return 0;
}
