#ifndef __NAMED_FIFO_H__
#define __NAMED_FIFO_H__

#include "sys/stat.h"
#include "sys/alt_dev.h"
#include "os/alt_sem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NAMED_FIFO_DEFAULT_CHUNK_SIZE (8 * 1024)

enum {
	NAMED_FIFO_FLAG_BUFFER_FULL   = (1<<0),
	NAMED_FIFO_FLAG_READ_BLOCKED  = (1<<1),
	NAMED_FIFO_FLAG_WRITE_BLOCKED = (1<<2),
	NAMED_FIFO_FLAG_BACK_PRESSURE = (1<<3),
	NAMED_FIFO_FLAG_AUTO_GROWTH   = (1<<4),
};

typedef struct named_fifo_chunk_s {
	struct named_fifo_chunk_s *next;
	char data[0];
} named_fifo_chunk;

typedef struct named_fifo_dev_s {
	alt_dev dev;
	int flags;
	int chunk_size;
	int read_offset;
	named_fifo_chunk *read_chunk;
	int write_offset;
	named_fifo_chunk *write_chunk;
	ALT_SEM(lock_read);
	ALT_SEM(lock_write);
	ALT_SEM(lock_common);
	ALT_SEM(awake_read);
	ALT_SEM(awake_write);
} named_fifo_dev;

extern void named_fifo_init(void);
extern int named_fifo_create(const char *name, int max_size, int back_pressure);
extern int mkfifo(const char *name, mode_t mode);

#define NAMED_FIFO_INSTANCE(name, state) extern int alt_no_storage
#define NAMED_FIFO_INIT(name, state) named_fifo_init()

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __NAMED_FIFO_H__ */
