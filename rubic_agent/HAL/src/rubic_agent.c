#include "rubic_agent.h"
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include "system.h"
#include "bson.h"
#include "peridot_rpc_server.h"

typedef struct rubic_agent_runtime_s {
	const char *name;
	const char *version;
	rubic_agent_runtime_runner runner;
} rubic_agent_runtime;

typedef struct rubic_agent_storage_s {
	const char *name;
	const char *path;
} rubic_agent_storage;

enum {
	WORKER_STATE_IDLE = 0,
	WORKER_STATE_STARTING,
	WORKER_STATE_RUNNING,
	WORKER_STATE_ABORTING,
	WORKER_STATE_FAILED,
};

typedef struct rubic_agent_worker_s {
	char tid;
	char state;
	pthread_mutex_t mutex;
	sem_t sem;
	peridot_rpc_server_async_context *context;
} rubic_agent_worker;

static struct rubic_agent_state_s {
	rubic_agent_runtime runtimes[RUBIC_AGENT_MAX_RUNTIMES];
	rubic_agent_storage storages[RUBIC_AGENT_MAX_STORAGES];
	rubic_agent_worker workers[RUBIC_AGENT_WORKER_THREADS];
	void *cached_info;
} state;

/**
 * @func find_runtime
 * @brief Find runtime from its name
 */
static rubic_agent_runtime *find_runtime(const char *name)
{
	int i;
	for (i = 0; i < RUBIC_AGENT_MAX_RUNTIMES; ++i) {
		rubic_agent_runtime *runtime = &state.runtimes[i];
		if (!runtime->name) {
			continue;
		}
		if ((!name) || (strcmp(runtime->name, name) == 0)) {
			return runtime;
		}
	}
	return NULL;
}

/**
 * @func worker
 * @brief Main worker thread
 */
static void *worker(rubic_agent_worker *worker)
{
	for (;;) {
		peridot_rpc_server_async_context *context;
		rubic_agent_runtime *runtime;
		const void *params;
		int off_runtime, off_file, off_source, off_debug;
		int flags;
		const char *file_or_source;
		int result;

		// Wait new start request
		worker->state = WORKER_STATE_IDLE;
		sem_wait(&worker->sem);
		pthread_mutex_lock(&worker->mutex);
		context = worker->context;
		if (context) {
			worker->state = WORKER_STATE_STARTING;
		}
		pthread_mutex_unlock(&worker->mutex);
		if (!context) {
			continue;
		}

		params = context->params;
		bson_get_props(params,
			"runtime", &off_runtime,
			"file", &off_file,
			"source", &off_source,
			"debug", &off_debug,
			NULL
		);

		runtime = find_runtime(bson_get_string(params, off_runtime, NULL));
		file_or_source = bson_get_string(params, off_file, NULL);
		if (file_or_source) {
			flags = RUBIC_AGENT_RUNNER_FLAG_FILE;
		} else {
			flags = RUBIC_AGENT_RUNNER_FLAG_SOURCE;
			file_or_source = bson_get_string(params, off_source, NULL);
		}
		if ((!runtime) || (file_or_source == NULL)) {
			// invalid request
			result = -ESRCH;
			goto reply_error;
		}
		if (bson_get_boolean(params, off_debug, 0)) {
			flags |= RUBIC_AGENT_RUNNER_FLAG_DEBUG;
		}
		result = (*runtime->runner)(file_or_source, flags, worker);

		if (worker->state == WORKER_STATE_STARTING) {
			// context for "start" still alive
reply_error:
			worker->context = NULL;
			peridot_rpc_server_async_callback(context, NULL, result);
			continue;
		}
	}

	return NULL;
}

/**
 * @func rubic_agent_runner_notify_init
 * @brief Initialization complete callback for runners
 */
int rubic_agent_runner_notify_init(void *context)
{
	rubic_agent_worker *worker = (rubic_agent_worker *)context;
	void *output = malloc(32);
	if (!output) {
		peridot_rpc_server_async_callback(worker->context, NULL, -ENOMEM);
		worker->context = NULL;
		worker->state = WORKER_STATE_FAILED;
		return 1;
	}

	bson_create_empty_document(output);
	bson_set_int32(output, "tid", worker->tid);
	peridot_rpc_server_async_callback(worker->context, output, 0);
	worker->context = NULL;
	worker->state = WORKER_STATE_RUNNING;
	return 0;
}

