// Host build of the console's decoder, screens, backup writer and restore logic, so
// milestones can be checked without hardware. It compiles arm9/source/wifi_slots.c,
// slot_list_view.c, backup_file.c and restore.c unmodified -- the same objects the ARM9
// links -- against a flash dump.
//
//   gcc -std=gnu17 -Wall -Iarm9/source -o host_slotlist tools/host_slotlist.c
//       arm9/source/wifi_slots.c arm9/source/slot_list_view.c arm9/source/backup_file.c
//       arm9/source/restore.c arm9/source/fb_render.c
//
// tools/crosscheck.py runs that build for you.
//
//   ./host_slotlist --fields  dump.bin              one line per slot, for crosscheck.py
//   ./host_slotlist --list    dump.bin              the list screen, both panes
//   ./host_slotlist --screens dump.bin              every screen the app draws
//
// The app draws on two screens, so --list and --screens tag every line with two columns:
// the pane (`T` top, `B` bottom) and the attribute (` ` plain, `d` dim, `g` good, `B` bad,
// `>` cursor). Both end with `overlong lines: N` and exit non-zero if N is not 0 -- a line
// past VIEW_COLS is cut by the renderer, which loses the end of an SSID or a file name
// without saying so.
//   ./host_slotlist --backup  dump.bin out [slots]  write a .dswifi (default: in-use).
//                                                   The app writes one record per file;
//                                                   this can write several, which is how
//                                                   the multi-record parse and restore
//                                                   paths still get tested.
//   ./host_slotlist --render                        renderer self-checks; non-zero if bad
//   ./host_slotlist --verify  file.dswifi           parse a .dswifi; non-zero if bad
//   ./host_slotlist --restore dump.bin f.dswifi N S out.bin
//                                                   apply record N to slot S, write the
//                                                   whole modified flash to out.bin
//
// CAUTION: --fields, --list and --screens print the SSID, and --backup writes a file
// with the passphrase in the clear. Run them on fixtures; for a real dump use
// tools/crosscheck.py, which compares without echoing.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backup_file.h"
#include "fb_render.h"
#include "restore.h"
#include "slot_list_view.h"
#include "wifi_slots.h"

// VIEW_COLS columns between the bars, built at run time so it cannot drift from VIEW_COLS
// the way a literal did when that changed from 31 to 51. Printed under the two-column
// pane-and-attribute tag so a rendered line and the ruler start in the same place.
static const char *ruler_text(void)
{
    static char buf[VIEW_COLS + 3];
    buf[0] = '|';
    for (int i = 0; i < VIEW_COLS; i++)
    {
        int col = i + 1;
        buf[1 + i] = (col % 10 == 0) ? (char)('0' + (col / 10) % 10) : '-';
    }
    buf[1 + VIEW_COLS] = '|';
    buf[2 + VIEW_COLS] = '\0';
    return buf;
}
#define RULER ruler_text()

// The app draws on two screens. Tagging each line with its pane keeps them apart here:
// interleaved, a width check would be measuring the wrong screen's rows.
//
// A line longer than VIEW_COLS is not a cosmetic problem. The console cuts it, so the
// end of an SSID or a file name is silently lost. Count them and report the total, so
// tools/crosscheck.py can fail on it instead of someone having to notice.
static int overlong_lines = 0;

// One letter per attribute, so the colouring is visible in a diff and in the test output
// without inventing terminal escapes here. The console maps these to colours; this maps
// them to a column, which is what a text harness can check.
static char attr_tag(view_attr_t a)
{
    switch (a)
    {
        case VIEW_DIM:    return 'd';
        case VIEW_GOOD:   return 'g';
        case VIEW_BAD:    return 'B';
        case VIEW_ACCENT: return '*';
        case VIEW_CURSOR: return '>';
        case VIEW_DEBUG:  return 'D';
        default:          return ' ';
    }
}

// Rows used by each pane on the screen being drawn. The renderer drops a row past the
// bottom of the screen, so a screen that grows too tall loses its tail silently -- the
// width check cannot see that, because every individual line is still legal.
static int pane_rows[VIEW_PANES];
static int overtall_screens = 0;

static void tagged_sink(view_pane_t pane, view_attr_t attr, const char *line)
{
    size_t n = strlen(line);

    // The footer is pinned, so it is not part of the flow and must not count toward the row
    // budget the overtall check reads. Tagged 'F' so a pinned line is visible as one here.
    if (pane != VIEW_FOOTER)
        pane_rows[pane]++;

    // The arrow glyphs are CP437's codepoints 0x18-0x1B, which a terminal would eat. They are
    // substituted for display only: one byte in still means one glyph out, so the width count
    // below measures exactly what the console draws.
    //
    // The order is CP437's -- up, down, RIGHT, left -- not the reading order of "^v<>". That
    // transposition is easy to make and silent, because both strings are four characters and
    // the widths still pass; only a screen showing the wrong direction reveals it.
    static char shown[256];
    for (size_t i = 0; i <= n && i < sizeof(shown) - 1; i++)
    {
        unsigned char c = (unsigned char)line[i];
        shown[i] = (c >= FB_UP && c <= FB_LEFT) ? "^v><"[c - FB_UP] : line[i];
    }
    shown[sizeof(shown) - 1] = '\0';
    line = shown;

    const char *tag = (pane == VIEW_TOP) ? "T" : (pane == VIEW_FOOTER) ? "F" : "B";
    printf("%s%c|%.*s%s\n", tag, attr_tag(attr), VIEW_COLS, line,
           (n > (size_t)VIEW_COLS) ? "   <-- OVERLONG, CUT ON THE CONSOLE" : "");

    if (n > (size_t)VIEW_COLS)
        overlong_lines++;
}

