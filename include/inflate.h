#ifndef INFLATE_H
#define INFLATE_H

/*
 * inflate.h — DEFLATE/gzip decompressor for Computer OS.
 *
 * Implements RFC 1951 (DEFLATE) and RFC 1952 (gzip).
 * No stdlib dependencies.
 */

/*
 * inflate_gzip — decompress a gzip buffer into `out`.
 *
 *   in      — pointer to the gzip-compressed data
 *   in_len  — size of compressed data in bytes
 *   out     — caller-allocated output buffer
 *   out_cap — capacity of output buffer in bytes
 *
 * Use inflate_gzip_size() to find the right output buffer size before
 * calling this.
 *
 * Returns the number of decompressed bytes written, or -1 on error.
 */
int inflate_gzip(const unsigned char* in,  unsigned int in_len,
                       unsigned char* out, unsigned int out_cap);

/*
 * inflate_gzip_size — read the uncompressed size from the gzip trailer.
 *
 * gzip stores the original file size (mod 2^32) in the last 4 bytes.
 * This lets callers allocate the right output buffer without guessing.
 *
 * Returns the uncompressed size in bytes, or 0 on error.
 * Note: accurate only for files smaller than 4 GB.
 */
unsigned int inflate_gzip_size(const unsigned char* in, unsigned int in_len);

#endif /* INFLATE_H */