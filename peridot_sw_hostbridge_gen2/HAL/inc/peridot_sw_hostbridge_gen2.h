#ifndef __PERIDOT_SW_HOSTBRIDGE_GEN2_H__
#define __PERIDOT_SW_HOSTBRIDGE_GEN2_H__

#include "alt_types.h"
#include "system.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AST_SOP             = 0x7a,
    AST_EOP_PREFIX      = 0x7b,
    AST_CHANNEL_PREFIX  = 0x7c,
    AST_ESCAPE_PREFIX   = 0x7d,
    AST_ESCAPE_XOR      = 0x20,
};

#define AST_NEEDS_ESCAPE(x) ((AST_SOP <= (x)) && ((x) <= AST_ESCAPE_PREFIX))

enum {
    HOSTBRIDGE_GEN2_SOURCE_PACKETIZED   = (1 << 0),
    HOSTBRIDGE_GEN2_SOURCE_RESET        = (1 << 1),
};

typedef struct hostbridge_channel_s {
    struct hostbridge_channel_s *next;
    union {
        int fd;
        int (*sink)(struct hostbridge_channel_s *channel, const void *ptr, int len);
    } dest;
    alt_u8 number;
    alt_u8 packetized;
    alt_u8 use_fd;
} hostbridge_channel;

extern int peridot_sw_hostbridge_gen2_init(void);
#ifndef PERIDOT_SW_HOSTBRIDGE_GEN2_USE_RECEIVER_THREAD
extern void peridot_sw_hostbridge_gen2_service(void);
#endif

extern int peridot_sw_hostbridge_gen2_register_channel(hostbridge_channel *channel);
extern int peridot_sw_hostbridge_gen2_source(hostbridge_channel *channel, const void *ptr, int len, int flags);

extern int peridot_sw_hostbridge_gen2_mkpipe(alt_u8 channel, int output_fd, int input_fd, size_t input_capacity);

#define PERIDOT_SW_HOSTBRIDGE_GEN2_INSTANCE(name, state) \
    extern int alt_no_storage

#define PERIDOT_SW_HOSTBRIDGE_GEN2_INIT(name, state) \
    peridot_sw_hostbridge_gen2_init()

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif  /* __PERIDOT_SW_HOSTBRIDGE_GEN2_H__ */