// A row of coloured pieces. Printed as the composed row plus a second line marking each piece's
// attribute under it, so per-symbol colouring is visible in the test output and a mistake in the
// progress logic shows up here rather than only on a console.
static void tagged_segs_sink(view_pane_t pane, const view_seg_t *segs, uint8_t count)
{
    char row[256], marks[256];
    size_t at = 0;

    for (uint8_t i = 0; i < count && at < sizeof(row) - 1; i++)
    {
        size_t len = strlen(segs[i].text);
        for (size_t j = 0; j < len && at < sizeof(row) - 1; j++, at++)
        {
            row[at] = segs[i].text[j];
            marks[at] = attr_tag(segs[i].attr);
        }
    }
    row[at] = marks[at] = '\0';

    tagged_sink(pane, VIEW_PLAIN, row);
    printf("  %s   <- piece colours\n", marks);
}

// Also the screen boundary: every screen is drawn between two of these, so this is where
// the row counts get judged and reset.
static void ruler(void)
{
    for (int p = 0; p < 2; p++)
    {
        if (pane_rows[p] > FB_ROWS)
        {
            printf("  %s pane used %d rows, screen holds %d   <-- OVERTALL, TAIL LOST\n",
                   (p == VIEW_TOP) ? "top" : "bottom", pane_rows[p], FB_ROWS);
            overtall_screens++;
        }
        pane_rows[p] = 0;
    }

    printf("  %s\n", RULER);
}


