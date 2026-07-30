// See slot_list_view.h.

#include <stdio.h>
#include <string.h>

#include "fb_render.h"
#include "slot_list_view.h"

// Lines are composed in a buffer with room to spare and cut to VIEW_COLS on the way
// out, so a long SSID or an undocumented security label can never wrap a row into the
// next one and turn one slot into two.
#define VIEW_SCRATCH 96

typedef char view_line_t[VIEW_SCRATCH];

// The list row spends "> N  " on its marker and number, and may add a note when the name
// is not printable. SSID_ROOM is what is left for the name itself: 40 at 51 columns, so a
// full 32-byte one fits whole.
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

static void default_sink(view_pane_t pane, view_attr_t attr, const char *line,
                         const view_attr_t *spans)
{
    // A host build that never calls view_set_sink() gets one undifferentiated stream,
    // which is what a quick `--list` wants. tools/host_slotlist.c installs its own.
    (void)pane;
    (void)attr;
    (void)spans;
    printf("%.*s\n", VIEW_COLS, line);
}

static view_sink_t sink = default_sink;

void view_set_sink(view_sink_t fn)
{
    sink = (fn != NULL) ? fn : default_sink;
}

static void emit(view_pane_t pane, view_attr_t attr, const char *line)
{
    sink(pane, attr, line, NULL);
}

