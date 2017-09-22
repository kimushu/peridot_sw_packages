#ifndef __NAMED_FIFO_H__
#define __NAMED_FIFO_H__

#include "sys/stat.h"
#include "sys/alt_dev.h"
#include "os/alt_sem.h"
#include "alt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NAMED_FIFO_MINIMUM_SIZE     (256)

enum {
	NAMED_FIFO_FLAG_BUFFER_FULL   = (1<<0),
	NAMED_FIFO_FLAG_READER_CLOSED = (1<<1),
	NAMED_FIFO_FLAG_WRITER_CLOSED = (1<<2),
};

typedef struct named_fifo_dev_s {
	alt_dev dev;
	alt_u16 flags;
	alt_u16 reserved;
	alt_u16 readers;
	alt_u16 writers;
	size_t capacity;
	size_t read_offset;
	size_t write_offset;
	alt_u8 *buffer;
	ALT_SEM(lock_common);
	ALT_SEM(sem_reader);
	ALT_SEM(sem_writer);
} named_fifo_dev;

extern void named_fifo_init(void);
extern void named_fifo_open_stdio(void);
extern void named_fifo_close_stdio(void);
extern int named_fifo_create(const char *name, size_t size);
extern int mkfifo(const char *name, mode_t mode);

#define NAMED_FIFO_INSTANCE(name, state) extern int alt_no_storage
#define NAMED_FIFO_INIT(name, state) named_fifo_init()

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __NAMED_FIFO_H__ */
