#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "named_fifo.h"
#include "system.h"
#include "sys/alt_llist.h"
#include "priv/alt_file.h"

static int named_fifo_open(alt_fd *fd, const char *file, int flags, int mode)
{
	named_fifo_dev *dev = (named_fifo_dev *)fd->dev;
	int accmode = (flags & O_ACCMODE) + 1;

	ALT_SEM_PEND(dev->lock_common, 0);
	if (accmode & _FREAD) {
		++dev->readers;
		dev->flags &= ~NAMED_FIFO_FLAG_READER_CLOSED;
	}
	if (accmode & _FWRITE) {
		++dev->writers;
		dev->flags &= ~NAMED_FIFO_FLAG_WRITER_CLOSED;
	}
	ALT_SEM_POST(dev->lock_common);

	return 0;
}

static int named_fifo_close(alt_fd *fd)
{
	named_fifo_dev *dev = (named_fifo_dev *)fd->dev;
	int accmode = (fd->fd_flags & O_ACCMODE) + 1;

	ALT_SEM_PEND(dev->lock_common, 0);
	if (accmode & _FREAD) {
		if (--dev->readers == 0) {
			dev->flags |= NAMED_FIFO_FLAG_READER_CLOSED;
			ALT_SEM_POST(dev->sem_reader);
		}
	}
	if (accmode & _FWRITE) {
		if (--dev->writers == 0) {
			dev->flags |= NAMED_FIFO_FLAG_WRITER_CLOSED;
			ALT_SEM_POST(dev->sem_writer);
		}
	}
	ALT_SEM_POST(dev->lock_common);

	return 0;
}

static int named_fifo_read(alt_fd *fd, char *ptr, int len)
{
	named_fifo_dev *dev = (named_fifo_dev *)fd->dev;
	size_t write_offset;
	ssize_t readable1, readable2;
	
	if (!(((fd->fd_flags & O_ACCMODE) + 1) & _FREAD)) {
		return -EACCES;
	}

	if (len == 0) {
		return 0;
	}

	// Wait for data
retry:
	ALT_SEM_PEND(dev->sem_reader, 0);
	write_offset = dev->write_offset;

	if (dev->read_offset < write_offset) {
		readable1 = write_offset - dev->read_offset;
		readable2 = 0;
	} else {
		readable1 = dev->capacity - dev->read_offset;
		readable2 = write_offset;
		if ((dev->read_offset == write_offset) &&
			!(dev->flags & NAMED_FIFO_FLAG_BUFFER_FULL)) {
			// No data to read now
			ALT_SEM_POST(dev->sem_reader);

			if (dev->flags & NAMED_FIFO_FLAG_WRITER_CLOSED) {
				// Closed pipe (No writer)
				return 0;
			}

			if (fd->fd_flags & O_NONBLOCK) {
				return -EWOULDBLOCK;
			}
			goto retry;
		}
	}

	// Adjustment read length
	if (len < readable1) {
		readable1 = len;
		readable2 = 0;
	} else {
		len -= readable1;
		if (len < readable2) {
			readable2 = len;
		}
	}
	len = readable1 + readable2;

	// Data transfer
	memcpy(ptr, dev->buffer + dev->read_offset, readable1);
	if (readable2 > 0) {
		memcpy(ptr + readable1, dev->buffer, readable2);
	}

	// Update offset & flags
	ALT_SEM_PEND(dev->lock_common, 0);
	dev->read_offset = (dev->read_offset + len) & (dev->capacity - 1);
	dev->flags &= ~NAMED_FIFO_FLAG_BUFFER_FULL;
	ALT_SEM_POST(dev->lock_common);

	ALT_SEM_POST(dev->sem_reader);
	ALT_SEM_POST(dev->sem_writer);
	
	return len;
}

static int named_fifo_write(alt_fd *fd, const char *ptr, int len)
{
	named_fifo_dev *dev = (named_fifo_dev *)fd->dev;
	size_t read_offset;
	ssize_t writable1, writable2;

	if (!(((fd->fd_flags & O_ACCMODE) + 1) & _FWRITE)) {
		return -EACCES;
	}

	if (len == 0) {
		return 0;
	}

	// Wait for space
retry:
	ALT_SEM_PEND(dev->sem_writer, 0);
	read_offset = dev->read_offset;

	if (dev->write_offset < read_offset) {
		writable1 = read_offset - dev->write_offset;
		writable2 = 0;
	} else {
		writable1 = dev->capacity - dev->write_offset;
		writable2 = read_offset;
		if ((read_offset == dev->write_offset) &&
			!(dev->flags & NAMED_FIFO_FLAG_BUFFER_FULL)) {
			// No space to write now
			ALT_SEM_POST(dev->sem_writer);

			if (dev->flags & NAMED_FIFO_FLAG_READER_CLOSED) {
				// Closed pipe (No reader)
				return -EPIPE;
			}

			if (fd->fd_flags & O_NONBLOCK) {
				return -EWOULDBLOCK;
			}
			goto retry;
		}
	}

	// Adjustment write length
	if (len < writable1) {
		writable1 = len;
		writable2 = 0;
	} else {
		len -= writable1;
		if (len < writable2) {
			writable2 = len;
		}
	}
	len = writable1 + writable2;

	// Data transfer
	memcpy(dev->buffer + dev->write_offset, ptr, writable1);
	if (writable2 > 0) {
		memcpy(dev->buffer, ptr + writable1, writable2);
	}

	// Update offset & flags
	ALT_SEM_PEND(dev->lock_common, 0);
	dev->write_offset = (dev->write_offset + len) & (dev->capacity - 1);
	if (dev->write_offset == dev->read_offset) {
		dev->flags |= NAMED_FIFO_FLAG_BUFFER_FULL;
	}

	ALT_SEM_POST(dev->sem_writer);
	ALT_SEM_POST(dev->sem_reader);

	return len;
}

