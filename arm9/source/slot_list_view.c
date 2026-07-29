// SPDX-License-Identifier: CC0-1.0
//
// See slot_list_view.h.

#include <stdio.h>
#include <string.h>

#include "slot_list_view.h"

// Lines are composed in a buffer with room to spare and cut to VIEW_COLS on the way
// out, so a long SSID or an undocumented security label can never wrap a row into the
// next one and turn one slot into two.
#define VIEW_SCRATCH 96

typedef char view_line_t[VIEW_SCRATCH];

// "> 1 NTR " is 8 columns, then the quotes and the non-ASCII marker.
#define SSID_ROOM (VIEW_COLS - 8 - 3)

// The banner's version, supplied by the build from git (see the root Makefile). Hand
// maintaining it did not work: this file said "v0.4" while another screen in the same build
// said "v0.5". The fallback is for a build with no git available, and says so rather than
// inventing a number.
//
// Its length varies, which matters more than it looks: the banner right-aligns it, so a
// long branch name makes that row wider. tools/crosscheck.py builds with a deliberately
// long value so the widths phase measures the worst case rather than today's commit.
#ifndef DSIWIFI_VERSION_STR
#define DSIWIFI_VERSION_STR "unknown"
#endif
#define VIEW_VERSION DSIWIFI_VERSION_STR


// The widest thing describe() can produce is its furniture plus a whole SSID, and the SSID
// is bounded by the record's field, not by how many columns a screen has. Stating that here
// means a change to WIFI_SSID_MAX fails the build rather than silently truncating a name on
// a screen. Deriving these from VIEW_COLS instead would be the wrong invariant: it is not
// the one that binds, and reasoning from it once produced a bug report that was not real.
#define DESC_FURNITURE 8
_Static_assert(VIEW_SCRATCH > VIEW_COLS,
               "a composed line plus its NUL must fit the scratch buffer");

static void default_sink(view_pane_t pane, view_attr_t attr, const char *line)
{
    // A host build that never calls view_set_sink() gets one undifferentiated stream,
    // which is what a quick `--list` wants. tools/host_slotlist.c installs its own.
    (void)pane;
    (void)attr;
    printf("%.*s\n", VIEW_COLS, line);
}

static view_sink_t sink = default_sink;

void view_set_sink(view_sink_t fn)
{
    sink = (fn != NULL) ? fn : default_sink;
}

static void emit(view_pane_t pane, view_attr_t attr, const char *line)
{
    sink(pane, attr, line);
}

// The bottom pane is where all the pre-existing screens draw, so it gets the short name.
// The plain forms are the common case; the _a forms are for the few lines that carry a
// verdict worth colouring.
static void put(const char *line)
{
    emit(VIEW_BOTTOM, VIEW_PLAIN, line);
}

static void put_a(view_attr_t attr, const char *line)
{
    emit(VIEW_BOTTOM, attr, line);
}

static void top(const char *line)
{
    emit(VIEW_TOP, VIEW_PLAIN, line);
}

static void top_a(view_attr_t attr, const char *line)
{
    emit(VIEW_TOP, attr, line);
}

// Separates the banner from whatever a pane is about: VIEW_COLS dashes, composed rather
// than written out, because a literal that has to match VIEW_COLS is a literal that drifts
// from it the first time VIEW_COLS changes. It changed from 31 to 51 already.
static void rule(view_pane_t pane)
{
    view_line_t line;
    memset(line, '-', VIEW_COLS);
    line[VIEW_COLS] = '\0';
    emit(pane, VIEW_DIM, line);
}

// A record's checksums decide how its rows read, everywhere they appear.
static view_attr_t crc_attr(const wifi_slot_t *s)
{
    bool ok = s->crc_ok && (!s->has_crc2 || s->crc2_ok);
    return ok ? VIEW_GOOD : VIEW_BAD;
}

static const char *family_name(wifi_family_t f)
{
    return (f == WIFI_FAMILY_TWL) ? "TWL" : "NTR";
}

