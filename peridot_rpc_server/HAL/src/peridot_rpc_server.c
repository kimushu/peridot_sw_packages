#include "system.h"
#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include "sys/alt_cache.h"
#include "peridot_rpc_server.h"
#include "peridot_swi.h"
#include "bson.h"

#if (PERIDOT_RPCSRV_WORKER_THREADS) > 0
# define PERIDOT_RPCSRV_MULTI_THREAD
# include <pthread.h>
# include <semaphore.h>
#else
# undef PERIDOT_RPCSRV_MULTI_THREAD
#endif

#ifdef PERIDOT_RPCSRV_ENABLE_ISOLATION
# define ISOLATED __attribute__((section(PERIDOT_RPCSRV_ISOLATED_SECTION)))
#else
# define ISOLATED
#endif

#ifdef NIOS2_DCACHE_LINE_SIZE
# define ALIGNED __attribute__((aligned(NIOS2_DCACHE_LINE_SIZE)))
#else
# define ALIGNED
#endif

// Macro for generating uncached data pointer (using bypass-cache bit)
#define UCPTR(p) ((typeof(p))((uintptr_t)(p) | (1u<<31)))

// Public RPC server information
ISOLATED ALIGNED static peridot_rpc_server_info pub_srvInfo;

// Public RPC request/response buffer
ISOLATED ALIGNED static char pub_reqBuf[(PERIDOT_RPCSRV_REQUEST_LENGTH)];
ISOLATED ALIGNED static char pub_resBuf[(PERIDOT_RPCSRV_RESPONSE_LENGTH)];

// Server status
static volatile int srv_running;

// Buffer pointers
peridot_rpc_server_buffer reqBuf;
peridot_rpc_server_buffer resBuf;

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
// Mutex for locking buffer operations
static pthread_mutex_t buf_mutex;

// Semaphore for notifying IRQ
static sem_t irq_sem;
#else
// Flag for notifying IRQ
static volatile int irq_sem;
#endif

// Registered method list
static peridot_rpc_server_method_entry *method_first, *method_last;

/*
 * Handler for software interrupt from client
 */
static void peridot_rpc_server_handler(void *param)
{
	(void)param;	// unused
#ifdef PERIDOT_RPCSRV_MULTI_THREAD
	sem_post(&irq_sem);
#else
	irq_sem = 1;
#endif
}

/*
 * Find method entry
 */
static peridot_rpc_server_method_entry *peridot_rpc_server_find_method(const char *name)
{
	peridot_rpc_server_method_entry *entry = method_first;

	for (; entry; entry = entry->next) {
		if (strcmp(entry->name, name) == 0) {
			return entry;
		}
	}

	return NULL;
}

/*
 * Stop server
 */
static void peridot_rpc_server_stop(void)
{
	srv_running = 0;
	peridot_swi_write_message(0);

	// TODO: cleanup method list
}

/*
 * Process one request
 */
static int peridot_rpc_server_process_request(void)
{
	const void *input;
	void *output;

	alt_u32 len;
	int off_jsonrpc;
	int off_method;
	peridot_rpc_server_method_entry *method;
	int off_params;
	int off_id = -1;
	char *result = NULL;
	int result_errno = 0;

	for (;;) {
		if (!srv_running) {
			return -1;
		}

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
		pthread_mutex_lock(&buf_mutex);
#endif
		len = *UCPTR((alt_u32 *)reqBuf.ptr);
		if (len != 0) {
			break;
		}
#ifdef PERIDOT_RPCSRV_MULTI_THREAD
		pthread_mutex_unlock(&buf_mutex);
		sem_wait(&irq_sem);
#else
		if (!irq_sem) {
			return 0;
		}
#endif
	}

	// still in critical section

	if (len > reqBuf.len) {
		// Too large
		input = NULL;
		result_errno = EBADMSG;
	} else {
		input = malloc(len);
		if (!input) {
			result_errno = ENOMEM;
		}
	}

	if (input) {
		// Copy input data to private memory
		memcpy((void *)input, UCPTR(reqBuf.ptr), (len < reqBuf.len) ? len : reqBuf.len);
	}

	// Now, public buffer can be reused for new request
	*UCPTR(&pub_srvInfo.request.len) = reqBuf.len;
	*UCPTR(&pub_srvInfo.request.ptr) = reqBuf.ptr;
	*UCPTR((alt_u32 *)reqBuf.ptr) = 0;

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
	pthread_mutex_unlock(&buf_mutex);
#endif

	// End of critical section

	if (!input) {
		// No memory / too large
		goto send_response;
	}

	if (bson_get_props(input,
		"jsonrpc", &off_jsonrpc,
		"method", &off_method,
		"params", &off_params,
		"id", &off_id,
		NULL) <= 0) {
		// Invalid input
		result_errno = EINVAL;
		goto send_response;
	}

	if (strcmp(bson_get_string(input, off_jsonrpc, ""), PERIDOT_RPCSRV_JSONRPC_VER) != 0) {
		// Unsupported JSON-RPC version
		result_errno = EINVAL;
		goto send_response;
	}

	method = peridot_rpc_server_find_method(bson_get_string(input, off_method, NULL));
	if (!method) {
		// Method is not found
		result_errno = ENOSYS;
		goto send_response;
	}

	len = bson_empty_size;
	len += bson_measure_string("jsonrpc", PERIDOT_RPCSRV_JSONRPC_VER);
	len += bson_measure_null("result");
	len += bson_measure_element("id", input, off_id);
	// len == (minimum response size with result=null)

	errno = 0;
	result = (*method->func)(
			bson_get_subdocument((void *)input, off_params, (void *)bson_empty_document),
			resBuf.len - len);
	result_errno = errno;

send_response:
	if ((result_errno == 0) && result) {
		int doclen;
		doclen = bson_measure_document(result);
		if ((len + doclen) > resBuf.len) {
			result_errno = JSONRPC_ERR_INTERNAL_ERROR;
		} else {
			len += doclen;
		}
	}
	if (result_errno != 0) {
		len += 32;
		// Now len > (minimum response size with error:{code:number})
	}
	output = malloc(len);
	if (!output) {
		// Critical fault!! (cannot allocate response memory)
		// TODO:
		return -2;
	}
	memcpy(output, &bson_empty_document, bson_empty_size);
	bson_set_string(output, "jsonrpc", PERIDOT_RPCSRV_JSONRPC_VER);
	if (result_errno == 0) {
		if (result) {
			bson_set_subdocument(output, "result", result);
		} else {
			bson_set_null(output, "result");
		}
	} else {
		char errobj[32];
		memcpy(errobj, &bson_empty_document, bson_empty_size);
		bson_set_int32(errobj, "code", result_errno);
		bson_set_subdocument(output, "error", errobj);
	}
	bson_set_element(output, "id", input, off_id);
	len = bson_measure_document(output);

	free((void *)input);
	free(result);

	for (;;) {
		int buf_len;

		if (!srv_running) {
			free(output);
			return -1;
		}

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
		pthread_mutex_lock(&buf_mutex);
#endif
		buf_len = *UCPTR((alt_u32 *)resBuf.ptr);
		if (buf_len == 0) {
			break;
		}
#ifdef PERIDOT_RPCSRV_MULTI_THREAD
		pthread_mutex_unlock(&buf_mutex);
		sem_wait(&irq_sem);
#else
		while (!irq_sem);
#endif
	}

	// still in critical section

	// Write response
	memcpy(UCPTR((alt_u32 *)resBuf.ptr + 1), (alt_u32 *)output + 1, len - 4);
	*UCPTR((alt_u32 *)resBuf.ptr) = *(alt_u32 *)output;

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
	pthread_mutex_unlock(&buf_mutex);
#endif

	// End of critical section

	free(output);
	return 1;
}

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
/*
 * Worker for server operations
 */
