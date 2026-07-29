// SPDX-License-Identifier: CC0-1.0
//
// See fb_render.h.

#include "fb_render.h"

// Halfword stores throughout rather than 32-bit ones. Writing a uint32_t through a
// uint16_t * is undefined behaviour, and the whole point of this file being portable is
// that tools/crosscheck.py can run it under UBSan. A full clear is 49152 stores, which on
// a redraw that only happens on a keypress is not worth trading that away for.
void fb_clear(uint16_t *fb, uint16_t colour)
{
    for (int i = 0; i < FB_PIXELS; i++)
        fb[i] = colour;
}

// The glyph rows for `c`, with anything the font does not cover mapped to FB_FALLBACK.
//
// This is the single guard standing between the renderer and an out-of-bounds read, and it
// belongs here rather than in the callers. SSIDs are already sanitised by wifi_slot_parse()
// -- file names are not. main.c copies whatever readdir() returns, filtering only on a
// leading dot, the extension and the length, so an accented character in a name on the SD
// card reaches this function. `char` is unsigned on ARM EABI, so 0xC3 arrives as 195 and
// an unguarded (c - FONT_FIRST) would read 163 entries past a 95-entry table.
static const uint8_t *glyph_for(unsigned char c)
{
    if (c < FONT_FIRST || c > FONT_LAST)
        c = FB_FALLBACK;

    return &font_spleen[(c - FONT_FIRST) * FONT_GLYPH_H];
}

// One glyph cell, background included, at pixel (x, y). The caller has already checked the
// cell is on screen.
static void draw_cell(uint16_t *fb, int x, int y, uint16_t fg, uint16_t bg, unsigned char c)
{
    const uint8_t *rows = glyph_for(c);

    for (int gy = 0; gy < FONT_GLYPH_H; gy++)
    {
        uint16_t *line = fb + (y + gy) * FB_WIDTH + x;
        uint8_t bits = rows[gy];

        // Bit 7 is the leftmost pixel, as the BDF stores it and tools/make_font.py
        // preserves it. The low 8 - FONT_GLYPH_W bits are unused and verified zero at
        // conversion time.
        for (int gx = 0; gx < FONT_GLYPH_W; gx++)
            line[gx] = (bits & (0x80u >> gx)) ? fg : bg;
    }
}

static void fill_band(uint16_t *fb, int y, int x0, int x1, uint16_t colour)
{
    for (int py = y; py < y + FONT_GLYPH_H; py++)
    {
        uint16_t *line = fb + py * FB_WIDTH;
        for (int px = x0; px < x1; px++)
            line[px] = colour;
    }
}

void fb_text(uint16_t *fb, int col, int row, uint16_t fg, uint16_t bg, const char *s)
{
    if (row < 0 || row >= FB_ROWS)
        return;

    int y = row * FONT_GLYPH_H;

    for (int i = 0; s[i] != '\0'; i++)
    {
        int c = col + i;
        if (c < 0)
            continue;
        if (c >= FB_COLS)
            return;     // clipped, not wrapped: a wrapped row would turn one entry into two

        draw_cell(fb, c * FONT_GLYPH_W, y, fg, bg, (unsigned char)s[i]);
    }
}

void fb_row_text(uint16_t *fb, int row, uint16_t fg, uint16_t bg, const char *s)
{
    if (row < 0 || row >= FB_ROWS)
        return;

    int y = row * FONT_GLYPH_H;
    int drawn = 0;

    for (; s[drawn] != '\0' && drawn < FB_COLS; drawn++)
        draw_cell(fb, drawn * FONT_GLYPH_W, y, fg, bg, (unsigned char)s[drawn]);

    // Everything the text did not cover, out to the true pixel width. FB_COLS * FONT_GLYPH_W
    // is 255, one short of the screen, so painting to FB_WIDTH rather than to the last whole
    // cell is what keeps a one-pixel stripe of the previous screen from surviving down the
    // right edge.
    fill_band(fb, y, drawn * FONT_GLYPH_W, FB_WIDTH, bg);
}

// --- primitives --------------------------------------------------------------------
//
// Everything clips to the screen. A caller whose arithmetic is wrong then loses pixels at
// the edge instead of writing past the buffer, which on the console is somebody else's VRAM
// and on the host is a heap overflow the sanitizers would have to catch after the fact.

static void clip_span(int *pos, int *len, int limit)
{
    if (*pos < 0) { *len += *pos; *pos = 0; }
    if (*pos + *len > limit) *len = limit - *pos;
    if (*len < 0) *len = 0;
}

void fb_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t colour)
{
    clip_span(&x, &w, FB_WIDTH);
    clip_span(&y, &h, FB_HEIGHT);

    for (int py = y; py < y + h; py++)
    {
        uint16_t *line = fb + py * FB_WIDTH;
        for (int px = x; px < x + w; px++)
            line[px] = colour;
    }
}
