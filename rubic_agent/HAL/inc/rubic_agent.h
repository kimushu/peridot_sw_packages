#ifndef __RUBIC_AGENT_H__
#define __RUBIC_AGENT_H__

#ifdef __cplusplus
extern "C" {
#endif

enum {
	RUBIC_AGENT_RUNNER_FLAG_FILE   = (1<<0),
	RUBIC_AGENT_RUNNER_FLAG_SOURCE = (1<<1),
	RUBIC_AGENT_RUNNER_FLAG_DEBUG  = (1<<2),
};

typedef int (*rubic_agent_runtime_runner)(const char *data, int flags, void *context);

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
