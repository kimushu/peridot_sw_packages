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
#define FLAG_CHANNEL    (1<<0)
#define FLAG_ESCAPED    (1<<1)

static struct peridot_sw_hostbridge_gen2_state_s {
    int fd;                         // File descriptor for UART
    hostbridge_listener *listeners; // Head of listener chain
    alt_16 channel;
    alt_16 flags;

    alt_u8 *resp_data;
    size_t resp_len;
    size_t resp_offset;

    alt_u8 req_buffer[REQUEST_BUFFER_LENGTH];
    size_t req_len;
    size_t req_offset;

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

int peridot_sw_hostbridge_gen2_service(void)
{
    hostbridge_listener *listener;
    int transfer_offset;

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

receive:
    listener = get_current_listener();
    transfer_offset = -1;
    while (state.req_offset < state.req_len) {
        alt_u8 byte = state.req_buffer[state.req_offset++];
        switch (byte) {
        case AST_SOP:
            if (listener) {
                listener->flags = (listener->flags | FLAG_IN_PACKET) & ~FLAG_LAST_BYTE;
            }
            continue;
        case AST_EOP:
            if (listener) {
                listener->flags |= FLAG_LAST_BYTE;
            }
            continue;
        case AST_CHANNEL:
            state.flags |= FLAG_CHANNEL;
            continue;
        case AST_ESCAPED:
            state.flags |= FLAG_ESCAPED;
            continue;
        }
        if (state.flags & FLAG_ESCAPED) {
            state.req_buffer[--state.req_offset - 1] = (byte ^ AST_ESCAPE_XOR);
            --state.req_len;
            state.flags &= ~FLAG_ESCAPED;
        }
        if (state.flags & FLAG_CHANNEL) {
            state.channel = byte;
            state.flags &= ~FLAG_CHANNEL;
            goto receive;
        }
    }
}

#define MIN_RECV_BUFFER_SIZE    1024
#define MIN_RECV_SPACE          64

enum {
    FLAG_CHANNEL    = (1<<0),
    FLAG_PACKET     = (1<<1),
    FLAG_LASTBYTE   = (1<<2),
    FLAG_ESCAPE7    = (1<<3),
    FLAG_ESCAPE3    = (1<<4),
    FLAG_V2ENABLED  = (1<<5),
    FLAG_V1COMMAND  = (1<<6),
};

static alt_u8 g_flags;      // Combination of FLAG_xx
static alt_u8 g_channel;    // Channel number

static alt_u8 *g_recv_data; // Buffer for data received
static alt_u8 *g_recv_next; // Next position to write received byte
static int g_recv_capacity; // Capacity for g_recv_data

static hostbridge_receiver_entry *g_entries;

static alt_u8 *g_send_data; // Buffer for data to send
static int g_send_len;      // Length of data to send
static int g_send_offset;   // Offset of data to send
static alt_u8 g_send_small[4];  // Small static buffer

static alt_u16 SWAP16(alt_u16 value)
{
    return (value >> 8) | (value << 8);
}

static alt_u32 SWAP32(alt_u32 value)
{
    return SWAP16(value >> 16) | (SWAP16(value) << 16);
}

static void process_v1_command(alt_u8 byte)
{
    alt_u8 last = g_last_v1cmd;

    g_send_data = NULL;
    g_send_small[0] = V1RESP_TDO | (last & (V1RESP_SCL | V1RESP_SDA)) | V1RESP_CONFDONE | V1RESP_NSTATUS | V1RESP_MSEL1;
    g_send_len = 1;
    g_send_offset = 0;

    if (!(last & V1CMD_JTAGEN) && !(byte & V1CMD_JTAGEN)) {
        // JTAG disabled (No change)
        if ((last & V1CMD_SCL_TCK) && (byte & V1CMD_SCL_TCK)) {
            // SCL=H (No change)
            if ((byte & V1CMD_TDI) && !(byte & V1CMD_SDA_TMS))
                g_send_small[0] &= ~V1RESP_TDO; // Notify v2 support
            }
        }
    }

    g_last_v1cmd = byte;
}

static int prefetch_recv_data(const alt_u8 **pptr)
{
    int capacity = g_recv_capacity;
    int stored = g_recv_next - g_recv_data;
    int space = capacity - stored;

    if (space < MIN_RECV_SPACE) {
        // Enlarge buffer
        capacity = (capacity > 0) ? (capacity * 2) : MIN_RECV_BUFFER_SIZE;
    } else {
        while ((capacity > MIN_RECV_BUFFER_SIZE) && ((capacity / 2 - stored) >= MIN_RECV_SPACE)) {
            capacity /= 2;
        }
    }
    if (capacity != g_recv_capacity) {
        alt_u8 *new_buffer = (alt_u8 *)realloc(g_recv_data, capacity);
        if (!new_buffer) {
            return -1;
        }
        g_recv_data = new_buffer;
        g_recv_next = g_recv_data + stored;
        g_recv_capacity = capacity;
        space = capacity - stored;
    }

    return read(g_hostuart_fd, g_recv_next, space);
}

static void clear_recv_data(void)
{
    g_recv_next = g_recv_data;
}

static int inspect_avm_access(alt_u32 *paddr, alt_u16 *psize)
{
    // FIXME
    return 1;
}

