/* file.c - the File device and its backing store.
 *
 * The store is deliberately small and entirely in the host's hands: eight
 * slots, a short name, half a kilobyte each. A machine with 64KB of memory
 * cannot usefully juggle much more, and a bounded store cannot be made to
 * exhaust memory by a program that has got confused.
 *
 * The on-disk form is the slots back to back with a header, so saving is one
 * fwrite and loading is one fread. A magic number guards against loading
 * something else by accident.
 */

#include "file.h"

#include <stdio.h>
#include <string.h>

#define STORE_MAGIC   "DUXSTORE"
#define STORE_VERSION 1

typedef struct {
	char name[FILE_NAME_MAX + 1];
	int  len;
	Uint8 data[FILE_DATA_MAX];
} Slot;

static Slot slot[FILE_SLOTS];

/* Port state. A 16-bit port is two consecutive ports, low byte first. */
static Uint16 result;
static Uint16 count;
static Uint16 name_ptr;
static Uint16 data_ptr;
static Uint8  status;

void file_init(void) {
	result = 0;
	count = 0;
	name_ptr = 0;
	data_ptr = 0;
	status = 0;
}

void file_store_reset(void) {
	memset(slot, 0, sizeof slot);
}

/* --------------------------------------------------------------- the store */

int file_store_load(const char *path) {
	FILE *f = fopen(path, "rb");
	char magic[8];
	int version;
	int i;

	memset(slot, 0, sizeof slot);
	if(!f) return 0;                    /* nothing saved yet: not an error */

	if(fread(magic, 1, sizeof magic, f) != sizeof magic ||
	   memcmp(magic, STORE_MAGIC, sizeof magic) != 0) {
		fclose(f);
		return -1;
	}
	if(fread(&version, sizeof version, 1, f) != 1 || version != STORE_VERSION) {
		fclose(f);
		return -1;
	}
	for(i = 0; i < FILE_SLOTS; i++) {
		if(fread(&slot[i], sizeof slot[i], 1, f) != 1) break;
		if(slot[i].len < 0 || slot[i].len > FILE_DATA_MAX) {
			memset(&slot[i], 0, sizeof slot[i]);
			continue;
		}
		slot[i].name[FILE_NAME_MAX] = 0;
	}
	fclose(f);
	return 0;
}

int file_store_save(const char *path) {
	FILE *f = fopen(path, "wb");
	int version = STORE_VERSION;
	int i;

	if(!f) return -1;
	fwrite(STORE_MAGIC, 1, 8, f);
	fwrite(&version, sizeof version, 1, f);
	for(i = 0; i < FILE_SLOTS; i++)
		fwrite(&slot[i], sizeof slot[i], 1, f);
	if(fclose(f) != 0) return -1;
	return 0;
}

/* -------------------------------------------------------------- slot lookup */

/* Copy the name at ptr out of memory, stopping at a NUL and at the width of a
   slot name. A pointer that is not readable yields an empty name, which
   matches no slot, rather than reading past the end of memory. */
static void read_name(Uint16 ptr, char *out) {
	int n = 0;
	out[0] = 0;
	while(n < FILE_NAME_MAX) {
		Uint8 c;
		if((Uint32)ptr + n >= MEM_SIZE) break;
		c = dux.mem[ptr + n];
		if(!c) break;
		out[n++] = (char)c;
	}
	out[n] = 0;
}

static int find_slot(const char *name) {
	int i;
	for(i = 0; i < FILE_SLOTS; i++)
		if(slot[i].name[0] && !strcmp(slot[i].name, name)) return i;
	return -1;
}

/* An empty slot the next new file can take. */
static int free_slot(void) {
	int i;
	for(i = 0; i < FILE_SLOTS; i++)
		if(!slot[i].name[0]) return i;
	return -1;
}

/* --------------------------------------------------------------- operations */

