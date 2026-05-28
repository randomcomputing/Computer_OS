#include "net.h"
#include "arp.h"
#include "e1000.h"
#include "string.h"
#include "pit.h"
#include "printf.h"
#include "net_internal.h"

static unsigned short htons(unsigned short v) { return (unsigned short)((v << 8) | (v >> 8)); }
static unsigned short ntohs(unsigned short v) { return htons(v); }

#define IP_PROTO_ICMP   1
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

typedef struct {
    unsigned char  type;
    unsigned char  code;
    unsigned short checksum;
    unsigned short id;
    unsigned short seq;
} __attribute__((packed)) icmp_hdr_t;

// Standard IPv4/ICMP ones-complement checksum over `len` bytes.
static unsigned short checksum16(const void* data, unsigned int len) {
    const unsigned char* p = (const unsigned char*)data;
    unsigned int sum = 0;
    while (len > 1) {
        sum += (unsigned int)((p[0] << 8) | p[1]);
        p += 2; len -= 2;
    }
    if (len) sum += (unsigned int)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)(~sum & 0xFFFF);
}

// Convert a host-order IP to network-order (big-endian) 32-bit.
static unsigned int ip_to_net(unsigned int ip) {
    return ((ip & 0xFF) << 24) | (((ip >> 8) & 0xFF) << 16) |
           (((ip >> 16) & 0xFF) << 8) | ((ip >> 24) & 0xFF);
}

