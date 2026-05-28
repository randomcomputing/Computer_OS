#ifndef TCP_H
#define TCP_H

// Minimal happy-path TCP client (Option A).
//
// Implements the client side only: three-way handshake, ordered send/receive,
// and FIN close. No retransmission, no out-of-order reassembly, no congestion
// control — it assumes the in-order, lossless delivery that QEMU's SLIRP
// provides on localhost. Polled, blocking, one connection at a time. This is
// enough to speak HTTP to a real server through the gateway NAT.

// Opaque-ish connection state. Callers treat it as a handle.
typedef struct tcp_conn tcp_conn_t;

// Open a connection to dst_ip (host order) : dst_port. Performs the SYN/
// SYN-ACK/ACK handshake. Returns a connection handle on success, 0 on failure
// (timeout, refused). The handle is a pointer into a static slot — one
// connection at a time for now.
tcp_conn_t* tcp_connect(unsigned int dst_ip, unsigned short dst_port);

// Send `len` bytes on the connection. Returns bytes queued (== len on success)
// or -1 on error. Blocks until the peer ACKs (happy path).
int tcp_send(tcp_conn_t* c, const void* data, unsigned int len);

// Receive up to `max` bytes into `out`, waiting up to timeout_ms for data.
// Returns bytes received (>0), 0 on timeout/no-data, or -1 if the peer closed
// the connection (FIN received and drained).
int tcp_recv(tcp_conn_t* c, void* out, unsigned int max, unsigned int timeout_ms);

// Close the connection (send FIN, drain). Safe to call once per handle.
void tcp_close(tcp_conn_t* c);

#endif