static uint8_t *slurp(const char *path, uint32_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0)
    {
        fprintf(stderr, "%s is empty\n", path);
        fclose(f);
        return NULL;
    }

    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len)
    {
        fprintf(stderr, "short read on %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *len_out = (uint32_t)len;
    return buf;
}

static void print_fields(const wifi_layout_t *layout, const wifi_slot_t *slots,
                         uint32_t flash_len)
{
    printf("layout base=0x%05lX implied=%lu size=%lu type=0x%02X dsi=%d count=%u "
           "region=0x%05lX\n",
           (unsigned long)layout->base, (unsigned long)layout->implied_size,
           (unsigned long)flash_len, layout->console_type, layout->is_dsi ? 1 : 0,
           layout->count, (unsigned long)layout->region_start);

    for (uint8_t i = 0; i < layout->count; i++)
    {
        const wifi_slot_t *s = &slots[i];
        printf("slot %u family=%s offset=0x%05lX len=%u free=%d status=0x%02X "
               "crc=%d crc2=%s sec_byte=0x%02X label=%s ssid_len=%u "
               "ssid_printable=%d ssid=%s\n",
               s->number, (s->family == WIFI_FAMILY_TWL) ? "TWL" : "NTR",
               (unsigned long)s->offset, s->length, s->is_free ? 1 : 0, s->status,
               s->crc_ok ? 1 : 0, s->has_crc2 ? (s->crc2_ok ? "1" : "0") : "-",
               wifi_slot_security_byte(s), wifi_security_label(s), s->ssid_len,
               s->ssid_printable ? 1 : 0, s->ssid);

        // The network fields go on their own line so the SSID stays last on its one --
        // crosscheck.py recovers the SSID by splitting the raw line, because a name may
        // contain spaces.
        printf("net %u ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns1=%u.%u.%u.%u dns2=%u.%u.%u.%u "
               "subnet=%u mtu=%u cfg=0x%02X\n",
               s->number,
               s->ip[0], s->ip[1], s->ip[2], s->ip[3],
               s->gateway[0], s->gateway[1], s->gateway[2], s->gateway[3],
               s->dns1[0], s->dns1[1], s->dns1[2], s->dns1[3],
               s->dns2[0], s->dns2[1], s->dns2[2], s->dns2[3],
               s->subnet_prefix, s->mtu, s->config_bits);
    }
}

// Both screens as the app draws them for one cursor position: the detail pane on top,
// the list on the bottom.
static void print_list(const wifi_layout_t *layout, const wifi_slot_t *slots,
                       uint8_t cursor)
{
    ruler();
    view_summary(layout, &slots[cursor], layout->count);
    view_list_title();
    for (uint8_t i = 0; i < layout->count; i++)
        view_conn_row(&slots[i], i == cursor);
    view_keys(true);   // draw the Dev-build hints too, so widths cover them
    ruler();
}

// Every screen in turn, so a line that would wrap on the console is visible here.
static void print_screens(const wifi_layout_t *layout, const wifi_slot_t *slots)
{
    printf("\n--- list, cursor on slot 1 ---\n");
    print_list(layout, slots, 0);

    // The confirm screen for whichever slot is in use, which is the widest case.
    uint8_t first_used = 0;
    for (uint8_t i = 0; i < layout->count; i++)
    {
        if (!slots[i].is_free)
        {
            first_used = i;
            break;
        }
    }

    // The detail pane for a slot that is in use, and for a free one -- they take
    // different branches and only one of them prints an SSID.
    printf("\n--- list, cursor on a slot in use ---\n");
    print_list(layout, slots, first_used);

    uint8_t first_free = 0;
    bool any_free = false;
    for (uint8_t i = 0; i < layout->count; i++)
    {
        if (slots[i].is_free)
        {
            first_free = i;
            any_free = true;
            break;
        }
    }
    if (any_free)
    {
        printf("\n--- list, cursor on a free slot ---\n");
        print_list(layout, slots, first_free);
    }

    // The slot screen, which is what A now opens: both actions on a slot in use, restore
    // alone on a free one.
    printf("\n--- about screen (SELECT from the list) ---\n");
    ruler();
    view_about(layout);
    ruler();

    printf("\n--- slot screen, slot in use, cursor on Back up ---\n");
    ruler();
    view_conn_screen(&slots[first_used], 0);
    ruler();

    printf("\n--- slot screen, slot in use, cursor on Restore ---\n");
    ruler();
    view_conn_screen(&slots[first_used], 1);
    ruler();

    if (any_free)
    {
        printf("\n--- slot screen, free slot: restore is the only option ---\n");
        ruler();
        view_conn_screen(&slots[first_free], 0);
        ruler();
    }

    printf("\n--- top pane: backup context ---\n");
    ruler();
    view_backup_context(&slots[first_used], "DSIWIFI/0009BF010203",
                        "wifi-20260728-015530.dswifi");
    ruler();

    printf("\n--- confirm ---\n");
    ruler();
    view_confirm(&slots[first_used], "DSIWIFI/0009BF010203",
                 "wifi-20260728-015530.dswifi");
    ruler();

    printf("\n--- result, written ---\n");
    ruler();
    view_result(true, "DSIWIFI/0009BF010203", "wifi-20260728-015530.dswifi", 269, 1,
                NULL);
    ruler();

    printf("\n--- result, failed ---\n");
    ruler();
    view_result(false, "DSIWIFI/0009BF010203", "wifi-20260728-015530.dswifi", 0, 0,
                "the SD card write was short");
    ruler();

    // The restore screens, built from a synthetic picker list so every one of them can
    // be checked for width without a card in a console.
    static backup_entry_t picked[3];
    uint8_t in_use = 0;

    snprintf(picked[0].dir, sizeof(picked[0].dir), "DSIWIFI/0009BF010203");
    snprintf(picked[0].name, sizeof(picked[0].name), "wifi-20260728-015530.dswifi");
    picked[0].ok = true;
    for (uint8_t i = 0; i < layout->count; i++)
    {
        if (!slots[i].is_free && in_use < WIFI_MAX_SLOTS)
            picked[0].rec[in_use++] = slots[i];
    }
    picked[0].count = in_use;

    // A second, different folder, so the picker's "from <console>" line is exercised.
    // Both MACs here are made up -- never put a real console's MAC in source.
    snprintf(picked[1].dir, sizeof(picked[1].dir), "DSIWIFI/0009BF040506");
    snprintf(picked[1].name, sizeof(picked[1].name), "undo4-20260801-113145.dswifi");
    picked[1].ok = true;
    picked[1].count = in_use ? 1 : 0;
    if (in_use)
        picked[1].rec[0] = picked[0].rec[0];

    snprintf(picked[2].dir, sizeof(picked[2].dir), "DSIWIFI/0009BF010203");
    snprintf(picked[2].name, sizeof(picked[2].name), "wifi001.dswifi");
    picked[2].ok = false;
    picked[2].problem = "junk after the last record";

    // The pickers now take the destination and draw only what can land in it, so both
    // families get rendered: an NTR slot sees the NTR backups, a TWL slot sees the TWL ones.
    const wifi_slot_t *ntr_dest = &slots[0];
    const wifi_slot_t *twl_dest = NULL;
    for (uint8_t i = 0; i < layout->count; i++)
        if (slots[i].family == WIFI_FAMILY_TWL) { twl_dest = &slots[i]; break; }

    printf("\n--- restore into an NTR slot: only NTR backups are offered ---\n");
    ruler();
    view_pick_file(picked, 3, 0, 0, ntr_dest);
    ruler();

    if (twl_dest != NULL)
    {
        printf("\n--- restore into a TWL slot: the NTR backups are simply absent ---\n");
        ruler();
        if (view_entry_count_fitting(picked, 3, twl_dest) == 0)
            view_none_fit(twl_dest);
        else
            view_pick_file(picked, 3, 0, 0, twl_dest);
        ruler();
    }

    printf("\n--- restore: no backups at all ---\n");
    ruler();
    view_no_backups();
    ruler();

    if (in_use == 0)
        return;

    const wifi_slot_t *source = &picked[0].rec[0];

    printf("\n--- restore: pick a record ---\n");
    ruler();
    view_pick_record(&picked[0], 0, ntr_dest);
    ruler();

    // Both variants of the no-op notice: a Release build only offers B, a Dev build also
    // offers the forced write. Rendering both here is why view_noop_notice takes a flag
    // instead of reading DSIWIFI_DEBUG itself.
    printf("\n--- restore: nothing to do, release build ---\n");
    ruler();
    view_noop_notice(&slots[0], false);
    ruler();

    printf("\n--- restore: nothing to do, debug build offers the write ---\n");
    ruler();
    view_noop_notice(&slots[0], true);
    ruler();

    // Deleting a backup, both cursor positions, so the widths and rows checks cover the
    // screen with the destructive option selected as well as the safe one.
    printf("\n--- delete a backup: safe option selected ---\n");
    ruler();
    view_delete_confirm(&picked[0], DELETE_KEEP);
    ruler();

    printf("\n--- delete a backup: the destructive option selected ---\n");
    ruler();
    view_delete_confirm(&picked[0], DELETE_DO_IT);
    ruler();

    printf("\n--- delete: done, and refused ---\n");
    ruler();
    view_delete_result(true, picked[0].name, NULL);
    ruler();
    view_delete_result(false, picked[0].name, "the SD card refused the delete");
    ruler();

    printf("\n--- restore: destination in use, offer a copy ---\n");
    ruler();
    view_undo_prompt(&slots[0], UNDO_SAVE_COPY);
    ruler();

    printf("\n--- restore: the same prompt with the overwrite option selected ---\n");
    ruler();
    view_undo_prompt(&slots[0], UNDO_OVERWRITE);
    ruler();

    // The three reachable combinations. A no-op never carries an undo copy: the app
    // does not offer one when the destination already holds these bytes.

    printf("\n--- restore: confirm, overwriting, copy kept ---\n");
    ruler();
    // A fixed sequence, so the screen renders identically every run and the width and height
    // checks are comparing the same thing. main.c generates a real one from its entropy.
    static const uint8_t combo[VIEW_COMBO_LEN] = { 0, 2, 1, 3 };
    view_restore_confirm(source, &slots[0], "undo1-20260728-015530.dswifi", combo, 0);
    ruler();

    printf("\n--- restore: confirm, overwriting, no copy kept ---\n");
    ruler();
    view_restore_confirm(source, &slots[0], NULL, combo, 0);
    ruler();

    printf("\n--- restore: confirm, two of the four entered ---\n");
    ruler();
    view_restore_confirm(source, &slots[0], NULL, combo, 2);
    ruler();

    printf("\n--- restore: confirm, waiting on the final A ---\n");
    ruler();
    view_restore_confirm(source, &slots[0], NULL, combo, VIEW_COMBO_LEN);
    ruler();

    printf("\n--- restore: a press that was not the next symbol ---\n");
    ruler();
    view_combo_wrong();
    ruler();

    printf("\n--- restore: done ---\n");
    ruler();
    view_restore_result(true, slots[0].number, &slots[0], NULL, NULL);
    ruler();

    printf("\n--- restore: failed ---\n");
    ruler();
    view_restore_result(false, slots[0].number, &slots[0], NULL,
                        "writeFirmware refused the write");
    ruler();

    // The top pane as the restore flow fills it in. Every stage, because a NULL field is
    // meant to print nothing rather than a placeholder, and that is easy to get wrong.
    view_restore_ctx_t rc;

    printf("\n--- top pane: restore, nothing picked yet ---\n");
    ruler();
    memset(&rc, 0, sizeof(rc));
    view_restore_context(&rc);
    ruler();

    printf("\n--- top pane: restore, file picked ---\n");
    ruler();
    rc.file = picked[0].name;
    rc.dir = picked[0].dir;
    rc.source = source;
    view_restore_context(&rc);
    ruler();

    printf("\n--- top pane: restore, destination picked ---\n");
    ruler();
    rc.dest = &slots[0];
    rc.noop = true;
    rc.noop_known = true;
    view_restore_context(&rc);
    ruler();

    printf("\n--- top pane: restore, no-op, no copy needed ---\n");
    ruler();
    rc.undo_settled = true;
    view_restore_context(&rc);
    ruler();

    printf("\n--- top pane: restore, overwriting with a copy kept ---\n");
    ruler();
    rc.noop = false;
    rc.undo = "undo1-20260728-015530.dswifi";
    view_restore_context(&rc);
    ruler();

    printf("\n--- top pane: restore, overwriting with no copy kept ---\n");
    ruler();
    rc.undo = NULL;
    view_restore_context(&rc);
    ruler();

    printf("\n--- top pane: idle ---\n");
    ruler();
    view_idle_context(layout);
    ruler();
}

static int write_backup(const wifi_layout_t *layout, const wifi_slot_t *slots,
                        const uint8_t *flash, const bool *selected, const char *out_path)
{
    backup_record_t recs[WIFI_MAX_SLOTS];
    uint8_t n = 0;

    for (uint8_t i = 0; i < layout->count; i++)
    {
        if (!selected[i])
            continue;
        recs[n].slot = slots[i].number;
        recs[n].family = (uint8_t)slots[i].family;
        recs[n].length = slots[i].length;
        recs[n].data = flash + slots[i].offset;
        n++;
    }

    if (n == 0)
    {
        fprintf(stderr, "no slots selected and none in use -- nothing to back up\n");
        return 1;
    }

    static uint8_t buf[BACKUP_MAX_LEN];
    backup_err_t err;
    uint32_t len = backup_build(buf, sizeof(buf), recs, n, &err);
    if (len == 0)
    {
        fprintf(stderr, "backup_build: %s\n", backup_strerror(err));
        return 1;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f || fwrite(buf, 1, len, f) != len)
    {
        fprintf(stderr, "cannot write %s\n", out_path);
        if (f)
            fclose(f);
        return 1;
    }
    fclose(f);

    printf("wrote %s: %lu bytes, %u record(s)\n", out_path, (unsigned long)len, n);
    return 0;
}

// Run the app's own parser over a backup file. Milestone 3 will restore from whatever
// this accepts, so what it rejects matters as much as what it reads.
static int verify_backup(const char *path)
{
    uint32_t len = 0;
    uint8_t *buf = slurp(path, &len);
    if (!buf)
        return 1;

    backup_record_t recs[WIFI_MAX_SLOTS];
    uint8_t count = 0;
    backup_err_t err = backup_parse(buf, len, recs, WIFI_MAX_SLOTS, &count);

    if (err != BACKUP_OK)
    {
        printf("reject %s: %s (parsed %u record(s))\n", path, backup_strerror(err),
               count);
        free(buf);
        return 1;
    }

    printf("accept %s: %lu bytes, %u record(s)\n", path, (unsigned long)len, count);
    for (uint8_t i = 0; i < count; i++)
    {
        printf("  record %u: slot %u family=%s len=%u\n", i + 1, recs[i].slot,
               (recs[i].family == WIFI_FAMILY_TWL) ? "TWL" : "NTR", recs[i].length);
    }

    free(buf);
    return 0;
}

// Apply a restore to a flash image in memory and write the result out whole, so the
// modified image can be decoded with tools/decode_wifi.py and diffed against the
// original. The console runs the same restore_check() and then hands the same bytes to
// writeFirmware.
static int restore_into(const char *dump, const char *backup, int record_index,
                        int dest_slot, const char *out_path)
{
    uint32_t flash_len = 0;
    uint8_t *flash = slurp(dump, &flash_len);
    if (!flash)
        return 1;

    wifi_layout_t layout;
    wifi_layout_err_t lerr = wifi_layout_derive(flash, flash_len, &layout);
    if (lerr != WIFI_LAYOUT_OK)
    {
        fprintf(stderr, "bad header: %s\n", wifi_layout_strerror(lerr));
        free(flash);
        return 1;
    }

    uint32_t blen = 0;
    uint8_t *bdata = slurp(backup, &blen);
    if (!bdata)
    {
        free(flash);
        return 1;
    }

    backup_record_t recs[WIFI_MAX_SLOTS];
    uint8_t count = 0;
    backup_err_t berr = backup_parse(bdata, blen, recs, WIFI_MAX_SLOTS, &count);
    if (berr != BACKUP_OK)
    {
        fprintf(stderr, "backup_parse: %s\n", backup_strerror(berr));
        free(bdata);
        free(flash);
        return 1;
    }

    if (record_index < 1 || record_index > count)
    {
        fprintf(stderr, "record %d is not in a file with %u record(s)\n", record_index,
                count);
        free(bdata);
        free(flash);
        return 1;
    }

    const backup_record_t *rec = &recs[record_index - 1];
    const wifi_slot_pos_t *dest = restore_find_slot(&layout, (uint8_t)dest_slot);

    if (dest != NULL)
    {
        printf("%s\n", restore_is_noop(rec, flash + dest->offset)
                       ? "no-op: the slot already holds these bytes"
                       : "the slot currently holds something else");
    }

    restore_err_t rerr = restore_apply(flash, flash_len, &layout, rec,
                                       (uint8_t)dest_slot);
    if (rerr != RESTORE_OK)
    {
        printf("refused: %s\n", restore_strerror(rerr));
        free(bdata);
        free(flash);
        return 1;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f || fwrite(flash, 1, flash_len, f) != flash_len)
    {
        fprintf(stderr, "cannot write %s\n", out_path);
        if (f)
            fclose(f);
        free(bdata);
        free(flash);
        return 1;
    }
    fclose(f);

    printf("restored record %d (slot %u) into slot %d, wrote %s\n", record_index,
           rec->slot, dest_slot, out_path);

    free(bdata);
    free(flash);
    return 0;
}

// Print the filename the app would choose, so the naming rules -- including the
// fallback for a console whose clock was never set -- are testable without a console.
//   --name  <undo slot> <year> <mon> <day> <hour> <min> <sec>
// --- the renderer ------------------------------------------------------------------
//
// fb_render.c draws into a plain uint16_t buffer, which is the whole reason it is portable:
// the console hands it VRAM and this hands it malloc'd memory, and what lands is the same.
// These check the properties that a console cannot be asked about cheaply and that a
// screenshot would not settle either.

static int fb_fail = 0;

static void fb_check(bool ok, const char *what)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fb_fail++;
}