// Send one ICMP echo request to dst_ip via dst_mac. Returns 1 if queued.
static int send_echo(const unsigned char dst_mac[6], unsigned int dst_ip,
                     unsigned short id, unsigned short seq) {
    unsigned char our_mac[6];
    e1000_get_mac(our_mac);

    unsigned char frame[14 + 20 + 8 + 32];   // eth + ip + icmp + 32B payload
    memset(frame, 0, sizeof(frame));
    eth_hdr_t*  eth  = (eth_hdr_t*)frame;
    ip_hdr_t*   ip   = (ip_hdr_t*)(frame + 14);
    icmp_hdr_t* icmp = (icmp_hdr_t*)(frame + 14 + 20);
    unsigned char* payload = frame + 14 + 20 + 8;

    for (int i = 0; i < 6; i++) eth->dst[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eth->src[i] = our_mac[i];
    eth->ethertype = htons(ETHERTYPE_IPV4);

    unsigned int payload_len = 32;
    unsigned int icmp_len = 8 + payload_len;
    unsigned int ip_total = 20 + icmp_len;

    ip->ver_ihl   = 0x45;            // IPv4, 5*4 = 20-byte header
    ip->tos       = 0;
    ip->total_len = htons((unsigned short)ip_total);
    ip->id        = htons(id);
    ip->flags_frag= 0;
    ip->ttl       = 64;
    ip->protocol  = IP_PROTO_ICMP;
    ip->checksum  = 0;
    ip->src_ip    = ip_to_net(NET_OUR_IP);
    ip->dst_ip    = ip_to_net(dst_ip);
    ip->checksum  = htons(checksum16(ip, 20));

    for (unsigned int i = 0; i < payload_len; i++) payload[i] = (unsigned char)('a' + (i % 26));

    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id  = htons(id);
    icmp->seq = htons(seq);
    icmp->checksum = htons(checksum16(icmp, icmp_len));

    return e1000_send(frame, 14 + ip_total);
}

// Poll for an ICMP echo reply matching (id, seq) from src_ip. Returns 1 on a
// match. Ignores ARP and non-matching frames.
static int poll_echo_reply(unsigned int expect_ip, unsigned short id, unsigned short seq) {
    unsigned char buf[1600];
    unsigned int len = e1000_receive_poll(buf, sizeof(buf));
    if (len < 14 + 20 + 8) return 0;

    eth_hdr_t* eth = (eth_hdr_t*)buf;
    if (eth->ethertype != htons(ETHERTYPE_IPV4)) return 0;

    ip_hdr_t* ip = (ip_hdr_t*)(buf + 14);
    if ((ip->ver_ihl >> 4) != 4) return 0;
    if (ip->protocol != IP_PROTO_ICMP) return 0;

    unsigned int ihl = (ip->ver_ihl & 0x0F) * 4;
    icmp_hdr_t* icmp = (icmp_hdr_t*)(buf + 14 + ihl);
    if (icmp->type != ICMP_ECHO_REPLY) return 0;
    if (ntohs(icmp->id) != id || ntohs(icmp->seq) != seq) return 0;

    (void)expect_ip;   // SLIRP may reply from the gateway; don't require exact src
    return 1;
}

// ---- shared: resolve the gateway MAC via ARP -----------------------------
// SLIRP routes everything for us, so all outbound IP goes to the gateway's
// MAC. Returns 1 and fills mac_out on success, 0 on timeout.
static int resolve_gateway_mac(unsigned char mac_out[6]) {
    if (!arp_request(NET_GATEWAY)) return 0;
    unsigned int ip;
    for (int t = 0; t < 200; t++) {
        if (arp_poll_reply(mac_out, &ip)) return 1;
        pit_sleep(10);
    }
    return 0;
}

// ---- UDP -----------------------------------------------------------------
#define IP_PROTO_UDP 17

typedef struct {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned short length;
    unsigned short checksum;
} __attribute__((packed)) udp_hdr_t;

// Send a UDP datagram to dst_ip:dst_port via the gateway MAC. payload/len is
// the UDP body. Returns 1 if queued. (UDP checksum is optional in IPv4 and we
// send 0 = "not computed", which SLIRP accepts.)
static int send_udp(const unsigned char dst_mac[6], unsigned int dst_ip,
                    unsigned short src_port, unsigned short dst_port,
                    const void* payload, unsigned int len) {
    unsigned char our_mac[6];
    e1000_get_mac(our_mac);

    unsigned char frame[14 + 20 + 8 + 512];
    if (len > 512) return 0;
    memset(frame, 0, sizeof(frame));
    eth_hdr_t* eth = (eth_hdr_t*)frame;
    ip_hdr_t*  ip  = (ip_hdr_t*)(frame + 14);
    udp_hdr_t* udp = (udp_hdr_t*)(frame + 14 + 20);
    unsigned char* body = frame + 14 + 20 + 8;

    for (int i = 0; i < 6; i++) eth->dst[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eth->src[i] = our_mac[i];
    eth->ethertype = htons(ETHERTYPE_IPV4);

    unsigned int udp_len = 8 + len;
    unsigned int ip_total = 20 + udp_len;

    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((unsigned short)ip_total);
    ip->id        = htons(0x4321);
    ip->flags_frag= 0;
    ip->ttl       = 64;
    ip->protocol  = IP_PROTO_UDP;
    ip->checksum  = 0;
    ip->src_ip    = ip_to_net(NET_OUR_IP);
    ip->dst_ip    = ip_to_net(dst_ip);
    ip->checksum  = htons(checksum16(ip, 20));

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons((unsigned short)udp_len);
    udp->checksum = 0;   // optional in IPv4; 0 = not computed

    for (unsigned int i = 0; i < len; i++) body[i] = ((const unsigned char*)payload)[i];

    return e1000_send(frame, 14 + ip_total);
}

// Poll for a UDP datagram arriving on our src_port. On a match, copies the UDP
// body into out (up to max) and returns its length; returns 0 if nothing yet.
static unsigned int poll_udp(unsigned short our_port, void* out, unsigned int max) {
    unsigned char buf[1600];
    unsigned int n = e1000_receive_poll(buf, sizeof(buf));
    if (n < 14 + 20 + 8) return 0;

    eth_hdr_t* eth = (eth_hdr_t*)buf;
    if (eth->ethertype != htons(ETHERTYPE_IPV4)) return 0;
    ip_hdr_t* ip = (ip_hdr_t*)(buf + 14);
    if ((ip->ver_ihl >> 4) != 4 || ip->protocol != IP_PROTO_UDP) return 0;

    unsigned int ihl = (ip->ver_ihl & 0x0F) * 4;
    udp_hdr_t* udp = (udp_hdr_t*)(buf + 14 + ihl);
    if (ntohs(udp->dst_port) != our_port) return 0;

    unsigned int body_len = ntohs(udp->length);
    if (body_len < 8) return 0;
    body_len -= 8;
    if (body_len > max) body_len = max;
    unsigned char* body = buf + 14 + ihl + 8;
    for (unsigned int i = 0; i < body_len; i++) ((unsigned char*)out)[i] = body[i];
    return body_len;
}

// ---- DNS -----------------------------------------------------------------
// Minimal DNS A-record query/response over UDP port 53.

typedef struct {
    unsigned short id;
    unsigned short flags;
    unsigned short qdcount;   // questions
    unsigned short ancount;   // answers
    unsigned short nscount;
    unsigned short arcount;
} __attribute__((packed)) dns_hdr_t;

// Encode "archlinux.org" as length-prefixed labels: 09 a r c h l i n u x 03 o
// r g 00. Writes into out, returns bytes written, or 0 on overflow.
static unsigned int dns_encode_name(const char* host, unsigned char* out, unsigned int max) {
    unsigned int o = 0;
    const char* p = host;
    while (*p) {
        const char* start = p;
        while (*p && *p != '.') p++;
        unsigned int seglen = (unsigned int)(p - start);
        if (seglen == 0 || seglen > 63) return 0;
        if (o + 1 + seglen >= max) return 0;
        out[o++] = (unsigned char)seglen;
        for (unsigned int i = 0; i < seglen; i++) out[o++] = (unsigned char)start[i];
        if (*p == '.') p++;
    }
    if (o + 1 > max) return 0;
    out[o++] = 0;   // root label terminator
    return o;
}

// Skip a (possibly compressed) DNS name starting at buf[off]. Returns the
// offset just past it. Compression pointers (top two bits set) are 2 bytes.
static unsigned int dns_skip_name(const unsigned char* buf, unsigned int off, unsigned int len) {
    while (off < len) {
        unsigned char b = buf[off];
        if (b == 0) { return off + 1; }
        if ((b & 0xC0) == 0xC0) { return off + 2; }   // compression pointer
        off += 1 + b;
    }
    return off;
}

int dns_resolve(const char* hostname, unsigned int* out_ip) {
    unsigned char gw_mac[6];
    if (!resolve_gateway_mac(gw_mac)) return 0;

    // Build the DNS query: header + question (name, QTYPE=A, QCLASS=IN).
    unsigned char query[512];
    dns_hdr_t* h = (dns_hdr_t*)query;
    h->id      = htons(0xABCD);
    h->flags   = htons(0x0100);   // standard query, recursion desired
    h->qdcount = htons(1);
    h->ancount = 0; h->nscount = 0; h->arcount = 0;

    unsigned int o = sizeof(dns_hdr_t);
    unsigned int namelen = dns_encode_name(hostname, query + o, sizeof(query) - o - 4);
    if (!namelen) return 0;
    o += namelen;
    query[o++] = 0x00; query[o++] = 0x01;   // QTYPE  = A
    query[o++] = 0x00; query[o++] = 0x01;   // QCLASS = IN

    unsigned short sport = 0xC000;   // arbitrary high source port
    if (!send_udp(gw_mac, NET_DNS, sport, 53, query, o)) return 0;

    // Await the response (~2s).
    unsigned char resp[1500];
    unsigned int rlen = 0;
    for (int t = 0; t < 200; t++) {
        rlen = poll_udp(sport, resp, sizeof(resp));
        if (rlen) break;
        pit_sleep(10);
    }
    if (rlen < sizeof(dns_hdr_t)) return 0;

    dns_hdr_t* rh = (dns_hdr_t*)resp;
    unsigned int answers = ntohs(rh->ancount);
    if (answers == 0) return 0;

    // Skip the question section: name + QTYPE(2) + QCLASS(2).
    unsigned int off = sizeof(dns_hdr_t);
    off = dns_skip_name(resp, off, rlen);
    off += 4;

    // Walk answer records, looking for the first A record (TYPE=1, RDLENGTH=4).
    for (unsigned int a = 0; a < answers && off + 10 <= rlen; a++) {
        off = dns_skip_name(resp, off, rlen);
        if (off + 10 > rlen) return 0;
        unsigned short type   = (unsigned short)((resp[off] << 8) | resp[off+1]);
        unsigned short rdlen  = (unsigned short)((resp[off+8] << 8) | resp[off+9]);
        unsigned int   rdata  = off + 10;
        if (type == 1 && rdlen == 4 && rdata + 4 <= rlen) {
            *out_ip = ((unsigned int)resp[rdata]   << 24) |
                      ((unsigned int)resp[rdata+1] << 16) |
                      ((unsigned int)resp[rdata+2] << 8)  |
                       (unsigned int)resp[rdata+3];
            return 1;
        }
        off = rdata + rdlen;   // not an A record — skip to the next answer
    }
    return 0;
}

int ping(unsigned int dst_ip, int count, unsigned int timeout_ms) {
    // Resolve the MAC to send to. For SLIRP, everything goes through the
    // gateway's MAC (it routes for us), so ARP the gateway regardless of dst.
    unsigned int arp_target = NET_GATEWAY;
    printf("resolving %u.%u.%u.%u via ARP...\n",
           (arp_target >> 24) & 0xFF, (arp_target >> 16) & 0xFF,
           (arp_target >> 8) & 0xFF, arp_target & 0xFF);

    if (!arp_request(arp_target)) { printf("ping: ARP send failed\n"); return -1; }

    unsigned char gw_mac[6];
    unsigned int gw_ip;
    int resolved = 0;
    for (int t = 0; t < 200; t++) {
        if (arp_poll_reply(gw_mac, &gw_ip)) { resolved = 1; break; }
        pit_sleep(10);
    }
    if (!resolved) { printf("ping: ARP timed out\n"); return -1; }

    int replies = 0;
    unsigned short id = 0x1234;
    for (int i = 0; i < count; i++) {
        unsigned short seq = (unsigned short)i;
        unsigned int t0 = pit_millis();
        if (!send_echo(gw_mac, dst_ip, id, seq)) {
            printf("ping: send failed (seq=%d)\n", i);
            continue;
        }

        int got = 0;
        while (pit_millis() - t0 < timeout_ms) {
            if (poll_echo_reply(dst_ip, id, seq)) { got = 1; break; }
            pit_sleep(1);
        }
        unsigned int rtt = pit_millis() - t0;
        if (got) {
            printf("reply from %u.%u.%u.%u: seq=%d time=%ums\n",
                   (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
                   (dst_ip >> 8) & 0xFF, dst_ip & 0xFF, i, rtt);
            replies++;
        } else {
            printf("request timed out: seq=%d\n", i);
        }
        pit_sleep(200);
    }

    printf("ping: %d/%d replies\n", replies, count);
    return replies;
}

// ---- internal helpers exposed to tcp.c (see net_internal.h) --------------
unsigned short net_checksum16(const void* data, unsigned int len) {
    return checksum16(data, len);
}
unsigned int net_ip_to_net(unsigned int ip) {
    return ip_to_net(ip);
}
int net_resolve_gateway_mac(unsigned char mac_out[6]) {
    return resolve_gateway_mac(mac_out);
}