// A row whose characters are individually coloured. Only the write confirmation uses this.
static void emit_spans(view_pane_t pane, view_attr_t attr, const char *line,
                       const view_attr_t *spans)
{
    sink(pane, attr, line, spans);
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

#define VIEW_APP_NAME "Wi-Fi Connections"

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

    snprintf(line, sizeof(line), "wifi 0x%05lX-0x%05lX  %u connections",
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

    put(line);
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



// Plain words for what the record is, not what byte says so. The security byte is spelled
// out on the connection screen; here the user wants to know whether it has a password.
static const char *security_words(const wifi_slot_t *s)
{
    const char *label = wifi_security_label(s);

    if (strcmp(label, "open") == 0 || strcmp(label, "none") == 0)
        return "Open network, no password";

    // An unverified label keeps its '?' even in plain language. Values 0x04-0x06 of the DSi
    // security byte are still inferred from the order of options in a menu, and docs/HARDWARE.md
    // forbids letting a label like that pass as settled. The connection screen has the byte.
    return (strchr(label, '?') != NULL) ? "Password protected, type unconfirmed"
                                        : "Password protected";
}

// The record's format, named. Three columns, and it claims nothing beyond the name.
//
// This replaced "Works on DS and DSi" / "DSi only", which was wrong in three ways: it
// described console compatibility when the only thing the family decides is where a backup
// can be restored; it advertised something no software can do, since fwTool's restore is
// commented out as TODO and this ROM's unitcode refuses to boot on a DS; and at 19 columns it
// was the longest thing on the row.
//
// Adopting NTR/TWL deliberately reverses the note in docs/HARDWARE.md under "Nintendo calls them
// connections", which said the split should acquire no user-facing vocabulary.
// That note was about not *inventing* one for a consumer flow. These are Nintendo's own
// codenames, they are already the names in wifi_slots.h, and this is a tool for someone
// moving records between connections on purpose. The reversal is recorded in both files
// rather than left to contradict them silently.
static const char *family_tag(wifi_family_t f)
{
    return (f == WIFI_FAMILY_TWL) ? "TWL" : "NTR";
}

// What the family means, for the pane that has room for a sentence. Only the WPA half is
// stated: connections 4-6 also carry a proxy, but no record with a configured proxy has ever
// been observed on any of the five test inputs, and docs/HARDWARE.md forbids letting an unobserved
// field pass as settled in user-facing text. The connection screen shows the raw bytes.
// Where a backup of this connection can go. The in-use counterpart to family_accepts(): that
// says what can come in, this says where this one can be put.
//
// It replaced a capability sentence -- "no room for a WPA password" -- which was unhelpful and
// misleading at once. Unhelpful because a working connection's inability to hold a kind of
// password it does not use tells the user nothing. Misleading because an NTR connection *can*
// carry a WEP key, which anyone would call a password.
//
// Numbers rather than NTR/TWL on purpose: the row already carries the tag for whoever wants the
// format name, and connection numbers are the only vocabulary Nintendo itself uses.
static const char *family_fits(wifi_family_t f)
{
    return (f == WIFI_FAMILY_TWL) ? "A backup of this fits Connections 4-6."
                                  : "A backup of this fits Connections 1-3.";
}

// What a free connection will accept. The article changes, so this is a whole phrase rather
// than the tag with a word glued either side.
static const char *family_takes(wifi_family_t f)
{
    return (f == WIFI_FAMILY_TWL) ? "takes a TWL backup" : "takes an NTR backup";
}

// What a free connection accepts. Numbers, matching family_fits(), so the pane speaks one
// vocabulary throughout; the NTR/TWL tag lives on the list row for whoever wants it.
static const char *family_accepts(wifi_family_t f)
{
    return (f == WIFI_FAMILY_TWL) ? "A backup from Connections 4-6 fits here."
                                  : "A backup from Connections 1-3 fits here.";
}

// Where the family tag starts on a list row. 5 columns of indent plus this leaves the tag at
// column 44 and ends it at 47, inside the 51 the pane has.
//
// It is 39 rather than the 20 the old family column forced because a three-column tag no
// longer has to be paid for out of the security phrase. That is the whole reason
// security_short() is gone: it shortened "Password protected, type unconfirmed" to
// "Password protected", which silently dropped the one word saying the type is a guess.
//
// The longest phrase security_words() returns is 36, so there are always at least three
// spaces before the tag. Nothing enforces that at compile time -- the phrases are chosen at
// runtime -- so the widths phase of tools/crosscheck.py is what holds it.
#define ROW_SEC_W 39

void view_summary(const wifi_layout_t *layout, const wifi_slot_t *s, uint8_t of)
{
    view_line_t line;
    (void)layout;

    top_banner();
    rule(VIEW_TOP);

    snprintf(line, sizeof(line), "Connection %u of %u", s->number, of);
    top_a(VIEW_DIM, line);
    top("");

    if (s->is_free)
    {
        top_a(VIEW_DIM, "Empty");
        top("");
        top_a(VIEW_DIM, "No network is saved here.");

        // Named, because the unqualified version promised something it could not keep. "A
        // backup can be restored into it" is false whenever every backup on the card is the
        // other family, and the app only said so after the user had picked this connection
        // and walked into view_none_fit.
        top_a(VIEW_DIM, family_accepts(s->family));
        return;
    }

    // The whole name, wrapped rather than cut. This is the one thing the user came to read.
    top_wrapped(s->ssid);
    if (!s->ssid_printable)
        top_a(VIEW_DIM, "  (some characters could not be shown)");
    top("");

    top(security_words(s));
    top(family_fits(s->family));
    top("");

    // The checksum matters to a person only as "is this readable", so it is silent when
    // fine and loud when not.
    bool ok = s->crc_ok && (!s->has_crc2 || s->crc2_ok);
    if (ok)
        top_a(VIEW_GOOD, "Saved settings look intact.");
    else
        top_a(VIEW_BAD, "These settings look damaged.");
}

void view_idle_context(const wifi_layout_t *layout)
{
    top_banner();
    top_layout(layout);
    rule(VIEW_TOP);
}

// "1  \"name\"" on one row, for the context panes, where the label eats 6 columns.
#define CTX_LABEL 6

static void top_slot_line(const char *label, const wifi_slot_t *s)
{
    view_line_t line;
    char ssid[WIFI_SSID_MAX + 2];

    if (s->is_free)
    {
        snprintf(line, sizeof(line), "%-*s%u  (empty)", CTX_LABEL, label, s->number);
    }
    else
    {
        // CTX_LABEL, the connection number, two spaces and the two quotes.
        ssid_text(s, VIEW_COLS - CTX_LABEL - DESC_FURNITURE, ssid, sizeof(ssid));
        snprintf(line, sizeof(line), "%-*s%u  \"%s\"", CTX_LABEL, label, s->number, ssid);
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

    top_slot_line("connection", slot);
    if (!slot->is_free)
        top_record_line(slot);

    top("");
    if (dir != NULL)
        top_text_line("to", dir);
    if (filename != NULL)
        top_text_line("file", filename);

    top("");
    rule(VIEW_TOP);
    top_a(VIEW_BAD, "The backup stores your Wi-Fi password");
    top_a(VIEW_BAD, "unprotected on the SD card.");
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
        top_text_line("copy", ctx->undo);
    }
    else if (ctx->undo_settled)
    {
        // Three different reasons for "no copy", and which one it is changes whether the
        // user should be worried. The confirm screen says the same thing; this pane keeps
        // saying it while they read the rest.
        if (ctx->dest != NULL && ctx->dest->is_free)
            top("copy  not needed, nothing here yet");
        else if (ctx->noop_known && ctx->noop)
            top("copy  not needed, settings already match");
        else
            top("copy  declined, no copy kept");
    }

    top("");
    rule(VIEW_TOP);

    if (ctx->noop_known && ctx->noop)
        top_a(VIEW_DIM, "Nothing will be programmed.");
    else if (ctx->dest != NULL)
        top_a(VIEW_BAD, "Changes your console's settings.");
}

// --- the bottom pane ---------------------------------------------------------------

void view_list_title(void)
{
    // Nintendo's own word, taken from the console's System Settings rather than from the
    // flash layout. See docs/HARDWARE.md, "Nintendo calls them connections".
    put("Connections");
}

void view_conn_row(const wifi_slot_t *s, bool cursor)
{
    view_line_t line;
    const char *mark = cursor ? ">" : " ";

    // An empty connection is one row. There is nothing to say about it beyond that, and
    // spending a second row on "no security, no family" would make six empty connections
    // look busier than six configured ones.
    if (s->is_free)
    {
        // What this connection accepts, on the cursor row only. On every row it would be six
        // repetitions of two phrases on a console with nothing saved yet -- which is the first
        // screen a new user sees -- and the information is only ever needed for the connection
        // being pointed at. The file picker already expands only its cursor entry for the same
        // reason, so this is that behaviour reused rather than a new idea.
        if (cursor)
            snprintf(line, sizeof(line), "%s %u  Empty   %s",
                     mark, s->number, family_takes(s->family));
        else
            snprintf(line, sizeof(line), "%s %u  Empty", mark, s->number);

        put_a(cursor ? VIEW_CURSOR : VIEW_DIM, line);
        return;
    }

    // SSID_ROOM is 40 at 51 columns, so a full 32-byte name fits here whole.
    char ssid[WIFI_SSID_MAX + 2];
    ssid_text(s, SSID_ROOM, ssid, sizeof(ssid));

    snprintf(line, sizeof(line), "%s %u  %s%s", mark, s->number, ssid,
             s->ssid_printable ? "" : "  (name has odd characters)");
    put_a(cursor ? VIEW_CURSOR : VIEW_PLAIN, line);

    // The second row is what the connection is, not what bytes say so. Damage is the one
    // thing worth shouting about, because it is the one thing the user can act on.
    bool ok = s->crc_ok && (!s->has_crc2 || s->crc2_ok);
    if (!ok)
    {
        put_a(cursor ? VIEW_CURSOR : VIEW_BAD, "     These settings look damaged.");
        return;
    }

    snprintf(line, sizeof(line), "     %-*s%s",
             ROW_SEC_W, security_words(s), family_tag(s->family));
    put_a(cursor ? VIEW_CURSOR : VIEW_DIM, line);
}


// The action legend, always the last line of a screen. Cart-Flasher's convention: each entry is
// <KEY> followed by a capitalised verb, three spaces between entries, drawn in the dim colour.
//
// Centralised because it had drifted: the same action read "pick" on one screen and "choose" on
// another, and the spacing between pairs was 2, 3 or 4 depending on the screen.
static void put_keys_a(view_attr_t attr, const char *legend)
{
    emit(VIEW_FOOTER, attr, legend);
}

static void put_keys(const char *legend)
{
    put_keys_a(VIEW_DIM, legend);
}

void view_keys(bool debug)
{
    // The About screen is raw flash offsets, so it is a Dev-build affordance and says so in the
    // colour every other debug affordance uses. Pinned two rows above the footer rather than
    // joining it: the footer is one row and a Dev-only extra must not push a real action off it,
    // but flowing under the list made it read as part of the list.
    if (debug)
        emit(VIEW_HINT, VIEW_DEBUG, "<SELECT> Flash layout");

    // No blank line before this any more. It was separating the legend from the list back when
    // the legend flowed; the footer is pinned to the last row now, so the gap is whatever is
    // left over.
    put_keys("<UP/DN> Move   <A> Open   <START> Exit");
}

void view_confirm(const wifi_slot_t *slot, const char *dir, const char *filename)
{
    view_line_t line;
    char ssid[WIFI_SSID_MAX + 2];

    snprintf(line, sizeof(line), "Back up Connection %u?", slot->number);
    put(line);
    put("");

    // Two columns of indent plus DESC_FURNITURE for the number, spaces and quotes.
    ssid_text(slot, VIEW_COLS - 2 - DESC_FURNITURE, ssid, sizeof(ssid));
    snprintf(line, sizeof(line), "  %u  \"%s\"", slot->number, ssid);
    put(line);

    put("");
    put_a(VIEW_BAD, "The backup stores your Wi-Fi password unprotected");
    put_a(VIEW_BAD, "on the SD card. Anyone with the card can read it.");
    put("");
    put(dir);
    put(filename);
    put("");
    put_keys("<A> Write   <B> Cancel");
}

void view_result(bool ok, const char *dir, const char *filename, uint32_t bytes,
                 uint8_t records, const char *detail)
{
    view_line_t line;

    put_a(ok ? VIEW_GOOD : VIEW_BAD, ok ? "Backup saved." : "Backup did not finish.");
    put("");

    if (ok)
    {
        put(dir);
        put(filename);
        // Byte count without the word "record": a user has one backup, not one record in a
        // file. The count only matters when there is more than one, which the app itself
        // never writes.
        if (records > 1)
            snprintf(line, sizeof(line), "%lu bytes, %u connections", (unsigned long)bytes,
                     records);
        else
            snprintf(line, sizeof(line), "%lu bytes", (unsigned long)bytes);
        put_a(VIEW_DIM, line);
        put("");
        put("Read back and verified.");
    }
    else if (detail != NULL)
    {
        put(detail);
    }

    put("");
    put_keys(ok ? "<A> Continue" : "<B> Back");
}

// --- restore ---------------------------------------------------------------------

// A connection rendered as text: "1  \"name\"" is DESC_FURNITURE columns of furniture plus
// the SSID room the caller allows, which is always less than a row. Sized so the compiler
// can see that composing one into a row cannot overflow.
#define VIEW_DESC 48
typedef char view_desc_t[VIEW_DESC];

// The bound that actually applies. `room` never binds, because it is always at least
// WIFI_SSID_MAX + DESC_FURNITURE for every caller, so the widest output is the furniture
// plus a full 32-byte SSID. Asserted rather than argued about: this was reported as an
// overflow once, on arithmetic that used `room` as the limit, and it was not one.
_Static_assert(VIEW_DESC > DESC_FURNITURE + WIFI_SSID_MAX,
               "describe() output plus its NUL must fit view_desc_t");

// "1  \"name\"" or "1  (empty)", fitted into `room` columns *in total*.
//
// `room` used to mean the SSID's share alone, and every caller passed VIEW_COLS minus
// its own prefix -- that is, the room for the whole description. So each one came out
// DESC_FURNITURE columns too wide, and with a 32-byte SSID the restore screens lost
// their right edge: "onto  1  \"ABCDEFGHIJKLMNOPQRSTU" with the closing quote cut off.
// Both SSIDs in the test inputs are 3 and 6 bytes, so it never showed. The width phase
// of tools/crosscheck.py exists because of this.
//
// Taking the total and subtracting here means a caller only has to know its own prefix,
// which is the one thing it actually knows.
static void describe(const wifi_slot_t *s, int room, char *out, size_t out_len)
{
    if (s->is_free)
    {
        snprintf(out, out_len, "%u  (empty)", s->number);
        return;
    }

    char ssid[WIFI_SSID_MAX + 2];
    ssid_text(s, room - DESC_FURNITURE, ssid, sizeof(ssid));
    snprintf(out, out_len, "%u  \"%s\"", s->number, ssid);
}

// A record fits a slot when the families match. That is the whole rule: a 0x200 record
// carries a passphrase and a precomputed PSK that physically do not fit in a 0x100 slot, and
// widening the other way is deliberately out of scope. restore_check() still has the final
// say before any byte is written; this only decides what the user is offered.
bool view_record_fits(const wifi_slot_t *rec, const wifi_slot_t *dest)
{
    return rec->family == dest->family;
}

bool view_entry_fits(const backup_entry_t *entry, const wifi_slot_t *dest)
{
    if (!entry->ok)
        return false;

    for (uint8_t r = 0; r < entry->count; r++)
    {
        if (view_record_fits(&entry->rec[r], dest))
            return true;
    }
    return false;
}

uint8_t view_entry_count_fitting(const backup_entry_t *entries, uint8_t count,
                                 const wifi_slot_t *dest)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        if (view_entry_fits(&entries[i], dest))
            n++;
    }
    return n;
}

