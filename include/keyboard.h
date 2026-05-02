#ifndef KEYBOARD_H
#define KEYBOARD_H

// Special key codes returned by keyboard_getchar() for non-printable
// keys. They sit above 0x7F so they can never collide with a real ASCII
// character. Cast to unsigned char when comparing.
#define KEY_UP        0x80
#define KEY_DOWN      0x81
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83
#define KEY_HOME      0x84
#define KEY_END       0x85
#define KEY_DELETE    0x86
#define KEY_PGUP      0x87
#define KEY_PGDN      0x88

// Install the IRQ1 handler. Call after idt_init() and pic_remap().
void keyboard_init(void);

// Block until a character is available, then return it. Supports shift
// and caps lock for ASCII letters/symbols. Returns one of the KEY_*
// constants above for arrows and other navigation keys.
char keyboard_getchar(void);

// Non-blocking variant. If a character is available, write it to *out
// and return 1. Otherwise return 0 immediately. Used by sys_read so a
// blocked user task can yield instead of monopolizing the CPU.
int keyboard_try_getchar(char* out);

#endif