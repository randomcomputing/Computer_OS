/*
 * inflate.c — DEFLATE decompressor for Computer OS.
 *
 * Implements RFC 1951 (DEFLATE) and RFC 1952 (gzip) from scratch.
 * No stdlib dependencies — uses only kmalloc/kfree and memset/memcpy.
 *
 * DEFLATE has three block types:
 *   Type 0  — stored (uncompressed)
 *   Type 1  — compressed with fixed Huffman codes
 *   Type 2  — compressed with dynamic Huffman codes
 *
 * Huffman coding: each symbol is assigned a variable-length bit code.
 * More frequent symbols get shorter codes. The decoder reconstructs
 * the code table from a list of code lengths.
 *
 * LZ77 back-references: DEFLATE compresses by replacing repeated byte
 * sequences with (distance, length) pairs pointing back into already-
 * decoded output. The decoder copies `length` bytes from `distance`
 * bytes behind the current output position.
 */

#include "inflate.h"
#include "kheap.h"
#include "string.h"
#include "printf.h"

/* ------------------------------------------------------------------ */
/* Bit-stream reader                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char* src;      /* compressed input buffer          */
    unsigned int         src_len;  /* total input bytes                */
    unsigned int         src_pos;  /* next byte to read                */
    unsigned int         bits;     /* bit buffer                       */
    unsigned int         nbits;    /* valid bits in buffer             */
} bitstream_t;

static void bs_init(bitstream_t* bs,
                    const unsigned char* src, unsigned int len) {
    bs->src     = src;
    bs->src_len = len;
    bs->src_pos = 0;
    bs->bits    = 0;
    bs->nbits   = 0;
}

/* Ensure at least `n` bits are in the buffer (n <= 24). */
static void bs_fill(bitstream_t* bs, unsigned int n) {
    while (bs->nbits < n) {
        if (bs->src_pos < bs->src_len) {
            bs->bits |= (unsigned int)bs->src[bs->src_pos++] << bs->nbits;
        }
        bs->nbits += 8;
    }
}

/* Peek at the lowest `n` bits without consuming them. */
static unsigned int bs_peek(bitstream_t* bs, unsigned int n) {
    bs_fill(bs, n);
    return bs->bits & ((1u << n) - 1u);
}

/* Consume `n` bits. */
static void bs_drop(bitstream_t* bs, unsigned int n) {
    bs->bits  >>= n;
    bs->nbits  -= n;
}

/* Read and consume `n` bits. */
static unsigned int bs_read(bitstream_t* bs, unsigned int n) {
    unsigned int v = bs_peek(bs, n);
    bs_drop(bs, n);
    return v;
}

/* Align to the next byte boundary (discard partial byte). */
static void bs_align(bitstream_t* bs) {
    unsigned int discard = bs->nbits & 7;
    bs->bits  >>= discard;
    bs->nbits  -= discard;
}

/* Read a 16-bit little-endian value from the byte stream directly. */
static unsigned int bs_read_u16_le(bitstream_t* bs) {
    bs_align(bs);
    unsigned int lo = bs_read(bs, 8);
    unsigned int hi = bs_read(bs, 8);
    return lo | (hi << 8);
}

/* ------------------------------------------------------------------ */
/* Huffman decoder                                                      */
/* ------------------------------------------------------------------ */

/*
 * We use a canonical Huffman decoder.
 *
 * Given a list of code lengths (one per symbol), we can reconstruct
 * the canonical Huffman codes: sort symbols by length, assign codes
 * in order. Decoding is done by reading bits one at a time and
 * checking against the range of codes for each length.
 *
 * We store, for each code length L:
 *   base_code[L]  — the first canonical code of length L
 *   base_sym[L]   — the first symbol index in the sorted symbol list
 *
 * And a flat sorted symbol array indexed by base_sym[L] + offset.
 */

#define HUFF_MAX_BITS   15
#define HUFF_MAX_SYMS  288

typedef struct {
    unsigned int  count[HUFF_MAX_BITS + 1]; /* # symbols of each length */
    unsigned int  base_code[HUFF_MAX_BITS + 1];
    unsigned int  base_sym[HUFF_MAX_BITS + 1];
    unsigned short symbols[HUFF_MAX_SYMS];  /* symbols sorted by length */
    unsigned int  nsyms;
} huffman_t;

/*
 * Build a Huffman decoder from an array of code lengths.
 * lengths[i] = bit-length of symbol i (0 = not used).
 */
