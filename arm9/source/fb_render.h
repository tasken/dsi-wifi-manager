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

// D-pad arrows at CP437's codepoints. The bitmaps are ours -- Cart-Flasher's come from the
// Linux kernel's font_6x10.c, GPL-2.0, which cannot enter this MIT repo. See
// licenses/README.md.
//
// Below 0x20 on purpose: FAT long names forbid 0x00-0x1F and wifi_slot_parse() substitutes '?'
// for any SSID byte outside 0x20-0x7E, so no file name or network name can render as an arrow.
#define FB_UP           '\x18'
#define FB_DOWN         '\x19'
#define FB_RIGHT        '\x1A'
#define FB_LEFT         '\x1B'

// --- the palette -------------------------------------------------------------------
//
// The DS stores 5 bits per channel, so two colours that look distinct in an RGB888 mock-up can
// be one step apart on screen. Adjacent surfaces here are 14-31 steps apart.
// Check any new colour after >> 3, never in a mock-up. docs/ARCHITECTURE.md, "Rendering".
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
