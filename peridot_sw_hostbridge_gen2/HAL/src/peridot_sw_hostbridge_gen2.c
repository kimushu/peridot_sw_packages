#include "peridot_sw_hostbridge_gen2.h"
#include <pthread.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <errno.h>

#define REQUEST_BUFFER_LENGTH   64

#define AST_SOP         0x7a
#define AST_EOP         0x7b
#define AST_CHANNEL     0x7c
#define AST_ESCAPED     0x7d
#define AST_ESCAPE_XOR  0x20

// Channel flags
#define FLAG_IN_PACKET  (1<<0)
#define FLAG_LAST_BYTE  (1<<1)

// Global flags
enum {
    STATE_FLAG_CHANNEL = (1<<0),
    STATE_FLAG_ESCAPED = (1<<1),
};

static struct peridot_sw_hostbridge_gen2_state_s {
    int fd;                         // File descriptor for UART
    hostbridge_listener *listeners; // Head of listener chain
    alt_16 channel;
    alt_16 flags;

    alt_u8 *resp_data;
    int resp_len;
    int resp_offset;

    alt_u8 req_buffer[REQUEST_BUFFER_LENGTH];
    int req_len;
    int req_offset;

} state;

void peridot_sw_hostbridge_gen2_init(void)
{
    state.fd = open(PERIDOT_SW_HOSTBRIDGE_GEN2_PIPE, O_RDWR | O_NONBLOCK);
    state.listeners = NULL;
    state.channel = -1;
}

int peridot_sw_hostbridge_gen2_add_listener(hostbridge_listener *listener)
{
    hostbridge_listener **prev = &state.listeners;
    for (;;) {
        hostbridge_listener *next = *prev;
        if (!next) {
            *prev = listener;
            listener->flags = 0;
            break;
        } else if (next->channel == listener->channel) {
            return -EEXIST;
        }
        prev = &next->next;
    }
    return 0;
}

static hostbridge_listener *get_current_listener(void)
{
    hostbridge_listener *listener;

    if (state.channel < 0) {
        return NULL;
    }
    for (listener = state.listeners; listener; listener = listener->next) {
        if (listener->channel == state.channel) {
            return listener;
        }
    }
    return NULL;
}

static void shift_recv_buffer(void)
{
    --state.req_len;
    --state.req_offset;
    memmove(
        state.req_buffer + state.req_offset,
        state.req_buffer + state.req_offset + 1,
        state.req_len - state.req_offset
    );
}

static void drain_recv_buffer(hostbridge_listener *listener, int offset, int trimBytes)
{
    int len = state.req_offset - trimBytes - offset;
    if (len > 0) {
        (*listener->write)(listener, state.req_buffer + offset, len);
    }
    listener->flags &= ~(LISTENER_FLAG_SOP | LISTENER_FLAG_BREAK | LISTENER_FLAG_EOP);
}

int peridot_sw_hostbridge_gen2_service(void)
{
    hostbridge_listener *listener;
    int head_offset;

    // Send response
    if (state.resp_data) {
        int sent = write(
            state.fd,
            state.resp_data + state.resp_offset,
            state.resp_len - state.resp_offset
        );
        if (sent > 0) {
            state.resp_offset += sent;
            if (state.resp_offset >= state.resp_len) {
                free(state.resp_data);
                state.resp_data = NULL;
            }
        }
    }

    // Receive request
    if (state.req_len < REQUEST_BUFFER_LENGTH) {
        int received = read(
            state.fd,
            state.req_buffer + state.req_len,
            REQUEST_BUFFER_LENGTH - state.req_len
        );
        if (received > 0) {
            state.req_len += received;
        }
    }

    head_offset = 0;
update_listener:
    listener = get_current_listener();
    while (state.req_offset < state.req_len) {
        alt_u8 byte = state.req_buffer[state.req_offset++];
        // byte
        // [xx] [yy] [zz]
        //     ^
        //     req_offset
        switch (byte) {
        case AST_SOP:
            if (!listener) {
                // No listener
            } else if (!listener->packetized) {
                // Drop this byte
                shift_recv_buffer();
            } else {
                // (Re)start a new packet
                if (listener->flags & LISTENER_FLAG_PACKET) {
                    listener->flags |= LISTENER_FLAG_BREAK;
                }
                listener->flags = (listener->flags & ~LISTENER_FLAG_EOP) |
                                    LISTENER_FLAG_SOP | LISTENER_FLAG_PACKET;
                head_offset = state.req_offset;
            }
            continue;
        case AST_EOP:
            if (!listener) {
                // No listener
            } else if (!listener->packetized) {
                // Drop this byte
                shift_recv_buffer();
            } else if (listener->flags & LISTENER_FLAG_PACKET) {
                // Next byte is the last
                listener->flags |= LISTENER_FLAG_EOP;
            }
            continue;
        case AST_CHANNEL:
            // Drain current data
            if (listener) {
                drain_recv_buffer(listener, head_offset, 1);
            }
            state.flags |= STATE_FLAG_CHANNEL;
            goto drain;
        case AST_ESCAPED:
            // Flag escaped
            state.flags |= STATE_FLAG_ESCAPED;
            continue;
        }
        if (state.flags & STATE_FLAG_CHANNEL) {
            // Channel change
            if (state.flags & STATE_FLAG_ESCAPED) {
                byte ^= AST_ESCAPE_XOR;
            }
            state.channel = byte;
            state.flags &= ~(STATE_FLAG_CHANNEL | STATE_FLAG_ESCAPED);
            head_offset = state.req_offset;
            goto update_listener;
        }
        if (state.flags & STATE_FLAG_ESCAPED) {
            // De-escape
            byte ^= AST_ESCAPE_XOR;
            shift_recv_buffer();
            state.req_buffer[state.req_offset - 1] = byte;
            state.flags &= ~STATE_FLAG_ESCAPED;
        }
        if (listener && (listener->flags & LISTENER_FLAG_EOP)) {
            // Force drain at the last byte of packet
            drain_recv_buffer(listener, head_offset, 0);
            listener->flags &= ~LISTENER_FLAG_PACKET;
        }
    }

    if (listener) {
        drain_recv_buffer(listener, head_offset, (state.flags & STATE_FLAG_ESCAPED) ? 1 : 0);
    }

    return 0;
}
