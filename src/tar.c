/*
 * tar.c — ustar/POSIX tar extractor for Computer OS.
 *
 * Reads a .tar image from the VFS (one big vfs_read_file call) and
 * walks the 512-byte block stream, creating directories and writing
 * file data through the VFS.
 *
 * Supported entry types:
 *   '0' / '\0'  regular file
 *   '2'         symbolic link (written as a regular file holding the target)
 *   '1'         hard link    (same treatment — just copy the target name)
 *   '5'         directory
 *   'L'         GNU @LongLink — next entry's filename follows in data blocks
 *   'K'         GNU @LongLink — next entry's link target follows in data blocks
 *   'g','x'     PAX extended headers — skipped (data blocks consumed)
 *
 * All other types ('3','4','6','7') are skipped.
 */

#include "tar.h"
#include "inflate.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"
#include "printf.h"
#include "console.h"

/* ------------------------------------------------------------------ */
/* ustar on-disk header (one 512-byte block)                           */
/* ------------------------------------------------------------------ */

#define TAR_BLOCK   512
#define TAR_NAMESZ  100
#define TAR_PREFIXSZ 155

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];       /* octal string */
    char mtime[12];      /* octal string */
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];       /* "ustar" */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];    /* path prefix for ustar */
    char pad[12];
} __attribute__((packed)) tar_header_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Parse an octal ASCII field of up to `len` bytes. */
static unsigned int octal_to_uint(const char* s, int len) {
    unsigned int v = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        v = (v << 3) | (unsigned int)(s[i] - '0');
    return v;
}

/* True if every byte in the 512-byte block is zero (end-of-archive). */
static int block_is_zero(const char* blk) {
    for (int i = 0; i < TAR_BLOCK; i++)
        if (blk[i]) return 0;
    return 1;
}

/* Verify the ustar checksum. Returns 1 if valid. */
static int checksum_ok(const tar_header_t* h) {
    /* Sum all bytes, treating the checksum field itself as spaces. */
    unsigned int sum = 0;
    const unsigned char* b = (const unsigned char*)h;
    for (int i = 0; i < TAR_BLOCK; i++) {
        if (i >= 148 && i < 156)
            sum += ' ';
        else
            sum += b[i];
    }
    unsigned int stored = octal_to_uint(h->checksum, 8);
    return sum == stored;
}

/*
 * Build the full path from a ustar header.
 * ustar stores an optional prefix; if present the real name is
 * prefix + "/" + name.
 */
static void build_name(const tar_header_t* h, char* out, unsigned int outsz) {
    /* Check if this is a proper ustar header with a prefix. */
    int has_ustar = (h->magic[0] == 'u' && h->magic[1] == 's' &&
                     h->magic[2] == 't' && h->magic[3] == 'a' &&
                     h->magic[4] == 'r');

    if (has_ustar && h->prefix[0] != '\0') {
        /* prefix + "/" + name */
        unsigned int i = 0;
        for (unsigned int k = 0; k < TAR_PREFIXSZ && h->prefix[k] && i < outsz - 1; k++)
            out[i++] = h->prefix[k];
        if (i < outsz - 1) out[i++] = '/';
        for (unsigned int k = 0; k < TAR_NAMESZ && h->name[k] && i < outsz - 1; k++)
            out[i++] = h->name[k];
        out[i] = '\0';
    } else {
        unsigned int i = 0;
        for (; i < TAR_NAMESZ - 1 && h->name[i]; i++)
            out[i] = h->name[i];
        out[i] = '\0';
    }
}

/*
 * Join dst_dir + "/" + entry_path into out[outsz].
 * Strips a trailing '/' from entry_path (directories in tar end with '/').
 * Collapses multiple slashes.
 */
static void join_path(const char* dir, const char* entry,
                      char* out, unsigned int outsz) {
    unsigned int i = 0;

    /* Write the destination directory. */
    for (const char* p = dir; *p && i < outsz - 1; p++)
        out[i++] = *p;

    /* Strip trailing slashes from dir half. */
    while (i > 1 && out[i-1] == '/') i--;

    /* Append '/' separator. */
    if (i < outsz - 1) out[i++] = '/';

    /* Write the entry path, collapsing leading '/'. */
    const char* e = entry;
    while (*e == '/') e++;

    for (; *e && i < outsz - 1; e++)
        out[i++] = *e;

    /* Strip trailing '/' (tar directory entries end with it). */
    while (i > 1 && out[i-1] == '/') i--;

    out[i] = '\0';
}

