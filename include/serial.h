#ifndef SERIAL_H
#define SERIAL_H

int  serial_init(void);
void serial_putc(char c);
void serial_write(const char *s);
int  serial_received(void);
char serial_getc(void);

#endif