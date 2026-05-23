#include "gfx.h"
#include "io.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"

// Linear framebuffer for mode 13h. One byte per pixel, row-major.
#define GFX_FRAMEBUFFER ((volatile unsigned char*)0xA0000)

// --- VGA register port addresses ---------------------------------------
// The VGA is programmed through several register groups, each reached via
// an index port (you write which sub-register you want) and a data port
// (you write/read its value). Same index/data dance your text cursor code
// already does with the CRTC at 0x3D4/0x3D5.
#define VGA_MISC_WRITE   0x3C2
#define VGA_SEQ_INDEX    0x3C4
#define VGA_SEQ_DATA     0x3C5
#define VGA_GC_INDEX     0x3CE
#define VGA_GC_DATA      0x3CF
#define VGA_CRTC_INDEX   0x3D4
#define VGA_CRTC_DATA    0x3D5
#define VGA_AC_INDEX     0x3C0   // attribute controller (quirky, see below)
#define VGA_AC_WRITE     0x3C0
#define VGA_INSTAT_READ  0x3DA   // reading this resets the AC flip-flop

// DAC palette ports. The palette is 256 entries of (R,G,B), each channel
// 6-bit (0..63). To read: write the start index to 0x3C7, then read R,G,B
// triples from 0x3C9. To write: write the start index to 0x3C8, then write
// R,G,B triples to 0x3C9. The index auto-increments after each triple.
#define VGA_DAC_READ_IDX   0x3C7
#define VGA_DAC_WRITE_IDX  0x3C8
#define VGA_DAC_DATA       0x3C9


// --- Register dumps -----------------------------------------------------
// These are the standard register values the BIOS programs for each mode.
// Order within each group matches the index it's written to (0, 1, 2, ...).
// You don't need to memorise these; they're a known-good table.

// Mode 13h: 320x200x256.
static const unsigned char mode13h_seq[5] = {
    0x03, 0x01, 0x0F, 0x00, 0x0E
};
static const unsigned char mode13h_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF
};
static const unsigned char mode13h_gc[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF
};
static const unsigned char mode13h_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};
static const unsigned char mode13h_misc = 0x63;

// Standard 80x25 text mode (mode 03h), used to restore the console.
static const unsigned char text_seq[5] = {
    0x03, 0x00, 0x03, 0x00, 0x02
};
static const unsigned char text_crtc[25] = {
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF
};
static const unsigned char text_gc[9] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
};
static const unsigned char text_ac[21] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x0C, 0x00, 0x0F, 0x08, 0x00
};
static const unsigned char text_misc = 0x67;

// Push one full register set into the hardware.
static void write_regs(unsigned char misc,
                       const unsigned char* seq,
                       const unsigned char* crtc,
                       const unsigned char* gc,
                       const unsigned char* ac) {
    // Miscellaneous output register: picks clock + I/O address (0x3Dx).
    outb(VGA_MISC_WRITE, misc);

    // Sequencer (5 registers). Reg 0 is the reset register.
    for (unsigned char i = 0; i < 5; i++) {
        outb(VGA_SEQ_INDEX, i);
        outb(VGA_SEQ_DATA, seq[i]);
    }

    // CRTC is normally write-protected by bit 7 of index 0x11. Clear that
    // protect bit first, or our writes to indices 0..7 silently no-op.
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, inb(VGA_CRTC_DATA) & 0x7F);

    // CRTC (25 registers): horizontal/vertical timing, scan lines, etc.
    for (unsigned char i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, i);
        outb(VGA_CRTC_DATA, crtc[i]);
    }

    // Graphics controller (9 registers).
    for (unsigned char i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, i);
        outb(VGA_GC_DATA, gc[i]);
    }

    // Attribute controller (21 registers). This one is weird: the SAME
    // port 0x3C0 is both index and data, toggled by an internal flip-flop.
    // Reading 0x3DA resets the flip-flop to "index" state. After all 21,
    // we write 0x20 to 0x3C0 to re-enable video output (without it the
    // screen stays blank).
    for (unsigned char i = 0; i < 21; i++) {
        (void)inb(VGA_INSTAT_READ);      // reset flip-flop -> index mode
        outb(VGA_AC_INDEX, i);
        outb(VGA_AC_WRITE, ac[i]);
    }
    (void)inb(VGA_INSTAT_READ);
    outb(VGA_AC_INDEX, 0x20);            // lock palette, enable display
}

