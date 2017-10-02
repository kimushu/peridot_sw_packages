#include "system.h"
#include "peridot_sw_hostbridge_gen2.h"
#include <unistd.h>
#include <sys/fcntl.h>
#include <string.h>
#include <errno.h>
#include "os/alt_sem.h"

#ifndef HOSTBRIDGE_NAME
# error "peridot_sw_hostbridge_gen2 requires UART device named 'hostbridge'!"
#endif

#define PERIDOT_SW_HOSTBRIDGE_PORT hostbridge
#define PERIDOT_SW_HOSTBRIDGE_PATH "/dev/hostbridge"

#ifdef ALT_USE_DIRECT_DRIVERS
# include "sys/alt_driver.h"
#endif

#define READ_BUFFER_LEN     128
#define WRITE_BUFFER_LEN    128

#ifdef __tinythreads__
# include <pthread.h>
# include <sched.h>
# define YIELD()    sched_yield()
#else
# define YIELD()    (void)
#endif

static struct {
    hostbridge_channel *channels;
    hostbridge_channel *sink_channel;
    alt_16 source_channel_number;
    alt_u8 channel_prefix;
    alt_u8 escape_prefix;
    ALT_SEM(lock);
#ifndef ALT_USE_DIRECT_DRIVERS
    int fd;
#endif
} state;

#ifdef ALT_USE_DIRECT_DRIVERS
ALT_DRIVER_READ_EXTERNS(PERIDOT_SW_HOSTBRIDGE_PORT);
ALT_DRIVER_WRITE_EXTERNS(PERIDOT_SW_HOSTBRIDGE_PORT);
#endif

extern int peridot_sw_hostbridge_gen2_avm_init(void);

/**
 * @func find_channel
 * @brief Find channel structure from channel number
 * @param number Channel number to find
 */
static hostbridge_channel *find_channel(alt_u8 number)
{
    hostbridge_channel *channel;
    for (channel = state.channels; channel; channel = channel->next) {
        if (channel->number == number) {
            return channel;
        }
    }
    return NULL;
}

/**
 * @func write_to_channel
 * @brief Write data to channel
 * @param channel Destination channel
 * @param buffer Pointer to buffer
 * @param from Start offset
 * @param to End offset (exclusive)
 */
static void write_to_channel(hostbridge_channel *channel, const alt_u8 *buffer, int from, int to)
{
    int len;

    if (!channel) {
        return;
    }

    buffer += from;
    len = to - from;
    while (len > 0) {
        int written;
        if (channel->use_fd) {
            // Use file descriptor
            written = write(channel->dest.fd, buffer, len);
        } else {
            // Use callback function
            written = (*channel->dest.sink)(channel, buffer, len);
        }
        if (written > 0) {
            buffer += written;
            len -= written;
        } else {
            YIELD();
        }
    }
}

#ifdef PERIDOT_SW_HOSTBRIDGE_GEN2_USE_RECEIVER_THREAD
static
#endif
/**
 * @func peridot_sw_hostbridge_gen2_service
 * @brief Process I/O with software-based hostbridge (Gen2)
 */
void peridot_sw_hostbridge_gen2_service(void)
{
    alt_u8 buffer[READ_BUFFER_LEN];
    int index;
    int head;
    int read_len;

#ifndef ALT_USE_DIRECT_DRIVERS
    read_len = read(state.fd, buffer, sizeof(buffer));
#else
    read_len = ALT_DRIVER_READ(PERIDOT_SW_HOSTBRIDGE_PORT, buffer, sizeof(buffer), O_NONBLOCK);
#endif
    if (read_len <= 0) {
        return;
    }

    head = 0;
    for (index = 0; index < read_len; ++index) {
        alt_u8 byte = buffer[index];
        switch (byte) {
        case AST_CHANNEL_PREFIX:
            state.channel_prefix = 1;
            continue;
        case AST_ESCAPE_PREFIX:
            state.escape_prefix = 1;
            if (state.sink_channel && !state.sink_channel->packetized) {
                // Drop special byte
                --read_len;
                memmove(buffer, buffer + 1, read_len - index);
            }
            continue;
        }
        if (state.escape_prefix) {
            byte ^= AST_ESCAPE_XOR;
            buffer[index] = byte;
            state.escape_prefix = 0;
        }
        if (state.channel_prefix) {
            write_to_channel(state.sink_channel, buffer, head, index - 1);
            state.sink_channel = find_channel(byte);
            head = index + 1;
            state.channel_prefix = 0;
            continue;
        }
        if (state.sink_channel && !state.sink_channel->packetized) {
            // Drop special bytes
            switch (byte) {
            case AST_SOP:
            case AST_EOP_PREFIX:
                --read_len;
                memmove(buffer, buffer + 1, read_len - index);
                continue;
            }
        }
    }

    write_to_channel(state.sink_channel, buffer, head, read_len);
}

