#ifndef TAR_H
#define TAR_H

/*
 * tar.h — ustar/POSIX tar extractor for Computer OS.
 *
 * Supports uncompressed .tar files stored on any VFS-mounted filesystem.
 * Handles regular files, hard links, symbolic links (stored as regular
 * files containing the link target), and directories.
 *
 * GNU tar extensions supported:
 *   - Long filenames via GNU @LongLink (type 'L') blocks
 *   - Long link targets via GNU @LongLink (type 'K') blocks
 *
 * gzip (.tar.gz / .tgz) decompression is NOT done here; add a gzip
 * layer on top and pass the decompressed stream to tar_extract_mem()
 * once you have it.
 */

/*
 * Extract a .tar file from the VFS into a destination directory.
 *
 *   src_path   — path to the .tar file on any mounted filesystem
 *   dst_dir    — destination directory (must already exist, e.g. "/")
 *   verbose    — if non-zero, print each extracted path
 *
 * Returns the number of entries extracted on success, or -1 on error.
 */
int tar_extract(const char* src_path, const char* dst_dir, int verbose);

/*
 * List the contents of a .tar file without extracting anything.
 * Prints one line per entry in the style of `tar -tv`.
 *
 * Returns the number of entries listed, or -1 on error.
 */
int tar_list(const char* src_path);

#endif /* TAR_H */