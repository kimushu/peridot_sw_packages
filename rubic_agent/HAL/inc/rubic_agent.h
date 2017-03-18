#ifndef __RUBIC_AGENT_H__
#define __RUBIC_AGENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rubic_agent_info_s {
} rubic_agent_info;

enum {
	RUBIC_AGENT_REQUEST_NONE,
	RUBIC_AGENT_REQUEST_START,
	RUBIC_AGENT_REQUEST_FORMAT,
};

typedef struct rubic_agent_message_s {
	int request;
	union {
		struct {
			const char *program;
		} start;
		struct {
		} format;
	} body;
} rubic_agent_message;

enum {
	RUBIC_AGENT_FILE_INFO,
	RUBIC_AGENT_FILE_RUN,
	RUBIC_AGENT_FILE_STOP,
	RUBIC_AGENT_FILE_FORMAT,
};

typedef struct rubic_agent_fddata_s {
	int file;
	int buf_ptr;
	int buf_len;
	int buf_max;
	char buf[0];
} rubic_agent_fddata;

extern void rubic_agent_init(void);
extern void rubic_agent_set_interrupt_handler(void (*handler)(int reason));
extern void rubic_agent_wait_request(rubic_agent_message *msg);
extern void rubic_agent_send_response(rubic_agent_message *msg);

#define RUBIC_AGENT_INSTANCE(name, state) extern int alt_no_storage

#define RUBIC_AGENT_INIT(name, state) \
	rubic_agent_init()

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __RUBIC_AGENT_H__ */