/**
 * @func rubic_agent_runner_cooperate
 * @brief Check request for runners
 */
void rubic_agent_runner_cooperate(void *context)
{
	rubic_agent_worker *worker = (rubic_agent_worker *)context;
	peridot_rpc_server_async_context *rpc_ctx;
	int off_name;
	const char *name;

	rpc_ctx = worker->context;
	if (!rpc_ctx) {
		return;
	}
	worker->context = NULL;

	bson_get_props(rpc_ctx->params, "name", &off_name, NULL);
	name = bson_get_string(rpc_ctx->params, off_name, "");
	if (strcmp(name, "abort") == 0) {
		// Abort request always succeeds with "null" response
		worker->state = WORKER_STATE_ABORTING;
		peridot_rpc_server_async_callback(rpc_ctx, NULL, 0);
		return;
	}

	// Unsupported request
	peridot_rpc_server_async_callback(rpc_ctx, NULL, -ESRCH);
}

/**
 * @func rubic_agent_runner_query_abort
 * @brief Query abort request for runners
 */
int rubic_agent_runner_query_abort(void *context)
{
	rubic_agent_worker *worker = (rubic_agent_worker *)context;
	return (worker->state == WORKER_STATE_ABORTING) ? 1 : 0;
}

/**
 * @func generate_info
 * @brief Generate Rubic agent information cache
 */
static int generate_info(void)
{
	int i;
	int len;
	void *subdoc1, *subdoc2;
	void *output;

	free(state.cached_info);
	state.cached_info = NULL;

	subdoc1 = malloc(64 * (RUBIC_AGENT_MAX_RUNTIMES + RUBIC_AGENT_MAX_STORAGES));
	if (!subdoc1) {
		return -ENOMEM;
	}
	subdoc2 = (char *)subdoc1 + 64 * RUBIC_AGENT_MAX_RUNTIMES;
	bson_create_empty_document(subdoc1);
	bson_create_empty_document(subdoc2);

	for (i = 0; i < RUBIC_AGENT_MAX_RUNTIMES; ++i) {
		rubic_agent_runtime *runtime = &state.runtimes[i];
		char temp[64];
		char subscript[2];

		if (!runtime->name) {
			break;
		}
		bson_create_empty_document(temp);
		bson_set_string(temp, "name", runtime->name);
		if (runtime->version) {
			bson_set_string(temp, "version", runtime->version);
		}
#if (RUBIC_AGENT_MAX_RUNTIMES > 10)
# error "rubic_agent.runtimes_max must be equal or smaller than 10."
#endif
		subscript[0] = i - '0';
		subscript[1] = '\0';
		bson_set_subdocument(subdoc1, subscript, temp);
	}

	for (i = 0; i < RUBIC_AGENT_MAX_STORAGES; ++i) {
		rubic_agent_storage *storage = &state.storages[i];
		if (!storage->name) {
			break;
		}
		bson_set_string(subdoc2, storage->name, storage->path);
	}

	len = bson_empty_size;
	len += bson_measure_string("rubicVersion", RUBIC_AGENT_RUBIC_VERSION);
	len += bson_measure_array("runtimes", subdoc1);
	len += bson_measure_subdocument("storages", subdoc2);
	output = malloc(len);
	if (!output) {
		free(subdoc1);
		return -ENOMEM;
	}
	bson_create_empty_document(output);
	bson_set_string(output, "rubicVersion", RUBIC_AGENT_RUBIC_VERSION);
	bson_set_array(output, "runtimes", subdoc1);
	bson_set_subdocument(output, "storages", subdoc2);
	free(subdoc1);
	state.cached_info = output;
	return 0;
}

/**
 * @func rubic_agent_method_info
 * @brief Query information
 */
static void *rubic_agent_method_info(const void *params)
{
	int len;
	void *result;

	if (!state.cached_info) {
		errno = generate_info();
		if (errno != 0) {
			return NULL;
		}
	}

	// Return cached data
	len = bson_measure_document(state.cached_info);
	result = malloc(len);
	if (!result) {
		return NULL;
	}
	memcpy(result, state.cached_info, len);
	return result;
}