// --- font preservation -------------------------------------------------
// The text-mode character generator (the bitmap font) lives in VGA plane 2
// at 0xA0000 when the card is set up for font access. Graphics mode reuses
// that same physical memory for pixels, so entering mode 13h destroys the
// font. The BIOS would normally reload it on a mode switch, but we can't
// call the BIOS from protected mode. Instead we SAVE the font out of plane
// 2 before switching to graphics, and RESTORE it on the way back — no
// hand-coded font table needed, we just preserve whatever the bootloader
// already loaded.
//
// Each of 256 glyphs occupies 32 bytes of slack in plane 2 (only the first
// 16 are used for an 8x16 font), so the font region is 256*32 = 8192 bytes.
#define FONT_BYTES 8192
static unsigned char saved_font[FONT_BYTES];
static int font_saved = 0;

// The DAC palette for the standard text-mode 16 colours. Each entry is
// (R,G,B) with 6-bit channels (0..63), matching the VGA hardware. This is
// the well-known IBM/VGA default palette for indices 0..15 — the same
// colours your enum vga_color names (0=black .. 15=white). We write these
// back on exit from graphics mode rather than trying to read the DAC,
// which is finicky on some hardware/emulators.
static const unsigned char text_palette16[16][3] = {
    {  0,  0,  0 },   //  0 black
    {  0,  0, 42 },   //  1 blue
    {  0, 42,  0 },   //  2 green
    {  0, 42, 42 },   //  3 cyan
    { 42,  0,  0 },   //  4 red
    { 42,  0, 42 },   //  5 magenta
    { 42, 21,  0 },   //  6 brown
    { 42, 42, 42 },   //  7 light grey
    { 21, 21, 21 },   //  8 dark grey
    { 21, 21, 63 },   //  9 light blue
    { 21, 63, 21 },   // 10 light green
    { 21, 63, 63 },   // 11 light cyan
    { 63, 21, 21 },   // 12 light red
    { 63, 21, 63 },   // 13 light magenta
    { 63, 63, 21 },   // 14 yellow
    { 63, 63, 63 },   // 15 white
};

// Restore the 16 text-mode colours into DAC entries 0..15.
// Write the standard 16 colours into DAC entries 0..15. Used both on
// entry to graphics mode (so index 15 is really white, etc. while drawing)
// and on exit (so the text console's colours look normal again).
static void write_palette16(void) {
    outb(VGA_DAC_WRITE_IDX, 0);           // start writing at index 0
    io_wait();
    for (int i = 0; i < 16; i++) {
        outb(VGA_DAC_DATA, text_palette16[i][0]);
        outb(VGA_DAC_DATA, text_palette16[i][1]);
        outb(VGA_DAC_DATA, text_palette16[i][2]);
    }
}

// Put the VGA into the special state where plane 2 is linearly readable/
// writable at 0xA0000 (sequencer + graphics-controller gymnastics), run
// `copy`, then restore normal operation. `to_ram` selects direction.
static void access_font(int to_ram) {
    // --- set up plane-2 linear access ---
    // Sequencer: disable odd/even, select plane 2 for writes.
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x04);   // map mask = plane 2
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x07);   // sequential, no odd/even
    // Graphics controller: read plane 2, no odd/even, map at 0xA0000.
    outb(VGA_GC_INDEX, 0x04); outb(VGA_GC_DATA, 0x02);     // read map = plane 2
    outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x00);     // mode 0, no odd/even
    outb(VGA_GC_INDEX, 0x06); outb(VGA_GC_DATA, 0x04);     // map at A0000, graphics off

    volatile unsigned char* plane2 = (volatile unsigned char*)0xA0000;
    if (to_ram) {
        for (int i = 0; i < FONT_BYTES; i++) saved_font[i] = plane2[i];
    } else {
        for (int i = 0; i < FONT_BYTES; i++) plane2[i] = saved_font[i];
    }

    // --- restore normal text-mode plane setup ---
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x03);   // map mask = planes 0,1
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x03);   // odd/even on
    outb(VGA_GC_INDEX, 0x04); outb(VGA_GC_DATA, 0x00);     // read map = plane 0
    outb(VGA_GC_INDEX, 0x05); outb(VGA_GC_DATA, 0x10);     // odd/even mode
    outb(VGA_GC_INDEX, 0x06); outb(VGA_GC_DATA, 0x0E);     // map at B8000, text
}

void gfx_init(void) {
    // Preserve the font once, before the first mode switch wipes it, so the
    // text console's glyphs come back on exit. (Colours are handled on exit
    // by writing the known text palette, so no palette save is needed here.)
    if (!font_saved) { access_font(1); font_saved = 1; }
    write_regs(mode13h_misc, mode13h_seq, mode13h_crtc, mode13h_gc, mode13h_ac);
    write_palette16();   // make indices 0..15 the standard colours
    gfx_clear(0);
}

