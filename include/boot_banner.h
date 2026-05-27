#ifndef BOOT_BANNER_H
#define BOOT_BANNER_H

// Reusable boot-screen drawing: the "Computer OS" banner box and colored
// status lines. Factored out of kmain so the same banner/status output can be
// produced twice — once in VGA text during early boot, and again in the
// framebuffer console after the graphics switch.
//
// Colors used by print_status: a green [OK] marker for success, a red [!!]
// marker for failure, with the descriptive text in white. The banner title
// "Computer OS" is drawn in light cyan inside a white box.

// Draw the three-line ### banner box with the cyan title.
void boot_banner(void);

// Print one status line: "[OK] <text>" with a green marker if ok != 0,
// otherwise "[!!] <text>" with a red marker. The text is white. Adds newline.
void print_status(int ok, const char* text);

#endif