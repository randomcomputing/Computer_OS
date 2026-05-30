#ifndef IDT_H
#define IDT_H

#include "stdint.h"

void idt_init(void);
void idt_set_user_gate(int num, uint64_t handler);

#endif