#ifndef __RUBIC_AGENT_H__
#define __RUBIC_AGENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rubic_agent_info_s {
} rubic_agent_info;

enum {
	RUBIC_AGENT_REQ_INFO,
	RUBIC_AGENT_REQ_RUN,
	RUBIC_AGENT_REQ_STOP,
};

typedef struct rubic_agent_fddata_s {
	int request;
	int data_ptr;
	int data_len;
	int data_max;
	char data[0];
} rubic_agent_fddata;

extern void rubic_agent_init(void);
extern void rubic_agent_set_interrupt_handler(void *(*handler)(int reason));
extern const char *rubic_agent_wait_start_request(void);
extern int rubic_agent_set_program(const char *name);

#define RUBIC_AGENT_INSTANCE(name, state) extern int alt_no_storage

#define RUBIC_AGENT_INIT(name, state) \
	rubic_agent_init()

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __RUBIC_AGENT_H__ */
