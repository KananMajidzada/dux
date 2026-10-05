/* file.h - the File device: a small persistent store.
 *
 * Eight named slots, held in the host and written to a single file on demand.
 * For a machine meant to be kept rather than reinstalled, storage that vanishes
 * when the process exits is storage that does not exist. The store is loaded
 * once at startup and flushed at exit, so a program sees a filesystem that
 * survives being closed.
 *
 * Port map, device 0xa0:
 *
 *   0xa0/0xa1  vector
 *   0xa2/0xa3  result    after a load: the length read, or 0xffff on failure
 *   0xa4/0xa5  count     for a save: how many bytes to write
 *   0xa6       operation write 1 to load, 2 to save
 *   0xa7       status    0 idle, 1 error
 *   0xa8/0xa9  name      pointer to a NUL-terminated name in memory
 *   0xaa/0xab  data      pointer to the buffer to read or write
 *
 * Loading a name that does not exist is not an error: it yields zero bytes,
 * which is what a program creating a new file wants.
 */

#ifndef FILE_H
#define FILE_H

#include "dux.h"

#define FILE_SLOTS     8
#define FILE_NAME_MAX  15
#define FILE_DATA_MAX  512

void file_init(void);

/* Read and write the backing store. Both return 0 on success. Missing files
   are not an error to load; there is simply nothing in them yet. */
int  file_store_load(const char *path);
int  file_store_save(const char *path);

/* Release the slots without writing, for a front end that would rather not
   touch the filesystem. */
void file_store_reset(void);

Uint8 file_read(Uint8 addr);
void  file_write(Uint8 addr, Uint8 val);

#endif /* FILE_H */