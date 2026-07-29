// SPDX-License-Identifier: CC0-1.0
//
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

// Columns available on a screen. This is FB_COLS: 256 pixels of screen over a 5-pixel
// glyph is 51, with one pixel to spare.
//
// It used to be 31, one short of the 32 a tile console gives, because that console wraps
// and a wrapped line turns one entry into two. The framebuffer clips instead of wrapping,
// so the margin is no longer needed for that; the cut stays as a bound on what any screen
// can emit, and tools/crosscheck.py fails the build if a line exceeds it.
#define VIEW_COLS 51

// The app draws on both screens. The bottom one is where the user acts: lists, prompts,
// results, one step at a time. The top one holds context that must not disappear when the
// bottom screen moves on -- which slot, which file, what is about to be written.
//
// Splitting them is why the sink exists. On the console each pane is a framebuffer and the
// sink draws into one; on the host they are two tagged streams, each checked for width and
// height on its own. Without the pane argument the two would interleave and those checks
// would be meaningless.
//
// The sink is also what let the renderer be replaced. This started on the libnds tile
// console at 32 columns and moved to a bitmap at 51 without slot_list_view.c changing
// beyond VIEW_COLS, because nothing here knows what a pane physically is.
typedef enum {
    VIEW_BOTTOM = 0,
    VIEW_TOP = 1
} view_pane_t;

// How a line should read, not what colour it is. The sink owns the palette, because the
// console and the host answer that question differently and neither answer belongs here.
//
// Carried beside the line rather than embedded in it as an escape sequence. Escapes would
// work on screen, but then strlen() counts them as visible columns and the width check in
// tools/crosscheck.py would depend on stripping them with a pattern. An attribute keeps
// the string exactly the characters that get drawn.
typedef enum {
    VIEW_PLAIN = 0,
    VIEW_DIM,       // present but not the point: a free slot, a hint
    VIEW_GOOD,      // a checksum that verifies
    VIEW_BAD,       // a checksum that does not, a refusal, a warning
    VIEW_CURSOR     // the row under the cursor, drawn as a bar
} view_attr_t;

typedef void (*view_sink_t)(view_pane_t pane, view_attr_t attr, const char *line);

// Install the sink. Until this is called, every pane goes to stdout undistinguished,
// which is what a plain host build wants and what the app did before it had two screens.
void view_set_sink(view_sink_t sink);

// --- the top pane ------------------------------------------------------------------
//
// Each of these draws the whole top screen, banner included. The caller clears the pane
// and calls exactly one of them; they are not composable.

// The list screen's companion: the full decode of the slot under the cursor, laid out in
// two columns -- identity on the left, network settings on the right. This is the app's
// detail view; there is no separate screen for it.
void view_detail(const wifi_layout_t *layout, const wifi_slot_t *slot);

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
void view_slot(const wifi_slot_t *slot, bool cursor);

// The list's own title row. Short, because the layout it used to also print moved to the
// top pane, where there is room to spell it out.
void view_list_title(void);

void view_keys(void);

// --- the slot screen -----------------------------------------------------------------
//
// Reached with A from the list, left with B. The list chooses; this is where things happen.
// The top pane already carries the full decode, so this carries the identity in one line
// and the actions.
//
// Only possible actions are offered. A free slot holds no network, so it has nothing to
// back up and shows restore alone -- an option that is absent rather than an error message
// explaining why the option you just picked was not available.
typedef enum {
    SLOT_ACTION_BACKUP = 0,
    SLOT_ACTION_RESTORE,
    SLOT_ACTION_COUNT
} slot_action_t;

// How many actions this slot offers, and which action is at index `i`. Both live here
// rather than in main.c so the host harness draws the same menu the console does.
uint8_t view_slot_action_count(const wifi_slot_t *slot);
slot_action_t view_slot_action_at(const wifi_slot_t *slot, uint8_t index);

void view_slot_screen(const wifi_slot_t *slot, uint8_t cursor);

// Shown before anything is written. Says in as many words that the file holds the
// passphrase in the clear -- DESIGN.md requires the app to say so rather than let the
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
void view_pick_file(const backup_entry_t *entries, uint8_t count, uint8_t cursor,
                    uint8_t top);

void view_pick_record(const backup_entry_t *entry, uint8_t cursor);

// Shown only when the destination is in use, so there is something to lose.
void view_undo_prompt(const wifi_slot_t *dest);

// `undo_name` is NULL if the user declined a safety copy.
//
// Neither of these takes a no-op flag any more. A restore whose bytes already match the
// destination is answered before the write flow starts, so a confirm screen saying "this
// will program nothing" and a result screen saying "nothing was programmed" describe a
// path the app no longer walks.
void view_restore_confirm(const wifi_slot_t *source, const wifi_slot_t *dest,
                          const char *undo_name);

void view_restore_result(bool ok, uint8_t dest_number, const wifi_slot_t *now,
                         const char *undo_name, const char *detail);

void view_no_backups(void);

// Shown instead of the whole write flow when the destination already holds exactly the
// record being restored. The write would program nothing, so three screens spent asking
// for confirmation and then reporting that nothing happened is three screens of "no".
//
// `allow_force` offers to do it anyway. That is a debug build's affordance, gated on
// DSIWIFI_DEBUG in main.c rather than on an #ifdef here, so both variants stay renderable
// by the host harness and the view layer stays free of build configuration.
//
// It is worth having in a Dev build precisely because it is the harmless case: libnds
// compares each page before erasing it, so a matching record programs zero bytes. That
// makes it the one way to exercise writeFirmware end to end with nothing at risk, which
// is how the write path was first proven on hardware.
void view_noop_notice(const wifi_slot_t *dest, bool allow_force);

#endif // SLOT_LIST_VIEW_H