void gfx_set_text_mode(void) {
    write_regs(text_misc, text_seq, text_crtc, text_gc, text_ac);
    // Reload the font we saved before going graphical, so glyphs render.
    if (font_saved) access_font(0);
    // Restore the standard 16 text colours so the prompt/text look normal.
    write_palette16();
    // Blank the text framebuffer so we don't show leftover graphics bytes
    // reinterpreted as character/attribute pairs.
    volatile unsigned short* text = (volatile unsigned short*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) text[i] = 0x0720;  // space, grey on black
}

void gfx_putpixel(int x, int y, unsigned char color) {
    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return;
    GFX_FRAMEBUFFER[y * GFX_WIDTH + x] = color;
}

unsigned char gfx_getpixel(int x, int y) {
    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return 0;
    return GFX_FRAMEBUFFER[y * GFX_WIDTH + x];
}

void gfx_clear(unsigned char color) {
    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) {
        GFX_FRAMEBUFFER[i] = color;
    }
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned char color) {
    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= GFX_HEIGHT) continue;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < 0 || px >= GFX_WIDTH) continue;
            GFX_FRAMEBUFFER[py * GFX_WIDTH + px] = color;
        }
    }
}
// --- line drawing (Bresenham) ------------------------------------------
// Draw a line from (x0,y0) to (x1,y1). Bresenham's algorithm steps one
// pixel along the "major" axis (whichever of dx/dy is larger) and uses an
// integer error term to decide when to also step the minor axis — so no
// floating point, and it works for any slope, not just 45-degree diagonals.
void gfx_line(int x0, int y0, int x1, int y1, unsigned char color) {
    int dx =  (x1 - x0); if (dx < 0) dx = -dx;
    int dy = -(y1 - y0); if (dy > 0) dy = -dy;   // dy kept negative (see err)
    int sx = (x0 < x1) ? 1 : -1;                 // x step direction
    int sy = (y0 < y1) ? 1 : -1;                 // y step direction
    int err = dx + dy;                           // error accumulator

    for (;;) {
        gfx_putpixel(x0, y0, color);             // putpixel clips for us
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }   // time to step x
        if (e2 <= dx) { err += dx; y0 += sy; }   // time to step y
    }
}

// Outlined rectangle (filled version already exists as gfx_fill_rect).
// Drawn as four lines; corners are shared, which is fine.
void gfx_draw_rect(int x, int y, int w, int h, unsigned char color) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w - 1, y2 = y + h - 1;
    gfx_line(x,  y,  x2, y,  color);   // top
    gfx_line(x,  y2, x2, y2, color);   // bottom
    gfx_line(x,  y,  x,  y2, color);   // left
    gfx_line(x2, y,  x2, y2, color);   // right
}

// --- interactive mouse cursor demo -------------------------------------
// A 11x11 arrow cursor. 0 = transparent (show background), 15 = white
// body, 1-ish outline. We keep it simple: a classic top-left arrow.
// Each byte is a palette index, 0 meaning "don't draw this pixel".
#define CUR_W 11
#define CUR_H 11
static const unsigned char cursor_sprite[CUR_H][CUR_W] = {
    {15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {15,15, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {15,15,15, 0, 0, 0, 0, 0, 0, 0, 0},
    {15,15,15,15, 0, 0, 0, 0, 0, 0, 0},
    {15,15,15,15,15, 0, 0, 0, 0, 0, 0},
    {15,15,15,15,15,15, 0, 0, 0, 0, 0},
    {15,15,15,15,15,15,15, 0, 0, 0, 0},
    {15,15,15,15,15,15,15,15, 0, 0, 0},
    {15,15,15,15,15,15,15,15,15, 0, 0},
    {15,15,15,15, 0,15,15, 0, 0, 0, 0},
    {15, 0, 0, 0, 0, 0,15,15, 0, 0, 0},
};

// Backing store for the pixels under the cursor, so we can erase it
// cleanly each frame without leaving a trail.
static unsigned char under_cursor[CUR_H][CUR_W];

// Save the screen pixels the cursor is about to cover.
static void cursor_save(int cx, int cy) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            under_cursor[j][i] = gfx_getpixel(cx + i, cy + j);
}

// Restore them (erase the cursor).
static void cursor_restore(int cx, int cy) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            gfx_putpixel(cx + i, cy + j, under_cursor[j][i]);
}

