// SPDX-License-Identifier: CC0-1.0
//
// Text into a 16-bit framebuffer, so the app is not limited to the 32 columns a tile
// console gives it. At 5x8 that is 51 columns by 24 rows on a DS screen.
//
// Deliberately free of libnds, for the same reason wifi_slots.c is: it takes a
// uint16_t * and writes pixels into it, so main.c can hand it 0x06000000 while
// tools/host_slotlist.c hands it a malloc'd buffer and the result is comparable. Nothing
// here touches a hardware register; setting the video mode is main.c's job.

#ifndef FB_RENDER_H
#define FB_RENDER_H

#include <stdint.h>

#include "font_spleen.h"

#define FB_WIDTH    256
#define FB_HEIGHT   192

#define FB_COLS     (FB_WIDTH / FONT_GLYPH_W)   // 51, with 1 pixel to spare
#define FB_ROWS     (FB_HEIGHT / FONT_GLYPH_H)  // 24, exactly

#define FB_PIXELS   (FB_WIDTH * FB_HEIGHT)

// Bit 15 is the DS's opaque flag in 16-bit bitmap mode, not part of the colour: libnds
// spells this out as ARGB16(a,r,g,b) = (a << 15) | ..., while its RGB15/RGB8 macros leave
// it clear. A pixel written without it is fully transparent, so a renderer that is
// otherwise perfect draws a blank screen showing the backdrop.
//
// It is baked into the macro rather than OR-ed in by callers precisely so it cannot be
// forgotten in one place out of twenty. Never build a colour any other way.
#define FB_RGB(r, g, b) \
    ((uint16_t)(0x8000u | ((r) & 31u) | (((g) & 31u) << 5) | (((b) & 31u) << 10)))

// Every byte outside the font's range draws as this, so an SD card holding a file whose
// name has an accent in it cannot walk off the end of the glyph table. See fb_render.c.
#define FB_FALLBACK     '?'

// --- the palette -------------------------------------------------------------------
//
// Chosen to survive the DS storing five bits per channel. A colour pair that looks distinct
// in an RGB888 mock-up can be one step apart on screen: the first draft of this palette had
// a background and a card two steps apart out of 31, which on a TN panel is one dark blob.
//
// ../Cart-Flasher, the one UI here known to read well on a DS screen, uses nothing but
// extremes: 14 to 31 steps between any two surfaces that touch. The cursor bar below is 22
// steps from the background and the dim text is 17 from it, which is in that range.
//
// See DESIGN.md, "Palette and type". Check any new colour after >> 3, not in a mock-up.
#define FB_BG           FB_RGB( 1,  1,  2)
#define FB_TEXT         FB_RGB(30, 30, 31)
#define FB_SECONDARY    FB_RGB(18, 19, 23)
#define FB_ACCENT       FB_RGB( 0, 26, 31)
#define FB_SELECT       FB_RGB( 0, 16, 22)
#define FB_GOOD         FB_RGB(12, 28, 18)
#define FB_WARN         FB_RGB(31, 23,  8)
#define FB_DANGER       FB_RGB(31, 12, 12)

void fb_clear(uint16_t *fb, uint16_t colour);

// A filled rectangle, clipped to the screen, so a caller whose arithmetic is wrong loses
// pixels rather than corrupting memory past the buffer. fb_row_text's tail fill uses it.
void fb_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t colour);

// A one-pixel outline, 2x scaled text, and procedural Wi-Fi and padlock glyphs all lived
// here while the app was drawing cards. The UI is text only, so they are gone rather than
// kept warm: DESIGN.md "P-UI (withdrawn)" records what they were and why they went, and
// git has them if the decision is ever revisited.

// One whole row of text: the string, then the rest of the row painted in `bg`, all the way
// to FB_WIDTH including the spare pixel column. Painting the tail is the point -- a sink
// built on this cannot leave the end of a longer previous line stranded on screen, which
// is the classic way a framebuffer UI rots where a text console would not.
//
// `s` is drawn from column 0 and clipped at FB_COLS. Rows outside 0..FB_ROWS-1 are ignored.
void fb_row_text(uint16_t *fb, int row, uint16_t fg, uint16_t bg, const char *s);

// Text at a character cell, without painting the rest of the row. For anything drawing
// over an area it has already prepared.
void fb_text(uint16_t *fb, int col, int row, uint16_t fg, uint16_t bg, const char *s);

#endif // FB_RENDER_H
