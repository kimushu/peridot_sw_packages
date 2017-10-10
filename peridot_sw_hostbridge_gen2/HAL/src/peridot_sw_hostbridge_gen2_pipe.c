#include "peridot_sw_hostbridge_gen2.h"
#include "sys/alt_dev.h"
#include "priv/alt_file.h"
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <stdlib.h>

inline static uint32_t roundup_pow2(uint32_t x)
{
    --x;
    x |= (x >>  1);
    x |= (x >>  2);
    x |= (x >>  4);
    x |= (x >>  8);
    x |= (x >> 16);
    return ++x;
}

typedef struct hostbridge_pipe_s {
    hostbridge_channel channel;
    ALT_SEM(sem_lock);
    ALT_SEM(sem_read);
    size_t capacity;
    size_t read_offset;
    size_t write_offset;
    char buffer[0];
} hostbridge_pipe;

static int hostbridge_pipe_read(alt_fd *fd, char *ptr, int len);
static int hostbridge_pipe_write(alt_fd *fd, const char *ptr, int len);

static const alt_dev hostbridge_pipe_dev = {
    .read = hostbridge_pipe_read,
    .write = hostbridge_pipe_write,
};

static int hostbridge_pipe_read(alt_fd *fd, char *ptr, int len)
{
    hostbridge_pipe *pipe = (hostbridge_pipe *)fd->priv;
    int read_offset, write_offset;
    int avail1, avail2;

retry:
    ALT_SEM_PEND(pipe->sem_lock, 0);
    read_offset = pipe->read_offset;
    write_offset = pipe->write_offset;
    ALT_SEM_POST(pipe->sem_lock);

    if (read_offset <= write_offset) {
        // ----r----w----
        //     aaaaa
        avail1 = write_offset - read_offset;
        if (avail1 == 0) {
            ALT_SEM_PEND(pipe->sem_read, 0);
            goto retry;
        }
        avail2 = 0;
    } else {
        // ----w----r----
        // aaaa     aaaaa
        avail1 = pipe->capacity - read_offset;
        avail2 = write_offset;
    }

    if (len < avail1) {
        avail1 = len;
        avail2 = 0;
    } else {
        len -= avail1;
        if (len < avail2) {
            avail2 = len;
        }
    }

    memcpy(ptr, pipe->buffer + read_offset, avail1);
    if (avail2 > 0) {
        memcpy(ptr + avail1, pipe->buffer, avail2);
        pipe->read_offset = avail2;
    } else {
        pipe->read_offset = (read_offset + avail1) & (pipe->capacity - 1);
    }

    return (avail1 + avail2);
}

static int hostbridge_pipe_write(alt_fd *fd, const char *ptr, int len)
{
    hostbridge_pipe *pipe = (hostbridge_pipe *)fd->priv;
    int result = peridot_sw_hostbridge_gen2_source(&pipe->channel, ptr, len, 0);
    if (result < 0) {
        return result;
    }
    return len;
}

static int hostbridge_pipe_sink(hostbridge_channel *channel, const void *ptr, int len)
{
    hostbridge_pipe *pipe = (hostbridge_pipe *)channel;
    int read_offset, write_offset;
    int free1, free2;

    ALT_SEM_PEND(pipe->sem_lock, 0);
    read_offset = pipe->read_offset;
    write_offset = pipe->write_offset;
    ALT_SEM_POST(pipe->sem_lock);

    if (read_offset <= write_offset) {
        // ----r----w----
        // fff-     fffff
        free1 = pipe->capacity - write_offset;
        free2 = read_offset - 1;
        if (free2 < 0) {
            --free1;
            free2 = 0;
        }
    } else {
        // ----w----r----
        //     ffff-
        free1 = read_offset - write_offset - 1;
        free2 = 0;
    }

    if (free1 == 0) {
        // Buffer overflow => Drop data
        return 0;
    }

    if (len < free1) {
        free1 = len;
        free2 = 0;
    } else {
        len -= free1;
        if (len < free2) {
            free2 = len;
        }
    }

    memcpy(pipe->buffer + write_offset, ptr, free1);
    if (free2 > 0) {
        memcpy(pipe->buffer, ptr + free1, free2);
        pipe->write_offset = free2;
    } else {
        pipe->write_offset = (write_offset + free1) & (pipe->capacity - 1);
    }

    if (read_offset == write_offset) {
        ALT_SEM_POST(pipe->sem_read);
    }

    return (free1 + free2);
}

int peridot_sw_hostbridge_gen2_mkpipe(alt_u8 channel, int output_fd, int input_fd, size_t input_capacity)
{
    hostbridge_pipe *pipe;
    int result;

    if (input_fd >= 0) {
        input_capacity = roundup_pow2(input_capacity);
    } else if (output_fd < 0) {
        return -EINVAL;
    }

    pipe = (hostbridge_pipe *)malloc(
        (input_fd >= 0) ? (sizeof(*pipe) + input_capacity) : sizeof(pipe->channel)
    );
    if (!pipe) {
        return -ENOMEM;
    }

    memset(&pipe->channel, 0, sizeof(pipe->channel));
    pipe->channel.number = channel;
    if (input_fd >= 0) {
        pipe->channel.dest.sink = hostbridge_pipe_sink;
        ALT_SEM_CREATE(&pipe->sem_lock, 1);
        ALT_SEM_CREATE(&pipe->sem_read, 0);
        pipe->capacity = input_capacity;
        pipe->read_offset = 0;
        pipe->write_offset = 0;
    }

    result = peridot_sw_hostbridge_gen2_register_channel(&pipe->channel);
    if (result < 0) {
        free(pipe);
        return result;
    }

    if (output_fd >= 0) {
        alt_fd *fd = &alt_fd_list[output_fd];
        if (fd->dev) {
            close(output_fd);
        }
        fd->dev = (alt_dev *)&hostbridge_pipe_dev;
        fd->priv = (alt_u8 *)pipe;
        fd->fd_flags = (input_fd == output_fd) ? O_RDWR : O_WRONLY;
    }
    if ((input_fd >= 0) && (input_fd != output_fd)) {
        alt_fd *fd = &alt_fd_list[input_fd];
        if (fd->dev) {
            close(input_fd);
        }
        fd->dev = (alt_dev *)&hostbridge_pipe_dev;
        fd->priv = (alt_u8 *)pipe;
        fd->fd_flags = O_RDONLY;
    }

    return 0;
}