// Only entries that can land in `dest` are drawn at all. An earlier version showed the rest
// dimmed with a reason, on the theory that hiding raises "where did my backup go?". It does
// not: the picker's job is to choose a backup for this slot, and one that cannot go here is
// not a candidate. When nothing fits, view_none_fit() answers that question properly, with
// room to explain; when something fits, the user gets on with it.
//
// `cursor` and `top` index the entries that fit, not the whole array.
void view_pick_file(const backup_entry_t *entries, uint8_t count, uint8_t cursor,
                    uint8_t top, const wifi_slot_t *dest)
{
    view_line_t line;
    uint8_t fitting = view_entry_count_fitting(entries, count, dest);

    snprintf(line, sizeof(line), "Restore into Connection %u: pick a backup (%u)",
             dest->number, fitting);
    put(line);
    put("");

    uint8_t drawn = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        if (!view_entry_fits(&entries[i], dest))
            continue;

        if (drawn < top || drawn >= (uint8_t)(top + VIEW_PICK_ROWS))
        {
            drawn++;
            continue;
        }

        bool sel = (drawn == cursor);
        snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", entries[i].name);
        put_a(sel ? VIEW_CURSOR : VIEW_PLAIN, line);
        drawn++;

        // Only the highlighted file expands. Everything the detail shows was decoded
        // when the card was scanned, so moving the cursor never reopens a file.
        if (!sel)
            continue;

        snprintf(line, sizeof(line), "    from %s", entries[i].dir);
        put_a(VIEW_DIM, line);

        for (uint8_t r = 0; r < entries[i].count; r++)
        {
            if (!view_record_fits(&entries[i].rec[r], dest))
                continue;
            view_desc_t what;
            describe(&entries[i].rec[r], VIEW_COLS - 4, what, sizeof(what));
            snprintf(line, sizeof(line), "    %s", what);
            put(line);
        }
    }

    // One legend either way. It used to say "scroll" when the list was longer than the window
    // and "move" otherwise, which cost a column the widest form could not spare once every key
    // gained its brackets.
    put_keys("<UP/DN> Move   <A> Select   <X> Delete   <B> Back");
}