static int huff_build(huffman_t* h, const unsigned char* lengths,
                      unsigned int nsyms) {
    unsigned int i;
    h->nsyms = nsyms;

    /* Count symbols of each length. */
    for (i = 0; i <= HUFF_MAX_BITS; i++) h->count[i] = 0;
    for (i = 0; i < nsyms; i++) {
        if (lengths[i] > HUFF_MAX_BITS) return -1;
        h->count[lengths[i]]++;
    }
    h->count[0] = 0; /* length-0 symbols are unused */

    /* Compute base codes per length (canonical Huffman assignment). */
    unsigned int code = 0;
    for (i = 1; i <= HUFF_MAX_BITS; i++) {
        h->base_code[i] = code;
        code = (code + h->count[i]) << 1;
    }

    /* Compute base symbol indices per length. */
    unsigned int idx = 0;
    for (i = 1; i <= HUFF_MAX_BITS; i++) {
        h->base_sym[i] = idx;
        idx += h->count[i];
    }

    /* Fill sorted symbol table. */
    unsigned int offsets[HUFF_MAX_BITS + 1];
    for (i = 1; i <= HUFF_MAX_BITS; i++) offsets[i] = h->base_sym[i];
    for (i = 0; i < nsyms; i++) {
        unsigned int l = lengths[i];
        if (l > 0) h->symbols[offsets[l]++] = (unsigned short)i;
    }

    return 0;
}

/*
 * Decode one symbol from the bitstream using the Huffman table.
 * Returns the symbol value, or -1 on error.
 */
static int huff_decode(huffman_t* h, bitstream_t* bs) {
    unsigned int code = 0;
    for (unsigned int len = 1; len <= HUFF_MAX_BITS; len++) {
        code = (code << 1) | bs_read(bs, 1);
        if (h->count[len] == 0) continue;
        if (code >= h->base_code[len] &&
            code <  h->base_code[len] + h->count[len]) {
            unsigned int idx = h->base_sym[len] +
                               (code - h->base_code[len]);
            if (idx >= h->nsyms) return -1;
            return (int)h->symbols[idx];
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Fixed Huffman tables (DEFLATE type 1)                               */
/* ------------------------------------------------------------------ */

/*
 * RFC 1951 §3.2.6: fixed literal/length codes
 *   0-143   → 8 bits (codes 0x30-0xBF)
 *   144-255 → 9 bits (codes 0x190-0x1FF)
 *   256-279 → 7 bits (codes 0x00-0x17)
 *   280-287 → 8 bits (codes 0xC0-0xC7)
 */
static huffman_t g_fixed_litlen;
static huffman_t g_fixed_dist;
static int       g_fixed_built = 0;

static void build_fixed_tables(void) {
    if (g_fixed_built) return;

    unsigned char lengths[288];
    unsigned int i;

    for (i =   0; i <= 143; i++) lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;
    huff_build(&g_fixed_litlen, lengths, 288);

    /* Distance codes: all length 5. */
    for (i = 0; i < 30; i++) lengths[i] = 5;
    huff_build(&g_fixed_dist, lengths, 30);

    g_fixed_built = 1;
}

/* ------------------------------------------------------------------ */
/* Length and distance tables (RFC 1951 §3.2.5)                        */
/* ------------------------------------------------------------------ */

/* Extra bits for length codes 257-285 */
static const unsigned char len_extra[29] = {
    0,0,0,0, 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};
/* Base lengths for codes 257-285 */
static const unsigned int len_base[29] = {
    3,4,5,6, 7,8,9,10, 11,13,15,17, 19,23,27,31,
    35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};

/* Extra bits for distance codes 0-29 */
static const unsigned char dist_extra[30] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6, 7,7,8,8,
    9,9,10,10, 11,11,12,12, 13,13
};
/* Base distances for codes 0-29 */
static const unsigned int dist_base[30] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073,
    4097,6145,8193,12289, 16385,24577
};

/* ------------------------------------------------------------------ */
/* Output buffer                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char* buf;
    unsigned int   cap;
    unsigned int   pos;
} outbuf_t;

static int out_byte(outbuf_t* out, unsigned char b) {
    if (out->pos >= out->cap) return -1; /* overflow */
    out->buf[out->pos++] = b;
    return 0;
}