/**
 * @func rubic_agent_method_queue
 * @brief Queue request (async)
 */
static int rubic_agent_method_queue(peridot_rpc_server_async_context *context)
{
	const void *params = context->params;
	const char *name;
	int off_name, off_tid;
	int tid;

	bson_get_props(params, "tid", &off_tid, NULL);
	tid = bson_get_int32(params, off_tid, -1);
	if ((0 <= tid) && (tid < RUBIC_AGENT_WORKER_THREADS)) {
		// Queue request to specific worker thread
		rubic_agent_worker *worker = &state.workers[tid];
		pthread_mutex_lock(&worker->mutex);
		if (worker->context) {
			pthread_mutex_unlock(&worker->mutex);
			return -EBUSY;
		}
		worker->context = context;
		pthread_mutex_unlock(&worker->mutex);
		sem_post(&worker->sem);
		return 0;
	}

	bson_get_props(params, "name", &off_name, NULL);
	name = bson_get_string(params, off_name, "");
	if (strcmp(name, "start") == 0) {
		rubic_agent_worker *worker = &state.workers[0];
		for (tid = 0; tid < RUBIC_AGENT_WORKER_THREADS; ++tid, ++worker) {
			pthread_mutex_lock(&worker->mutex);
			if (worker->state == WORKER_STATE_IDLE) {
				worker->state = WORKER_STATE_STARTING;
				worker->context = context;
				pthread_mutex_unlock(&worker->mutex);
				sem_post(&worker->sem);
				return 0;
			}
			pthread_mutex_unlock(&worker->mutex);
		}
		return -EBUSY;
	}
	return -ESRCH;
}

/**
 * @func rubic_agent_method_status
 * @brief Queue status
 */
static void *rubic_agent_method_status(const void *params)
{
	// FIXME
	return NULL;
}

/**
 * @func rubic_agent_init
 * @brief Initialize Rubic agent
 */
int rubic_agent_init(void)
{
	int i;
	for (i = 0; i < RUBIC_AGENT_WORKER_THREADS; ++i) {
		rubic_agent_worker *worker = &state.workers[i];
		pthread_mutex_init(&worker->mutex, NULL);
		sem_init(&worker->sem, 0, 0);
		worker->tid = i;
	}
	peridot_rpc_server_register_sync_method("rubic.info", rubic_agent_method_info);
	peridot_rpc_server_register_async_method("rubic.queue", rubic_agent_method_queue);
	peridot_rpc_server_register_sync_method("rubic.status", rubic_agent_method_status);
	return 0;
}

/**
 * @func rubic_agent_register_runtime
 * @brief Register runtime
 */
int rubic_agent_register_runtime(const char *name, const char *version, rubic_agent_runtime_runner runner)
{
	int i;
	for (i = 0; i < RUBIC_AGENT_MAX_RUNTIMES; ++i) {
		if (!state.runtimes[i].name) {
			state.runtimes[i].name = name;
			state.runtimes[i].version = version;
			state.runtimes[i].runner = runner;
			return 0;
		}
	}
	return -ENOSPC;
}

/**
 * @func rubic_agent_register_storage
 * @brief Register storage
 */
int rubic_agent_register_storage(const char *name, const char *path)
{
	int i;
	for (i = 0; i < RUBIC_AGENT_MAX_STORAGES; ++i) {
		if (!state.storages[i].name) {
			state.storages[i].name = name;
			state.storages[i].path = path;
			return 0;
		}
	}
	return -ENOSPC;
}

/**
 * @func rubic_agent_start
 * @brief Start receiver thread(s)
 */
int rubic_agent_service(void)
{
	int i;
#if (RUBIC_AGENT_WORKER_THREADS < 1)
# error "rubic_agent.workers_max must be equal or larger than 1."
#endif
	for (i = 1; i < RUBIC_AGENT_WORKER_THREADS; ++i) {
		pthread_t tid;
		int result = pthread_create(&tid, NULL, (void *(*)(void *))worker, &state.workers[i]);
		if (result != 0) {
			return result;
		}
	}
	worker(&state.workers[0]);
	return 0;
}