void view_none_fit(const wifi_slot_t *dest)
{
    view_line_t line;

    snprintf(line, sizeof(line), "No backup fits Connection %u.", dest->number);
    put(line);
    put("");

    // Says which connections a backup has to come from, rather than what a record can hold.
    // Earlier versions explained by console ("one the DS can read too") and then by capability
    // ("no room for a WPA password"); neither told the user where to look instead.
    if (dest->family == WIFI_FAMILY_TWL)
    {
        put("Only a backup from Connections 4-6 fits here, and");
        put("every backup on the card came from Connections 1-3.");
    }
    else
    {
        put("Only a backup from Connections 1-3 fits here, and");
        put("every backup on the card came from Connections 4-6.");
    }

    put("");
    put_keys("<B> Back");
}

void view_pick_record(const backup_entry_t *entry, uint8_t cursor,
                      const wifi_slot_t *dest)
{
    view_line_t line;

    snprintf(line, sizeof(line), "Restore into Connection %u: pick a backup", dest->number);
    put(line);
    put(entry->name);
    put("");

    // Records that cannot land here are not drawn, for the same reason files are not.
    for (uint8_t i = 0; i < entry->count; i++)
    {
        const wifi_slot_t *s = &entry->rec[i];
        if (!view_record_fits(s, dest))
            continue;

        view_desc_t what;
        bool sel = (i == cursor);

        describe(s, VIEW_COLS - 2, what, sizeof(what));
        snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", what);
        put_a(sel ? VIEW_CURSOR : VIEW_PLAIN, line);

        char crc[16];
        crc_text(s, crc, sizeof(crc));
        snprintf(line, sizeof(line), "    %-10s [%02X] %s",
                 wifi_security_label(s), wifi_slot_security_byte(s), crc);
        put(line);
    }

    put("");
    put_keys("<A> Select   <B> Back");
}