#ifdef PERIDOT_SW_HOSTBRIDGE_GEN2_USE_RECEIVER_THREAD
static void *peridot_sw_hostbridge_gen2_worker(void *param)
{
    (void)param;
    for (;;) {
        peridot_sw_hostbridge_gen2_service();
    }
    return NULL;
}
#endif  /* PERIDOT_SW_HOSTBRIDGE_GEN2_USE_RECEIVER_THREAD */

/**
 * @func peridot_sw_hostbridge_gen2_init
 * @brief Initialize software-based hostbridge (Gen2)
 * @note This function will be called from alt_sys_init
 */
int peridot_sw_hostbridge_gen2_init(void)
{
    int result;
#ifdef PERIDOT_SW_HOSTBRIDGE_GEN2_USE_RECEIVER_THREAD
    pthread_t tid;
#endif
#ifndef ALT_USE_DIRECT_DRIVERS
    state.fd = open(PERIDOT_SW_HOSTBRIDGE_PATH, O_RDWR
# ifndef PERIDOT_SW_HOSTBRIDGE_GEN2_USE_RECEIVER_THREAD
         | O_NONBLOCK
# endif /* !PERIDOT_SW_HOSTBRIDGE_GEN2_USE_RECEIVER_THREAD */
    );
#endif  /* !ALT_USE_DIRECT_DRIVERS */
    state.source_channel_number = -1;
    ALT_SEM_CREATE(&state.lock, 1);
    result = peridot_sw_hostbridge_gen2_avm_init();
    if (result != 0) {
        return result;
    }
#ifdef PERIDOT_SW_HOSTBRIDGE_GEN2_USE_RECEIVER_THREAD
    result = -pthread_create(&tid, NULL, peridot_sw_hostbridge_gen2_worker, NULL);
#endif
    return result;
}

/**
 * @func peridot_sw_hostbridge_gen2_register_channel
 * @brief Register channel
 * @param channel Channel structure to register
 */
int peridot_sw_hostbridge_gen2_register_channel(hostbridge_channel *channel)
{
    hostbridge_channel *next, **prev;

    for (prev = &state.channels; (next = *prev) != NULL; prev = &next->next) {
        if (next->number == channel->number) {
            return -EEXIST;
        }
    }
    channel->next = NULL;
    *prev = channel;
    return 0;
}

/**
 * @func write_to_host
 * @brief Write data (source) to host
 * @param ptr Pointer to buffer
 * @param len Length of buffer
 */
static void write_to_host(const void *ptr, int len)
{
    while (len > 0) {
        int written;
#ifndef ALT_USE_DIRECT_DRIVERS
        written = write(state.fd, ptr, len);
#else
        written = ALT_DRIVER_WRITE(PERIDOT_SW_HOSTBRIDGE_PORT, ptr, len, O_NONBLOCK);
#endif
        if (written > 0) {
            ptr = (const alt_u8 *)ptr + written;
            len -= written;
        } else {
            YIELD();
        }
    }
}

/**
 * @func peridot_sw_hostbridge_gen2_source
 * @brief Write data from channel
 * @param channel Source channel
 * @param ptr Pointer to buffer
 * @param len Length of buffer
 */
int peridot_sw_hostbridge_gen2_source(hostbridge_channel *channel, const void *ptr, int len, int flags)
{
    int packetize = (flags & HOSTBRIDGE_GEN2_SOURCE_PACKETIZED) ? 1 : 0;
    ALT_SEM_PEND(state.lock, 0);

    if ((channel->number != state.source_channel_number) || (flags & HOSTBRIDGE_GEN2_SOURCE_RESET)) {
        alt_u8 buffer[3];
        alt_u8 byte = channel->number;
        int write_len;
        buffer[0] = AST_CHANNEL_PREFIX;
        if (AST_NEEDS_ESCAPE(byte)) {
            // Escape
            buffer[1] = AST_ESCAPE_PREFIX;
            buffer[2] = byte ^ AST_ESCAPE_XOR;
            write_len = 3;
        } else {
            buffer[1] = byte;
            write_len = 2;
        }
        write_to_host(buffer, write_len);
        state.source_channel_number = byte;
    }

    if (channel->packetized && !packetize) {
        write_to_host(ptr, len);
    } else {
        alt_u8 buffer[WRITE_BUFFER_LEN + 2];
        const alt_u8 *src = (const alt_u8 *)ptr;
        while (len > 0) {
            int write_len = 0;
            if (packetize > 0) {
                buffer[0] = AST_SOP;
                write_len = 1;
                packetize = -1;
            }
            while ((write_len < WRITE_BUFFER_LEN) && (len > 0)) {
                alt_u8 byte = *src++;
                if (packetize && (len == 1)) {
                    buffer[write_len++] = AST_EOP_PREFIX;
                }
                if (AST_NEEDS_ESCAPE(byte)) {
                    // Escape
                    buffer[write_len++] = AST_ESCAPE_PREFIX;
                    buffer[write_len++] = byte ^ AST_ESCAPE_XOR;
                } else {
                    buffer[write_len++] = byte;
                }
                --len;
            }
            write_to_host(buffer, write_len);
        }
    }

    ALT_SEM_POST(state.lock);
    return 0;
}