static void do_load(void) {
	char name[FILE_NAME_MAX + 1];
	int i, n;

	read_name(name_ptr, name);
	if(!name[0]) { status = 1; result = 0xffff; return; }

	i = find_slot(name);
	if(i < 0) {                 /* a name that is not there reads as empty */
		result = 0;
		status = 0;
		return;
	}

	/* Clamp to the buffer the program pointed at. Writing past it would
	   corrupt whatever is next in memory, so the store gives up instead of
	   overrunning, and the length reports what actually arrived. */
	n = slot[i].len;
	if((Uint32)data_ptr + n > MEM_SIZE) n = (int)(MEM_SIZE - data_ptr);
	if(n > 0) memcpy(dux.mem + data_ptr, slot[i].data, (size_t)n);
	result = (Uint16)n;
	status = 0;
}

static void do_save(void) {
	char name[FILE_NAME_MAX + 1];
	int i, n;

	read_name(name_ptr, name);
	if(!name[0]) { status = 1; return; }

	n = count;
	if(n > FILE_DATA_MAX) n = FILE_DATA_MAX;
	if((Uint32)data_ptr + n > MEM_SIZE) n = (int)(MEM_SIZE - data_ptr);
	if(n < 0) n = 0;

	i = find_slot(name);
	if(i < 0) {
		i = free_slot();
		if(i < 0) { status = 1; return; }   /* the store is full */
		memset(&slot[i], 0, sizeof slot[i]);
		strncpy(slot[i].name, name, FILE_NAME_MAX);
		slot[i].name[FILE_NAME_MAX] = 0;
	}
	if(n > 0) memcpy(slot[i].data, dux.mem + data_ptr, (size_t)n);
	slot[i].len = n;
	status = 0;
}

/* -------------------------------------------------------------------- ports
 *
 *   0xa0/0xa1  vector        0xa4/0xa5  count
 *   0xa2/0xa3  result        0xa6       operation
 *                           0xa7       status
 *   0xa8/0xa9  name pointer  0xaa/0xab  data pointer
 *
 * A 16-bit value lives in two consecutive ports, low byte first, so the base
 * address of a pair is always even and these helpers can rely on that.
 */

/* One half of a short port. The high byte sits at the base address, which is
   how DEO2 and DEI2 both order a device pair. */
static Uint16 merge(Uint16 old, Uint8 addr, Uint8 val) {
	if(addr & 1) return (Uint16)((old & 0xff00) | val);
	return (Uint16)((old & 0x00ff) | (val << 8));
}

Uint8 file_read(Uint8 addr) {
	Uint16 v;
	if(addr == 0xa7) return status;       /* the odd half of no pair */
	switch(addr & 0xfe) {
	case 0xa0: v = (Uint16)((dux.mem[IO_BASE + 0xa0] << 8) |
	                        dux.mem[IO_BASE + 0xa1]);     /* vector */
	           break;
	case 0xa2: v = result;  break;
	case 0xa4: v = count;   break;
	case 0xa8: v = name_ptr; break;
	case 0xaa: v = data_ptr; break;
	default:   return dux.mem[IO_BASE + addr];
	}
	return (addr & 1) ? (Uint8)v : (Uint8)(v >> 8);
}

void file_write(Uint8 addr, Uint8 val) {
	if(addr == 0xa6) {                   /* the operation byte */
		if(val == 1) do_load();
		else if(val == 2) do_save();
		return;
	}
	if(addr == 0xa7) { status = 0; return; }
	switch(addr & 0xfe) {
	case 0xa0:                       /* vector, kept in shadow memory */
		dux.mem[IO_BASE + addr] = val;
		return;
	case 0xa2: result  = merge(result,  addr, val); return;
	case 0xa4: count   = merge(count,   addr, val); return;
	case 0xa8: name_ptr = merge(name_ptr, addr, val); return;
	case 0xaa: data_ptr = merge(data_ptr, addr, val); return;
	default:
		dux.mem[IO_BASE + addr] = val;
		return;
	}
}