static const alt_dev named_fifo_dev_template = {
	ALT_LLIST_ENTRY,
	NULL, /* name: filled in named_fifo_create */
	named_fifo_open,
	named_fifo_close,
	named_fifo_read,
	named_fifo_write,
	NULL, /* lseek */
	NULL, /* fstat */
	NULL, /* ioctl */
};

#if (NAMED_FIFO_STDIN_ENABLE) || (NAMED_FIFO_STDOUT_ENABLE) || (NAMED_FIFO_STDERR_ENABLE)
static void redirect_fd(int new_fd, const char *name, int flags)
{
	int old_fd;
	alt_fd *fd = &alt_fd_list[new_fd];
	close(new_fd);
	old_fd = open(name, flags, 0);
	if (old_fd >= 0) {
		fd->dev      = alt_fd_list[old_fd].dev;
		fd->priv     = alt_fd_list[old_fd].priv;
		fd->fd_flags = alt_fd_list[old_fd].fd_flags;
		alt_release_fd(old_fd);
	}
}
#endif  /* (NAMED_FIFO_STDIN_ENABLE) || (NAMED_FIFO_STDOUT_ENABLE) || (NAMED_FIFO_STDERR_ENABLE) */

void named_fifo_open_stdio(void)
{
#if (NAMED_FIFO_STDIN_ENABLE)
	redirect_fd(STDIN_FILENO, NAMED_FIFO_STDIN_NAME, O_RDONLY);
#endif
#if (NAMED_FIFO_STDOUT_ENABLE)
	redirect_fd(STDOUT_FILENO, NAMED_FIFO_STDOUT_NAME, O_RDONLY);
#endif
#if (NAMED_FIFO_STDERR_ENABLE)
	redirect_fd(STDERR_FILENO, NAMED_FIFO_STDERR_NAME, O_RDONLY);
#endif
}

void named_fifo_close_stdio(void)
{
#if (NAMED_FIFO_STDIN_ENABLE)
	redirect_fd(STDIN_FILENO, "/dev/null", O_RDONLY);
#endif
#if (NAMED_FIFO_STDOUT_ENABLE)
	redirect_fd(STDOUT_FILENO, "/dev/null", O_RDONLY);
#endif
#if (NAMED_FIFO_STDERR_ENABLE)
	redirect_fd(STDERR_FILENO, "/dev/null", O_RDONLY);
#endif
}

void named_fifo_init(void)
{
#if (NAMED_FIFO_STDIN_ENABLE)
# ifdef ALT_STDIN_PRESENT
#  error "To use named FIFO as stdin, change hal.stdin to 'none'"
# endif
	named_fifo_create(NAMED_FIFO_STDIN_NAME, NAMED_FIFO_STDIN_SIZE);
#endif
#if (NAMED_FIFO_STDOUT_ENABLE)
# ifdef ALT_STDOUT_PRESENT
#  error "To use named FIFO as stdout, change hal.stdout to 'none'"
# endif
	named_fifo_create(NAMED_FIFO_STDOUT_NAME, NAMED_FIFO_STDOUT_SIZE);
#endif
#if (NAMED_FIFO_STDERR_ENABLE)
# ifdef ALT_STDERR_PRESENT
#  error "To use named FIFO as stderr, change hal.stderr to 'none'"
# endif
	named_fifo_create(NAMED_FIFO_STDERR_NAME, NAMED_FIFO_STDERR_SIZE);
#endif
#if (NAMED_FIFO_STDIN_ENABLE) || (NAMED_FIFO_STDOUT_ENABLE) || (NAMED_FIFO_STDERR_ENABLE)
# if (NAMED_FIFO_STDIO_INIT_OPENED)
	named_fifo_open_stdio();
# endif
#endif
}

int named_fifo_create(const char *name, size_t size)
{
	int namelen = strlen(name) + 1;
	named_fifo_dev *dev;

	if (size <= NAMED_FIFO_MINIMUM_SIZE) {
		size = NAMED_FIFO_MINIMUM_SIZE;
	}

	// Round up size to power of 2
	--size;
	size |= (size >> 1);
	size |= (size >> 2);
	size |= (size >> 4);
	size |= (size >> 8);
	size |= (size >> 16);
	++size;

	dev = (named_fifo_dev *)malloc(sizeof(*dev) + size + namelen);
	if (!dev) {
		return -ENOMEM;
	}

	memcpy(&dev->dev, &named_fifo_dev_template, sizeof(dev->dev));
	dev->buffer = (alt_u8 *)(dev + 1);
	dev->dev.name = (const char *)(dev->buffer + size);
	memcpy((char *)dev->dev.name, name, namelen);

	dev->flags = 0;
	dev->reserved = 0;
	dev->readers = 0;
	dev->writers = 0;
	dev->capacity = size;
	dev->read_offset = 0;
	dev->write_offset = 0;

	ALT_SEM_CREATE(&dev->lock_common, 1);
	ALT_SEM_CREATE(&dev->sem_reader, 0);
	ALT_SEM_CREATE(&dev->sem_writer, 0);

	return alt_dev_reg(&dev->dev);
}

int mkfifo(const char *name, mode_t mode)
{
	return named_fifo_create(name, 0);
}