// "crc ok", "crc BAD", or for a TWL record both checksums: "crc ok/BAD".
static void crc_text(const wifi_slot_t *s, char *buf, size_t len)
{
    if (s->has_crc2)
    {
        snprintf(buf, len, "crc %s/%s", s->crc_ok ? "ok" : "BAD",
                 s->crc2_ok ? "ok" : "BAD");
    }
    else
    {
        snprintf(buf, len, "crc %s", s->crc_ok ? "ok" : "BAD");
    }
}

// `src` cut to `room` columns, with a '~' where it was cut. Used for SSIDs and for file
// names, which are the two things here long enough to need it.
static void cut_text(const char *src, int room, char *buf, size_t len)
{
    size_t have = strlen(src);

    if (room > 0 && (int)have > room && (size_t)room < len)
    {
        memcpy(buf, src, (size_t)room - 1);
        buf[room - 1] = '~';
        buf[room] = '\0';
    }
    else
    {
        snprintf(buf, len, "%s", src);
    }
}

// The SSID cut to `room` columns, with a '~' where it was cut.
static void ssid_text(const wifi_slot_t *s, int room, char *buf, size_t len)
{
    cut_text(s->ssid, room, buf, len);
}

// --- the top pane ------------------------------------------------------------------

#define VIEW_APP_NAME "DSi WiFi Slots"

static void top_banner(void)
{
    view_line_t line;
    char version[VIEW_COLS + 1];

    // The version is right-aligned when it fits and cut when it does not. It comes from git
    // now, so its length is not ours to choose: a branch with a long name makes it long, and
    // a banner that overflows would fail the widths phase on a build that is otherwise fine.
    // Cutting shows a '~' rather than silently losing the end.
    int room = VIEW_COLS - (int)strlen(VIEW_APP_NAME) - 1;
    cut_text(VIEW_VERSION, room, version, sizeof(version));

    int pad = VIEW_COLS - (int)strlen(VIEW_APP_NAME) - (int)strlen(version);
    if (pad < 1)
        pad = 1;

    snprintf(line, sizeof(line), "%s%*s%s", VIEW_APP_NAME, pad, "", version);
    top(line);
}

static void top_layout(const wifi_layout_t *layout)
{
    view_line_t line;

    snprintf(line, sizeof(line), "base 0x%05lX  type 0x%02X %s",
             (unsigned long)layout->base, layout->console_type,
             layout->is_dsi ? "DSi" : "DS");
    top(line);

    snprintf(line, sizeof(line), "wifi 0x%05lX-0x%05lX  %u slots",
             (unsigned long)layout->region_start, (unsigned long)(layout->base - 1),
             layout->count);
    top(line);
}

// A label and a value that may be too long for one row: the value goes on its own
// indented rows and is wrapped rather than cut, because this pane exists precisely to
// show what the list had to truncate. A 32-byte SSID needs two rows at this indent.
#define TOP_INDENT 2
#define TOP_WRAP   (VIEW_COLS - TOP_INDENT)

static void top_wrapped(const char *text)
{
    view_line_t line;
    size_t len = strlen(text);
    size_t pos = 0;

    if (len == 0)
    {
        top("  (empty)");
        return;
    }

    while (pos < len)
    {
        size_t take = len - pos;
        if (take > TOP_WRAP)
            take = TOP_WRAP;

        snprintf(line, sizeof(line), "%*s%.*s", TOP_INDENT, "", (int)take, text + pos);
        top(line);
        pos += take;
    }
}

// Two label/value pairs on one row. The right column starts here; the left gets the rest.
#define DETAIL_LABEL 9
#define DETAIL_COL2  26

// A cell holds a label plus a value, and it is sized for the widest value any caller can
// produce rather than for the column it is padded into. Those are different numbers: an
// unverified security label prints as "WPA2-PSK (TKIP)?  [05]", which is 22 characters and
// does not fit in DETAIL_COL2 at all.
//
// A cell wider than DETAIL_COL2 pushes the right column along, which is a layout problem
// the widths check reports. Truncating it here instead would silently drop the end of a
// value, which nothing reports. Prefer the visible failure.
#define DETAIL_CELL 48
typedef char detail_cell_t[DETAIL_CELL];
_Static_assert(DETAIL_CELL > DETAIL_LABEL + WIFI_SSID_MAX,
               "a detail cell must hold its label plus the widest value put in one");