// A cursor menu, not three buttons.
//
// This asked A for yes, X for no and B for cancel, which was the only three-button choice in
// the app and put the destructive option -- overwrite with no copy -- on a key with no
// conventional meaning on this console. Everywhere else here, and everywhere on a DS, A
// confirms and B goes back while a cursor picks between options. It reads the same way now
// as the connection screen does, and "overwrite without a copy" has to be moved to rather than
// being one press away.
void view_undo_prompt(const wifi_slot_t *dest, uint8_t cursor)
{
    view_line_t line;
    view_desc_t what;

    snprintf(line, sizeof(line), "Connection %u already has settings:", dest->number);
    put(line);

    describe(dest, VIEW_COLS - 2, what, sizeof(what));
    snprintf(line, sizeof(line), "  %s", what);
    put(line);

    put("");
    put_a(VIEW_BAD, "Restoring overwrites it.");
    put("");

    for (uint8_t i = 0; i < UNDO_CHOICE_COUNT; i++)
    {
        snprintf(line, sizeof(line), "%s %s", (i == cursor) ? ">" : " ",
                 (i == UNDO_SAVE_COPY) ? "Save a copy to the SD first, then restore"
                                       : "Restore without saving a copy");
        put_a((i == cursor) ? VIEW_CURSOR : VIEW_PLAIN, line);
    }

    put("");
    put_keys("<A> Select   <B> Cancel");
}

