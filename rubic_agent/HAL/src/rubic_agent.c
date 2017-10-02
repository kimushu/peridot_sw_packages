#include "rubic_agent.h"
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include "system.h"
#include "bson.h"
#include "peridot_rpc_server.h"

static struct {
	pthread_mutex_t mutex;
	sem_t sem;
	unsigned char busy;
	unsigned char aborting;
	const char *request;
	void *params;
	void (*abort_handler)(void);
} state;

static void *make_runtime_info(const char *name, const char *version)
{
	static char runtime[64];
	bson_create_empty_document(runtime);
	bson_set_string(runtime, "name", name);
	bson_set_string(runtime, "version", version);
	return runtime;
}

/*
 * Query agent information
 */
static void *rubic_agent_method_info(const void *params)
{
	void *output = malloc(2048);
	void *subdoc;

	if (!output) {
		return NULL;
	}
	subdoc = ((char *)output) + (2048 - 256);

	bson_create_empty_document(output);
	bson_set_string(output, "rubicVersion", RUBIC_AGENT_RUBIC_VERSION);

	// Storage information
	bson_create_empty_document(subdoc);
	bson_set_string(subdoc, "internal", RUBIC_AGENT_STORAGE_INTERNAL);
	bson_set_subdocument(output, "storage", subdoc);

	// Runtime information
	bson_create_empty_document(subdoc);
	bson_set_subdocument(subdoc, "0", make_runtime_info(RUBIC_AGENT_RUNTIME1_NAME, RUBIC_AGENT_RUNTIME1_VERSION));
#ifdef RUBIC_AGENT_RUNTIME2_PRESENT
	bson_set_subdocument(subdoc, "1", make_runtime_info(RUBIC_AGENT_RUNTIME2_NAME, RUBIC_AGENT_RUNTIME2_VERSION));
#ifdef RUBIC_AGENT_RUNTIME3_PRESENT
	bson_set_subdocument(subdoc, "2", make_runtime_info(RUBIC_AGENT_RUNTIME3_NAME, RUBIC_AGENT_RUNTIME3_VERSION));
#endif
#endif
	bson_set_array(output, "runtimes", subdoc);

	return output;
}

/*
 * Run program
 */
static void *new_request(const char *request, const void *params)
{
	int params_len;
	int off_target;
	if (bson_get_props(params, "target", &off_target, NULL) < 1) {
		errno = JSONRPC_ERR_INVALID_PARAMS;
		return NULL;
	}

	pthread_mutex_lock(&state.mutex);
	if (state.request) {
		pthread_mutex_unlock(&state.mutex);
		errno = EBUSY;
		return NULL;
	}

	params_len = bson_measure_document(params);
	state.params = malloc(params_len);
	if (!state.params) {
		pthread_mutex_unlock(&state.mutex);
		errno = ENOMEM;
		return NULL;
	}

	state.request = request;
	state.aborting = 0;
	memcpy(state.params, params, params_len);
	pthread_mutex_unlock(&state.mutex);

	sem_post(&state.sem);
	errno = 0;
	return NULL;
}

/*
 * Start program
 */
static void *rubic_agent_method_start(const void *params)
{
	return new_request("start", params);
}

/*
 * Format storage
 */
static void *rubic_agent_method_format(const void *params)
{
	return new_request("format", params);
}

/*
 * Abort program
 */
static void *rubic_agent_method_abort(const void *params)
{
	pthread_mutex_lock(&state.mutex);
	if (state.request) {
		if (!state.aborting) {
			state.aborting = 1;
			if (state.abort_handler) {
				(*state.abort_handler)();
			}
		}
	}
	pthread_mutex_unlock(&state.mutex);
	errno = 0;
	return NULL;
}

/*
 * Query program status
 */
static void *rubic_agent_method_status(const void *params)
{
	void *output = malloc(64);
	if (!output) {
		return NULL;
	}

	pthread_mutex_lock(&state.mutex);
	bson_create_empty_document(output);
	if (state.request) {
		bson_set_string(output, "request", state.request);
	}
	bson_set_boolean(output, "busy", state.busy);
	pthread_mutex_unlock(&state.mutex);

	return output;
}

/*
 * Start Rubic agent
 */
void rubic_agent_init(void)
{
	peridot_rpc_server_register_method("rubic.info", rubic_agent_method_info);
	peridot_rpc_server_register_method("rubic.start", rubic_agent_method_start);
	peridot_rpc_server_register_method("rubic.format", rubic_agent_method_format);
	peridot_rpc_server_register_method("rubic.abort", rubic_agent_method_abort);
	peridot_rpc_server_register_method("rubic.status", rubic_agent_method_status);
}

/**
 * Wait request
 */
const void *rubic_agent_wait_request(const char **request, void (*abort_handler)(void))
{
	const void *params;
	state.abort_handler = NULL;

	for (;;) {
		sem_wait(&state.sem);
		pthread_mutex_lock(&state.mutex);
		if (state.request) {
			break;
		}
		pthread_mutex_unlock(&state.mutex);
	}
	// mutex still locked

	*request = state.request;
	state.busy = 1;
	state.abort_handler = abort_handler;
	params = state.params;
	pthread_mutex_unlock(&state.mutex);

	return params;
}

/**
 * Check aborting
 */
int rubic_agent_is_aborting(void)
{
	return (state.aborting != 0);
}

/**
 * Finish request
 */
void rubic_agent_finish_request(void)
{
	pthread_mutex_lock(&state.mutex);
	if (state.request) {
		state.request = NULL;
		state.busy = 0;
		state.aborting = 0;
		free(state.params);
		state.params = NULL;
		state.abort_handler = NULL;
	}
	pthread_mutex_unlock(&state.mutex);
}
