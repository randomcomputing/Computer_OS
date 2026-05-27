#ifndef FBCON_H
#define FBCON_H

#include "console_backend.h"

// Initialize the framebuffer console against the active VBE mode (set earlier
// via bochs_vbe_set_mode). Clears the screen and returns the backend, or 0 if
// no linear-framebuffer mode is active.
const console_backend_t* fbcon_init(void);

// True once fbcon_init has succeeded.
int fbcon_ready(void);

#endif