static void process_avm_packet(void)
{
    const alt_u8 *ptr = g_recv_data;
    int len = g_recv_len;
    int deny_access;
    alt_u8 code;
    alt_u16 size;
    alt_u32 addr;

    // Validate transaction code
    code = (len < 8) ? 0x7f : *ptr++;
    if ((code & ~0x14) != 0) {
        // Malformed packet
no_transaction:
        g_send_small[0] = 0x7f ^ 0x80;
        g_send_small[1] = 0x00;
        g_send_small[2] = 0x00;
        g_send_small[3] = 0x00;
        g_send_data = NULL;
        g_send_len = 4;
        g_send_offset = 0;
        return;
    }

    // Get size & address
    size = SWAP16(*(alt_u16 *)(ptr + 2));
    addr = SWAP32(*(alt_u32 *)(ptr + 4));

    // Inspect memory access range
    deny_access = inspect_avm_access(&addr, &size);

    if (code & 0x10) {
        // Read
        g_send_data = malloc(size);
        g_send_len = size;
        g_send_offset = 0;
        if (deny_access) {
            // Access denied
            memset(g_send_data, 0, size);
        } else if (code & 0x04) {
            // incrementing address
            memcpy(g_send_data, (const void *)addr, size);
        } else {
            // non-incrementing address
            *(alt_u32 *)g_send_data = *(alt_u32 *)addr;
        }
    } else {
        // Write
        ptr += 8;
        len -= 8;
        if (len != size) {
            goto no_transaction;
        }
        if (deny_access) {
            // Access denied (nothing to do)
        } else if (code & 0x04) {
            // incrementing address
            memcpy(g_response_data, (const void *)addr, size);
        } else {
            // non-incrementing address
            *(alt_u32 *)addr = *(alt_u32 *)g_response_data;
        }
        g_send_small[0] = code ^ 0x80;
        g_send_small[1] = 0;
        g_send_small[2] = (size >> 8);
        g_send_small[3] = (size & 0xff);
        g_send_data = NULL;
        g_send_len = 4;
        g_send_offset = 0;
    }
}

static void receive_bytes(void)
{
    int read_bytes = prefetch_recv_data();
    const alt_u8 *ptr = g_recv_next;

    if (read_bytes < 0) {
        return;
    }

    for (; read_bytes > 0; --read_bytes) {
        alt_u8 byte = *ptr++;

        // Process v1 command byte (no escape3 nor escape7)
        if (g_flags & FLAG_V1COMMAND) {
            g_flags &= ~FLAG_V1COMMAND;
            process_v1_command(byte);
            continue;
        }

        switch (byte) {
        case 0x3a:
            // Back to v1 compatibility mode
            g_flags = (g_flags & ~FLAG_V2ENABLED) | FLAG_V1COMMAND;
            continue;
        case 0x3d:
            // Mark escape3
            g_flags |= FLAG_ESCAPE3;
            continue;
        }

        // Unescape 0x3d
        if (g_flags & FLAG_ESCAPE3) {
            g_flags &= ~FLAG_ESCAPE3;
            byte ^= 0x20;
        }

        switch (byte) {
        case 0x7a:
            // SOP
            g_flags = (g_flags | FLAG_PACKET) & ~FLAG_LASTBYTE;
            clear_recv_data();
            continue;
        case 0x7b:
            // Mark EOP
            g_flags |= FLAG_LASTBYTE;
            continue;
        case 0x7c:
            // Mark channel prefix
            g_flags |= FLAG_CHANNEL;
            continue;
        case 0x7d:
            // Mark escape7
            g_flags |= FLAG_ESCAPE7;
            continue;
        }

        // Unescape 0x7d
        if (g_flags & FLAG_ESCAPE7) {
            g_flags &= ~FLAG_ESCAPE7;
            byte ^= 0x20;
        }

        // Read channel
        if (g_flags & FLAG_CHANNEL) {
            g_channel = byte;
            continue;
        }

        // Drop bytes outside of packet
        if ((g_flags & FLAG_PACKET) == 0) {
            continue;
        }

        // Store packet content
        if (g_recv_next != ptr) {
            *g_recv_next = byte;
        }

        if ((g_flags & FLAG_LASTBYTE) == 0) {
            // More data for this packet
            continue;
        }

        // Received last byte of packet
        g_flags &= ~(FLAG_PACKET | FLAG_LASTBYTE);

        // Process Avalon-MM transaction packet (Channel 0)
        if (g_channel == 0) {
            process_avm_packet();
            continue;
        }

        // Other channels can be used in v2 mode only
        if ((g_flags & FLAG_V2ENABLED) == 0) {
            continue;
        }

        // Find receiver
        hostbridge_receiver_entry *entry;
        for (entry = g_entries; entry; entry = entry->next) {
            if (entry->channel == g_channel) {
                // Call receiver
                g_send_len = 0;
                g_send_data = (*entry->receiver)(g_recv_data, g_recv_len, &g_send_len);
                g_send_offset = 0;
                break;
            }
        }

        // Release buffer
        clear_recv_data();
    }
}

static int send_bytes(void)
{
    int len = g_send_len - g_send_offset;
    int written;

    if (len <= 0) {
        // No more bytes to send
        if (g_send_data) {
            free(g_send_data);
            g_send_data = NULL;
        }
        return 0;
    }

    written = write(g_hostuart_fd, g_send_data ? g_send_data : g_send_small, len);
    if (written < 0) {
        return;
    }

    g_send_offset += written;
    return 1;
}
