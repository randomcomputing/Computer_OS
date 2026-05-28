#include "tcp.h"
#include "net.h"
#include "net_internal.h"
#include "e1000.h"
#include "string.h"
#include "pit.h"
#include "printf.h"

static unsigned short htons(unsigned short v) { return (unsigned short)((v << 8) | (v >> 8)); }
static unsigned short ntohs(unsigned short v) { return htons(v); }
static unsigned int   ntohl(unsigned int v) {
    return ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) |
           (((v >> 16) & 0xFF) << 8) | ((v >> 24) & 0xFF);
}
static unsigned int   htonl(unsigned int v) { return ntohl(v); }

// TCP header (20 bytes, no options). Network byte order on the wire.
typedef struct {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned int   seq;
    unsigned int   ack;
    unsigned char  data_off;    // high 4 bits = header length in 32-bit words
    unsigned char  flags;       // FIN SYN RST PSH ACK URG (low 6 bits)
    unsigned short window;
    unsigned short checksum;
    unsigned short urgent;
} __attribute__((packed)) tcp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

struct tcp_conn {
    int            used;
    unsigned int   dst_ip;       // host order
    unsigned short dst_port;     // host order
    unsigned short src_port;     // host order
    unsigned int   snd_nxt;      // next sequence number we'll send (host order)
    unsigned int   rcv_nxt;      // next sequence number we expect (host order)
    unsigned char  gw_mac[6];
    int            peer_fin;     // peer has sent FIN
};

static struct tcp_conn g_conn;   // single connection slot for now

// Pseudo-header used only for the TCP checksum (not transmitted).
typedef struct {
    unsigned int   src_ip;
    unsigned int   dst_ip;
    unsigned char  zero;
    unsigned char  protocol;
    unsigned short tcp_len;
} __attribute__((packed)) pseudo_hdr_t;

// Compute the TCP checksum over pseudo-header + tcp header + payload, by
// copying them contiguously into a scratch buffer and folding.
static unsigned short tcp_checksum(unsigned int src_ip_net, unsigned int dst_ip_net,
                                   const void* seg, unsigned int seg_len) {
    unsigned char buf[12 + 1500];
    if (seg_len > 1500) return 0;
    pseudo_hdr_t* ph = (pseudo_hdr_t*)buf;
    ph->src_ip   = src_ip_net;
    ph->dst_ip   = dst_ip_net;
    ph->zero     = 0;
    ph->protocol = IP_PROTO_TCP;
    ph->tcp_len  = htons((unsigned short)seg_len);
    for (unsigned int i = 0; i < seg_len; i++) buf[12 + i] = ((const unsigned char*)seg)[i];
    unsigned int total = 12 + seg_len;
    if (total & 1) { buf[total] = 0; total++; }   // pad to even for checksum
    return net_checksum16(buf, total);
}

// Build and send one TCP segment with the given flags and payload.
static int tcp_send_segment(struct tcp_conn* c, unsigned char flags,
                            const void* payload, unsigned int plen) {
    unsigned char our_mac[6];
    e1000_get_mac(our_mac);

    unsigned char frame[14 + 20 + 20 + 1460];
    if (plen > 1460) return 0;
    memset(frame, 0, sizeof(frame));
    eth_hdr_t* eth = (eth_hdr_t*)frame;
    ip_hdr_t*  ip  = (ip_hdr_t*)(frame + 14);
    tcp_hdr_t* tcp = (tcp_hdr_t*)(frame + 14 + 20);
    unsigned char* body = frame + 14 + 20 + 20;

    for (int i = 0; i < 6; i++) eth->dst[i] = c->gw_mac[i];
    for (int i = 0; i < 6; i++) eth->src[i] = our_mac[i];
    eth->ethertype = htons(ETHERTYPE_IPV4);

    unsigned int tcp_total = 20 + plen;
    unsigned int ip_total  = 20 + tcp_total;

    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((unsigned short)ip_total);
    ip->id        = htons(0x5555);
    ip->flags_frag= htons(0x4000);   // don't fragment
    ip->ttl       = 64;
    ip->protocol  = IP_PROTO_TCP;
    ip->checksum  = 0;
    ip->src_ip    = net_ip_to_net(NET_OUR_IP);
    ip->dst_ip    = net_ip_to_net(c->dst_ip);
    ip->checksum  = htons(net_checksum16(ip, 20));

    tcp->src_port = htons(c->src_port);
    tcp->dst_port = htons(c->dst_port);
    tcp->seq      = htonl(c->snd_nxt);
    tcp->ack      = htonl(c->rcv_nxt);
    tcp->data_off = (5 << 4);        // 5 words = 20 bytes, no options
    tcp->flags    = flags;
    tcp->window   = htons(64240);
    tcp->checksum = 0;
    tcp->urgent   = 0;

    for (unsigned int i = 0; i < plen; i++) body[i] = ((const unsigned char*)payload)[i];

    tcp->checksum = htons(tcp_checksum(ip->src_ip, ip->dst_ip, tcp, tcp_total));

    return e1000_send(frame, 14 + ip_total);
}