// Directions as the renderer's arrow glyphs, which are real triangles rather than the
// '^' 'v' '<' '>' this used to spell them with. Their codepoints sit below 0x20 where no
// user-supplied string can reach; see fb_render.h.
char view_combo_symbol(uint8_t dir)
{
    switch (dir & 3)
    {
        case 0:  return FB_UP;
        case 1:  return FB_DOWN;
        case 2:  return FB_LEFT;
        default: return FB_RIGHT;
    }
}

void view_restore_confirm(const wifi_slot_t *source, const wifi_slot_t *dest,
                          const char *undo_name, const uint8_t *seq, uint8_t at)
{
    view_line_t line;
    view_desc_t what;

    snprintf(line, sizeof(line), "Restore into Connection %u", dest->number);
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
        put_a(VIEW_BAD, "Its settings will be lost, with no copy kept.");
        put("");
    }

    put_a(VIEW_BAD, "This changes your console's settings.");
    put("");

    // Where the marker goes. The symbols themselves are built as coloured pieces below; this is
    // only the column arithmetic, four columns per symbol so a glance cannot mistake one for
    // its neighbour.
    int at_col = (at >= VIEW_COMBO_LEN) ? (VIEW_COMBO_LEN * 4) : (at * 4);

    put("Enter this to continue:");
    put("");

    // Each symbol coloured by whether it has been entered.
    //
    // Entered symbols are VIEW_GOOD (green) and pending ones VIEW_PLAIN (white). Deliberately not
    // accent-to-good, which is what the palette would otherwise suggest: FB_ACCENT and FB_GOOD are
    // two saturated colours about 12-13 five-bit steps apart per channel, while white-to-green is
    // a brightness collapse that cannot be misread. This is the one screen where miscounting gates
    // an irreversible write, so legibility beats palette consistency.
    //
    // Built as one string plus one attribute per character, so strlen() still equals the rendered
    // width and the overlong check reads this row like any other.
    view_line_t row;
    view_attr_t spans[VIEW_SCRATCH];
    size_t len = 0;

    for (int i = 0; i < 4 && len < sizeof(row) - 1; i++, len++)
    {
        row[len] = ' ';
        spans[len] = VIEW_PLAIN;
    }

    for (uint8_t i = 0; i <= VIEW_COMBO_LEN && len < sizeof(row) - 1; i++)
    {
        // Four columns per symbol, matching the marker arithmetic above, so a glance cannot
        // mistake one symbol for its neighbour.
        int wide = (i < VIEW_COMBO_LEN) ? 4 : 1;
        view_attr_t a = (i < at) ? VIEW_GOOD : VIEW_PLAIN;

        for (int c = 0; c < wide && len < sizeof(row) - 1; c++, len++)
        {
            row[len] = (c == 0) ? ((i < VIEW_COMBO_LEN) ? view_combo_symbol(seq[i]) : 'A') : ' ';
            spans[len] = a;
        }
    }
    row[len] = '\0';

    emit_spans(VIEW_BOTTOM, VIEW_PLAIN, row, spans);

    // The marker stays under the next symbol. Colour says how far you have come; this says
    // which one is next, and one signal doing both jobs was ambiguous at the final A.
    //
    // A '-' rather than a '^': the symbols above are real triangles, and a caret directly under
    // the up arrow read as a second, smaller arrow.
    char caret[VIEW_COMBO_LEN * 4 + 8];
    memset(caret, ' ', sizeof(caret));
    caret[4 + at_col] = '-';
    caret[4 + at_col + 1] = '\0';
    put_a(VIEW_ACCENT, caret);

    put("");
    put_keys("<B> Cancel");
}

// Shown when a press was not the next symbol. Nothing has been written at this point -- the
// combo gates the write and this is a failure to get through it -- and saying so is the whole
// point of the screen: the user needs to know the console was not touched before deciding
// whether to try again.
void view_combo_wrong(void)
{
    put_a(VIEW_BAD, "Wrong button, nothing was written.");
    put("");
    put("The sequence starts over.");
    put("");
    put_keys("<A> Retry   <B> Cancel");
}

void view_restore_result(bool ok, uint8_t dest_number, const wifi_slot_t *now,
                         const char *undo_name, const char *detail)
{
    view_line_t line;

    put_a(ok ? VIEW_GOOD : VIEW_BAD, ok ? "Restore done." : "Restore did not finish.");
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
        snprintf(line, sizeof(line), "Connection %u could not be read back,", dest_number);
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
    put_keys(ok ? "<A> Continue" : "<B> Back");
}


// --- the connection screen -----------------------------------------------------------