static int out_copy(outbuf_t* out, unsigned int dist, unsigned int len) {
    if (dist > out->pos) return -1; /* invalid back-reference */
    for (unsigned int i = 0; i < len; i++) {
        /* Note: dist may be < len (run-length expansion), so we read
           from the already-written portion of out->buf each iteration. */
        unsigned char b = out->buf[out->pos - dist];
        if (out_byte(out, b) < 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Decompress one DEFLATE block                                         */
/* ------------------------------------------------------------------ */

static int inflate_block(bitstream_t* bs, outbuf_t* out,
                         huffman_t* litlen, huffman_t* dist_h) {
    for (;;) {
        int sym = huff_decode(litlen, bs);
        if (sym < 0) return -1;

        if (sym < 256) {
            /* Literal byte. */
            if (out_byte(out, (unsigned char)sym) < 0) return -1;

        } else if (sym == 256) {
            /* End of block. */
            break;

        } else {
            /* Length/distance back-reference. */
            unsigned int lcode = (unsigned int)sym - 257;
            if (lcode >= 29) return -1;

            unsigned int length = len_base[lcode] +
                                  bs_read(bs, len_extra[lcode]);

            int dcode = huff_decode(dist_h, bs);
            if (dcode < 0 || (unsigned int)dcode >= 30) return -1;

            unsigned int distance = dist_base[dcode] +
                                    bs_read(bs, dist_extra[dcode]);

            if (out_copy(out, distance, length) < 0) return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dynamic Huffman block (type 2)                                       */
/* ------------------------------------------------------------------ */

/*
 * Code length alphabet: 19 symbols encoded in a meta-Huffman tree.
 * The order they appear in the stream is specified by RFC 1951.
 */
static const unsigned char codelen_order[19] = {
    16,17,18, 0, 8,7, 9,6, 10,5, 11,4, 12,3, 13,2, 14,1, 15
};

static int inflate_dynamic(bitstream_t* bs, outbuf_t* out) {
    unsigned int hlit  = bs_read(bs, 5) + 257; /* # literal/length codes */
    unsigned int hdist = bs_read(bs, 5) + 1;   /* # distance codes       */
    unsigned int hclen = bs_read(bs, 4) + 4;   /* # code length codes    */

    if (hlit > 286 || hdist > 30 || hclen > 19) return -1;

    /* Read code length alphabet lengths. */
    unsigned char cl_lengths[19];
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (unsigned int i = 0; i < hclen; i++)
        cl_lengths[codelen_order[i]] = (unsigned char)bs_read(bs, 3);

    huffman_t cl_tree;
    if (huff_build(&cl_tree, cl_lengths, 19) < 0) return -1;

    /* Decode literal/length + distance code lengths. */
    unsigned char all_lengths[286 + 30];
    unsigned int total = hlit + hdist;
    unsigned int i = 0;

    while (i < total) {
        int sym = huff_decode(&cl_tree, bs);
        if (sym < 0) return -1;

        if (sym < 16) {
            /* Literal length value. */
            all_lengths[i++] = (unsigned char)sym;

        } else if (sym == 16) {
            /* Copy previous length 3-6 times. */
            if (i == 0) return -1;
            unsigned int rep = bs_read(bs, 2) + 3;
            unsigned char prev = all_lengths[i - 1];
            for (unsigned int r = 0; r < rep && i < total; r++)
                all_lengths[i++] = prev;

        } else if (sym == 17) {
            /* Repeat zero 3-10 times. */
            unsigned int rep = bs_read(bs, 3) + 3;
            for (unsigned int r = 0; r < rep && i < total; r++)
                all_lengths[i++] = 0;

        } else if (sym == 18) {
            /* Repeat zero 11-138 times. */
            unsigned int rep = bs_read(bs, 7) + 11;
            for (unsigned int r = 0; r < rep && i < total; r++)
                all_lengths[i++] = 0;

        } else {
            return -1;
        }
    }

    huffman_t litlen_tree, dist_tree;
    if (huff_build(&litlen_tree, all_lengths,        hlit)  < 0) return -1;
    if (huff_build(&dist_tree,   all_lengths + hlit, hdist) < 0) return -1;

    return inflate_block(bs, out, &litlen_tree, &dist_tree);
}

/* ------------------------------------------------------------------ */
/* CRC-32 verification                                                  */
/* ------------------------------------------------------------------ */

static unsigned int crc32(const unsigned char* data, unsigned int len) {
    unsigned int crc = 0xFFFFFFFFu;
    for (unsigned int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320u : 0);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/*
 * inflate_gzip — decompress a gzip-compressed buffer.
 *
 *   in      — compressed input bytes
 *   in_len  — number of compressed bytes
 *   out     — output buffer (caller allocates)
 *   out_cap — size of output buffer in bytes
 *
 * Returns the number of decompressed bytes on success, or -1 on error.
 */
int inflate_gzip(const unsigned char* in,  unsigned int in_len,
                       unsigned char* out, unsigned int out_cap) {
    build_fixed_tables();

    /* ---- Parse gzip header (RFC 1952) ---- */
    if (in_len < 18) {
        printf("inflate: input too short for gzip header\n");
        return -1;
    }

    /* Magic number */
    if (in[0] != 0x1F || in[1] != 0x8B) {
        printf("inflate: not a gzip file (bad magic)\n");
        return -1;
    }

    /* Compression method must be 8 (DEFLATE) */
    if (in[2] != 8) {
        printf("inflate: unsupported compression method %u\n", in[2]);
        return -1;
    }

    unsigned char flags = in[3];
    /* Bytes 4-7: mtime, byte 8: xfl, byte 9: OS — skip all */
    unsigned int pos = 10;

    /* FEXTRA: skip extra field */
    if (flags & 0x04) {
        if (pos + 2 > in_len) return -1;
        unsigned int xlen = (unsigned int)in[pos] | ((unsigned int)in[pos+1] << 8);
        pos += 2 + xlen;
    }

    /* FNAME: skip null-terminated filename */
    if (flags & 0x08) {
        while (pos < in_len && in[pos] != 0) pos++;
        pos++; /* skip null terminator */
    }

    /* FCOMMENT: skip null-terminated comment */
    if (flags & 0x10) {
        while (pos < in_len && in[pos] != 0) pos++;
        pos++;
    }

    /* FHCRC: skip 2-byte header CRC */
    if (flags & 0x02) pos += 2;

    if (pos >= in_len) {
        printf("inflate: gzip header consumed all input\n");
        return -1;
    }

    /* ---- Decompress DEFLATE blocks ---- */
    bitstream_t bs;
    bs_init(&bs, in + pos, in_len - pos);

    outbuf_t ob;
    ob.buf = out;
    ob.cap = out_cap;
    ob.pos = 0;

    int final_block = 0;
    while (!final_block) {
        final_block = (int)bs_read(&bs, 1); /* BFINAL */
        unsigned int btype = bs_read(&bs, 2); /* BTYPE */

        if (btype == 0) {
            /* ---- Type 0: stored block ---- */
            bs_align(&bs);
            unsigned int blen  = bs_read_u16_le(&bs);
            unsigned int bnlen = bs_read_u16_le(&bs);
            if ((blen ^ bnlen) != 0xFFFF) {
                printf("inflate: stored block length check failed\n");
                return -1;
            }
            for (unsigned int i = 0; i < blen; i++) {
                if (out_byte(&ob, (unsigned char)bs_read(&bs, 8)) < 0) {
                    printf("inflate: output buffer full\n");
                    return -1;
                }
            }

        } else if (btype == 1) {
            /* ---- Type 1: fixed Huffman ---- */
            if (inflate_block(&bs, &ob,
                              &g_fixed_litlen, &g_fixed_dist) < 0) {
                printf("inflate: fixed Huffman block error\n");
                return -1;
            }

        } else if (btype == 2) {
            /* ---- Type 2: dynamic Huffman ---- */
            if (inflate_dynamic(&bs, &ob) < 0) {
                printf("inflate: dynamic Huffman block error\n");
                return -1;
            }

        } else {
            printf("inflate: reserved block type 3\n");
            return -1;
        }
    }

    /* ---- Verify gzip trailer (CRC32 + ISIZE) ---- */
    /* Align to byte boundary, then read 8 bytes from the raw stream. */
    bs_align(&bs);
    unsigned int trailer_pos = pos + (bs.src_pos - (bs.nbits / 8));

    if (trailer_pos + 8 > in_len) {
        printf("inflate: truncated gzip trailer\n");
        return -1;
    }

    unsigned int stored_crc =
        (unsigned int)in[trailer_pos + 0]        |
        ((unsigned int)in[trailer_pos + 1] << 8)  |
        ((unsigned int)in[trailer_pos + 2] << 16) |
        ((unsigned int)in[trailer_pos + 3] << 24);

    unsigned int stored_size =
        (unsigned int)in[trailer_pos + 4]        |
        ((unsigned int)in[trailer_pos + 5] << 8)  |
        ((unsigned int)in[trailer_pos + 6] << 16) |
        ((unsigned int)in[trailer_pos + 7] << 24);

    if (stored_size != ob.pos) {
        printf("inflate: size mismatch (got %u, expected %u)\n",
               ob.pos, stored_size);
        return -1;
    }

    unsigned int actual_crc = crc32(out, ob.pos);
    if (actual_crc != stored_crc) {
        printf("inflate: CRC32 mismatch (got 0x%x, expected 0x%x)\n",
               actual_crc, stored_crc);
        return -1;
    }

    return (int)ob.pos;
}

/*
 * inflate_gzip_size — return the uncompressed size from the gzip trailer.
 * This is the ISIZE field (last 4 bytes of the file).
 * Accurate for files < 4 GB. Returns 0 on error.
 */
unsigned int inflate_gzip_size(const unsigned char* in, unsigned int in_len) {
    if (in_len < 4) return 0;
    unsigned int off = in_len - 4;
    return (unsigned int)in[off + 0]        |
           ((unsigned int)in[off + 1] << 8)  |
           ((unsigned int)in[off + 2] << 16) |
           ((unsigned int)in[off + 3] << 24);
}