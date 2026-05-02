#ifndef PIT_H
#define PIT_H

// Program the PIT to fire IRQ0 at the given frequency (Hz) and install
// the tick handler. Call once at boot.
void pit_init(unsigned int hz);

// Total ticks since boot. Increments `hz` times per second.
unsigned int pit_ticks(void);

// Milliseconds since boot. Convenience wrapper around pit_ticks().
unsigned int pit_millis(void);

// Block for at least `ms` milliseconds.
void pit_sleep(unsigned int ms);

#endif