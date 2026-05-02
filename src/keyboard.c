#include "keyboard.h"
#include "irq.h"
#include "pic.h"
#include "io.h"
#include "isr.h"

// US layout, set 1. Index = scancode (0..0x3A). 0 means no printable char.
static const char scancode_to_ascii[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8',   // 0x00..0x09
    '9', '0', '-', '=', '\b','\t','q', 'w', 'e', 'r',   // 0x0A..0x13
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,    // 0x14..0x1D (0x1D = LCtrl)
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   // 0x1E..0x27
    '\'','`', 0,   '\\','z', 'x', 'c', 'v', 'b', 'n',   // 0x28..0x31 (0x2A = LShift)
    'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,     // 0x32..0x3B (0x36=RShift,0x38=LAlt,0x39=Space,0x3A=CapsLock)
    // Rest are function keys, keypad, etc. — leave as 0.
};

static const char scancode_to_ascii_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b','\t','Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N',
    'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,
};

// Simple ring buffer for characters.
#define BUF_SIZE 128
static volatile char buf[BUF_SIZE];
static volatile unsigned int buf_head = 0;   // written by IRQ
static volatile unsigned int buf_tail = 0;   // read by consumer

static volatile int shift_down = 0;
static volatile int caps_lock  = 0;
// Set when we see 0xE0; consume the *next* scancode as an extended key.
static volatile int extended    = 0;

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void push_char(char c) {
    unsigned int next = (buf_head + 1) % BUF_SIZE;
    if (next == buf_tail) return;   // buffer full, drop the key
    buf[buf_head] = c;
    buf_head = next;
}

// Translate an extended (0xE0-prefixed) make-code into one of our KEY_*
// constants. Returns 0 if we don't care about it.
static char translate_extended(unsigned char sc) {
    switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x53: return KEY_DELETE;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        default:   return 0;
    }
}

static void keyboard_irq(struct registers* regs) {
    (void)regs;

    unsigned char sc = inb(0x60);

    // The PS/2 controller sends 0xE0 before extended keys (arrows, etc.).
    // We just remember we saw it and consume the next byte as extended.
    if (sc == 0xE0) {
        extended = 1;
        return;
    }

    // Top bit set = key release. We only care about releases for shift
    // tracking; everything else (including extended-key releases) gets
    // dropped on the floor.
    if (sc & 0x80) {
        unsigned char code = sc & 0x7F;
        if (extended) {
            // Drop the extended-key release; we already emitted on press.
            extended = 0;
            return;
        }
        if (code == 0x2A || code == 0x36) shift_down = 0;
        return;
    }

    // Extended-key press: arrows, Home, End, Delete, Page Up/Down.
    if (extended) {
        extended = 0;
        char k = translate_extended(sc);
        if (k) push_char(k);
        return;
    }

    // Regular key press from here on.
    if (sc == 0x2A || sc == 0x36) { shift_down = 1; return; }
    if (sc == 0x3A)               { caps_lock = !caps_lock; return; }

    if (sc >= 128) return;

    char c = shift_down ? scancode_to_ascii_shift[sc] : scancode_to_ascii[sc];
    if (c == 0) return;

    // Caps lock flips case of letters only.
    if (caps_lock && is_letter(c)) {
        if (shift_down) {
            // shift + caps = lowercase for letters
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        } else {
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        }
    }

    push_char(c);
}

void keyboard_init(void) {
    irq_install_handler(1, keyboard_irq);
    pic_enable_irq(1);
}

char keyboard_getchar(void) {
    // Spin with hlt — CPU sleeps until the next interrupt, saving power
    // and, more importantly, not deadlocking on the volatile check.
    while (buf_head == buf_tail) {
        __asm__ volatile ("sti; hlt");
    }
    char c = buf[buf_tail];
    buf_tail = (buf_tail + 1) % BUF_SIZE;
    return c;
}

int keyboard_try_getchar(char* out) {
    if (buf_head == buf_tail) return 0;
    *out = buf[buf_tail];
    buf_tail = (buf_tail + 1) % BUF_SIZE;
    return 1;
}