// Poll for one TCP segment belonging to this connection. On a match, returns
// the payload length (may be 0), writes a pointer to the payload via *payload
// (into the static rx buffer), and the segment's flags via *flags and its seq
// via *seq. Returns -1 if no matching segment is currently available.
static unsigned char g_rxbuf[1600];
static int tcp_poll_segment(struct tcp_conn* c, unsigned char* flags,
                            unsigned int* seq, unsigned int* ack,
                            unsigned char** payload, unsigned int* plen) {
    unsigned int n = e1000_receive_poll(g_rxbuf, sizeof(g_rxbuf));
    if (n < 14 + 20 + 20) return -1;

    eth_hdr_t* eth = (eth_hdr_t*)g_rxbuf;
    if (eth->ethertype != htons(ETHERTYPE_IPV4)) return -1;
    ip_hdr_t* ip = (ip_hdr_t*)(g_rxbuf + 14);
    if ((ip->ver_ihl >> 4) != 4 || ip->protocol != IP_PROTO_TCP) return -1;

    unsigned int ihl = (ip->ver_ihl & 0x0F) * 4;
    tcp_hdr_t* tcp = (tcp_hdr_t*)(g_rxbuf + 14 + ihl);
    if (ntohs(tcp->dst_port) != c->src_port) return -1;
    if (ntohs(tcp->src_port) != c->dst_port) return -1;

    unsigned int tcp_hlen = (tcp->data_off >> 4) * 4;
    unsigned int ip_total = ntohs(ip->total_len);
    unsigned int seg_payload = ip_total - ihl - tcp_hlen;

    *flags   = tcp->flags;
    *seq     = ntohl(tcp->seq);
    *ack     = ntohl(tcp->ack);
    *payload = g_rxbuf + 14 + ihl + tcp_hlen;
    *plen    = seg_payload;
    return (int)seg_payload;
}

tcp_conn_t* tcp_connect(unsigned int dst_ip, unsigned short dst_port) {
    struct tcp_conn* c = &g_conn;
    memset(c, 0, sizeof(*c));
    c->used     = 1;
    c->dst_ip   = dst_ip;
    c->dst_port = dst_port;
    c->src_port = 0xD000 + (pit_millis() & 0x0FFF);  // ephemeral-ish port
    c->snd_nxt  = 0x1000 + (pit_millis() * 7);       // crude initial seq number
    c->rcv_nxt  = 0;

    if (!net_resolve_gateway_mac(c->gw_mac)) { printf("tcp: ARP failed\n"); return 0; }

    // --- send SYN ---
    if (!tcp_send_segment(c, TCP_SYN, 0, 0)) { printf("tcp: SYN send failed\n"); return 0; }
    unsigned int syn_seq = c->snd_nxt;
    c->snd_nxt = syn_seq + 1;   // SYN consumes one sequence number

    // --- await SYN-ACK ---
    unsigned char flags; unsigned int rseq, rack, plen; unsigned char* pl;
    int got = 0;
    for (int t = 0; t < 300; t++) {     // ~3s
        int r = tcp_poll_segment(c, &flags, &rseq, &rack, &pl, &plen);
        if (r >= 0) {
            if (flags & TCP_RST) { printf("tcp: connection refused\n"); return 0; }
            if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
                c->rcv_nxt = rseq + 1;   // SYN consumes one seq
                got = 1;
                break;
            }
        }
        pit_sleep(10);
    }
    if (!got) { printf("tcp: no SYN-ACK\n"); return 0; }

    // --- send ACK to complete the handshake ---
    if (!tcp_send_segment(c, TCP_ACK, 0, 0)) { printf("tcp: ACK send failed\n"); return 0; }
    return c;
}

