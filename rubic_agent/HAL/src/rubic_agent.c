#include "rubic_agent.h"
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <string.h>
#include <malloc.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include "system.h"
#include "bson.h"
#include "peridot_rpc_server.h"
#ifdef RUBIC_AGENT_ENABLE_PROGRAMMER
# include "md5.h"
#endif  /* RUBIC_AGENT_ENABLE_PROGRAMMER */
#include "io.h"
#include "sys/alt_irq.h"

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
	WORKER_STATE_AUTOBOOT,
};

typedef struct rubic_agent_worker_s {
	pthread_t tid;
	char thread_index;
	char state;
	pthread_mutex_t mutex;
	sem_t sem;
	peridot_rpc_server_async_context *context;
	peridot_rpc_server_async_context *callback_context;
} rubic_agent_worker;

struct rubic_agent_state_s {
	rubic_agent_runtime runtimes[RUBIC_AGENT_MAX_RUNTIMES];
	rubic_agent_storage storages[RUBIC_AGENT_MAX_STORAGES];
	rubic_agent_worker workers[RUBIC_AGENT_WORKER_THREADS];
	void *cached_info;
#ifdef RUBIC_AGENT_ENABLE_PROGRAMMER
	struct {
		rubic_agent_prog_blksize blksize;
		rubic_agent_prog_reader reader;
		rubic_agent_prog_writer writer;
		void *user_data;
	} prog;
#endif  /* RUBIC_AGENT_ENABLE_PROGRAMMER */
} rubic_agent_state __attribute__((weak));

static struct rubic_agent_state_s state
__attribute__((alias("rubic_agent_state")));

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

char *strskip(char *s, const char *skip)
{
	if (strpbrk(s, skip) == s) {
		return s + strspn(s, skip);
	}
	return s;
}

/**
 * @func parse_boot_json
 * @brief Parse boot.json for auto boot
 */
static char *parse_boot_json(rubic_agent_runtime **pruntime, int *pflags)
{
	static const char *const json_spaces = "\t\r\n ";
	rubic_agent_storage *storage;
	int i;
	char buffer[256];
	char *input, *file;
	int fd;

	*pruntime = find_runtime(NULL);
	*pflags = RUBIC_AGENT_RUNNER_FLAG_AUTOBOOT | RUBIC_AGENT_RUNNER_FLAG_FILE;
	file = NULL;
	
	for (i = 0; i < RUBIC_AGENT_MAX_STORAGES; ++i) {
		storage = &state.storages[i];
		if (!storage->name) {
			break;
		}
		if (strcmp(storage->name, "internal") == 0) {
			goto found_storage;
		}
	}
	// No internal storage
	return NULL;

found_storage:
	buffer[sizeof(buffer) - 1] = 0;
	strncpy(buffer, storage->path, sizeof(buffer) - 1);
	strncat(buffer, "/boot.json", sizeof(buffer) - 1);
	fd = open(buffer, O_RDONLY);
	if (fd < 0) {
		// No boot file
		return NULL;
	}
	i = 0;
	for (;;) {
		int bytes_read = read(fd, buffer + i, sizeof(buffer) - 1 - i);
		if (bytes_read < 0) {
			// Read error
			close(fd);
			return NULL;
		}
		if (bytes_read == 0) {
			// EOF
			break;
		}
		i += bytes_read;
	}
	close(fd);

	// Parse JSON
	input = strskip(buffer, json_spaces);
	if (*input++ != '{') {
		goto syntax_error;
	}
	for (;;) {
		char *key, *val;
		input = strskip(input, json_spaces);
		if (*input++ != '"') {
			goto syntax_error;
		}
		key = input;
		input = strchr(input, '"');
		if (!input) {
			goto syntax_error;
		}
		*input++ = '\0';
		input = strskip(input, json_spaces);
		if (*input++ != ':') {
			goto syntax_error;
		}
		input = strskip(input, json_spaces);
		if (*input++ != '"') {
			// not string
			input = strpbrk(input, "{}[],");
			if (!input) {
				goto syntax_error;
			}
		} else {
			// string
			val = input;
			for (;;) {
				input = strpbrk(input, "\\\"");
				if (!input) {
					goto syntax_error;
				}
				if (*input == '"') {
					*input++ = '\0';
					break;
				}
				if (*input == '\\') {
					// This routine do not convert escape characters!
					input += 2;
				}
			}
			input = strskip(input, json_spaces);

			if (strcmp(key, "runtime") == 0) {
				*pruntime = find_runtime(val);
			} else if (strcmp(key, "file") == 0) {
				file = val;
			}
		}
		if (*input++ == '}') {
			// End of JSON
			break;
		}
		if (*input++ != ',') {
			goto syntax_error;
		}
	}

	if (!file) {
		goto syntax_error;
	}

	if (*file != '/') {
		// Convert to full path
		char *full_path = (char *)malloc(strlen(storage->path) + 1 + strlen(file) + 1);
		if (!full_path) {
			return NULL;
		}
		strcpy(full_path, storage->path);
		strcat(full_path, "/");
		strcat(full_path, file);
		return full_path;
	}

	// Return path
	return strdup(file);

syntax_error:
	return NULL;
}