/*
 * Ensure every directory component of `path` exists.
 * Works by iterating through '/' separators and calling vfs_mkdir
 * on each prefix that doesn't yet exist. Ignores errors from
 * vfs_mkdir when the directory already exists.
 */
static void mkdir_p(const char* path) {
    char buf[256];
    unsigned int len = 0;
    const char* p = path;

    /* Skip leading slash — root always exists. */
    if (*p == '/') { buf[len++] = '/'; p++; }

    while (*p) {
        /* Copy until next '/'. */
        while (*p && *p != '/') {
            if (len < sizeof(buf) - 1) buf[len++] = *p;
            p++;
        }
        buf[len] = '\0';
        /* Attempt to create this prefix (ignore "already exists" errors). */
        if (len > 1) vfs_mkdir(buf);
        if (*p == '/') {
            if (len < sizeof(buf) - 1) buf[len++] = '/';
            p++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Max size of tar we'll load.  64 MB should cover most packages.      */
/* ------------------------------------------------------------------ */
#define TAR_MAX_SIZE (4 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Internal worker — operates on an in-memory tar image.               */
/* ------------------------------------------------------------------ */

static int tar_process(const unsigned char* data, unsigned int size,
                       const char* dst_dir, int extract, int verbose) {
    unsigned int pos     = 0;
    int          entries = 0;
    int          zero_blocks = 0;

    /* Buffers for GNU LongLink overrides. */
    char longname[512];
    char longlink[512];
    int  have_longname = 0;
    int  have_longlink = 0;

    while (pos + TAR_BLOCK <= size) {
        const tar_header_t* h = (const tar_header_t*)(data + pos);

        /* Two consecutive zero blocks = end of archive. */
        if (block_is_zero((const char*)h)) {
            if (++zero_blocks >= 2) break;
            pos += TAR_BLOCK;
            continue;
        }
        zero_blocks = 0;

        if (!checksum_ok(h)) {
            printf("tar: bad checksum at offset %u, stopping\n", pos);
            break;
        }

        /* Entry name (may be overridden by a preceding LongLink block). */
        char name[512];
        if (have_longname) {
            unsigned int i = 0;
            while (i < sizeof(name) - 1 && longname[i]) { name[i] = longname[i]; i++; }
            name[i] = '\0';
            have_longname = 0;
        } else {
            build_name(h, name, sizeof(name));
        }

        /* Link target (may be overridden by a preceding LongLink 'K' block). */
        char linkname[256];
        if (have_longlink) {
            unsigned int i = 0;
            while (i < sizeof(linkname) - 1 && longlink[i]) { linkname[i] = longlink[i]; i++; }
            linkname[i] = '\0';
            have_longlink = 0;
        } else {
            unsigned int i = 0;
            while (i < 99 && h->linkname[i]) { linkname[i] = h->linkname[i]; i++; }
            linkname[i] = '\0';
        }

        unsigned int file_size = octal_to_uint(h->size, 12);
        /* Number of 512-byte data blocks that follow this header. */
        unsigned int data_blocks = (file_size + TAR_BLOCK - 1) / TAR_BLOCK;

        pos += TAR_BLOCK; /* Advance past header. */

        char typeflag = h->typeflag;
        /* Old-style tar uses '\0' for regular files. */
        if (typeflag == '\0') typeflag = '0';

        /* ----------------------------------------------------------
         * GNU @LongLink: the data blocks contain the long filename
         * (type 'L') or long linkname (type 'K') for the NEXT entry.
         * ---------------------------------------------------------- */
        if (typeflag == 'L') {
            unsigned int copy = file_size < sizeof(longname) - 1
                                ? file_size : sizeof(longname) - 1;
            memcpy(longname, data + pos, copy);
            longname[copy] = '\0';
            have_longname = 1;
            pos += data_blocks * TAR_BLOCK;
            continue;
        }
        if (typeflag == 'K') {
            unsigned int copy = file_size < sizeof(longlink) - 1
                                ? file_size : sizeof(longlink) - 1;
            memcpy(longlink, data + pos, copy);
            longlink[copy] = '\0';
            have_longlink = 1;
            pos += data_blocks * TAR_BLOCK;
            continue;
        }

        /* PAX extended headers — skip data and move on. */
        if (typeflag == 'g' || typeflag == 'x') {
            pos += data_blocks * TAR_BLOCK;
            continue;
        }

        /* Build the full destination path. */
        char fullpath[256];
        join_path(dst_dir, name, fullpath, sizeof(fullpath));

        /* ----------------------------------------------------------
         * Dispatch by type.
         * ---------------------------------------------------------- */
        if (typeflag == '5') {
            /* Directory */
            if (extract) {
                mkdir_p(fullpath);
            }
            if (verbose) {
                con_set_color(CON_CYAN, CON_BLACK);
                printf("d ");
                con_set_color(CON_WHITE, CON_BLACK);
                printf("%s\n", fullpath);
            }
            entries++;
            /* Directories carry no data blocks. */

        } else if (typeflag == '0' || typeflag == '1' || typeflag == '2') {
            /* Regular file, hard link, or symlink.
             * Hard links and symlinks: if we have no data, write the
             * link target string as the file content (simple but useful). */

            const unsigned char* file_data = data + pos;
            unsigned int         write_size = file_size;

            /* For hard/symlinks with zero data size, use linkname as content. */
            if ((typeflag == '1' || typeflag == '2') && file_size == 0 && linkname[0]) {
                file_data  = (const unsigned char*)linkname;
                write_size = (unsigned int)strlen(linkname);
            }

            if (extract) {
                /* Ensure the parent directory exists. */
                char parent[256];
                unsigned int plen = 0;
                for (unsigned int i = 0; fullpath[i]; i++) {
                    parent[plen++] = fullpath[i];
                }
                parent[plen] = '\0';
                /* Walk back to last '/' to get the parent. */
                while (plen > 1 && parent[plen-1] != '/') plen--;
                if (plen > 1) { parent[plen-1] = '\0'; mkdir_p(parent); }

                int r = vfs_write_file(fullpath, file_data, write_size);
                if (r < 0 && verbose) {
                    con_set_color(CON_LIGHT_RED, CON_BLACK);
                    printf("tar: write failed: %s\n", fullpath);
                    con_set_color(CON_WHITE, CON_BLACK);
                }
            }

            if (verbose) {
                if (typeflag == '2') {
                    con_set_color(CON_LIGHT_CYAN, CON_BLACK);
                    printf("l ");
                    con_set_color(CON_WHITE, CON_BLACK);
                    printf("%s -> %s\n", fullpath, linkname);
                } else {
                    con_set_color(CON_WHITE, CON_BLACK);
                    printf("f ");
                    printf("%s  (%u bytes)\n", fullpath, file_size);
                }
            }
            entries++;
            pos += data_blocks * TAR_BLOCK;

        } else {
            /* Device nodes, FIFOs, etc. — skip data, count as skipped. */
            if (verbose)
                printf("tar: skipping type '%c': %s\n", typeflag, name);
            pos += data_blocks * TAR_BLOCK;
        }
    }

    return entries;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/* Returns 1 if `name` ends with `suffix` (case-insensitive). */
static int ends_with_ci(const char* name, const char* suffix) {
    unsigned int nlen = (unsigned int)strlen(name);
    unsigned int slen = (unsigned int)strlen(suffix);
    if (nlen < slen) return 0;
    const char* p = name + (nlen - slen);
    for (unsigned int i = 0; i < slen; i++) {
        char a = p[i]      >= 'a' && p[i]      <= 'z' ? p[i]      - 32 : p[i];
        char b = suffix[i] >= 'a' && suffix[i] <= 'z' ? suffix[i] - 32 : suffix[i];
        if (a != b) return 0;
    }
    return 1;
}

/*
 * Load the tar file from VFS into a heap buffer, then process it.
 * Transparently handles both .tar and .tar.gz / .tgz files.
 */
static int tar_load_and_process(const char* src_path, const char* dst_dir,
                                int extract, int verbose) {
    /* Get actual file size via vfs_list to avoid over-allocating. */
    unsigned int compressed_size = TAR_MAX_SIZE;
    {
        char parent[256]; parent[0] = '/'; parent[1] = '\0';
        unsigned int sl = 0;
        for (unsigned int i = 0; src_path[i]; i++)
            if (src_path[i] == '/') sl = i;
        if (sl > 0) {
            for (unsigned int i = 0; i < sl && i < 254; i++)
                parent[i] = src_path[i];
            parent[sl] = '\0';
        }
        /* If path starts with '/', sl=0 means the slash is at index 0.
           base is everything after the last slash. */
        const char* base = (src_path[sl] == '/') ? src_path + sl + 1 : src_path + sl;
        vfs_dirent_t* ents = (vfs_dirent_t*)kmalloc(64 * sizeof(vfs_dirent_t));
        if (!ents) { compressed_size = TAR_MAX_SIZE; goto skip_size_lookup; }
        int ne = vfs_list(parent, ents, 64);
        for (int i = 0; i < ne; i++) {
            const char* a = ents[i].name;
            const char* b = base;
            int match = 1; int j = 0;
            for (; a[j] && b[j]; j++) {
                char ca = a[j]>='a'&&a[j]<='z'?a[j]-32:a[j];
                char cb = b[j]>='a'&&b[j]<='z'?b[j]-32:b[j];
                if (ca != cb) { match = 0; break; }
            }
            if (match && !a[j] && !b[j] && ents[i].size > 0) {
                compressed_size = ents[i].size;
                break;
            }
        }
        kfree(ents);
        if (compressed_size > TAR_MAX_SIZE) compressed_size = TAR_MAX_SIZE;
    }
skip_size_lookup:;

    unsigned char* buf = (unsigned char*)kmalloc(compressed_size);
    if (!buf) {
        printf("tar: out of memory\n");
        return -1;
    }

    int n = vfs_read_file(src_path, buf, compressed_size);
    if (n <= 0) {
        printf("tar: cannot read %s\n", src_path);
        kfree(buf);
        return -1;
    }

    /* Detect gzip by magic bytes (1F 8B). */
    int is_gzip = ((unsigned int)n >= 2 &&
                   buf[0] == 0x1F && buf[1] == 0x8B);
    if (!is_gzip) {
        is_gzip = ends_with_ci(src_path, ".tar.gz") ||
                  ends_with_ci(src_path, ".tgz");
    }

    int result;

    if (is_gzip) {
        unsigned int raw_size = inflate_gzip_size(buf, (unsigned int)n);
        if (raw_size == 0 || raw_size > TAR_MAX_SIZE) {
            printf("tar: bad or too-large gzip size %u\n", raw_size);
            kfree(buf);
            return -1;
        }

        unsigned char* raw = (unsigned char*)kmalloc(raw_size);
        if (!raw) {
            printf("tar: out of memory for decompression\n");
            kfree(buf);
            return -1;
        }

        if (verbose)
            printf("tar: decompressing %s (%d -> %u bytes)\n",
                   src_path, n, raw_size);

        int decomp = inflate_gzip(buf, (unsigned int)n, raw, raw_size);
        kfree(buf);

        if (decomp < 0) {
            printf("tar: decompression failed\n");
            kfree(raw);
            return -1;
        }

        result = tar_process(raw, (unsigned int)decomp,
                             dst_dir, extract, verbose);
        kfree(raw);
    } else {
        if (verbose && extract)
            printf("tar: reading %s (%d bytes)\n", src_path, n);
        result = tar_process(buf, (unsigned int)n, dst_dir, extract, verbose);
        kfree(buf);
    }

    return result;
}

int tar_extract(const char* src_path, const char* dst_dir, int verbose) {
    return tar_load_and_process(src_path, dst_dir, 1, verbose);
}

int tar_list(const char* src_path) {
    return tar_load_and_process(src_path, "/", 0, 1);
}