#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "named_fifo.h"
#include "system.h"
#include "sys/alt_llist.h"
#include "priv/alt_file.h"

static int named_fifo_read(alt_fd *fd, char *ptr, int len)
{
	named_fifo_dev *dev = (named_fifo_dev *)fd->dev;
	int read_len = 0;

	if (!(((fd->fd_flags & O_ACCMODE) + 1) & _FREAD)) {
		return -EACCES;
	}

	ALT_SEM_PEND(dev->lock_read, 0);

retry:
	ALT_SEM_PEND(dev->lock_common, 0);
	dev->flags &= ~NAMED_FIFO_FLAG_READ_BLOCKED;

	while (len > 0) {
		int chunk_len;
		int read_offset = dev->read_offset;

		if (dev->read_chunk != dev->write_chunk) {
			// All of the rest of read_offset is readable
			chunk_len = dev->chunk_size - read_offset;
		} else if (dev->flags & NAMED_FIFO_FLAG_BUFFER_FULL) {
			// Buffer is full
			chunk_len = dev->chunk_size - read_offset;
		} else if (read_offset <= dev->write_offset) {
			// Ring buffer (read <= write)
			chunk_len = dev->write_offset - read_offset;
		} else {
			// Ring buffer (write < read)
			chunk_len = dev->chunk_size - read_offset;
		}

		if (chunk_len == 0) {
			// No data to read
			if (fd->fd_flags & O_NONBLOCK) {
				if (read_len == 0) {
					read_len = -EAGAIN;
				}
				break;
			}
			dev->flags |= NAMED_FIFO_FLAG_READ_BLOCKED;
			break;
		}

		if (chunk_len > len) {
			chunk_len = len;
		}

		memcpy(ptr, dev->read_chunk->data + read_offset, chunk_len);
		ptr += chunk_len;
		len -= chunk_len;
		read_len += chunk_len;
		dev->flags &= ~NAMED_FIFO_FLAG_BUFFER_FULL;
		read_offset = (read_offset + chunk_len) & (dev->chunk_size - 1);
		if (read_offset == 0) {
			named_fifo_chunk *next = dev->read_chunk->next;
			if (dev->flags & NAMED_FIFO_FLAG_AUTO_GROWTH) {
				free(dev->read_chunk);
			}
			dev->read_chunk = next;
		}
		dev->read_offset = read_offset;
	}

	if (dev->flags & NAMED_FIFO_FLAG_WRITE_BLOCKED) {
		ALT_SEM_POST(dev->awake_write);
	}
	ALT_SEM_POST(dev->lock_common);

	if (dev->flags & NAMED_FIFO_FLAG_READ_BLOCKED) {
		ALT_SEM_PEND(dev->awake_read, 0);
		goto retry;
	}
	ALT_SEM_POST(dev->lock_read);

	return read_len;
}

static int named_fifo_write(alt_fd *fd, const char *ptr, int len)
{
	named_fifo_dev *dev = (named_fifo_dev *)fd->dev;
	int written_len = 0;

	if (!(((fd->fd_flags & O_ACCMODE) + 1) & _FWRITE)) {
		return -EACCES;
	}

	ALT_SEM_PEND(dev->lock_write, 0);
retry:
	ALT_SEM_PEND(dev->lock_common, 0);
	dev->flags &= ~NAMED_FIFO_FLAG_WRITE_BLOCKED;

	while (len > 0) {
		int chunk_len;
		int write_offset = dev->write_offset;

		if ((dev->read_chunk != dev->write_chunk) ||
			((!(dev->flags & NAMED_FIFO_FLAG_BACK_PRESSURE)) &&
			 (!(dev->flags & NAMED_FIFO_FLAG_READ_BLOCKED)))) {
			// All of the rest of write_offset is writable
			chunk_len = dev->chunk_size - write_offset;
		} else if (dev->flags & NAMED_FIFO_FLAG_BUFFER_FULL) {
			// Buffer is full
			chunk_len = 0;
		} else if (dev->read_offset <= write_offset) {
			// Ring buffer (read <= write)
			chunk_len = dev->chunk_size - write_offset;
		} else {
			// Ring buffer (write < read)
			chunk_len = dev->read_offset - write_offset;
		}

		if (chunk_len == 0) {
			// No space to write
			if (fd->fd_flags & O_NONBLOCK) {
				if (written_len == 0) {
					written_len = -EAGAIN;
				}
				break;
			}
			dev->flags |= NAMED_FIFO_FLAG_WRITE_BLOCKED;
			break;
		}

		if (chunk_len > len) {
			chunk_len = len;
		}

		memcpy(dev->write_chunk->data + write_offset, ptr, chunk_len);
		ptr += chunk_len;
		len -= chunk_len;
		written_len += chunk_len;
		write_offset = (write_offset + chunk_len);
		if (dev->read_chunk == dev->write_chunk) {
			if ((dev->flags & NAMED_FIFO_FLAG_BUFFER_FULL) ||
				((dev->read_offset >= dev->write_offset) &&
				 (dev->read_offset <= write_offset))) {
				// old data may be discarded
				dev->read_offset = write_offset & (dev->chunk_size - 1);
				dev->flags |= NAMED_FIFO_FLAG_BUFFER_FULL;
			}
		}
		write_offset &= (dev->chunk_size - 1);
		if (write_offset == 0) {
			named_fifo_chunk *next;
			if (dev->flags & NAMED_FIFO_FLAG_AUTO_GROWTH) {
				next = (named_fifo_chunk *)malloc(sizeof(*next) + dev->chunk_size);
				if (!next) {
					written_len = -ENOMEM;
					break;
				}
				dev->write_chunk->next = next;
				next->next = NULL;
			} else {
				next = dev->write_chunk->next;
			}
			dev->write_chunk = next;
		}
		dev->write_offset = write_offset;
	}

	if (dev->flags & NAMED_FIFO_FLAG_READ_BLOCKED) {
		ALT_SEM_POST(dev->awake_read);
	}
	ALT_SEM_POST(dev->lock_common);

	if (dev->flags & NAMED_FIFO_FLAG_WRITE_BLOCKED) {
		ALT_SEM_PEND(dev->awake_write, 0);
		goto retry;
	}
	ALT_SEM_POST(dev->lock_write);

	return written_len;
}

