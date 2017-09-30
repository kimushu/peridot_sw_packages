#ifndef __PERIDOT_SW_HOSTBRIDGE_GEN2_H__
#define __PERIDOT_SW_HOSTBRIDGE_GEN2_H__

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LISTENER_FLAG_SOP = (1<<0),
    LISTENER_FLAG_EOP = (1<<1),
};

typedef struct hostbridge_listener_s {
    struct hostbridge_listener_s *next;
    alt_u8 channel;
    alt_u8 packetized;
    alt_u16 flags;
    int (*write)(struct hostbridge_listener *listener, const void *ptr, int len, int flags);
    int (*read)(struct hostbridge_listener *listener, void *ptr, int len, int *flags);
} hostbridge_listener;

extern void peridot_sw_hostbridge_gen2_init(void);
extern int peridot_sw_hostbridge_gen2_add_listener(hostbridge_listener *listener);

#define PERIDOT_SW_HOSTBRIDGE_GEN2_INSTANCE(name, state) \
    extern int alt_no_storage

#define PERIDOT_SW_HOSTBRIDGE_GEN2_INIT(name, state) \
    peridot_sw_hostbridge_gen2_init()

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif  /* __PERIDOT_SW_HOSTBRIDGE_GEN2_H__ */