uint8_t view_conn_action_count(const wifi_slot_t *slot)
{
    // A free slot has nothing to copy out of it, so backup is not offered at all. That
    // replaces the old message telling the user to go and stand somewhere else.
    return slot->is_free ? 1 : CONN_ACTION_COUNT;
}

conn_action_t view_conn_action_at(const wifi_slot_t *slot, uint8_t index)
{
    if (slot->is_free)
        return CONN_ACTION_RESTORE;

    return (index == 0) ? CONN_ACTION_BACKUP : CONN_ACTION_RESTORE;
}

static const char *action_words(conn_action_t a)
{
    return (a == CONN_ACTION_BACKUP) ? "Back up this connection to the SD card"
                                     : "Restore a backup into this connection";
}

void view_conn_screen(const wifi_slot_t *s, uint8_t cursor)
{
    view_line_t line;

    if (s->is_free)
    {
        snprintf(line, sizeof(line), "Connection %u   empty", s->number);
        put_a(VIEW_DIM, line);
        put("");
        put("Nothing is saved in this connection.");
    }
    else
    {
        char ssid[WIFI_SSID_MAX + 2];

        // The whole SSID fits: this row spends only the slot number and family before it.
        ssid_text(s, VIEW_COLS - 16, ssid, sizeof(ssid));
        snprintf(line, sizeof(line), "Connection %u   \"%s\"", s->number, ssid);
        put(line);

    }

    put("");
    view_conn_detail(s);
    put("");

    uint8_t n = view_conn_action_count(s);
    for (uint8_t i = 0; i < n; i++)
    {
        snprintf(line, sizeof(line), "%s %s", (i == cursor) ? ">" : " ",
                 action_words(view_conn_action_at(s, i)));
        put_a((i == cursor) ? VIEW_CURSOR : VIEW_PLAIN, line);
    }

    put("");
    put_keys("<A> Select   <B> Back");
}

void view_noop_notice(const wifi_slot_t *dest, bool allow_force)
{
    view_line_t line;

    put_a(VIEW_GOOD, "Nothing to restore.");
    put("");

    snprintf(line, sizeof(line), "Connection %u already holds exactly that backup,",
             dest->number);
    put(line);
    put("byte for byte. Nothing was written.");
    put("");

    if (allow_force)
    {
        // The whole block, the action line included. Colouring only the headline left the
        // key that actually triggers the forced write looking like an ordinary hint.
        put_a(VIEW_DEBUG, "Debug build: the write can be run anyway.");
        put_a(VIEW_DEBUG, "It programs zero bytes, because libnds skips");
        put_a(VIEW_DEBUG, "any page whose contents already match.");
        put("");
        put_keys_a(VIEW_DEBUG, "<X> Write anyway   <B> Back");
    }
    else
    {
        put_keys("<B> Back");
    }
}

// --- the slot's full decode, on the bottom screen ------------------------------------
//
// This is what the top pane used to carry. It moved because the top screen sits further
// from the eye, cannot scroll and cannot be touched, and because P1 Tier B will add the
// secrets block and push this past 24 rows.
void view_conn_detail(const wifi_slot_t *s)
{
    view_line_t line;
    char crc[16];

    // The raw offset and length: the one pair of facts that ties a slot on screen to a
    // range in a flash dump, which is how every hardware finding here was checked.
    snprintf(line, sizeof(line), "0x%05lX  %u bytes    status 0x%02X    config 0x%02X",
             (unsigned long)s->offset, s->length, s->status, s->config_bits);
    put_a(VIEW_DIM, line);

    crc_text(s, crc, sizeof(crc));

    if (s->is_free)
    {
        // Do not name a security mode for a slot holding no network. wifi_security_label()
        // reads "open" for the 0x00 a free slot happens to carry, and printing that directly
        // under "nothing is saved in this slot" states two contradictory things. The raw byte
        // still appears, because that is the rule; only the interpretation is withheld.
        snprintf(line, sizeof(line), "%-*s[%02X]  no network here   %s",
                 DETAIL_LABEL, "Security", wifi_slot_security_byte(s), crc);
        put_a(crc_attr(s), line);
        return;
    }

    snprintf(line, sizeof(line), "%-*s%-10s [%02X]   %s", DETAIL_LABEL, "Security",
             wifi_security_label(s), wifi_slot_security_byte(s), crc);
    put_a(crc_attr(s), line);

    put("");

    detail_cell_t l[2];
    detail_cell_t r[4];
    char v[40];

    addr_text(s->ip, "automatic (DHCP)", v, sizeof(v));
    detail_cell(l[0], "Address", v);
    addr_text(s->gateway, "automatic", v, sizeof(v));
    detail_cell(l[1], "Gateway", v);

    addr_text(s->dns1, "not set", v, sizeof(v));
    detail_cell(r[0], "DNS 1", v);
    addr_text(s->dns2, "not set", v, sizeof(v));
    detail_cell(r[1], "DNS 2", v);

    // Printed as they read. Subnet 0 and MTU 0 are both what a working connection stores on
    // the reference DSi, and MTU 0 is the value System Settings later rewrites to 1400, so
    // dressing either up as invalid would contradict a hardware finding.
    snprintf(v, sizeof(v), "/%u", s->subnet_prefix);
    detail_cell(r[2], "Subnet", v);
    snprintf(v, sizeof(v), "%u", s->mtu);
    detail_cell(r[3], "MTU", v);

    for (int i = 0; i < 4; i++)
        detail_row((i < 2) ? l[i] : "", r[i]);
}

