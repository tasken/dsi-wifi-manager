// Every screen the app draws. Portable for the same reason wifi_slots.c is: it composes
// finished lines and hands each one to a sink the caller installs, so tools/host_slotlist.c
// --screens renders the exact bytes the console shows and a line that would wrap is
// visible on a PC.
//
// Clearing a screen is the caller's job -- that part is libnds's.

#ifndef SLOT_LIST_VIEW_H
#define SLOT_LIST_VIEW_H

#include <stdbool.h>

#include "backup_file.h"
#include "wifi_slots.h"

// Columns available on a screen: FB_COLS, 256 pixels over a 5-pixel glyph, one pixel spare.
// A bound on what any screen may emit; the widths phase of tools/crosscheck.py enforces it.
#define VIEW_COLS 51

// Bottom = where the user acts, one step at a time. Top = context that must survive the bottom
// screen moving on.
//
// The tag is what keeps the two panes separately measurable on the host. Drop it and they
// interleave, and the width and height checks measure the wrong screen.
typedef enum {
    VIEW_BOTTOM = 0,
    VIEW_TOP = 1,

    // The bottom screen's last row, whatever the content above it came to. Cart-Flasher pins
    // its action legend there by naming an absolute row; this app cannot, because screens here
    // emit lines in order and never name a row -- that is what lets the host harness render the
    // exact bytes and check widths offline.
    //
    // So placement is declared rather than computed: the view says "footer", the sink decides
    // where that is. One row only, matching Cart-Flasher; anything that wants to sit above it
    // stays part of the normal flow.
    VIEW_FOOTER = 2
} view_pane_t;

#define VIEW_PANES 3

// How a line should read, not what colour it is -- the sink owns the palette.
//
// Beside the line, never embedded as an escape sequence: strlen() would then count escapes as
// visible columns and the width check would have to strip them with a pattern.
typedef enum {
    VIEW_PLAIN = 0,
    VIEW_DIM,       // present but not the point: a free slot, a hint
    VIEW_GOOD,      // a checksum that verifies
    VIEW_BAD,       // a checksum that does not, a refusal, a warning
    VIEW_ACCENT,    // something to act on: the sequence that confirms a write
    VIEW_CURSOR,    // the row under the cursor, drawn as a bar
    VIEW_DEBUG      // present only in a Dev build: raw offsets, forced writes
} view_attr_t;

typedef void (*view_sink_t)(view_pane_t pane, view_attr_t attr, const char *line);

// One row built from differently-coloured pieces, for the write confirmation: Cart-Flasher
// colours each symbol of the combo by whether it has been entered, and a line-at-a-time sink
// cannot express that because it carries one attribute per line.
//
// Only that screen needs it. A sink that does not install a handler still gets the row, as one
// line in the last piece's colour, so nothing has to implement this to keep working.
typedef struct {
    const char  *text;
    view_attr_t  attr;
} view_seg_t;

typedef void (*view_segs_sink_t)(view_pane_t pane, const view_seg_t *segs, uint8_t count);

void view_set_segs_sink(view_segs_sink_t sink);

// Install the sink. Until this is called, every pane goes to stdout undistinguished,
// which is what a plain host build wants and what the app did before it had two screens.
void view_set_sink(view_sink_t sink);

// --- the top pane ------------------------------------------------------------------
//
// Each of these draws the whole top screen, banner included. The caller clears the pane
// and calls exactly one of them; they are not composable.

// A glanceable summary of the slot under the cursor, in words -- not a datasheet. The full
// decode is on the connection screen.
void view_summary(const wifi_layout_t *layout, const wifi_slot_t *slot, uint8_t of);

// The full decode, on the bottom screen, under the slot's own heading and above its actions.
// Reached by pressing A on a slot, which is a deliberate act of looking closely.
void view_conn_detail(const wifi_slot_t *slot);

// Held on screen for every step of a backup, so the confirm screen is not the only place
// that says which slot and which file.
void view_backup_context(const wifi_slot_t *slot, const char *dir, const char *filename);

// Held on screen for every step of a restore. Fields are filled in as the user descends
// through the pickers; a NULL is a question not answered yet and prints nothing, so the
// pane grows rather than lying about what has been chosen.
typedef struct {
    const char        *file;        // backup file name
    const char        *dir;         // folder it came from
    const wifi_slot_t *source;      // the record to write
    const wifi_slot_t *dest;        // the slot it goes into

    // `undo` is the name of the safety copy. NULL means no copy, which is three different
    // situations, so `undo_settled` says whether the question has been asked at all and
    // the destination's own state says why the answer was no.
    const char        *undo;
    bool               undo_settled;

    // Whether the destination already holds these exact bytes. Only meaningful once a
    // destination has been picked and the file re-read, hence the second flag.
    bool               noop;
    bool               noop_known;
} view_restore_ctx_t;

void view_restore_context(const view_restore_ctx_t *ctx);

// The banner alone, for the moments with no slot to describe: scanning the card, or a
// message screen that replaced a flow.
void view_idle_context(const wifi_layout_t *layout);

// --- the bottom pane ---------------------------------------------------------------

// One slot, two rows. `cursor` draws the ">" marker.
void view_conn_row(const wifi_slot_t *slot, bool cursor);

// The list's own title row.
void view_list_title(void);

// `debug` adds the hints for affordances a Release build does not have. main.c passes
// DSIWIFI_DEBUG rather than this file testing it, so the host harness can draw both variants.
void view_keys(bool debug);

