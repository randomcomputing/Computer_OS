#ifndef E1000_H
#define E1000_H

// Intel e1000 (8086:100E) network driver.
//
// Milestone 1: find the card on PCI, enable bus mastering, map BAR0 MMIO,
//              read CTRL/STATUS, read the MAC address.
// Milestone 2: RX/TX descriptor rings — the card can now DMA raw Ethernet
//              frames in and out. e1000_send() transmits a frame;
//              e1000_receive_poll() pulls a received frame if one is waiting.
//
// Still polled (no IRQ) and no protocol stack yet — these move raw frames.

// Bring up the card and its RX/TX rings. Returns 1 on success, 0 otherwise.
int e1000_init(void);

// Transmit one raw Ethernet frame (already including dst/src MAC + ethertype).
// Returns 1 if queued to the card, 0 if the TX ring is full or len invalid.
int e1000_send(const void* data, unsigned int len);

// Poll for one received frame. Copies up to `max` bytes into `out` and returns
// the frame length, or 0 if nothing has been received since the last poll.
unsigned int e1000_receive_poll(void* out, unsigned int max);

// Copy the card's 6-byte MAC address into mac_out.
void e1000_get_mac(unsigned char mac_out[6]);

#endif