static void *peridot_rpc_server_worker(void *param)
{
	(void)param;	// unused
	while (peridot_rpc_server_process_request() >= 0);
	return NULL;
}
#endif  /* PERIDOT_RPCSRV_MULTI_THREAD */

/*
 * Initialize RPC server
 */
void peridot_rpc_server_init(void)
{
#ifdef PERIDOT_RPCSRV_MULTI_THREAD
	int i;
#endif

	peridot_rpc_server_stop();

#ifdef PERIDOT_RPCSRV_MULTI_THREAD
	pthread_mutex_init(&buf_mutex, NULL);
	sem_init(&irq_sem, 0, 0);
#else
	irq_sem = 0;
#endif

	// Initialize buffer
	reqBuf.ptr = pub_reqBuf;
	reqBuf.len = sizeof(pub_reqBuf);
	*UCPTR((alt_u32 *)reqBuf.ptr) = 0;
	resBuf.ptr = pub_resBuf;
	resBuf.len = sizeof(pub_resBuf);
	*UCPTR((alt_u32 *)resBuf.ptr) = 0;

	// Initialize server information
	pub_srvInfo.if_ver = PERIDOT_RPCSRV_IF_VERSION;
	pub_srvInfo.reserved = 0;
	pub_srvInfo.host_id[0] = 0;
	pub_srvInfo.host_id[1] = 0;
	pub_srvInfo.request.len = reqBuf.len;
	pub_srvInfo.request.ptr = reqBuf.ptr;
	pub_srvInfo.response.len = resBuf.len;
	pub_srvInfo.response.ptr = resBuf.ptr;
	alt_dcache_flush(&pub_srvInfo, sizeof(pub_srvInfo));

	// Publish server information
	peridot_swi_set_handler(peridot_rpc_server_handler, NULL);
	peridot_swi_write_message((alt_u32)&pub_srvInfo);

	atexit(peridot_rpc_server_stop);

	srv_running = 1;
#ifdef PERIDOT_RPCSRV_MULTI_THREAD
	for (i = 0; i < (PERIDOT_RPCSRV_WORKER_THREADS); ++i) {
		pthread_t tid;
		pthread_create(&tid, NULL, peridot_rpc_server_worker, NULL);
		// tid is not used
	}
#endif
}

/*
 * Register function as RPC-callable
 */
void peridot_rpc_server_register_method(const char *name, peridot_rpc_server_function func)
{
	peridot_rpc_server_method_entry *entry;
	entry = malloc(sizeof(*entry) + strlen(name) + 1);
	if (!entry) {
		// TODO:
		return;
	}
	entry->next = NULL;
	entry->func = func;
	strcpy(entry->name, name);
	if (method_last) {
		method_last->next = entry;
	} else {
		method_first = entry;
	}
	method_last = entry;
}

/*
 * Process RPC requests (for non-multi-threading configurations)
 */
void peridot_rpc_server_process(void)
{
#ifndef PERIDOT_RPCSRV_MULTI_THREAD
	peridot_rpc_server_process_request();
#endif
}