// Every pixel the renderer writes must have bit 15 set. On the DS that bit is the opaque
// flag in 16-bit bitmap mode, not part of the colour, so a pixel without it is invisible
// and a renderer that is otherwise perfect shows a blank screen. It is the single most
// likely way this fails on hardware, and this is the only place it can be caught before
// then: the buffer would look correct to any test that only compared colours.
static bool all_pixels_opaque(const uint16_t *fb)
{
    for (int i = 0; i < FB_PIXELS; i++)
    {
        if ((fb[i] & 0x8000u) == 0)
            return false;
    }
    return true;
}

static int render_checks(void)
{
    uint16_t *fb = malloc(FB_PIXELS * sizeof(uint16_t));
    uint16_t *ref = malloc(FB_PIXELS * sizeof(uint16_t));
    if (!fb || !ref)
    {
        fprintf(stderr, "out of memory\n");
        free(fb);
        free(ref);
        return 1;
    }

    printf("renderer\n");

    printf("  geometry: %d cols x %d rows, glyph %dx%d\n",
           FB_COLS, FB_ROWS, FONT_GLYPH_W, FONT_GLYPH_H);
    fb_check(FB_COLS * FONT_GLYPH_W <= FB_WIDTH, "columns fit the screen width");
    fb_check(FB_ROWS * FONT_GLYPH_H == FB_HEIGHT, "rows fill the screen height exactly");

    // Bit 15, after a clear and after text, including the tail fill.
    fb_clear(fb, FB_BG);
    fb_check(all_pixels_opaque(fb), "every pixel opaque after fb_clear");

    fb_row_text(fb, 0, FB_TEXT, FB_SELECT, "opaque check");
    fb_check(all_pixels_opaque(fb), "every pixel opaque after fb_row_text");

    // Every printable glyph must actually draw something, or the font conversion dropped
    // rows. Space is the one that legitimately draws nothing.
    fb_clear(fb, FB_BG);
    int blank = 0;
    for (int c = FONT_FIRST; c <= FONT_LAST; c++)
    {
        char s[2] = { (char)c, '\0' };
        fb_clear(fb, FB_BG);
        fb_text(fb, 0, 0, FB_TEXT, FB_BG, s);

        bool lit = false;
        for (int y = 0; y < FONT_GLYPH_H && !lit; y++)
            for (int x = 0; x < FONT_GLYPH_W; x++)
                if (fb[y * FB_WIDTH + x] == FB_TEXT) { lit = true; break; }

        if (!lit)
            blank++;
    }
    fb_check(blank == 1, "exactly one blank glyph in the font (space)");

    // An out-of-range byte draws the fallback rather than reading past the table. A file
    // name with an accent in it is the realistic way this arrives, so it must be handled
    // in the renderer and not left to callers.
    fb_clear(fb, FB_BG);
    fb_text(fb, 0, 0, FB_TEXT, FB_BG, "\xC3");
    fb_clear(ref, FB_BG);
    fb_text(ref, 0, 0, FB_TEXT, FB_BG, "?");
    fb_check(memcmp(fb, ref, FB_PIXELS * sizeof(uint16_t)) == 0,
             "byte 0xC3 draws the fallback glyph");

    fb_clear(fb, FB_BG);
    fb_text(fb, 0, 0, FB_TEXT, FB_BG, "\x05");
    fb_check(memcmp(fb, ref, FB_PIXELS * sizeof(uint16_t)) == 0,
             "byte 0x05 draws the fallback glyph");

    fb_clear(fb, FB_BG);
    fb_text(fb, 0, 0, FB_TEXT, FB_BG, "\x7F");
    fb_check(memcmp(fb, ref, FB_PIXELS * sizeof(uint16_t)) == 0,
             "byte 0x7F draws the fallback glyph");

    // 0x18-0x1B are the arrows, so they must NOT be the fallback, and each must differ from
    // the others -- four identical triangles would make the confirmation sequence unreadable
    // while still passing every other check here.
    bool arrows_distinct = true, arrows_not_fallback = true;
    static uint16_t arrow[4][FB_PIXELS];
    int ink_w[4] = { 0 }, ink_h[4] = { 0 };
    for (int a = 0; a < 4; a++)
    {
        fb_clear(arrow[a], FB_BG);
        char one[2] = { (char)(FB_UP + a), '\0' };
        fb_text(arrow[a], 0, 0, FB_TEXT, FB_BG, one);
        if (memcmp(arrow[a], ref, FB_PIXELS * sizeof(uint16_t)) == 0)
            arrows_not_fallback = false;

        // The ink's bounding box inside the glyph cell, measured off the pixels rather than
        // read back out of the table, so this tests what gets drawn.
        int x0 = FONT_GLYPH_W, x1 = -1, y0 = FONT_GLYPH_H, y1 = -1;
        for (int y = 0; y < FONT_GLYPH_H; y++)
            for (int x = 0; x < FONT_GLYPH_W; x++)
                if (arrow[a][y * FB_WIDTH + x] == FB_TEXT)
                {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        ink_w[a] = x1 - x0 + 1;
        ink_h[a] = y1 - y0 + 1;
    }
    for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++)
            if (memcmp(arrow[a], arrow[b], FB_PIXELS * sizeof(uint16_t)) == 0)
                arrows_distinct = false;

    fb_check(arrows_not_fallback, "bytes 0x18-0x1B draw arrows, not the fallback");
    fb_check(arrows_distinct, "the four arrows are all different from each other");

    // Distinct is not the same as legible. Four glyphs can differ by a pixel and still read as
    // one symbol at 5x8, which is what the first version did: every arrow was a triangle with
    // a shaft, so every one had a full-width middle row and they came out as four variations
    // on a plus sign.
    //
    // What separates them now is orientation, so that is what is asserted: an up or down arrow
    // is taller than it is wide, a left or right arrow wider than tall. Widening a head back
    // to the full 5 columns squares the vertical pair's bounding box and fails here.
    fb_check(ink_h[0] > ink_w[0] && ink_h[1] > ink_w[1],
             "up and down are taller than they are wide");
    fb_check(ink_w[2] > ink_h[2] && ink_w[3] > ink_h[3],
             "right and left are wider than they are tall");

    // Over-long text clips at the right edge instead of wrapping. A wrapped row would turn
    // one slot into two, which is the reason VIEW_COLS exists at all.
    fb_clear(fb, FB_BG);
    char longline[FB_COLS + 20];
    memset(longline, 'M', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    fb_row_text(fb, 0, FB_TEXT, FB_BG, longline);

    bool row1_clean = true;
    for (int i = FONT_GLYPH_H * FB_WIDTH; i < 2 * FONT_GLYPH_H * FB_WIDTH; i++)
        if (fb[i] != FB_BG) { row1_clean = false; break; }
    fb_check(row1_clean, "over-long text clips, it does not wrap");

    // fb_row_text repaints the whole row, so a shorter line cannot leave the tail of a
    // longer one behind. This is the framebuffer failure a text console never has.
    fb_clear(fb, FB_BG);
    fb_row_text(fb, 3, FB_TEXT, FB_BG, longline);
    fb_row_text(fb, 3, FB_TEXT, FB_BG, "short");

    bool tail_clean = true;
    for (int y = 3 * FONT_GLYPH_H; y < 4 * FONT_GLYPH_H; y++)
        for (int x = 5 * FONT_GLYPH_W; x < FB_WIDTH; x++)
            if (fb[y * FB_WIDTH + x] != FB_BG) { tail_clean = false; }
    fb_check(tail_clean, "a shorter line erases the longer one's tail");

    // Including the spare pixel column: 51 cells of 5 pixels is 255, one short of 256, and
    // a stripe of the previous screen surviving there would be a permanent artefact.
    bool edge_clean = true;
    for (int y = 3 * FONT_GLYPH_H; y < 4 * FONT_GLYPH_H; y++)
        if (fb[y * FB_WIDTH + (FB_WIDTH - 1)] != FB_BG) edge_clean = false;
    fb_check(edge_clean, "the spare right-edge pixel column is painted");

    // Rows outside the screen are ignored rather than written.
    fb_clear(fb, FB_BG);
    memcpy(ref, fb, FB_PIXELS * sizeof(uint16_t));
    fb_row_text(fb, -1, FB_TEXT, FB_DANGER, "off the top");
    fb_row_text(fb, FB_ROWS, FB_TEXT, FB_DANGER, "off the bottom");
    fb_row_text(fb, FB_ROWS + 50, FB_TEXT, FB_DANGER, "far off the bottom");
    fb_check(memcmp(fb, ref, FB_PIXELS * sizeof(uint16_t)) == 0,
             "rows outside the screen are ignored");

    free(fb);
    free(ref);

    printf(fb_fail ? "FAIL: renderer\n" : "PASS: renderer\n");
    return fb_fail ? 1 : 0;
}

//   --index <undo slot> <index>
static int print_name(int argc, char **argv)
{
    char name[BACKUP_NAME_LEN];

    if (strcmp(argv[1], "--index") == 0)
    {
        if (argc != 4)
            return 2;
        backup_name_indexed(name, sizeof(name), (uint8_t)atoi(argv[2]),
                            (uint16_t)atoi(argv[3]));
        printf("%s\n", name);
        return 0;
    }

    if (argc != 9)
        return 2;

    backup_stamp_t t = {
        .year = (uint16_t)atoi(argv[3]),
        .month = (uint8_t)atoi(argv[4]),
        .day = (uint8_t)atoi(argv[5]),
        .hour = (uint8_t)atoi(argv[6]),
        .minute = (uint8_t)atoi(argv[7]),
        .second = (uint8_t)atoi(argv[8]),
    };

    if (!backup_stamp_plausible(&t))
    {
        printf("implausible\n");
        return 0;
    }

    backup_name_stamped(name, sizeof(name), (uint8_t)atoi(argv[2]), &t);
    printf("%s\n", name);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "";
    const char *path = (argc > 2) ? argv[2] : NULL;

    if (strcmp(mode, "--name") == 0 || strcmp(mode, "--index") == 0)
        return print_name(argc, argv);

    if (strcmp(mode, "--restore") == 0)
    {
        if (argc != 7)
        {
            fprintf(stderr, "usage: host_slotlist --restore <dump.bin> <file.dswifi> "
                            "<record#> <dest slot> <out.bin>\n");
            return 2;
        }
        return restore_into(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]), argv[6]);
    }

    if (strcmp(mode, "--render") == 0)
        return render_checks();

    if (strcmp(mode, "--verify") == 0)
    {
        if (!path)
        {
            fprintf(stderr, "usage: host_slotlist --verify <file.dswifi>\n");
            return 2;
        }
        return verify_backup(path);
    }

    int is_fields = (strcmp(mode, "--fields") == 0);
    int is_list = (strcmp(mode, "--list") == 0);
    int is_screens = (strcmp(mode, "--screens") == 0);
    int is_backup = (strcmp(mode, "--backup") == 0);

    if (!path || !(is_fields || is_list || is_screens || is_backup) ||
        (is_backup && argc < 4))
    {
        fprintf(stderr,
                "usage: host_slotlist --fields|--list|--screens <dump.bin>\n"
                "       host_slotlist --backup <dump.bin> <out.dswifi> [slot ...]\n");
        return 2;
    }

    uint32_t flash_len = 0;
    uint8_t *flash = slurp(path, &flash_len);
    if (!flash)
        return 1;

    if (flash_len < WIFI_HEADER_LEN)
    {
        fprintf(stderr, "%s is too small to hold a flash header\n", path);
        free(flash);
        return 1;
    }

    // The console passes 0 here because nothing reports the chip size; on a file we
    // know it, so the bounds check is the tighter one.
    wifi_layout_t layout;
    wifi_layout_err_t err = wifi_layout_derive(flash, flash_len, &layout);
    if (err != WIFI_LAYOUT_OK)
    {
        fprintf(stderr, "bad header: %s\n", wifi_layout_strerror(err));
        free(flash);
        return 1;
    }

    wifi_slot_t slots[WIFI_MAX_SLOTS];
    bool selected[WIFI_MAX_SLOTS] = { false };

    for (uint8_t i = 0; i < layout.count; i++)
    {
        const wifi_slot_pos_t *pos = &layout.slots[i];
        if (pos->offset + pos->length > flash_len)
        {
            fprintf(stderr, "slot %u runs past the end of the file\n", pos->number);
            free(flash);
            return 1;
        }
        wifi_slot_parse(pos, flash + pos->offset, &slots[i]);
        selected[i] = !slots[i].is_free;
    }

    // --backup takes an explicit slot list after the output path; with none, it takes
    // every slot that is in use.
    if (is_backup && argc > 4)
    {
        for (uint8_t i = 0; i < layout.count; i++)
            selected[i] = false;

        for (int a = 4; a < argc; a++)
        {
            int want = atoi(argv[a]);
            bool found = false;
            for (uint8_t i = 0; i < layout.count; i++)
            {
                if (slots[i].number == want)
                {
                    selected[i] = true;
                    found = true;
                }
            }
            if (!found)
            {
                fprintf(stderr, "slot %d is not in this flash's layout\n", want);
                free(flash);
                return 1;
            }
        }
    }

    int rc = 0;
    if (is_fields)
    {
        print_fields(&layout, slots, flash_len);
    }
    else if (is_list || is_screens)
    {
        // Only the screen modes tag their panes. --fields is a data format, not a
        // rendering, and prefixing it would break crosscheck.py's parser.
        view_set_sink(tagged_sink);
    view_set_segs_sink(tagged_segs_sink);

        if (is_list)
            print_list(&layout, slots, 0);
        else
            print_screens(&layout, slots);

        // The line crosscheck.py reads. Zero is the only passing value: a longer line is
        // cut on the console, which loses the end of an SSID or a file name silently.
        ruler();    // judge the last screen's row counts
        printf("\ncolumns: %d\nrows: %d\noverlong lines: %d\novertall screens: %d\n",
               VIEW_COLS, FB_ROWS, overlong_lines, overtall_screens);
        if (overlong_lines != 0 || overtall_screens != 0)
            rc = 1;
    }
    else
    {
        rc = write_backup(&layout, slots, flash, selected, argv[3]);
    }

    free(flash);
    return rc;
}
