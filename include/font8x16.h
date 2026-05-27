#ifndef FONT8X16_H
#define FONT8X16_H

// Embedded IBM VGA 8x16 bitmap font.
//
// 256 glyphs, each 16 scanlines tall and 8 pixels wide. One byte per row,
// with the high bit (0x80) being the leftmost pixel — the native VGA BIOS
// font format. A renderer draws glyph `ch` like this:
//
//     const unsigned char* g = font8x16_glyph(ch);
//     for (int row = 0; row < FONT8X16_HEIGHT; row++)
//         for (int col = 0; col < FONT8X16_WIDTH; col++)
//             if (g[row] & (0x80 >> col)) plot(x + col, y + row);
//
// We carry our own copy so text rendering doesn't depend on grabbing the
// font out of VGA plane 2 — that memory disappears the moment we move to a
// linear-framebuffer video mode.

#define FONT8X16_WIDTH   8
#define FONT8X16_HEIGHT  16
#define FONT8X16_GLYPHS  256

// The raw table: FONT8X16_GLYPHS * FONT8X16_HEIGHT bytes, glyph-major.
extern const unsigned char font8x16[FONT8X16_GLYPHS * FONT8X16_HEIGHT];

// Pointer to the first scanline byte of glyph `ch` (16 bytes follow).
const unsigned char* font8x16_glyph(unsigned char ch);

#endif