/**
 * @func worker_thread
 * @brief Main worker thread
 */
static void *worker_thread(rubic_agent_worker *worker)
{
	pthread_mutex_lock(&worker->mutex);
	for (;;) {
		peridot_rpc_server_async_context *context;
		rubic_agent_runtime *runtime;
		const void *params;
		int off_runtime, off_file, off_source, off_debug;
		int flags;
		const char *file_or_source;
		int result;

		if (worker->state == WORKER_STATE_AUTOBOOT) {
			file_or_source = parse_boot_json(&runtime, &flags);
			if (!file_or_source) {
				worker->state = WORKER_STATE_IDLE;
				continue;
			}
			context = NULL;
			worker->state = WORKER_STATE_STARTING;
			pthread_mutex_unlock(&worker->mutex);
			goto invoke_runner;
		}
		worker->state = WORKER_STATE_IDLE;
		pthread_mutex_unlock(&worker->mutex);

		// Wait new start request
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
		if (bson_get_boolean(params, off_debug, 0)) {
			flags |= RUBIC_AGENT_RUNNER_FLAG_DEBUG;
		}
invoke_runner:
		if ((!runtime) || (file_or_source == NULL)) {
			// invalid request
			result = -ESRCH;
			goto reply_error;
		}
		result = (*runtime->runner)(file_or_source, flags, worker);

		pthread_mutex_lock(&worker->mutex);
		if (worker->state == WORKER_STATE_STARTING) {
			// context for "start" still alive
reply_error:
			worker->context = NULL;
			if (context) {
				peridot_rpc_server_async_callback(context, NULL, result);
			}
		}

		context = worker->callback_context;
		if (context) {
			worker->callback_context = NULL;
			void *output = malloc(bson_empty_size + bson_measure_int32("result"));
			if (output) {
				bson_create_empty_document(output);
				bson_set_int32(output, "result", result);
			}
			peridot_rpc_server_async_callback(context, output, output ? 0 : -ENOMEM);
		}

		if (flags & RUBIC_AGENT_RUNNER_FLAG_AUTOBOOT) {
			free((char *)file_or_source);
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
	bson_set_int32(output, "tid", worker->thread_index);
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
	} else if (strcmp(name, "callback") == 0) {
		if (worker->callback_context) {
			// Callback already exists
			peridot_rpc_server_async_callback(rpc_ctx, NULL, EEXIST);
			return;
		}
		worker->callback_context = rpc_ctx;
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
	char temp[20];
	void *subdoc, *result;
	int i;

	subdoc = malloc(bson_empty_size + (4 + bson_empty_size + bson_measure_boolean("running")) * RUBIC_AGENT_WORKER_THREADS);
	if (!subdoc) {
		goto nomem;
	}
	bson_create_empty_document(subdoc);

#if (RUBIC_AGENT_WORKER_THREADS > 10)
# error "rubic_agent.workers_max must be equal or smaller than 10."
#endif

	for (i = 0; i < RUBIC_AGENT_WORKER_THREADS; ++i) {
		char key[2];
		key[0] = '0' + i;
		key[1] = '\0';
		rubic_agent_worker *worker = &state.workers[i];
		bson_create_empty_document(temp);
		bson_set_boolean(temp, "running", worker->state != WORKER_STATE_IDLE);
		bson_set_subdocument(subdoc, key, temp);
	}

	result = malloc(bson_empty_size + bson_measure_array("threads", subdoc));
	if (!result) {
		free(subdoc);
nomem:
		errno = ENOMEM;
		return NULL;
	}

	bson_create_empty_document(result);
	bson_set_array(result, "threads", subdoc);
	free(subdoc);
	return result;
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
		memset(worker, 0, sizeof(*worker));
		pthread_mutex_init(&worker->mutex, NULL);
		sem_init(&worker->sem, 0, 0);
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

#ifdef RUBIC_AGENT_ENABLE_PROGRAMMER
/**
 * @func rubic_agent_method_prog_hash
 * @brief Programmer hash read request (sync)
 * @param params {
 *   area: <string>
 *   offset: <int32>
 * }
 * @return {
 *   hash: <binary>
 *   length: <int32>
 * }
 */
void *rubic_agent_method_prog_hash(const void *params)
{
	int off_area, off_offset;
	const char *area;
	int offset;
	void *buf;
	int result;
	digest_md5_t hash, *hash_ptr;
	int block_size;
	
	if (bson_get_props(params, "area", &off_area, "offset", &off_offset, NULL) < 0) {
inval:
		errno = EINVAL;
		return NULL;
	}

	area = bson_get_string(params, off_area, NULL);
	if (!area) {
		goto inval;
	}

	block_size = (*state.prog.blksize)(area, state.prog.user_data);
	if (block_size <= 0) {
		errno = ENODEV;
		return NULL;
	}

	offset = bson_get_int32(params, off_offset, -1);
	if ((offset < 0) || ((offset & (block_size - 1)) != 0)) {
		goto inval;
	}

	buf = malloc(block_size);
	if (!buf) {
nomem:
		errno = ENOMEM;
		return NULL;
	}

	result = (*state.prog.reader)(area, state.prog.user_data, offset, buf, block_size);
	if (result < 0) {
		free(buf);
		errno = EIO;
		return NULL;
	}

	digest_md5_calc(&hash, buf, block_size);
	free(buf);

	buf = malloc(bson_empty_size + bson_measure_binary("hash", sizeof(hash)) + bson_measure_int32("length"));
	if (!buf) {
		goto nomem;
	}

	bson_create_empty_document(buf);
	bson_set_binary_generic(buf, "hash", sizeof(hash), (void **)&hash_ptr);
	memcpy(hash_ptr, &hash, sizeof(hash));
	bson_set_int32(buf, "length", block_size);
	return buf;
}

/**
 * @func rubic_agent_method_prog_write
 * @brief Programmer write request (sync)
 * @param params {
 *   area: <string>
 *   offset: <int32>
 *   data: <binary>
 *   hash: <binary>
 * }
 * @return null
 */
void *rubic_agent_method_prog_write(const void *params)
{
	int off_area, off_offset, off_data, off_hash;
	const char *area;
	int offset;
	int result;
	int data_len, hash_len;
	const void *data_ptr;
	digest_md5_t hash;
	const digest_md5_t *hash_ptr;
	int block_size;

	if (bson_get_props(params, "area", &off_area, "offset", &off_offset, "data", &off_data, "hash", &off_hash, NULL) < 0) {
inval:
		errno = EINVAL;
		return NULL;
	}

	area = bson_get_string(params, off_area, NULL);
	if (!area) {
		goto inval;
	}

	block_size = (*state.prog.blksize)(area, state.prog.user_data);
	if (block_size <= 0) {
		errno = ENODEV;
		return NULL;
	}

	offset = bson_get_int32(params, off_offset, -1);
	if ((offset < 0) || ((offset & (block_size - 1)) != 0)) {
		goto inval;
	}

	data_ptr = bson_get_binary(params, off_data, &data_len);
	if ((!data_ptr) || ((data_len & (block_size - 1)) != 0)) {
		goto inval;
	}

	hash_ptr = bson_get_binary(params, off_hash, &hash_len);
	if ((!hash_ptr) || (hash_len != sizeof(hash))) {
		goto inval;
	}

	digest_md5_calc(&hash, data_ptr, data_len);
	if (memcmp(&hash, hash_ptr, sizeof(hash)) != 0) {
		errno = EILSEQ;
		return NULL;
	}

	result = (*state.prog.writer)(area, state.prog.user_data, offset, data_ptr, data_len);
	if (result < 0) {
		errno = EIO;
		return NULL;
	}

	errno = 0;
	return NULL;
}
/**
 * @func rubic_agent_method_prog_reset
 * @brief Programmer reset request (sync)
 * @param params {
 * }
 * @return null
 */
void *rubic_agent_method_prog_reset(const void *params)
{
#if ((RUBIC_AGENT_DUALBOOT_BASE) > 0)
	// Disable IRQs
	alt_irq_disable_all();
	// Wait for IP busy signal cleared
	while (IORD((RUBIC_AGENT_DUALBOOT_BASE), 3) & 1);
	// Trigger reconfiguration
	IOWR((RUBIC_AGENT_DUALBOOT_BASE), 0, 1);
	// Halt system
	for (;;);
#endif
	return NULL;
}

/**
 * @func rubic_agent_register_programmer
 * @brief Register firmware updater
 */
int rubic_agent_register_programmer(rubic_agent_prog_blksize blksize, rubic_agent_prog_reader reader, rubic_agent_prog_writer writer, void *user_data)
{
	state.prog.blksize = blksize;
	state.prog.reader = reader;
	state.prog.writer = writer;
	state.prog.user_data = user_data;
	peridot_rpc_server_register_sync_method("rubic.prog.hash", rubic_agent_method_prog_hash);
	peridot_rpc_server_register_sync_method("rubic.prog.write", rubic_agent_method_prog_write);
	peridot_rpc_server_register_sync_method("rubic.prog.reset", rubic_agent_method_prog_reset);
	return 0;
}
#endif  /* RUBIC_AGENT_ENABLE_PROGRAMMER */

/**
 * @func rubic_agent_start
 * @brief Start receiver thread(s)
 */
int rubic_agent_service(int disableAutoBoot)
{
#if (RUBIC_AGENT_WORKER_THREADS < 1)
# error "rubic_agent.workers_max must be equal or larger than 1."
#endif
#if (RUBIC_AGENT_WORKER_THREADS > 1)
	int i;
	char name[16];
	strcpy(name, "rubic_agent_0");
	for (i = 1; i < RUBIC_AGENT_WORKER_THREADS; ++i) {
		rubic_agent_worker *worker = &state.workers[i];
		int result = pthread_create(&worker->tid, NULL, (void *(*)(void *))worker_thread, worker);
		if (result != 0) {
			return result;
		}
		name[12] = i + '0';
		pthread_setname_np(worker->tid, name);
	}
#endif  /* RUBIC_AGENT_WORKER_THREADS > 1 */
	state.workers[0].tid = pthread_self();
	if (!disableAutoBoot) {
		state.workers[0].state = WORKER_STATE_AUTOBOOT;
	}
	worker_thread(&state.workers[0]);
	return 0;
}
