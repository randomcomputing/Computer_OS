#include "pit.h"
#include "io.h"
#include "irq.h"
#include "pic.h"
#include "isr.h"
#include "task.h"

// PIT ports
#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

// The PIT's base frequency in Hz.
#define PIT_BASE_HZ   1193182

// Command byte: channel 0, lobyte+hibyte access, rate generator, binary.
//   bits 7-6 = 00 (channel 0)
//   bits 5-4 = 11 (access mode: lobyte then hibyte)
//   bits 3-1 = 010 (mode 2: rate generator)
//   bit  0   = 0  (binary)
#define PIT_CMD_MODE2  0x34

static volatile unsigned int tick_count = 0;
static unsigned int          ticks_per_sec = 0;
static unsigned int          ms_per_tick = 0;

static void pit_irq(struct registers* regs) {
    (void)regs;
    tick_count++;
    scheduler_tick();   // no-op until tasking_enable_preemption()
}

void pit_init(unsigned int hz) {
    ticks_per_sec = hz;
    ms_per_tick   = (hz == 0) ? 0 : (1000u / hz);

    unsigned int divisor = PIT_BASE_HZ / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;   // PIT is 16-bit
    if (divisor < 1)      divisor = 1;

    outb(PIT_COMMAND, PIT_CMD_MODE2);
    outb(PIT_CHANNEL0, (unsigned char)(divisor & 0xFF));         // low byte
    outb(PIT_CHANNEL0, (unsigned char)((divisor >> 8) & 0xFF));  // high byte

    irq_install_handler(0, pit_irq);
    pic_enable_irq(0);
}

unsigned int pit_ticks(void) {
    return tick_count;
}

unsigned int pit_millis(void) {
    // All 32-bit math (no libgcc __udivdi3 available under -nostdlib).
    // Valid whenever ticks_per_sec evenly divides 1000 — true for 100, 1000,
    // 500, 250, 200, 125, 100, 50, etc. We call pit_init(100) which gives
    // ms_per_tick = 10, so this is just tick_count * 10.
    return tick_count * ms_per_tick;
}

void pit_sleep(unsigned int ms) {
    unsigned int target = pit_millis() + ms;
    // sti+hlt so the CPU sleeps between interrupts instead of spinning hot.
    while (pit_millis() < target) {
        __asm__ volatile ("sti; hlt");
    }
}