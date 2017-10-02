#ifndef __RUBIC_AGENT_H__
#define __RUBIC_AGENT_H__

#ifdef __cplusplus
extern "C" {
#endif

extern void rubic_agent_init(void);

extern const void *rubic_agent_wait_request(const char **request, void (*abort_handler)(void));
extern int rubic_agent_is_aborting(void);
extern void rubic_agent_finish_request(void);

#define RUBIC_AGENT_INSTANCE(name, state) extern int alt_no_storage

#define RUBIC_AGENT_INIT(name, state) \
	rubic_agent_init()

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __RUBIC_AGENT_H__ */
