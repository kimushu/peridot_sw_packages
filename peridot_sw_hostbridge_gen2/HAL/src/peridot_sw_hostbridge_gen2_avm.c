#include "peridot_sw_hostbridge_gen2.h"

#define AST_CHANNEL_AVM     0x00

#define AVM_READABLE_BASE   0x10000000
#define AVM_READABLE_SPAN   16
#define AVM_READABLE_END    (AVM_READABLE_BASE + AVM_READABLE_SPAN)

enum {
    AVM_OFFSET_INIT = 0,
    AVM_OFFSET_FULL = 8,
    AVM_OFFSET_WRITE,
    AVM_OFFSET_READ,
    AVM_OFFSET_NOTR,
};

static struct {
    hostbridge_channel channel;
    alt_u8 escape_prefix;
    alt_u8 eop_prefix;
    alt_u8 inside_packet;
    alt_u8 offset;
    union {
        alt_u8 u8[8];
        alt_u16 u16[4];
        alt_u32 u32[2];
    } buffer;
} state;

inline static alt_u16 SWAP16(alt_u16 x)
{
    return (x >> 8) | (x << 8);
}

inline static alt_u32 SWAP32(alt_u32 x)
{
    return (SWAP16(x) << 16) | SWAP16(x >> 16);
}

static int avm_sink(hostbridge_channel *channel, const void *ptr, int len)
{
    const alt_u8 *src = (const alt_u8 *)ptr;
    int read_len = 0;

    while (read_len < len) {
        int flags = HOSTBRIDGE_GEN2_SOURCE_PACKETIZED;
        alt_u8 byte = *src++;
        ++read_len;
        switch (byte) {
        case AST_SOP:
            state.offset = AVM_OFFSET_INIT;
            state.inside_packet = 1;
            state.eop_prefix = 0;
            continue;
        case AST_EOP_PREFIX:
            state.eop_prefix = 1;
            continue;
        case AST_ESCAPE_PREFIX:
            state.escape_prefix = 1;
            continue;
        }
        if (state.escape_prefix) {
            byte ^= AST_ESCAPE_XOR;
            state.escape_prefix = 0;
        }
        if (!state.inside_packet) {
            continue;
        }
        if (state.offset < AVM_OFFSET_FULL) {
            state.buffer.u8[state.offset++] = byte;
        }
        if (state.offset == AVM_OFFSET_FULL) {
            switch (state.buffer.u8[0]) {
            case 0x00:  // Write, non-incrementing address
            case 0x04:  // Write, incrementing address
                state.offset = AVM_OFFSET_WRITE;
                break;
            case 0x10:  // Read, non-incrementing address
            case 0x14:  // Read, incrementing address
                state.offset = AVM_OFFSET_READ;
                break;
            case 0x7f:  // No transaction
                flags |= HOSTBRIDGE_GEN2_SOURCE_RESET;
                /* no break */
            default:    // No transaction or others
                state.offset = AVM_OFFSET_NOTR;
                break;
            }
        }
        if (!state.eop_prefix) {
            continue;
        }
        state.eop_prefix = 0;
        state.inside_packet = 0;
        state.buffer.u8[0] ^= 0x80;
        state.buffer.u8[1] = 0x00;
        switch (state.offset) {
        case AVM_OFFSET_READ:
            {
                alt_u32 addr = SWAP32(state.buffer.u32[1]);
                alt_u16 size = SWAP16(state.buffer.u16[1]);
                if ((addr < AVM_READABLE_BASE) || ((addr + size) > AVM_READABLE_END)) {
                    // Out of range (returns 1 byte zero)
                    peridot_sw_hostbridge_gen2_source(channel, "", 1, flags);
                } else {
                    peridot_sw_hostbridge_gen2_source(channel, (const void *)addr, size, flags);
                }
            }
            break;
        case AVM_OFFSET_WRITE:
            // Write is not permitted
            /* no break */
        default:
            // No transaction or others
            state.buffer.u16[1] = SWAP16(0);
            peridot_sw_hostbridge_gen2_source(channel, &state.buffer, 4, flags);
            break;
        }
    }

    return len;
}

int peridot_sw_hostbridge_gen2_avm_init(void)
{
    state.channel.dest.sink = avm_sink;
    state.channel.number = AST_CHANNEL_AVM;
    state.channel.packetized = 1;
    return peridot_sw_hostbridge_gen2_register_channel(&state.channel);
}
