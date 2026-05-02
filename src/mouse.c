#include "mouse.h"
#include "io.h"
#include "irq.h"
#include "pic.h"
#include "isr.h"
#include "vga.h"

// PS/2 controller ports.
#define PS2_DATA   0x60   // read responses, write data bytes
#define PS2_STATUS 0x64   // read status
#define PS2_CMD    0x64   // write controller commands

// Status register bits.
#define PS2_OUT_FULL 0x01   // 1 = output buffer has a byte for the CPU
#define PS2_IN_FULL  0x02   // 1 = input buffer is full (don't write yet)
#define PS2_AUX_DATA 0x20   // 1 = byte in 0x60 came from the mouse, not keyboard

// Controller commands (sent to 0x64).
#define CMD_DISABLE_AUX  0xA7
#define CMD_ENABLE_AUX   0xA8
#define CMD_READ_CONFIG  0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_WRITE_AUX    0xD4   // "the next 0x60 byte goes to the mouse"

// Mouse device commands (sent through 0xD4, then 0x60).
#define MOUSE_SET_SAMPLE  0xF3
#define MOUSE_GET_ID      0xF2
#define MOUSE_ENABLE      0xF4
#define MOUSE_SET_DEFAULTS 0xF6
#define MOUSE_RESET       0xFF
#define MOUSE_ACK         0xFA

// State.
static volatile int mx = 0, my = 0, mbtn = 0;
static int has_wheel = 0;             // True after IntelliMouse magic knock succeeded.
static int packet_size = 3;           // 3 normally, 4 with wheel.

// Packet assembly. Stream mode sends N bytes per movement event; we
// accumulate them and dispatch once a full packet is in.
static unsigned char pkt[4];
static int pkt_idx = 0;

// ---- low-level helpers ----

// Wait until we can WRITE a byte (input buffer empty).
static int wait_input_clear(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS) & PS2_IN_FULL) == 0) return 1;
    }
    return 0;
}

// Wait until there's a byte to READ.
static int wait_output_full(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & PS2_OUT_FULL) return 1;
    }
    return 0;
}

// Send one byte to the mouse (auxiliary device).
static int mouse_write(unsigned char b) {
    if (!wait_input_clear()) return 0;
    outb(PS2_CMD, CMD_WRITE_AUX);
    if (!wait_input_clear()) return 0;
    outb(PS2_DATA, b);
    return 1;
}

// Read one byte from the mouse, blocking briefly. Used only at init
// time, before we've enabled IRQs on the device.
static int mouse_read(unsigned char* out) {
    if (!wait_output_full()) return 0;
    *out = inb(PS2_DATA);
    return 1;
}

// Send a command and check that the mouse acked.
static int mouse_cmd(unsigned char b) {
    if (!mouse_write(b)) return 0;
    unsigned char r;
    if (!mouse_read(&r)) return 0;
    return r == MOUSE_ACK;
}

// Set sample rate (used for both real config and the IntelliMouse knock).
static int mouse_set_sample_rate(unsigned char rate) {
    if (!mouse_cmd(MOUSE_SET_SAMPLE)) return 0;
    if (!mouse_cmd(rate))             return 0;
    return 1;
}

// Try the magic knock that flips a standard PS/2 mouse into IntelliMouse
// mode (3-byte packets become 4-byte packets, with byte 4 being the
// scroll wheel delta). Sequence is sample-rate 200, 100, 80, then
// query device ID — if it answers 3, we have a wheel.
static int try_enable_wheel(void) {
    if (!mouse_set_sample_rate(200)) return 0;
    if (!mouse_set_sample_rate(100)) return 0;
    if (!mouse_set_sample_rate(80))  return 0;

    if (!mouse_cmd(MOUSE_GET_ID)) return 0;
    unsigned char id;
    if (!mouse_read(&id)) return 0;
    return id == 3;
}

// ---- IRQ handler ----

static void mouse_irq(struct registers* regs) {
    (void)regs;

    // The status bit tells us whether this byte is really from the mouse.
    // If it isn't, drop it — IRQ12 should only fire for mouse data, but
    // checking is cheap and prevents us from desyncing the keyboard.
    unsigned char status = inb(PS2_STATUS);
    if ((status & PS2_OUT_FULL) == 0)   return;
    if ((status & PS2_AUX_DATA) == 0)   { (void)inb(PS2_DATA); return; }

    unsigned char b = inb(PS2_DATA);

    // Sync: byte 0 always has bit 3 set. If we're at index 0 and that
    // bit isn't set, we're out of phase with the stream — discard this
    // byte and try again on the next IRQ.
    if (pkt_idx == 0 && (b & 0x08) == 0) {
        return;
    }

    pkt[pkt_idx++] = b;
    if (pkt_idx < packet_size) return;
    pkt_idx = 0;

    // Decode the packet.
    unsigned char flags = pkt[0];
    int dx = (int)(signed char)pkt[1];
    int dy = (int)(signed char)pkt[2];

    // Discard moves where the controller flagged X or Y overflow.
    if (flags & 0x40) dx = 0;       // X overflow
    if (flags & 0x80) dy = 0;       // Y overflow

    mbtn = flags & 0x07;
    mx  += dx;
    my  -= dy;   // mouse Y is inverted vs. screen Y

    // Wheel byte. The low nibble is a signed 4-bit delta on standard
    // IntelliMouse: positive = wheel down, negative = wheel up.
    if (has_wheel) {
        signed char wheel = (signed char)(pkt[3] & 0x0F);
        if (wheel & 0x08) wheel |= 0xF0;        // sign-extend the 4-bit value

        if (wheel != 0) {
            // Each detent on a real wheel is 1 unit. macOS trackpad
            // two-finger scroll usually delivers deltas in the same
            // range; bigger gestures pile on more. Map +N → scroll
            // DOWN by N rows (toward newer), -N → UP by N rows. Cap
            // per-event so a stray big delta can't fly all the way.
            int rows = wheel;
            if (rows > 5)  rows = 5;
            if (rows < -5) rows = -5;
            if (rows > 0) {
                vga_scroll_down(rows);
            } else {
                vga_scroll_up(-rows);
            }
        }
    }
}