// --- the connection screen -----------------------------------------------------------
//
// Reached with A from the list, left with B. The list chooses; this is where things happen.
// The top pane carries a glanceable summary; this carries the full decode and the actions,
// because arriving here is a deliberate act of looking closely.
//
// Only possible actions are offered. A free slot holds no network, so it has nothing to
// back up and shows restore alone -- an option that is absent rather than an error message
// explaining why the option you just picked was not available.
typedef enum {
    CONN_ACTION_BACKUP = 0,
    CONN_ACTION_RESTORE,
    CONN_ACTION_COUNT
} conn_action_t;

// How many actions this slot offers, and which action is at index `i`. Both live here
// rather than in main.c so the host harness draws the same menu the console does.
uint8_t view_conn_action_count(const wifi_slot_t *slot);
conn_action_t view_conn_action_at(const wifi_slot_t *slot, uint8_t index);

void view_conn_screen(const wifi_slot_t *slot, uint8_t cursor);

// The console's flash layout, on request from the list. Constant for the session, so it is
// not worth three rows of the glanceable pane on every cursor move.
void view_about(const wifi_layout_t *layout);

// Shown before anything is written. Says in as many words that the file holds the
// passphrase in the clear -- the app must say so rather than let the
// user find out.
void view_confirm(const wifi_slot_t *slot, const char *dir, const char *filename);

void view_result(bool ok, const char *dir, const char *filename, uint32_t bytes,
                 uint8_t records, const char *detail);

// --- restore ---------------------------------------------------------------------
//
// Four pickers and two prompts. Each is drawn from data already in memory, so none of
// them touches the card while the user is deciding.

// `top` is the first entry drawn; the caller scrolls by moving it. Only the entry under
// the cursor shows its folder and contents, which is what keeps a dozen files on screen.
//
// 12 rather than the 6 it was on a 24-row screen: at 6 the picker used 10 of 24 rows and
// made you scroll through a folder of backups twice as often for no reason. Worst case is
// 12 entries plus a six-record expansion plus the furniture, which is 22 of 24 -- and the
// overtall check in tools/crosscheck.py fails the build if that is ever wrong.
#define VIEW_PICK_ROWS 12

// Files holding nothing that can land in `dest` are shown and marked, not hidden -- "it exists
// but not here" is worth saying. Selectability is decided here, not in main.c, so the harness
// draws the same picker the console does.
bool view_entry_fits(const backup_entry_t *entry, const wifi_slot_t *dest);
uint8_t view_entry_count_fitting(const backup_entry_t *entries, uint8_t count,
                                 const wifi_slot_t *dest);

void view_pick_file(const backup_entry_t *entries, uint8_t count, uint8_t cursor,
                    uint8_t top, const wifi_slot_t *dest);

// Same rule inside one file: a multi-record file can hold both families, and only the
// records that fit the destination are selectable.
bool view_record_fits(const wifi_slot_t *rec, const wifi_slot_t *dest);
void view_pick_record(const backup_entry_t *entry, uint8_t cursor,
                      const wifi_slot_t *dest);

// Shown when the card holds backups but none of them can go in this slot.
void view_none_fit(const wifi_slot_t *dest);

// Deleting a backup is irreversible and the file may be the only copy of a Wi-Fi password,
// so it is confirmed on its own screen, as a cursor menu with the safe option selected.
typedef enum {
    DELETE_KEEP = 0,
    DELETE_DO_IT,
    DELETE_CHOICE_COUNT
} delete_choice_t;

void view_delete_confirm(const backup_entry_t *entry, uint8_t cursor);
void view_delete_result(bool ok, const char *name, const char *detail);

// Shown only when the destination is in use, so there is something to lose. A cursor menu
// with the same keys as every other choice in the app: A picks, B backs out.
typedef enum {
    UNDO_SAVE_COPY = 0,
    UNDO_OVERWRITE,
    UNDO_CHOICE_COUNT
} undo_choice_t;

void view_undo_prompt(const wifi_slot_t *dest, uint8_t cursor);

// The last screen before flash is written, and the only action gated behind more than one press.
// `seq` is directions the user must enter, then A -- the GodMode9 convention, because a
// confirmation you can hold A through is not one. `at` is how far through they are.
//
// main.c owns the entropy and only passes the sequence in, so the host harness can draw this
// screen with a fixed one.
#define VIEW_COMBO_LEN 4

void view_restore_confirm(const wifi_slot_t *source, const wifi_slot_t *dest,
                          const char *undo_name, const uint8_t *seq, uint8_t at);

// A wrong press. Offers a retry rather than ending the flow, matching Cart-Flasher and
// GodMode9: mistyping a four-symbol sequence is not a decision to abandon the restore.
void view_combo_wrong(void);

// One character per direction, for the sequence display: the FB_UP/FB_DOWN/FB_LEFT/FB_RIGHT
// arrow glyphs, and 'A' for the final press. Returns the codepoint, not a legible ASCII
// stand-in -- tools/host_slotlist.c substitutes "^v><" for a terminal.
char view_combo_symbol(uint8_t dir);

void view_restore_result(bool ok, uint8_t dest_number, const wifi_slot_t *now,
                         const char *undo_name, const char *detail);

void view_no_backups(void);

// Shown instead of the write flow when the destination already holds this exact record: the
// write would program nothing, so three screens of confirmation to report that is three noes.
//
// `allow_force` offers to do it anyway. main.c gates it on DSIWIFI_DEBUG rather than an #ifdef
// here, so both variants stay renderable by the host harness.
void view_noop_notice(const wifi_slot_t *dest, bool allow_force);

#endif // SLOT_LIST_VIEW_H