// Draw the cursor sprite (skipping transparent pixels).
static void cursor_draw(int cx, int cy) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            if (cursor_sprite[j][i])
                gfx_putpixel(cx + i, cy + j, cursor_sprite[j][i]);
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Run the interactive demo. Move the mouse to move the cursor; hold the
// left button to paint; press any key to quit back to text mode.
// Returns when a key is pressed.
void gfx_mouse_demo(void) {
    gfx_init();
    gfx_clear(0);

    // A little instructional scaffolding drawn with our new primitives:
    // a border and an X, so there's something on screen besides the cursor.
    gfx_draw_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, 7);
    gfx_line(0, 0, GFX_WIDTH - 1, GFX_HEIGHT - 1, 8);
    gfx_line(GFX_WIDTH - 1, 0, 0, GFX_HEIGHT - 1, 8);
    // On-screen instructions, drawn with the bitmap font.
    gfx_text(8, 6,  "Move mouse to move cursor.", 15, -1);
    gfx_text(8, 24, "Hold LEFT button to paint.", 14, -1);
    gfx_text(8, 42, "Press any key to exit.",     11, -1);

    // Start the cursor at wherever the mouse currently reports (clamped).
    // mx/my accumulate from boot, so this may not be screen-centre — that's
    // fine, the cursor simply begins at the live position.
    int px = clampi(mouse_dx(), 0, GFX_WIDTH  - CUR_W);
    int py = clampi(mouse_dy(), 0, GFX_HEIGHT - CUR_H);
    cursor_save(px, py);
    cursor_draw(px, py);

    char dummy;
    for (;;) {
        // Quit on any keypress.
        if (keyboard_try_getchar(&dummy)) break;

        // mouse_dx/dy are accumulated absolute-ish coordinates. Clamp to
        // the screen (leaving room for the sprite).
        int nx = clampi(mouse_dx(), 0, GFX_WIDTH  - CUR_W);
        int ny = clampi(mouse_dy(), 0, GFX_HEIGHT - CUR_H);

        // Paint with the left button (bit 0). We draw a small dot at the
        // cursor hotspot (top-left) under the restored background, so the
        // paint persists when the cursor moves on.
        int painting = (mouse_buttons() & 0x01);

        if (nx != px || ny != py || painting) {
            cursor_restore(px, py);            // erase old cursor
            if (painting) gfx_fill_rect(px, py, 2, 2, 12);  // leave paint
            px = nx; py = ny;
            cursor_save(px, py);               // remember new background
            cursor_draw(px, py);               // draw cursor at new spot
        }

        pit_sleep(8);   // ~120 Hz poll; keeps the CPU from spinning hot
    }

    gfx_set_text_mode();
}

// --- text rendering in graphics mode -----------------------------------
// We reuse the VGA bitmap font we already copied into saved_font during
// gfx_init. The font is 256 glyphs of 32 bytes each (an 8x16 cell uses 16
// bytes — one byte per scanline — and 16 bytes are padding). Within a
// scanline byte, bit 7 is the leftmost pixel.
//
// IMPORTANT: these only work after gfx_init() has run at least once, since
// that's when the font gets captured. In practice you're always in
// graphics mode when you call them, so that's fine.
#define GLYPH_STRIDE 32     // bytes per glyph in saved_font
#define GLYPH_W       8
#define GLYPH_H      16

// Draw one character at pixel (x,y). `fg` is the foreground colour. If
// `bg` is >= 0 it fills the 8x16 cell background with that colour first;
// pass bg = -1 for a transparent background (only foreground pixels drawn).
void gfx_char(int x, int y, char ch, unsigned char fg, int bg) {
    const unsigned char* glyph = &saved_font[(unsigned char)ch * GLYPH_STRIDE];
    for (int row = 0; row < GLYPH_H; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < GLYPH_W; col++) {
            if (bits & (0x80 >> col)) {
                gfx_putpixel(x + col, y + row, fg);
            } else if (bg >= 0) {
                gfx_putpixel(x + col, y + row, (unsigned char)bg);
            }
        }
    }
}

// Draw a null-terminated string starting at (x,y). Advances 8 px per
// character. Handles '\n' (next line, back to starting x) so multi-line
// strings work. Returns nothing — clipping is handled per-pixel.
void gfx_text(int x, int y, const char* s, unsigned char fg, int bg) {
    int cx = x, cy = y;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; cy += GLYPH_H; continue; }
        gfx_char(cx, cy, *s, fg, bg);
        cx += GLYPH_W;
    }
}