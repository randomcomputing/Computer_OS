#ifndef ARP_H
#define ARP_H

// Minimal ARP (Address Resolution Protocol) over Ethernet.
//
// ARP maps an IPv4 address to the MAC address that owns it on the local link.
// To find a host's MAC we broadcast a "who-has <ip>?" request; the owner
// replies with its MAC. This is the first real protocol on top of the e1000
// raw-frame driver, and resolving the SLIRP gateway (10.0.2.2) is the test
// that proves the whole TX+RX path end to end.
//
// IPv4 addresses here are passed as host-order 32-bit values, e.g.
// 10.0.2.2 == 0x0A000202. The implementation handles network byte order.

// Broadcast an ARP request asking who owns `target_ip` (host byte order).
// Returns 1 if the frame was queued to the card, 0 on failure.
int arp_request(unsigned int target_ip);

// Check for an incoming ARP reply. If one has arrived, writes the sender's
// 6-byte MAC into mac_out and its IP (host order) into *ip_out, and returns 1.
// Returns 0 if no ARP reply is currently waiting. Non-ARP frames are ignored.
int arp_poll_reply(unsigned char mac_out[6], unsigned int* ip_out);

#endif