// The layout the header used to occupy on every screen. Constant for the session, so it is
// seen once on request rather than redrawn on every cursor move.
void view_about(const wifi_layout_t *layout)
{
    view_line_t line;

    // Its own label width: "Flash type" is ten characters and DETAIL_LABEL is nine, so
    // reusing that one silently ran the label into its value.
    const int lw = 12;

    // Every line is VIEW_DEBUG, not just a heading. main.c only opens this screen in a Dev
    // build, and a screen that exists in one build and not the other should be unmistakable
    // the moment it appears rather than after reading it.
    put_a(VIEW_DEBUG, "About this console");
    put("");

    snprintf(line, sizeof(line), "%-*s0x%02X  %s", lw, "Flash type",
             layout->console_type, layout->is_dsi ? "DSi" : "Nintendo DS");
    put_a(VIEW_DEBUG, line);

    snprintf(line, sizeof(line), "%-*s%u", lw, "Connections", layout->count);
    put_a(VIEW_DEBUG, line);

    snprintf(line, sizeof(line), "%-*s0x%05lX-0x%05lX", lw, "Wi-Fi area",
             (unsigned long)layout->region_start, (unsigned long)(layout->base - 1));
    put_a(VIEW_DEBUG, line);

    snprintf(line, sizeof(line), "%-*s0x%05lX", lw, "Settings",
             (unsigned long)layout->base);
    put_a(VIEW_DEBUG, line);

    put("");
    put_a(VIEW_DEBUG, "Connection positions are read from the console,");
    put_a(VIEW_DEBUG, "never assumed, so a console laid out");
    put_a(VIEW_DEBUG, "differently still decodes correctly.");
    put("");
    put_keys_a(VIEW_DEBUG, "<B> Back");
}

void view_no_backups(void)
{
    put("No backups on the card.");
    put("");
    put("Every folder under DSIWIFI/ was searched for");
    put(".dswifi files and none were found.");
    put("");
    put("Back up a connection first, or copy a backup from");
    put("another console onto the card.");
    put("");
    put_keys("<B> Back");
}

// --- deleting a backup ----------------------------------------------------------------

void view_delete_confirm(const backup_entry_t *entry, uint8_t cursor)
{
    view_line_t line;

    put("Delete this backup?");
    put("");

    snprintf(line, sizeof(line), "  %s", entry->name);
    put(line);
    snprintf(line, sizeof(line), "  from %s", entry->dir);
    put_a(VIEW_DIM, line);
    put("");

    // Say what is not affected as well as what is. The file and the connection have similar
    // names on screen, and someone reaching for "delete" wants to be sure which one goes.
    put_a(VIEW_BAD, "This cannot be undone.");
    put("Your console's settings are not affected, only");
    put("this file on the SD card.");
    put("");

    for (uint8_t i = 0; i < DELETE_CHOICE_COUNT; i++)
    {
        snprintf(line, sizeof(line), "%s %s", (i == cursor) ? ">" : " ",
                 (i == DELETE_KEEP) ? "Keep it" : "Delete it");
        put_a((i == cursor) ? VIEW_CURSOR : VIEW_PLAIN, line);
    }

    put("");
    put_keys("<A> Select   <B> Cancel");
}

void view_delete_result(bool ok, const char *name, const char *detail)
{
    view_line_t line;

    put_a(ok ? VIEW_GOOD : VIEW_BAD, ok ? "Backup deleted." : "Could not delete it.");
    put("");

    snprintf(line, sizeof(line), "  %s", name);
    put_a(VIEW_DIM, line);

    if (!ok && detail != NULL)
    {
        put("");
        put_a(VIEW_DIM, detail);
    }

    put("");
    put_keys(ok ? "<A> Continue" : "<B> Back");
}