int tcp_send(tcp_conn_t* c, const void* data, unsigned int len) {
    if (!c || !c->used) return -1;
    const unsigned char* p = (const unsigned char*)data;
    unsigned int remaining = len;

    while (remaining) {
        unsigned int chunk = remaining > 1460 ? 1460 : remaining;
        if (!tcp_send_segment(c, TCP_PSH | TCP_ACK, p, chunk)) return -1;
        c->snd_nxt += chunk;
        p += chunk; remaining -= chunk;

        // Wait for an ACK that covers what we've sent (happy path).
        unsigned char flags; unsigned int rseq, rack, plen; unsigned char* pl;
        for (int t = 0; t < 200; t++) {
            int r = tcp_poll_segment(c, &flags, &rseq, &rack, &pl, &plen);
            if (r >= 0) {
                // Any data that arrives with the ACK: advance rcv_nxt + ack it.
                if (plen > 0 && rseq == c->rcv_nxt) {
                    c->rcv_nxt += plen;
                    tcp_send_segment(c, TCP_ACK, 0, 0);
                }
                if (flags & TCP_ACK) break;
            }
            pit_sleep(5);
        }
    }
    return (int)len;
}

int tcp_recv(tcp_conn_t* c, void* out, unsigned int max, unsigned int timeout_ms) {
    if (!c || !c->used) return -1;
    if (c->peer_fin) return -1;

    unsigned int waited = 0;
    while (waited < timeout_ms) {
        unsigned char flags; unsigned int rseq, rack, plen; unsigned char* pl;
        int r = tcp_poll_segment(c, &flags, &rseq, &rack, &pl, &plen);
        if (r >= 0) {
            if (flags & TCP_RST) { c->peer_fin = 1; return -1; }

            // In-order data segment: deliver it and ACK.
            if (plen > 0 && rseq == c->rcv_nxt) {
                unsigned int n = plen > max ? max : plen;
                for (unsigned int i = 0; i < n; i++) ((unsigned char*)out)[i] = pl[i];
                c->rcv_nxt += plen;
                tcp_send_segment(c, TCP_ACK, 0, 0);
                return (int)n;
            }

            // Peer closing: FIN consumes one seq; ACK it and report EOF.
            if (flags & TCP_FIN) {
                c->rcv_nxt = rseq + plen + 1;
                tcp_send_segment(c, TCP_ACK, 0, 0);
                c->peer_fin = 1;
                return -1;
            }
        }
        pit_sleep(5);
        waited += 5;
    }
    return 0;   // timeout, no data
}

void tcp_close(tcp_conn_t* c) {
    if (!c || !c->used) return;
    tcp_send_segment(c, TCP_FIN | TCP_ACK, 0, 0);
    c->snd_nxt += 1;   // FIN consumes one seq

    // Briefly drain the peer's FIN/ACK so the close is graceful.
    for (int t = 0; t < 50; t++) {
        unsigned char flags; unsigned int rseq, rack, plen; unsigned char* pl;
        int r = tcp_poll_segment(c, &flags, &rseq, &rack, &pl, &plen);
        if (r >= 0 && (flags & TCP_FIN)) {
            c->rcv_nxt = rseq + plen + 1;
            tcp_send_segment(c, TCP_ACK, 0, 0);
            break;
        }
        pit_sleep(5);
    }
    c->used = 0;
}