static const alt_dev named_fifo_dev_template = {
	ALT_LLIST_ENTRY,
	NULL, /* filled in named_fifo_create */
	NULL, /* open */
	NULL, /* close */
	named_fifo_read,
	named_fifo_write,
	NULL, /* lseek */
	NULL, /* fstat */
	NULL, /* ioctl */
};

#if (NAMED_FIFO_STDIN_ENABLE) || (NAMED_FIFO_STDOUT_ENABLE) || (NAMED_FIFO_STDERR_ENABLE)
static void named_fifo_enable_stdio(int new_fd, int flags, const char *name, int size, int back_pressure)
{
	int old_fd;
	alt_fd *fd = &alt_fd_list[new_fd];

	named_fifo_create(name, size, back_pressure);
	old_fd = open(name, flags, 0);
	if (old_fd >= 0) {
		fd->dev      = alt_fd_list[old_fd].dev;
		fd->priv     = alt_fd_list[old_fd].priv;
		fd->fd_flags = alt_fd_list[old_fd].fd_flags;
		alt_release_fd(old_fd);
	}
}
#endif  /* (NAMED_FIFO_STDIN_ENABLE) || (NAMED_FIFO_STDOUT_ENABLE) || (NAMED_FIFO_STDERR_ENABLE) */

void named_fifo_init(void)
{
#if (NAMED_FIFO_STDIN_ENABLE)
# ifdef ALT_STDIN_PRESENT
#  error "To use named FIFO as stdin, change hal.stdin to 'none'"
# endif
	named_fifo_enable_stdio(STDIN_FILENO, O_RDONLY,
			NAMED_FIFO_STDIN_NAME, NAMED_FIFO_STDIN_SIZE,
			NAMED_FIFO_STDIN_BACK_PRESSURE);
#endif
#if (NAMED_FIFO_STDOUT_ENABLE)
# ifdef ALT_STDOUT_PRESENT
#  error "To use named FIFO as stdout, change hal.stdout to 'none'"
# endif
	named_fifo_enable_stdio(STDOUT_FILENO, O_WRONLY,
			NAMED_FIFO_STDOUT_NAME, NAMED_FIFO_STDOUT_SIZE,
			NAMED_FIFO_STDOUT_BACK_PRESSURE);
#endif
#if (NAMED_FIFO_STDERR_ENABLE)
# ifdef ALT_STDERR_PRESENT
#  error "To use named FIFO as stderr, change hal.stderr to 'none'"
# endif
	named_fifo_enable_stdio(STDERR_FILENO, O_WRONLY,
			NAMED_FIFO_STDERR_NAME, NAMED_FIFO_STDERR_SIZE,
			NAMED_FIFO_STDERR_BACK_PRESSURE);
#endif
}

int named_fifo_create(const char *name, int max_size, int back_pressure)
{
	int namelen = strlen(name) + 1;
	named_fifo_dev *dev;
	named_fifo_chunk *chunk;

	if (max_size < 0) {
		return -EINVAL;
	} else if (max_size > 0) {
		// Round up max_size to power of 2
		--max_size;
		max_size |= (max_size >> 1);
		max_size |= (max_size >> 2);
		max_size |= (max_size >> 4);
		max_size |= (max_size >> 8);
		max_size |= (max_size >> 16);
		++max_size;
	}

	dev = (named_fifo_dev *)malloc(sizeof(*dev) + namelen);
	if (!dev) {
		return -ENOMEM;
	}

	memcpy(dev + 1, name, namelen);
	memcpy(&dev->dev, &named_fifo_dev_template, sizeof(dev->dev));
	dev->dev.name = (const char *)(dev + 1);
	dev->flags = (max_size == 0 ? NAMED_FIFO_FLAG_AUTO_GROWTH : 0) |
			(back_pressure ? NAMED_FIFO_FLAG_BACK_PRESSURE : 0);
	dev->chunk_size = (max_size == 0 ? NAMED_FIFO_DEFAULT_CHUNK_SIZE : max_size);

	chunk = (named_fifo_chunk *)malloc(sizeof(*chunk) + dev->chunk_size);
	if (!chunk) {
		free(dev);
		return -ENOMEM;
	}

	if (dev->flags & NAMED_FIFO_FLAG_AUTO_GROWTH) {
		chunk->next = NULL;
	} else {
		chunk->next = chunk;
	}

	dev->read_offset = 0;
	dev->read_chunk = chunk;
	dev->write_offset = 0;
	dev->write_chunk = chunk;
	ALT_SEM_CREATE(&dev->lock_read, 1);
	ALT_SEM_CREATE(&dev->lock_write, 1);
	ALT_SEM_CREATE(&dev->lock_common, 1);
	ALT_SEM_CREATE(&dev->awake_read, 0);
	ALT_SEM_CREATE(&dev->awake_write, 0);

	alt_dev_reg(&dev->dev);
	return 0;
}

int mkfifo(const char *name, mode_t mode)
{
	return named_fifo_create(name, 0, 1);
}