// ---- init ----

// Drain anything sitting in the controller's output buffer so we start clean.
static void drain_output(void) {
    for (int i = 0; i < 32; i++) {
        if ((inb(PS2_STATUS) & PS2_OUT_FULL) == 0) return;
        (void)inb(PS2_DATA);
    }
}

void mouse_init(void) {
    // ---- 1. Quiesce both PS/2 ports during init ----
    // If we're booting after a warm reboot, the mouse is probably still
    // in stream mode from the previous run, chattering away. Disable
    // both ports so nothing fires IRQs while we configure the
    // controller, then drain whatever's in the buffer.
    wait_input_clear();
    outb(PS2_CMD, 0xAD);              // disable keyboard port
    wait_input_clear();
    outb(PS2_CMD, CMD_DISABLE_AUX);   // disable aux (mouse) port
    drain_output();

    // ---- 2. Configure the controller ----
    // Read config, set "IRQ12 on" + "mouse clock on", but clear "IRQ1
    // on" (we'll re-enable it last) so no keyboard IRQ fires during
    // mouse setup either.
    wait_input_clear();
    outb(PS2_CMD, CMD_READ_CONFIG);
    unsigned char cfg = 0;
    if (wait_output_full()) cfg = inb(PS2_DATA);
    cfg |=  (1 << 1);        // enable IRQ12 (aux interrupt)
    cfg &= ~(1 << 5);        // enable mouse clock
    wait_input_clear();
    outb(PS2_CMD, CMD_WRITE_CONFIG);
    wait_input_clear();
    outb(PS2_DATA, cfg);

    // ---- 3. Re-enable aux port and reset the mouse ----
    wait_input_clear();
    outb(PS2_CMD, CMD_ENABLE_AUX);
    drain_output();

    // Send 0xFF to RESET the mouse. This is critical for warm-reboot
    // recovery: the mouse may have been in IntelliMouse 4-byte mode and
    // mid-packet from the previous boot. After reset it returns to the
    // factory cold-boot state (3-byte packets, stream disabled).
    //
    // The reset response is three bytes:
    //   ACK (0xFA), then BAT result (0xAA = pass), then device ID (0x00).
    // We accept either order on the BAT/ID since some controllers swap.
    if (mouse_write(MOUSE_RESET)) {
        unsigned char r;
        // ACK
        if (mouse_read(&r) && r == MOUSE_ACK) {
            // BAT result + device id (read both, don't care about order).
            (void)mouse_read(&r);
            (void)mouse_read(&r);
        }
    }
    drain_output();

    // ---- 4. Configure the mouse itself ----
    if (!mouse_cmd(MOUSE_SET_DEFAULTS)) {
        // Mouse didn't ack; re-enable keyboard and bail. The OS still
        // works, you just have no mouse this boot.
        wait_input_clear();
        outb(PS2_CMD, 0xAE);          // re-enable keyboard port
        return;
    }

    // Try the IntelliMouse magic knock for scroll-wheel support.
    has_wheel = try_enable_wheel();
    packet_size = has_wheel ? 4 : 3;

    if (!mouse_cmd(MOUSE_ENABLE)) {
        wait_input_clear();
        outb(PS2_CMD, 0xAE);
        return;
    }

    // Reset packet assembly — drain anything that may have arrived
    // during init, and start fresh on the next IRQ.
    pkt_idx = 0;
    drain_output();

    // ---- 5. Re-enable keyboard port and turn on bit 0 of the config ----
    // We didn't touch the keyboard-IRQ enable bit, but the keyboard
    // port itself is currently disabled. Turn it back on.
    wait_input_clear();
    outb(PS2_CMD, 0xAE);

    // ---- 6. Hook up the IRQ. ----
    // IRQ12 is on the slave PIC, so we also need IRQ2 unmasked on the
    // master for cascade signals to get through.
    irq_install_handler(12, mouse_irq);
    pic_enable_irq(2);
    pic_enable_irq(12);
}

int mouse_dx(void)      { return mx;   }
int mouse_dy(void)      { return my;   }
int mouse_buttons(void) { return mbtn; }