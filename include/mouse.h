#ifndef MOUSE_H
#define MOUSE_H

// Initialize the PS/2 mouse: enable the auxiliary device on the PS/2
// controller, put the mouse in stream mode at default rate/resolution,
// and try to enable the IntelliMouse Z-axis (scroll wheel) extension.
// Installs the IRQ12 handler. Call AFTER idt_init() and pic_remap().
void mouse_init(void);

// Polled accessor for the most recent absolute deltas. Mostly useful
// for debugging — the wheel handling drives the scrollback directly
// from inside the IRQ.
int  mouse_dx(void);
int  mouse_dy(void);
int  mouse_buttons(void);

#endif