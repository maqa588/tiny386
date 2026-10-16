#ifndef ISOFS_H
#define ISOFS_H

#include <stdint.h>

typedef struct ISOFolder ISOFolder;

/*
 * Streaming ISO9660 view over a host folder.
 *
 * The disc metadata (volume descriptors, path tables and directory records)
 * is generated once into a small RAM buffer; file *contents* are never
 * cached and are read straight from the host/SD files on demand.  This keeps
 * the RAM footprint at a few tens of KB even for large folders.
 *
 * Files with .iso/.img/.vhd extensions and any host path listed in
 * `forbidden` (NULL-terminated, may be NULL) are excluded from the image, so
 * the virtual disc can never contain the currently mounted images.
 *
 * Returns NULL if the folder is missing, unreadable or empty.
 */
ISOFolder *isofs_open(const char *folder, const char *const *forbidden);

void isofs_close(ISOFolder *fs);

/* Total disc size in bytes (always a multiple of 2048). */
uint64_t isofs_size(ISOFolder *fs);

/* Number of files included in the image. */
int isofs_file_count(ISOFolder *fs);

/* Read exactly `len` bytes at `offset` into `buf`.  Bytes past the end of an
 * individual file (sector padding) are zero filled.  Returns 0 on success,
 * -1 on error. */
int isofs_read(ISOFolder *fs, uint64_t offset, void *buf, uint32_t len);

#endif /* ISOFS_H */
