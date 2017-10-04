#ifndef __RUBIC_AGENT_H__
#define __RUBIC_AGENT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rubic_agent_request_s {
	const char *name;
	const void *params;
	void *_context;
} rubic_agent_request;

typedef int (*rubic_agent_runtime_runner)(const char *file, int debug, void *context);

extern int rubic_agent_init(void);

extern int rubic_agent_register_runtime(const char *name, const char *version, rubic_agent_runtime_runner runner);
extern int rubic_agent_register_storage(const char *name, const char *path);
extern int rubic_agent_service(void);

extern int rubic_agent_runner_notify_init(void *context);
extern void rubic_agent_runner_cooperate(void *context);
extern int rubic_agent_runner_query_abort(void *context);

#define RUBIC_AGENT_INSTANCE(name, state) extern int alt_no_storage

#define RUBIC_AGENT_INIT(name, state) \
	rubic_agent_init()

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __RUBIC_AGENT_H__ */