_Static_assert(VIEW_SCRATCH >= DETAIL_COL2 + DETAIL_CELL,
               "detail_row composes a padded left cell plus a right cell");

// The precisions here are proofs for the compiler, not layout limits: they bound each %s
// so -Wformat-truncation can see the result fits, which it cannot do through a const char *.
// Both are far above any value a caller actually produces, so neither ever truncates in
// practice, and if one ever did the widths check would report the over-long row.
static void detail_cell(detail_cell_t out, const char *label, const char *value)
{
    snprintf(out, sizeof(detail_cell_t), "%-*.*s%.*s",
             DETAIL_LABEL, DETAIL_CELL - 1, label,
             DETAIL_CELL - 1 - DETAIL_LABEL, value);
}

// `right` may be NULL for a row that only uses the left column, and `left` may be blank for
// one that only uses the right.
static void detail_row(const char *left, const char *right)
{
    view_line_t line;

    if (right == NULL)
        snprintf(line, sizeof(line), "%.*s", DETAIL_CELL - 1, left);
    else
        snprintf(line, sizeof(line), "%-*.*s%.*s",
                 DETAIL_COL2, DETAIL_CELL - 1, left, DETAIL_CELL - 1, right);

    top(line);
}

// A dotted quad, or what its being zero means. Which question to ask is the caller's,
// because the address and the DNS servers are independent settings and answering for one
// from the other would state something the record does not.
static void addr_text(const uint8_t a[4], const char *if_zero, char *buf, size_t len)
{
    if (wifi_addr_is_zero(a))
        snprintf(buf, len, "%s", if_zero);
    else
        snprintf(buf, len, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

void view_detail(const wifi_layout_t *layout, const wifi_slot_t *s)
{
    view_line_t line;

    top_banner();
    top_layout(layout);
    rule(VIEW_TOP);

    // The offset is the one fact that ties a slot on screen to a range in a flash dump,
    // which is how every hardware finding in this project was checked.
    snprintf(line, sizeof(line), "Slot %u  %s        0x%05lX  %u bytes",
             s->number, family_name(s->family),
             (unsigned long)s->offset, s->length);
    top(line);

    if (s->is_free)
    {
        // Free slots are not blank -- they carry a valid checksum and non-zero fields.
        // Saying so here is cheaper than explaining it once someone dumps the flash.
        snprintf(line, sizeof(line), "Free      E7=%02X, no network", s->status);
        top_a(VIEW_DIM, line);

        // The raw byte without its label. wifi_security_label() reads "open" for the 0x00
        // a free slot happens to carry, and naming a security mode for a slot holding no
        // network would be inventing a fact the record does not state.
        snprintf(line, sizeof(line), "Security  [%02X] not meaningful",
                 wifi_slot_security_byte(s));
        top(line);

        char crc[16];
        crc_text(s, crc, sizeof(crc));
        snprintf(line, sizeof(line), "Checksum  %s", crc);
        top_a(crc_attr(s), line);
        return;
    }

    // At 51 columns the SSID and its length usually fit one row together, where at 31 the
    // name alone needed two.
    //
    // Compose it and measure, rather than predicting the width from the SSID length. The
    // prediction here was `ssid_len + DETAIL_LABEL + 12`, and the 12 quietly assumed the
    // note was empty: a 20-byte SSID with a non-ASCII character in it produced a 60-column
    // row that got cut at "not all p", losing the one warning that case exists to give.
    // Measuring cannot drift from the format string, because it is the format string.
    const char *note = s->ssid_printable ? "" : ", not all printable";

    snprintf(line, sizeof(line), "%-*s%s  (%u bytes%s)", DETAIL_LABEL, "SSID", s->ssid,
             s->ssid_len, note);

    if (strlen(line) <= VIEW_COLS)
    {
        top(line);
    }
    else
    {
        top("SSID");
        top_wrapped(s->ssid);
        snprintf(line, sizeof(line), "  %u bytes%s", s->ssid_len, note);
        top(line);
    }
    top("");

    // Identity on the left, network on the right. The right column is longer, so the left
    // one runs out first and the remaining rows carry only a right cell.
    detail_cell_t l[3];
    detail_cell_t r[6];
    char v[32];

    snprintf(v, sizeof(v), "0x%02X in use", s->status);
    detail_cell(l[0], "Status", v);

    snprintf(v, sizeof(v), "%s  [%02X]", wifi_security_label(s), wifi_slot_security_byte(s));
    detail_cell(l[1], "Security", v);

    snprintf(v, sizeof(v), "0x%02X", s->config_bits);
    detail_cell(l[2], "Config", v);

    // "auto (DHCP)" only for the address. A zero DNS is not the same statement -- it means
    // the console takes whatever the network offers -- and calling both "auto" would read
    // as one setting when they are two. The reference DSi runs a DHCP address with a
    // manually set DNS 1, which is the case that proves they are independent.
    addr_text(s->ip, "auto (DHCP)", v, sizeof(v));
    detail_cell(r[0], "IP", v);

    addr_text(s->gateway, "auto", v, sizeof(v));
    detail_cell(r[1], "Gateway", v);

    addr_text(s->dns1, "(unset)", v, sizeof(v));
    detail_cell(r[2], "DNS 1", v);

    addr_text(s->dns2, "(unset)", v, sizeof(v));
    detail_cell(r[3], "DNS 2", v);

    // Printed as they read. Subnet 0 and MTU 0 are both what a working connection stores on
    // the reference console, and MTU 0 is the value System Settings later rewrites to 1400,
    // so dressing either up as invalid would contradict a hardware finding.
    snprintf(v, sizeof(v), "/%u", s->subnet_prefix);
    detail_cell(r[4], "Subnet", v);

    snprintf(v, sizeof(v), "%u", s->mtu);
    detail_cell(r[5], "MTU", v);

    for (int i = 0; i < 6; i++)
        detail_row((i < 3) ? l[i] : "", r[i]);

    // Its own row: the sink colours a whole line, so anything sharing this one would be
    // dragged green or red with it.
    top("");
    char crc[16];
    crc_text(s, crc, sizeof(crc));
    snprintf(line, sizeof(line), "%-*s%s", DETAIL_LABEL, "Checksum", crc);
    top_a(crc_attr(s), line);
}

void view_idle_context(const wifi_layout_t *layout)
{
    top_banner();
    top_layout(layout);
    rule(VIEW_TOP);
}

// "1 NTR \"name\"" on one row, for the context panes, where the label eats 6 columns.
#define CTX_LABEL 6

static void top_slot_line(const char *label, const wifi_slot_t *s)
{
    view_line_t line;
    char ssid[WIFI_SSID_MAX + 2];

    if (s->is_free)
    {
        snprintf(line, sizeof(line), "%-*s%u %s (free)", CTX_LABEL, label,
                 s->number, family_name(s->family));
    }
    else
    {
        // CTX_LABEL, the slot number, the family and the two quotes.
        ssid_text(s, VIEW_COLS - CTX_LABEL - 8, ssid, sizeof(ssid));
        snprintf(line, sizeof(line), "%-*s%u %s \"%s\"", CTX_LABEL, label,
                 s->number, family_name(s->family), ssid);
    }

    top(line);
}

// The record's security and checksums as a continuation of the line above, indented to
// the label column. The detail pane spells these out as their own labelled fields; here
// they belong to the "rec" line and should read that way.
static void top_record_line(const wifi_slot_t *s)
{
    view_line_t line;
    char crc[16];

    crc_text(s, crc, sizeof(crc));
    snprintf(line, sizeof(line), "%*s%-10s [%02X] %s", CTX_LABEL, "",
             wifi_security_label(s), wifi_slot_security_byte(s), crc);
    top(line);
}

static void top_text_line(const char *label, const char *text)
{
    view_line_t line;

    // Sized to exactly what fits beside the label, so the compiler can see that composing
    // one into a row cannot overflow -- the same reason view_desc_t exists below.
    char cut[VIEW_COLS - CTX_LABEL + 1];

    cut_text(text, VIEW_COLS - CTX_LABEL, cut, sizeof(cut));
    snprintf(line, sizeof(line), "%-*s%s", CTX_LABEL, label, cut);
    top(line);
}

void view_backup_context(const wifi_slot_t *slot, const char *dir, const char *filename)
{
    top_banner();
    rule(VIEW_TOP);
    top("BACK UP");
    top("");

    top_slot_line("slot", slot);
    if (!slot->is_free)
        top_record_line(slot);

    top("");
    if (dir != NULL)
        top_text_line("to", dir);
    if (filename != NULL)
        top_text_line("file", filename);

    top("");
    rule(VIEW_TOP);
    top_a(VIEW_BAD, "! The file stores your WiFi");
    top_a(VIEW_BAD, "  password in the clear.");
}

void view_restore_context(const view_restore_ctx_t *ctx)
{
    top_banner();
    rule(VIEW_TOP);
    top("RESTORE");
    top("");

    // Nothing is invented: a field the user has not reached yet simply is not drawn, so
    // the pane never shows a destination before one has been chosen.
    if (ctx->file != NULL)
        top_text_line("file", ctx->file);
    if (ctx->dir != NULL)
        top_text_line("from", ctx->dir);

    if (ctx->source != NULL)
    {
        top_slot_line("rec", ctx->source);
        top_record_line(ctx->source);
    }

    if (ctx->dest != NULL)
    {
        top("");
        top_slot_line("into", ctx->dest);
    }

    if (ctx->undo != NULL)
    {
        top_text_line("undo", ctx->undo);
    }
    else if (ctx->undo_settled)
    {
        // Three different reasons for "no copy", and which one it is changes whether the
        // user should be worried. The confirm screen says the same thing; this pane keeps
        // saying it while they read the rest.
        if (ctx->dest != NULL && ctx->dest->is_free)
            top("undo  not needed, slot free");
        else if (ctx->noop_known && ctx->noop)
            top("undo  not needed, bytes match");
        else
            top("undo  declined, no copy kept");
    }

    top("");
    rule(VIEW_TOP);

    if (ctx->noop_known && ctx->noop)
        top_a(VIEW_DIM, "Nothing will be programmed.");
    else if (ctx->dest != NULL)
        top_a(VIEW_BAD, "! Writes the console flash.");
}

// --- the bottom pane ---------------------------------------------------------------

void view_list_title(void)
{
    // The layout summary that used to sit here moved to the top pane, which freed four
    // rows: six slots at two rows each plus a key legend is a tight fit in 24.
    put("Slots");
}

void view_slot(const wifi_slot_t *s, bool cursor)
{
    view_line_t line;
    char crc[16];
    char ssid[WIFI_SSID_MAX + 2];

    crc_text(s, crc, sizeof(crc));

    // The ">" stays even though the cursor row is now drawn as a bar. The host harness has
    // no colour, so without it tools/host_slotlist.c --list could not show which row is
    // selected, and that output is how the screens get read during testing.
    const char *mark = cursor ? ">" : " ";

    // A free slot is dim; a bad checksum on the detail row is loud; the cursor outranks
    // both, because where you are matters more than what is there.
    view_attr_t head = cursor ? VIEW_CURSOR : (s->is_free ? VIEW_DIM : VIEW_PLAIN);
    view_attr_t body = cursor ? VIEW_CURSOR : (s->is_free ? VIEW_DIM : crc_attr(s));

    if (s->is_free)
    {
        snprintf(line, sizeof(line), "%s %u %s (free)",
                 mark, s->number, family_name(s->family));
        put_a(head, line);

        // Free slots say E7=FF outright instead of borrowing the [NN] notation, which
        // means the security byte everywhere else.
        snprintf(line, sizeof(line), "    E7=%02X unused     %s", s->status, crc);
        put_a(body, line);
        return;
    }

    // SSID_ROOM is 40 at 51 columns, so a full 32-byte SSID now fits here whole. It did
    // not at 31, where this had 20 and a real name was routinely cut.
    ssid_text(s, SSID_ROOM, ssid, sizeof(ssid));

    snprintf(line, sizeof(line), "%s %u %s \"%s\"%s",
             mark, s->number, family_name(s->family), ssid,
             s->ssid_printable ? "" : "*");
    put_a(head, line);

    snprintf(line, sizeof(line), "    %-10s [%02X] %s",
             wifi_security_label(s), wifi_slot_security_byte(s), crc);
    put_a(body, line);
}

void view_keys(void)
{
    put("");
    put_a(VIEW_DIM, "UP/DN move    A open slot    START exit");
    put("");
    put_a(VIEW_DIM, "The top screen shows the slot under the cursor.");
}

void view_confirm(const wifi_slot_t *slot, const char *dir, const char *filename)
{
    view_line_t line;
    char ssid[WIFI_SSID_MAX + 2];

    snprintf(line, sizeof(line), "Back up slot %u?", slot->number);
    put(line);
    put("");

    // "  N FAM " is 8 columns here, leaving room for the quotes.
    ssid_text(slot, VIEW_COLS - 8 - 2, ssid, sizeof(ssid));
    snprintf(line, sizeof(line), "  %u %s \"%s\"",
             slot->number, family_name(slot->family), ssid);
    put(line);

    put("");
    put("! The file stores your WiFi");
    put("  password in the clear.");
    put("");
    put(dir);
    put(filename);
    put("");
    put("A write     B cancel");
}

void view_result(bool ok, const char *dir, const char *filename, uint32_t bytes,
                 uint8_t records, const char *detail)
{
    view_line_t line;

    put_a(ok ? VIEW_GOOD : VIEW_BAD, ok ? "Backup written." : "Backup FAILED.");
    put("");

    if (ok)
    {
        put(dir);
        put(filename);
        snprintf(line, sizeof(line), "%lu bytes, %u record%s",
                 (unsigned long)bytes, records, (records == 1) ? "" : "s");
        put(line);
        put("");
        put("Read back and verified.");
    }
    else if (detail != NULL)
    {
        put(detail);
    }

    put("");
    put("B: back");
}

// --- restore ---------------------------------------------------------------------

// A slot rendered as text: "1 NTR \"name\"" is at most 9 columns of furniture plus the
// SSID room the caller allows, which is always less than a row. Sized so the compiler
// can see that composing one into a row cannot overflow.
#define VIEW_DESC 48
typedef char view_desc_t[VIEW_DESC];

// The bound that actually applies. `room` never binds, because it is always at least
// WIFI_SSID_MAX + DESC_FURNITURE for every caller, so the widest output is the furniture
// plus a full 32-byte SSID. Asserted rather than argued about: this was reported as an
// overflow once, on arithmetic that used `room` as the limit, and it was not one.
_Static_assert(VIEW_DESC > DESC_FURNITURE + WIFI_SSID_MAX,
               "describe() output plus its NUL must fit view_desc_t");

// "1 NTR \"name\"" or "1 NTR (free)", fitted into `room` columns *in total*.
//
// `room` used to mean the SSID's share alone, and every caller passed VIEW_COLS minus
// its own prefix -- that is, the room for the whole description. So each one came out
// DESC_FURNITURE columns too wide, and with a 32-byte SSID the restore screens lost
// their right edge: "onto  1 NTR \"ABCDEFGHIJKLMNOPQR" with the closing quote cut off.
// Both SSIDs in the test inputs are 3 and 6 bytes, so it never showed. The width phase
// of tools/crosscheck.py exists because of this.
//
// Taking the total and subtracting here means a caller only has to know its own prefix,
// which is the one thing it actually knows.
static void describe(const wifi_slot_t *s, int room, char *out, size_t out_len)
{
    if (s->is_free)
    {
        snprintf(out, out_len, "%u %s (free)", s->number, family_name(s->family));
        return;
    }

    char ssid[WIFI_SSID_MAX + 2];
    ssid_text(s, room - DESC_FURNITURE, ssid, sizeof(ssid));
    snprintf(out, out_len, "%u %s \"%s\"", s->number, family_name(s->family), ssid);
}

void view_pick_file(const backup_entry_t *entries, uint8_t count, uint8_t cursor,
                    uint8_t top)
{
    view_line_t line;

    snprintf(line, sizeof(line), "Restore: pick a file (%u)", count);
    put(line);
    put("");

    for (uint8_t i = top; i < count && i < (uint8_t)(top + VIEW_PICK_ROWS); i++)
    {
        snprintf(line, sizeof(line), "%s %s", (i == cursor) ? ">" : " ",
                 entries[i].name);
        put(line);

        // Only the highlighted file expands. Everything the detail shows was decoded
        // when the card was scanned, so moving the cursor never reopens a file.
        if (i != cursor)
            continue;

        snprintf(line, sizeof(line), "    from %s", entries[i].dir);
        put(line);

        if (!entries[i].ok)
        {
            // The reason goes on its own row: sharing one with a label cut the longer
            // parse errors off mid-word.
            put("    unreadable:");
            snprintf(line, sizeof(line), "    %s",
                     entries[i].problem ? entries[i].problem : "bad file");
            put(line);
            continue;
        }

        for (uint8_t r = 0; r < entries[i].count; r++)
        {
            view_desc_t what;
            describe(&entries[i].rec[r], VIEW_COLS - 4, what, sizeof(what));
            snprintf(line, sizeof(line), "    %s", what);
            put(line);
        }
    }

    put("");
    if (count > VIEW_PICK_ROWS)
        put("UP/DN scroll  A pick  B back");
    else
        put("UP/DN move  A pick  B back");
}

void view_pick_record(const backup_entry_t *entry, uint8_t cursor)
{
    view_line_t line;

    put("Restore: pick a record");
    put(entry->name);
    put("");

    for (uint8_t i = 0; i < entry->count; i++)
    {
        const wifi_slot_t *s = &entry->rec[i];
        view_desc_t what;

        describe(s, VIEW_COLS - 2, what, sizeof(what));
        snprintf(line, sizeof(line), "%s %s", (i == cursor) ? ">" : " ", what);
        put(line);

        char crc[16];
        crc_text(s, crc, sizeof(crc));
        snprintf(line, sizeof(line), "    %-10s [%02X] %s",
                 wifi_security_label(s), wifi_slot_security_byte(s), crc);
        put(line);
    }

    put("");
    put("A pick   B back");
}

void view_undo_prompt(const wifi_slot_t *dest)
{
    view_line_t line;
    view_desc_t what;

    snprintf(line, sizeof(line), "Slot %u is in use:", dest->number);
    put(line);

    describe(dest, VIEW_COLS - 2, what, sizeof(what));
    snprintf(line, sizeof(line), "  %s", what);
    put(line);

    put("");
    put("Restoring overwrites it.");
    put("Save a copy to the SD first?");
    put("");
    put("A  yes, back it up");
    put("X  no, overwrite it");
    put("B  cancel");
}

void view_restore_confirm(const wifi_slot_t *source, const wifi_slot_t *dest,
                          const char *undo_name)
{
    view_line_t line;
    view_desc_t what;

    snprintf(line, sizeof(line), "Restore into slot %u", dest->number);
    put(line);
    put("");

    describe(source, VIEW_COLS - 6, what, sizeof(what));
    snprintf(line, sizeof(line), "from  %s", what);
    put(line);

    describe(dest, VIEW_COLS - 6, what, sizeof(what));
    snprintf(line, sizeof(line), "onto  %s", what);
    put(line);
    put("");

    if (undo_name != NULL)
    {
        put("A copy of it is saved to:");
        put(undo_name);
        put("");
    }
    else if (!dest->is_free)
    {
        put_a(VIEW_BAD, "! Slot contents will be lost, with no copy kept.");
        put("");
    }

    put_a(VIEW_BAD, "This writes the console flash.");

    put("A write     B cancel");
}

void view_restore_result(bool ok, uint8_t dest_number, const wifi_slot_t *now,
                         const char *undo_name, const char *detail)
{
    view_line_t line;

    put_a(ok ? VIEW_GOOD : VIEW_BAD, ok ? "Restore done." : "Restore FAILED.");
    put("");

    if (!ok && detail != NULL)
    {
        put(detail);
        put("");
    }

    // Worked or not, the slot was re-read afterwards, so state what it actually holds
    // rather than hedge about what was probably left alone. `now` is NULL only when even
    // the read back failed -- the one case that genuinely cannot be stated.
    if (now != NULL)
    {
        view_desc_t what;
        char crc[16];

        describe(now, VIEW_COLS - 6, what, sizeof(what));
        snprintf(line, sizeof(line), "now   %s", what);
        put(line);

        crc_text(now, crc, sizeof(crc));
        snprintf(line, sizeof(line), "      read back, %s", crc);
        put(line);
    }
    else
    {
        snprintf(line, sizeof(line), "Slot %u could not be read back,", dest_number);
        put(line);
        put("so what it holds is unknown.");
    }

    if (undo_name != NULL)
    {
        put("");
        put("Previous contents saved as:");
        put(undo_name);
    }

    put("");
    put("B: back");
}

void view_no_backups(void)
{
    put("");
    put("No backup files found.");
    put("");
    put("Looked in DSIWIFI/ for any");
    put("folder holding .dswifi files.");
    put("");
    put("Take a backup first, or copy");
    put("one onto the card.");
}

// --- the slot screen ------------------------------------------------------------------

uint8_t view_slot_action_count(const wifi_slot_t *slot)
{
    // A free slot has nothing to copy out of it, so backup is not offered at all. That
    // replaces the old message telling the user to go and stand somewhere else.
    return slot->is_free ? 1 : SLOT_ACTION_COUNT;
}

slot_action_t view_slot_action_at(const wifi_slot_t *slot, uint8_t index)
{
    if (slot->is_free)
        return SLOT_ACTION_RESTORE;

    return (index == 0) ? SLOT_ACTION_BACKUP : SLOT_ACTION_RESTORE;
}

static const char *action_words(slot_action_t a)
{
    return (a == SLOT_ACTION_BACKUP) ? "Back up this slot to the SD card"
                                     : "Restore a backup into this slot";
}

void view_slot_screen(const wifi_slot_t *s, uint8_t cursor)
{
    view_line_t line;
    char crc[16];

    if (s->is_free)
    {
        snprintf(line, sizeof(line), "Slot %u  %s   empty",
                 s->number, family_name(s->family));
        put_a(VIEW_DIM, line);
        put("");
        put("Nothing is saved in this slot.");
    }
    else
    {
        char ssid[WIFI_SSID_MAX + 2];

        // The whole SSID fits: this row spends only the slot number and family before it.
        ssid_text(s, VIEW_COLS - 16, ssid, sizeof(ssid));
        snprintf(line, sizeof(line), "Slot %u  %s   \"%s\"",
                 s->number, family_name(s->family), ssid);
        put(line);

        crc_text(s, crc, sizeof(crc));
        snprintf(line, sizeof(line), "%-12s%s", wifi_security_label(s), crc);
        put_a(crc_attr(s), line);
    }

    put("");

    uint8_t n = view_slot_action_count(s);
    for (uint8_t i = 0; i < n; i++)
    {
        snprintf(line, sizeof(line), "%s %s", (i == cursor) ? ">" : " ",
                 action_words(view_slot_action_at(s, i)));
        put_a((i == cursor) ? VIEW_CURSOR : VIEW_PLAIN, line);
    }

    put("");
    put_a(VIEW_DIM, "A choose    B back");
}

void view_noop_notice(const wifi_slot_t *dest, bool allow_force)
{
    view_line_t line;

    put_a(VIEW_GOOD, "Nothing to restore.");
    put("");

    snprintf(line, sizeof(line), "Slot %u already holds exactly that backup,",
             dest->number);
    put(line);
    put("byte for byte. Nothing was written.");
    put("");

    if (allow_force)
    {
        put_a(VIEW_DIM, "Debug build: the write can be run anyway.");
        put_a(VIEW_DIM, "It programs zero bytes, because libnds skips");
        put_a(VIEW_DIM, "any page whose contents already match.");
        put("");
        put_a(VIEW_DIM, "X write anyway    B back");
    }
    else
    {
        put_a(VIEW_DIM, "B back");
    }
}
