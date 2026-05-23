#ifndef GFX_H
#define GFX_H

// VGA mode 13h driver: 320x200, 256 colours, linear framebuffer at 0xA0000.
//
// "Mode 13h" is the classic DOS-era graphics mode. Each pixel is one byte
// that indexes into a 256-entry palette (the DAC), so the framebuffer is
// exactly 320*200 = 64000 bytes living at physical 0xA0000. That address
// is inside the first 4 MB, which entry.asm already identity-maps as a
// single 4 MB page, so we can write to it directly with no extra paging.
//
// The usual way to enter mode 13h is the BIOS call int 0x10 / AX=0x13,
// but we're already in 32-bit protected mode by the time the kernel runs,
// so the BIOS is no longer reachable. Instead we set the VGA's hardware
// registers by hand — the same values the BIOS would have written. The
// register dump below is the well-known mode 13h table.
//
// NOTE: this takes over the screen. The text driver (vga.c) writes to
// 0xB8000 and assumes text mode; once you're in graphics mode, text-mode
// output goes nowhere useful. Call gfx_set_text_mode() to get the console
// back (it restores an 80x25 text-mode register set and clears 0xB8000).

#define GFX_WIDTH   320
#define GFX_HEIGHT  200

// Switch the VGA hardware into 320x200x256 graphics mode.
void gfx_init(void);

// Restore standard 80x25 text mode (so the shell/console works again).
void gfx_set_text_mode(void);

// Plot one pixel. (x,y) outside the screen is ignored. `color` is a
// palette index 0..255. With the default mode 13h palette, a handful of
// useful indices: 0=black, 15=white, 4=red, 2=green, 1=blue, 14=yellow.
void gfx_putpixel(int x, int y, unsigned char color);

// Read back a pixel's palette index (0 if out of range).
unsigned char gfx_getpixel(int x, int y);

// Fill the whole screen with one colour.
void gfx_clear(unsigned char color);

// Filled rectangle. Clipped to the screen.
void gfx_fill_rect(int x, int y, int w, int h, unsigned char color);

// Line from (x0,y0) to (x1,y1) via Bresenham. Any slope. Clipped.
void gfx_line(int x0, int y0, int x1, int y1, unsigned char color);

// Outlined (unfilled) rectangle. Clipped.
void gfx_draw_rect(int x, int y, int w, int h, unsigned char color);

// Interactive demo: move the mouse to move a cursor, hold left button to
// paint, press any key to exit back to text mode.
void gfx_mouse_demo(void);

// ---- text in graphics mode ----
// Draw text using the VGA bitmap font (captured on gfx_init). `bg` >= 0
// fills the character cell background; bg = -1 means transparent.
// 8x16 glyphs. Only valid after gfx_init() has run at least once.
void gfx_char(int x, int y, char ch, unsigned char fg, int bg);
void gfx_text(int x, int y, const char* s, unsigned char fg, int bg);

// Interactive paint program: toolbar of colours + CLEAR/QUIT, canvas you
// draw on with the left button (right button erases). Key or QUIT exits.
void gfx_paint(void);

#endif